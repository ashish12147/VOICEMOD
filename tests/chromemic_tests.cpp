#include "app_processes.h"
#include "audio_devices.h"
#include "audio_router.h"
#include "dsp.h"
#include "process_loopback.h"
#include "update_checker.h"
#include "voice_effects_tests.inc"

#include <Windows.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmreg.h>

#include <atomic>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <wrl/client.h>

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

WAVEFORMATEX FloatStereo48k() {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = 2;
    format.nSamplesPerSec = 48000;
    format.wBitsPerSample = 32;
    format.nBlockAlign = 8;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    return format;
}

void TestRingWrapAndOverflow() {
    AudioFrameRing ring(4, sizeof(int));
    const int first[] = {1, 2, 3};
    Check(ring.Push(reinterpret_cast<const std::byte*>(first), 3) == 0, "initial push should not drop");
    int out[2]{};
    Check(ring.Pop(reinterpret_cast<std::byte*>(out), 2) == 2, "pop count");
    Check(out[0] == 1 && out[1] == 2, "pop order");

    const int second[] = {4, 5, 6, 7};
    Check(ring.Push(reinterpret_cast<const std::byte*>(second), 4) == 1, "overflow drops oldest frame");
    int finalOut[4]{};
    Check(ring.Pop(reinterpret_cast<std::byte*>(finalOut), 4) == 4, "full pop count");
    Check(finalOut[0] == 4 && finalOut[1] == 5 && finalOut[2] == 6 && finalOut[3] == 7,
          "overflow retains newest frames in order");
}

void TestRingOversizedPush() {
    AudioFrameRing ring(3, sizeof(int));
    const int values[] = {10, 11, 12, 13, 14};
    Check(ring.Push(reinterpret_cast<const std::byte*>(values), 5) == 2, "oversized push drop count");
    int output[3]{};
    ring.Pop(reinterpret_cast<std::byte*>(output), 3);
    Check(output[0] == 12 && output[1] == 13 && output[2] == 14, "oversized push keeps newest capacity");

    ring.Clear();
    Check(ring.SizeFrames() == 0, "ring clear resets occupancy");
    Check(ring.Pop(reinterpret_cast<std::byte*>(output), 1) == 0, "empty ring underflow returns no frames");

    AudioFrameRing zeroRing(0, sizeof(int));
    Check(zeroRing.Push(reinterpret_cast<const std::byte*>(values), 1) == 0, "zero-capacity ring rejects input safely");
    Check(zeroRing.Pop(reinterpret_cast<std::byte*>(output), 1) == 0, "zero-capacity ring pop is safe");
}

void TestLimiterAndMalformedFloat() {
    const auto format = FloatStereo48k();
    AudioProcessor processor(format);
    std::vector<float> samples = {0.25F, -0.25F, 2.0F, -2.0F,
                                  std::numeric_limits<float>::quiet_NaN(),
                                  std::numeric_limits<float>::infinity()};
    const DspResult result = processor.Process(reinterpret_cast<std::byte*>(samples.data()), 3, 0.0F, false);
    Check(result.limiting, "limiter should report limiting and sanitization");
    Check(result.peak <= 0.891251F, "limiter ceiling is -1 dBFS");
    for (float value : samples) {
        Check(std::isfinite(value), "processed float must be finite");
        Check(std::abs(value) <= 0.891251F, "processed float must respect ceiling");
    }
}

void TestMuteAndGainClamp() {
    const auto format = FloatStereo48k();
    AudioProcessor processor(format);
    float samples[] = {0.5F, -0.5F, 0.5F, -0.5F};
    const DspResult result = processor.Process(reinterpret_cast<std::byte*>(samples), 2, 99.0F, true);
    Check(result.peak == 0.0F, "muted peak");
    Check(samples[0] == 0.0F && samples[1] == 0.0F && samples[2] == 0.0F && samples[3] == 0.0F,
          "mute outputs silence");
    Check(AudioProcessor::ClampGainDb(99.0F) == 6.0F, "upper gain clamp");
    Check(AudioProcessor::ClampGainDb(-99.0F) == -24.0F, "lower gain clamp");
    Check(AudioProcessor::ClampGainDb(std::numeric_limits<float>::quiet_NaN()) == 0.0F, "NaN gain becomes zero");
}

