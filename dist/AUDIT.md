# ChromeMic 1.2 release audit

## Outcome

ChromeMic 1.2 captures one exact application process tree, optionally captures one exact microphone, applies an independently selected voice effect and gain, mixes both sources, and renders the result to a separately installed virtual-audio cable. An optional mic/effect-only loopback test renders to a physical listening endpoint without duplicating the selected application's normal playback.

The release also has a strict update gate at startup and before every route start. Routing stays disabled until the repository's HTTPS manifest exactly matches the built-in `1.2.0` version. A newer version opens only a validated `ashish12147/VOICEMOD` GitHub release URL; an older manifest is rejected as stale or rollback data. ChromeMic does not download or execute it.

## Review tracks

- Audio architecture: dual shared-mode capture paths, independent bounded rings and drift controllers, saturating final mix, monitor-failure isolation, hard stop on enabled-microphone failure, and explicit PCM16 stereo/44.1-kHz process-loopback format.
- Effects/DSP: allocation-free stateful effects, mode-reset isolation, finite-value sanitization, linked stereo dynamics, smoothed per-source gains, and a final `-1 dBFS` limiter.
- Selection/UX: exact PID/path/creation-time application identity, exact endpoint IDs, mic/monitor off by default, physical-only monitor choices, no fallback from stale saved choices, separate meters and clear failure copy.
- Update security: HTTPS WinHTTP on a background thread, no-cache requests, redirects disabled, certificate-revocation checks, bounded response size, strict JSON/version parsing, duplicate/unknown-field rejection, and an exact repository/version/asset URL allow-list.
- Release integrity: static MSVC runtime, ASLR, high-entropy VA, DEP/NX, CFG, CET compatibility, reproducible-link settings, post-test staging, normalized archive timestamps, and SHA-256 manifests.

## Automated verification

The deterministic suite covers:

- PCM source mixing, absent-source silence, cancellation, game-output mute, and final headroom.
- All six voice modes, reset reproducibility, callback-chunk invariance, stereo coherence, invalid-mode fallback, mode distinction, Radio band-pass response, and Deep Tone pitch shift.
- Semantic-version parsing/comparison; strict valid manifest; bad host/path/version; duplicates; unknown fields; state preservation on failure; and the 16-KiB limit.
- Chrome recognition, application labels, saved identity selection, stale-selection failure, and process-loopback activation parameters.
- Live process-loopback activation and active endpoint enumeration on an interactive supported host.
- PCM16/24/32 and extensible PCM/float DSP; gain clamp/smoothing; ring wrap/overflow/underrun/zero capacity; and deterministic ±1000-ppm 30-minute drift simulations.
- Router validation for a missing enabled microphone and unsafe cable monitoring, plus repeated error lifecycle and concurrent Stop/Start.

The current-source host probes were also run directly on this machine. The combined application + physical-microphone + headphone-monitor smoke reached `Running` with the monitor active. The process-isolation signal test routed only its 997-Hz target into VB-CABLE and measured it at `-31.17 dBFS`; the unrelated 1601-Hz process measured `-138.43 dBFS`, or `107.26 dB` below the target. The package's `BUILD-INFO.txt` flags describe only the options used by the package-producing build command; these independent host results remain separate evidence.

## Safety and privacy

- Audio is held only in bounded RAM and is never written to disk or sent over the network.
- The update request contains no audio, process/device names, persistent identifier, telemetry, or account data.
- Routing never auto-starts. Microphone inclusion and loopback monitoring reset to off on launch.
- An enabled microphone failure stops routing rather than silently dropping the user's voice.
- A headphone-monitor failure disables only monitoring; game output continues.
- Monitoring into the cable is rejected to avoid recursive capture. The UI limits monitor choices to non-virtual playback endpoints.
- The update manifest cannot redirect ChromeMic to another host or mismatched release path.

## Residual limits

- Application loopback requires Windows 10 build 20348 or later. There is no whole-device fallback.
- Chrome capture is process-tree-wide, not reliably per-tab.
- Paused/no-stream applications produce silence; DRM playback may be unavailable.
- Effects are lightweight local DSP, not AI voice cloning or studio noise reconstruction.
- Mic signal quality and a particular game's processing chain depend on external hardware/software and are not fully deterministic in automated tests.
- Strict fail-closed update policy means GitHub/network/TLS failure prevents starting a new route.
- Closing during an in-flight update request may wait for WinHTTP's bounded network timeouts before the process exits.
- A separate signed virtual-cable driver is still required.
- The executable is not Authenticode-signed; hashes do not replace publisher authentication or SmartScreen reputation.
