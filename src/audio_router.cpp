#include "audio_router.h"

#include "audio_devices.h"
#include "dsp.h"
#include "process_loopback.h"

#include <Windows.h>
#include <Audioclient.h>
#include <Audiopolicy.h>
#include <Avrt.h>
#include <Mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <exception>
#include <new>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr REFERENCE_TIME kBufferDuration = 500000; // 50 ms in 100 ns units.
constexpr uint32_t kQueueDurationMs = 250;
constexpr uint32_t kTargetQueueDurationMs = 40;

VoiceEffectMode NormalizeVoiceEffectMode(VoiceEffectMode mode) noexcept {
    switch (mode) {
    case VoiceEffectMode::Natural:
    case VoiceEffectMode::ClearMic:
    case VoiceEffectMode::Broadcast:
    case VoiceEffectMode::Radio:
    case VoiceEffectMode::Robot:
    case VoiceEffectMode::DeepTone:
        return mode;
    default:
        return VoiceEffectMode::Natural;
    }
}

std::wstring RouteFailure(const wchar_t* operation, HRESULT result) {
    return std::wstring(operation) + L": " + FriendlyHResult(result);
}

class ScopedMmcss {
public:
    ScopedMmcss() {
        DWORD taskIndex = 0;
        handle_ = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    }
    ~ScopedMmcss() {
        if (handle_ != nullptr) {
            AvRevertMmThreadCharacteristics(handle_);
        }
    }

private:
    HANDLE handle_ = nullptr;
};

class ScopedComApartment {
public:
    explicit ScopedComApartment(DWORD model) noexcept : result_(CoInitializeEx(nullptr, model)) {}
    ~ScopedComApartment() {
        if (SUCCEEDED(result_)) {
            CoUninitialize();
        }
    }
    [[nodiscard]] HRESULT Result() const noexcept { return result_; }

private:
    HRESULT result_ = E_FAIL;
};

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle = nullptr) noexcept : handle_(handle) {}
    ~ScopedHandle() {
        if (handle_ != nullptr) {
            CloseHandle(handle_);
        }
    }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    [[nodiscard]] HANDLE Get() const noexcept { return handle_; }

private:
    HANDLE handle_ = nullptr;
};

HRESULT ValidateProcessIdentity(HANDLE process, const RouteConfig& config) noexcept {
    if (process == nullptr || config.sourceProcessCreationTime == 0 ||
        config.sourceExecutablePath.empty()) {
        return E_INVALIDARG;
    }

    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    ULARGE_INTEGER creationValue{};
    creationValue.LowPart = created.dwLowDateTime;
    creationValue.HighPart = created.dwHighDateTime;
    if (creationValue.QuadPart != config.sourceProcessCreationTime) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }

    std::array<wchar_t, 32768> currentPath{};
    DWORD pathCharacters = static_cast<DWORD>(currentPath.size());
    if (!QueryFullProcessImageNameW(process, 0, currentPath.data(), &pathCharacters) ||
        pathCharacters == 0 || static_cast<size_t>(pathCharacters) > currentPath.size()) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    if (config.sourceExecutablePath.size() > static_cast<size_t>(INT_MAX)) {
        return E_INVALIDARG;
    }
    return CompareStringOrdinal(currentPath.data(), static_cast<int>(pathCharacters),
                                config.sourceExecutablePath.c_str(),
                                static_cast<int>(config.sourceExecutablePath.size()), TRUE) == CSTR_EQUAL
        ? S_OK
        : HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
}

HRESULT OpenSharedRenderer(IMMDevice* device,
                           WAVEFORMATEX& format,
                           ComPtr<IAudioClient>& audioClient,
                           ComPtr<IAudioRenderClient>& renderClient,
                           UINT32& bufferFrames) noexcept {
    if (device == nullptr) {
        return E_POINTER;
    }
    HRESULT result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                      reinterpret_cast<void**>(audioClient.GetAddressOf()));
    if (FAILED(result)) {
        return result;
    }
    constexpr DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
                            AUDCLNT_STREAMFLAGS_NOPERSIST;
    result = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
                                     kBufferDuration, 0, &format, nullptr);
    if (FAILED(result)) {
        return result;
    }
    result = audioClient->GetService(IID_PPV_ARGS(&renderClient));
    if (FAILED(result)) {
        return result;
    }

    ComPtr<ISimpleAudioVolume> sessionVolume;
    if (SUCCEEDED(audioClient->GetService(IID_PPV_ARGS(&sessionVolume)))) {
        sessionVolume->SetMute(FALSE, nullptr);
        sessionVolume->SetMasterVolume(1.0F, nullptr);
    }
    ComPtr<IAudioSessionControl> sessionControl;
    ComPtr<IAudioSessionControl2> sessionControl2;
    if (SUCCEEDED(audioClient->GetService(IID_PPV_ARGS(&sessionControl))) &&
        SUCCEEDED(sessionControl.As(&sessionControl2))) {
        sessionControl2->SetDuckingPreference(TRUE);
    }

    result = audioClient->GetBufferSize(&bufferFrames);
    if (FAILED(result) || bufferFrames == 0) {
        return FAILED(result) ? result : E_UNEXPECTED;
    }
    BYTE* initialBuffer = nullptr;
    result = renderClient->GetBuffer(bufferFrames, &initialBuffer);
    if (FAILED(result)) {
        return result;
    }
    return renderClient->ReleaseBuffer(bufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);
}