void TestPcm16Mixing() {
    constexpr uint32_t frames = 3;
    const int16_t application[] = {10000, -10000, 30000, -30000, 1200, -1200};
    const int16_t microphone[] = {5000, -5000, 30000, -30000, -1200, 1200};
    int16_t output[frames * 2]{};

    const DspResult mixed = MixPcm16Stereo(
        reinterpret_cast<const std::byte*>(application),
        reinterpret_cast<const std::byte*>(microphone),
        reinterpret_cast<std::byte*>(output), frames, false);
    Check(output[0] > 14990 && output[0] < 15010 &&
              output[1] < -14990 && output[1] > -15010,
          "PCM16 mixer sums application and microphone samples");
    Check(mixed.limiting && mixed.peak <= 0.891251F,
          "PCM16 mix limiter protects summed sources at -1 dBFS");
    Check(output[4] == 0 && output[5] == 0,
          "PCM16 mixer preserves cancellation between sources");

    const DspResult applicationOnly = MixPcm16Stereo(
        reinterpret_cast<const std::byte*>(application), nullptr,
        reinterpret_cast<std::byte*>(output), 1, false);
    Check(output[0] > 9990 && output[0] < 10010 && applicationOnly.peak > 0.30F,
          "PCM16 mixer treats an absent microphone as silence");

    const DspResult muted = MixPcm16Stereo(
        reinterpret_cast<const std::byte*>(application),
        reinterpret_cast<const std::byte*>(microphone),
        reinterpret_cast<std::byte*>(output), frames, true);
    bool allSilent = true;
    for (int16_t sample : output) {
        allSilent = allSilent && sample == 0;
    }
    Check(allSilent && muted.peak == 0.0F && !muted.limiting,
          "game-output mute produces absolute silence after mixing");
    Check(MixPcm16Stereo(nullptr, nullptr, nullptr, frames, false).peak == 0.0F,
          "PCM16 mixer is null-output safe");
}

void TestUpdateManifestAndVersions() {
    SemanticVersion version;
    Check(ParseSemanticVersion("1.2.0", version) &&
              version.major == 1 && version.minor == 2 && version.patch == 0,
          "strict semantic version accepts MAJOR.MINOR.PATCH");
    Check(!ParseSemanticVersion("01.2.0", version) &&
              !ParseSemanticVersion("1.2", version) &&
              !ParseSemanticVersion("1.2.0-beta", version) &&
              !ParseSemanticVersion("4294967296.0.0", version),
          "strict semantic version rejects ambiguity, suffixes, and overflow");
    int comparison = 0;
    Check(CompareSemanticVersionStrings("1.2.0", "1.1.9", comparison) &&
              comparison > 0,
          "semantic versions compare numerically");

    constexpr std::string_view validManifest =
        R"({"version":"1.2.0","download_url":"https://github.com/ashish12147/VOICEMOD/releases/download/v1.2.0/ChromeMic-1.2.0-win-x64.zip"})";
    UpdateManifest manifest;
    Check(ParseUpdateManifest(validManifest, manifest) == UpdateManifestError::None &&
              manifest.version == "1.2.0",
          "strict update manifest accepts the matching official GitHub release URL");

    const std::string originalVersion = manifest.version;
    Check(ParseUpdateManifest(
              R"({"version":"1.2.0","version":"1.2.0","download_url":"https://github.com/ashish12147/VOICEMOD/releases/download/v1.2.0/ChromeMic-1.2.0-win-x64.zip"})",
              manifest) == UpdateManifestError::DuplicateField,
          "update manifest rejects duplicate security-sensitive fields");
    Check(ParseUpdateManifest(
              R"({"version":"1.2.0","download_url":"https://evil.example/ChromeMic.zip"})",
              manifest) == UpdateManifestError::InvalidDownloadUrl,
          "update manifest rejects non-official download hosts");
    Check(ParseUpdateManifest(
              R"({"version":"1.2.0","download_url":"https://github.com/ashish12147/VOICEMOD/releases/download/v1.2.1/ChromeMic-1.2.1-win-x64.zip"})",
              manifest) == UpdateManifestError::InvalidDownloadUrl,
          "update manifest rejects mismatched version and asset paths");
    Check(ParseUpdateManifest(
              R"({"version":"1.2.0","download_url":"https://github.com/ashish12147/VOICEMOD/releases/download/v1.2.0/ChromeMic-1.2.0-win-x64.zip","extra":true})",
              manifest) == UpdateManifestError::UnexpectedField,
          "update manifest rejects unknown fields");
    Check(manifest.version == originalVersion,
          "failed manifest parsing leaves the prior validated value unchanged");

    const std::string oversized(kMaximumUpdateManifestBytes + 1U, ' ');
    Check(ParseUpdateManifest(oversized, manifest) ==
              UpdateManifestError::ManifestTooLarge,
          "update manifest enforces the 16 KiB response bound");
}

