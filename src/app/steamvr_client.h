#pragma once

#include <openvr.h>

#include <string>
#include <vector>

#include "../common/shared_state.h"

namespace vr_controller_freeze::app {

// Controller-button toggles pressed since the last poll.
struct ToggleRequests {
    bool left = false;
    bool right = false;
    bool both = false;
};

// The app's connection to SteamVR, as a background application: it never starts SteamVR itself.
class SteamVrClient {
public:
    // Returns false if SteamVR is not running or refuses the connection. Problems that still leave
    // a usable connection are added to warnings.
    bool Connect(std::vector<std::wstring>& warnings);
    void Disconnect();
    bool Connected() const { return system_ != nullptr; }

    // Processes SteamVR events. Returns true if SteamVR is quitting; the connection is then closed.
    bool PollQuit();

    // The device holding the hand role, or kInvalidDeviceIndex.
    LONG HandDevice(bool left) const;
    std::wstring DeviceModel(LONG deviceIndex) const;

    ToggleRequests PollToggles();
    // One pulse to confirm a freeze, two to confirm an unfreeze.
    void ConfirmationHaptic(bool left, bool twice);
    bool OpenBindings();

private:
    void ResetActionHandles();
    bool ReadPress(vr::VRActionHandle_t action) const;

    vr::IVRSystem* system_ = nullptr;
    vr::VRActionSetHandle_t actionSet_ = vr::k_ulInvalidActionSetHandle;
    vr::VRActionHandle_t toggleLeft_ = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t toggleRight_ = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t toggleBoth_ = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t hapticLeft_ = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t hapticRight_ = vr::k_ulInvalidActionHandle;
};

} // namespace vr_controller_freeze::app
