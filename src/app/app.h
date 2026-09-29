#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

#include "app_log.h"
#include "driver_link.h"
#include "hand_logic.h"
#include "main_window.h"
#include "steamvr_client.h"

namespace vr_controller_freeze::app {

inline constexpr wchar_t kProductName[] = L"VR Controller Freeze";

// Connects the window, the SteamVR client and the driver link, and turns what they report into
// the status display and the Live Log.
class App {
public:
    // Runs until the window closes. Returns the process exit code.
    int Run(HINSTANCE instance, int showCommand);

private:
    // A controller counts as "streaming" if the driver saw a pose update for it this recently.
    static constexpr std::uint32_t kPoseStreamTimeoutMs = 1500;
    // The driver answers an emergency release on its next frame; this allows for a busy SteamVR.
    static constexpr std::uint32_t kEmergencyReleaseTimeoutMs = 2000;

    // The app's view of one hand.
    struct Hand {
        explicit Hand(bool isLeft) : left(isLeft) {}
        bool left;
        LONG device = kInvalidDeviceIndex; // device currently holding the hand role
        std::wstring model;
        PoseStreamMonitor stream{kPoseStreamTimeoutMs};
        FrozenRoleMonitor role;
        AckMonitor ack;
    };

    void OnTick();
    void OnCommand(Command command);

    void Log(LogLevel level, const std::wstring& message) { log_.Write(level, message); }
    void ConnectSteamVr();
    void UpdateHandTarget(Hand& hand);
    HandView View(const Hand& hand) const { return link_.View(hand.left, hand.device); }

    void Toggle(Hand& hand);
    void ToggleBoth();
    void SendRequest(Hand& hand, bool freeze);
    void EmergencyRelease();

    void ObserveDriver();
    void ObservePoseStream(Hand& hand, std::uint32_t nowMs);
    void ObserveFrozenRole(Hand& hand);
    void ObserveAck(Hand& hand);
    void ObserveEmergencyRelease(std::uint32_t nowMs);
    void RefreshUi();
    void ShowDiagnostics();
    void OpenLogFolder();

    AppLog log_;
    DriverLink link_;
    SteamVrClient steamVr_;
    MainWindow window_;
    Hand left_{true};
    Hand right_{false};
    EmergencyReleaseMonitor emergencyRelease_{kEmergencyReleaseTimeoutMs};
    DWORD lastReconnectAttemptMs_ = 0;
    DWORD lastObserveMs_ = 0;
    bool steamVrOutageLogged_ = false;
    bool darkMode_ = true;
    int lastDriverHealthy_ = -1;
    LONG lastHookStatus_ = -1;
};

} // namespace vr_controller_freeze::app
