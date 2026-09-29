#include <windows.h>

#include <string>

#include "app.h"

using vr_controller_freeze::app::App;
using vr_controller_freeze::app::kProductName;

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    // One instance at a time: two apps would fight over the shared memory and the hand targets.
    HANDLE instanceMutex = ::CreateMutexW(nullptr, FALSE, L"Local\\VRControllerFreezeApp");
    if (!instanceMutex || ::GetLastError() == ERROR_ALREADY_EXISTS) {
        ::MessageBoxW(nullptr, (std::wstring(kProductName) + L" is already running.").c_str(), kProductName, MB_OK | MB_ICONINFORMATION);
        if (instanceMutex) ::CloseHandle(instanceMutex);
        return 0;
    }
    App app;
    const int exitCode = app.Run(instance, showCommand);
    ::CloseHandle(instanceMutex);
    return exitCode;
}
