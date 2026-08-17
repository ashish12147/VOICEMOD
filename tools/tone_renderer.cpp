#include <Windows.h>
#include <Audioclient.h>
#include <Audiopolicy.h>
#include <Avrt.h>
#include <Mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT32 kSampleRate = 48000;
constexpr WORD kChannels = 2;
constexpr WORD kBitsPerSample = 16;
constexpr REFERENCE_TIME kBufferDuration = 500000; // 50 ms in 100 ns units.
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

class ScopedMmcss {
public:
    ScopedMmcss() noexcept {
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

class ScopedAudioStart {
public:
    explicit ScopedAudioStart(IAudioClient* client) noexcept : client_(client) {}
    ~ScopedAudioStart() {
        if (client_ != nullptr) {
            client_->Stop();
        }
    }

private:
    IAudioClient* client_ = nullptr;
};

struct Options {
    std::wstring endpointId;
    double frequencyHz = 0.0;
    double amplitude = 0.0;
    uint32_t durationMilliseconds = 0;
};

WAVEFORMATEX ToneFormat() noexcept {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = kChannels;
    format.nSamplesPerSec = kSampleRate;
    format.wBitsPerSample = kBitsPerSample;
    format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    return format;
}

std::wstring HResultText(HRESULT result) {
    wchar_t* rawMessage = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(result), 0, reinterpret_cast<wchar_t*>(&rawMessage), 0, nullptr);
    std::wstring message;
    if (length != 0 && rawMessage != nullptr) {
        message.assign(rawMessage, length);
        LocalFree(rawMessage);
        while (!message.empty() &&
               (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
            message.pop_back();
        }
    } else {
        wchar_t fallback[32]{};
        swprintf_s(fallback, L"HRESULT 0x%08lX", static_cast<unsigned long>(result));
        message = fallback;
    }
    return message;
}

bool ParseDouble(const wchar_t* text, double& value) noexcept {
    if (text == nullptr || *text == L'\0') {
        return false;
    }
    wchar_t* end = nullptr;
    value = std::wcstod(text, &end);
    return end != text && end != nullptr && *end == L'\0' && std::isfinite(value);
}

bool ParseUnsigned(const wchar_t* text, uint32_t& value) noexcept {
    if (text == nullptr || *text == L'\0' || *text == L'-') {
        return false;
    }
    wchar_t* end = nullptr;
    const unsigned long parsed = std::wcstoul(text, &end, 10);
    if (end == text || end == nullptr || *end != L'\0' ||
        parsed > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    value = static_cast<uint32_t>(parsed);
    return true;
}

bool ParseOptions(int argc, wchar_t** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        if (index + 1 >= argc) {
            return false;
        }
        const wchar_t* value = argv[++index];
        if (argument == L"--endpoint-id") {
            options.endpointId = value;
        } else if (argument == L"--frequency") {
            if (!ParseDouble(value, options.frequencyHz)) {
                return false;
            }
        } else if (argument == L"--amplitude") {
            if (!ParseDouble(value, options.amplitude)) {
                return false;
            }
        } else if (argument == L"--duration-ms") {
            if (!ParseUnsigned(value, options.durationMilliseconds)) {
                return false;
            }
        } else {
            return false;
        }
    }

    return !options.endpointId.empty() && options.frequencyHz >= 100.0 &&
           options.frequencyHz <= 5000.0 && options.amplitude > 0.0 &&
           options.amplitude <= 0.05 && options.durationMilliseconds >= 500 &&
           options.durationMilliseconds <= 15000;
}

UINT32 FillTone(BYTE* destination, UINT32 frames, uint64_t firstFrame,
                uint64_t totalToneFrames, double frequencyHz, double amplitude) noexcept {
    if (destination == nullptr || frames == 0) {
        return 0;
    }

    auto* samples = reinterpret_cast<int16_t*>(destination);
    const UINT32 toneFrames = static_cast<UINT32>(std::min<uint64_t>(
        frames, totalToneFrames > firstFrame ? totalToneFrames - firstFrame : 0));
    const uint64_t rampFrames = std::min<uint64_t>(kSampleRate / 50, totalToneFrames / 4); // 20 ms.

    for (UINT32 frame = 0; frame < frames; ++frame) {
        double sample = 0.0;
        if (frame < toneFrames) {
            const uint64_t absoluteFrame = firstFrame + frame;
            double envelope = 1.0;
            if (rampFrames != 0 && absoluteFrame < rampFrames) {
                envelope = static_cast<double>(absoluteFrame) / static_cast<double>(rampFrames);
            }
            const uint64_t remainingFrames = totalToneFrames - absoluteFrame;
            if (rampFrames != 0 && remainingFrames <= rampFrames) {
                envelope = std::min(envelope,
                    static_cast<double>(remainingFrames) / static_cast<double>(rampFrames));
            }
            const double phase = 2.0 * kPi * frequencyHz *
                                 static_cast<double>(absoluteFrame) / static_cast<double>(kSampleRate);
            sample = std::sin(phase) * amplitude * envelope;
        }
        const auto encoded = static_cast<int16_t>(std::lround(std::clamp(sample, -1.0, 1.0) * 32767.0));
        samples[static_cast<size_t>(frame) * kChannels] = encoded;
        samples[static_cast<size_t>(frame) * kChannels + 1] = encoded;
    }
    return toneFrames;
}

HRESULT RenderTone(const Options& options) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS(&enumerator));
    if (FAILED(result)) {
        return result;
    }

