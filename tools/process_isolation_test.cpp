#include "audio_devices.h"
#include "audio_router.h"

#include <Windows.h>
#include <Audioclient.h>
#include <Mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <iomanip>
#include <iostream>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr int kSkipExitCode = 77;
constexpr UINT32 kSampleRate = 48000;
constexpr WORD kChannels = 2;
constexpr WORD kBitsPerSample = 16;
constexpr REFERENCE_TIME kCaptureBufferDuration = 500000; // 50 ms.
constexpr double kTargetFrequencyHz = 997.0;
constexpr double kUnrelatedFrequencyHz = 1601.0;
constexpr double kToneAmplitude = 0.015; // About -36.5 dBFS per tone.
constexpr uint32_t kToneDurationMilliseconds = 5000;
constexpr uint32_t kBaselineMilliseconds = 400;
constexpr uint32_t kToneCaptureMilliseconds = 4200;
constexpr uint32_t kAnalysisWarmupMilliseconds = 700;
constexpr uint32_t kAnalysisMilliseconds = 3000;
constexpr double kMinimumTargetAmplitude = 0.001;
constexpr double kMaximumUnrelatedRatioDb = -20.0;
constexpr double kMaximumIdleRms = 0.0025;
constexpr double kPi = 3.1415926535897932384626433832795;

class ScopedComApartment {
public:
    ScopedComApartment() noexcept : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ScopedComApartment() {
        if (SUCCEEDED(result_)) {
            CoUninitialize();
        }
    }
    [[nodiscard]] HRESULT Result() const noexcept { return result_; }

private:
    HRESULT result_ = E_FAIL;
};

struct Options {
    std::wstring cableRenderId;
    std::wstring cableCaptureId;
    std::wstring monitorRenderId;
    std::wstring toneRendererPath;
    bool showHelp = false;
};

bool EqualsIgnoringCase(std::wstring_view left, std::wstring_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t index = 0; index < left.size(); ++index) {
        if (std::towlower(static_cast<wint_t>(left[index])) !=
            std::towlower(static_cast<wint_t>(right[index]))) {
            return false;
        }
    }
    return true;
}

bool ContainsIgnoringCase(std::wstring_view text, std::wstring_view needle) {
    if (needle.empty()) {
        return true;
    }
    return std::search(text.begin(), text.end(), needle.begin(), needle.end(),
        [](wchar_t left, wchar_t right) {
            return std::towlower(static_cast<wint_t>(left)) ==
                   std::towlower(static_cast<wint_t>(right));
        }) != text.end();
}

bool ParseOptions(int argc, wchar_t** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        if (argument == L"--help" || argument == L"-h") {
            options.showHelp = true;
            continue;
        }
        if (index + 1 >= argc) {
            return false;
        }
        const wchar_t* value = argv[++index];
        if (argument == L"--cable-render-id") {
            options.cableRenderId = value;
        } else if (argument == L"--cable-capture-id") {
            options.cableCaptureId = value;
        } else if (argument == L"--monitor-render-id") {
            options.monitorRenderId = value;
        } else if (argument == L"--tone-renderer") {
            options.toneRendererPath = value;
        } else {
            return false;
        }
    }
    return true;
}

void PrintUsage() {
    std::wcout
        << L"Usage: chromemic_process_isolation_test.exe [options]\n"
        << L"  --cable-render-id <id>   Explicit generic cable playback endpoint\n"
        << L"  --cable-capture-id <id>  Explicit matching generic cable recording endpoint\n"
        << L"  --monitor-render-id <id> Non-virtual endpoint used for the two faint tones\n"
        << L"  --tone-renderer <path>   Path to chromemic_tone_renderer.exe\n"
        << L"\nBoth cable IDs must be supplied together. Without them, the test runs only\n"
        << L"when one unambiguous generic cable pair can be identified. Exit 77 means skipped.\n";
}

const AudioDeviceInfo* FindEndpoint(const std::vector<AudioDeviceInfo>& devices,
                                    std::wstring_view id) noexcept {
    for (const auto& device : devices) {
        if (EqualsIgnoringCase(device.id, id)) {
            return &device;
        }
    }
    return nullptr;
}

