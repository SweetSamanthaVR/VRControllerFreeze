#!/usr/bin/env python3
"""Structural checks for the VR Controller Freeze repository.

Behaviour is tested by tests/app_logic_tests.cpp and tests/driver_harness.cpp; these checks guard
project structure, key invariants, naming, version consistency and file hygiene.
"""
from __future__ import annotations
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
errors: list[str] = []


def text(rel: str) -> str:
    return (ROOT / rel).read_text(encoding='utf-8')


def project_files():
    for path in ROOT.rglob('*'):
        rel = path.relative_to(ROOT)
        if path.is_file() and rel.parts[0] not in ('build', '.git', '.vs', 'out'):
            yield path, rel


json_files = [
    'assets/input/actions.json',
    'assets/input/bindings_oculus_touch.json',
    'assets/vrcontrollerfreeze.vrmanifest',
    'driver/driver.vrdrivermanifest',
]
for rel in json_files:
    try:
        json.loads(text(rel))
    except Exception as exc:
        errors.append(f'{rel}: invalid JSON: {exc}')

cmake = text('CMakeLists.txt')
app = '\n'.join(p.read_text(encoding='utf-8') for p in sorted((ROOT / 'src' / 'app').glob('*.*')))
manifest = text('src/app/app.manifest')
shared = text('src/common/shared_state.h')
provider = text('src/driver/device_provider.cpp')
hook = text('src/driver/pose_hook.cpp')
harness = text('tests/driver_harness.cpp')
install = text('tools/install-driver.ps1')
uninstall = text('tools/uninstall-driver.ps1')
paths = text('tools/steamvr-paths.ps1')
build = text('tools/build.ps1')
workflow = text('.github/workflows/windows-build.yml')
bindings = json.loads(text('assets/input/bindings_oculus_touch.json'))

version_match = re.search(r'project\(VRControllerFreeze VERSION (\d+\.\d+\.\d+)', cmake)
version = version_match.group(1) if version_match else None
binding_inputs = {key for source in bindings['bindings']['/actions/vrcontrollerfreeze']['sources'] for key in source['inputs']}

checks = {
    # Version: one source of truth.
    'version defined in CMakeLists.txt': version is not None,
    'app version comes from CMake': 'APP_VERSION="${PROJECT_VERSION}"' in cmake and 'APP_VERSION' in app,
    'scripts read the version from CMakeLists.txt': "'CMakeLists.txt'" in paths and '$ProductVersion' in paths,
    'README does not repeat the version': version is not None and version not in text('README.md'),
    # Driver.
    'shared layout v1': 'kSharedVersion = 1' in shared and 'VRControllerFreeze.State.v1' in shared,
    'dependencies pinned by hash': cmake.count('URL_HASH SHA256=') == 2,
    'hooks TrackedDevicePoseUpdated': 'kPoseUpdatedSlot = 1' in hook and 'MH_CreateHook' in hook and 'MH_EnableHook' in hook,
    'hook passes through foreign pose layouts': 'poseSize != sizeof(vr::DriverPose_t)' in hook,
    'hook always calls original': hook.count('g_original(self, deviceIndex') == 2,
    'hook refuses to install unpinned': 'GET_MODULE_HANDLE_EX_FLAG_PIN' in hook and 'HookError::PinModuleFailed' in hook,
    'freeze age read under the lock': 'Freeze(std::uint32_t deviceIndex, DWORD maxPoseAgeMs)' in hook and 'const DWORD nowMs = ::GetTickCount();' in hook,
    'frozen pose has zero motion': 'vecVelocity[i] = 0.0' in hook and 'vecAngularVelocity[i] = 0.0' in hook,
    'freeze requires a fresh valid pose': 'kMaxCapturePoseAgeMs' in provider and 'lastPose.poseIsValid' in hook,
    'no virtual devices registered': 'TrackedDeviceAdded' not in provider,
    'hook installed only once the app runs': 'appAlive && !hookAttempted_' in provider,
    'requests from before a restart are discarded': 'DiscardRequestsFromBeforeStart();' in provider,
    'driver never writes the app request flag': 'WriteSharedLong(f.requested' not in provider and 'requestedLeftFrozen' not in provider,
    'no requests acted on while the app is gone': 'if (appAlive) {' in provider,
    'runtime reset on Init/Cleanup': provider.count('ResetRuntime();') >= 2,
    'watchdog': 'ApplyWatchdog' in provider and 'kAppWatchdogMs = 3000' in provider,
    'shared memory works across elevation': 'S:(ML;;NW;;;ME)' in text('src/common/win_shared_memory.h'),
    # App.
    'app split into modules': all((ROOT / 'src' / 'app' / f).is_file() for f in
                                  ('app.cpp', 'hand_logic.cpp', 'driver_link.cpp', 'steamvr_client.cpp', 'app_log.cpp', 'main_window.cpp')),
    'app identifies itself by real PID': 'IdentifyApplication(::GetCurrentProcessId(), kAppKey)' in app,
    'app logs SteamVR outage once': 'steamVrOutageLogged_' in app,
    'app rotates log during a session': 'GetFileSizeEx' in app,
    'app publishes hand-role targets': 'GetTrackedDeviceIndexForControllerRole' in app,
    'app warns when a frozen hand moves': 'FrozenRoleMonitor' in app and 'Not in effect' in app,
    'app reports hook visibility': 'Driver is intercepting' in app and 'Driver is not seeing pose updates' in app,
    'Live Log kept': 'Live Log' in app and 'Open Log Folder' in app,
    'per-monitor DPI aware': 'PerMonitorV2' in manifest and 'WM_DPICHANGED' in app and 'SystemParametersInfoForDpi' in app,
    'modern controls': 'Microsoft.Windows.Common-Controls' in manifest and 'src/app/app.manifest' in cmake,
    'icon embedded': (ROOT / 'assets/icon/VRControllerFreeze.ico').is_file() and 'IDI_APP_ICON ICON' in cmake,
    'version resources for both binaries': 'driver_version.rc' in cmake and 'app_version.rc' in cmake and (ROOT / 'src/version.rc.in').is_file(),
    'controller toggle is a long press': binding_inputs == {'long'},
    'hand cards and status pills from tested logic': 'SummariseHand' in app and 'WindowView' in app and 'BS_OWNERDRAW' in app,
    'dark mode by default with a light mode': 'DWMWA_USE_IMMERSIVE_DARK_MODE' in app and 'DarkMode_Explorer' in app
                                                 and 'Command::ToggleTheme' in app and 'value = 1' in app,
    'uninstaller removes app preferences': 'Software' in uninstall and 'VRControllerFreeze' in uninstall and 'Remove-Item' in uninstall,
    # Tests and tooling.
    'app logic unit tests built': 'add_executable(app_logic_tests' in cmake,
    'harness refuses a live session': 'OpenFileMappingW' in harness,
    'build script runs every test': all(t in build for t in ('app_logic_tests.exe', 'driver_harness.exe', '--with-calibrator', 'static_validate.py')),
    'build script can sign': 'signtool' in build and '/tr' in build,
    'CI runs the full test build': 'tools/build.ps1 -Test' in workflow or 'tools\\build.ps1 -Test' in workflow,
    'installer has no dead -WhatIf support': 'SupportsShouldProcess' not in install,
    'installer rolls back unregistered drivers': 'foreach ($other in $unregistered)' in install,
    'installer removes stale registrations': 'Get-RegisteredDriverCopies' in install,
    'uninstaller removes all registrations': 'Get-RegisteredDriverCopies' in uninstall,
    'uninstaller backs up before writing': 'Backup-VRSettings' in uninstall,
    'settings backups are pruned': 'Select-Object -Skip $Keep' in paths,
    'CMake external OpenVR warnings': '/external:anglebrackets' in cmake and '/external:W0' in cmake,
    'MinHook licence staged': 'THIRD_PARTY_MINHOOK_LICENSE.txt' in cmake,
    'developer guide present': (ROOT / 'docs/DEVELOPMENT.md').is_file(),
    'no wrapper scripts at the root': not list(ROOT.glob('*.ps1')),
}
for name, ok in checks.items():
    if not ok:
        errors.append(f'check failed: {name}')

