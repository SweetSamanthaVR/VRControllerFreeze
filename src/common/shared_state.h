#pragma once

#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#else
using LONG = std::int32_t;
#endif

// Shared-memory contract between VRControllerFreeze.exe (the app) and
// driver_vrcontrollerfreeze.dll (the driver, loaded in vrserver).
//
// Request protocol, per hand:
//   1. The app writes requested<Hand>Frozen (1 = freeze, 0 = unfreeze), then increments <hand>RequestSequence.
//   2. The driver acts once per new sequence value, then publishes <hand>RequestResult and copies the
//      sequence into <hand>RequestAckSequence.
// Each field has exactly one writer, so neither side can overwrite the other's intent. The one exception
// is requestedEmergencyRelease, a one-shot flag the app sets and the driver clears once every hand is
// released and published as unfrozen; the app treats the cleared flag as confirmation. When the app
// sets it, it also clears both requested<Hand>Frozen flags, without a new sequence.
// While sequence != ack a request is pending; otherwise <hand>Frozen is the driver's authoritative state.
namespace vr_controller_freeze {

// Bump kSharedVersion and the mapping name together whenever SharedState's layout or meaning changes,
// so a mismatched app and driver never share memory.
inline constexpr LONG kSharedMagic = 0x46435653; // "SVCF" in memory order
inline constexpr LONG kSharedVersion = 1;
inline constexpr wchar_t kSharedMappingName[] = L"Local\\VRControllerFreeze.State.v1";
inline constexpr LONG kInvalidDeviceIndex = -1;

// State of the driver's hook on IVRServerDriverHost::TrackedDevicePoseUpdated. The hook is only
// installed once VRControllerFreeze.exe is running, so NotInstalled is normal before the app starts.
enum class HookStatus : LONG {
    NotInstalled = 0,
    Active = 1,
    Failed = 2,
};

// Which step of hook installation failed (published with HookStatus::Failed).
enum class HookError : LONG {
    None = 0,
    HostInterfaceUnavailable = 1,
    MinHookInitialiseFailed = 2,
    MinHookCreateFailed = 3,
    MinHookEnableFailed = 4,
    PinModuleFailed = 5,
};

enum class FreezeRequestResult : LONG {
    None = 0,
    Frozen = 1,
    Live = 2,
    RejectedNoController = 3,
    RejectedNoRecentPose = 4,
    RejectedHookInactive = 5,
    WatchdogReleased = 6,
    EmergencyReleased = 7,
    // The driver (re)started and discarded a request it could not safely act on.
    ReleasedDriverRestart = 8,
};

#pragma pack(push, 8)
struct SharedState {
    LONG magic;
    LONG version;

    // Written by the app.
    volatile LONG appHeartbeatMs;
    volatile LONG requestedLeftFrozen;
    volatile LONG requestedRightFrozen;
    volatile LONG requestedEmergencyRelease; // one-shot: set by the app, cleared by the driver once done
    volatile LONG leftRequestSequence;
    volatile LONG rightRequestSequence;
    // SteamVR device indices currently holding the left/right hand roles.
    volatile LONG leftTargetDeviceIndex;
    volatile LONG rightTargetDeviceIndex;

    // Written by the driver.
    volatile LONG driverHeartbeatMs;
    volatile LONG driverReady;
    volatile LONG hookStatus;
    volatile LONG hookError;
    volatile LONG hookErrorDetail; // MH_STATUS from MinHook, or GetLastError() for PinModuleFailed
    volatile LONG leftFrozen;
    volatile LONG rightFrozen;
    volatile LONG leftFrozenDeviceIndex;
    volatile LONG rightFrozenDeviceIndex;
    volatile LONG leftRequestAckSequence;
    volatile LONG rightRequestAckSequence;
    volatile LONG leftRequestResult;
    volatile LONG rightRequestResult;
    volatile LONG rejectedFreezeCount;
    // Running count of pose updates the hook has seen for the device named alongside it. Compare
    // successive values for change only; the count may wrap.
    volatile LONG leftPoseUpdateCount;
    volatile LONG rightPoseUpdateCount;
    volatile LONG leftPoseUpdateDeviceIndex;
    volatile LONG rightPoseUpdateDeviceIndex;
};
#pragma pack(pop)

static_assert(sizeof(LONG) == 4, "The shared-memory layout requires 32-bit LONG values.");

// The per-hand fields of SharedState, so both sides can handle either hand with one code path.
struct HandFields {
    volatile LONG* requested;
    volatile LONG* sequence;
    volatile LONG* ack;
    volatile LONG* result;
    volatile LONG* target;
    volatile LONG* frozen;
    volatile LONG* frozenDeviceIndex;
    volatile LONG* poseUpdateCount;
    volatile LONG* poseUpdateDeviceIndex;
};

inline HandFields HandFieldsFor(SharedState* state, bool left) {
    if (left) {
        return {&state->requestedLeftFrozen, &state->leftRequestSequence, &state->leftRequestAckSequence,
                &state->leftRequestResult, &state->leftTargetDeviceIndex, &state->leftFrozen,
                &state->leftFrozenDeviceIndex, &state->leftPoseUpdateCount, &state->leftPoseUpdateDeviceIndex};
    }
    return {&state->requestedRightFrozen, &state->rightRequestSequence, &state->rightRequestAckSequence,
            &state->rightRequestResult, &state->rightTargetDeviceIndex, &state->rightFrozen,
            &state->rightFrozenDeviceIndex, &state->rightPoseUpdateCount, &state->rightPoseUpdateDeviceIndex};
}

} // namespace vr_controller_freeze
