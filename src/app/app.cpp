#include "app.h"

#include <shellapi.h>

#include <sstream>
#include <vector>

#include "settings.h"

namespace vr_controller_freeze::app {
namespace {

constexpr DWORD kReconnectPeriodMs = 2000;
constexpr DWORD kObservePeriodMs = 250; // logging and the status display; requests are handled every tick

const wchar_t* Label(bool left) { return left ? L"Left" : L"Right"; }
const wchar_t* Word(bool left) { return left ? L"left" : L"right"; }

} // namespace

int App::Run(HINSTANCE instance, int showCommand) {
    log_.Open();
    log_.SetListener([this](const std::wstring& text, size_t trimmed) { window_.SetLog(text, trimmed); });
    Log(LogLevel::Info, std::wstring(kProductName) + L" v" APP_VERSION L" starting. Persistent log: " + log_.DisplayFile());

    if (!link_.Open()) {
        std::wstringstream message;
        message << L"Could not create the shared memory used to talk to the SteamVR driver (error " << ::GetLastError() << L").";
        Log(LogLevel::Error, message.str());
        ::MessageBoxW(nullptr, message.str().c_str(), kProductName, MB_OK | MB_ICONERROR);
        return 1;
    }
    link_.BeginSession();
    for (Hand* hand : {&left_, &right_}) hand->ack.Ignore(link_.RequestSequence(hand->left));
    ConnectSteamVr();

    const std::wstring title = std::wstring(kProductName) + L" v" APP_VERSION;
    darkMode_ = LoadDarkMode();
    if (!window_.Create(instance, title, darkMode_, {[this] { OnTick(); }, [this](Command command) { OnCommand(command); }})) {
        Log(LogLevel::Error, L"Could not create the main window.");
        link_.EndSession();
        return 2;
    }
    window_.SetLog(log_.VisibleText(), std::wstring::npos);
    RefreshUi();
    window_.Show(showCommand);
    const int exitCode = window_.RunMessageLoop();

    // Release everything before going; the driver also releases once the heartbeat goes stale.
    link_.EndSession();
    Log(LogLevel::Info, std::wstring(kProductName) + L" shutting down; any frozen hands are released.");
    steamVr_.Disconnect();
    link_.Close();
    return exitCode;
}

// Every tick: keep the heartbeat and hand targets current (the driver's watchdog and freeze
// targeting depend on them) and poll SteamVR. Logging and the status display refresh less often.
void App::OnTick() {
    link_.Heartbeat();
    if (steamVr_.PollQuit()) Log(LogLevel::Warn, L"SteamVR connection closed. The app will reconnect automatically.");
    if (!steamVr_.Connected() && ::GetTickCount() - lastReconnectAttemptMs_ >= kReconnectPeriodMs) ConnectSteamVr();
    UpdateHandTarget(left_);
    UpdateHandTarget(right_);

    const ToggleRequests toggles = steamVr_.PollToggles();
    if (toggles.left) Toggle(left_);
    if (toggles.right) Toggle(right_);
    if (toggles.both) ToggleBoth();

    const DWORD now = ::GetTickCount();
    if (now - lastObserveMs_ >= kObservePeriodMs) {
        lastObserveMs_ = now;
        ObserveDriver();
        RefreshUi();
    }
}

void App::OnCommand(Command command) {
    switch (command) {
    case Command::ToggleLeft: Toggle(left_); break;
    case Command::ToggleRight: Toggle(right_); break;
    case Command::ToggleBoth: ToggleBoth(); break;
    case Command::EmergencyRelease: EmergencyRelease(); break;
    case Command::Bindings:
        if (!steamVr_.OpenBindings()) Log(LogLevel::Error, L"SteamVR could not open the bindings editor. Is SteamVR running?");
        break;
    case Command::Diagnostics: ShowDiagnostics(); break;
    case Command::OpenLogFolder: OpenLogFolder(); break;
    case Command::ClearLog: log_.Clear(); break;
    case Command::ToggleTheme:
        darkMode_ = !darkMode_;
        window_.SetDarkMode(darkMode_);
        SaveDarkMode(darkMode_);
        break;
    }
    RefreshUi();
}

// Logs a SteamVR outage once, not on every retry.
void App::ConnectSteamVr() {
    lastReconnectAttemptMs_ = ::GetTickCount();
    std::vector<std::wstring> warnings;
    if (steamVr_.Connect(warnings)) {
        steamVrOutageLogged_ = false;
        Log(LogLevel::Info, L"Connected to SteamVR.");
        for (const std::wstring& warning : warnings) Log(LogLevel::Warn, warning);
    } else if (!steamVrOutageLogged_) {
        steamVrOutageLogged_ = true;
        Log(LogLevel::Warn, L"SteamVR is not running. The app will connect automatically when it starts.");
    }
}

// Publishes which device holds the hand role. The driver freezes whichever device this names at
// the moment a freeze request is processed.
void App::UpdateHandTarget(Hand& hand) {
    const LONG device = steamVr_.HandDevice(hand.left);
    link_.PublishTarget(hand.left, device);
    if (device == hand.device) return;
    hand.device = device;
    hand.model = steamVr_.DeviceModel(device);
    hand.stream.Reset();
    if (device < 0) {
        Log(LogLevel::Warn, std::wstring(Label(hand.left)) + L" hand: no controller currently holds this role.");
    } else {
        Log(LogLevel::Info, std::wstring(Label(hand.left)) + L" hand is " + DeviceText(device, hand.model) + L".");
    }
}

void App::Toggle(Hand& hand) { SendRequest(hand, !IsHeld(View(hand))); }

void App::ToggleBoth() {
    const bool freeze = !IsHeld(View(left_)) || !IsHeld(View(right_));
    SendRequest(left_, freeze);
    SendRequest(right_, freeze);
}

void App::SendRequest(Hand& hand, bool freeze) {
    const std::wstring label = Label(hand.left);
    if (!link_.DriverHealthy()) {
        Log(LogLevel::Error, label + L" request not sent: the SteamVR freeze driver is not running, so nothing can be frozen. "
                                     L"Is SteamVR running, and was install-driver.cmd run?");
        return;
    }
    if (freeze && !link_.HookActive()) {
        Log(LogLevel::Error, label + L" freeze not sent: the driver's pose hook is " + link_.HookStatusDescription() + L".");
        return;
    }
    if (freeze && hand.device < 0) {
        Log(LogLevel::Error, label + L" freeze not sent: SteamVR reports no " + Word(hand.left) + L" controller.");
        return;
    }
    const LONG id = link_.SendRequest(hand.left, freeze);
    std::wstringstream message;
    message << label << (freeze ? L" freeze" : L" unfreeze") << L" requested (#" << id << L").";
    Log(LogLevel::Info, message.str());
}

void App::EmergencyRelease() {
    link_.RequestEmergencyRelease();
    Log(LogLevel::Warn, L"Emergency release requested.");
    if (link_.DriverHealthy()) {
        emergencyRelease_.Start(static_cast<std::uint32_t>(::GetTickCount()));
    } else {
        Log(LogLevel::Info, L"The SteamVR freeze driver is not running, so nothing is frozen.");
    }
}

void App::ObserveEmergencyRelease(std::uint32_t nowMs) {
    const auto outcome = emergencyRelease_.Update(link_.EmergencyReleasePending(), link_.DriverHealthy(), nowMs);
    if (!outcome) return;
    switch (*outcome) {
    case EmergencyReleaseMonitor::Outcome::Confirmed:
        Log(LogLevel::Info, L"Emergency release confirmed: nothing is frozen.");
        break;
    case EmergencyReleaseMonitor::Outcome::DriverStopped:
        Log(LogLevel::Warn, L"Emergency release not confirmed: the SteamVR freeze driver stopped. Nothing is frozen while it is stopped.");
        break;
    case EmergencyReleaseMonitor::Outcome::TimedOut:
        Log(LogLevel::Error, L"Emergency release not confirmed: the driver did not answer within 2 seconds. "
                             L"Close this app to make the driver's watchdog release every hand, then collect diagnostics.");
        break;
    }
}

void App::ObserveDriver() {
    const int healthy = link_.DriverHealthy() ? 1 : 0;
    if (healthy != lastDriverHealthy_) {
        lastDriverHealthy_ = healthy;
        if (healthy) Log(LogLevel::Info, L"SteamVR freeze driver heartbeat is healthy.");
        else Log(LogLevel::Error, L"SteamVR freeze driver is not running or its heartbeat is stale. Nothing is frozen while it is stopped.");
    }
    // NotInstalled is normal for the moment between this app starting and the driver noticing it.
    const LONG hookStatus = healthy ? link_.HookStatusValue() : -1;
    if (healthy && hookStatus != lastHookStatus_ && hookStatus != static_cast<LONG>(HookStatus::NotInstalled)) {
        Log(hookStatus == static_cast<LONG>(HookStatus::Active) ? LogLevel::Info : LogLevel::Error,
            L"Driver pose hook: " + link_.HookStatusDescription() + L".");
    }
    lastHookStatus_ = hookStatus;

    const auto now = static_cast<std::uint32_t>(::GetTickCount());
    for (Hand* hand : {&left_, &right_}) {
        ObservePoseStream(*hand, now);
        ObserveFrozenRole(*hand);
        ObserveAck(*hand);
    }
    ObserveEmergencyRelease(now);
}

// Whether the hook is intercepting pose updates for this hand's controller: the direct signal
// that freezing can work with the active tracking driver (e.g. Steam Link).
void App::ObservePoseStream(Hand& hand, std::uint32_t nowMs) {
    if (hand.device < 0 || !link_.DriverHealthy() || !link_.HookActive()) {
        hand.stream.Reset();
        return;
    }
    const std::optional<LONG> count = link_.PoseUpdateCount(hand.left, hand.device);
    if (!count) return;
    const auto change = hand.stream.Update(*count, nowMs);
    if (!change) return;
    const std::wstring device = DeviceText(hand.device, hand.model);
    // A frozen hand's controller was streaming through the hook when it was frozen (the freeze needs a
    // fresh pose), so if its updates stop now, the controller is asleep rather than bypassing the hook.
    const bool frozen = View(hand).frozen;
    if (frozen && *change == PoseStreamMonitor::State::Silent) {
        Log(LogLevel::Info, std::wstring(Label(hand.left)) + L" controller is asleep (" + device + L"); its frozen position is held.");
    } else if (frozen) {
        Log(LogLevel::Info, std::wstring(Label(hand.left)) + L" controller is awake again (" + device + L"); its hand stays frozen.");
    } else if (*change == PoseStreamMonitor::State::Streaming) {
        Log(LogLevel::Info, std::wstring(L"Driver is intercepting ") + Word(hand.left) + L" controller pose updates (" + device +
                                L"). Freezing is available.");
    } else {
        Log(LogLevel::Warn, std::wstring(L"Driver is not seeing pose updates for the ") + Word(hand.left) + L" controller (" + device +
                                L"). Wake the controller; if this persists, its tracking driver bypasses the hook and freezing cannot work.");
    }
}

// A freeze holds one device. If the hand role moves to another device (for example Steam Link
// switching to hand tracking), the freeze no longer affects the hand applications use; say so.
void App::ObserveFrozenRole(Hand& hand) {
    const HandView view = View(hand);
    const FrozenRoleMonitor::Change change = hand.role.Update(view.driverHealthy && view.frozen, view.frozenDevice, hand.device);
    std::wstringstream message;
    if (change == FrozenRoleMonitor::Change::Moved && hand.device < 0) {
        // A frozen controller keeps its hand role while asleep (the driver hides the disconnect), so
        // losing it means something else: the controller was switched off or lost by Steam Link.
        message << L"No device holds the " << Word(hand.left) << L" hand (is the " << Word(hand.left)
                << L" controller switched off?), so the freeze has no effect for now. It applies again when the controller returns.";
        Log(LogLevel::Warn, message.str());
    } else if (change == FrozenRoleMonitor::Change::Moved) {
        message << Label(hand.left) << L" hand moved from frozen device " << view.frozenDevice << L" to "
                << DeviceText(hand.device, hand.model) << L". The freeze no longer affects the " << Word(hand.left)
                << L" hand; unfreeze and freeze again.";
        Log(LogLevel::Warn, message.str());
    } else if (change == FrozenRoleMonitor::Change::Restored) {
        message << Label(hand.left) << L" hand is back on frozen device " << view.frozenDevice << L"; the freeze is in effect again.";
        Log(LogLevel::Info, message.str());
    }
}

// Logs the driver's result each time it acknowledges a new request, with a confirmation haptic.
void App::ObserveAck(Hand& hand) {
    const std::optional<LONG> ack = hand.ack.Update(link_.AckSequence(hand.left));
    if (!ack) return;
    const LONG result = link_.Result(hand.left);
    std::wstringstream message;
    message << Label(hand.left) << L" request #" << *ack << L" -> " << RequestResultText(result) << L".";
    Log(IsRejected(result) ? LogLevel::Error : LogLevel::Info, message.str());
    if (result == static_cast<LONG>(FreezeRequestResult::Frozen)) steamVr_.ConfirmationHaptic(hand.left, false);
    if (result == static_cast<LONG>(FreezeRequestResult::Live)) steamVr_.ConfirmationHaptic(hand.left, true);
}

// Builds what the window shows from the driver link, the SteamVR connection and each hand's
// monitors; the window repaints only if something changed.
void App::RefreshUi() {
    const bool connected = steamVr_.Connected();
    const bool healthy = link_.DriverHealthy();
    WindowView view;
    view.steamVr = SteamVrSummary(connected);
    view.driver = DriverSummary(connected, healthy);
    view.hook = HookSummary(healthy, link_.HookStatusValue(), link_.HookFailureStep(), link_.HookFailureDetail());
    for (const Hand* hand : {&left_, &right_}) {
        const HandView state = View(*hand);
        HandPanelView& panel = hand->left ? view.left : view.right;
        panel.summary = SummariseHand(state);
        panel.device = DeviceLabel(hand->device, hand->model);
        panel.tracking = TrackingSummary(hand->device, hand->stream.Current(), state.frozen);
        panel.held = IsHeld(state);
        panel.canFreeze = healthy && link_.HookActive() && hand->device >= 0;
    }
    window_.SetView(view);
}

void App::ShowDiagnostics() {
    const bool healthy = link_.DriverHealthy();
    std::wstringstream text;
    text << kProductName << L" v" APP_VERSION L"\nShared memory layout: v" << kSharedVersion
         << L"\nSteamVR connected: " << (steamVr_.Connected() ? L"yes" : L"no")
         << L"\nDriver heartbeat healthy: " << (healthy ? L"yes" : L"no (driver values below are from its last run)")
         << L"\nPose hook: " << link_.HookStatusDescription();
    for (const Hand* hand : {&left_, &right_}) {
        const DriverLink::HandDiagnostics d = link_.Diagnostics(hand->left);
        text << L"\n\n" << Label(hand->left) << L" target: " << DeviceText(hand->device, hand->model)
             << L"\n  pose updates seen: " << d.poseUpdateCount << L" (for device " << d.poseUpdateDevice << L")"
             << L"\n  frozen device: " << d.frozenDevice
             << L"\n  request seq/ack: " << d.sequence << L"/" << d.ack
             << L"\n  last result: " << RequestResultText(d.result);
    }
    text << L"\n\nRejected requests: " << link_.RejectedCount() << L"\n\nLog: " << log_.DisplayFile();
    ::MessageBoxW(window_.Handle(), text.str().c_str(), (std::wstring(kProductName) + L" Diagnostics").c_str(), MB_OK | MB_ICONINFORMATION);
}

void App::OpenLogFolder() {
    const auto result = reinterpret_cast<INT_PTR>(
        ::ShellExecuteW(window_.Handle(), L"open", log_.Directory().c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (log_.Directory().empty() || result <= 32) Log(LogLevel::Error, L"Windows could not open the log folder.");
}

} // namespace vr_controller_freeze::app