std::vector<const AudioDeviceInfo*> GenericCandidates(
    const std::vector<AudioDeviceInfo>& devices) {
    std::vector<const AudioDeviceInfo*> candidates;
    for (const auto& device : devices) {
        if (device.isGenericCable && !device.isVoicemod) {
            candidates.push_back(&device);
        }
    }
    return candidates;
}

void PrintCandidates(const AudioDeviceInventory& inventory) {
    std::wcout << L"Generic cable playback candidates:\n";
    for (const auto& device : inventory.playback) {
        if (device.isGenericCable && !device.isVoicemod) {
            std::wcout << L"  " << device.name << L"\n    " << device.id << L"\n";
        }
    }
    std::wcout << L"Generic cable recording candidates:\n";
    for (const auto& device : inventory.recording) {
        if (device.isGenericCable && !device.isVoicemod) {
            std::wcout << L"  " << device.name << L"\n    " << device.id << L"\n";
        }
    }
}

bool ResolveCablePair(const AudioDeviceInventory& inventory, const Options& options,
                      const AudioDeviceInfo*& cableRender,
                      const AudioDeviceInfo*& cableCapture,
                      std::wstring& reason, bool& configurationError) {
    cableRender = nullptr;
    cableCapture = nullptr;
    configurationError = false;

    const bool explicitRender = !options.cableRenderId.empty();
    const bool explicitCapture = !options.cableCaptureId.empty();
    if (explicitRender != explicitCapture) {
        configurationError = true;
        reason = L"--cable-render-id and --cable-capture-id must be supplied together";
        return false;
    }
    if (explicitRender) {
        cableRender = FindEndpoint(inventory.playback, options.cableRenderId);
        cableCapture = FindEndpoint(inventory.recording, options.cableCaptureId);
        if (cableRender == nullptr || cableCapture == nullptr) {
            configurationError = true;
            reason = L"an explicit cable endpoint ID is not active or has the wrong data flow";
            return false;
        }
        if (!cableRender->isGenericCable || !cableCapture->isGenericCable ||
            cableRender->isVoicemod || cableCapture->isVoicemod) {
            configurationError = true;
            reason = L"explicit endpoints must be a recognized generic cable pair, not a mixer bridge";
            return false;
        }
        return true;
    }

    const auto renderCandidates = GenericCandidates(inventory.playback);
    const auto captureCandidates = GenericCandidates(inventory.recording);
    std::vector<const AudioDeviceInfo*> canonicalRenders;
    std::vector<const AudioDeviceInfo*> canonicalCaptures;
    for (const auto* candidate : renderCandidates) {
        if (ContainsIgnoringCase(candidate->name, L"CABLE Input")) {
            canonicalRenders.push_back(candidate);
        }
    }
    for (const auto* candidate : captureCandidates) {
        if (ContainsIgnoringCase(candidate->name, L"CABLE Output")) {
            canonicalCaptures.push_back(candidate);
        }
    }

    if (canonicalRenders.size() == 1 && canonicalCaptures.size() == 1) {
        cableRender = canonicalRenders.front();
        cableCapture = canonicalCaptures.front();
        return true;
    }
    if (renderCandidates.size() == 1 && captureCandidates.size() == 1) {
        cableRender = renderCandidates.front();
        cableCapture = captureCandidates.front();
        return true;
    }

    reason = renderCandidates.empty() || captureCandidates.empty()
        ? L"no active generic render-to-recording cable pair was found"
        : L"multiple generic cable endpoints are active; specify the matching pair explicitly";
    return false;
}

