# ChromeMic build plan

## Goal

Build a fast, local-only Windows desktop utility that captures only a selected application's audio process tree (Chrome by default), applies safe gain/limiting, and sends it to the playback side of an installed true render-to-recording virtual-audio cable. Games then receive that cable's matching recording side as a microphone without game or unrelated-app sound being included. Mixer products may require additional vendor-specific routing.

## Technical boundary

Windows cannot expose a new microphone endpoint from an ordinary desktop process; that endpoint requires a signed audio driver. ChromeMic therefore targets existing true virtual cables such as VB-CABLE, uses name detection only as a setup hint, and allows an explicitly confirmed unrecognized endpoint. Mixer products such as VoiceMeeter or Sonar are not promised as plug-and-play because their internal routing must be configured separately. The app itself will not install or silently trust third-party drivers.

Per-application loopback uses Microsoft's process-loopback activation API and therefore requires Windows 10 build 20348 or later (including Windows 11). “Selected app only” includes the selected process and its child processes, which is necessary for multi-process browsers such as Chrome. Protected/DRM audio can still be unavailable.

## Architecture

- Dependency-free native C++20/Win32 desktop app, built with the installed MSVC and Windows 11 SDK, for a small single executable.
- A refreshable selector for visible desktop applications, with a unique-Chrome first-run default and fail-closed handling when a selected process exits or restarts.
- Windows Core Audio process-loopback capture using `ActivateAudioInterfaceAsync` and `PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE`.
- Buffered WASAPI output to the selected virtual-cable render endpoint.
- Windows shared-mode format conversion to the target endpoint, gain smoothing, mute, safe limiting, level metering, bounded buffering, and underrun/overflow recovery.
- Safe fail-stop on process exit or device invalidation, manual Refresh/reselection, and persisted non-secret application/cable settings.
- No accounts, telemetry, network service, browser extension, or audio recording to disk.

## Work phases

1. Replace endpoint-wide capture with Windows process-loopback activation and application enumeration.
2. Add Chrome-first application selection, include-target-process-tree capture, process-exit detection, settings, and updated UI copy. Do not add inverse capture because it would recapture this router's own cable output.
3. Add tests for process selection/classification, activation parameter construction, gain/limiting, buffering, drift, and lifecycle transitions.
4. Build and run automated tests on this Windows host.
5. Conduct parallel architecture, reliability/security, and UX audits.
6. Apply audit fixes, rebuild, retest, and produce a deterministic portable release folder/archive.
7. Document the exact setup: install/choose a virtual cable, select Chrome, select the cable playback side, and select its recording side as the game microphone.

## Host-specific fast path

This PC has Voicemod's internal virtual render/capture pair and VB-CABLE. ChromeMic blocks Voicemod's internal bridge and uses a true generic cable such as the installed VB-CABLE for host-side smoke and delivery testing.

## Acceptance criteria

- Start/stop routing without restarting the app.
- Select a visible application/process tree and virtual-cable destination endpoint.
- In default mode, Chrome audio is captured while game and unrelated-app audio is excluded.
- Application loopback does not mute or reroute the selected application's normal playback.
- Real-time level and clipping indicators; mute and gain controls work safely.
- Clear status/error messages when a device is missing, disconnected, or not a probable virtual cable.
- Audio is never written to disk and the application makes no network requests.
- Release build and tests pass, with audit findings resolved or explicitly documented.
