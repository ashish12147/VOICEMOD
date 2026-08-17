#include "audio_devices.h"

#include <Windows.h>
#include <Audioclient.h>
#include <Propkeydef.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <Mmdeviceapi.h>
#include <Propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <map>
#include <sstream>

using Microsoft::WRL::ComPtr;

namespace {

class ScopedCom {
public:
    explicit ScopedCom(DWORD model) noexcept {
        const HRESULT result = CoInitializeEx(nullptr, model);
        usable_ = SUCCEEDED(result) || result == RPC_E_CHANGED_MODE;
        uninitialize_ = SUCCEEDED(result);
        result_ = result;
    }

    ~ScopedCom() {
        if (uninitialize_) {
            CoUninitialize();
        }
    }

    [[nodiscard]] bool usable() const noexcept { return usable_; }
    [[nodiscard]] HRESULT result() const noexcept { return result_; }

private:
    bool usable_ = false;
    bool uninitialize_ = false;
    HRESULT result_ = E_FAIL;
};

std::wstring Lowercase(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

std::wstring EndpointId(IMMDevice* device) {
    LPWSTR rawId = nullptr;
    if (FAILED(device->GetId(&rawId)) || rawId == nullptr) {
        return {};
    }

    std::wstring id(rawId);
    CoTaskMemFree(rawId);
    return id;
}

std::wstring EndpointName(IMMDevice* device) {
    ComPtr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &store))) {
        return L"Unnamed audio device";
    }

    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring result = L"Unnamed audio device";
    if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value))) {
        wchar_t text[512]{};
        if (SUCCEEDED(PropVariantToString(value, text, static_cast<UINT>(std::size(text)))) && text[0] != L'\0') {
            result = text;
        }
    }
    PropVariantClear(&value);
    return result;
}

std::wstring ShortId(const std::wstring& id) {
    if (id.size() <= 8) {
        return id;
    }
    std::wstring suffix = id.substr(id.size() - 8);
    suffix.erase(std::remove_if(suffix.begin(), suffix.end(), [](wchar_t character) {
        return character == L'}' || character == L'{' || character == L'.';
    }), suffix.end());
    return suffix.empty() ? id.substr(id.size() - 8) : suffix;
}

HRESULT EnumerateFlow(IMMDeviceEnumerator* enumerator, EDataFlow flow, std::vector<AudioDeviceInfo>& devices) {
    ComPtr<IMMDeviceCollection> collection;
    HRESULT result = enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(result)) {
        return result;
    }

    ComPtr<IMMDevice> defaultDevice;
    std::wstring defaultId;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eMultimedia, &defaultDevice))) {
        defaultId = EndpointId(defaultDevice.Get());
    }

    UINT count = 0;
    result = collection->GetCount(&count);
    if (FAILED(result)) {
        return result;
    }

    devices.clear();
    devices.reserve(count);
    for (UINT index = 0; index < count; ++index) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(index, &device))) {
            continue;
        }

        AudioDeviceInfo info;
        info.id = EndpointId(device.Get());
        info.name = EndpointName(device.Get());
        if (info.id.empty()) {
            continue;
        }
        info.isDefault = info.id == defaultId;
        info.isLikelyVirtual = IsLikelyVirtualAudioName(info.name);
        info.isVoicemod = IsVoicemodAudioName(info.name);
        info.isGenericCable = IsGenericVirtualCableName(info.name);
        devices.push_back(std::move(info));
    }

    std::map<std::wstring, size_t> nameCounts;
    for (const auto& device : devices) {
        ++nameCounts[Lowercase(device.name)];
    }
    for (auto& device : devices) {
        if (nameCounts[Lowercase(device.name)] > 1) {
            device.name += L"  [" + ShortId(device.id) + L"]";
        }
    }

    std::stable_sort(devices.begin(), devices.end(), [](const AudioDeviceInfo& left, const AudioDeviceInfo& right) {
        if (left.isDefault != right.isDefault) {
            return left.isDefault;
        }
        if (left.isGenericCable != right.isGenericCable) {
            return left.isGenericCable;
        }
        return Lowercase(left.name) < Lowercase(right.name);
    });
    return S_OK;
}

} // namespace

