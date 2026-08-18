# ChromeMic 1.2 feature plan

## Goal

Upgrade ChromeMic into a polished Windows audio mixer that sends only the selected application's process tree plus an explicitly selected microphone into the virtual cable. Add strict GitHub update enforcement, safe local monitoring, useful microphone effects, clearer source balancing, and a redesigned native UI.

## Product behavior

- Keep selected-application process-tree isolation; Chrome remains the preferred application.
- Ask the user which active recording endpoint to use. Microphone mixing is optional and always defaults to off at launch; the saved endpoint is never opened until the user explicitly enables it.
- Mix application audio and microphone audio into the cable with separate source gains, then apply a final safety limiter.
- Offer microphone effects: Natural, Clear Mic, Broadcast, Radio, Robot, and Deep Tone. Effects run locally and only on the microphone path.
- Add a headphone-only microphone/effect monitor mode to an explicitly selected physical playback device. Chrome already plays normally, so the monitor must not duplicate application audio. It must reject the cable destination and warn about speaker feedback.
- Check `https://raw.githubusercontent.com/ashish12147/VOICEMOD/main/update.json` on startup and on Retry. Routing remains disabled until the check succeeds. If the published version is newer, the app blocks routing and exposes an Update button. Network failure also fails closed, matching the request that only the latest verified version may run.
- No audio recording, telemetry, background service, account, driver installation, or automatic executable replacement.

## Architecture

- Retain dependency-free C++20/Win32 and WASAPI.
- Add a bounded WinHTTP update client with strict HTTPS host/path, response-size, semantic-version, and download-URL validation. Run it on a background thread and deliver results to the UI by posted message.
- Extend endpoint inventory and settings for microphone and monitor selection.
- Extend `RouteConfig` with microphone ID, source gains, voice-effect mode, and optional monitor endpoint.
- Capture selected-app and microphone streams in the common PCM16 stereo 44.1-kHz format using Windows shared-mode conversion.
- Maintain independent bounded queues/drift control for application and microphone clocks. Mix with saturation-safe arithmetic and a final limiter; silence one source without stopping the other.
- Duplicate the processed microphone stream to an optional monitor renderer using an independent bounded queue so monitor drift or disconnect cannot corrupt the cable path. A monitor failure stops monitoring visibly but must not silently change endpoints.

## Work phases

1. Add update manifest/client, strict version comparison, UI state, retry/update actions, and deterministic tests.
2. Add microphone enumeration/selection, second capture path, independent source gain, and mixed-output tests.
3. Add voice-effect DSP with resettable state and deterministic signal tests.
4. Add safe mic/effect-only loopback test mode with device validation and lifecycle tests.
5. Redesign the Win32 interface around Update status, Sources, Voice style, Output, Monitor, and Live status sections.
6. Update version/resources/docs/build scripts to 1.2.0 and add WinHTTP linkage.
7. Run automated, live-device, Chrome/VB-CABLE, microphone-mix, isolation, update-network, and GUI smoke tests.
8. Run parallel audio, update-security, UX, and release audits; apply findings; rebuild deterministically.
9. Push `main` with `update.json`, the 1.2 source/package, and verify the live update endpoint recognizes 1.2.0.

## Acceptance criteria

- Routing cannot start until the live GitHub manifest confirms this build is current or newer than the published version.
- An older build is visibly blocked and receives a safe GitHub download action.
- The user explicitly chooses the microphone; application and microphone meters/gains are independently understandable.
- Selected-app audio and microphone voice both reach `CABLE Output`; unrelated application audio remains excluded.
- Natural/Clear/Broadcast/Radio/Robot/Deep Tone modes are stable, bounded, and cannot produce non-finite or over-ceiling samples.
- Monitor mode is opt-in, rejects virtual cable/internal endpoints, labels headphones-only feedback risk, and releases cleanly.
- Stop, process exit, microphone/device invalidation, cancellation, and app close remain bounded and fail closed.
- Release build/tests/audits pass, package hashes verify, and GitHub `main` serves the matching 1.2 update manifest.
