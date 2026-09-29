#include <openvr_driver.h>
#include <cstring>
#include "device_provider.h"

static vr_controller_freeze::DeviceProvider g_provider;

// Entry point vrserver calls after loading the DLL. Only a server driver is provided.

extern "C" __declspec(dllexport) void* HmdDriverFactory(const char* interfaceName, int* returnCode) {
    if (std::strcmp(interfaceName, vr::IServerTrackedDeviceProvider_Version) == 0) return &g_provider;
    if (returnCode) *returnCode = vr::VRInitError_Init_InterfaceNotFound;
    return nullptr;
}
