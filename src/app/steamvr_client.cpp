#include "steamvr_client.h"

#include "win_util.h"

namespace vr_controller_freeze::app {
namespace {

constexpr char kAppKey[] = "com.vrcontrollerfreeze.app";
constexpr char kActionSet[] = "/actions/vrcontrollerfreeze";

} // namespace

bool SteamVrClient::Connect(std::vector<std::wstring>& warnings) {
    vr::EVRInitError initError = vr::VRInitError_None;
    system_ = vr::VR_Init(&initError, vr::VRApplication_Background);
    if (!system_ || initError != vr::VRInitError_None) {
        system_ = nullptr;
        return false;
    }

    const std::filesystem::path root = ExecutableDirectory();
    const std::string manifest = Utf8((root / L"vrcontrollerfreeze.vrmanifest").wstring());
    const std::string actions = Utf8((root / L"input" / L"actions.json").wstring());
    if (auto* apps = vr::VRApplications()) {
        const vr::EVRApplicationError addError = apps->AddApplicationManifest(manifest.c_str(), true);
        if (addError != vr::VRApplicationError_None) {
            warnings.push_back(L"SteamVR rejected this app's manifest: " + Widen(apps->GetApplicationsErrorNameFromEnum(addError)) + L".");
        }
        // Pass the real PID. The SDK documents 0 as "the calling process", but SteamVR records it
        // literally as PID 0, which leaves this app under an auto-generated key so the SteamVR
        // Bindings button would edit a different app's bindings.
        const vr::EVRApplicationError identifyError = apps->IdentifyApplication(::GetCurrentProcessId(), kAppKey);
        if (identifyError != vr::VRApplicationError_None) {
            warnings.push_back(L"SteamVR could not identify this app as " + Widen(kAppKey) + L": " +
                               Widen(apps->GetApplicationsErrorNameFromEnum(identifyError)) + L". Custom bindings may not apply.");
        }
    }
    if (auto* input = vr::VRInput()) {
        if (input->SetActionManifestPath(actions.c_str()) != vr::VRInputError_None) {
            warnings.push_back(L"SteamVR could not load this app's action manifest. Controller toggles are unavailable; desktop buttons still work.");
        }
        const std::string set = kActionSet;
        input->GetActionSetHandle(set.c_str(), &actionSet_);
        input->GetActionHandle((set + "/in/toggle_left").c_str(), &toggleLeft_);
        input->GetActionHandle((set + "/in/toggle_right").c_str(), &toggleRight_);
        input->GetActionHandle((set + "/in/toggle_both").c_str(), &toggleBoth_);
        input->GetActionHandle((set + "/out/haptic_left").c_str(), &hapticLeft_);
        input->GetActionHandle((set + "/out/haptic_right").c_str(), &hapticRight_);
    }
    return true;
}

void SteamVrClient::Disconnect() {
    if (!system_) return;
    vr::VR_Shutdown();
    system_ = nullptr;
    ResetActionHandles();
}

bool SteamVrClient::PollQuit() {
    if (!system_) return false;
    vr::VREvent_t event{};
    while (system_->PollNextEvent(&event, sizeof(event))) {
        if (event.eventType == vr::VREvent_Quit) {
            Disconnect();
            return true;
        }
    }
    return false;
}

LONG SteamVrClient::HandDevice(bool left) const {
    if (!system_) return kInvalidDeviceIndex;
    const vr::TrackedDeviceIndex_t index = system_->GetTrackedDeviceIndexForControllerRole(
        left ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
    return index == vr::k_unTrackedDeviceIndexInvalid ? kInvalidDeviceIndex : static_cast<LONG>(index);
}

std::wstring SteamVrClient::DeviceModel(LONG deviceIndex) const {
    if (deviceIndex < 0 || !system_) return {};
    char model[256]{};
    system_->GetStringTrackedDeviceProperty(static_cast<vr::TrackedDeviceIndex_t>(deviceIndex), vr::Prop_ModelNumber_String,
                                            model, sizeof(model));
    return Widen(model);
}

ToggleRequests SteamVrClient::PollToggles() {
    ToggleRequests requests;
    if (!system_ || actionSet_ == vr::k_ulInvalidActionSetHandle || !vr::VRInput()) return requests;
    vr::VRActiveActionSet_t active{};
    active.ulActionSet = actionSet_;
    active.ulRestrictedToDevice = vr::k_ulInvalidInputValueHandle;
    active.ulSecondaryActionSet = vr::k_ulInvalidActionSetHandle;
    if (vr::VRInput()->UpdateActionState(&active, sizeof(active), 1) != vr::VRInputError_None) return requests;
    requests.left = ReadPress(toggleLeft_);
    requests.right = ReadPress(toggleRight_);
    requests.both = ReadPress(toggleBoth_);
    return requests;
}

void SteamVrClient::ConfirmationHaptic(bool left, bool twice) {
    const vr::VRActionHandle_t action = left ? hapticLeft_ : hapticRight_;
    if (!system_ || action == vr::k_ulInvalidActionHandle || !vr::VRInput()) return;
    vr::VRInput()->TriggerHapticVibrationAction(action, 0.0f, 0.055f, 120.0f, 0.45f, vr::k_ulInvalidInputValueHandle);
    if (twice) vr::VRInput()->TriggerHapticVibrationAction(action, 0.10f, 0.055f, 120.0f, 0.45f, vr::k_ulInvalidInputValueHandle);
}

bool SteamVrClient::OpenBindings() {
    if (!system_ || !vr::VRInput()) return false;
    return vr::VRInput()->OpenBindingUI(kAppKey, actionSet_, vr::k_ulInvalidInputValueHandle, true) == vr::VRInputError_None;
}

void SteamVrClient::ResetActionHandles() {
    actionSet_ = vr::k_ulInvalidActionSetHandle;
    toggleLeft_ = toggleRight_ = toggleBoth_ = hapticLeft_ = hapticRight_ = vr::k_ulInvalidActionHandle;
}

bool SteamVrClient::ReadPress(vr::VRActionHandle_t action) const {
    if (action == vr::k_ulInvalidActionHandle || !vr::VRInput()) return false;
    vr::InputDigitalActionData_t data{};
    return vr::VRInput()->GetDigitalActionData(action, &data, sizeof(data), vr::k_ulInvalidInputValueHandle) == vr::VRInputError_None &&
           data.bActive && data.bChanged && data.bState;
}

} // namespace vr_controller_freeze::app
