#pragma once

#include <openvr_driver.h>
#include <windows.h>
#include <cstdint>

#include "../common/shared_state.h"

// Intercepts IVRServerDriverHost::TrackedDevicePoseUpdated (IVRServerDriverHost_006), the call
// tracking drivers such as Steam Link's driver_vrlink use to report device poses. Poses pass
// through untouched unless the device is frozen, in which case the captured pose is submitted
// instead. Buttons, skeletal input and haptics never pass through this path, so they stay live
// while a pose is frozen.
namespace vr_controller_freeze::pose_hook {

struct InstallResult {
    HookStatus status = HookStatus::NotInstalled;
    HookError error = HookError::None;
    LONG detail = 0;
};

enum class FreezeOutcome {
    Frozen,
    NoRecentPose,
    InvalidDevice,
};

// Pins this module, then detours the host's TrackedDevicePoseUpdated. Safe to call again after
// Deactivate(): an existing detour is reused and re-activated.
InstallResult Install(vr::IVRServerDriverHost* host);

// Makes the detour pass every pose through untouched and releases all frozen devices. The detour
// itself stays installed and the module stays pinned, so a driver thread already inside it can
// never return into unloaded code.
void Deactivate();

// Freezes deviceIndex at its most recent pose, which must be valid, connected and at most
// maxPoseAgeMs old. Freezing an already-frozen device keeps the original capture.
FreezeOutcome Freeze(std::uint32_t deviceIndex, DWORD maxPoseAgeMs);

// Lets deviceIndex follow its live pose again. No effect if it is not frozen.
void Release(std::uint32_t deviceIndex);
void ReleaseAll();

// Running count of pose updates seen for deviceIndex. Compare successive values for change only.
LONG PoseUpdateCount(std::uint32_t deviceIndex);

} // namespace vr_controller_freeze::pose_hook