size_t FillFromRing(AudioFrameRing& ring,
                    DriftCompensator& drift,
                    bool& primed,
                    AudioProcessor& converter,
                    std::vector<std::byte>& rateScratch,
                    std::byte* destination,
                    uint32_t outputFrames,
                    uint16_t blockAlign,
                    uint64_t& underrunFrames) {
    if (destination == nullptr || outputFrames == 0 || blockAlign == 0) {
        return 0;
    }
    std::memset(destination, 0, static_cast<size_t>(outputFrames) * blockAlign);
    if (!primed) {
        return 0;
    }

    size_t copiedFrames = 0;
    const size_t inputFrames = drift.InputFrames(ring.SizeFrames(), outputFrames);
    if (ring.SizeFrames() >= inputFrames && inputFrames != 0) {
        if (inputFrames == outputFrames) {
            copiedFrames = ring.Pop(destination, outputFrames);
        } else {
            rateScratch.resize(inputFrames * blockAlign);
            ring.Pop(rateScratch.data(), inputFrames);
            if (converter.ConvertFrameCount(rateScratch.data(), static_cast<uint32_t>(inputFrames),
                                            destination, outputFrames)) {
                copiedFrames = outputFrames;
            }
        }
    } else {
        copiedFrames = ring.Pop(destination, outputFrames);
        primed = false;
        drift.Reset();
    }

    if (copiedFrames < outputFrames && copiedFrames != 0) {
        underrunFrames += outputFrames - copiedFrames;
    }
    return copiedFrames;
}

} // namespace

AudioRouter::~AudioRouter() {
    Stop();
}

bool AudioRouter::Start(const RouteConfig& config, std::wstring& errorMessage) {
    std::lock_guard lock(lifecycleMutex_);
    if (stopping_) {
        errorMessage = L"The previous route is still stopping. Wait a moment and retry.";
        return false;
    }
    if (worker_.joinable()) {
        RouterState priorState = RouterState::Starting;
        {
            std::lock_guard snapshotLock(snapshotMutex_);
            priorState = snapshot_.state;
        }
        if (priorState == RouterState::Error || priorState == RouterState::Stopped) {
            // A worker that published a terminal state has no remaining route
            // work and never needs lifecycleMutex_ while unwinding. Reap it so
            // the user can correct a selection and retry without restarting.
            worker_.join();
        } else {
            errorMessage = L"Routing is already active. Stop it before starting again.";
            return false;
        }
    }
    if (config.sourceProcessId == 0 || config.destinationId.empty()) {
        errorMessage = L"Choose both an application and a virtual-cable destination.";
        return false;
    }
    if (config.includeMicrophone && config.microphoneId.empty()) {
        errorMessage = L"Choose a microphone or turn off Include microphone.";
        return false;
    }
    if (config.enableMonitor && (!config.includeMicrophone || config.monitorId.empty())) {
        errorMessage = L"Headphone monitoring requires an enabled microphone and a monitor device.";
        return false;
    }
    if (config.enableMonitor && config.monitorId == config.destinationId) {
        errorMessage = L"The headphone monitor cannot be the virtual-cable destination.";
        return false;
    }

    stopRequested_.store(false, std::memory_order_release);
    applicationGainDb_.store(AudioProcessor::ClampGainDb(config.applicationGainDb),
                             std::memory_order_relaxed);
    microphoneGainDb_.store(AudioProcessor::ClampGainDb(config.microphoneGainDb),
                            std::memory_order_relaxed);
    voiceEffectMode_.store(
        static_cast<uint8_t>(NormalizeVoiceEffectMode(config.voiceEffect)),
        std::memory_order_relaxed);
    muted_.store(config.muted, std::memory_order_relaxed);
    {
        std::lock_guard snapshotLock(snapshotMutex_);
        snapshot_ = {};
        snapshot_.state = RouterState::Starting;
        snapshot_.message = config.includeMicrophone
            ? L"Opening application audio, microphone, and outputs..."
            : L"Opening application audio and the virtual cable...";
        snapshot_.sourceName = config.sourceName;
        snapshot_.microphoneName = config.microphoneName;
        snapshot_.destinationName = config.destinationName;
        snapshot_.monitorName = config.monitorName;
        snapshot_.muted = config.muted;
    }

    try {
        worker_ = std::thread(&AudioRouter::Worker, this, config);
    } catch (const std::exception&) {
        errorMessage = L"The audio worker thread could not start.";
        SetSnapshotState(RouterState::Error, errorMessage);
        return false;
    }
    errorMessage.clear();
    return true;
}