void TestGainSmoothing() {
    auto format = FloatStereo48k();
    format.nChannels = 1;
    format.nBlockAlign = 4;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    AudioProcessor processor(format);
    std::vector<float> samples(480, 0.1F);
    processor.Process(reinterpret_cast<std::byte*>(samples.data()), static_cast<uint32_t>(samples.size()), 6.0F, false);
    Check(samples.front() > 0.1F && samples.front() < 0.11F, "gain change begins smoothly rather than jumping");
    Check(samples.back() > samples.front() + 0.04F && samples.back() < 0.2F, "gain smoothing converges over time");
}

void TestFrameCountConversion() {
    WAVEFORMATEX format = FloatStereo48k();
    format.nChannels = 1;
    format.nBlockAlign = 4;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    AudioProcessor processor(format);
    const float input[] = {0.0F, 0.2F, 0.4F, 0.6F, 0.8F};
    float compressed[4]{};
    Check(processor.ConvertFrameCount(reinterpret_cast<const std::byte*>(input), 5,
                                      reinterpret_cast<std::byte*>(compressed), 4),
          "5-to-4 drift conversion succeeds");
    Check(std::abs(compressed[0] - 0.0F) < 0.0001F && std::abs(compressed[3] - 0.8F) < 0.0001F,
          "drift conversion preserves endpoints");
    Check(compressed[1] > compressed[0] && compressed[2] > compressed[1] && compressed[3] > compressed[2],
          "drift conversion interpolates monotonically");

    float expanded[6]{};
    Check(processor.ConvertFrameCount(reinterpret_cast<const std::byte*>(input), 5,
                                      reinterpret_cast<std::byte*>(expanded), 6),
          "5-to-6 drift conversion succeeds");
    Check(std::abs(expanded[0] - 0.0F) < 0.0001F && std::abs(expanded[5] - 0.8F) < 0.0001F,
          "expanded conversion preserves endpoints");
}

