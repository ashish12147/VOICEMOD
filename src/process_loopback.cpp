#include "process_loopback.h"

#include <Mmdeviceapi.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <atomic>
#include <chrono>
#include <iterator>
#include <new>

using Microsoft::WRL::ComPtr;

namespace {

class ProcessLoopbackActivationHandler final :
    public Microsoft::WRL::RuntimeClass<
        Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
        Microsoft::WRL::FtmBase,
        IActivateAudioInterfaceCompletionHandler> {
public:
    explicit ProcessLoopbackActivationHandler(DWORD processId) noexcept
        : completedEvent_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        parameters_ = MakeProcessLoopbackActivationParams(processId);
        activationParameters_.vt = VT_BLOB;
        activationParameters_.blob.cbSize = sizeof(parameters_);
        activationParameters_.blob.pBlobData = reinterpret_cast<BYTE*>(&parameters_);
    }

    ~ProcessLoopbackActivationHandler() {
        if (completedEvent_ != nullptr) {
            CloseHandle(completedEvent_);
        }
    }

    [[nodiscard]] HANDLE CompletedEvent() const noexcept { return completedEvent_; }
    [[nodiscard]] PROPVARIANT* ActivationParameters() noexcept { return &activationParameters_; }

    HRESULT CopyAudioClient(IAudioClient** destination) const noexcept {
        if (destination == nullptr) {
            return E_POINTER;
        }
        *destination = nullptr;
        if (FAILED(result_)) {
            return result_;
        }
        if (audioClient_ == nullptr) {
            return E_UNEXPECTED;
        }
        return audioClient_.CopyTo(destination);
    }

    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) noexcept override {
        result_ = E_UNEXPECTED;
        if (operation != nullptr) {
            HRESULT activationResult = E_UNEXPECTED;
            ComPtr<IUnknown> activatedInterface;
            const HRESULT operationResult = operation->GetActivateResult(
                &activationResult, &activatedInterface);
            if (FAILED(operationResult)) {
                result_ = operationResult;
            } else if (FAILED(activationResult)) {
                result_ = activationResult;
            } else {
                result_ = activatedInterface.As(&audioClient_);
            }
        }
        if (completedEvent_ != nullptr) {
            SetEvent(completedEvent_);
        }
        return S_OK;
    }

private:
    HANDLE completedEvent_ = nullptr;
    AUDIOCLIENT_ACTIVATION_PARAMS parameters_{};
    PROPVARIANT activationParameters_{};
    HRESULT result_ = E_PENDING;
    ComPtr<IAudioClient> audioClient_;
};

} // namespace

AUDIOCLIENT_ACTIVATION_PARAMS MakeProcessLoopbackActivationParams(
    DWORD processId) noexcept {
    AUDIOCLIENT_ACTIVATION_PARAMS parameters{};
    parameters.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    parameters.ProcessLoopbackParams.TargetProcessId = processId;
    parameters.ProcessLoopbackParams.ProcessLoopbackMode =
        PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    return parameters;
}

WAVEFORMATEX MakeProcessLoopbackCaptureFormat() noexcept {
    // Microsoft's process-loopback reference sample uses stereo PCM16 at 44.1 kHz.
    // AUTOCONVERTPCM on both sides handles application and cable rate differences.
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = 44100;
    format.wBitsPerSample = 16;
    format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    return format;
}

HRESULT ActivateProcessLoopbackClient(DWORD processId,
                                      const std::atomic_bool& stopRequested,
                                      HANDLE targetProcess,
                                      IAudioClient** audioClient) noexcept {
    if (audioClient == nullptr) {
        return E_POINTER;
    }
    *audioClient = nullptr;
    if (processId == 0 || targetProcess == nullptr || targetProcess == INVALID_HANDLE_VALUE) {
        return E_INVALIDARG;
    }
    if (stopRequested.load(std::memory_order_acquire)) {
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }
    const DWORD initialProcessState = WaitForSingleObject(targetProcess, 0);
    if (initialProcessState == WAIT_OBJECT_0) {
        return HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED);
    }
    if (initialProcessState == WAIT_FAILED) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    try {
        auto handler = Microsoft::WRL::Make<ProcessLoopbackActivationHandler>(processId);
        if (handler == nullptr) {
            return E_OUTOFMEMORY;
        }
        if (handler->CompletedEvent() == nullptr) {
            return HRESULT_FROM_WIN32(GetLastError());
        }

        ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
        const HRESULT activateResult = ActivateAudioInterfaceAsync(
            VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
            __uuidof(IAudioClient),
            handler->ActivationParameters(),
            handler.Get(),
            &operation);
        if (FAILED(activateResult)) {
            return activateResult;
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        const HANDLE waitHandles[] = {handler->CompletedEvent(), targetProcess};
        for (;;) {
            const DWORD waitResult = WaitForMultipleObjects(
                static_cast<DWORD>(std::size(waitHandles)), waitHandles, FALSE, 50);
            if (waitResult == WAIT_OBJECT_0) {
                if (stopRequested.load(std::memory_order_acquire)) {
                    return HRESULT_FROM_WIN32(ERROR_CANCELLED);
                }
                const DWORD completedProcessState = WaitForSingleObject(targetProcess, 0);
                if (completedProcessState == WAIT_OBJECT_0) {
                    return HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED);
                }
                if (completedProcessState == WAIT_FAILED) {
                    return HRESULT_FROM_WIN32(GetLastError());
                }
                return handler->CopyAudioClient(audioClient);
            }
            if (waitResult == WAIT_OBJECT_0 + 1) {
                return HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED);
            }
            if (waitResult == WAIT_FAILED) {
                return HRESULT_FROM_WIN32(GetLastError());
            }
            if (stopRequested.load(std::memory_order_acquire)) {
                // Windows retains the agile completion handler until the async operation
                // finishes, so a late callback cannot access AudioRouter state.
                return HRESULT_FROM_WIN32(ERROR_CANCELLED);
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            }
        }
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_UNEXPECTED;
    }
}