bool ResolveMonitorEndpoint(const AudioDeviceInventory& inventory, const Options& options,
                            const AudioDeviceInfo* cableRender,
                            const AudioDeviceInfo*& monitor, std::wstring& reason) {
    monitor = nullptr;
    if (!options.monitorRenderId.empty()) {
        monitor = FindEndpoint(inventory.playback, options.monitorRenderId);
        if (monitor == nullptr) {
            reason = L"the explicit monitor playback endpoint is not active";
            return false;
        }
        if (EqualsIgnoringCase(monitor->id, cableRender->id) || monitor->isLikelyVirtual ||
            monitor->isGenericCable || monitor->isVoicemod) {
            reason = L"the monitor endpoint must be a non-virtual playback device distinct from the cable";
            return false;
        }
        return true;
    }

    for (const auto& device : inventory.playback) {
        if (device.isDefault && !device.isLikelyVirtual && !device.isGenericCable &&
            !device.isVoicemod && !EqualsIgnoringCase(device.id, cableRender->id)) {
            monitor = &device;
            return true;
        }
    }
    for (const auto& device : inventory.playback) {
        if (!device.isLikelyVirtual && !device.isGenericCable && !device.isVoicemod &&
            !EqualsIgnoringCase(device.id, cableRender->id)) {
            monitor = &device;
            return true;
        }
    }
    reason = L"no non-virtual playback endpoint is active for the isolated source tones";
    return false;
}

std::wstring SiblingToneRendererPath() {
    std::wstring modulePath(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, modulePath.data(),
                                            static_cast<DWORD>(modulePath.size()));
    if (length == 0 || static_cast<size_t>(length) >= modulePath.size()) {
        return {};
    }
    modulePath.resize(length);
    const size_t separator = modulePath.find_last_of(L"\\/");
    if (separator == std::wstring::npos) {
        return L"chromemic_tone_renderer.exe";
    }
    modulePath.resize(separator + 1);
    modulePath += L"chromemic_tone_renderer.exe";
    return modulePath;
}

std::wstring QuoteCommandLineArgument(std::wstring_view argument) {
    std::wstring quoted;
    quoted.push_back(L'"');
    size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(character);
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

class ChildProcess {
public:
    ChildProcess() = default;
    ~ChildProcess() { Cleanup(); }
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    bool CreateSuspended(const std::wstring& executable, const std::wstring& endpointId,
                         double frequencyHz, std::wstring& error) {
        std::wstring command = QuoteCommandLineArgument(executable) +
            L" --endpoint-id " + QuoteCommandLineArgument(endpointId) +
            L" --frequency " + std::to_wstring(frequencyHz) +
            L" --amplitude " + std::to_wstring(kToneAmplitude) +
            L" --duration-ms " + std::to_wstring(kToneDurationMilliseconds);

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                            CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr,
                            &startup, &processInformation_)) {
            error = L"CreateProcess failed: " + FriendlyHResult(HRESULT_FROM_WIN32(GetLastError()));
            return false;
        }
        return true;
    }

    bool Resume(std::wstring& error) {
        if (processInformation_.hThread == nullptr ||
            ResumeThread(processInformation_.hThread) == static_cast<DWORD>(-1)) {
            error = L"ResumeThread failed: " + FriendlyHResult(HRESULT_FROM_WIN32(GetLastError()));
            return false;
        }
        resumed_ = true;
        return true;
    }

    bool WaitForSuccessfulExit(DWORD timeoutMilliseconds, std::wstring& error) {
        if (processInformation_.hProcess == nullptr) {
            error = L"tone process was not created";
            return false;
        }
        const DWORD waitResult = WaitForSingleObject(processInformation_.hProcess, timeoutMilliseconds);
        if (waitResult != WAIT_OBJECT_0) {
            error = waitResult == WAIT_TIMEOUT
                ? L"tone process exceeded its bounded run time"
                : L"waiting for tone process failed";
            return false;
        }
        DWORD exitCode = std::numeric_limits<DWORD>::max();
        if (!GetExitCodeProcess(processInformation_.hProcess, &exitCode)) {
            error = L"GetExitCodeProcess failed";
            return false;
        }
        if (exitCode != 0) {
            error = L"tone process exited with code " + std::to_wstring(exitCode);
            return false;
        }
        return true;
    }

    [[nodiscard]] DWORD Id() const noexcept { return processInformation_.dwProcessId; }
    [[nodiscard]] HANDLE ProcessHandle() const noexcept { return processInformation_.hProcess; }