    ComPtr<IMMDevice> device;
    result = enumerator->GetDevice(options.endpointId.c_str(), &device);
    if (FAILED(result)) {
        return result;
    }

    ComPtr<IAudioClient> audioClient;
    result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(audioClient.GetAddressOf()));
    if (FAILED(result)) {
        return result;
    }

    WAVEFORMATEX format = ToneFormat();
    constexpr DWORD streamFlags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                  AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
                                  AUDCLNT_STREAMFLAGS_NOPERSIST;
    result = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags,
                                     kBufferDuration, 0, &format, nullptr);
    if (FAILED(result)) {
        return result;
    }

    ComPtr<IAudioRenderClient> renderClient;
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

    UINT32 bufferFrames = 0;
    result = audioClient->GetBufferSize(&bufferFrames);
    if (FAILED(result) || bufferFrames == 0) {
        return FAILED(result) ? result : E_UNEXPECTED;
    }

    const uint64_t totalToneFrames =
        static_cast<uint64_t>(kSampleRate) * options.durationMilliseconds / 1000;
    uint64_t submittedToneFrames = 0;

    BYTE* initialData = nullptr;
    result = renderClient->GetBuffer(bufferFrames, &initialData);
    if (FAILED(result)) {
        return result;
    }
    const UINT32 initialToneFrames = FillTone(initialData, bufferFrames, submittedToneFrames,
                                               totalToneFrames, options.frequencyHz, options.amplitude);
    result = renderClient->ReleaseBuffer(bufferFrames,
        initialToneFrames == 0 ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
    if (FAILED(result)) {
        return result;
    }
    submittedToneFrames += initialToneFrames;

    result = audioClient->Start();
    if (FAILED(result)) {
        return result;
    }
    ScopedAudioStart started(audioClient.Get());
    ScopedMmcss mmcss;

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(options.durationMilliseconds + 3000);
    while (submittedToneFrames < totalToneFrames) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
        UINT32 paddingFrames = 0;
        result = audioClient->GetCurrentPadding(&paddingFrames);
        if (FAILED(result)) {
            return result;
        }
        if (paddingFrames > bufferFrames) {
            return E_UNEXPECTED;
        }
        const UINT32 writableFrames = bufferFrames - paddingFrames;
        if (writableFrames == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        BYTE* renderData = nullptr;
        result = renderClient->GetBuffer(writableFrames, &renderData);
        if (FAILED(result)) {
            return result;
        }
        const UINT32 toneFrames = FillTone(renderData, writableFrames, submittedToneFrames,
                                           totalToneFrames, options.frequencyHz, options.amplitude);
        result = renderClient->ReleaseBuffer(writableFrames,
            toneFrames == 0 ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
        if (FAILED(result)) {
            return result;
        }
        submittedToneFrames += toneFrames;
    }

    // Allow the final queued frames to leave the endpoint before stopping the stream.
    for (;;) {
        UINT32 paddingFrames = 0;
        result = audioClient->GetCurrentPadding(&paddingFrames);
        if (FAILED(result)) {
            return result;
        }
        if (paddingFrames == 0) {
            return S_OK;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetThreadDescription(GetCurrentThread(), L"ChromeMic deterministic tone renderer");
    Options options;
    if (!ParseOptions(argc, argv, options)) {
        std::wcerr << L"Usage: chromemic_tone_renderer.exe --endpoint-id <id> "
                      L"--frequency <100..5000> --amplitude <0..0.05> "
                      L"--duration-ms <500..15000>\n";
        return 64;
    }

    ScopedComApartment apartment;
    if (FAILED(apartment.Result())) {
        std::wcerr << L"COM initialization failed: " << HResultText(apartment.Result()) << L"\n";
        return 1;
    }

    const HRESULT result = RenderTone(options);
    if (FAILED(result)) {
        std::wcerr << L"Tone rendering failed: " << HResultText(result) << L"\n";
        return 1;
    }
    return 0;
}
