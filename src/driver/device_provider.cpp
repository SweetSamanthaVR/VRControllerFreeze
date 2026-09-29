#include "device_provider.h"

#include <cstdarg>
#include <cstdio>

#include "pose_hook.h"

namespace vr_controller_freeze {
namespace {

// A freeze captures the controller's most recent pose, which must be at most this old.
constexpr DWORD kMaxCapturePoseAgeMs = 500;
// The app counts as gone once its heartbeat is this stale: frozen hands are released and no new
// requests are acted on until it returns.
constexpr DWORD kAppWatchdogMs = 3000;

bool HeartbeatFresh(LONG heartbeat, DWORD now, DWORD maxAge) {
    if (heartbeat == 0) return false;
    return static_cast<DWORD>(now - static_cast<DWORD>(heartbeat)) <= maxAge;
}

void DriverLog(const char* format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (vr::VRDriverLog()) vr::VRDriverLog()->Log(buffer);
}

const char* HandName(bool left) { return left ? "left" : "right"; }

} // namespace

vr::EVRInitError DeviceProvider::Init(vr::IVRDriverContext* context) {
    VR_INIT_SERVER_DRIVER_CONTEXT(context);
    if (!sharedMemory_.OpenOrCreate()) {
        DriverLog("VRControllerFreeze: could not open shared memory (error %lu); driver disabled.", ::GetLastError());
        VR_CLEANUP_SERVER_DRIVER_CONTEXT();
        return vr::VRInitError_Driver_Failed;
    }
    state_ = sharedMemory_.Get();

    // This object is static and its DLL is pinned, so state from an earlier Init must not leak in.
    ResetRuntime();
    DiscardRequestsFromBeforeStart();
    WriteSharedLong(&state_->hookStatus, static_cast<LONG>(HookStatus::NotInstalled));
    WriteSharedLong(&state_->hookError, static_cast<LONG>(HookError::None));
    WriteSharedLong(&state_->hookErrorDetail, 0);
    PublishHand(leftRuntime_, true);
    PublishHand(rightRuntime_, false);
    WriteSharedLong(&state_->driverReady, 1);
    DriverLog("VRControllerFreeze: driver loaded; the pose hook is installed when VRControllerFreeze.exe starts.");
    return vr::VRInitError_None;
}

void DeviceProvider::Cleanup() {
    pose_hook::Deactivate();
    ResetRuntime();
    if (state_) {
        WriteSharedLong(&state_->driverReady, 0);
        WriteSharedLong(&state_->driverHeartbeatMs, 0);
        WriteSharedLong(&state_->hookStatus, static_cast<LONG>(HookStatus::NotInstalled));
        PublishHand(leftRuntime_, true);
        PublishHand(rightRuntime_, false);
    }
    state_ = nullptr;
    sharedMemory_.Close();
    VR_CLEANUP_SERVER_DRIVER_CONTEXT();
}

const char* const* DeviceProvider::GetInterfaceVersions() { return vr::k_InterfaceVersions; }

void DeviceProvider::ResetRuntime() {
    leftRuntime_ = HandRuntime{};
    rightRuntime_ = HandRuntime{};
    hookAttempted_ = false;
    hookActive_ = false;
}

// After a SteamVR restart the app may still hold a request from the previous session (for example
// "freeze" for a hand that was frozen). Acting on it now would freeze a hand nobody just asked for,
// so acknowledge it as discarded instead.
void DeviceProvider::DiscardRequestsFromBeforeStart() {
    for (bool left : {true, false}) {
        const HandFields f = HandFieldsFor(state_, left);
        HandRuntime& runtime = left ? leftRuntime_ : rightRuntime_;
        const LONG sequence = ReadSharedLong(f.sequence);
        runtime.lastProcessedRequestSequence = sequence;
        WriteSharedLong(f.result, static_cast<LONG>(ReadSharedLong(f.requested) != 0 ? FreezeRequestResult::ReleasedDriverRestart
                                                                                     : FreezeRequestResult::Live));
        WriteSharedLong(f.ack, sequence);
    }
}

void DeviceProvider::InstallHook() {
    hookAttempted_ = true;
    const pose_hook::InstallResult hook = pose_hook::Install(vr::VRServerDriverHost());
    hookActive_ = hook.status == HookStatus::Active;
    WriteSharedLong(&state_->hookError, static_cast<LONG>(hook.error));
    WriteSharedLong(&state_->hookErrorDetail, hook.detail);
    WriteSharedLong(&state_->hookStatus, static_cast<LONG>(hook.status));
    if (hookActive_) {
        DriverLog("VRControllerFreeze: pose hook active on TrackedDevicePoseUpdated.");
    } else {
        DriverLog("VRControllerFreeze: pose hook FAILED (step %ld, detail %ld). Freezing is unavailable.",
                  static_cast<long>(hook.error), static_cast<long>(hook.detail));
    }
}

void DeviceProvider::ReleaseHand(HandRuntime& runtime, bool left) {
    if (runtime.frozenDeviceIndex == kInvalidDeviceIndex) return;
    // Both hands could, in principle, target the same device; only unfreeze it once neither holds it.
    const HandRuntime& other = left ? rightRuntime_ : leftRuntime_;
    if (other.frozenDeviceIndex != runtime.frozenDeviceIndex) {
        pose_hook::Release(static_cast<std::uint32_t>(runtime.frozenDeviceIndex));
    }
    DriverLog("VRControllerFreeze: %s hand released (device %ld).", HandName(left), static_cast<long>(runtime.frozenDeviceIndex));
    runtime.frozenDeviceIndex = kInvalidDeviceIndex;
}

void DeviceProvider::ProcessHandRequest(HandRuntime& runtime, bool left) {
    const HandFields f = HandFieldsFor(state_, left);
    const LONG requestSequence = ReadSharedLong(f.sequence);
    if (requestSequence == runtime.lastProcessedRequestSequence) return;
    runtime.lastProcessedRequestSequence = requestSequence;

    FreezeRequestResult outcome = FreezeRequestResult::Live;
    if (ReadSharedLong(f.requested) != 0) {
        const LONG deviceIndex = ReadSharedLong(f.target);
        if (runtime.frozenDeviceIndex != kInvalidDeviceIndex) {
            outcome = FreezeRequestResult::Frozen;
        } else if (!hookActive_) {
            outcome = FreezeRequestResult::RejectedHookInactive;
        } else if (deviceIndex < 0 || deviceIndex >= static_cast<LONG>(vr::k_unMaxTrackedDeviceCount)) {
            outcome = FreezeRequestResult::RejectedNoController;
        } else {
            switch (pose_hook::Freeze(static_cast<std::uint32_t>(deviceIndex), kMaxCapturePoseAgeMs)) {
            case pose_hook::FreezeOutcome::Frozen:
                outcome = FreezeRequestResult::Frozen;
                runtime.frozenDeviceIndex = deviceIndex;
                DriverLog("VRControllerFreeze: %s hand frozen (device %ld).", HandName(left), static_cast<long>(deviceIndex));
                break;
            case pose_hook::FreezeOutcome::NoRecentPose:
                outcome = FreezeRequestResult::RejectedNoRecentPose;
                break;
            case pose_hook::FreezeOutcome::InvalidDevice:
                outcome = FreezeRequestResult::RejectedNoController;
                break;
            }
        }
        // requested<Hand>Frozen belongs to the app and is never written here; the result and the
        // acknowledged sequence tell the app the request was refused.
        if (outcome != FreezeRequestResult::Frozen) {
            WriteSharedLong(&state_->rejectedFreezeCount, ReadSharedLong(&state_->rejectedFreezeCount) + 1);
        }
    } else {
        ReleaseHand(runtime, left);
    }
    WriteSharedLong(f.result, static_cast<LONG>(outcome));
    WriteSharedLong(f.ack, requestSequence);
}

void DeviceProvider::ApplyWatchdog(bool appAlive) {
    auto releaseBoth = [&](FreezeRequestResult reason) {
        for (bool left : {true, false}) {
            HandRuntime& runtime = left ? leftRuntime_ : rightRuntime_;
            if (runtime.frozenDeviceIndex == kInvalidDeviceIndex) continue;
            ReleaseHand(runtime, left);
            WriteSharedLong(HandFieldsFor(state_, left).result, static_cast<LONG>(reason));
        }
    };

    if (!appAlive) releaseBoth(FreezeRequestResult::WatchdogReleased);
    if (ReadSharedLong(&state_->requestedEmergencyRelease) != 0) {
        releaseBoth(FreezeRequestResult::EmergencyReleased);
        pose_hook::ReleaseAll();
        // Publish the released hands before clearing the flag: the app takes the cleared flag as
        // confirmation, so it must never see the flag cleared while a hand still reads as frozen.
        PublishHand(leftRuntime_, true);
        PublishHand(rightRuntime_, false);
        WriteSharedLong(&state_->requestedEmergencyRelease, 0);
    }
}

void DeviceProvider::PublishHand(const HandRuntime& runtime, bool left) {
    const HandFields f = HandFieldsFor(state_, left);
    const LONG target = ReadSharedLong(f.target);
    const LONG updates = target >= 0 ? pose_hook::PoseUpdateCount(static_cast<std::uint32_t>(target)) : 0;
    // Publish the count before the device it belongs to; the app re-reads the device to detect a mismatch.
    WriteSharedLong(f.poseUpdateCount, updates);
    WriteSharedLong(f.poseUpdateDeviceIndex, target);
    WriteSharedLong(f.frozenDeviceIndex, runtime.frozenDeviceIndex);
    WriteSharedLong(f.frozen, runtime.frozenDeviceIndex != kInvalidDeviceIndex ? 1 : 0);
}

void DeviceProvider::RunFrame() {
    if (!state_) return;
    const DWORD now = ::GetTickCount();
    WriteSharedLong(&state_->driverHeartbeatMs, static_cast<LONG>(now));

    vr::VREvent_t event{};
    while (vr::VRServerDriverHost()->PollNextEvent(&event, sizeof(event))) {}

    const bool appAlive = HeartbeatFresh(ReadSharedLong(&state_->appHeartbeatMs), now, kAppWatchdogMs);
    // Nothing is hooked until VRControllerFreeze.exe is running.
    if (appAlive && !hookAttempted_) InstallHook();

    ApplyWatchdog(appAlive);
    if (appAlive) {
        ProcessHandRequest(leftRuntime_, true);
        ProcessHandRequest(rightRuntime_, false);
    }
    PublishHand(leftRuntime_, true);
    PublishHand(rightRuntime_, false);
}

} // namespace vr_controller_freeze
