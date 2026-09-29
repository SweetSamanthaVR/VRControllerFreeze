// Loads the real driver_vrcontrollerfreeze.dll into a fake vrserver host and exercises the pose hook.
// Build: cmake --build build --config Release --target driver_harness
// Run:   build\Release\driver_harness.exe build\dist\driver\vrcontrollerfreeze\bin\win64\driver_vrcontrollerfreeze.dll [--with-calibrator]
// It refuses to run while SteamVR or VRControllerFreeze.exe is using the shared memory,
// because the real driver would act on the harness's requests and freeze real controllers.
#include <openvr_driver.h>
#include <windows.h>
#include <MinHook.h>
#include <cstdio>
#include <cstring>
#include "win_shared_memory.h"

using namespace vr_controller_freeze;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond, msg) do { ++g_checks; if (cond) std::printf("PASS  %s\n", msg); else { std::printf("FAIL  %s\n", msg); ++g_failures; } } while (0)

struct Received { uint32_t index = 0xFFFFFFFF; vr::DriverPose_t pose{}; int calls = 0; };
static Received g_received;

class FakeHost : public vr::IVRServerDriverHost {
public:
    bool TrackedDeviceAdded(const char*, vr::ETrackedDeviceClass, vr::ITrackedDeviceServerDriver*) override { return false; }
    // noinline and non-trivial so MinHook has a real function body to detour.
    __declspec(noinline) void TrackedDevicePoseUpdated(uint32_t idx, const vr::DriverPose_t& pose, uint32_t size) override {
        if (size != sizeof(vr::DriverPose_t)) return;
        g_received.index = idx;
        std::memcpy(&g_received.pose, &pose, sizeof(pose));
        ++g_received.calls;
    }
    void VsyncEvent(double) override {}
    void VendorSpecificEvent(uint32_t, vr::EVREventType, const vr::VREvent_Data_t&, double) override {}
    bool IsExiting() override { return false; }
    bool PollNextEvent(vr::VREvent_t*, uint32_t) override { return false; }
    void GetRawTrackedDevicePoses(float, vr::TrackedDevicePose_t*, uint32_t) override {}
    void RequestRestart(const char*, const char*, const char*, const char*) override {}
    uint32_t GetFrameTimings(vr::Compositor_FrameTiming*, uint32_t) override { return 0; }
    void SetDisplayEyeToHead(uint32_t, const vr::HmdMatrix34_t&, const vr::HmdMatrix34_t&) override {}
    void SetDisplayProjectionRaw(uint32_t, const vr::HmdRect2_t&, const vr::HmdRect2_t&) override {}
    void SetRecommendedRenderTargetSize(uint32_t, uint32_t, uint32_t) override {}
};

class FakeLog : public vr::IVRDriverLog {
public:
    void Log(const char* msg) override { std::printf("      [driver log] %s\n", msg); }
};

static FakeHost g_host;
static FakeLog g_log;

class FakeContext : public vr::IVRDriverContext {
public:
    void* GetGenericInterface(const char* version, vr::EVRInitError* err) override {
        if (err) *err = vr::VRInitError_None;
        if (std::strcmp(version, vr::IVRServerDriverHost_Version) == 0) return static_cast<vr::IVRServerDriverHost*>(&g_host);
        if (std::strcmp(version, vr::IVRDriverLog_Version) == 0) return static_cast<vr::IVRDriverLog*>(&g_log);
        // InitServer only requires these to be non-null; the driver never calls them.
        static char placeholder[64];
        for (const char* required : {vr::IVRSettings_Version, vr::IVRProperties_Version, vr::IVRDriverManager_Version, vr::IVRResources_Version})
            if (std::strcmp(version, required) == 0) return placeholder;
        if (err) *err = vr::VRInitError_Init_InterfaceNotFound;
        return nullptr;
    }
    vr::DriverHandle_t GetDriverHandle() override { return 1; }
};

