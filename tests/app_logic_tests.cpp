// Unit tests for the app's decision logic (src/app/hand_logic.*), which has no UI or SteamVR
// dependencies. Build: cmake --build build --config Release --target app_logic_tests
// Run:   build\Release\app_logic_tests.exe
#include <climits>
#include <cstdio>
#include <string>

#include "hand_logic.h"

using namespace vr_controller_freeze;
using namespace vr_controller_freeze::app;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond, msg) do { ++g_checks; if (!(cond)) { std::printf("FAIL  %s\n", msg); ++g_failures; } else { std::printf("PASS  %s\n", msg); } } while (0)

static bool Contains(const std::wstring& text, const wchar_t* part) { return text.find(part) != std::wstring::npos; }

static HandView Healthy() {
    HandView view;
    view.driverHealthy = true;
    view.targetDevice = 1;
    return view;
}

static void TestHandState() {
    HandView view = Healthy();
    HandSummary summary = SummariseHand(view);
    CHECK(!IsHeld(view) && summary.state.text == L"Live" && summary.state.tone == Tone::Neutral, "idle hand is live and not held");

    view.frozen = true;
    view.frozenDevice = 1;
    summary = SummariseHand(view);
    CHECK(IsHeld(view) && summary.state.text == L"Frozen" && summary.state.tone == Tone::Accent, "frozen hand is held and shown in the accent colour");
    CHECK(!FreezeNotInEffect(view) && Contains(summary.note.text, L"Held in place"), "freeze on the role's device is in effect");

    view.targetDevice = 5;
    summary = SummariseHand(view);
    CHECK(FreezeNotInEffect(view) && summary.state.tone == Tone::Warning && Contains(summary.note.text, L"different device"),
          "freeze after the role moved is flagged");
    view.targetDevice = kInvalidDeviceIndex;
    summary = SummariseHand(view);
    CHECK(FreezeNotInEffect(view) && summary.note.tone == Tone::Warning && Contains(summary.note.text, L"put down or asleep"),
          "freeze while no device holds the role says the controller was put down");

    view = Healthy();
    view.pending = true;
    view.requested = true;
    CHECK(IsHeld(view) && SummariseHand(view).state.text == L"Freezing...", "pending freeze counts as held");
    view.frozen = true;
    view.requested = false;
    CHECK(!IsHeld(view) && SummariseHand(view).state.text == L"Unfreezing...", "pending unfreeze of a frozen hand is not held");

    view = Healthy();
    view.frozen = true;
    view.driverHealthy = false;
    summary = SummariseHand(view);
    CHECK(!IsHeld(view) && summary.state.text == L"Unavailable" && summary.state.tone == Tone::Muted, "nothing is held while the driver is down");
    CHECK(!FreezeNotInEffect(view), "no role warning while the driver is down");

    view = Healthy();
    view.result = static_cast<LONG>(FreezeRequestResult::RejectedNoRecentPose);
    summary = SummariseHand(view);
    CHECK(summary.state.text == L"Live" && summary.note.tone == Tone::Error && Contains(summary.note.text, L"no recent tracked pose"),
          "a rejection is shown until the next request");
    view.result = static_cast<LONG>(FreezeRequestResult::WatchdogReleased);
    CHECK(SummariseHand(view).note.tone == Tone::Muted, "a release is shown as live, not as an error");
}

