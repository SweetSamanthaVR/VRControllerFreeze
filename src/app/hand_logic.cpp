#include "hand_logic.h"

#include <sstream>

namespace vr_controller_freeze::app {

bool IsHeld(const HandView& hand) {
    if (!hand.driverHealthy) return false;
    return hand.pending ? hand.requested : hand.frozen;
}

bool FreezeNotInEffect(const HandView& hand) {
    return hand.driverHealthy && !hand.pending && hand.frozen && hand.frozenDevice != hand.targetDevice;
}

HandSummary SummariseHand(const HandView& hand) {
    if (!hand.driverHealthy) {
        return {{L"Unavailable", Tone::Muted}, {L"The SteamVR freeze driver is not running.", Tone::Muted}};
    }
    if (hand.pending) {
        return {{hand.requested ? L"Freezing..." : L"Unfreezing...", Tone::Neutral}, {L"Waiting for the driver.", Tone::Muted}};
    }
    if (hand.frozen) {
        if (FreezeNotInEffect(hand)) {
            return {{L"Frozen", Tone::Warning},
                    {hand.targetDevice < 0 ? L"Not in effect: the controller is put down or asleep."
                                           : L"Not in effect: this hand is now a different device.",
                     Tone::Warning}};
        }
        return {{L"Frozen", Tone::Accent}, {L"Held in place. Buttons, sticks and fingers stay live.", Tone::Muted}};
    }
    if (IsRejected(hand.result)) return {{L"Live", Tone::Neutral}, {RequestResultText(hand.result), Tone::Error}};
    return {{L"Live", Tone::Neutral}, {L"Following the controller.", Tone::Muted}};
}

Summary SteamVrSummary(bool connected) {
    return connected ? Summary{L"Connected", Tone::Good} : Summary{L"Not running", Tone::Warning};
}

Summary DriverSummary(bool steamVrConnected, bool driverHealthy) {
    if (driverHealthy) return {L"Running", Tone::Good};
    if (!steamVrConnected) return {L"Waiting for SteamVR", Tone::Muted};
    return {L"Not running", Tone::Error};
}

Summary HookSummary(bool driverHealthy, LONG status, LONG error, LONG detail) {
    if (!driverHealthy) return {L"Waiting for the driver", Tone::Muted};
    switch (static_cast<HookStatus>(status)) {
    case HookStatus::Active: return {L"Active", Tone::Good};
    case HookStatus::Failed: return {HookStatusText(status, error, detail), Tone::Error};
    default: return {L"Starting", Tone::Muted};
    }
}

std::wstring DeviceLabel(LONG deviceIndex, const std::wstring& modelName) {
    if (deviceIndex < 0) return L"No controller in this hand";
    std::wstringstream text;
    if (modelName.empty()) text << L"Device " << deviceIndex;
    else text << modelName << L", device " << deviceIndex;
    return text.str();
}

Summary TrackingSummary(LONG deviceIndex, PoseStreamMonitor::State state) {
    if (deviceIndex < 0) return {L"Controller put down, asleep or off", Tone::Muted};
    switch (state) {
    case PoseStreamMonitor::State::Streaming: return {L"Movement coming through", Tone::Good};
    case PoseStreamMonitor::State::Silent: return {L"No movement coming through: wake the controller", Tone::Warning};
    default: return {L"Checking movement...", Tone::Muted};
    }
}

std::wstring RequestResultText(LONG result) {
    switch (static_cast<FreezeRequestResult>(result)) {
    case FreezeRequestResult::Frozen: return L"Frozen";
    case FreezeRequestResult::Live: return L"Live";
    case FreezeRequestResult::RejectedNoController: return L"Rejected: no controller holds this hand role";
    case FreezeRequestResult::RejectedNoRecentPose: return L"Rejected: no recent tracked pose from this controller (is it awake and tracking?)";
    case FreezeRequestResult::RejectedHookInactive: return L"Rejected: the driver's pose hook is not active";
    case FreezeRequestResult::WatchdogReleased: return L"Released by watchdog";
    case FreezeRequestResult::EmergencyReleased: return L"Emergency release";
    case FreezeRequestResult::ReleasedDriverRestart: return L"Discarded: the SteamVR driver restarted, nothing is frozen";
    default: return L"None";
    }
}

bool IsRejected(LONG result) {
    const auto value = static_cast<FreezeRequestResult>(result);
    return value == FreezeRequestResult::RejectedNoController || value == FreezeRequestResult::RejectedNoRecentPose ||
           value == FreezeRequestResult::RejectedHookInactive;
}

std::wstring HookStatusText(LONG status, LONG error, LONG detail) {
    switch (static_cast<HookStatus>(status)) {
    case HookStatus::Active: return L"Active";
    case HookStatus::Failed: {
        std::wstringstream text;
        text << L"FAILED (step " << error << L", detail " << detail << L")";
        return text.str();
    }
    default: return L"Not installed yet";
    }
}

std::wstring DeviceText(LONG deviceIndex, const std::wstring& modelName) {
    if (deviceIndex < 0) return L"no controller";
    std::wstringstream text;
    text << L"device " << deviceIndex;
    if (!modelName.empty()) text << L" (" << modelName << L")";
    return text.str();
}

void PoseStreamMonitor::Reset() {
    haveBaseline_ = false;
    sawChange_ = false;
    state_ = State::Unknown;
}

std::optional<PoseStreamMonitor::State> PoseStreamMonitor::Update(LONG count, std::uint32_t nowMs) {
    if (!haveBaseline_) {
        haveBaseline_ = true;
        lastCount_ = count;
        lastChangeMs_ = nowMs;
        return std::nullopt;
    }
    if (count != lastCount_) {
        lastCount_ = count;
        lastChangeMs_ = nowMs;
        sawChange_ = true;
    }
    const bool recent = nowMs - lastChangeMs_ <= timeoutMs_; // unsigned, so tick-count wrap is harmless
    if (!sawChange_ && recent) return std::nullopt;          // still waiting for the first update
    const State state = (sawChange_ && recent) ? State::Streaming : State::Silent;
    if (state == state_) return std::nullopt;
    state_ = state;
    return state;
}

FrozenRoleMonitor::Change FrozenRoleMonitor::Update(bool frozen, LONG frozenDevice, LONG targetDevice) {
    const bool mismatch = frozen && frozenDevice != targetDevice;
    if (mismatch == mismatch_) return Change::None;
    mismatch_ = mismatch;
    if (mismatch) return Change::Moved;
    return frozen ? Change::Restored : Change::None; // an unfreeze ends the mismatch silently
}

void EmergencyReleaseMonitor::Start(std::uint32_t nowMs) {
    pending_ = true;
    startMs_ = nowMs;
}

std::optional<EmergencyReleaseMonitor::Outcome> EmergencyReleaseMonitor::Update(bool flagStillSet, bool driverHealthy,
                                                                                 std::uint32_t nowMs) {
    if (!pending_) return std::nullopt;
    std::optional<Outcome> outcome;
    if (!flagStillSet) outcome = Outcome::Confirmed; // checked first: a driver that answered and then stopped still answered
    else if (!driverHealthy) outcome = Outcome::DriverStopped;
    else if (nowMs - startMs_ > timeoutMs_) outcome = Outcome::TimedOut;
    if (outcome) pending_ = false;
    return outcome;
}

std::optional<LONG> AckMonitor::Update(LONG ack) {
    if (!haveBaseline_) {
        haveBaseline_ = true;
        last_ = ack;
        return std::nullopt;
    }
    if (ack == last_) return std::nullopt;
    last_ = ack;
    if (ack <= 0 || ack == ignored_) return std::nullopt;
    return ack;
}

} // namespace vr_controller_freeze::app