void AudioRouter::Stop() {
    std::thread threadToJoin;
    {
        std::unique_lock lock(lifecycleMutex_);
        if (stopping_) {
            lifecycleCondition_.wait(lock, [this] { return !stopping_; });
            return;
        }
        stopping_ = true;
        stopRequested_.store(true, std::memory_order_release);
        if (worker_.joinable()) {
            threadToJoin = std::move(worker_);
        }
    }
    if (threadToJoin.joinable()) {
        threadToJoin.join();
    }
    {
        std::lock_guard lifecycleLock(lifecycleMutex_);
        {
            std::lock_guard snapshotLock(snapshotMutex_);
            snapshot_.state = RouterState::Stopped;
            snapshot_.message = L"Stopped — all capture and monitor devices were released";
            snapshot_.peak = 0.0F;
            snapshot_.applicationPeak = 0.0F;
            snapshot_.microphonePeak = 0.0F;
            snapshot_.limiting = false;
            snapshot_.monitorActive = false;
            snapshot_.queuedMilliseconds = 0;
        }
        stopping_ = false;
    }
    lifecycleCondition_.notify_all();
}

void AudioRouter::SetApplicationGainDb(float gainDb) noexcept {
    applicationGainDb_.store(AudioProcessor::ClampGainDb(gainDb), std::memory_order_relaxed);
}

void AudioRouter::SetMicrophoneGainDb(float gainDb) noexcept {
    microphoneGainDb_.store(AudioProcessor::ClampGainDb(gainDb), std::memory_order_relaxed);
}

void AudioRouter::SetVoiceEffect(VoiceEffectMode mode) noexcept {
    voiceEffectMode_.store(
        static_cast<uint8_t>(NormalizeVoiceEffectMode(mode)),
        std::memory_order_relaxed);
}

void AudioRouter::SetMuted(bool muted) noexcept {
    muted_.store(muted, std::memory_order_relaxed);
}

RouterSnapshot AudioRouter::Snapshot() const {
    std::lock_guard lock(snapshotMutex_);
    return snapshot_;
}

void AudioRouter::SetSnapshotState(RouterState state, const std::wstring& message) {
    std::lock_guard lock(snapshotMutex_);
    snapshot_.state = state;
    snapshot_.message = message;
    if (state != RouterState::Running) {
        snapshot_.peak = 0.0F;
        snapshot_.applicationPeak = 0.0F;
        snapshot_.microphonePeak = 0.0F;
        snapshot_.limiting = false;
        snapshot_.monitorActive = false;
    }
}

