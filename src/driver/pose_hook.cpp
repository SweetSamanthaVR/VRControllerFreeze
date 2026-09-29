#include "pose_hook.h"

#include <MinHook.h>

#include <array>

namespace vr_controller_freeze::pose_hook {
namespace {

using PoseUpdatedFn = void (*)(vr::IVRServerDriverHost* self, std::uint32_t deviceIndex,
                               const vr::DriverPose_t& pose, std::uint32_t poseSize);

// IVRServerDriverHost_006 vtable: 0 TrackedDeviceAdded, 1 TrackedDevicePoseUpdated.
constexpr std::size_t kPoseUpdatedSlot = 1;
constexpr std::uint32_t kMaxDevices = vr::k_unMaxTrackedDeviceCount;

struct DeviceSlot {
    vr::DriverPose_t lastPose{};
    DWORD lastPoseMs = 0;
    bool hasPose = false;
    vr::DriverPose_t frozenPose{};
    bool frozen = false;
};

SRWLOCK g_lock = SRWLOCK_INIT;
std::array<DeviceSlot, kMaxDevices> g_devices{};
volatile LONG g_updateCounts[kMaxDevices]{};
volatile LONG g_active = 0;
PoseUpdatedFn g_original = nullptr;

void DetourTrackedDevicePoseUpdated(vr::IVRServerDriverHost* self, std::uint32_t deviceIndex,
                                    const vr::DriverPose_t& pose, std::uint32_t poseSize) {
    // Pass through untouched while inactive, for out-of-range indices, and for drivers built
    // against a different DriverPose_t layout.
    if (::InterlockedCompareExchange(&g_active, 0, 0) == 0 || deviceIndex >= kMaxDevices ||
        poseSize != sizeof(vr::DriverPose_t)) {
        g_original(self, deviceIndex, pose, poseSize);
        return;
    }

    vr::DriverPose_t frozenPose;
    bool submitFrozen = false;
    ::AcquireSRWLockExclusive(&g_lock);
    DeviceSlot& slot = g_devices[deviceIndex];
    slot.lastPose = pose;
    slot.lastPoseMs = ::GetTickCount();
    slot.hasPose = true;
    // A disconnect is passed on rather than hidden behind the frozen pose.
    if (slot.frozen && pose.deviceIsConnected) {
        frozenPose = slot.frozenPose;
        submitFrozen = true;
    }
    ::ReleaseSRWLockExclusive(&g_lock);
    ::InterlockedIncrement(&g_updateCounts[deviceIndex]);

    // Always call through, so vrserver and any hooks chained after this one still run.
    g_original(self, deviceIndex, submitFrozen ? frozenPose : pose, poseSize);
}

} // namespace

InstallResult Install(vr::IVRServerDriverHost* host) {
    if (!host) return {HookStatus::Failed, HookError::HostInterfaceUnavailable, 0};

    // Detouring the function body, rather than patching a vtable entry, catches every caller of
    // this implementation, including other drivers' IVRServerDriverHost_006 pointers, and chains
    // with pose hooks installed earlier (Space Calibrator, SmoothTracking).
    void** vtable = *reinterpret_cast<void***>(host);
    void* target = vtable[kPoseUpdatedSlot];

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        return {HookStatus::Failed, HookError::MinHookInitialiseFailed, static_cast<LONG>(status)};
    }
    // Once the detour is live this module must never unload, whatever order SteamVR shuts down in.
    // Refuse to hook at all if that cannot be guaranteed.
    HMODULE module = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                              reinterpret_cast<LPCWSTR>(&DetourTrackedDevicePoseUpdated), &module)) {
        return {HookStatus::Failed, HookError::PinModuleFailed, static_cast<LONG>(::GetLastError())};
    }
    // On a re-install MinHook reports the existing detour; g_original still holds its trampoline.
    status = MH_CreateHook(target, reinterpret_cast<LPVOID>(&DetourTrackedDevicePoseUpdated),
                           reinterpret_cast<LPVOID*>(&g_original));
    if (status != MH_OK && status != MH_ERROR_ALREADY_CREATED) {
        return {HookStatus::Failed, HookError::MinHookCreateFailed, static_cast<LONG>(status)};
    }
    status = MH_EnableHook(target);
    if (status != MH_OK && status != MH_ERROR_ENABLED) {
        return {HookStatus::Failed, HookError::MinHookEnableFailed, static_cast<LONG>(status)};
    }
    ::InterlockedExchange(&g_active, 1);
    return {HookStatus::Active, HookError::None, 0};
}

void Deactivate() {
    ::InterlockedExchange(&g_active, 0);
    ReleaseAll();
}

FreezeOutcome Freeze(std::uint32_t deviceIndex, DWORD maxPoseAgeMs) {
    if (deviceIndex >= kMaxDevices) return FreezeOutcome::InvalidDevice;

    FreezeOutcome outcome = FreezeOutcome::NoRecentPose;
    ::AcquireSRWLockExclusive(&g_lock);
    // Read the clock under the lock: every lastPoseMs was stamped under it too, so it can never be
    // newer than this and the unsigned age below cannot wrap.
    const DWORD nowMs = ::GetTickCount();
    DeviceSlot& slot = g_devices[deviceIndex];
    if (slot.frozen) {
        outcome = FreezeOutcome::Frozen;
    } else if (slot.hasPose && slot.lastPose.poseIsValid && slot.lastPose.deviceIsConnected &&
               nowMs - slot.lastPoseMs <= maxPoseAgeMs) {
        vr::DriverPose_t& frozen = slot.frozenPose;
        frozen = slot.lastPose;
        // Zero motion so SteamVR's pose prediction cannot drift the held pose.
        frozen.poseTimeOffset = 0.0;
        for (int i = 0; i < 3; ++i) {
            frozen.vecVelocity[i] = 0.0;
            frozen.vecAcceleration[i] = 0.0;
            frozen.vecAngularVelocity[i] = 0.0;
            frozen.vecAngularAcceleration[i] = 0.0;
        }
        frozen.result = vr::TrackingResult_Running_OK;
        slot.frozen = true;
        outcome = FreezeOutcome::Frozen;
    }
    ::ReleaseSRWLockExclusive(&g_lock);
    return outcome;
}

void Release(std::uint32_t deviceIndex) {
    if (deviceIndex >= kMaxDevices) return;
    ::AcquireSRWLockExclusive(&g_lock);
    g_devices[deviceIndex].frozen = false;
    ::ReleaseSRWLockExclusive(&g_lock);
}

void ReleaseAll() {
    ::AcquireSRWLockExclusive(&g_lock);
    for (DeviceSlot& slot : g_devices) slot.frozen = false;
    ::ReleaseSRWLockExclusive(&g_lock);
}

LONG PoseUpdateCount(std::uint32_t deviceIndex) {
    if (deviceIndex >= kMaxDevices) return 0;
    return ::InterlockedCompareExchange(&g_updateCounts[deviceIndex], 0, 0);
}

} // namespace vr_controller_freeze::pose_hook