void SimulateClockDrift(double partsPerMillion, const char* label) {
    constexpr size_t targetFrames = 2400;
    constexpr size_t deadbandFrames = 96;
    constexpr size_t capacityFrames = 12000;
    constexpr uint32_t writablePattern[] = {441, 480, 512, 447};
    constexpr size_t iterations = 180000; // 30 minutes at an average 10 ms service period.

    DriftCompensator controller(targetFrames, deadbandFrames);
    size_t queuedFrames = targetFrames;
    size_t minimumQueue = queuedFrames;
    size_t maximumQueue = queuedFrames;
    uint64_t compressedCycles = 0;
    uint64_t expandedCycles = 0;
    bool underflow = false;
    bool overflow = false;
    double producerAccumulator = 0.0;

    for (size_t iteration = 0; iteration < iterations; ++iteration) {
        const uint32_t outputFrames = writablePattern[iteration % std::size(writablePattern)];
        producerAccumulator += static_cast<double>(outputFrames) * (1.0 + partsPerMillion / 1000000.0);
        const size_t producedFrames = static_cast<size_t>(producerAccumulator);
        producerAccumulator -= static_cast<double>(producedFrames);
        queuedFrames += producedFrames;
        if (queuedFrames > capacityFrames) {
            overflow = true;
            break;
        }

        const uint32_t inputFrames = controller.InputFrames(queuedFrames, outputFrames);
        if (inputFrames > queuedFrames) {
            underflow = true;
            break;
        }
        queuedFrames -= inputFrames;
        compressedCycles += inputFrames > outputFrames ? 1 : 0;
        expandedCycles += inputFrames < outputFrames ? 1 : 0;
        minimumQueue = std::min(minimumQueue, queuedFrames);
        maximumQueue = std::max(maximumQueue, queuedFrames);
    }

    Check(!underflow, label);
    Check(!overflow, "drift controller prevents long-run queue overflow");
    Check(minimumQueue > 256 && maximumQueue < 6000, "drift controller keeps 30-minute queue occupancy bounded");
    if (partsPerMillion > 0.0) {
        Check(compressedCycles > 0 && compressedCycles > expandedCycles, "fast source is corrected by N+1 to N conversion");
    } else {
        Check(expandedCycles > 0 && expandedCycles > compressedCycles, "slow source is corrected by N-1 to N conversion");
    }
}

void TestClockDriftController() {
    SimulateClockDrift(1000.0, "fast-source drift does not underrun");
    SimulateClockDrift(-1000.0, "slow-source drift does not underrun");
}

void TestPcm16() {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = 44100;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 2;
    format.nAvgBytesPerSec = 88200;
    AudioProcessor processor(format);
    int16_t samples[] = {32767, -32768, 1000};
    const auto result = processor.Process(reinterpret_cast<std::byte*>(samples), 3, 0.0F, false);
    Check(result.limiting, "PCM16 full scale should enter limiter");
    Check(std::abs(static_cast<int>(samples[0])) <= 29205, "PCM16 positive ceiling");
    Check(std::abs(static_cast<int>(samples[1])) <= 29205, "PCM16 negative ceiling");
}