bool EnumerateAudioDevices(AudioDeviceInventory& inventory, std::wstring& errorMessage) {
    ScopedCom apartment(COINIT_APARTMENTTHREADED);
    if (!apartment.usable()) {
        errorMessage = L"Windows audio could not initialize: " + FriendlyHResult(apartment.result());
        return false;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(result)) {
        errorMessage = L"Windows audio device service is unavailable: " + FriendlyHResult(result);
        return false;
    }

    result = EnumerateFlow(enumerator.Get(), eRender, inventory.playback);
    if (FAILED(result)) {
        errorMessage = L"Playback devices could not be listed: " + FriendlyHResult(result);
        return false;
    }
    result = EnumerateFlow(enumerator.Get(), eCapture, inventory.recording);
    if (FAILED(result)) {
        errorMessage = L"Recording devices could not be listed: " + FriendlyHResult(result);
        return false;
    }

    errorMessage.clear();
    return true;
}

bool IsLikelyVirtualAudioName(const std::wstring& name) {
    const std::wstring lower = Lowercase(name);
    constexpr const wchar_t* markers[] = {
        L"voicemod", L"cable input", L"cable output", L"vb-audio", L"voicemeeter", L"virtual audio",
        L"virtual cable", L"dummy output", L"sonar", L"wave link", L"vac ", L"blackhole"
    };
    return std::any_of(std::begin(markers), std::end(markers), [&](const wchar_t* marker) {
        return lower.find(marker) != std::wstring::npos;
    });
}

bool IsGenericVirtualCableName(const std::wstring& name) {
    const std::wstring lower = Lowercase(name);
    const bool mixer = lower.find(L"voicemeeter") != std::wstring::npos ||
                       lower.find(L"sonar") != std::wstring::npos ||
                       lower.find(L"wave link") != std::wstring::npos;
    const bool cable = lower.find(L"cable input") != std::wstring::npos ||
                       lower.find(L"cable output") != std::wstring::npos ||
                       lower.find(L"virtual audio cable") != std::wstring::npos ||
                       lower.find(L"virtual cable") != std::wstring::npos ||
                       lower.find(L"vac ") != std::wstring::npos;
    return cable && !mixer && !IsVoicemodAudioName(name);
}

bool IsVoicemodAudioName(const std::wstring& name) {
    return Lowercase(name).find(L"voicemod") != std::wstring::npos ||
           Lowercase(name).find(L"dummy output") != std::wstring::npos;
}

std::wstring FriendlyHResult(long rawResult) {
    const HRESULT result = static_cast<HRESULT>(rawResult);
    switch (result) {
    case AUDCLNT_E_DEVICE_IN_USE:
        return L"the endpoint is busy (close the other audio app or disable Exclusive mode, then retry)";
    case AUDCLNT_E_DEVICE_INVALIDATED:
        return L"the endpoint was disconnected or its driver restarted";
    case AUDCLNT_E_UNSUPPORTED_FORMAT:
        return L"the endpoints use incompatible formats (set both to 48 kHz in Sound settings)";
    case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED:
        return L"Exclusive mode is not available";
    case E_ACCESSDENIED:
        return L"Windows denied audio access";
    default:
        break;
    }

    wchar_t* message = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = FormatMessageW(flags, nullptr, static_cast<DWORD>(result), 0,
                                        reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wstring text;
    if (length != 0 && message != nullptr) {
        text.assign(message, length);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ' || text.back() == L'.')) {
            text.pop_back();
        }
        LocalFree(message);
    } else {
        std::wostringstream stream;
        stream << L"Windows error 0x" << std::hex << std::uppercase << static_cast<unsigned long>(result);
        text = stream.str();
    }
    return text;
}
