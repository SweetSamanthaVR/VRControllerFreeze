#include "driver_link.h"

namespace vr_controller_freeze::app {
namespace {

constexpr DWORD kDriverHeartbeatMaxAgeMs = 1500;

bool Fresh(LONG heartbeat, DWORD maxAgeMs) {
    if (heartbeat == 0) return false;
    return static_cast<DWORD>(::GetTickCount() - static_cast<DWORD>(heartbeat)) <= maxAgeMs;
}

LONG NextSequence(volatile LONG* sequence) {
    LONG next = ReadSharedLong(sequence) + 1;
    if (next <= 0) next = 1; // 0 is never a valid request id, so the driver's initial state never matches
    WriteSharedLong(sequence, next);
    return next;
}

} // namespace

bool DriverLink::Open() {
    if (!memory_.OpenOrCreate()) return false;
    state_ = memory_.Get();
    return true;
}

void DriverLink::Close() {
    state_ = nullptr;
    memory_.Close();
}

void DriverLink::BeginSession() {
    Heartbeat();
    WriteSharedLong(&state_->requestedLeftFrozen, 0);
    WriteSharedLong(&state_->requestedRightFrozen, 0);
    WriteSharedLong(&state_->requestedEmergencyRelease, 0);
    NextSequence(&state_->leftRequestSequence);
    NextSequence(&state_->rightRequestSequence);
}

void DriverLink::EndSession() {
    RequestEmergencyRelease();
    WriteSharedLong(&state_->appHeartbeatMs, 0);
}

void DriverLink::Heartbeat() { WriteSharedLong(&state_->appHeartbeatMs, static_cast<LONG>(::GetTickCount())); }

bool DriverLink::DriverHealthy() const {
    return ReadSharedLong(&state_->driverReady) != 0 && Fresh(ReadSharedLong(&state_->driverHeartbeatMs), kDriverHeartbeatMaxAgeMs);
}

bool DriverLink::HookActive() const { return HookStatusValue() == static_cast<LONG>(HookStatus::Active); }

LONG DriverLink::HookStatusValue() const { return ReadSharedLong(&state_->hookStatus); }

LONG DriverLink::HookFailureStep() const { return ReadSharedLong(&state_->hookError); }

LONG DriverLink::HookFailureDetail() const { return ReadSharedLong(&state_->hookErrorDetail); }

std::wstring DriverLink::HookStatusDescription() const {
    return HookStatusText(HookStatusValue(), ReadSharedLong(&state_->hookError), ReadSharedLong(&state_->hookErrorDetail));
}

void DriverLink::PublishTarget(bool left, LONG deviceIndex) { WriteSharedLong(Fields(left).target, deviceIndex); }

HandView DriverLink::View(bool left, LONG targetDevice) const {
    const HandFields f = Fields(left);
    HandView view;
    view.driverHealthy = DriverHealthy();
    view.pending = ReadSharedLong(f.sequence) != ReadSharedLong(f.ack);
    view.requested = ReadSharedLong(f.requested) != 0;
    view.frozen = ReadSharedLong(f.frozen) != 0;
    view.frozenDevice = ReadSharedLong(f.frozenDeviceIndex);
    view.targetDevice = targetDevice;
    view.result = ReadSharedLong(f.result);
    return view;
}

LONG DriverLink::RequestSequence(bool left) const { return ReadSharedLong(Fields(left).sequence); }

LONG DriverLink::AckSequence(bool left) const { return ReadSharedLong(Fields(left).ack); }

LONG DriverLink::Result(bool left) const { return ReadSharedLong(Fields(left).result); }

std::optional<LONG> DriverLink::PoseUpdateCount(bool left, LONG expectedDevice) const {
    // The driver publishes the count before the device it belongs to, so reading the device on
    // both sides of the count detects a count from a different device.
    const HandFields f = Fields(left);
    const LONG deviceBefore = ReadSharedLong(f.poseUpdateDeviceIndex);
    const LONG count = ReadSharedLong(f.poseUpdateCount);
    if (deviceBefore != expectedDevice || ReadSharedLong(f.poseUpdateDeviceIndex) != deviceBefore) return std::nullopt;
    return count;
}

LONG DriverLink::SendRequest(bool left, bool freeze) {
    const HandFields f = Fields(left);
    WriteSharedLong(f.requested, freeze ? 1 : 0);
    return NextSequence(f.sequence);
}

void DriverLink::RequestEmergencyRelease() {
    WriteSharedLong(&state_->requestedLeftFrozen, 0);
    WriteSharedLong(&state_->requestedRightFrozen, 0);
    WriteSharedLong(&state_->requestedEmergencyRelease, 1);
}

bool DriverLink::EmergencyReleasePending() const { return ReadSharedLong(&state_->requestedEmergencyRelease) != 0; }

DriverLink::HandDiagnostics DriverLink::Diagnostics(bool left) const {
    const HandFields f = Fields(left);
    return {ReadSharedLong(f.poseUpdateCount), ReadSharedLong(f.poseUpdateDeviceIndex), ReadSharedLong(f.frozenDeviceIndex),
            ReadSharedLong(f.sequence),        ReadSharedLong(f.ack),                   ReadSharedLong(f.result)};
}

LONG DriverLink::RejectedCount() const { return ReadSharedLong(&state_->rejectedFreezeCount); }

} // namespace vr_controller_freeze::app