void TestAdditionalPcmFormats() {
    WAVEFORMATEX pcm24{};
    pcm24.wFormatTag = WAVE_FORMAT_PCM;
    pcm24.nChannels = 1;
    pcm24.nSamplesPerSec = 48000;
    pcm24.wBitsPerSample = 24;
    pcm24.nBlockAlign = 3;
    pcm24.nAvgBytesPerSec = 144000;
    AudioProcessor processor24(pcm24);
    std::byte sample24[] = {std::byte{0xFF}, std::byte{0xFF}, std::byte{0x7F}};
    const auto result24 = processor24.Process(sample24, 1, 0.0F, false);
    Check(processor24.IsSupported() && result24.limiting, "PCM24 processing and limiting");

    WAVEFORMATEX pcm32{};
    pcm32.wFormatTag = WAVE_FORMAT_PCM;
    pcm32.nChannels = 1;
    pcm32.nSamplesPerSec = 96000;
    pcm32.wBitsPerSample = 32;
    pcm32.nBlockAlign = 4;
    pcm32.nAvgBytesPerSec = 384000;
    AudioProcessor processor32(pcm32);
    int32_t sample32 = INT32_MAX;
    const auto result32 = processor32.Process(reinterpret_cast<std::byte*>(&sample32), 1, 0.0F, false);
    Check(processor32.IsSupported() && result32.limiting, "PCM32 processing and limiting");
    Check(sample32 < INT32_MAX, "PCM32 limiter lowers full scale");

    WAVEFORMATEXTENSIBLE extensible{};
    extensible.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    extensible.Format.nChannels = 1;
    extensible.Format.nSamplesPerSec = 48000;
    extensible.Format.wBitsPerSample = 32;
    extensible.Format.nBlockAlign = 4;
    extensible.Format.nAvgBytesPerSec = 192000;
    extensible.Format.cbSize = 22;
    extensible.Samples.wValidBitsPerSample = 24;
    extensible.dwChannelMask = SPEAKER_FRONT_CENTER;
    extensible.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    AudioProcessor extensibleProcessor(extensible.Format);
    int32_t valid24In32 = 0x7FFFFF00;
    extensibleProcessor.Process(reinterpret_cast<std::byte*>(&valid24In32), 1, 0.0F, false);
    Check(extensibleProcessor.IsSupported(), "24-valid-in-32 extensible PCM is supported");
    Check((valid24In32 & 0xFF) == 0, "extensible PCM preserves low padding bits");

    WAVEFORMATEXTENSIBLE extensibleFloat{};
    extensibleFloat.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    extensibleFloat.Format.nChannels = 1;
    extensibleFloat.Format.nSamplesPerSec = 48000;
    extensibleFloat.Format.wBitsPerSample = 32;
    extensibleFloat.Format.nBlockAlign = 4;
    extensibleFloat.Format.nAvgBytesPerSec = 192000;
    extensibleFloat.Format.cbSize = 22;
    extensibleFloat.Samples.wValidBitsPerSample = 32;
    extensibleFloat.dwChannelMask = SPEAKER_FRONT_CENTER;
    extensibleFloat.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    AudioProcessor extensibleFloatProcessor(extensibleFloat.Format);
    float floatSample = 0.25F;
    extensibleFloatProcessor.Process(reinterpret_cast<std::byte*>(&floatSample), 1, 0.0F, false);
    Check(extensibleFloatProcessor.IsSupported() && std::abs(floatSample - 0.25F) < 0.001F,
          "extensible float32 is supported");
}

void TestDeviceClassification() {
    Check(IsLikelyVirtualAudioName(L"CABLE Input (VB-Audio Virtual Cable)"), "VB-CABLE detection");
    Check(IsLikelyVirtualAudioName(L"Dummy Output (Voicemod Virtual Audio Device)"), "Voicemod detection");
    Check(IsLikelyVirtualAudioName(L"SteelSeries Sonar - Stream"), "Sonar detection");
    Check(!IsLikelyVirtualAudioName(L"Speakers (Realtek(R) Audio)"), "physical speaker is not classified virtual");
    Check(IsVoicemodAudioName(L"Microphone (Voicemod Virtual Audio Device)"), "Voicemod marker");
    Check(IsGenericVirtualCableName(L"CABLE Input (VB-Audio Virtual Cable)"), "VB-CABLE is a generic cable");
    Check(!IsGenericVirtualCableName(L"Dummy Output (Voicemod)"), "Voicemod internal output is not a generic cable");
    Check(!IsGenericVirtualCableName(L"VoiceMeeter Input (VB-Audio VoiceMeeter VAIO)"),
          "VoiceMeeter mixer endpoint requires explicit vendor routing");
    Check(!IsGenericVirtualCableName(L"SteelSeries Sonar - Stream"),
          "Sonar mixer endpoint is not auto-selected as a cable");
}

