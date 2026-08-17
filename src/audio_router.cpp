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
#include <memory>
#include <new>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr REFERENCE_TIME kBufferDuration = 500000; // 50 ms in 100 ns units
constexpr uint32_t kQueueDurationMs = 250;

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

} // namespace

AudioRouter::~AudioRouter() {
    Stop();
}

bool AudioRouter::Start(const RouteConfig& config, std::wstring& errorMessage) {
    std::lock_guard lock(lifecycleMutex_);
    if (worker_.joinable() || stopping_) {
        errorMessage = stopping_
            ? L"The previous route is still stopping. Wait a moment and retry."
            : L"Routing is already active. Stop it before starting again.";
        return false;
    }
    if (config.sourceProcessId == 0 || config.destinationId.empty()) {
        errorMessage = L"Choose both an application and a virtual-cable destination.";
        return false;
    }

    stopRequested_.store(false, std::memory_order_release);
    gainDb_.store(AudioProcessor::ClampGainDb(config.gainDb), std::memory_order_relaxed);
    muted_.store(config.muted, std::memory_order_relaxed);
    {
        std::lock_guard snapshotLock(snapshotMutex_);
        snapshot_ = {};
        snapshot_.state = RouterState::Starting;
        snapshot_.message = L"Opening per-application capture and the virtual cable...";
        snapshot_.sourceName = config.sourceName;
        snapshot_.destinationName = config.destinationName;
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
        // Guard the complete Stop transaction even when no worker is currently joinable.
        // Otherwise Start could create a generation before the Stopped snapshot is published.
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
            snapshot_.message = L"Stopped — no audio is being captured";
            snapshot_.peak = 0.0F;
            snapshot_.limiting = false;
            snapshot_.queuedMilliseconds = 0;
        }
        stopping_ = false;
    }
    lifecycleCondition_.notify_all();
}

void AudioRouter::SetGainDb(float gainDb) noexcept {
    gainDb_.store(AudioProcessor::ClampGainDb(gainDb), std::memory_order_relaxed);
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
        snapshot_.limiting = false;
    }
}

