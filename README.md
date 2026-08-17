# ChromeMic 1.1

ChromeMic routes audio from one selected Windows application into a virtual microphone cable. Pick Google Chrome, play YouTube, and a game can receive that audio from the cable microphone while the game's own sound, voice chat, notifications, and unrelated applications stay excluded.

Chrome capture is process-tree-wide: all tabs and windows belonging to the selected Chrome process tree are included. It is not a per-tab selector. If Chrome is restarted, ChromeMic stops and requires **Refresh apps** and **Start routing** again instead of silently attaching to a different process.

## Requirements

- 64-bit Windows 10 build 20348 or later, or Windows 11.
- A trusted, signed render-to-recording virtual cable. [VB-CABLE](https://vb-audio.com/Cable/index.htm) is one option.
- Headphones are recommended to prevent ordinary acoustic feedback.

A desktop application cannot create a microphone endpoint by itself; Windows requires a signed audio driver. ChromeMic does not download or install drivers. Voicemod's internal render bridge is intentionally blocked because it is not a generic cable endpoint.

This local build is not Authenticode-signed. Windows may show a reputation warning after download or transfer. The release includes SHA-256 manifests for integrity checking, but a bundled hash does not authenticate the publisher by itself.

## Quick setup

1. Install a trusted virtual cable and reboot if its installer requests it.
2. Open Chrome and start the YouTube/video/audio you want to send.
3. Run `ChromeMic.exe`, then press **Refresh apps** if Chrome is not listed.
4. Under **Capture only this application**, select **Google Chrome — all tabs/windows in this process tree**.
5. Under **Send to virtual cable**, select the cable playback side. With VB-CABLE this is normally **CABLE Input**.
6. Leave gain at `0.0 dB`, press **Start routing**, and confirm the level meter moves.
7. In the game, select the cable recording side as its microphone. With VB-CABLE this is normally **CABLE Output**.

Use **Mute output** for instant silence while keeping the route open. **Stop** releases capture completely.

## Included features

- Application picker with Chrome preferred and a manual refresh button.
- Process-tree isolation: audio from unrelated apps and the game is excluded.
- Fail-closed PID, executable-path, and process-creation-time validation.
- Automatic stop if the selected application exits or restarts.
- Live output meter, adjustable `-12 dB` to `+6 dB` gain, click-free smoothing, mute, and an always-on `-1 dBFS` limiter.
- Bounded in-memory buffering, silence on underrun, and cross-device clock-drift compensation.
- Cable-side pairing guidance for the game's microphone selector.
- No recording, telemetry, account, updater, background service, or automatic network request.

Saved application path, destination ID, and gain are stored under `HKCU\Software\ChromeMic`. Audio remains in bounded RAM and is never written to disk.

## Important behavior

- Only the selected application's process tree is captured; choosing Chrome includes its audio-service and renderer child processes.
- ChromeMic does not capture a single tab. Use separate Chrome process trees/profiles only if Windows exposes them as separate selectable application roots.
- A selected application with no active render stream produces silence. Pausing YouTube is not an error.
- Protected or DRM-controlled playback may not be capturable.
- The app does not automatically reconnect to a restarted process. Refresh and confirm the selection before starting again.
- Do not choose speakers or headphones as the destination; choose the playback side of a real virtual cable.

## Troubleshooting

- **Chrome is missing:** open a visible Chrome window, then press **Refresh apps**.
- **Meter stays at zero:** make sure the selected Chrome process tree is actively playing audio. Refresh and reselect it if Chrome restarted. Protected content may remain silent.
- **Meter moves but the game hears nothing:** explicitly select the cable's recording side in the game, then restart the game if it cached its device list.
- **No cable appears:** finish installing the cable, reboot if requested, and press **Refresh apps**.
- **Music sounds cut or robotic:** disable the game's noise suppression, echo cancellation, and automatic gain control; enable an original-sound/music mode if offered.
- **Echo or feedback:** use headphones and disable Windows “Listen to this device,” microphone monitoring, and cable monitoring.
- **Route stops:** the selected app exited/restarted or an audio endpoint changed. Press **Refresh apps**, verify both choices, and start again.

## Build and test

The program is dependency-free C++20/Win32 using WASAPI. Build with Visual Studio 2022 Build Tools, the Desktop C++ workload, and Windows SDK 10.0.20348 or newer:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1
```

The script builds a statically linked x64 release, runs automated process-selection, activation-parameter, DSP, buffering, drift, lifecycle, and live Windows-audio tests, then publishes a whitelisted deterministic package. With Chrome open and a supported cable installed, request an endpoint-open smoke check with:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1 -HardwareSmoke
```

The hardware smoke check proves the selected application capture and cable-render clients can start. A stronger deterministic test sends two faint tones from separate processes, routes only one, captures the installed cable's recording side, and checks that the unrelated tone is excluded:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1 -ProcessIsolationTest
```

This signal test changes no default device or endpoint volume. It runs only when it can identify an unambiguous generic cable pair and a separate physical playback endpoint. `scripts\build.ps1` is the authoritative release pipeline. `CMakeLists.txt` is a secondary developer entry point.

## Architecture

```text
selected application process tree (Windows process loopback)
  -> fixed PCM capture + Windows format conversion
  -> smoothed gain + finite-sample sanitization + -1 dBFS limiter
  -> bounded overwrite-oldest frame ring + drift compensation
  -> shared-mode WASAPI render
  -> virtual cable playback side
  -> matching cable recording side selected as the game's microphone
```

Windows application loopback is endpoint-independent, so Chrome may keep playing through your normal headphones while only its captured process tree is copied into the cable.

See `AUDIT.md` for review coverage, verification evidence, and residual limits.