void TestApplicationSelectionHelpers() {
    AppProcessInfo editor;
    editor.processId = 10;
    editor.creationTime = 100;
    editor.windowTitle = L"Notes";
    editor.executableName = L"notepad.exe";
    editor.executablePath = L"C:\\Windows\\notepad.exe";

    AppProcessInfo chrome;
    chrome.processId = 20;
    chrome.creationTime = 200;
    chrome.windowTitle = L"Video title - YouTube";
    chrome.executableName = L"chrome.exe";
    chrome.executablePath = L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe";

    const std::vector<AppProcessInfo> applications = {editor, chrome};
    Check(IsChromeExecutableName(L"C:\\PROGRAM FILES\\Google\\Chrome.EXE"),
          "Chrome detection accepts a full path case-insensitively");
    Check(FindPreferredApplicationIndex(applications) == 1,
          "first-run application preference selects Chrome");
    Check(FindPreferredApplicationIndex(applications, 0, editor.executablePath) == 0,
          "saved executable path restores the matching application");
    Check(FindPreferredApplicationIndex(applications, 0, L"C:\\Missing\\gone.exe") == -1,
          "stale saved application fails closed");
    Check(FindPreferredApplicationIndex(applications, chrome.processId,
                                        chrome.executablePath, chrome.creationTime) == 1,
          "active application identity restores only its exact process generation");
    Check(FindPreferredApplicationIndex(applications, chrome.processId,
                                        chrome.executablePath, chrome.creationTime + 1) == -1,
          "reused PID with a different creation time fails closed");
    AppProcessInfo secondChrome = chrome;
    secondChrome.processId = 21;
    secondChrome.creationTime = 201;
    secondChrome.windowTitle = L"Other profile";
    const std::vector<AppProcessInfo> ambiguousChrome = {editor, chrome, secondChrome};
    Check(FindPreferredApplicationIndex(ambiguousChrome) == -1,
          "multiple Chrome process trees require an explicit choice");
    Check(FindPreferredApplicationIndex(ambiguousChrome, 0, chrome.executablePath) == -1,
          "ambiguous saved executable path requires an explicit choice");
    Check(BuildApplicationDisplayLabel(chrome).find(L"all tabs/windows") != std::wstring::npos,
          "Chrome label discloses process-tree-wide scope");
    Check(BuildApplicationDisplayLabel(chrome).find(chrome.windowTitle) != std::wstring::npos,
          "Chrome label includes a representative title to distinguish process trees");
}

void TestProcessLoopbackParameters() {
    constexpr DWORD processId = 4242;
    const AUDIOCLIENT_ACTIVATION_PARAMS parameters = MakeProcessLoopbackActivationParams(processId);
    Check(parameters.ActivationType == AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK,
          "activation type is process loopback");
    Check(parameters.ProcessLoopbackParams.TargetProcessId == processId,
          "activation parameters preserve the selected PID");
    Check(parameters.ProcessLoopbackParams.ProcessLoopbackMode ==
              PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE,
          "activation includes only the selected process tree");

    const WAVEFORMATEX format = MakeProcessLoopbackCaptureFormat();
    Check(format.wFormatTag == WAVE_FORMAT_PCM && format.nChannels == 2 &&
              format.nSamplesPerSec == 44100 && format.wBitsPerSample == 16 &&
              format.nBlockAlign == 4 && format.nAvgBytesPerSec == 176400,
          "process capture uses the Microsoft-sample-compatible fixed format");
}

void TestLiveApplicationEnumeration() {
    std::vector<AppProcessInfo> applications;
    std::wstring error;
    const bool listed = EnumerateDesktopApplications(applications, error);
    if (!listed) {
        // Managed/non-interactive build sandboxes can deny access to the user's
        // window station. Pure picker logic is tested above; an interactive
        // release audit runs this same binary outside that sandbox.
        std::wcout << L"Desktop application enumeration skipped: " << error << L"\n";
        return;
    }
    for (const auto& application : applications) {
        Check(application.processId != GetCurrentProcessId(), "application picker excludes ChromeMic itself");
        Check(application.processId != 0 && application.creationTime != 0 &&
                  !application.executableName.empty() && !application.executablePath.empty(),
              "enumerated application has a stable process identity");
    }
    std::wcout << L"Selectable applications: " << applications.size() << L".\n";
}