private:
    void Cleanup() noexcept {
        if (processInformation_.hProcess != nullptr) {
            if (WaitForSingleObject(processInformation_.hProcess, 0) == WAIT_TIMEOUT) {
                // This handle identifies only the child created by this test.
                TerminateProcess(processInformation_.hProcess, ERROR_CANCELLED);
                WaitForSingleObject(processInformation_.hProcess, 2000);
            }
        }
        if (processInformation_.hThread != nullptr) {
            CloseHandle(processInformation_.hThread);
        }
        if (processInformation_.hProcess != nullptr) {
            CloseHandle(processInformation_.hProcess);
        }
        processInformation_ = {};
        resumed_ = false;
    }

    PROCESS_INFORMATION processInformation_{};
    bool resumed_ = false;
};

bool QueryProcessIdentity(HANDLE process, uint64_t& creationTime,
                          std::wstring& executablePath, std::wstring& error) {
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        error = L"GetProcessTimes failed: " + FriendlyHResult(HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }
    ULARGE_INTEGER value{};
    value.LowPart = created.dwLowDateTime;
    value.HighPart = created.dwHighDateTime;
    creationTime = value.QuadPart;

    executablePath.assign(32768, L'\0');
    DWORD length = static_cast<DWORD>(executablePath.size());
    if (!QueryFullProcessImageNameW(process, 0, executablePath.data(), &length) || length == 0) {
        error = L"QueryFullProcessImageName failed: " +
                FriendlyHResult(HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }
    executablePath.resize(length);
    return creationTime != 0;
}

WAVEFORMATEX CaptureFormat() noexcept {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = kChannels;
    format.nSamplesPerSec = kSampleRate;
    format.wBitsPerSample = kBitsPerSample;
    format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    return format;
}

class CableCapture {
public:
    ~CableCapture() { Stop(); }
    CableCapture(const CableCapture&) = delete;
    CableCapture& operator=(const CableCapture&) = delete;
    CableCapture() = default;

    HRESULT Open(const std::wstring& endpointId) {
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                          IID_PPV_ARGS(&enumerator));
        if (FAILED(result)) {
            return result;
        }
        ComPtr<IMMDevice> device;
        result = enumerator->GetDevice(endpointId.c_str(), &device);
        if (FAILED(result)) {
            return result;
        }
        result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(audioClient_.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }

        WAVEFORMATEX format = CaptureFormat();
        constexpr DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        result = audioClient_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
                                          kCaptureBufferDuration, 0, &format, nullptr);
        if (FAILED(result)) {
            return result;
        }
        return audioClient_->GetService(IID_PPV_ARGS(&captureClient_));
    }

    HRESULT Start() {
        if (audioClient_ == nullptr || captureClient_ == nullptr) {
            return E_UNEXPECTED;
        }
        const HRESULT result = audioClient_->Start();
        started_ = SUCCEEDED(result);
        return result;
    }

    void Stop() noexcept {
        if (started_ && audioClient_ != nullptr) {
            audioClient_->Stop();
        }
        started_ = false;
    }

    HRESULT PumpFor(std::chrono::milliseconds duration, std::vector<float>& monoSamples,
                    size_t maximumSamples, AudioRouter& router,
                    uint64_t& discontinuities, std::wstring& detail) {
        const auto deadline = std::chrono::steady_clock::now() + duration;
        while (std::chrono::steady_clock::now() < deadline) {
            const RouterSnapshot snapshot = router.Snapshot();
            if (snapshot.state != RouterState::Running) {
                detail = L"router left Running state: " + snapshot.message;
                return HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED);
            }

            UINT32 packetFrames = 0;
            HRESULT result = captureClient_->GetNextPacketSize(&packetFrames);
            if (FAILED(result)) {
                return result;
            }
            while (packetFrames != 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                result = captureClient_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (FAILED(result)) {
                    return result;
                }

                if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0) {
                    ++discontinuities;
                }
                const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr;
                const auto* pcm = reinterpret_cast<const int16_t*>(data);
                for (UINT32 frame = 0; frame < frames && monoSamples.size() < maximumSamples; ++frame) {
                    float sample = 0.0F;
                    if (!silent) {
                        const int32_t left = pcm[static_cast<size_t>(frame) * kChannels];
                        const int32_t right = pcm[static_cast<size_t>(frame) * kChannels + 1];
                        sample = static_cast<float>(left + right) / 65536.0F;
                    }
                    monoSamples.push_back(sample);
                }

                const HRESULT releaseResult = captureClient_->ReleaseBuffer(frames);
                if (FAILED(releaseResult)) {
                    return releaseResult;
                }
                result = captureClient_->GetNextPacketSize(&packetFrames);
                if (FAILED(result)) {
                    return result;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return S_OK;
    }

private:
    ComPtr<IAudioClient> audioClient_;
    ComPtr<IAudioCaptureClient> captureClient_;
    bool started_ = false;
};

