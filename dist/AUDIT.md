# ChromeMic 1.1 release audit

## Outcome

ChromeMic 1.1 is a local-only x64 Windows router that captures one selected application's process tree and renders it to a separately installed virtual-audio cable. Selecting Chrome includes audio from that Chrome process tree while excluding the game and unrelated applications. The game must use the cable's matching recording side as its microphone.

The selector is Chrome-wide for the chosen process tree, not per-tab. ChromeMic does not create a microphone driver, silently change the selected process, or automatically attach after the selected app restarts.

## Independent review rounds

Three specialist review tracks covered the process-loopback engine, application-selection UX/reliability, and release/toolchain integrity. Findings were applied and re-audited.

- Audio architecture: replaced playback-endpoint loopback with Windows process-tree loopback; uses explicit stereo PCM16/44.1-kHz capture because the virtual process client does not expose a normal endpoint mix format; made asynchronous activation bounded and stop-aware; kept the completion handler self-contained for safe late callbacks; and rejected inverse/exclude mode because it can recapture ChromeMic's own cable render stream.
- Selection reliability: enumerates visible desktop application roots, prefers Chrome only for an empty initial choice, labels Chrome scope as all tabs/windows in its process tree, stores the executable path rather than a reusable PID, validates PID creation time and canonical image path before routing, and fails closed when the selected application exits or restarts.
- Release integrity: aligned the authoritative PowerShell build and secondary CMake project; required Windows SDK process-loopback headers; statically linked the MSVC runtime; enabled ASLR, high-entropy VA, DEP/NX, Control Flow Guard, CET compatibility, and reproducible-link settings; staged only whitelisted files after tests; normalized archive timestamps; and generated SHA-256 manifests.

## Automated verification

The release test suite covers:

- Chrome recognition, application labels, saved-path selection, and stale-selection failure.
- Exact process-loopback activation parameters and fixed capture format.
- Live desktop-application and audio-endpoint enumeration where the active window station permits it.
- Live process-loopback activation/initialization on a supported Windows build.
- PCM16/24/32 and extensible PCM/float DSP paths; finite-sample sanitization; gain clamp, smoothing, mute, and limiter behavior.
- Ring-buffer wrap, overflow, underrun, zero-capacity, and clear behavior.
- Drift conversion and deterministic ±1000-ppm 30-minute queue simulations.
- Invalid-route errors, repeated lifecycle transitions, and concurrent Stop/Start behavior.

Interactive host verification enumerated seven selectable desktop applications and all active audio endpoints. With a temporary isolated Chrome profile playing a local 997-Hz Web Audio tone, `scripts\build.ps1 -HardwareSmoke` successfully opened Chrome process-tree capture and rendered it to the installed VB-CABLE endpoint.

The stronger `scripts\build.ps1 -ProcessIsolationTest` check spawned two independent WASAPI renderer processes on a physical endpoint, routed only the 997-Hz target process into VB-CABLE, captured `CABLE Output`, and measured:

```text
Target 997 Hz:              -49.00 dBFS
Unrelated 1601 Hz:         -143.76 dBFS
Unrelated/target ratio:     -94.77 dB
Result: PASS
```

This directly verifies on the current host that target-process audio reaches the cable while audio from an unrelated process is excluded. A particular game's microphone-processing chain remains outside this deterministic test.

## Safety and privacy review

- No audio is written to disk or sent over a network.
- No account, telemetry, updater, service, or automatic network request is present.
- Routing stops on target exit, stale identity, device invalidation, or unrecoverable audio failure.
- The audio queue is bounded; underruns emit silence; output has smoothed gain and an always-on limiter.
- Voicemod's internal render bridge is blocked as an unsupported destination. A true signed render-to-recording cable is required.

## Residual limits

- Application loopback requires Windows 10 build 20348 or later. Unsupported systems fail rather than falling back to whole-device capture.
- Chrome isolation is process-tree-wide, not per-tab. Chrome profiles/windows sharing a process tree cannot be separated by this API.
- Paused applications and applications without an active render stream legitimately produce silence. Protected/DRM audio may not be available.
- A game may apply voice filters that degrade music; its noise suppression, echo cancellation, and automatic gain control may need adjustment.
- The local executable is not Authenticode-signed. SHA-256 manifests detect changed bytes when compared through a trusted channel but do not authenticate the publisher or prevent Windows reputation warnings.