// Simulates another pose-hooking driver (Space Calibrator / SmoothTracking) installed before VRControllerFreeze:
// it shifts X by +10 on every pose.
using PoseFn = void (*)(vr::IVRServerDriverHost*, uint32_t, const vr::DriverPose_t&, uint32_t);
static PoseFn g_calibratorOriginal = nullptr;
static void CalibratorDetour(vr::IVRServerDriverHost* self, uint32_t idx, const vr::DriverPose_t& pose, uint32_t size) {
    vr::DriverPose_t shifted = pose;
    shifted.vecPosition[0] += 10.0;
    g_calibratorOriginal(self, idx, shifted, size);
}

static vr::DriverPose_t MakePose(double x, double y, double z) {
    vr::DriverPose_t p{};
    p.qWorldFromDriverRotation.w = 1; p.qDriverFromHeadRotation.w = 1; p.qRotation.w = 1;
    p.vecPosition[0] = x; p.vecPosition[1] = y; p.vecPosition[2] = z;
    p.vecVelocity[0] = 1.5; p.vecAngularVelocity[1] = 2.5; p.vecAcceleration[2] = 3.5;
    p.poseTimeOffset = 0.02;
    p.result = vr::TrackingResult_Running_OK; p.poseIsValid = true; p.deviceIsConnected = true;
    return p;
}

// Calls through the vtable exactly as driver_vrlink would.
static void Submit(uint32_t idx, const vr::DriverPose_t& pose) {
    vr::IVRServerDriverHost* host = &g_host;
    host->TrackedDevicePoseUpdated(idx, pose, sizeof(pose));
}

