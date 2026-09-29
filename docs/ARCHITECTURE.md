# VR Controller Freeze architecture

## Data flow

```text
Quest Pro / Touch Pro
        |
Steam Link (driver_vrlink.dll in vrserver)
        |  IVRServerDriverHost::TrackedDevicePoseUpdated(device, pose)
        v
Freeze detour (driver_vrcontrollerfreeze.dll)  <-- shared memory <-- VRControllerFreeze.exe
        |  live: original pose / frozen: captured pose, zero motion
        v
pose hooks installed before this one, if any (Space Calibrator, SmoothTracking)
        |
vrserver -> SteamVR Input -> VRChat
```

Hooks on the same function run newest first. The driver installs its hook when the app starts, which is after every driver has loaded, so on a normal setup it runs first and other pose hooks see its output. A driver that hooks later would run before it.

## The desktop app

`VRControllerFreeze.exe` is split into modules under `src/app/`. `app.cpp` connects them: every 25 ms it refreshes the heartbeat and each hand's role owner, polls SteamVR, and handles toggles; every 250 ms it observes the driver and refreshes the status display. `hand_logic.cpp` holds the decisions (hand state, result text, and monitors for pose updates, moved hands and acknowledgements) with no UI or SteamVR calls, so it is unit tested. `driver_link.cpp` is the app's side of the shared-memory protocol, `steamvr_client.cpp` the SteamVR connection, `app_log.cpp` the Live Log, and `main_window.cpp` the per-monitor DPI-aware window. `app.cpp` builds a `WindowView` (status pills and one card per hand, with wording and colour from `hand_logic`) and the window repaints only when it changes. The window draws its header, pills, cards and buttons itself, with GDI+ for anti-aliased shapes, so dark and light themes look the same on every part.

## Why hook instead of adding virtual controllers

Over Steam Link the only source of controller tracking is the `vrlink` driver inside SteamVR. Replacing the controllers with virtual devices would need that tracking re-published, and SteamVR would still have to be persuaded to give the virtual devices the hand roles. Freezing the real devices' poses in place avoids both problems and leaves input, skeletal data and hand roles untouched.

## Pose hook

`src/driver/pose_hook.cpp` uses MinHook to detour the function body at slot 1 (`TrackedDevicePoseUpdated`) of the `IVRServerDriverHost_006` vtable obtained from the driver's own context. Because the function body is patched rather than a vtable entry, calls to that implementation from every driver's `IVRServerDriverHost_006` pointer are intercepted. Steam Link's `driver_vrlink.dll` uses `IVRServerDriverHost_006`. A driver built against an older host interface version may use a different implementation and would not be intercepted; the app detects this (see Failure containment).

The detour:
- records each device's latest pose and a per-device update counter;
- substitutes the captured pose for a frozen device, unless the incoming pose reports the device disconnected;
- always calls the original function, so other hooks and vrserver still run;
- passes through untouched any call whose pose-struct size differs from this SDK's `DriverPose_t`.

When another driver has already hooked the same function with MinHook (Space Calibrator, SmoothTracking), MinHook relocates that driver's jump into this driver's trampoline, so the hooks chain.

The hook is installed lazily, the first frame the app's heartbeat is seen, so installing the driver alone changes nothing in SteamVR. Before hooking, the driver pins its DLL; if pinning fails it does not hook at all. On `Cleanup` the detour is made inert rather than removed, so a driver thread still inside it can never return into unloaded code. A later `Init` in the same process reuses the detour.

## Freeze state

Freeze is keyed by SteamVR device index. `VRControllerFreeze.exe` publishes the indices returned by `GetTrackedDeviceIndexForControllerRole` for each hand. On a freeze request the driver copies the current target device's most recent pose, which must be valid, connected and at most 500 ms old; the age is measured under the same lock that stamps each pose. It then zeroes velocity, acceleration and `poseTimeOffset`, so prediction cannot drift it.

The frozen device index is kept until unfreeze. If the hand role moves to another device, for example when Steam Link switches to hand tracking, applications follow the new device and the freeze stops affecting the hand. The app warns, and the hand's card shows the freeze as not in effect; it does not move or release the freeze automatically, so a role that flips back briefly does not lose the captured pose.

Buttons, axes, skeletal input and haptics use `IVRDriverInput`, not the pose path, so they are unaffected.

## Shared memory and request protocol

The mapping is `Local\VRControllerFreeze.State.v1`; its name and `kSharedVersion` change together whenever the layout does, so a mismatched app and driver never share memory. `src/common/shared_state.h` documents the protocol. Each field has one writer: the app owns the request flags, sequences, hand targets and heartbeat; the driver owns results, acknowledgements, frozen state and diagnostics. The only exception is `requestedEmergencyRelease`, which the app sets and the driver clears once it has released every hand and published them as unfrozen; the app takes the cleared flag as confirmation and logs it, or reports if the driver stopped or did not answer within 2 seconds.

Per hand, the app writes the request (1 freeze, 0 unfreeze) and then increments the sequence. The driver acts once per new sequence and publishes one of `Frozen`, `Live`, `RejectedNoController`, `RejectedNoRecentPose`, `RejectedHookInactive`, `WatchdogReleased`, `EmergencyReleased` or `ReleasedDriverRestart`, then acknowledges the sequence. While the sequence and acknowledgement differ, a request is pending; otherwise the driver's frozen flag is authoritative.

On `Init` the driver acknowledges whatever request is already in shared memory without acting on it (`ReleasedDriverRestart` if it was a freeze). A SteamVR restart therefore never reapplies a freeze from the previous session.

Whichever process creates the mapping initialises it and publishes the magic value last; a process that opens an existing mapping waits for that rather than re-initialising live state. The mapping's security descriptor grants SYSTEM, Administrators and the current user full access with a medium integrity label, so it works when only one of SteamVR and the app runs elevated.

## Failure containment

- The driver stays loaded if the hook fails, so the app can report the failure step and detail (MinHook status, or the Windows error for a pin failure).
- A stale app heartbeat (three seconds) releases both hands, and no new requests are acted on until the heartbeat returns.
- Emergency Release clears every frozen device.
- The app logs whether the hook is seeing pose updates for each hand's controller, the direct test of whether the active tracking driver can be frozen. The driver publishes each count together with the device it belongs to, and only a change in the count is treated as activity.

## Known limitations

- Freezing depends on the tracking driver reporting poses through `IVRServerDriverHost_006`. The app reports this directly ("Driver is intercepting ... pose updates").
- A freeze holds one device. If the hand role moves to another device (for example Steam Link switching to hand tracking), the freeze stops affecting that hand until the user unfreezes and freezes again; the app warns when this happens.
- The thumbstick long-press toggle relies on SteamVR delivering input to a background application. It works in SteamVR Home; whether it reaches the app while a game has focus depends on SteamVR. The desktop buttons always work.
- The installer and uninstaller rewrite `steamvr.vrsettings` through PowerShell's JSON conversion, which may reformat the file (values are preserved). A backup is always taken first.
- The binaries are only signed when a code-signing certificate is supplied; unsigned builds may trigger SmartScreen or antivirus warnings.
