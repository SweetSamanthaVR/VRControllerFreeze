#pragma once

#include <openvr_driver.h>

#include "../common/win_shared_memory.h"

namespace vr_controller_freeze {

// Registers no devices of its own. Once VRControllerFreeze.exe is running it installs the
// pose hook and turns the app's freeze requests into freezes of the real controllers holding the
// hand roles.
class DeviceProvider final : public vr::IServerTrackedDeviceProvider {
public:
    vr::EVRInitError Init(vr::IVRDriverContext* pDriverContext) override;
    void Cleanup() override;
    const char* const* GetInterfaceVersions() override;
    void RunFrame() override;
    bool ShouldBlockStandbyMode() override { return false; }
    void EnterStandby() override {}
    void LeaveStandby() override {}

private:
    struct HandRuntime {
        LONG lastProcessedRequestSequence = 0;
        LONG frozenDeviceIndex = kInvalidDeviceIndex;
        bool liveDisconnected = false; // the frozen controller reports itself disconnected (asleep)
    };

    void ResetRuntime();
    void DiscardRequestsFromBeforeStart();
    void InstallHook();
    void ProcessHandRequest(HandRuntime& runtime, bool left);
    void ReleaseHand(HandRuntime& runtime, bool left);
    void ApplyWatchdog(bool appAlive);
    void LogSleepingController(HandRuntime& runtime, bool left);
    void PublishHand(const HandRuntime& runtime, bool left);

    SharedMemory sharedMemory_;
    SharedState* state_ = nullptr;
    bool hookAttempted_ = false;
    bool hookActive_ = false;
    HandRuntime leftRuntime_;
    HandRuntime rightRuntime_;
};

} // namespace vr_controller_freeze
