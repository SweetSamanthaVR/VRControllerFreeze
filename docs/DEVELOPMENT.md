# Developing VR Controller Freeze

## Requirements

- Windows 10 or 11 x64.
- Visual Studio 2026 (Community or any other edition) with "Desktop development with C++" (MSVC, Windows SDK and the bundled CMake). `tools\build.ps1` finds Visual Studio's CMake if none is on `PATH`.
- Internet access for the first configure: CMake downloads OpenVR SDK 2.15.6 and MinHook 1.3.4 and checks each against a pinned SHA-256.
- Python 3 for the static checks, and Pillow only if you regenerate the icon.

## Build

```cmd
build.cmd                  :: Release build, staged in build\dist
build.cmd -Clean           :: delete build\ first
build.cmd -Test            :: build, then run every test (stops at the first failure)
build.cmd -Package         :: also zip build\dist as build\VRControllerFreeze-<version>-win-x64.zip
build.cmd -Configuration Debug
build.cmd -CertificateThumbprint <SHA-1>   :: also code-sign the app and driver
```

`build\dist` contains everything that ships, laid out exactly as the release zip: the app, `openvr_api.dll`, the SteamVR app manifest and input bindings, the driver folder, the README, the licences, and the install, uninstall and diagnostics launchers with their scripts. The scripts find the driver next to themselves in a release, or in `build\dist` when run from the project. A release has no `CMakeLists.txt`, so there they read the version from `VRControllerFreeze.exe`, which the build stamps with it.

The project's own C++ code builds with `/W4 /permissive-`, and warnings are errors in Release. Both binaries use Control Flow Guard, ASLR and DEP (`/guard:cf /DYNAMICBASE /NXCOMPAT`). MinHook's C sources are compiled with warnings suppressed.

## Project layout

```text
assets/            icon, SteamVR app manifest and input bindings
driver/            SteamVR driver manifest
src/common/        shared-memory contract used by both the app and the driver
src/driver/        driver_vrcontrollerfreeze.dll: pose hook and request handling
src/app/           VRControllerFreeze.exe
  app.*              connects the modules below
  hand_logic.*       decision logic with no UI or SteamVR calls (unit tested)
  driver_link.*      the app's side of the shared-memory protocol
  steamvr_client.*   SteamVR connection, hand roles, toggles, haptics
  app_log.*          Live Log and log file
  main_window.*      window: status pills, hand cards, drawn buttons, DPI-aware layout, dark and light themes
  settings.*         saved preferences (HKCU\Software\VRControllerFreeze)
src/version.rc.in  version resource template for both binaries
tests/             static checks, app logic unit tests, driver harness
tools/             build, install, uninstall, diagnostics and icon scripts
docs/              architecture, this guide, and the README's icon and screenshot
```

See [ARCHITECTURE.md](ARCHITECTURE.md) for how the pieces fit together.

## Names

| Used for | Name |
|---|---|
| Product, window and SteamVR display name | VR Controller Freeze |
| Files, folders, logs and Windows objects | `VRControllerFreeze` |
| SteamVR driver, driver DLL, app key and action set | `vrcontrollerfreeze` |
| C++ namespace | `vr_controller_freeze` |

The scripts take the product and driver names from `tools\steamvr-paths.ps1`.

## Tests

| Test | What it covers | Run |
|---|---|---|
| `tests/static_validate.py` | Project structure, key invariants, naming, version consistency, line endings | `python tests\static_validate.py` |
| `tests/app_logic_tests.cpp` | The app's decision logic: hand state, result text, and the pose-stream, moved-hand, acknowledgement and emergency-release monitors | `build\Release\app_logic_tests.exe` |
| `tests/driver_harness.cpp` | The real driver DLL in a fake SteamVR host: hook install, freeze, rejections, watchdog, restarts, shutdown, and chaining behind another pose hook | `build\Release\driver_harness.exe <driver dll> [--with-calibrator]` |

`build.cmd -Test` runs them all. The driver harness refuses to run while SteamVR or the app is open, because the real driver would act on its requests. CI (`.github/workflows/windows-build.yml`) runs the same command. Behaviour with a real headset is checked by hand before each release (see Release checklist).

## Versioning

The version lives only in `CMakeLists.txt` (`project(... VERSION x.y.z ...)`). The app, the version resources and the scripts all read it from there, so to release a new version, change it there. Describe what changed in the notes of the GitHub Release.

## Code signing

Unsigned binaries trigger Windows SmartScreen and some antivirus warnings, especially a DLL loaded into SteamVR. To sign, install a code-signing certificate in your certificate store and pass its SHA-1 thumbprint:

```cmd
build.cmd -CertificateThumbprint 0123456789ABCDEF0123456789ABCDEF01234567
```

Both binaries are signed with SHA-256 and an RFC 3161 timestamp (default `http://timestamp.digicert.com`; change it with `-TimestampUrl`). A self-signed certificate works for testing but is not trusted by other PCs.

## Icon

`assets/icon/VRControllerFreeze.ico` and the README's `docs/icon.png` are generated by `python tools\make-icon.py`, which draws each size separately so small sizes stay crisp.

`docs/screenshot.png` is a capture of the window in dark mode with the left hand frozen. Retake it when the window's look changes.

## Releasing

The workflow builds, tests and zips the project, then attaches the zip to the release, so every download is a clean, tested build of the tagged source.

1. Work through the release checklist below.
2. On GitHub, draft a new release whose tag matches the version in `CMakeLists.txt` (`v1.0.0` for 1.0.0).
3. Optional: to see the zip on the draft before anyone else can, open **Actions → Windows build → Run workflow** and enter the tag.
4. Publish the release. The workflow attaches `VRControllerFreeze-<version>-win-x64.zip`, replacing any earlier copy. A tag that does not match the version fails the workflow instead of attaching the wrong build.

The workflow's builds are unsigned, as the certificate stays on your PC. To ship signed binaries, build with `build.cmd -Clean -Test -Package -CertificateThumbprint <SHA-1>` and upload that zip to the release after the workflow has run, replacing its copy.

## Release checklist

1. `build.cmd -Clean -Test -Package` passes.
2. The zip works on its own: extracted to a new folder, `install-driver.cmd`, the app and `uninstall-driver.cmd` all run from there.
3. Checked with the headset: install, freeze and unfreeze each hand from the window and with the thumbstick press and hold, Emergency Release, closing the app with a hand frozen, and uninstall.
4. Version updated (see Versioning).
5. Binaries signed, if you have a certificate (see Releasing).