static void TestStatusSummaries() {
    CHECK(SteamVrSummary(true).tone == Tone::Good && SteamVrSummary(false).tone == Tone::Warning, "SteamVR pill");
    CHECK(DriverSummary(true, true).tone == Tone::Good, "driver pill: running");
    CHECK(DriverSummary(false, false).tone == Tone::Muted, "driver pill: waiting while SteamVR is closed");
    CHECK(DriverSummary(true, false).tone == Tone::Error, "driver pill: an error when SteamVR runs without it");
    CHECK(HookSummary(true, static_cast<LONG>(HookStatus::Active), 0, 0).tone == Tone::Good, "hook pill: active");
    CHECK(HookSummary(true, static_cast<LONG>(HookStatus::NotInstalled), 0, 0).tone == Tone::Muted, "hook pill: starting");
    const Summary failed = HookSummary(true, static_cast<LONG>(HookStatus::Failed), 4, 9);
    CHECK(failed.tone == Tone::Error && Contains(failed.text, L"step 4"), "hook pill: failure names the step");
    CHECK(HookSummary(false, static_cast<LONG>(HookStatus::Active), 0, 0).tone == Tone::Muted, "hook pill: waiting while the driver is down");

    CHECK(DeviceLabel(kInvalidDeviceIndex, L"") == L"No controller in this hand", "device label: none");
    CHECK(DeviceLabel(1, L"Quest Pro Left") == L"Quest Pro Left, device 1", "device label: with model");
    CHECK(DeviceLabel(2, L"") == L"Device 2", "device label: without model");

    using State = PoseStreamMonitor::State;
    CHECK(TrackingSummary(1, State::Streaming).tone == Tone::Good, "tracking: movement coming through");
    CHECK(TrackingSummary(1, State::Silent).tone == Tone::Warning, "tracking: no movement is a warning");
    CHECK(TrackingSummary(1, State::Unknown).tone == Tone::Muted, "tracking: still checking");
    CHECK(TrackingSummary(kInvalidDeviceIndex, State::Streaming).tone == Tone::Muted, "tracking: no controller");
}

static void TestResultText() {
    CHECK(IsRejected(static_cast<LONG>(FreezeRequestResult::RejectedNoController)) &&
              IsRejected(static_cast<LONG>(FreezeRequestResult::RejectedNoRecentPose)) &&
              IsRejected(static_cast<LONG>(FreezeRequestResult::RejectedHookInactive)),
          "all three rejections are rejections");
    CHECK(!IsRejected(static_cast<LONG>(FreezeRequestResult::Frozen)) && !IsRejected(static_cast<LONG>(FreezeRequestResult::Live)) &&
              !IsRejected(static_cast<LONG>(FreezeRequestResult::ReleasedDriverRestart)),
          "successes and releases are not rejections");
    bool allNamed = true;
    for (LONG result = 1; result <= static_cast<LONG>(FreezeRequestResult::ReleasedDriverRestart); ++result) {
        if (RequestResultText(result) == L"None") allNamed = false;
    }
    CHECK(allNamed, "every request result has a description");
    CHECK(RequestResultText(999) == L"None", "unknown result is described as None");

    CHECK(HookStatusText(static_cast<LONG>(HookStatus::Active), 0, 0) == L"Active", "active hook text");
    CHECK(HookStatusText(static_cast<LONG>(HookStatus::NotInstalled), 0, 0) == L"Not installed yet", "not-installed hook text");
    CHECK(HookStatusText(static_cast<LONG>(HookStatus::Failed), 3, 11) == L"FAILED (step 3, detail 11)", "failed hook text names step and detail");

    CHECK(DeviceText(kInvalidDeviceIndex, L"") == L"no controller", "no device");
    CHECK(DeviceText(2, L"") == L"device 2", "device without a model name");
    CHECK(DeviceText(1, L"Quest Pro Left") == L"device 1 (Quest Pro Left)", "device with a model name");
}

static void TestPoseStream() {
    using State = PoseStreamMonitor::State;
    PoseStreamMonitor monitor(1500);
    CHECK(!monitor.Update(100, 1000) && monitor.Current() == State::Unknown, "first count is only a baseline");
    CHECK(!monitor.Update(100, 1500), "unchanged count inside the timeout reports nothing");
    auto change = monitor.Update(101, 1750);
    CHECK(change && *change == State::Streaming, "a changed count means updates are arriving");
    CHECK(!monitor.Update(140, 2000), "steady updates report nothing new");
    change = monitor.Update(140, 3600);
    CHECK(change && *change == State::Silent, "no change for longer than the timeout means silent");
    change = monitor.Update(141, 3700);
    CHECK(change && *change == State::Streaming, "updates resuming are reported");

    PoseStreamMonitor stale(1500);
    stale.Update(5000, 0);
    CHECK(!stale.Update(5000, 1000), "a stale non-zero count is not treated as activity");
    change = stale.Update(5000, 2000);
    CHECK(change && *change == State::Silent, "a stale count ends up silent");

    PoseStreamMonitor wrapping(1500);
    wrapping.Update(LONG_MAX, 0);
    change = wrapping.Update(LONG_MIN, 100);
    CHECK(change && *change == State::Streaming, "a counter wrapping past LONG_MAX is still a change");

    PoseStreamMonitor tick(1500);
    tick.Update(1, 0xFFFFFF00u);
    change = tick.Update(2, 0x00000010u);
    CHECK(change && *change == State::Streaming, "tick-count wrap does not break the timeout");

    monitor.Reset();
    CHECK(monitor.Current() == State::Unknown && !monitor.Update(500, 5000), "reset starts from a new baseline");
}

