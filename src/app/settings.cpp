#include "settings.h"

#include <windows.h>

namespace vr_controller_freeze::app {
namespace {

constexpr wchar_t kKey[] = L"Software\\VRControllerFreeze";
constexpr wchar_t kDarkModeValue[] = L"DarkMode";

} // namespace

bool LoadDarkMode() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    if (::RegGetValueW(HKEY_CURRENT_USER, kKey, kDarkModeValue, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) {
        return true;
    }
    return value != 0;
}

void SaveDarkMode(bool dark) {
    const DWORD value = dark ? 1 : 0;
    ::RegSetKeyValueW(HKEY_CURRENT_USER, kKey, kDarkModeValue, REG_DWORD, &value, sizeof(value));
}

} // namespace vr_controller_freeze::app