static LONG Bump(volatile LONG* p) { LONG n = ReadSharedLong(p) + 1; WriteSharedLong(p, n); return n; }

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: driver_harness <path to driver_vrcontrollerfreeze.dll> [--with-calibrator]\n");
        return 2;
    }
    if (HANDLE existing = OpenFileMappingW(FILE_MAP_READ, FALSE, kSharedMappingName)) {
        CloseHandle(existing);
        std::printf("REFUSED: the shared memory is in use. Close SteamVR and VRControllerFreeze.exe first,\n"
                    "otherwise the real driver would act on this harness's freeze requests.\n");
        return 2;
    }

    const bool withCalibrator = argc > 2 && std::strcmp(argv[2], "--with-calibrator") == 0;
    if (withCalibrator) {
        void* target = (*reinterpret_cast<void***>(static_cast<vr::IVRServerDriverHost*>(&g_host)))[1];
        MH_Initialize();
        const bool ok = MH_CreateHook(target, reinterpret_cast<LPVOID>(&CalibratorDetour), reinterpret_cast<LPVOID*>(&g_calibratorOriginal)) == MH_OK && MH_EnableHook(target) == MH_OK;
        CHECK(ok, "pre-existing calibrator-style hook installed");
    }
    const double shift = withCalibrator ? 10.0 : 0.0;

    HMODULE dll = LoadLibraryA(argv[1]);
    CHECK(dll != nullptr, "driver DLL loads");
    if (!dll) return 1;
    using FactoryFn = void* (*)(const char*, int*);
    auto factory = reinterpret_cast<FactoryFn>(GetProcAddress(dll, "HmdDriverFactory"));
    CHECK(factory != nullptr, "driver exports HmdDriverFactory");
    if (!factory) return 1;
    int rc = 0;
    auto* provider = static_cast<vr::IServerTrackedDeviceProvider*>(factory(vr::IServerTrackedDeviceProvider_Version, &rc));
    CHECK(provider != nullptr, "HmdDriverFactory returns provider");
    if (!provider) return 1;

    FakeContext context;
    CHECK(provider->Init(&context) == vr::VRInitError_None, "provider Init succeeds");

    SharedMemory shm;
    CHECK(shm.OpenOrCreate(), "harness opens the driver's shared memory");
    SharedState* s = shm.Get();
    const HandFields left = HandFieldsFor(s, true);
    const HandFields right = HandFieldsFor(s, false);
    auto appTick = [&] { WriteSharedLong(&s->appHeartbeatMs, static_cast<LONG>(GetTickCount())); };

    // Lazy hook: nothing is hooked until the app's heartbeat appears.
    provider->RunFrame();
    CHECK(ReadSharedLong(&s->hookStatus) == static_cast<LONG>(HookStatus::NotInstalled), "hook not installed before the app runs");
    Submit(1, MakePose(1, 2, 3));
    provider->RunFrame();
    CHECK(ReadSharedLong(left.poseUpdateCount) == 0, "no poses intercepted before the app runs");

    appTick();
    WriteSharedLong(left.target, 1);
    WriteSharedLong(right.target, 2);
    provider->RunFrame();
    CHECK(ReadSharedLong(&s->hookStatus) == static_cast<LONG>(HookStatus::Active), "hook installed once the app heartbeat appears");

    // Pass-through while live.
    Submit(1, MakePose(1, 2, 3));
    CHECK(g_received.index == 1 && g_received.pose.vecPosition[0] == 1 + shift && g_received.pose.vecVelocity[0] == 1.5, "live pose passes through unchanged");
    Submit(2, MakePose(7, 8, 9));
    appTick(); provider->RunFrame();
    CHECK(ReadSharedLong(left.poseUpdateCount) >= 1 && ReadSharedLong(right.poseUpdateCount) >= 1, "pose update counters published");
    CHECK(ReadSharedLong(left.poseUpdateDeviceIndex) == 1 && ReadSharedLong(right.poseUpdateDeviceIndex) == 2, "pose counters name the device they belong to");

    // Freeze left.
    WriteSharedLong(left.requested, 1); LONG seq = Bump(left.sequence);
    appTick(); provider->RunFrame();
    CHECK(ReadSharedLong(left.ack) == seq, "left freeze acknowledged");
    CHECK(ReadSharedLong(left.result) == static_cast<LONG>(FreezeRequestResult::Frozen), "left freeze result Frozen");
    CHECK(ReadSharedLong(left.frozen) == 1 && ReadSharedLong(left.frozenDeviceIndex) == 1, "left frozen on device 1");

    Submit(1, MakePose(50, 60, 70));
    const auto& fp = g_received.pose;
    CHECK(fp.vecPosition[0] == 1 + shift && fp.vecPosition[1] == 2 && fp.vecPosition[2] == 3, "frozen device reports captured position");
    CHECK(fp.vecVelocity[0] == 0 && fp.vecAngularVelocity[1] == 0 && fp.vecAcceleration[2] == 0 && fp.poseTimeOffset == 0, "frozen pose has zero motion");
    CHECK(fp.poseIsValid && fp.deviceIsConnected, "frozen pose stays valid and connected");

    Submit(2, MakePose(11, 12, 13));
    CHECK(g_received.index == 2 && g_received.pose.vecPosition[0] == 11 + shift, "other controller stays live while left is frozen");

    // A controller going to sleep reports itself disconnected. While frozen, SteamVR must keep seeing
    // the captured pose, connected, or it drops the hand role and the game's hand jumps on waking.
    vr::DriverPose_t disconnected = MakePose(99, 99, 99); disconnected.deviceIsConnected = false; disconnected.poseIsValid = false;
    Submit(1, disconnected);
    CHECK(g_received.pose.deviceIsConnected && g_received.pose.poseIsValid && g_received.pose.vecPosition[0] == 1 + shift,
          "a sleeping controller still reports its frozen pose, connected");
    appTick(); provider->RunFrame();
    CHECK(ReadSharedLong(left.frozen) == 1, "the freeze survives the controller sleeping");
    Submit(1, MakePose(51, 61, 71));
    CHECK(g_received.pose.vecPosition[0] == 1 + shift && g_received.pose.vecVelocity[0] == 0, "still frozen, with no motion, when it wakes");

    // Unfreeze.
    WriteSharedLong(left.requested, 0); seq = Bump(left.sequence);
    appTick(); provider->RunFrame();
    CHECK(ReadSharedLong(left.result) == static_cast<LONG>(FreezeRequestResult::Live) && ReadSharedLong(left.frozen) == 0, "left unfreeze result Live");
    Submit(1, MakePose(52, 62, 72));
    CHECK(g_received.pose.vecPosition[0] == 52 + shift, "unfrozen device follows live pose again");
    Submit(1, disconnected);
    CHECK(!g_received.pose.deviceIsConnected, "once unfrozen, a disconnect is passed on as normal");
    Submit(1, MakePose(52, 62, 72));

    // Rejections. The driver must never write the app-owned request flag.
    WriteSharedLong(left.target, 5);
    WriteSharedLong(left.requested, 1); seq = Bump(left.sequence);
    appTick(); provider->RunFrame();
    CHECK(ReadSharedLong(left.result) == static_cast<LONG>(FreezeRequestResult::RejectedNoRecentPose) && ReadSharedLong(left.ack) == seq, "freeze of device with no poses rejected");
    CHECK(ReadSharedLong(left.requested) == 1 && ReadSharedLong(left.frozen) == 0, "rejection leaves the app's request flag alone");
    WriteSharedLong(left.target, kInvalidDeviceIndex);
    seq = Bump(left.sequence);
    appTick(); provider->RunFrame();
    CHECK(ReadSharedLong(left.result) == static_cast<LONG>(FreezeRequestResult::RejectedNoController), "freeze with no controller in role rejected");
    WriteSharedLong(left.target, 1);
    Sleep(600);
    seq = Bump(left.sequence);
    appTick(); provider->RunFrame();
    CHECK(ReadSharedLong(left.result) == static_cast<LONG>(FreezeRequestResult::RejectedNoRecentPose), "freeze with stale (>500ms) pose rejected");

    // A pose that arrives immediately before the freeze is always fresh enough.
    Submit(1, MakePose(2, 2, 2));
    seq = Bump(left.sequence);
    appTick(); provider->RunFrame();
    CHECK(ReadSharedLong(left.result) == static_cast<LONG>(FreezeRequestResult::Frozen), "freeze straight after a pose succeeds");
    WriteSharedLong(left.requested, 0); Bump(left.sequence); appTick(); provider->RunFrame();

    // Emergency release.
    Submit(1, MakePose(3, 3, 3)); Submit(2, MakePose(4, 4, 4));
    WriteSharedLong(left.requested, 1); Bump(left.sequence);
    WriteSharedLong(right.requested, 1); Bump(right.sequence);
    appTick(); provider->RunFrame();
    CHECK(ReadSharedLong(left.frozen) == 1 && ReadSharedLong(right.frozen) == 1, "both hands frozen");
    WriteSharedLong(&s->requestedEmergencyRelease, 1);
    appTick(); provider->RunFrame();
    Submit(2, MakePose(40, 40, 40));
    CHECK(ReadSharedLong(left.frozen) == 0 && ReadSharedLong(right.frozen) == 0 && g_received.pose.vecPosition[0] == 40 + shift, "emergency release frees both hands");
    CHECK(ReadSharedLong(&s->requestedEmergencyRelease) == 0, "emergency release flag is consumed");
    CHECK(ReadSharedLong(left.result) == static_cast<LONG>(FreezeRequestResult::EmergencyReleased) &&
              ReadSharedLong(right.result) == static_cast<LONG>(FreezeRequestResult::EmergencyReleased),
          "emergency release is recorded as each hand's result");
    WriteSharedLong(&s->requestedEmergencyRelease, 1);
    appTick(); provider->RunFrame();
    CHECK(ReadSharedLong(&s->requestedEmergencyRelease) == 0, "emergency release with nothing frozen is still confirmed");
    WriteSharedLong(left.requested, 0); Bump(left.sequence);
    WriteSharedLong(right.requested, 0); Bump(right.sequence);
    appTick(); provider->RunFrame();

    // Watchdog, and no new requests acted on while the app is unresponsive.
    Submit(1, MakePose(5, 5, 5));
    WriteSharedLong(left.requested, 1); Bump(left.sequence);
    appTick(); provider->RunFrame();
    CHECK(ReadSharedLong(left.frozen) == 1, "left frozen before watchdog test");
    WriteSharedLong(&s->appHeartbeatMs, static_cast<LONG>(GetTickCount() - 4000));
    provider->RunFrame();
    Submit(1, MakePose(6, 6, 6));
    CHECK(ReadSharedLong(left.frozen) == 0 && ReadSharedLong(left.result) == static_cast<LONG>(FreezeRequestResult::WatchdogReleased) && g_received.pose.vecPosition[0] == 6 + shift, "watchdog releases when app heartbeat stops");
    Submit(2, MakePose(7, 7, 7));
    WriteSharedLong(right.requested, 1); seq = Bump(right.sequence);
    provider->RunFrame();
    CHECK(ReadSharedLong(right.ack) != seq && ReadSharedLong(right.frozen) == 0, "requests are not acted on while the app heartbeat is stale");
    WriteSharedLong(right.requested, 0); seq = Bump(right.sequence);
    appTick(); provider->RunFrame();
    CHECK(ReadSharedLong(right.ack) == seq && ReadSharedLong(right.frozen) == 0, "requests resume when the app heartbeat returns");

    // SteamVR restart while a hand is frozen: the old request must not be replayed.
    Submit(1, MakePose(8, 8, 8));
    WriteSharedLong(left.requested, 1); seq = Bump(left.sequence);
    appTick(); provider->RunFrame();
    CHECK(ReadSharedLong(left.frozen) == 1, "left frozen before simulated SteamVR restart");
    provider->Cleanup();
    Submit(1, MakePose(9, 9, 9));
    CHECK(g_received.pose.vecPosition[0] == 9 + shift, "after Cleanup, poses pass through untouched");
    CHECK(ReadSharedLong(left.frozen) == 0 && ReadSharedLong(&s->driverReady) == 0, "Cleanup publishes nothing frozen and driver not ready");

    CHECK(provider->Init(&context) == vr::VRInitError_None, "provider re-Init succeeds");
    CHECK(ReadSharedLong(left.ack) == seq && ReadSharedLong(left.result) == static_cast<LONG>(FreezeRequestResult::ReleasedDriverRestart), "request from before the restart is acknowledged as discarded");
    appTick(); provider->RunFrame();
    Submit(1, MakePose(10, 10, 10));
    CHECK(ReadSharedLong(left.frozen) == 0 && g_received.pose.vecPosition[0] == 10 + shift, "old freeze is not replayed after restart");
    CHECK(ReadSharedLong(&s->hookStatus) == static_cast<LONG>(HookStatus::Active), "hook re-activates after restart");

    // A request still pending when SteamVR stopped is discarded too.
    provider->Cleanup();
    WriteSharedLong(right.requested, 1); seq = Bump(right.sequence);
    provider->Init(&context);
    CHECK(ReadSharedLong(right.ack) == seq && ReadSharedLong(right.result) == static_cast<LONG>(FreezeRequestResult::ReleasedDriverRestart), "pending request at restart is discarded");
    appTick(); provider->RunFrame();
    Submit(2, MakePose(11, 11, 11));
    CHECK(ReadSharedLong(right.frozen) == 0 && g_received.pose.vecPosition[0] == 11 + shift, "pending freeze is not acted on after restart");

    // After re-Init, a new freeze holds its captured pose (no stale runtime state).
    Submit(1, MakePose(12, 12, 12));
    WriteSharedLong(left.requested, 1); Bump(left.sequence);
    appTick(); provider->RunFrame();
    Submit(1, MakePose(13, 13, 13));
    CHECK(ReadSharedLong(left.frozen) == 1 && g_received.pose.vecPosition[0] == 12 + shift, "freeze after re-Init holds the captured pose");

    provider->Cleanup();
    const int before = g_received.calls;
    Submit(1, MakePose(14, 14, 14));
    CHECK(g_received.calls == before + 1 && g_received.pose.vecPosition[0] == 14 + shift, "final Cleanup leaves poses passing through");

    std::printf("\n%s (%d checks, %d failure%s)\n", g_failures ? "HARNESS FAILED" : "HARNESS PASSED", g_checks, g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