static void TestFrozenRole() {
    using Change = FrozenRoleMonitor::Change;
    FrozenRoleMonitor monitor;
    CHECK(monitor.Update(false, kInvalidDeviceIndex, 1) == Change::None, "not frozen: nothing to report");
    CHECK(monitor.Update(true, 1, 1) == Change::None, "frozen on the role's device: nothing to report");
    CHECK(monitor.Update(true, 1, 5) == Change::Moved, "role moving away from the frozen device is reported");
    CHECK(monitor.Update(true, 1, 5) == Change::None, "the move is reported once");
    CHECK(monitor.Update(true, 1, 1) == Change::Restored, "role returning to the frozen device is reported");

    FrozenRoleMonitor unfreeze;
    unfreeze.Update(true, 1, 5);
    CHECK(unfreeze.Update(false, kInvalidDeviceIndex, 5) == Change::None, "unfreezing ends a mismatch without a message");
    CHECK(unfreeze.Update(true, 5, 5) == Change::None, "a new freeze on the new device is fine");
}

static void TestAck() {
    AckMonitor monitor;
    CHECK(!monitor.Update(7), "first acknowledgement is only a baseline");
    CHECK(!monitor.Update(7), "an unchanged acknowledgement reports nothing");
    const auto next = monitor.Update(8);
    CHECK(next && *next == 8, "a new acknowledgement is reported");
    CHECK(!monitor.Update(0), "a zero acknowledgement is never reported");

    AckMonitor startup;
    startup.Ignore(4);
    startup.Update(3);
    CHECK(!startup.Update(4), "the app's own startup request is not reported when the driver acknowledges it late");
    const auto after = startup.Update(5);
    CHECK(after && *after == 5, "requests after the startup request are reported");
}

static void TestEmergencyRelease() {
    using Outcome = EmergencyReleaseMonitor::Outcome;
    EmergencyReleaseMonitor monitor(2000);
    CHECK(!monitor.Update(false, true, 0) && !monitor.Pending(), "nothing is reported before a release is requested");

    monitor.Start(1000);
    CHECK(monitor.Pending() && !monitor.Update(true, true, 1500), "waiting while the driver has not answered yet");
    auto outcome = monitor.Update(false, true, 1600);
    CHECK(outcome && *outcome == Outcome::Confirmed && !monitor.Pending(), "cleared flag confirms the release");
    CHECK(!monitor.Update(false, true, 1700), "the confirmation is reported once");

    monitor.Start(5000);
    outcome = monitor.Update(true, false, 5100);
    CHECK(outcome && *outcome == Outcome::DriverStopped, "driver stopping before answering is reported");

    monitor.Start(8000);
    CHECK(!monitor.Update(true, true, 10000), "no timeout at exactly the limit");
    outcome = monitor.Update(true, true, 10001);
    CHECK(outcome && *outcome == Outcome::TimedOut, "no answer within the limit times out");

    monitor.Start(9000);
    outcome = monitor.Update(false, false, 9100);
    CHECK(outcome && *outcome == Outcome::Confirmed, "an answer counts even if the driver stopped afterwards");

    monitor.Start(0xFFFFFF00u);
    CHECK(!monitor.Update(true, true, 0x00000100u), "tick-count wrap does not cause a false timeout");
}

int main() {
    TestHandState();
    TestStatusSummaries();
    TestResultText();
    TestPoseStream();
    TestFrozenRole();
    TestAck();
    TestEmergencyRelease();
    std::printf("\n%s (%d checks, %d failure%s)\n", g_failures ? "APP LOGIC TESTS FAILED" : "APP LOGIC TESTS PASSED", g_checks, g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