# Pre-release project names, never shipped, so they may not appear anywhere.
# Split so this file does not match itself.
OLD_NAMES = ('sweet' + 'freeze', 'sweets' + 'vr', 'sweets' + '_vr', "sweet's" + ' vr')
TEXT_SUFFIXES = {'.cpp', '.h', '.in', '.manifest', '.md', '.ps1', '.json', '.yml', '.txt', '.py', '.cmd', '.vrmanifest', '.vrdrivermanifest'}
for path, rel in project_files():
    if path.suffix.lower() not in TEXT_SUFFIXES and path.name not in ('.gitignore', '.gitattributes', 'LICENSE', 'CMakeLists.txt'):
        continue
    data = path.read_bytes()
    content = data.decode('utf-8', errors='replace')
    # Keep repository punctuation plain and avoid accidental smart dash characters.
    if '\u2014' in content or '\u2013' in content:
        errors.append(f'{rel} contains en/em dash')
    # One project name, in every file's contents and path.
    if any(name in (content + str(rel)).lower() for name in OLD_NAMES):
        errors.append(f'{rel} still uses an old project name')
    # Line endings as declared in .gitattributes: CRLF for batch files, LF for everything else.
    crlf = data.count(b'\r\n')
    lf = data.count(b'\n') - crlf
    if path.suffix.lower() == '.cmd':
        if lf:
            errors.append(f'{rel} must use CRLF line endings')
    elif crlf:
        errors.append(f'{rel} must use LF line endings')

actions = json.loads(text('assets/input/actions.json'))
for binding in actions.get('default_bindings', []):
    if not (ROOT / 'assets/input' / binding['binding_url']).is_file():
        errors.append(f"default binding file missing: {binding['binding_url']}")

if 'Resolve-CMake' not in build or 'vswhere.exe' not in build:
    errors.append('build script does not auto-discover Visual Studio CMake')
for launcher in ['build.cmd', 'install-driver.cmd', 'uninstall-driver.cmd', 'collect-diagnostics.cmd']:
    if not (ROOT / launcher).is_file():
        errors.append(f'missing launcher {launcher}')
    elif f'tools\\{launcher[:-4]}.ps1' not in text(launcher):
        errors.append(f'{launcher} does not call tools\\{launcher[:-4]}.ps1')
if 'Initialize-ObjectProperty $settings "driver_$DriverName"' not in install:
    errors.append('installer does not use the canonical driver_<name> settings section')
if '$section = "driver_$DriverName"' not in uninstall:
    errors.append('uninstaller does not remove the driver_<name> settings section')

if errors:
    print('STATIC VALIDATION FAILED')
    for error in errors:
        print(' -', error)
    sys.exit(1)

print(f'Static validation passed ({len(checks)} checks).')
