# ChromeMic 1.2

ChromeMic mixes audio from one selected Windows application with an optional microphone and sends the result to a virtual microphone cable. Select Google Chrome for YouTube and the selected microphone for your voice; game audio, notifications, voice chat, and unrelated applications remain excluded.

Chrome capture is process-tree-wide: all tabs and windows in the selected Chrome process tree are included. It is not a per-tab selector. If Chrome restarts, ChromeMic stops and requires a fresh selection instead of silently attaching to another process.

## Requirements

- 64-bit Windows 10 build 20348 or later, or Windows 11.
- A trusted, signed render-to-recording virtual cable. [VB-CABLE](https://vb-audio.com/Cable/index.htm) is one option.
- Internet access to GitHub when ChromeMic starts or **Retry update check** is pressed. By request, routing is fail-closed until this exact version is confirmed current.
- Headphones for loopback testing, to prevent acoustic feedback.

A desktop application cannot create a microphone endpoint by itself; Windows requires a signed audio driver. ChromeMic does not install drivers. Voicemod's internal bridge is intentionally blocked as a cable destination.

This release is not Authenticode-signed. Windows may show a reputation warning. SHA-256 manifests check file integrity, but a bundled hash does not authenticate the publisher by itself.

## Quick setup

1. Install a trusted virtual cable and reboot if its installer requests it.
2. Open Chrome and play the YouTube/video/audio you want to send.
3. Run `ChromeMic.exe` and wait for **ChromeMic 1.2.0 is current**. If the check fails, verify internet access and press **Retry**.
4. Select Chrome under **Application audio**. Press **Refresh apps** if it is missing.
5. To add your voice, enable **Include microphone**, then select the exact microphone you want.
6. Select a microphone effect: **Natural**, **Clear Mic**, **Broadcast**, **Radio**, **Robot**, or **Deep Tone**. Effects change only your microphone.
7. Select the cable playback side under **Game cable**. With VB-CABLE this is normally **CABLE Input**.
8. Press **Start routing** and confirm the application, microphone, and game-output meters move.
9. In the game, select the cable recording side as its microphone. With VB-CABLE this is normally **CABLE Output**.

Use **Mute game output** for instant silence to the game while keeping capture open. **Stop** releases every audio endpoint.

## Loopback test mode

Enable **Loopback test — hear microphone/effect in headphones**, choose physical headphones, and start routing. This monitors only the selected microphone after its effect and gain; it does not replay Chrome because Chrome is already audible through its normal output.

Loopback testing is off at every launch and cannot target the game cable. Use headphones, keep the level low, and turn it off before using speakers. If the monitor disconnects, game routing continues and ChromeMic reports that monitoring stopped.

## Included features

- Exact application/process-tree picker with Chrome preferred on first use.
- Optional exact microphone selection and independent application/microphone gains from `-24 dB` to `+6 dB`.
- Six live microphone modes: Natural, Clear Mic, Broadcast, Radio, Robot, and Deep Tone.
- Mic/effect-only headphone loopback test, off by default.
- Three live meters, limiter warning, and game-output-only mute.
- Process identity validation using PID, creation time, and executable path; no silent fallback after a restart.
- Bounded queues, silence on underrun, independent clock-drift compensation, smoothed gains, finite-sample sanitization, and a final `-1 dBFS` limiter.
- A strict GitHub update gate using a small validated HTTPS manifest. Update links must match this repository and release version exactly.
- No recording, account, telemetry, background service, or automatic executable installation.

Saved application path, endpoint IDs, gains, and effect are stored under `HKCU\Software\ChromeMic`. Microphone inclusion and loopback monitoring are deliberately disabled again at every launch. Audio stays in bounded RAM and is never written to disk.

## Update and privacy behavior

At startup, before every route start, and on **Retry**, ChromeMic makes one HTTPS GET for [`update.json`](https://github.com/ashish12147/VOICEMOD/blob/main/update.json). It sends no audio, endpoint names, process names, identifiers, analytics, or account data. The response is capped at 16 KiB, parsed as a strict two-field manifest, and must point to the matching release ZIP in `ashish12147/VOICEMOD`.

Routing is allowed only when the manifest version exactly equals the running build. A newer version exposes **Update now** and opens only the validated GitHub release asset; an older manifest is rejected as stale or rollback data. ChromeMic never downloads or executes an update itself. Network, TLS, HTTP, or manifest-validation failure also blocks a new route because the requested policy is “run only on the latest update.”

## Important behavior

- Only the selected application's process tree is captured. Chrome includes its renderer/audio-service child processes.
- ChromeMic cannot isolate one tab when tabs share a process tree.
- A paused application or one with no active render stream produces silence. Protected/DRM playback may not be capturable.
- A missing enabled microphone stops the route rather than silently dropping your voice.
- A failed headphone monitor is isolated: the cable route continues.
- Do not choose speakers/headphones as the game destination; choose the playback side of a real virtual cable.
- Games may heavily filter music. Disable their noise suppression, echo cancellation, and automatic gain control where possible.

## Troubleshooting

- **Update check is blocked:** verify GitHub access, Windows date/time, proxy/firewall, then press **Retry**.
- **Chrome is missing:** open a visible Chrome window, then press **Refresh apps**.
- **Application meter is zero:** play non-protected audio and reselect Chrome if it restarted.
- **Mic meter is zero:** verify the selected endpoint and enable Windows microphone access for desktop apps.
- **Meters move but the game hears nothing:** select the cable recording side in the game, then restart the game if it cached devices.
- **Echo or feedback:** use headphones and disable Windows “Listen to this device” and other monitoring paths.
- **Music sounds cut or robotic in-game:** disable the game's voice cleanup/AGC or use its original-sound/music mode.

## Build and test

Build with Visual Studio 2022 Build Tools, the Desktop C++ workload, and Windows SDK 10.0.20348 or newer:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1
```

The authoritative script builds a static-runtime x64 release, runs parser, mixer, effects, process-selection, DSP, buffering, drift, lifecycle, and live Windows-audio tests, then stages a deterministic whitelisted package. Optional endpoint-open and process-isolation checks are:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1 -HardwareSmoke
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1 -MicrophoneSmoke -MonitorSmoke
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1 -ProcessIsolationTest
```

The microphone/monitor smoke briefly opens the first safe physical microphone and playback endpoint; use headphones before requesting it.

`CMakeLists.txt` is a secondary developer entry point.

## Architecture

```text
selected application process tree -> process loopback -> app gain/drift ----+
                                                                           +-> sum + final limiter -> virtual cable -> game microphone
selected microphone -> shared capture -> effect + mic gain/drift ----------+
                                             +-> optional physical-headphone loopback test
```

See `AUDIT.md` for verification evidence and residual limits.