void AudioRouter::Worker(RouteConfig config) {
    SetThreadDescription(GetCurrentThread(), L"ChromeMic audio mixer");
    try {
        ScopedComApartment apartment(COINIT_MULTITHREADED);
        if (FAILED(apartment.Result())) {
            SetSnapshotState(RouterState::Error,
                RouteFailure(L"Windows audio initialization failed", apartment.Result()));
            return;
        }

        ScopedMmcss mmcss;
        HRESULT failure = S_OK;
        std::wstring failureStep;

        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> destinationDevice;
        ComPtr<IMMDevice> microphoneDevice;
        ComPtr<IMMDevice> monitorDevice;

        ComPtr<IAudioClient> applicationAudioClient;
        ComPtr<IAudioCaptureClient> applicationCaptureClient;
        ComPtr<IAudioClient> microphoneAudioClient;
        ComPtr<IAudioCaptureClient> microphoneCaptureClient;
        ComPtr<IAudioClient> destinationAudioClient;
        ComPtr<IAudioRenderClient> destinationRenderClient;
        ComPtr<IAudioClient> monitorAudioClient;
        ComPtr<IAudioRenderClient> monitorRenderClient;

        WAVEFORMATEX sourceFormat = MakeProcessLoopbackCaptureFormat();
        HANDLE sourceProcessHandle = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                                 FALSE, config.sourceProcessId);
        const DWORD sourceProcessOpenError = sourceProcessHandle == nullptr
            ? GetLastError() : ERROR_SUCCESS;
        ScopedHandle sourceProcess(sourceProcessHandle);

        bool applicationStarted = false;
        bool microphoneStarted = false;
        bool destinationStarted = false;
        bool monitorStarted = false;
        bool monitorOperational = config.enableMonitor;
        std::wstring monitorSetupMessage;
        UINT32 destinationBufferFrames = 0;
        UINT32 monitorBufferFrames = 0;

        auto Require = [&](HRESULT result, const wchar_t* step) -> bool {
            if (FAILED(result)) {
                failure = result;
                failureStep = step;
                return false;
            }
            return true;
        };

        do {
            if (sourceProcess.Get() == nullptr) {
                failure = HRESULT_FROM_WIN32(sourceProcessOpenError);
                failureStep = L"Selected application (press Refresh apps and choose it again)";
                break;
            }
            if (!Require(ValidateProcessIdentity(sourceProcess.Get(), config),
                         L"Selected application changed or restarted (press Refresh apps)")) {
                break;
            }
            if (!Require(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                          IID_PPV_ARGS(&enumerator)), L"Audio device service")) {
                break;
            }
            if (!Require(enumerator->GetDevice(config.destinationId.c_str(), &destinationDevice),
                         L"Virtual-cable destination")) {
                break;
            }
            if (config.includeMicrophone &&
                !Require(enumerator->GetDevice(config.microphoneId.c_str(), &microphoneDevice),
                         L"Selected microphone")) {
                break;
            }
            if (monitorOperational) {
                const HRESULT monitorDeviceResult =
                    enumerator->GetDevice(config.monitorId.c_str(), &monitorDevice);
                if (FAILED(monitorDeviceResult)) {
                    monitorOperational = false;
                    monitorSetupMessage =
                        L"Headphone monitor unavailable; game output continues (" +
                        FriendlyHResult(monitorDeviceResult) + L")";
                }
            }
            if (stopRequested_.load(std::memory_order_acquire)) {
                break;
            }

            if (!Require(ActivateProcessLoopbackClient(config.sourceProcessId,
                                                       stopRequested_, sourceProcess.Get(),
                                                       applicationAudioClient.GetAddressOf()),
                         L"Per-application capture (requires Windows 10 build 20348 or later)")) {
                break;
            }
            if (stopRequested_.load(std::memory_order_acquire)) {
                break;
            }
            if (!Require(applicationAudioClient->Initialize(
                    AUDCLNT_SHAREMODE_SHARED,
                    AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                    0, 0, &sourceFormat, nullptr),
                    L"Per-application loopback capture")) {
                break;
            }
            if (!Require(applicationAudioClient->GetService(
                    IID_PPV_ARGS(&applicationCaptureClient)),
                    L"Per-application capture service")) {
                break;
            }

            if (config.includeMicrophone) {
                if (!Require(microphoneDevice->Activate(
                        __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                        reinterpret_cast<void**>(microphoneAudioClient.GetAddressOf())),
                        L"Microphone audio client")) {
                    break;
                }
                constexpr DWORD microphoneFlags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                                   AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
                                                   AUDCLNT_STREAMFLAGS_NOPERSIST;
                if (!Require(microphoneAudioClient->Initialize(
                        AUDCLNT_SHAREMODE_SHARED, microphoneFlags,
                        kBufferDuration, 0, &sourceFormat, nullptr),
                        L"Microphone shared format (check Windows microphone access)")) {
                    break;
                }
                if (!Require(microphoneAudioClient->GetService(
                        IID_PPV_ARGS(&microphoneCaptureClient)),
                        L"Microphone capture service")) {
                    break;
                }
            }

            if (!Require(OpenSharedRenderer(destinationDevice.Get(), sourceFormat,
                                            destinationAudioClient, destinationRenderClient,
                                            destinationBufferFrames),
                         L"Virtual-cable output")) {
                break;
            }
            if (monitorOperational) {
                const HRESULT monitorOpenResult = OpenSharedRenderer(
                    monitorDevice.Get(), sourceFormat, monitorAudioClient,
                    monitorRenderClient, monitorBufferFrames);
                if (FAILED(monitorOpenResult)) {
                    monitorOperational = false;
                    monitorSetupMessage =
                        L"Headphone monitor could not open; game output continues (" +
                        FriendlyHResult(monitorOpenResult) + L")";
                    monitorRenderClient.Reset();
                    monitorAudioClient.Reset();
                    monitorDevice.Reset();
                    monitorBufferFrames = 0;
                }
            }

            const DWORD setupProcessState = WaitForSingleObject(sourceProcess.Get(), 0);
            if (setupProcessState == WAIT_OBJECT_0) {
                failure = HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED);
                failureStep = L"Selected application ended during capture setup";
                break;
            }
            if (setupProcessState == WAIT_FAILED) {
                failure = HRESULT_FROM_WIN32(GetLastError());
                failureStep = L"Checking the selected application during capture setup";
                break;
            }
            if (!Require(ValidateProcessIdentity(sourceProcess.Get(), config),
                         L"Selected application changed during capture setup")) {
                break;
            }
            if (stopRequested_.load(std::memory_order_acquire)) {
                break;
            }

            if (!Require(destinationAudioClient->Start(), L"Virtual-cable output start")) {
                break;
            }
            destinationStarted = true;
            if (monitorOperational) {
                const HRESULT monitorStartResult = monitorAudioClient->Start();
                if (FAILED(monitorStartResult)) {
                    monitorOperational = false;
                    monitorSetupMessage =
                        L"Headphone monitor could not start; game output continues (" +
                        FriendlyHResult(monitorStartResult) + L")";
                    monitorRenderClient.Reset();
                    monitorAudioClient.Reset();
                    monitorDevice.Reset();
                    monitorBufferFrames = 0;
                } else {
                    monitorStarted = true;
                }
            }
            if (config.includeMicrophone) {
                if (!Require(microphoneAudioClient->Start(), L"Microphone capture start")) {
                    break;
                }
                microphoneStarted = true;
            }
            if (!Require(applicationAudioClient->Start(), L"Per-application capture start")) {
                break;
            }
            applicationStarted = true;
            if (stopRequested_.load(std::memory_order_acquire)) {
                break;
            }

            const UINT32 sampleRate = sourceFormat.nSamplesPerSec;
            const UINT16 channels = sourceFormat.nChannels;
            const UINT16 blockAlign = sourceFormat.nBlockAlign;
            if (sampleRate != VoiceEffectsProcessor::kSampleRate || channels != 2 ||
                blockAlign != 4 || sourceFormat.wBitsPerSample != 16) {
                failure = AUDCLNT_E_UNSUPPORTED_FORMAT;
                failureStep = L"Internal mixer format";
                break;
            }

            const size_t largestRenderBuffer = std::max<UINT32>(
                destinationBufferFrames, monitorOperational ? monitorBufferFrames : 0);
            const size_t queueFrames = std::max<size_t>(largestRenderBuffer * 2ULL,
                static_cast<size_t>(sampleRate) * kQueueDurationMs / 1000ULL);
            const size_t destinationTargetFrames = std::max<size_t>(destinationBufferFrames,
                static_cast<size_t>(sampleRate) * kTargetQueueDurationMs / 1000ULL);
            const size_t monitorTargetFrames = std::max<size_t>(
                monitorOperational ? monitorBufferFrames : destinationBufferFrames,
                static_cast<size_t>(sampleRate) * kTargetQueueDurationMs / 1000ULL);
            const size_t driftDeadbandFrames = std::max<size_t>(
                1, static_cast<size_t>(sampleRate) * 2ULL / 1000ULL);

            AudioProcessor applicationProcessor(sourceFormat, config.applicationGainDb);
            AudioProcessor microphoneGainProcessor(sourceFormat, config.microphoneGainDb);
            VoiceEffectsProcessor voiceProcessor(NormalizeVoiceEffectMode(config.voiceEffect));
            if (!applicationProcessor.IsSupported() || !microphoneGainProcessor.IsSupported()) {
                failure = AUDCLNT_E_UNSUPPORTED_FORMAT;
                failureStep = L"Internal audio processor";
                break;
            }

            AudioFrameRing applicationRing(queueFrames, blockAlign);
            AudioFrameRing microphoneRing(queueFrames, blockAlign);
            AudioFrameRing monitorRing(queueFrames, blockAlign);
            DriftCompensator applicationDrift(destinationTargetFrames, driftDeadbandFrames);
            DriftCompensator microphoneDrift(destinationTargetFrames, driftDeadbandFrames);
            DriftCompensator monitorDrift(monitorTargetFrames, driftDeadbandFrames);
            bool applicationPrimed = false;
            bool microphonePrimed = false;
            bool monitorPrimed = false;

            std::vector<std::byte> applicationPacket;
            std::vector<std::byte> microphonePacket;
            std::vector<std::byte> applicationFrames;
            std::vector<std::byte> microphoneFrames;
            std::vector<std::byte> applicationRateScratch;
            std::vector<std::byte> microphoneRateScratch;
            std::vector<std::byte> monitorRateScratch;

            uint64_t droppedFrames = 0;
            uint64_t underrunFrames = 0;
            float displayedApplicationPeak = 0.0F;
            float displayedMicrophonePeak = 0.0F;
            float displayedOutputPeak = 0.0F;
            bool previouslyMuted = config.muted;
            auto limiterLatchUntil = std::chrono::steady_clock::time_point{};
            auto nextSnapshotUpdate = std::chrono::steady_clock::now();

            {
                std::lock_guard snapshotLock(snapshotMutex_);
                snapshot_.state = RouterState::Running;
                snapshot_.message = config.includeMicrophone
                    ? L"LIVE — selected application + microphone are routed to the game cable"
                    : L"LIVE — only the selected application is routed to the game cable";
                snapshot_.sampleRate = sampleRate;
                snapshot_.channels = channels;
                snapshot_.latencyMilliseconds = static_cast<uint32_t>(
                    (destinationBufferFrames + destinationTargetFrames) * 1000ULL / sampleRate);
                snapshot_.monitorActive = monitorOperational;
                snapshot_.monitorMessage = monitorOperational
                    ? L"Microphone/effect monitor is active" : monitorSetupMessage;
            }

            auto DisableMonitor = [&](const std::wstring& reason) {
                if (monitorStarted && monitorAudioClient != nullptr) {
                    monitorAudioClient->Stop();
                }
                monitorStarted = false;
                monitorOperational = false;
                monitorPrimed = false;
                monitorRing.Clear();
                monitorDrift.Reset();
                monitorRenderClient.Reset();
                monitorAudioClient.Reset();
                monitorDevice.Reset();
                monitorBufferFrames = 0;
                std::lock_guard snapshotLock(snapshotMutex_);
                snapshot_.monitorActive = false;
                snapshot_.monitorMessage = reason;
            };

            while (!stopRequested_.load(std::memory_order_acquire)) {
                const DWORD processState = WaitForSingleObject(sourceProcess.Get(), 0);
                if (processState == WAIT_OBJECT_0) {
                    failure = HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED);
                    failureStep = L"Selected application ended (press Refresh apps)";
                    break;
                }
                if (processState == WAIT_FAILED) {
                    failure = HRESULT_FROM_WIN32(GetLastError());
                    failureStep = L"Checking the selected application";
                    break;
                }

                const bool currentlyMuted = muted_.load(std::memory_order_relaxed);
                bool discardGameCaptureThisCycle = false;
                if (currentlyMuted != previouslyMuted) {
                    // The cable path keeps a deliberate queue target. Flush it
                    // on both edges so samples captured during a muted interval
                    // cannot emerge after unmute. The independent monitor queue
                    // remains live. Delayed effects are reset only when opening
                    // the game path again, purging their muted-interval history.
                    applicationRing.Clear();
                    microphoneRing.Clear();
                    applicationPrimed = false;
                    microphonePrimed = false;
                    applicationDrift.Reset();
                    microphoneDrift.Reset();
                    displayedOutputPeak = 0.0F;
                    discardGameCaptureThisCycle = !currentlyMuted;
                    previouslyMuted = currentlyMuted;
                }
                const VoiceEffectMode requestedVoiceEffect = NormalizeVoiceEffectMode(
                    static_cast<VoiceEffectMode>(
                        voiceEffectMode_.load(std::memory_order_relaxed)));
                if (requestedVoiceEffect != voiceProcessor.Mode()) {
                    voiceProcessor.SetMode(requestedVoiceEffect);
                    microphoneRing.Clear();
                    monitorRing.Clear();
                    microphonePrimed = false;
                    monitorPrimed = false;
                    microphoneDrift.Reset();
                    monitorDrift.Reset();
                    displayedMicrophonePeak = 0.0F;
                }

                float applicationCyclePeak = 0.0F;
                float microphoneCyclePeak = 0.0F;
                bool cycleLimited = false;

                UINT32 applicationPacketFrames = 0;
                HRESULT result = applicationCaptureClient->GetNextPacketSize(
                    &applicationPacketFrames);
                if (FAILED(result)) {
                    failure = result;
                    failureStep = L"Reading selected-application audio";
                    break;
                }
                while (applicationPacketFrames != 0) {
                    BYTE* captureData = nullptr;
                    UINT32 capturedFrames = 0;
                    DWORD captureFlags = 0;
                    result = applicationCaptureClient->GetBuffer(
                        &captureData, &capturedFrames, &captureFlags, nullptr, nullptr);
                    if (FAILED(result)) {
                        failure = result;
                        failureStep = L"Reading selected-application audio buffer";
                        break;
                    }
                    const size_t packetBytes = static_cast<size_t>(capturedFrames) * blockAlign;
                    applicationPacket.resize(packetBytes);
                    if ((captureFlags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || captureData == nullptr) {
                        std::memset(applicationPacket.data(), 0, packetBytes);
                    } else {
                        std::memcpy(applicationPacket.data(), captureData, packetBytes);
                    }
                    result = applicationCaptureClient->ReleaseBuffer(capturedFrames);
                    if (FAILED(result)) {
                        failure = result;
                        failureStep = L"Releasing selected-application audio buffer";
                        break;
                    }
                    if ((captureFlags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0) {
                        applicationRing.Clear();
                        applicationPrimed = false;
                        applicationDrift.Reset();
                    }
                    const DspResult processed = applicationProcessor.Process(
                        applicationPacket.data(), capturedFrames,
                        applicationGainDb_.load(std::memory_order_relaxed), false);
                    applicationCyclePeak = std::max(applicationCyclePeak, processed.peak);
                    cycleLimited = cycleLimited || processed.limiting;
                    if (!discardGameCaptureThisCycle) {
                        droppedFrames += applicationRing.Push(
                            applicationPacket.data(), capturedFrames);
                    }
                    result = applicationCaptureClient->GetNextPacketSize(
                        &applicationPacketFrames);
                    if (FAILED(result)) {
                        failure = result;
                        failureStep = L"Reading selected-application packet size";
                        break;
                    }
                }
                if (FAILED(failure)) {
                    break;
                }

                if (config.includeMicrophone) {
                    UINT32 microphonePacketFrames = 0;
                    result = microphoneCaptureClient->GetNextPacketSize(&microphonePacketFrames);
                    if (FAILED(result)) {
                        failure = result;
                        failureStep = L"Microphone disconnected or became unavailable";
                        break;
                    }
                    while (microphonePacketFrames != 0) {
                        BYTE* captureData = nullptr;
                        UINT32 capturedFrames = 0;
                        DWORD captureFlags = 0;
                        result = microphoneCaptureClient->GetBuffer(
                            &captureData, &capturedFrames, &captureFlags, nullptr, nullptr);
                        if (FAILED(result)) {
                            failure = result;
                            failureStep = L"Reading microphone audio buffer";
                            break;
                        }
                        const size_t packetBytes = static_cast<size_t>(capturedFrames) * blockAlign;
                        microphonePacket.resize(packetBytes);
                        if ((captureFlags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || captureData == nullptr) {
                            std::memset(microphonePacket.data(), 0, packetBytes);
                        } else {
                            std::memcpy(microphonePacket.data(), captureData, packetBytes);
                        }
                        result = microphoneCaptureClient->ReleaseBuffer(capturedFrames);
                        if (FAILED(result)) {
                            failure = result;
                            failureStep = L"Releasing microphone audio buffer";
                            break;
                        }
                        if ((captureFlags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0) {
                            microphoneRing.Clear();
                            monitorRing.Clear();
                            microphonePrimed = false;
                            monitorPrimed = false;
                            microphoneDrift.Reset();
                            monitorDrift.Reset();
                            voiceProcessor.Reset();
                        }

                        const VoiceEffectsResult effectResult = voiceProcessor.Process(
                            reinterpret_cast<int16_t*>(microphonePacket.data()), capturedFrames);
                        const DspResult gainResult = microphoneGainProcessor.Process(
                            microphonePacket.data(), capturedFrames,
                            microphoneGainDb_.load(std::memory_order_relaxed), false);
                        microphoneCyclePeak = std::max(microphoneCyclePeak, gainResult.peak);
                        cycleLimited = cycleLimited || effectResult.limiting || gainResult.limiting;
                        if (!discardGameCaptureThisCycle) {
                            droppedFrames += microphoneRing.Push(
                                microphonePacket.data(), capturedFrames);
                        }
                        if (monitorOperational) {
                            droppedFrames += monitorRing.Push(
                                microphonePacket.data(), capturedFrames);
                        }
                        result = microphoneCaptureClient->GetNextPacketSize(
                            &microphonePacketFrames);
                        if (FAILED(result)) {
                            failure = result;
                            failureStep = L"Reading microphone packet size";
                            break;
                        }
                    }
                    if (FAILED(failure)) {
                        break;
                    }
                }
                if (discardGameCaptureThisCycle) {
                    // Capture packets drained on the unmute edge can predate
                    // the user's click. They were available to the independent
                    // monitor for continuity, but must not seed delayed effect
                    // state or either queue used by the game path.
                    voiceProcessor.Reset();
                }

                if (!applicationPrimed && applicationRing.SizeFrames() >= destinationTargetFrames) {
                    applicationPrimed = true;
                }
                if (config.includeMicrophone && !microphonePrimed &&
                    microphoneRing.SizeFrames() >= destinationTargetFrames) {
                    microphonePrimed = true;
                }
                if (monitorOperational && !monitorPrimed &&
                    monitorRing.SizeFrames() >= monitorTargetFrames) {
                    monitorPrimed = true;
                }

                UINT32 destinationPadding = 0;
                result = destinationAudioClient->GetCurrentPadding(&destinationPadding);
                if (FAILED(result)) {
                    failure = result;
                    failureStep = L"Checking virtual-cable output";
                    break;
                }
                if (destinationPadding > destinationBufferFrames) {
                    failure = E_UNEXPECTED;
                    failureStep = L"Virtual-cable buffer state";
                    break;
                }

                const UINT32 destinationWritable = destinationBufferFrames - destinationPadding;
                DspResult mixResult;
                if (destinationWritable != 0) {
                    const size_t writableBytes = static_cast<size_t>(destinationWritable) * blockAlign;
                    applicationFrames.resize(writableBytes);
                    microphoneFrames.resize(writableBytes);
                    FillFromRing(applicationRing, applicationDrift, applicationPrimed,
                                 applicationProcessor, applicationRateScratch,
                                 applicationFrames.data(), destinationWritable,
                                 blockAlign, underrunFrames);
                    if (config.includeMicrophone) {
                        FillFromRing(microphoneRing, microphoneDrift, microphonePrimed,
                                     microphoneGainProcessor, microphoneRateScratch,
                                     microphoneFrames.data(), destinationWritable,
                                     blockAlign, underrunFrames);
                    } else {
                        std::memset(microphoneFrames.data(), 0, writableBytes);
                    }

                    BYTE* renderData = nullptr;
                    result = destinationRenderClient->GetBuffer(destinationWritable, &renderData);
                    if (FAILED(result)) {
                        failure = result;
                        failureStep = L"Opening virtual-cable output buffer";
                        break;
                    }
                    mixResult = MixPcm16Stereo(
                        applicationFrames.data(),
                        config.includeMicrophone ? microphoneFrames.data() : nullptr,
                        reinterpret_cast<std::byte*>(renderData), destinationWritable,
                        currentlyMuted);
                    const DWORD releaseFlags = mixResult.peak <= 0.000001F
                        ? AUDCLNT_BUFFERFLAGS_SILENT : 0;
                    result = destinationRenderClient->ReleaseBuffer(
                        destinationWritable, releaseFlags);
                    if (FAILED(result)) {
                        failure = result;
                        failureStep = L"Sending mixed audio to the virtual cable";
                        break;
                    }
                    cycleLimited = cycleLimited || mixResult.limiting;
                }

                if (monitorOperational) {
                    UINT32 monitorPadding = 0;
                    result = monitorAudioClient->GetCurrentPadding(&monitorPadding);
                    if (FAILED(result) || monitorPadding > monitorBufferFrames) {
                        DisableMonitor(L"Headphone monitor disconnected; game output continues");
                    } else {
                        const UINT32 monitorWritable = monitorBufferFrames - monitorPadding;
                        if (monitorWritable != 0) {
                            BYTE* monitorData = nullptr;
                            result = monitorRenderClient->GetBuffer(monitorWritable, &monitorData);
                            if (FAILED(result)) {
                                DisableMonitor(L"Headphone monitor stopped; game output continues");
                            } else {
                                const size_t copied = FillFromRing(
                                    monitorRing, monitorDrift, monitorPrimed,
                                    microphoneGainProcessor, monitorRateScratch,
                                    reinterpret_cast<std::byte*>(monitorData), monitorWritable,
                                    blockAlign, underrunFrames);
                                result = monitorRenderClient->ReleaseBuffer(
                                    monitorWritable,
                                    copied == 0 ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
                                if (FAILED(result)) {
                                    DisableMonitor(L"Headphone monitor stopped; game output continues");
                                }
                            }
                        }
                    }
                }

                displayedApplicationPeak = std::max(
                    applicationCyclePeak, displayedApplicationPeak * 0.88F);
                displayedMicrophonePeak = std::max(
                    microphoneCyclePeak, displayedMicrophonePeak * 0.88F);
                displayedOutputPeak = std::max(mixResult.peak, displayedOutputPeak * 0.88F);
                if (cycleLimited) {
                    limiterLatchUntil = std::chrono::steady_clock::now() +
                                        std::chrono::milliseconds(700);
                }
                const auto now = std::chrono::steady_clock::now();
                if (now >= nextSnapshotUpdate) {
                    std::lock_guard snapshotLock(snapshotMutex_);
                    snapshot_.applicationPeak = displayedApplicationPeak;
                    snapshot_.microphonePeak = config.includeMicrophone
                        ? displayedMicrophonePeak : 0.0F;
                    snapshot_.peak = currentlyMuted ? 0.0F : displayedOutputPeak;
                    snapshot_.muted = currentlyMuted;
                    snapshot_.limiting = now < limiterLatchUntil;
                    snapshot_.droppedFrames = droppedFrames;
                    snapshot_.underrunFrames = underrunFrames;
                    snapshot_.queuedMilliseconds = static_cast<uint32_t>(
                        std::max(applicationRing.SizeFrames(), microphoneRing.SizeFrames()) *
                        1000ULL / sampleRate);
                    snapshot_.monitorActive = monitorOperational;
                    if (currentlyMuted) {
                        snapshot_.message = monitorOperational
                            ? L"MUTED TO GAME — microphone monitor remains active"
                            : L"MUTED TO GAME — capture remains active";
                    } else if (config.includeMicrophone) {
                        snapshot_.message = L"LIVE — application + microphone → game cable";
                    } else {
                        snapshot_.message = L"LIVE — selected application → game cable";
                    }
                    nextSnapshotUpdate = now + std::chrono::milliseconds(33);
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(3));
            }
        } while (false);

        if (applicationStarted) {
            applicationAudioClient->Stop();
        }
        if (microphoneStarted) {
            microphoneAudioClient->Stop();
        }
        if (monitorStarted) {
            monitorAudioClient->Stop();
        }
        if (destinationStarted) {
            destinationAudioClient->Stop();
        }

        if (FAILED(failure) && !stopRequested_.load(std::memory_order_acquire)) {
            SetSnapshotState(RouterState::Error, RouteFailure(failureStep.c_str(), failure));
        } else if (!stopRequested_.load(std::memory_order_acquire)) {
            SetSnapshotState(RouterState::Stopped, L"Routing ended");
        }
    } catch (const std::bad_alloc&) {
        if (!stopRequested_.load(std::memory_order_acquire)) {
            SetSnapshotState(RouterState::Error,
                             L"Audio routing stopped because memory allocation failed");
        }
    } catch (const std::exception& exception) {
        if (!stopRequested_.load(std::memory_order_acquire)) {
            std::wstring message = L"Audio routing stopped after an internal error";
            if (exception.what() != nullptr && exception.what()[0] != '\0') {
                message += L" (see the source build log for details)";
            }
            SetSnapshotState(RouterState::Error, message);
        }
    } catch (...) {
        if (!stopRequested_.load(std::memory_order_acquire)) {
            SetSnapshotState(RouterState::Error,
                             L"Audio routing stopped after an unknown internal error");
        }
    }
}
