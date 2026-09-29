#pragma once

#include <optional>

#include "../common/win_shared_memory.h"
#include "hand_logic.h"

namespace vr_controller_freeze::app {

// The app's side of the shared-memory protocol documented in shared_state.h.
class DriverLink {
public:
    struct HandDiagnostics {
        LONG poseUpdateCount;
        LONG poseUpdateDevice;
        LONG frozenDevice;
        LONG sequence;
        LONG ack;
        LONG result;
    };

    // Creates or opens the shared memory. On failure, GetLastError() describes why.
    bool Open();
    void Close();

    // Starts a session from a clean slate: heartbeat first, so the driver treats the new requests
    // as live, then no hand requested and fresh request sequences.
    void BeginSession();
    // Releases every hand and stops the heartbeat so the driver's watchdog also lets go.
    void EndSession();
    void Heartbeat();

    bool DriverHealthy() const;
    bool HookActive() const;
    std::wstring HookStatusDescription() const;
    LONG HookStatusValue() const;
    LONG HookFailureStep() const;
    LONG HookFailureDetail() const;

    // Publishes which device holds the hand role; the driver freezes that device when a freeze is processed.
    void PublishTarget(bool left, LONG deviceIndex);
    HandView View(bool left, LONG targetDevice) const;
    LONG RequestSequence(bool left) const;
    LONG AckSequence(bool left) const;
    LONG Result(bool left) const;
    // The pose-update count, only if the driver published it for expectedDevice.
    std::optional<LONG> PoseUpdateCount(bool left, LONG expectedDevice) const;

    // Sends a freeze (true) or unfreeze (false) request and returns its id.
    LONG SendRequest(bool left, bool freeze);
    void RequestEmergencyRelease();
    // True until the driver has released every hand and cleared the request.
    bool EmergencyReleasePending() const;

    HandDiagnostics Diagnostics(bool left) const;
    LONG RejectedCount() const;

private:
    HandFields Fields(bool left) const { return HandFieldsFor(state_, left); }

    SharedMemory memory_;
    SharedState* state_ = nullptr;
};

} // namespace vr_controller_freeze::app