bool WaitForRouter(AudioRouter& router, HANDLE targetProcess, std::wstring& detail) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    while (std::chrono::steady_clock::now() < deadline) {
        if (WaitForSingleObject(targetProcess, 0) == WAIT_OBJECT_0) {
            detail = L"target tone process exited during route setup";
            return false;
        }
        const RouterSnapshot snapshot = router.Snapshot();
        if (snapshot.state == RouterState::Running) {
            return true;
        }
        if (snapshot.state == RouterState::Error) {
            detail = snapshot.message;
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    detail = L"router did not reach Running within 12 seconds";
    return false;
}

double RootMeanSquare(const std::vector<float>& samples, size_t begin, size_t end) noexcept {
    if (begin >= end || end > samples.size()) {
        return 0.0;
    }
    double sum = 0.0;
    for (size_t index = begin; index < end; ++index) {
        const double value = samples[index];
        sum += value * value;
    }
    return std::sqrt(sum / static_cast<double>(end - begin));
}

double SpectralAmplitude(const std::vector<float>& samples, size_t begin, size_t end,
                         double frequencyHz) noexcept {
    if (end <= begin + 2 || end > samples.size()) {
        return 0.0;
    }
    const size_t count = end - begin;
    double mean = 0.0;
    for (size_t index = begin; index < end; ++index) {
        mean += samples[index];
    }
    mean /= static_cast<double>(count);

    double real = 0.0;
    double imaginary = 0.0;
    double windowSum = 0.0;
    const double angularStep = 2.0 * kPi * frequencyHz / static_cast<double>(kSampleRate);
    for (size_t offset = 0; offset < count; ++offset) {
        const double window = 0.5 - 0.5 *
            std::cos(2.0 * kPi * static_cast<double>(offset) / static_cast<double>(count - 1));
        const double value = (static_cast<double>(samples[begin + offset]) - mean) * window;
        const double phase = angularStep * static_cast<double>(offset);
        real += value * std::cos(phase);
        imaginary -= value * std::sin(phase);
        windowSum += window;
    }
    return windowSum > 0.0 ? 2.0 * std::hypot(real, imaginary) / windowSum : 0.0;
}

double Decibels(double amplitude) noexcept {
    return 20.0 * std::log10(std::max(amplitude, 1.0e-12));
}

int RunTest(const Options& options) {
    ScopedComApartment apartment;
    if (FAILED(apartment.Result())) {
        std::wcerr << L"COM initialization failed: " << FriendlyHResult(apartment.Result()) << L"\n";
        return 2;
    }

    AudioDeviceInventory inventory;
    std::wstring error;
    if (!EnumerateAudioDevices(inventory, error)) {
        std::wcerr << L"Audio endpoint enumeration failed: " << error << L"\n";
        return 2;
    }

    const AudioDeviceInfo* cableRender = nullptr;
    const AudioDeviceInfo* cableCapture = nullptr;
    bool configurationError = false;
    if (!ResolveCablePair(inventory, options, cableRender, cableCapture,
                          error, configurationError)) {
        std::wcerr << (configurationError ? L"Configuration error: " : L"Isolation test skipped: ")
                   << error << L"\n";
        PrintCandidates(inventory);
        return configurationError ? 64 : kSkipExitCode;
    }

    const AudioDeviceInfo* monitor = nullptr;
    if (!ResolveMonitorEndpoint(inventory, options, cableRender, monitor, error)) {
        std::wcerr << L"Isolation test skipped: " << error << L"\n";
        return kSkipExitCode;
    }

    std::wstring toneRenderer = options.toneRendererPath.empty()
        ? SiblingToneRendererPath() : options.toneRendererPath;
    if (toneRenderer.empty() || GetFileAttributesW(toneRenderer.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wcerr << L"Tone renderer was not found: " << toneRenderer << L"\n";
        return 64;
    }

    std::wcout << L"Cable playback:  " << cableRender->name << L"\n"
               << L"Cable recording: " << cableCapture->name << L"\n"
               << L"Tone playback:   " << monitor->name << L"\n"
               << L"Emitting two faint tones for five seconds; no defaults or volume settings are changed.\n";

    ChildProcess targetTone;
    ChildProcess unrelatedTone;
    AudioRouter router;
    CableCapture capture;

    HRESULT result = capture.Open(cableCapture->id);
    if (FAILED(result)) {
        std::wcerr << L"Cable recording endpoint could not open: "
                   << FriendlyHResult(result) << L"\n";
        return 2;
    }
    if (!targetTone.CreateSuspended(toneRenderer, monitor->id, kTargetFrequencyHz, error) ||
        !unrelatedTone.CreateSuspended(toneRenderer, monitor->id, kUnrelatedFrequencyHz, error)) {
        std::wcerr << L"Tone process creation failed: " << error << L"\n";
        return 2;
    }

    uint64_t targetCreationTime = 0;
    std::wstring targetExecutablePath;
    if (!QueryProcessIdentity(targetTone.ProcessHandle(), targetCreationTime,
                              targetExecutablePath, error)) {
        std::wcerr << L"Target process identity failed: " << error << L"\n";
        return 2;
    }

    RouteConfig route;
    route.sourceProcessId = targetTone.Id();
    route.sourceProcessCreationTime = targetCreationTime;
    route.sourceExecutablePath = targetExecutablePath;
    route.sourceName = L"Isolation target tone [PID " + std::to_wstring(targetTone.Id()) + L"]";
    route.destinationId = cableRender->id;
    route.destinationName = cableRender->name;
    if (!router.Start(route, error)) {
        std::wcerr << L"AudioRouter start failed: " << error << L"\n";
        return 2;
    }
    if (!WaitForRouter(router, targetTone.ProcessHandle(), error)) {
        std::wcerr << L"AudioRouter setup failed: " << error << L"\n";
        return 2;
    }

    result = capture.Start();
    if (FAILED(result)) {
        std::wcerr << L"Cable recording could not start: " << FriendlyHResult(result) << L"\n";
        return 2;
    }

    const size_t maximumSamples = static_cast<size_t>(kSampleRate) * 7;
    std::vector<float> samples;
    samples.reserve(maximumSamples);
    uint64_t discontinuities = 0;
    result = capture.PumpFor(std::chrono::milliseconds(kBaselineMilliseconds), samples,
                             maximumSamples, router, discontinuities, error);
    if (FAILED(result)) {
        std::wcerr << L"Baseline cable capture failed: "
                   << (error.empty() ? FriendlyHResult(result) : error) << L"\n";
        return 2;
    }
    const size_t toneStartFrame = samples.size();
    const size_t idleSkip = std::min<size_t>(toneStartFrame, kSampleRate / 20);
    const double idleRms = RootMeanSquare(samples, idleSkip, toneStartFrame);
    if (toneStartFrame < kSampleRate / 5 || idleRms > kMaximumIdleRms) {
        std::wcerr << L"Cable is not idle enough for a deterministic test (baseline RMS "
                   << std::fixed << std::setprecision(2) << Decibels(idleRms) << L" dBFS).\n";
        return 2;
    }

    if (!targetTone.Resume(error) || !unrelatedTone.Resume(error)) {
        std::wcerr << L"Tone process resume failed: " << error << L"\n";
        return 2;
    }
    result = capture.PumpFor(std::chrono::milliseconds(kToneCaptureMilliseconds), samples,
                             maximumSamples, router, discontinuities, error);
    capture.Stop();
    router.Stop();
    if (FAILED(result)) {
        std::wcerr << L"Tone cable capture failed: "
                   << (error.empty() ? FriendlyHResult(result) : error) << L"\n";
        return 2;
    }

    std::wstring childError;
    const bool targetExitedCleanly = targetTone.WaitForSuccessfulExit(3000, childError);
    if (!targetExitedCleanly) {
        std::wcerr << L"Target tone failed: " << childError << L"\n";
        return 2;
    }
    const bool unrelatedExitedCleanly = unrelatedTone.WaitForSuccessfulExit(3000, childError);
    if (!unrelatedExitedCleanly) {
        std::wcerr << L"Unrelated tone failed: " << childError << L"\n";
        return 2;
    }

    const size_t analysisBegin = toneStartFrame +
        static_cast<size_t>(kSampleRate) * kAnalysisWarmupMilliseconds / 1000;
    const size_t analysisEnd = analysisBegin +
        static_cast<size_t>(kSampleRate) * kAnalysisMilliseconds / 1000;
    if (analysisEnd > samples.size()) {
        std::wcerr << L"Too few cable samples were captured for the fixed analysis window ("
                   << samples.size() << L" frames).\n";
        return 2;
    }

    const double targetAmplitude = SpectralAmplitude(
        samples, analysisBegin, analysisEnd, kTargetFrequencyHz);
    const double unrelatedAmplitude = SpectralAmplitude(
        samples, analysisBegin, analysisEnd, kUnrelatedFrequencyHz);
    const double exclusionDb = Decibels(unrelatedAmplitude) - Decibels(targetAmplitude);

    std::wcout << std::fixed << std::setprecision(2)
               << L"Target " << kTargetFrequencyHz << L" Hz: "
               << Decibels(targetAmplitude) << L" dBFS\n"
               << L"Unrelated " << kUnrelatedFrequencyHz << L" Hz: "
               << Decibels(unrelatedAmplitude) << L" dBFS\n"
               << L"Unrelated/target ratio: " << exclusionDb << L" dB"
               << L"; capture discontinuities: " << discontinuities << L"\n";

    const bool targetPresent = std::isfinite(targetAmplitude) &&
                               targetAmplitude >= kMinimumTargetAmplitude;
    const bool unrelatedExcluded = std::isfinite(exclusionDb) &&
                                   exclusionDb <= kMaximumUnrelatedRatioDb;
    if (!targetPresent || !unrelatedExcluded) {
        std::wcerr << L"FAIL: "
                   << (!targetPresent ? L"target tone was not present strongly enough; " : L"")
                   << (!unrelatedExcluded ? L"unrelated tone was not excluded by at least 20 dB" : L"")
                   << L".\n";
        return 1;
    }

    std::wcout << L"PASS: target-process audio reached the cable and the sibling process tone was strongly excluded.\n";
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetThreadDescription(GetCurrentThread(), L"ChromeMic process-isolation integration test");
    Options options;
    if (!ParseOptions(argc, argv, options)) {
        PrintUsage();
        return 64;
    }
    if (options.showHelp) {
        PrintUsage();
        return 0;
    }

    try {
        return RunTest(options);
    } catch (const std::bad_alloc&) {
        std::wcerr << L"Integration test stopped because memory allocation failed.\n";
        return 2;
    } catch (const std::exception& exception) {
        std::cerr << "Integration test stopped after an exception: " << exception.what() << "\n";
        return 2;
    } catch (...) {
        std::wcerr << L"Integration test stopped after an unknown exception.\n";
        return 2;
    }
}