bool CurrentProcessIdentity(RouteConfig& config) {
    config.sourceProcessId = GetCurrentProcessId();
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, config.sourceProcessId);
    if (process == nullptr) {
        return false;
    }
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    bool success = GetProcessTimes(process, &created, &exited, &kernel, &user) != FALSE;
    ULARGE_INTEGER creation{};
    creation.LowPart = created.dwLowDateTime;
    creation.HighPart = created.dwHighDateTime;
    config.sourceProcessCreationTime = creation.QuadPart;

    std::wstring path(32768, L'\0');
    DWORD pathLength = static_cast<DWORD>(path.size());
    success = success && QueryFullProcessImageNameW(process, 0, path.data(), &pathLength) != FALSE;
    CloseHandle(process);
    if (!success || pathLength == 0) {
        return false;
    }
    path.resize(pathLength);
    config.sourceExecutablePath = std::move(path);
    config.sourceName = L"ChromeMic test process";
    return config.sourceProcessCreationTime != 0;
}

void TestLiveProcessLoopbackActivation() {
    const HRESULT apartmentResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Check(SUCCEEDED(apartmentResult), "process-loopback integration test initializes COM");
    if (FAILED(apartmentResult)) {
        return;
    }

    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, GetCurrentProcessId());
    Check(process != nullptr, "process-loopback integration test opens its target");
    if (process != nullptr) {
        std::atomic_bool alreadyStopped = true;
        Microsoft::WRL::ComPtr<IAudioClient> cancelledClient;
        const HRESULT cancellationResult = ActivateProcessLoopbackClient(
            GetCurrentProcessId(), alreadyStopped, process, &cancelledClient);
        Check(cancellationResult == HRESULT_FROM_WIN32(ERROR_CANCELLED) && cancelledClient == nullptr,
              "process-loopback activation honors an already-requested stop");

        std::atomic_bool stopRequested = false;
        Microsoft::WRL::ComPtr<IAudioClient> audioClient;
        const HRESULT activationResult = ActivateProcessLoopbackClient(
            GetCurrentProcessId(), stopRequested, process, &audioClient);
        Check(SUCCEEDED(activationResult) && audioClient != nullptr,
              "live process-loopback activation succeeds on this Windows build");
        if (SUCCEEDED(activationResult) && audioClient != nullptr) {
            WAVEFORMATEX format = MakeProcessLoopbackCaptureFormat();
            const HRESULT initializeResult = audioClient->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                    AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                0, 0, &format, nullptr);
            Check(SUCCEEDED(initializeResult), "live process-loopback client accepts the fixed format");
        }
        audioClient.Reset();
        CloseHandle(process);
    }
    CoUninitialize();
}

void TestLiveDeviceEnumeration() {
    AudioDeviceInventory inventory;
    std::wstring error;
    Check(EnumerateAudioDevices(inventory, error), "live Windows endpoint enumeration");
    std::wcout << L"Active endpoints: " << inventory.playback.size() << L" playback, "
               << inventory.recording.size() << L" recording.\n";
    for (const auto& device : inventory.playback) {
        std::wcout << L"  Playback: " << device.name
                   << (device.isDefault ? L" [default]" : L"")
                   << (device.isGenericCable ? L" [cable]" : (device.isLikelyVirtual ? L" [virtual/internal]" : L"")) << L"\n";
    }
    for (const auto& device : inventory.recording) {
        std::wcout << L"  Recording: " << device.name
                   << (device.isDefault ? L" [default]" : L"")
                   << (device.isGenericCable ? L" [cable]" : (device.isLikelyVirtual ? L" [virtual/internal]" : L"")) << L"\n";
    }
}

