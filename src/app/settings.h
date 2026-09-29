#pragma once

// The app's saved preferences, stored per user under HKEY_CURRENT_USER\Software\VRControllerFreeze.
namespace vr_controller_freeze::app {

// Dark mode is the default until the user switches to light mode.
bool LoadDarkMode();
void SaveDarkMode(bool dark);

} // namespace vr_controller_freeze::app
