#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "../common/shared_state.h"

// The app's decision logic, kept free of Windows UI and SteamVR calls so it can be unit tested
// (tests/app_logic_tests.cpp).
namespace vr_controller_freeze::app {

// One hand's state as the UI needs it, combined from shared memory and the app's own view.
struct HandView {
    bool driverHealthy = false;
    bool pending = false;   // a request has been sent but not yet acknowledged
    bool requested = false; // the pending request is a freeze (rather than an unfreeze)
    bool frozen = false;    // the driver's state
    LONG frozenDevice = kInvalidDeviceIndex;
    LONG targetDevice = kInvalidDeviceIndex; // device currently holding the hand role
    LONG result = 0;                         // the driver's latest FreezeRequestResult for this hand
};

// Whether the hand is frozen or about to be: the pending request if there is one, otherwise the
// driver's state. Nothing is held while the driver is not running.
bool IsHeld(const HandView& hand);

// A frozen hand whose role has moved to another device, or that no device holds (the controller is
// put down or asleep), so the freeze no longer affects it.
bool FreezeNotInEffect(const HandView& hand);

// How a piece of status text should look: the window maps each tone to a theme colour.
enum class Tone { Neutral, Muted, Good, Accent, Warning, Error };

struct Summary {
    std::wstring text;
    Tone tone = Tone::Neutral;
    bool operator==(const Summary&) const = default;
};

// A hand's card: a short state ("Live", "Frozen") and a sentence explaining it.
struct HandSummary {
    Summary state;
    Summary note;
    bool operator==(const HandSummary&) const = default;
};

HandSummary SummariseHand(const HandView& hand);
Summary SteamVrSummary(bool connected);
Summary DriverSummary(bool steamVrConnected, bool driverHealthy);
Summary HookSummary(bool driverHealthy, LONG status, LONG error, LONG detail);
// The controller holding a hand role, for its card.
std::wstring DeviceLabel(LONG deviceIndex, const std::wstring& modelName);

std::wstring RequestResultText(LONG result);
bool IsRejected(LONG result);
std::wstring HookStatusText(LONG status, LONG error, LONG detail);
std::wstring DeviceText(LONG deviceIndex, const std::wstring& modelName);

// Decides whether the driver is seeing pose updates for a device, from a count it increments on
// every update. Only a change in the count is activity, so a stale count or a counter that wraps
// cannot look like updates.
class PoseStreamMonitor {
public:
    enum class State { Unknown, Streaming, Silent };

    explicit PoseStreamMonitor(std::uint32_t timeoutMs) : timeoutMs_(timeoutMs) {}

    void Reset();
    // Feeds the latest count. Returns the new state when it changes, so each change is reported once.
    std::optional<State> Update(LONG count, std::uint32_t nowMs);
    State Current() const { return state_; }

private:
    std::uint32_t timeoutMs_;
    bool haveBaseline_ = false;
    bool sawChange_ = false;
    LONG lastCount_ = 0;
    std::uint32_t lastChangeMs_ = 0;
    State state_ = State::Unknown;
};

// Whether pose updates are arriving for a hand's controller, for its card.
Summary TrackingSummary(LONG deviceIndex, PoseStreamMonitor::State state);

// Detects a frozen hand's role moving to another device, and moving back.
class FrozenRoleMonitor {
public:
    enum class Change { None, Moved, Restored };
    Change Update(bool frozen, LONG frozenDevice, LONG targetDevice);

private:
    bool mismatch_ = false;
};

// Follows an emergency release to its outcome. The driver clears the request flag only after every
// hand is released and published as unfrozen, so a cleared flag is the confirmation.
class EmergencyReleaseMonitor {
public:
    enum class Outcome { Confirmed, DriverStopped, TimedOut };

    explicit EmergencyReleaseMonitor(std::uint32_t timeoutMs) : timeoutMs_(timeoutMs) {}

    void Start(std::uint32_t nowMs);
    bool Pending() const { return pending_; }
    // Returns the outcome once, when it is known; nothing while still waiting or when not started.
    std::optional<Outcome> Update(bool flagStillSet, bool driverHealthy, std::uint32_t nowMs);

private:
    std::uint32_t timeoutMs_;
    bool pending_ = false;
    std::uint32_t startMs_ = 0;
};

// Reports each newly acknowledged request once. The first value seen is only a baseline.
class AckMonitor {
public:
    // Never reports this request: the unfreeze the app sends itself when it starts, which the
    // driver may acknowledge at any time (for example when SteamVR starts after the app).
    void Ignore(LONG sequence) { ignored_ = sequence; }
    std::optional<LONG> Update(LONG ack);

private:
    bool haveBaseline_ = false;
    LONG last_ = 0;
    LONG ignored_ = 0;
};

} // namespace vr_controller_freeze::app