void TestRouterFailureLifecycle() {
    RouteConfig config;
    Check(CurrentProcessIdentity(config), "router lifecycle test reads current process identity");
    config.destinationId = L"{ChromeMic-invalid-destination}";
    config.destinationName = L"Invalid destination";

    {
        AudioRouter validationRouter;
        RouteConfig missingMicrophone = config;
        missingMicrophone.includeMicrophone = true;
        std::wstring validationError;
        Check(!validationRouter.Start(missingMicrophone, validationError) &&
                  validationError.find(L"microphone") != std::wstring::npos,
              "router rejects enabled microphone without an exact endpoint");

        RouteConfig monitorWithoutMicrophone = config;
        monitorWithoutMicrophone.enableMonitor = true;
        monitorWithoutMicrophone.monitorId = L"physical-headphones";
        Check(!validationRouter.Start(monitorWithoutMicrophone, validationError),
              "router rejects monitor without microphone capture");

        RouteConfig unsafeMonitor = config;
        unsafeMonitor.includeMicrophone = true;
        unsafeMonitor.microphoneId = L"physical-microphone";
        unsafeMonitor.enableMonitor = true;
        unsafeMonitor.monitorId = unsafeMonitor.destinationId;
        Check(!validationRouter.Start(unsafeMonitor, validationError) &&
                  validationError.find(L"cannot") != std::wstring::npos,
              "router rejects monitoring into the game cable");
    }

    AudioRouter router;
    RouteConfig staleIdentity = config;
    ++staleIdentity.sourceProcessCreationTime;
    std::wstring staleError;
    Check(router.Start(staleIdentity, staleError), "stale-identity worker starts asynchronously");
    for (int poll = 0; poll < 100 && router.Snapshot().state == RouterState::Starting; ++poll) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const RouterSnapshot staleSnapshot = router.Snapshot();
    Check(staleSnapshot.state == RouterState::Error &&
              staleSnapshot.message.find(L"changed or restarted") != std::wstring::npos,
          "stale process identity fails before opening the destination");

    std::wstring retryError;
    Check(router.Start(config, retryError),
          "terminal error worker is reaped so the route can retry without an app restart");
    for (int poll = 0; poll < 100 && router.Snapshot().state == RouterState::Starting; ++poll) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    Check(router.Snapshot().state == RouterState::Error,
          "retry after a terminal route error runs the corrected generation");
    router.Stop();

    for (int iteration = 0; iteration < 12; ++iteration) {
        std::wstring error;
        Check(router.Start(config, error), "invalid-device worker should start asynchronously");
        for (int poll = 0; poll < 100 && router.Snapshot().state == RouterState::Starting; ++poll) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        Check(router.Snapshot().state == RouterState::Error, "invalid endpoint should fail visibly");
        router.Stop();
        Check(router.Snapshot().state == RouterState::Stopped, "stop after route failure");
    }

    std::wstring error;
    Check(router.Start(config, error), "race test worker starts");
    bool concurrentStartResult = false;
    std::thread stopper([&router] { router.Stop(); });
    std::thread starter([&] {
        std::wstring startError;
        concurrentStartResult = router.Start(config, startError);
        if (concurrentStartResult) {
            router.Stop();
        }
    });
    stopper.join();
    starter.join();
    router.Stop();
    Check(router.Snapshot().state == RouterState::Stopped, "concurrent stop/start leaves one stopped generation");
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--live-update-check") {
        const UpdateCheckResult update = CheckForChromeMicUpdates();
        std::wcout << L"Live update status: " << static_cast<int>(update.status)
                   << L" — " << update.message << L"\n";
        return update.status == UpdateCheckStatus::UpToDate ? 0 : 2;
    }

    TestRingWrapAndOverflow();
    TestRingOversizedPush();
    TestLimiterAndMalformedFloat();
    TestMuteAndGainClamp();
    TestPcm16Mixing();
    TestUpdateManifestAndVersions();
    voice_effects_tests::Run(Check);
    TestGainSmoothing();
    TestFrameCountConversion();
    TestClockDriftController();
    TestPcm16();
    TestAdditionalPcmFormats();
    TestDeviceClassification();
    TestApplicationSelectionHelpers();
    TestProcessLoopbackParameters();
    TestLiveApplicationEnumeration();
    TestLiveProcessLoopbackActivation();
    TestLiveDeviceEnumeration();
    TestRouterFailureLifecycle();

    if (failures == 0) {
        std::cout << "All ChromeMic tests passed.\n";
        return 0;
    }
    std::cerr << failures << " test(s) failed.\n";
    return 1;
}