void AudioRouter::Worker(RouteConfig config) {
    SetThreadDescription(GetCurrentThread(), L"ChromeMic audio router");
    try {
        // Declared before all COM interfaces so it is destroyed last.
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
        ComPtr<IAudioClient> captureAudioClient;
        ComPtr<IAudioCaptureClient> captureClient;
        ComPtr<IAudioClient> renderAudioClient;
        ComPtr<IAudioRenderClient> renderClient;
        ComPtr<ISimpleAudioVolume> renderSessionVolume;
        ComPtr<IAudioSessionControl> renderSessionControl;
        ComPtr<IAudioSessionControl2> renderSessionControl2;
        WAVEFORMATEX sourceFormat = MakeProcessLoopbackCaptureFormat();
        HANDLE sourceProcessHandle = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                                 FALSE, config.sourceProcessId);
        const DWORD sourceProcessOpenError = sourceProcessHandle == nullptr ? GetLastError() : ERROR_SUCCESS;
        ScopedHandle sourceProcess(sourceProcessHandle);
        bool captureStarted = false;
        bool renderStarted = false;

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
        if (!Require(enumerator->GetDevice(config.destinationId.c_str(), &destinationDevice), L"Destination device")) {
            break;
        }
        if (stopRequested_.load(std::memory_order_acquire)) {
            break;
        }
        if (!Require(ActivateProcessLoopbackClient(config.sourceProcessId,
                                                   stopRequested_,
                                                   sourceProcess.Get(),
                                                   captureAudioClient.GetAddressOf()),
                     L"Per-application capture (requires Windows 10 build 20348 or later)")) {
            break;
        }
        if (stopRequested_.load(std::memory_order_acquire)) {
            break;
        }

        AudioProcessor processor(sourceFormat, config.gainDb);
        if (!processor.IsSupported()) {
            failure = AUDCLNT_E_UNSUPPORTED_FORMAT;
            failureStep = L"Process capture format (use PCM or 32-bit float in Sound settings)";
            break;
        }

        if (!Require(captureAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                                    AUDCLNT_STREAMFLAGS_LOOPBACK |
                                                        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                                    0, 0, &sourceFormat, nullptr),
                     L"Per-application loopback capture")) {
            break;
        }
        if (!Require(captureAudioClient->GetService(IID_PPV_ARGS(&captureClient)), L"Per-application capture service")) {
            break;
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
                     L"Selected application ended or restarted during capture setup")) {
            break;
        }

        if (!Require(destinationDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                                 reinterpret_cast<void**>(renderAudioClient.GetAddressOf())), L"Destination audio client")) {
            break;
        }
        constexpr DWORD conversionFlags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                          AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
                                          AUDCLNT_STREAMFLAGS_NOPERSIST;
        if (!Require(renderAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, conversionFlags,
                                                   kBufferDuration, 0, &sourceFormat, nullptr),
                     L"Destination format")) {
            break;
        }
        if (!Require(renderAudioClient->GetService(IID_PPV_ARGS(&renderClient)), L"Destination render service")) {
            break;
        }
        if (SUCCEEDED(renderAudioClient->GetService(IID_PPV_ARGS(&renderSessionVolume)))) {
            // This is ChromeMic's per-session volume, not the endpoint master volume.
            renderSessionVolume->SetMute(FALSE, nullptr);
            renderSessionVolume->SetMasterVolume(1.0F, nullptr);
        }
        if (SUCCEEDED(renderAudioClient->GetService(IID_PPV_ARGS(&renderSessionControl))) &&
            SUCCEEDED(renderSessionControl.As(&renderSessionControl2))) {
            renderSessionControl2->SetDuckingPreference(TRUE);
        }

        UINT32 renderBufferFrames = 0;
        if (!Require(renderAudioClient->GetBufferSize(&renderBufferFrames), L"Destination buffer size")) {
            break;
        }
        const UINT32 sampleRate = sourceFormat.nSamplesPerSec;
        const UINT16 channels = sourceFormat.nChannels;
        const UINT16 blockAlign = sourceFormat.nBlockAlign;
        if (renderBufferFrames == 0 || sampleRate == 0 || blockAlign == 0) {
            failure = E_INVALIDARG;
            failureStep = L"Audio endpoint format";
            break;
        }

        BYTE* initialBuffer = nullptr;
        if (!Require(renderClient->GetBuffer(renderBufferFrames, &initialBuffer), L"Destination prefill")) {
            break;
        }
        if (!Require(renderClient->ReleaseBuffer(renderBufferFrames, AUDCLNT_BUFFERFLAGS_SILENT), L"Destination prefill")) {
            break;
        }

        if (stopRequested_.load(std::memory_order_acquire)) {
            break;
        }
        const DWORD finalSetupProcessState = WaitForSingleObject(sourceProcess.Get(), 0);
        if (finalSetupProcessState == WAIT_OBJECT_0) {
            failure = HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED);
            failureStep = L"Selected application ended before capture start";
            break;
        }
        if (finalSetupProcessState == WAIT_FAILED) {
            failure = HRESULT_FROM_WIN32(GetLastError());
            failureStep = L"Checking the selected application before capture start";
            break;
        }
        if (!Require(ValidateProcessIdentity(sourceProcess.Get(), config),
                     L"Selected application changed before capture start")) {
            break;
        }

        if (!Require(renderAudioClient->Start(), L"Destination start")) {
            break;
        }
        renderStarted = true;
        if (stopRequested_.load(std::memory_order_acquire)) {
            break;
        }
        if (!Require(captureAudioClient->Start(), L"Per-application capture start")) {
            break;
        }
        captureStarted = true;
        if (stopRequested_.load(std::memory_order_acquire)) {
            break;
        }

        const size_t queueFrames = std::max<size_t>(renderBufferFrames * 2ULL,
            static_cast<size_t>(sampleRate) * kQueueDurationMs / 1000ULL);
        const size_t targetQueueFrames = std::max<size_t>(renderBufferFrames,
            static_cast<size_t>(sampleRate) * 40ULL / 1000ULL);
        const size_t driftDeadbandFrames = std::max<size_t>(1, static_cast<size_t>(sampleRate) * 2ULL / 1000ULL);
        AudioFrameRing ring(queueFrames, blockAlign);
        std::vector<std::byte> packet;
        std::vector<std::byte> rateScratch;
        uint64_t droppedFrames = 0;
        uint64_t underrunFrames = 0;
        auto limiterLatchUntil = std::chrono::steady_clock::time_point{};
        auto nextSnapshotUpdate = std::chrono::steady_clock::now();
        float displayedPeak = 0.0F;
        DriftCompensator driftCompensator(targetQueueFrames, driftDeadbandFrames);
        bool queuePrimed = false;

        {
            std::lock_guard lock(snapshotMutex_);
            snapshot_.state = RouterState::Running;
            snapshot_.message = L"Routing only the selected app — game and other-app audio are excluded";
            snapshot_.sampleRate = sampleRate;
            snapshot_.channels = channels;
            snapshot_.latencyMilliseconds = static_cast<uint32_t>(
                (renderBufferFrames + targetQueueFrames) * 1000ULL / sampleRate);
        }

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
            UINT32 nextPacketFrames = 0;
            HRESULT result = captureClient->GetNextPacketSize(&nextPacketFrames);
            if (FAILED(result)) {
                failure = result;
                failureStep = L"Reading source audio";
                break;
            }

            float cyclePeak = 0.0F;
            bool cycleLimited = false;
            while (nextPacketFrames != 0) {
                BYTE* captureData = nullptr;
                UINT32 capturedFrames = 0;
                DWORD captureFlags = 0;
                result = captureClient->GetBuffer(&captureData, &capturedFrames, &captureFlags, nullptr, nullptr);
                if (FAILED(result)) {
                    failure = result;
                    failureStep = L"Reading source audio buffer";
                    break;
                }

                const size_t packetBytes = static_cast<size_t>(capturedFrames) * blockAlign;
                packet.resize(packetBytes);
                if ((captureFlags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || captureData == nullptr) {
                    std::memset(packet.data(), 0, packetBytes);
                } else {
                    std::memcpy(packet.data(), captureData, packetBytes);
                }
                result = captureClient->ReleaseBuffer(capturedFrames);
                if (FAILED(result)) {
                    failure = result;
                    failureStep = L"Releasing source audio buffer";
                    break;
                }

                if ((captureFlags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0) {
                    ring.Clear();
                    queuePrimed = false;
                    driftCompensator.Reset();
                }

                const bool isMuted = muted_.load(std::memory_order_relaxed);
                const DspResult dspResult = processor.Process(packet.data(), capturedFrames,
                    gainDb_.load(std::memory_order_relaxed), isMuted);
                cyclePeak = std::max(cyclePeak, dspResult.peak);
                cycleLimited = cycleLimited || dspResult.limiting;
                droppedFrames += ring.Push(packet.data(), capturedFrames);

                result = captureClient->GetNextPacketSize(&nextPacketFrames);
                if (FAILED(result)) {
                    failure = result;
                    failureStep = L"Reading source packet size";
                    break;
                }
            }
            if (FAILED(failure)) {
                break;
            }

            const bool currentlyMuted = muted_.load(std::memory_order_relaxed);
            if (currentlyMuted) {
                // Mute is immediate: discard any already-processed live frames so they cannot leak after the click.
                ring.Clear();
                cyclePeak = 0.0F;
                displayedPeak = 0.0F;
                queuePrimed = false;
                driftCompensator.Reset();
            } else if (!queuePrimed && ring.SizeFrames() >= targetQueueFrames) {
                queuePrimed = true;
            }

            UINT32 paddingFrames = 0;
            result = renderAudioClient->GetCurrentPadding(&paddingFrames);
            if (FAILED(result)) {
                failure = result;
                failureStep = L"Checking destination buffer";
                break;
            }
            if (paddingFrames > renderBufferFrames) {
                failure = E_UNEXPECTED;
                failureStep = L"Destination buffer state";
                break;
            }

            const UINT32 writableFrames = renderBufferFrames - paddingFrames;
            if (writableFrames != 0) {
                BYTE* renderData = nullptr;
                result = renderClient->GetBuffer(writableFrames, &renderData);
                if (FAILED(result)) {
                    failure = result;
                    failureStep = L"Opening destination buffer";
                    break;
                }

                size_t copiedFrames = 0;
                if (queuePrimed) {
                    const size_t inputFrames = driftCompensator.InputFrames(ring.SizeFrames(), writableFrames);
                    if (ring.SizeFrames() >= inputFrames) {
                        if (inputFrames == writableFrames) {
                            copiedFrames = ring.Pop(reinterpret_cast<std::byte*>(renderData), writableFrames);
                        } else {
                            rateScratch.resize(inputFrames * blockAlign);
                            ring.Pop(rateScratch.data(), inputFrames);
                            if (processor.ConvertFrameCount(rateScratch.data(), static_cast<uint32_t>(inputFrames),
                                                            reinterpret_cast<std::byte*>(renderData), writableFrames)) {
                                copiedFrames = writableFrames;
                            }
                        }
                    } else {
                        copiedFrames = ring.Pop(reinterpret_cast<std::byte*>(renderData), writableFrames);
                        queuePrimed = false;
                        driftCompensator.Reset();
                    }
                }

                if (copiedFrames < writableFrames) {
                    std::memset(renderData + copiedFrames * blockAlign, 0,
                                static_cast<size_t>(writableFrames - copiedFrames) * blockAlign);
                    if (copiedFrames != 0) {
                        // No packets while the source is silent is normal; count only partial starvation.
                        underrunFrames += writableFrames - copiedFrames;
                    }
                }
                const DWORD flags = copiedFrames == 0 ? AUDCLNT_BUFFERFLAGS_SILENT : 0;
                result = renderClient->ReleaseBuffer(writableFrames, flags);
                if (FAILED(result)) {
                    failure = result;
                    failureStep = L"Sending destination audio";
                    break;
                }
            }

            displayedPeak = std::max(cyclePeak, displayedPeak * 0.88F);
            if (cycleLimited) {
                limiterLatchUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= nextSnapshotUpdate) {
                std::lock_guard lock(snapshotMutex_);
                snapshot_.peak = displayedPeak;
                snapshot_.muted = currentlyMuted;
                snapshot_.limiting = now < limiterLatchUntil;
                snapshot_.droppedFrames = droppedFrames;
                snapshot_.underrunFrames = underrunFrames;
                snapshot_.queuedMilliseconds = static_cast<uint32_t>(ring.SizeFrames() * 1000ULL / sampleRate);
                snapshot_.message = snapshot_.muted
                    ? L"Muted — capture is active but only silence is sent"
                    : L"Routing only the selected app — other apps are excluded";
                nextSnapshotUpdate = now + std::chrono::milliseconds(33);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        } while (false);

        if (captureStarted) {
            captureAudioClient->Stop();
        }
        if (renderStarted) {
            renderAudioClient->Stop();
        }

        if (FAILED(failure) && !stopRequested_.load(std::memory_order_acquire)) {
            SetSnapshotState(RouterState::Error, RouteFailure(failureStep.c_str(), failure));
        } else if (!stopRequested_.load(std::memory_order_acquire)) {
            SetSnapshotState(RouterState::Stopped, L"Routing ended");
        }
    } catch (const std::bad_alloc&) {
        if (!stopRequested_.load(std::memory_order_acquire)) {
            SetSnapshotState(RouterState::Error, L"Audio routing stopped because memory allocation failed");
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
            SetSnapshotState(RouterState::Error, L"Audio routing stopped after an unknown internal error");
        }
    }
}
