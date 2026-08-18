#include "app_processes.h"
#include "audio_devices.h"
#include "audio_router.h"
#include "update_checker.h"

#include <Windows.h>
#include <CommCtrl.h>
#include <Dwmapi.h>
#include <Objbase.h>
#include <Shellapi.h>
#include <Uxtheme.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr wchar_t kWindowClass[] = L"ChromeMicMainWindow";
constexpr wchar_t kRegistryPath[] = L"Software\\ChromeMic";
constexpr UINT_PTR kStatusTimer = 1;
constexpr UINT kUpdateCheckCompleteMessage = WM_APP + 17;
constexpr int kMinimumClientWidth = 720;
constexpr int kMinimumClientHeight = 500;
constexpr int kInitialClientWidth = 860;
constexpr int kInitialClientHeight = 650;
constexpr int kContentHeight = 870;
constexpr DWORD kMainWindowStyle =
    WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_VSCROLL;

constexpr int IDC_UPDATE_RETRY = 1001;
constexpr int IDC_UPDATE_NOW = 1002;
constexpr int IDC_APPLICATION = 1003;
constexpr int IDC_REFRESH_APPS = 1004;
constexpr int IDC_APP_GAIN = 1005;
constexpr int IDC_APP_GAIN_VALUE = 1006;
constexpr int IDC_APP_METER = 1007;
constexpr int IDC_MIC_ENABLE = 1008;
constexpr int IDC_MICROPHONE = 1009;
constexpr int IDC_REFRESH_DEVICES = 1010;
constexpr int IDC_MIC_GAIN = 1011;
constexpr int IDC_MIC_GAIN_VALUE = 1012;
constexpr int IDC_MIC_METER = 1013;
constexpr int IDC_VOICE_EFFECT = 1014;
constexpr int IDC_DESTINATION = 1015;
constexpr int IDC_GET_CABLE = 1016;
constexpr int IDC_GAME_MIC = 1017;
constexpr int IDC_MONITOR_ENABLE = 1018;
constexpr int IDC_MONITOR = 1019;
constexpr int IDC_SOUND_SETTINGS = 1020;
constexpr int IDC_MIC_PRIVACY = 1021;
constexpr int IDC_OUTPUT_METER = 1022;
constexpr int IDC_LIMITER = 1023;
constexpr int IDC_MUTE = 1024;
constexpr int IDC_START = 1025;
constexpr int IDC_STOP = 1026;
constexpr int IDC_STATUS = 1027;
constexpr int IDC_METRICS = 1028;
constexpr int IDC_SETUP = 1029;

constexpr COLORREF kBackground = RGB(245, 247, 251);
constexpr COLORREF kText = RGB(26, 31, 44);
constexpr COLORREF kMutedText = RGB(91, 101, 122);
constexpr COLORREF kGreen = RGB(0, 112, 67);
constexpr COLORREF kRed = RGB(208, 54, 64);
constexpr COLORREF kOrange = RGB(145, 73, 0);
constexpr COLORREF kBlue = RGB(55, 94, 246);
constexpr COLORREF kMeterBackground = RGB(222, 227, 237);

enum class UpdateGateState { Checking, UpToDate, UpdateAvailable, Failed };

struct SavedSettings {
    std::wstring sourceApplicationPath;
    std::wstring destinationId;
    std::wstring microphoneId;
    std::wstring monitorId;
    int applicationGainTenths = 0;
    int microphoneGainTenths = 0;
    VoiceEffectMode voiceEffect = VoiceEffectMode::Natural;
};

struct UiControls {
    HWND title = nullptr;
    HWND subtitle = nullptr;
    HWND updateGroup = nullptr;
    HWND updateStatus = nullptr;
    HWND updateRetry = nullptr;
    HWND updateNow = nullptr;
    HWND sourcesGroup = nullptr;
    HWND applicationLabel = nullptr;
    HWND applicationCombo = nullptr;
    HWND refreshApps = nullptr;
    HWND applicationGainLabel = nullptr;
    HWND applicationGain = nullptr;
    HWND applicationGainValue = nullptr;
    HWND applicationMeterLabel = nullptr;
    HWND applicationMeter = nullptr;
    HWND microphoneEnable = nullptr;
    HWND microphoneCombo = nullptr;
    HWND refreshDevices = nullptr;
    HWND microphoneGainLabel = nullptr;
    HWND microphoneGain = nullptr;
    HWND microphoneGainValue = nullptr;
    HWND microphoneMeterLabel = nullptr;
    HWND microphoneMeter = nullptr;
    HWND effectLabel = nullptr;
    HWND effectCombo = nullptr;
    HWND effectHelp = nullptr;
    HWND outputsGroup = nullptr;
    HWND destinationLabel = nullptr;
    HWND destinationCombo = nullptr;
    HWND getCable = nullptr;
    HWND gameMicrophone = nullptr;
    HWND monitorEnable = nullptr;
    HWND monitorCombo = nullptr;
    HWND soundSettings = nullptr;
    HWND microphonePrivacy = nullptr;
    HWND routeGroup = nullptr;
    HWND outputMeterLabel = nullptr;
    HWND outputMeter = nullptr;
    HWND limiter = nullptr;
    HWND mute = nullptr;
    HWND start = nullptr;
    HWND stop = nullptr;
    HWND status = nullptr;
    HWND metrics = nullptr;
    HWND setup = nullptr;
    HWND privacy = nullptr;
};

HINSTANCE gInstance = nullptr;
HWND gMainWindow = nullptr;
UiControls gUi;
HFONT gTitleFont = nullptr;
HFONT gHeadingFont = nullptr;
HFONT gBodyFont = nullptr;
HFONT gSmallFont = nullptr;
HBRUSH gBackgroundBrush = nullptr;
UINT gUiDpi = 96;

AudioDeviceInventory gInventory;
std::vector<AppProcessInfo> gApplications;
std::vector<AudioDeviceInfo> gCableDestinations;
std::vector<AudioDeviceInfo> gMicrophones;
std::vector<AudioDeviceInfo> gPhysicalMonitors;
std::unique_ptr<AudioRouter> gRouter;
SavedSettings gSavedSettings;
std::wstring gApplicationError;
std::wstring gApplicationNotice;
std::wstring gAudioDeviceError;
std::wstring gAudioDeviceNotice;

UpdateGateState gUpdateGateState = UpdateGateState::Checking;
std::wstring gUpdateBanner = L"Checking for updates — routing is locked...";
std::wstring gValidatedUpdateUrl;
std::thread gUpdateThread;
std::mutex gUpdateResultMutex;
UpdateCheckResult gPendingUpdateResult;
std::atomic_bool gUpdateResultReady = false;
std::atomic_bool gUpdateCheckInFlight = false;
std::atomic_bool gIsClosing = false;
bool gStartAfterUpdateCheck = false;
int gVerticalScroll = 0;
int gLayoutOffsetY = 0;

int ScaleUi(int value) {
    return MulDiv(value, static_cast<int>(gUiDpi), 96);
}

int UnscaleUi(int value) {
    return MulDiv(value, 96, static_cast<int>(gUiDpi));
}

bool IsChecked(HWND control) {
    return control != nullptr &&
           SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

std::wstring Lowercase(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

bool ContainsIgnoringCase(const std::wstring& value,
                          const std::wstring& fragment) {
    return Lowercase(value).find(Lowercase(fragment)) != std::wstring::npos;
}

std::wstring AsciiToWide(std::string_view value) {
    std::wstring result;
    try {
        result.reserve(value.size());
        for (const unsigned char character : value) {
            if (character < 0x20U || character > 0x7EU) {
                return {};
            }
            result.push_back(static_cast<wchar_t>(character));
        }
    } catch (...) {
        return {};
    }
    return result;
}

HFONT CreateUiFont(int pointSize, int weight) {
    const int height = -MulDiv(pointSize, static_cast<int>(gUiDpi), 72);
    return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                       L"Segoe UI");
}

void ApplyFont(HWND control, HFONT font) {
    if (control != nullptr && font != nullptr) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

HWND MakeControl(const wchar_t* className, const wchar_t* text, DWORD style,
                 int id, HFONT font = nullptr) {
    HWND control = CreateWindowExW(
        0, className, text, style | WS_CHILD | WS_VISIBLE, 0, 0, 10, 10,
        gMainWindow, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        gInstance, nullptr);
    ApplyFont(control, font);
    return control;
}

void MoveControl(HWND control, int x, int y, int width, int height) {
    if (control != nullptr) {
        MoveWindow(control, ScaleUi(x), ScaleUi(y + gLayoutOffsetY), ScaleUi(width),
                   ScaleUi(height), TRUE);
    }
}

std::wstring ReadRegistryString(HKEY key, const wchar_t* name) {
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) !=
            ERROR_SUCCESS ||
        type != REG_SZ || bytes == 0 || bytes > 16384 ||
        bytes % sizeof(wchar_t) != 0) {
        return {};
    }
    std::wstring value(bytes / sizeof(wchar_t) + 1U, L'\0');
    DWORD actualBytes = static_cast<DWORD>(value.size() * sizeof(wchar_t));
    if (RegQueryValueExW(key, name, nullptr, nullptr,
                         reinterpret_cast<BYTE*>(value.data()),
                         &actualBytes) != ERROR_SUCCESS ||
        actualBytes > bytes || actualBytes % sizeof(wchar_t) != 0) {
        return {};
    }
    value.resize(actualBytes / sizeof(wchar_t));
    while (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    return value;
}

int ReadRegistryInteger(HKEY key, const wchar_t* name, int defaultValue,
                        int minimum, int maximum) {
    DWORD type = 0;
    DWORD bytes = sizeof(DWORD);
    DWORD raw = 0;
    if (RegQueryValueExW(key, name, nullptr, &type,
                         reinterpret_cast<BYTE*>(&raw), &bytes) !=
            ERROR_SUCCESS ||
        type != REG_DWORD || bytes != sizeof(DWORD)) {
        return defaultValue;
    }
    return std::clamp(static_cast<int>(std::bit_cast<std::int32_t>(raw)),
                      minimum, maximum);
}

SavedSettings LoadSettings() {
    SavedSettings settings;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, KEY_QUERY_VALUE,
                      &key) != ERROR_SUCCESS) {
        return settings;
    }
    settings.sourceApplicationPath =
        ReadRegistryString(key, L"SourceApplicationPath");
    settings.destinationId =
        ReadRegistryString(key, L"DestinationEndpointId");
    settings.microphoneId = ReadRegistryString(key, L"MicrophoneEndpointId");
    settings.monitorId = ReadRegistryString(key, L"MonitorEndpointId");
    constexpr int kMissingGainSetting = -1000000;
    settings.applicationGainTenths = ReadRegistryInteger(
        key, L"ApplicationGainTenths", kMissingGainSetting, -240, 60);
    if (settings.applicationGainTenths == kMissingGainSetting) {
        // ChromeMic 1.1 stored the application level under this older name.
        settings.applicationGainTenths =
            ReadRegistryInteger(key, L"GainTenths", 0, -240, 60);
    }
    settings.microphoneGainTenths =
        ReadRegistryInteger(key, L"MicrophoneGainTenths", 0, -240, 60);
    settings.voiceEffect = static_cast<VoiceEffectMode>(
        ReadRegistryInteger(key, L"VoiceEffect", 0, 0, 5));
    RegCloseKey(key);
    return settings;
}

void WriteRegistryString(HKEY key, const wchar_t* name,
                         const std::wstring& value) {
    const DWORD bytes =
        static_cast<DWORD>((value.size() + 1U) * sizeof(wchar_t));
    RegSetValueExW(key, name, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(value.c_str()), bytes);
}

void WriteRegistryInteger(HKEY key, const wchar_t* name, int value) {
    const DWORD raw =
        std::bit_cast<DWORD>(static_cast<std::int32_t>(value));
    RegSetValueExW(key, name, 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&raw), sizeof(raw));
}

const AppProcessInfo* SelectedApplication() {
    if (gUi.applicationCombo == nullptr) {
        return nullptr;
    }
    const LRESULT item =
        SendMessageW(gUi.applicationCombo, CB_GETCURSEL, 0, 0);
    if (item == CB_ERR) {
        return nullptr;
    }
    const LRESULT index = SendMessageW(gUi.applicationCombo, CB_GETITEMDATA,
                                        static_cast<WPARAM>(item), 0);
    if (index < 0 || static_cast<size_t>(index) >= gApplications.size()) {
        return nullptr;
    }
    return &gApplications[static_cast<size_t>(index)];
}

const AudioDeviceInfo* SelectedDevice(
    HWND combo, const std::vector<AudioDeviceInfo>& devices) {
    if (combo == nullptr) {
        return nullptr;
    }
    const LRESULT item = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (item == CB_ERR) {
        return nullptr;
    }
    const LRESULT index =
        SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(item), 0);
    if (index < 0 || static_cast<size_t>(index) >= devices.size()) {
        return nullptr;
    }
    return &devices[static_cast<size_t>(index)];
}

VoiceEffectMode SelectedVoiceEffect() {
    if (gUi.effectCombo == nullptr) {
        return VoiceEffectMode::Natural;
    }
    const LRESULT item = SendMessageW(gUi.effectCombo, CB_GETCURSEL, 0, 0);
    if (item == CB_ERR) {
        return VoiceEffectMode::Natural;
    }
    const LRESULT value = SendMessageW(gUi.effectCombo, CB_GETITEMDATA,
                                        static_cast<WPARAM>(item), 0);
    if (value < static_cast<LRESULT>(VoiceEffectMode::Natural) ||
        value > static_cast<LRESULT>(VoiceEffectMode::DeepTone)) {
        return VoiceEffectMode::Natural;
    }
    return static_cast<VoiceEffectMode>(value);
}

void CaptureAvailableSelections() {
    if (const auto* application = SelectedApplication()) {
        gSavedSettings.sourceApplicationPath = application->executablePath;
    }
    if (const auto* destination =
            SelectedDevice(gUi.destinationCombo, gCableDestinations)) {
        gSavedSettings.destinationId = destination->id;
    }
    if (const auto* microphone =
            SelectedDevice(gUi.microphoneCombo, gMicrophones)) {
        gSavedSettings.microphoneId = microphone->id;
    }
    if (const auto* monitor =
            SelectedDevice(gUi.monitorCombo, gPhysicalMonitors)) {
        gSavedSettings.monitorId = monitor->id;
    }
    if (gUi.applicationGain != nullptr) {
        gSavedSettings.applicationGainTenths = static_cast<int>(
            SendMessageW(gUi.applicationGain, TBM_GETPOS, 0, 0));
    }
    if (gUi.microphoneGain != nullptr) {
        gSavedSettings.microphoneGainTenths = static_cast<int>(
            SendMessageW(gUi.microphoneGain, TBM_GETPOS, 0, 0));
    }
    if (gUi.effectCombo != nullptr) {
        gSavedSettings.voiceEffect = SelectedVoiceEffect();
    }
}

void SaveSettings() {
    CaptureAvailableSelections();
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    WriteRegistryString(key, L"SourceApplicationPath",
                        gSavedSettings.sourceApplicationPath);
    WriteRegistryString(key, L"DestinationEndpointId",
                        gSavedSettings.destinationId);
    WriteRegistryString(key, L"MicrophoneEndpointId",
                        gSavedSettings.microphoneId);
    WriteRegistryString(key, L"MonitorEndpointId",
                        gSavedSettings.monitorId);
    WriteRegistryInteger(key, L"ApplicationGainTenths",
                         gSavedSettings.applicationGainTenths);
    WriteRegistryInteger(key, L"MicrophoneGainTenths",
                         gSavedSettings.microphoneGainTenths);
    WriteRegistryInteger(key, L"VoiceEffect",
                         static_cast<int>(gSavedSettings.voiceEffect));
    RegCloseKey(key);
}

int FindExactDeviceIndex(const std::vector<AudioDeviceInfo>& devices,
                         const std::wstring& id) {
    if (id.empty()) {
        return -1;
    }
    for (size_t index = 0; index < devices.size(); ++index) {
        if (devices[index].id == id) {
            return index <= static_cast<size_t>(INT_MAX)
                       ? static_cast<int>(index)
                       : -1;
        }
    }
    return -1;
}

std::wstring DeviceDisplayName(const AudioDeviceInfo& device) {
    std::wstring label = device.name;
    if (device.isDefault) {
        label += L"  — Windows default";
    }
    if (device.isGenericCable) {
        label += L"  [virtual cable]";
    } else if (device.isLikelyVirtual) {
        label += L"  [virtual]";
    }
    return label;
}

void FillApplicationCombo(int desiredIndex) {
    SendMessageW(gUi.applicationCombo, CB_RESETCONTENT, 0, 0);
    const LRESULT placeholder = SendMessageW(
        gUi.applicationCombo, CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(L"Choose a running application..."));
    if (placeholder != CB_ERR && placeholder != CB_ERRSPACE) {
        SendMessageW(gUi.applicationCombo, CB_SETITEMDATA,
                     static_cast<WPARAM>(placeholder), -1);
    }
    int selectedItem = placeholder >= 0 ? static_cast<int>(placeholder) : -1;
    for (size_t index = 0; index < gApplications.size(); ++index) {
        const std::wstring label =
            BuildApplicationDisplayLabel(gApplications[index]);
        const LRESULT item = SendMessageW(
            gUi.applicationCombo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(label.c_str()));
        if (item != CB_ERR && item != CB_ERRSPACE) {
            SendMessageW(gUi.applicationCombo, CB_SETITEMDATA,
                         static_cast<WPARAM>(item),
                         static_cast<LPARAM>(index));
            if (static_cast<int>(index) == desiredIndex) {
                selectedItem = static_cast<int>(item);
            }
        }
    }
    SendMessageW(gUi.applicationCombo, CB_SETCURSEL, selectedItem, 0);
    SendMessageW(gUi.applicationCombo, CB_SETMINVISIBLE, 12, 0);
}

void FillDeviceCombo(HWND combo, const std::vector<AudioDeviceInfo>& devices,
                     const std::wstring& desiredId,
                     const wchar_t* placeholderText) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    const LRESULT placeholder = SendMessageW(
        combo, CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(placeholderText));
    if (placeholder != CB_ERR && placeholder != CB_ERRSPACE) {
        SendMessageW(combo, CB_SETITEMDATA, static_cast<WPARAM>(placeholder),
                     -1);
    }
    const int desiredIndex = FindExactDeviceIndex(devices, desiredId);
    int selectedItem = placeholder >= 0 ? static_cast<int>(placeholder) : -1;
    for (size_t index = 0; index < devices.size(); ++index) {
        const std::wstring label = DeviceDisplayName(devices[index]);
        const LRESULT item = SendMessageW(
            combo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(label.c_str()));
        if (item != CB_ERR && item != CB_ERRSPACE) {
            SendMessageW(combo, CB_SETITEMDATA, static_cast<WPARAM>(item),
                         static_cast<LPARAM>(index));
            if (static_cast<int>(index) == desiredIndex) {
                selectedItem = static_cast<int>(item);
            }
        }
    }
    SendMessageW(combo, CB_SETCURSEL, selectedItem, 0);
    SendMessageW(combo, CB_SETMINVISIBLE, 12, 0);
}

void BuildFilteredDeviceLists() {
    gCableDestinations.clear();
    gMicrophones.clear();
    gPhysicalMonitors.clear();
    for (const auto& device : gInventory.playback) {
        if (device.isGenericCable && !device.isVoicemod) {
            gCableDestinations.push_back(device);
        }
        if (!device.isLikelyVirtual && !device.isGenericCable &&
            !device.isVoicemod) {
            gPhysicalMonitors.push_back(device);
        }
    }
    for (const auto& device : gInventory.recording) {
        if (!device.isGenericCable && !device.isVoicemod) {
            gMicrophones.push_back(device);
        }
    }
}

std::wstring SuggestedGameMicrophone() {
    const auto* destination =
        SelectedDevice(gUi.destinationCombo, gCableDestinations);
    if (destination == nullptr) {
        return L"Choose a virtual cable playback side first.";
    }
    if (ContainsIgnoringCase(destination->name, L"CABLE Input")) {
        const AudioDeviceInfo* matchingRecording = nullptr;
        size_t matchingCount = 0;
        for (const auto& recording : gInventory.recording) {
            if (ContainsIgnoringCase(recording.name, L"CABLE Output")) {
                matchingRecording = &recording;
                ++matchingCount;
            }
        }
        if (matchingCount == 1 && matchingRecording != nullptr) {
            return L"In your game, select: " + matchingRecording->name +
                   L" (verify before chat)";
        }
        if (matchingCount > 1) {
            return L"Multiple CABLE Output devices were found—select the recording side matching this cable product.";
        }
    }
    const AudioDeviceInfo* onlyCandidate = nullptr;
    size_t candidateCount = 0;
    for (const auto& recording : gInventory.recording) {
        if (recording.isGenericCable) {
            onlyCandidate = &recording;
            ++candidateCount;
        }
    }
    if (candidateCount == 1 && onlyCandidate != nullptr) {
        return L"Likely game microphone: " + onlyCandidate->name +
               L" — verify the vendor pairing.";
    }
    if (candidateCount > 1) {
        return L"Select the recording side matching this cable product in your game.";
    }
    return L"The matching recording side is not active.";
}

void UpdateControlState();
void StartRoutingVerified();

void UpdateSetupGuidance() {
    std::wstring message;
    if (!gApplicationError.empty()) {
        message = L"Application list error — " + gApplicationError;
    } else if (!gAudioDeviceError.empty()) {
        message = L"Audio-device error — " + gAudioDeviceError;
    } else if (SelectedApplication() == nullptr) {
        message = !gApplicationNotice.empty()
                      ? gApplicationNotice
                      : L"Choose an application. Open Chrome, then press Refresh apps if needed.";
    } else if (SelectedDevice(gUi.destinationCombo, gCableDestinations) ==
               nullptr) {
        message = !gAudioDeviceNotice.empty()
                      ? gAudioDeviceNotice
                      : L"Choose a recognized generic virtual-cable playback side.";
    } else if (IsChecked(gUi.microphoneEnable) &&
               SelectedDevice(gUi.microphoneCombo, gMicrophones) == nullptr) {
        message = L"Include microphone is on — choose a non-cable microphone.";
    } else if (IsChecked(gUi.monitorEnable) &&
               SelectedDevice(gUi.monitorCombo, gPhysicalMonitors) == nullptr) {
        message = L"Loopback test is on — choose physical headphones or speakers.";
    } else if (!gApplicationNotice.empty()) {
        message = gApplicationNotice;
    } else if (!gAudioDeviceNotice.empty()) {
        message = gAudioDeviceNotice;
    } else if (const auto* application = SelectedApplication()) {
        message = IsChromeExecutableName(application->executableName)
                      ? L"Chrome-wide capture: all tabs/windows in this Chrome process tree are included; unrelated apps stay excluded."
                      : L"Only the selected application's process tree is captured; unrelated apps stay excluded.";
    }
    SetWindowTextW(gUi.setup, message.c_str());
    const std::wstring gameMicrophone = SuggestedGameMicrophone();
    SetWindowTextW(gUi.gameMicrophone, gameMicrophone.c_str());
    InvalidateRect(gUi.setup, nullptr, TRUE);
    InvalidateRect(gUi.gameMicrophone, nullptr, TRUE);
}

void RefreshApplications(bool preserveSelection) {
    const RouterSnapshot snapshot = gRouter->Snapshot();
    if (snapshot.state == RouterState::Running ||
        snapshot.state == RouterState::Starting) {
        return;
    }
    if (snapshot.state == RouterState::Error) {
        gRouter->Stop();
    }
    const AppProcessInfo* previous =
        preserveSelection ? SelectedApplication() : nullptr;
    const uint32_t previousProcessId =
        previous != nullptr ? previous->processId : 0;
    const uint64_t previousCreationTime =
        previous != nullptr ? previous->creationTime : 0;
    const std::wstring preferredPath = previous != nullptr
                                           ? previous->executablePath
                                           : gSavedSettings.sourceApplicationPath;
    std::vector<AppProcessInfo> applications;
    std::wstring error;
    if (!EnumerateDesktopApplications(applications, error)) {
        gApplications.clear();
        gApplicationError = error.empty()
                                ? L"Windows could not enumerate desktop applications."
                                : error;
        gApplicationNotice.clear();
        FillApplicationCombo(-1);
        UpdateSetupGuidance();
        UpdateControlState();
        return;
    }
    gApplicationError.clear();
    gApplications = std::move(applications);
    const int selectedIndex = FindPreferredApplicationIndex(
        gApplications, previousProcessId, preferredPath, previousCreationTime);
    FillApplicationCombo(selectedIndex);
    if ((previousProcessId != 0 || !preferredPath.empty()) &&
        selectedIndex < 0) {
        gApplicationNotice = previousProcessId != 0
                                 ? L"The selected application ended or restarted. Choose it again explicitly."
                                 : L"The saved application is missing or ambiguous. Choose it explicitly.";
    } else if (gApplications.empty()) {
        gApplicationNotice =
            L"No selectable applications found. Open Chrome, then press Refresh apps.";
    } else if (selectedIndex < 0) {
        gApplicationNotice =
            L"Choose a running application; multiple candidates are never auto-selected.";
    } else {
        gApplicationNotice.clear();
    }
    UpdateSetupGuidance();
    UpdateControlState();
}

void RefreshAudioDevices(bool preserveSelections) {
    const RouterSnapshot snapshot = gRouter->Snapshot();
    if (snapshot.state == RouterState::Running ||
        snapshot.state == RouterState::Starting) {
        return;
    }
    if (snapshot.state == RouterState::Error) {
        gRouter->Stop();
    }
    const auto* currentDestination = preserveSelections
                                         ? SelectedDevice(gUi.destinationCombo,
                                                          gCableDestinations)
                                         : nullptr;
    const auto* currentMicrophone = preserveSelections
                                        ? SelectedDevice(gUi.microphoneCombo,
                                                         gMicrophones)
                                        : nullptr;
    const auto* currentMonitor = preserveSelections
                                     ? SelectedDevice(gUi.monitorCombo,
                                                      gPhysicalMonitors)
                                     : nullptr;
    const std::wstring destinationId = currentDestination != nullptr
                                           ? currentDestination->id
                                           : gSavedSettings.destinationId;
    const std::wstring microphoneId = currentMicrophone != nullptr
                                          ? currentMicrophone->id
                                          : gSavedSettings.microphoneId;
    const std::wstring monitorId = currentMonitor != nullptr
                                       ? currentMonitor->id
                                       : gSavedSettings.monitorId;
    AudioDeviceInventory inventory;
    std::wstring error;
    if (!EnumerateAudioDevices(inventory, error)) {
        gInventory = {};
        gCableDestinations.clear();
        gMicrophones.clear();
        gPhysicalMonitors.clear();
        gAudioDeviceError = error.empty()
                                ? L"Windows could not enumerate active audio devices."
                                : error;
        gAudioDeviceNotice.clear();
        FillDeviceCombo(gUi.destinationCombo, gCableDestinations, {},
                        L"Choose a virtual cable...");
        FillDeviceCombo(gUi.microphoneCombo, gMicrophones, {},
                        L"Choose a microphone...");
        FillDeviceCombo(gUi.monitorCombo, gPhysicalMonitors, {},
                        L"Choose headphones...");
        UpdateSetupGuidance();
        UpdateControlState();
        return;
    }
    gAudioDeviceError.clear();
    gInventory = std::move(inventory);
    BuildFilteredDeviceLists();
    FillDeviceCombo(gUi.destinationCombo, gCableDestinations, destinationId,
                    L"Choose a virtual cable...");
    FillDeviceCombo(gUi.microphoneCombo, gMicrophones, microphoneId,
                    L"Choose a microphone...");
    FillDeviceCombo(gUi.monitorCombo, gPhysicalMonitors, monitorId,
                    L"Choose headphones...");
    if (!destinationId.empty() &&
        FindExactDeviceIndex(gCableDestinations, destinationId) < 0) {
        gAudioDeviceNotice =
            L"The saved cable is missing or is not a recognized generic cable. Choose explicitly.";
    } else if (gCableDestinations.empty()) {
        gAudioDeviceNotice =
            L"No recognized generic virtual cable is active. Install VB-CABLE, then Refresh devices.";
    } else if (destinationId.empty()) {
        gAudioDeviceNotice =
            L"Choose a virtual cable explicitly; ChromeMic never falls back to another endpoint.";
    } else {
        gAudioDeviceNotice.clear();
    }
    UpdateSetupGuidance();
    UpdateControlState();
}

float CurrentGainDb(HWND slider) {
    return static_cast<float>(SendMessageW(slider, TBM_GETPOS, 0, 0)) /
           10.0F;
}

void UpdateGainLabels(bool updateRouter) {
    const float applicationGain = CurrentGainDb(gUi.applicationGain);
    const float microphoneGain = CurrentGainDb(gUi.microphoneGain);
    wchar_t applicationText[32]{};
    wchar_t microphoneText[32]{};
    swprintf_s(applicationText, L"%+.1f dB", applicationGain);
    swprintf_s(microphoneText, L"%+.1f dB", microphoneGain);
    SetWindowTextW(gUi.applicationGainValue, applicationText);
    SetWindowTextW(gUi.microphoneGainValue, microphoneText);
    if (updateRouter) {
        gRouter->SetApplicationGainDb(applicationGain);
        gRouter->SetMicrophoneGainDb(microphoneGain);
    }
}

int MeterPosition(float peak) {
    if (!std::isfinite(peak) || peak <= 0.000001F) {
        return 0;
    }
    const float decibels = 20.0F * std::log10(peak);
    return static_cast<int>(
        std::clamp((decibels + 60.0F) / 60.0F, 0.0F, 1.0F) *
        1000.0F);
}

void UpdateUpdateBanner() {
    SetWindowTextW(gUi.updateStatus, gUpdateBanner.c_str());
    const bool retryVisible = gUpdateGateState == UpdateGateState::Failed;
    const bool updateVisible =
        gUpdateGateState == UpdateGateState::UpdateAvailable;
    ShowWindow(gUi.updateRetry, retryVisible ? SW_SHOW : SW_HIDE);
    ShowWindow(gUi.updateNow, updateVisible ? SW_SHOW : SW_HIDE);
    EnableWindow(gUi.updateRetry,
                 retryVisible && !gUpdateCheckInFlight.load());
    EnableWindow(gUi.updateNow,
                 updateVisible && !gValidatedUpdateUrl.empty());
    InvalidateRect(gUi.updateStatus, nullptr, TRUE);
}

void UpdateControlState() {
    if (gRouter == nullptr || gUi.start == nullptr) {
        return;
    }
    const RouterSnapshot snapshot = gRouter->Snapshot();
    const bool stopped = snapshot.state == RouterState::Stopped;
    const bool active = snapshot.state == RouterState::Running ||
                        snapshot.state == RouterState::Starting;
    const bool pendingStart = gStartAfterUpdateCheck;
    const bool configurationLocked = !stopped || pendingStart;
    const bool microphoneEnabled = IsChecked(gUi.microphoneEnable);
    const bool monitorEnabled = IsChecked(gUi.monitorEnable);
    const bool hasApplication = SelectedApplication() != nullptr;
    const bool hasDestination =
        SelectedDevice(gUi.destinationCombo, gCableDestinations) != nullptr;
    const bool hasMicrophone =
        SelectedDevice(gUi.microphoneCombo, gMicrophones) != nullptr;
    const bool hasMonitor =
        SelectedDevice(gUi.monitorCombo, gPhysicalMonitors) != nullptr;
    const bool configurationComplete =
        hasApplication && hasDestination &&
        (!microphoneEnabled || hasMicrophone) &&
        (!monitorEnabled || (microphoneEnabled && hasMonitor));
    const bool inventoriesHealthy =
        gApplicationError.empty() && gAudioDeviceError.empty();
    const bool updateAllowsRouting =
        gUpdateGateState == UpdateGateState::UpToDate;
    EnableWindow(gUi.applicationCombo, !configurationLocked);
    EnableWindow(gUi.refreshApps, !configurationLocked);
    EnableWindow(gUi.destinationCombo, !configurationLocked);
    EnableWindow(gUi.refreshDevices, !configurationLocked);
    EnableWindow(gUi.getCable, !configurationLocked);
    EnableWindow(gUi.microphoneEnable, !configurationLocked);
    EnableWindow(gUi.microphoneCombo,
                 !configurationLocked && microphoneEnabled);
    EnableWindow(gUi.monitorEnable,
                 !configurationLocked && microphoneEnabled);
    EnableWindow(gUi.monitorCombo,
                 !configurationLocked && microphoneEnabled && monitorEnabled);
    EnableWindow(gUi.applicationGain,
                 !pendingStart && snapshot.state != RouterState::Error);
    EnableWindow(gUi.microphoneGain,
                 !pendingStart && microphoneEnabled &&
                     snapshot.state != RouterState::Error);
    EnableWindow(gUi.effectCombo,
                 !pendingStart && microphoneEnabled &&
                     snapshot.state != RouterState::Error);
    EnableWindow(gUi.mute,
                 !pendingStart && snapshot.state != RouterState::Error);
    EnableWindow(gUi.start, stopped && updateAllowsRouting &&
                                inventoriesHealthy && configurationComplete);
    EnableWindow(gUi.stop,
                 pendingStart || active || snapshot.state == RouterState::Error);
    SetWindowTextW(gUi.stop,
                   pendingStart
                       ? L"Cancel start"
                       : snapshot.state == RouterState::Error
                       ? L"Reset & refresh"
                       : L"Stop");
    UpdateUpdateBanner();
}

void BeginUpdateCheck(bool startAfterSuccess = false) {
    bool expected = false;
    if (!gUpdateCheckInFlight.compare_exchange_strong(expected, true)) {
        if (startAfterSuccess) {
            gStartAfterUpdateCheck = true;
        }
        return;
    }
    gStartAfterUpdateCheck = startAfterSuccess;
    if (gUpdateThread.joinable()) {
        gUpdateThread.join();
    }
    gUpdateGateState = UpdateGateState::Checking;
    gUpdateBanner = L"Checking for updates — routing is locked...";
    gValidatedUpdateUrl.clear();
    UpdateUpdateBanner();
    UpdateControlState();
    try {
        const HWND targetWindow = gMainWindow;
        gUpdateThread = std::thread([targetWindow] {
            UpdateCheckResult result = CheckForChromeMicUpdates();
            {
                std::lock_guard lock(gUpdateResultMutex);
                gPendingUpdateResult = std::move(result);
            }
            gUpdateResultReady.store(true, std::memory_order_release);
            gUpdateCheckInFlight.store(false, std::memory_order_release);
            if (!gIsClosing.load(std::memory_order_acquire)) {
                PostMessageW(targetWindow, kUpdateCheckCompleteMessage, 0, 0);
            }
        });
    } catch (...) {
        gStartAfterUpdateCheck = false;
        gUpdateCheckInFlight.store(false, std::memory_order_release);
        gUpdateGateState = UpdateGateState::Failed;
        gUpdateBanner =
            L"Update check could not start — routing remains locked. Press Retry.";
        UpdateUpdateBanner();
        UpdateControlState();
    }
}

void HandleUpdateCheckComplete() {
    if (!gUpdateResultReady.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    if (gUpdateThread.joinable()) {
        gUpdateThread.join();
    }
    UpdateCheckResult result;
    {
        std::lock_guard lock(gUpdateResultMutex);
        result = std::move(gPendingUpdateResult);
        gPendingUpdateResult = {};
    }
    gValidatedUpdateUrl.clear();
    bool startAfterVerification = false;
    if (result.status == UpdateCheckStatus::UpToDate) {
        gUpdateGateState = UpdateGateState::UpToDate;
        const std::wstring version = AsciiToWide(kChromeMicCurrentVersion);
        gUpdateBanner = L"Version " + version +
                        L" verified — routing is unlocked.";
        startAfterVerification = gStartAfterUpdateCheck;
    } else if (result.status == UpdateCheckStatus::UpdateAvailable) {
        const std::wstring version = AsciiToWide(result.manifest.version);
        const std::wstring url = AsciiToWide(result.manifest.downloadUrl);
        if (!version.empty() && !url.empty()) {
            gUpdateGateState = UpdateGateState::UpdateAvailable;
            gValidatedUpdateUrl = url;
            gUpdateBanner = L"ChromeMic " + version +
                            L" is required before routing.";
        } else {
            gUpdateGateState = UpdateGateState::Failed;
            gUpdateBanner =
                L"The update response was not safe to open — routing remains locked.";
        }
    } else {
        gUpdateGateState = UpdateGateState::Failed;
        gUpdateBanner = L"Update check failed — routing remains locked. ";
        gUpdateBanner += result.message.empty()
                             ? L"Check the connection, then press Retry."
                             : result.message;
    }
    gStartAfterUpdateCheck = false;
    UpdateUpdateBanner();
    UpdateControlState();
    if (startAfterVerification &&
        !gIsClosing.load(std::memory_order_acquire)) {
        StartRoutingVerified();
    }
}

void OpenValidatedUpdate() {
    if (gUpdateGateState != UpdateGateState::UpdateAvailable ||
        gValidatedUpdateUrl.empty()) {
        return;
    }
    const HINSTANCE result = ShellExecuteW(
        gMainWindow, L"open", gValidatedUpdateUrl.c_str(), nullptr, nullptr,
        SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        MessageBoxW(gMainWindow,
                    L"Windows could not open the validated GitHub release URL. "
                    L"Check your default browser and try again.",
                    L"ChromeMic — update", MB_OK | MB_ICONERROR);
    }
}

void RequestStartRouting() {
    if (gUpdateGateState != UpdateGateState::UpToDate) {
        MessageBoxW(gMainWindow,
                    L"ChromeMic must complete the update check and be up to date "
                    L"before routing can start.",
                    L"ChromeMic — update required", MB_OK | MB_ICONWARNING);
        return;
    }
    BeginUpdateCheck(true);
}

void StartRoutingVerified() {
    if (gUpdateGateState != UpdateGateState::UpToDate) {
        MessageBoxW(gMainWindow,
                    L"ChromeMic must complete the update check and be up to date "
                    L"before routing can start.",
                    L"ChromeMic — update required", MB_OK | MB_ICONWARNING);
        return;
    }
    // Exact selections are re-enumerated immediately before opening audio.
    RefreshApplications(true);
    RefreshAudioDevices(true);
    const auto* application = SelectedApplication();
    const auto* destination =
        SelectedDevice(gUi.destinationCombo, gCableDestinations);
    const bool includeMicrophone = IsChecked(gUi.microphoneEnable);
    const bool enableMonitor = IsChecked(gUi.monitorEnable);
    const auto* microphone =
        SelectedDevice(gUi.microphoneCombo, gMicrophones);
    const auto* monitor =
        SelectedDevice(gUi.monitorCombo, gPhysicalMonitors);
    if (application == nullptr || destination == nullptr) {
        MessageBoxW(gMainWindow,
                    L"Choose a running application and a recognized generic virtual cable.",
                    L"ChromeMic — setup incomplete", MB_OK | MB_ICONWARNING);
        return;
    }
    if (!destination->isGenericCable || destination->isVoicemod) {
        MessageBoxW(gMainWindow,
                    L"The destination is not a recognized generic virtual cable. "
                    L"Refresh devices and choose a valid cable playback side.",
                    L"ChromeMic — unsafe destination", MB_OK | MB_ICONERROR);
        return;
    }
    if (includeMicrophone &&
        (microphone == nullptr || microphone->isGenericCable ||
         microphone->isVoicemod)) {
        MessageBoxW(gMainWindow,
                    L"Choose a non-cable microphone, or turn off Include microphone.",
                    L"ChromeMic — microphone required", MB_OK | MB_ICONWARNING);
        return;
    }
    if (enableMonitor &&
        (!includeMicrophone || monitor == nullptr || monitor->isLikelyVirtual ||
         monitor->isGenericCable || monitor->isVoicemod ||
         monitor->id == destination->id)) {
        MessageBoxW(gMainWindow,
                    L"Loopback test requires the microphone and a physical "
                    L"headphone/speaker endpoint that is not the virtual cable.",
                    L"ChromeMic — unsafe monitor", MB_OK | MB_ICONWARNING);
        return;
    }
    RouteConfig config;
    config.sourceProcessId = application->processId;
    config.sourceProcessCreationTime = application->creationTime;
    config.sourceExecutablePath = application->executablePath;
    config.sourceName = BuildApplicationDisplayLabel(*application);
    config.destinationId = destination->id;
    config.destinationName = destination->name;
    config.includeMicrophone = includeMicrophone;
    if (includeMicrophone && microphone != nullptr) {
        config.microphoneId = microphone->id;
        config.microphoneName = microphone->name;
    }
    config.enableMonitor = enableMonitor;
    if (enableMonitor && monitor != nullptr) {
        config.monitorId = monitor->id;
        config.monitorName = monitor->name;
    }
    config.applicationGainDb = CurrentGainDb(gUi.applicationGain);
    config.microphoneGainDb = CurrentGainDb(gUi.microphoneGain);
    config.voiceEffect = SelectedVoiceEffect();
    config.muted = IsChecked(gUi.mute);
    std::wstring error;
    if (!gRouter->Start(config, error)) {
        MessageBoxW(gMainWindow,
                    error.empty() ? L"The audio route could not start."
                                  : error.c_str(),
                    L"ChromeMic could not start", MB_OK | MB_ICONERROR);
        UpdateControlState();
        return;
    }
    SaveSettings();
    UpdateControlState();
}

void StopRouting() {
    const RouterSnapshot snapshot = gRouter->Snapshot();
    if (gStartAfterUpdateCheck && snapshot.state == RouterState::Stopped) {
        gStartAfterUpdateCheck = false;
        gUpdateBanner =
            L"Pending route start cancelled. Update verification continues.";
        UpdateControlState();
        return;
    }
    const bool resetAfterError = snapshot.state == RouterState::Error;
    SetWindowTextW(gUi.status,
                   L"●  STOPPING — releasing capture and output devices...");
    UpdateWindow(gUi.status);
    gRouter->Stop();
    if (resetAfterError) {
        RefreshApplications(true);
        RefreshAudioDevices(true);
    }
    UpdateControlState();
}

void UpdateStatusUi() {
    const RouterSnapshot snapshot = gRouter->Snapshot();
    std::wstring status;
    switch (snapshot.state) {
    case RouterState::Running:
        status = L"●  " + snapshot.message;
        break;
    case RouterState::Starting:
        status = L"●  STARTING — " + snapshot.message;
        break;
    case RouterState::Error:
        status = L"●  ERROR — " + snapshot.message;
        break;
    case RouterState::Stopped:
    default:
        status = (!gApplicationError.empty() || !gAudioDeviceError.empty())
                     ? L"●  SETUP ERROR — refresh the affected list."
                     : L"●  Ready — choose sources and outputs, then Start routing.";
        break;
    }
    SetWindowTextW(gUi.status, status.c_str());
    SendMessageW(gUi.applicationMeter, PBM_SETPOS,
                 MeterPosition(snapshot.applicationPeak), 0);
    SendMessageW(gUi.microphoneMeter, PBM_SETPOS,
                 MeterPosition(snapshot.microphonePeak), 0);
    SendMessageW(gUi.outputMeter, PBM_SETPOS,
                 MeterPosition(snapshot.peak), 0);
    SetWindowTextW(gUi.limiter,
                   snapshot.limiting
                       ? L"LIMITER ACTIVE — lower App or Mic level"
                       : L"Final limiter: −1 dBFS ceiling");
    std::wostringstream metrics;
    if (snapshot.sampleRate != 0) {
        metrics << static_cast<double>(snapshot.sampleRate) / 1000.0
                << L" kHz  •  " << snapshot.channels << L" ch"
                << L"  •  buffer " << snapshot.latencyMilliseconds << L" ms"
                << L"  •  queue " << snapshot.queuedMilliseconds << L" ms";
        if (snapshot.droppedFrames != 0) {
            metrics << L"  •  dropped " << snapshot.droppedFrames;
        }
        if (snapshot.underrunFrames != 0) {
            metrics << L"  •  underrun " << snapshot.underrunFrames;
        }
        if (!snapshot.monitorMessage.empty()) {
            metrics << L"  •  " << snapshot.monitorMessage;
        }
    } else {
        metrics << L"Audio stays in memory; ChromeMic does not record it to disk.";
    }
    SetWindowTextW(gUi.metrics, metrics.str().c_str());
    UpdateControlState();
    InvalidateRect(gUi.status, nullptr, TRUE);
    InvalidateRect(gUi.limiter, nullptr, TRUE);
}

void PopulateEffectCombo() {
    struct EffectEntry {
        const wchar_t* label;
        VoiceEffectMode mode;
    };
    constexpr EffectEntry effects[] = {
        {L"None (clean microphone)", VoiceEffectMode::Natural},
        {L"Clear mic", VoiceEffectMode::ClearMic},
        {L"Broadcast", VoiceEffectMode::Broadcast},
        {L"Radio", VoiceEffectMode::Radio},
        {L"Robot", VoiceEffectMode::Robot},
        {L"Deep tone", VoiceEffectMode::DeepTone},
    };
    int selectedItem = 0;
    for (const auto& effect : effects) {
        const LRESULT item = SendMessageW(
            gUi.effectCombo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(effect.label));
        if (item != CB_ERR && item != CB_ERRSPACE) {
            SendMessageW(gUi.effectCombo, CB_SETITEMDATA,
                         static_cast<WPARAM>(item),
                         static_cast<LPARAM>(effect.mode));
            if (effect.mode == gSavedSettings.voiceEffect) {
                selectedItem = static_cast<int>(item);
            }
        }
    }
    SendMessageW(gUi.effectCombo, CB_SETCURSEL, selectedItem, 0);
}

void ConfigureMeter(HWND meter, COLORREF color) {
    SendMessageW(meter, PBM_SETRANGE32, 0, 1000);
    SendMessageW(meter, PBM_SETBARCOLOR, 0, color);
    SendMessageW(meter, PBM_SETBKCOLOR, 0, kMeterBackground);
    SetWindowTheme(meter, L"", L"");
}

void LayoutControls() {
    if (gMainWindow == nullptr || gUi.title == nullptr) {
        return;
    }
    RECT client{};
    GetClientRect(gMainWindow, &client);
    const int width = std::max(kMinimumClientWidth, UnscaleUi(client.right));
    const int clientHeight =
        std::max(1, UnscaleUi(client.bottom - client.top));
    const int height = std::max(kContentHeight, clientHeight);
    const int maximumScroll = std::max(0, height - clientHeight);
    gVerticalScroll = std::clamp(gVerticalScroll, 0, maximumScroll);
    gLayoutOffsetY = -gVerticalScroll;
    SCROLLINFO scrollInfo{};
    scrollInfo.cbSize = sizeof(scrollInfo);
    scrollInfo.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    scrollInfo.nMin = 0;
    scrollInfo.nMax = height - 1;
    scrollInfo.nPage = static_cast<UINT>(clientHeight);
    scrollInfo.nPos = gVerticalScroll;
    SetScrollInfo(gMainWindow, SB_VERT, &scrollInfo, TRUE);
    const int left = 24;
    const int right = width - 24;
    const int contentWidth = right - left;
    const int innerLeft = 44;
    const int innerRight = width - 44;
    const int innerWidth = innerRight - innerLeft;
    MoveControl(gUi.title, left, 16, 320, 34);
    MoveControl(gUi.subtitle, left, 51, contentWidth, 22);
    MoveControl(gUi.updateGroup, left, 78, contentWidth, 68);
    MoveControl(gUi.updateStatus, innerLeft, 89, innerWidth - 154, 52);
    MoveControl(gUi.updateRetry, innerRight - 100, 97, 100, 30);
    MoveControl(gUi.updateNow, innerRight - 128, 97, 128, 30);
    MoveControl(gUi.sourcesGroup, left, 156, contentWidth, 288);
    MoveControl(gUi.applicationLabel, innerLeft, 178, innerWidth, 20);
    MoveControl(gUi.applicationCombo, innerLeft, 199, innerWidth - 144, 260);
    MoveControl(gUi.refreshApps, innerRight - 132, 198, 132, 30);
    const int meterWidth = std::max(180, innerWidth / 4);
    const int gainSliderWidth =
        std::max(260, innerWidth - 104 - 76 - 54 - meterWidth - 28);
    const int gainSliderX = innerLeft + 96;
    const int gainValueX = gainSliderX + gainSliderWidth + 4;
    const int meterLabelX = gainValueX + 72;
    const int meterX = meterLabelX + 44;
    const int actualMeterWidth = std::max(100, innerRight - meterX);
    MoveControl(gUi.applicationGainLabel, innerLeft, 240, 92, 22);
    MoveControl(gUi.applicationGain, gainSliderX, 233, gainSliderWidth, 34);
    MoveControl(gUi.applicationGainValue, gainValueX, 238, 68, 24);
    MoveControl(gUi.applicationMeterLabel, meterLabelX, 240, 40, 20);
    MoveControl(gUi.applicationMeter, meterX, 242, actualMeterWidth, 15);
    MoveControl(gUi.microphoneEnable, innerLeft, 272, innerWidth, 24);
    MoveControl(gUi.microphoneCombo, innerLeft, 299, innerWidth - 144, 260);
    MoveControl(gUi.refreshDevices, innerRight - 132, 298, 132, 30);
    MoveControl(gUi.microphoneGainLabel, innerLeft, 340, 92, 22);
    MoveControl(gUi.microphoneGain, gainSliderX, 333, gainSliderWidth, 34);
    MoveControl(gUi.microphoneGainValue, gainValueX, 338, 68, 24);
    MoveControl(gUi.microphoneMeterLabel, meterLabelX, 340, 40, 20);
    MoveControl(gUi.microphoneMeter, meterX, 342, actualMeterWidth, 15);
    MoveControl(gUi.effectLabel, innerLeft, 385, 98, 22);
    MoveControl(gUi.effectCombo, innerLeft + 96, 379, 300, 220);
    MoveControl(gUi.effectHelp, innerLeft + 408, 385,
                std::max(120, innerRight - (innerLeft + 408)), 22);
    MoveControl(gUi.outputsGroup, left, 456, contentWidth, 178);
    MoveControl(gUi.destinationLabel, innerLeft, 478, innerWidth, 20);
    MoveControl(gUi.destinationCombo, innerLeft, 499, innerWidth - 144, 240);
    MoveControl(gUi.getCable, innerRight - 132, 498, 132, 30);
    MoveControl(gUi.gameMicrophone, innerLeft, 535, innerWidth, 24);
    MoveControl(gUi.monitorEnable, innerLeft, 564, innerWidth, 24);
    MoveControl(gUi.monitorCombo, innerLeft, 590, innerWidth - 284, 220);
    MoveControl(gUi.soundSettings, innerRight - 272, 589, 128, 30);
    MoveControl(gUi.microphonePrivacy, innerRight - 132, 589, 132, 30);
    const int routeTop = 644;
    MoveControl(gUi.routeGroup, left, routeTop, contentWidth,
                height - routeTop - 12);
    MoveControl(gUi.outputMeterLabel, innerLeft, routeTop + 23, 104, 20);
    MoveControl(gUi.outputMeter, innerLeft + 104, routeTop + 23,
                innerWidth - 376, 17);
    MoveControl(gUi.limiter, innerRight - 260, routeTop + 21, 260, 22);
    const int privacyY = height - 48;
    const int setupY = privacyY - 24;
    const int metricsY = setupY - 24;
    const int statusY = metricsY - 29;
    const int buttonsY = std::max(routeTop + 52, statusY - 46);
    MoveControl(gUi.start, innerLeft, buttonsY, 190, 38);
    MoveControl(gUi.stop, innerLeft + 202, buttonsY, 148, 38);
    MoveControl(gUi.mute, innerLeft + 370, buttonsY + 6, 180, 28);
    MoveControl(gUi.status, innerLeft, statusY, innerWidth, 25);
    MoveControl(gUi.metrics, innerLeft, metricsY, innerWidth, 23);
    MoveControl(gUi.setup, innerLeft, setupY, innerWidth, 22);
    MoveControl(gUi.privacy, innerLeft, privacyY, innerWidth, 36);
}

void ScrollTo(int logicalPosition) {
    if (gMainWindow == nullptr) {
        return;
    }
    RECT client{};
    GetClientRect(gMainWindow, &client);
    const int clientHeight =
        std::max(1, UnscaleUi(client.bottom - client.top));
    const int maximumScroll = std::max(0, kContentHeight - clientHeight);
    const int clamped = std::clamp(logicalPosition, 0, maximumScroll);
    if (clamped != gVerticalScroll) {
        gVerticalScroll = clamped;
        LayoutControls();
        InvalidateRect(gMainWindow, nullptr, TRUE);
    }
}

void EnsureFocusedControlVisible(HWND control) {
    if (control == nullptr || control == gMainWindow ||
        !IsChild(gMainWindow, control)) {
        return;
    }
    RECT controlRectangle{};
    RECT clientRectangle{};
    if (!GetWindowRect(control, &controlRectangle) ||
        !GetClientRect(gMainWindow, &clientRectangle)) {
        return;
    }
    MapWindowPoints(nullptr, gMainWindow,
                    reinterpret_cast<POINT*>(&controlRectangle), 2);
    constexpr int kFocusMargin = 8;
    const int controlTop = UnscaleUi(controlRectangle.top);
    const int controlBottom = UnscaleUi(controlRectangle.bottom);
    const int clientBottom = UnscaleUi(clientRectangle.bottom);
    if (controlTop < kFocusMargin) {
        ScrollTo(gVerticalScroll + controlTop - kFocusMargin);
    } else if (controlBottom > clientBottom - kFocusMargin) {
        ScrollTo(gVerticalScroll + controlBottom - clientBottom +
                 kFocusMargin);
    }
}

bool CreateMainControls() {
    gTitleFont = CreateUiFont(22, FW_SEMIBOLD);
    gHeadingFont = CreateUiFont(10, FW_SEMIBOLD);
    gBodyFont = CreateUiFont(10, FW_NORMAL);
    gSmallFont = CreateUiFont(9, FW_NORMAL);
    gUi.title = MakeControl(L"STATIC", L"ChromeMic 1.2", SS_LEFT,
                            0, gTitleFont);
    gUi.subtitle = MakeControl(
        L"STATIC", L"Mix one application + optional microphone → game mic",
        SS_LEFT | SS_NOPREFIX, 0, gSmallFont);
    gUi.updateGroup = MakeControl(L"BUTTON", L"Update gate",
                                  BS_GROUPBOX, 0, gHeadingFont);
    gUi.updateStatus = MakeControl(L"STATIC", gUpdateBanner.c_str(),
                                   SS_LEFT | SS_NOPREFIX, 0, gBodyFont);
    gUi.updateRetry = MakeControl(L"BUTTON", L"Retry",
                                  BS_PUSHBUTTON | WS_TABSTOP,
                                  IDC_UPDATE_RETRY, gBodyFont);
    gUi.updateNow = MakeControl(L"BUTTON", L"Download update",
                                BS_PUSHBUTTON | WS_TABSTOP,
                                IDC_UPDATE_NOW, gBodyFont);
    gUi.sourcesGroup = MakeControl(L"BUTTON", L"1 · Sources",
                                   BS_GROUPBOX, 0, gHeadingFont);
    gUi.applicationLabel = MakeControl(
        L"STATIC", L"Application audio (Chrome selection is browser-wide)",
        SS_LEFT | SS_NOPREFIX, 0, gHeadingFont);
    gUi.applicationCombo = MakeControl(
        WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
        IDC_APPLICATION, gBodyFont);
    gUi.refreshApps = MakeControl(L"BUTTON", L"Refresh apps",
                                  BS_PUSHBUTTON | WS_TABSTOP,
                                  IDC_REFRESH_APPS, gBodyFont);
    gUi.applicationGainLabel = MakeControl(
        L"STATIC", L"App level", SS_LEFT, 0, gBodyFont);
    gUi.applicationGain = MakeControl(
        TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP,
        IDC_APP_GAIN, gBodyFont);
    gUi.applicationGainValue = MakeControl(
        L"STATIC", L"+0.0 dB", SS_CENTER, IDC_APP_GAIN_VALUE, gHeadingFont);
    gUi.applicationMeterLabel = MakeControl(
        L"STATIC", L"App", SS_LEFT, 0, gSmallFont);
    gUi.applicationMeter = MakeControl(
        PROGRESS_CLASSW, L"", PBS_SMOOTH, IDC_APP_METER, gBodyFont);
    gUi.microphoneEnable = MakeControl(
        L"BUTTON", L"Include microphone", BS_AUTOCHECKBOX | WS_TABSTOP,
        IDC_MIC_ENABLE, gHeadingFont);
    gUi.microphoneCombo = MakeControl(
        WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
        IDC_MICROPHONE, gBodyFont);
    gUi.refreshDevices = MakeControl(
        L"BUTTON", L"Refresh devices", BS_PUSHBUTTON | WS_TABSTOP,
        IDC_REFRESH_DEVICES, gBodyFont);
    gUi.microphoneGainLabel = MakeControl(
        L"STATIC", L"Mic level", SS_LEFT, 0, gBodyFont);
    gUi.microphoneGain = MakeControl(
        TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP,
        IDC_MIC_GAIN, gBodyFont);
    gUi.microphoneGainValue = MakeControl(
        L"STATIC", L"+0.0 dB", SS_CENTER, IDC_MIC_GAIN_VALUE, gHeadingFont);
    gUi.microphoneMeterLabel = MakeControl(
        L"STATIC", L"Mic", SS_LEFT, 0, gSmallFont);
    gUi.microphoneMeter = MakeControl(
        PROGRESS_CLASSW, L"", PBS_SMOOTH, IDC_MIC_METER, gBodyFont);
    gUi.effectLabel = MakeControl(L"STATIC", L"Voice effect", SS_LEFT,
                                  0, gBodyFont);
    gUi.effectCombo = MakeControl(
        WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
        IDC_VOICE_EFFECT, gBodyFont);
    gUi.effectHelp = MakeControl(
        L"STATIC", L"Affects the microphone only.", SS_LEFT, 0, gSmallFont);
    gUi.outputsGroup = MakeControl(L"BUTTON", L"2 · Outputs",
                                   BS_GROUPBOX, 0, gHeadingFont);
    gUi.destinationLabel = MakeControl(
        L"STATIC", L"Game cable (playback side)", SS_LEFT, 0, gHeadingFont);
    gUi.destinationCombo = MakeControl(
        WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
        IDC_DESTINATION, gBodyFont);
    gUi.getCable = MakeControl(L"BUTTON", L"Get VB-CABLE",
                               BS_PUSHBUTTON | WS_TABSTOP,
                               IDC_GET_CABLE, gBodyFont);
    gUi.gameMicrophone = MakeControl(
        L"STATIC", L"Choose a virtual cable playback side first.",
        SS_LEFT | SS_NOPREFIX, IDC_GAME_MIC, gSmallFont);
    gUi.monitorEnable = MakeControl(
        L"BUTTON", L"Loopback test — hear microphone/effect in headphones",
        BS_AUTOCHECKBOX | WS_TABSTOP, IDC_MONITOR_ENABLE, gHeadingFont);
    gUi.monitorCombo = MakeControl(
        WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
        IDC_MONITOR, gBodyFont);
    gUi.soundSettings = MakeControl(
        L"BUTTON", L"Sound settings", BS_PUSHBUTTON | WS_TABSTOP,
        IDC_SOUND_SETTINGS, gBodyFont);
    gUi.microphonePrivacy = MakeControl(
        L"BUTTON", L"Mic privacy", BS_PUSHBUTTON | WS_TABSTOP,
        IDC_MIC_PRIVACY, gBodyFont);
    gUi.routeGroup = MakeControl(L"BUTTON", L"3 · Route",
                                 BS_GROUPBOX, 0, gHeadingFont);
    gUi.outputMeterLabel = MakeControl(
        L"STATIC", L"Game output", SS_LEFT, 0, gBodyFont);
    gUi.outputMeter = MakeControl(
        PROGRESS_CLASSW, L"", PBS_SMOOTH, IDC_OUTPUT_METER, gBodyFont);
    gUi.limiter = MakeControl(
        L"STATIC", L"Final limiter: −1 dBFS ceiling", SS_RIGHT,
        IDC_LIMITER, gSmallFont);
    gUi.start = MakeControl(
        L"BUTTON", L"Start routing", BS_DEFPUSHBUTTON | WS_TABSTOP,
        IDC_START, gHeadingFont);
    gUi.stop = MakeControl(L"BUTTON", L"Stop", BS_PUSHBUTTON | WS_TABSTOP,
                           IDC_STOP, gHeadingFont);
    gUi.mute = MakeControl(
        L"BUTTON", L"Mute game output", BS_AUTOCHECKBOX | WS_TABSTOP,
        IDC_MUTE, gBodyFont);
    gUi.status = MakeControl(
        L"STATIC", L"●  Ready — routing is stopped", SS_LEFT | SS_NOPREFIX,
        IDC_STATUS, gHeadingFont);
    gUi.metrics = MakeControl(
        L"STATIC", L"Audio stays in memory; ChromeMic does not record it to disk.",
        SS_LEFT | SS_NOPREFIX, IDC_METRICS, gSmallFont);
    gUi.setup = MakeControl(L"STATIC", L"", SS_LEFT | SS_NOPREFIX,
                            IDC_SETUP, gSmallFont);
    gUi.privacy = MakeControl(
        L"STATIC",
        L"Use headphones for loopback testing; speakers can feed back into the mic. "
        L"At launch, only the update manifest is requested from GitHub—no audio or device names are sent.",
        SS_LEFT | SS_NOPREFIX, 0, gSmallFont);
    const HWND essential[] = {
        gUi.title, gUi.updateStatus, gUi.applicationCombo, gUi.refreshApps,
        gUi.applicationGain, gUi.applicationMeter, gUi.microphoneEnable,
        gUi.microphoneCombo, gUi.refreshDevices, gUi.microphoneGain,
        gUi.microphoneMeter, gUi.effectCombo, gUi.destinationCombo,
        gUi.monitorEnable, gUi.monitorCombo, gUi.outputMeter, gUi.start,
        gUi.stop, gUi.status};
    if (std::any_of(std::begin(essential), std::end(essential),
                    [](HWND control) { return control == nullptr; })) {
        return false;
    }
    SendMessageW(gUi.applicationGain, TBM_SETRANGE, TRUE, MAKELONG(-240, 60));
    SendMessageW(gUi.applicationGain, TBM_SETTICFREQ, 50, 0);
    SendMessageW(gUi.applicationGain, TBM_SETPOS, TRUE,
                 gSavedSettings.applicationGainTenths);
    SendMessageW(gUi.microphoneGain, TBM_SETRANGE, TRUE, MAKELONG(-240, 60));
    SendMessageW(gUi.microphoneGain, TBM_SETTICFREQ, 50, 0);
    SendMessageW(gUi.microphoneGain, TBM_SETPOS, TRUE,
                 gSavedSettings.microphoneGainTenths);
    SendMessageW(gUi.microphoneEnable, BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageW(gUi.monitorEnable, BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageW(gUi.mute, BM_SETCHECK, BST_UNCHECKED, 0);
    PopulateEffectCombo();
    ConfigureMeter(gUi.applicationMeter, kBlue);
    ConfigureMeter(gUi.microphoneMeter, kGreen);
    ConfigureMeter(gUi.outputMeter, kGreen);
    SetWindowTheme(gUi.applicationCombo, L"Explorer", nullptr);
    SetWindowTheme(gUi.microphoneCombo, L"Explorer", nullptr);
    SetWindowTheme(gUi.effectCombo, L"Explorer", nullptr);
    SetWindowTheme(gUi.destinationCombo, L"Explorer", nullptr);
    SetWindowTheme(gUi.monitorCombo, L"Explorer", nullptr);
    UpdateGainLabels(true);
    UpdateUpdateBanner();
    LayoutControls();
    return true;
}

LRESULT OnControlColor(HDC dc, HWND control) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, kText);
    if (control == gUi.subtitle || control == gUi.effectHelp ||
        control == gUi.metrics || control == gUi.privacy ||
        control == gUi.applicationMeterLabel ||
        control == gUi.microphoneMeterLabel) {
        SetTextColor(dc, kMutedText);
    } else if (control == gUi.gameMicrophone) {
        SetTextColor(dc, kBlue);
    } else if (control == gUi.setup) {
        SetTextColor(dc, (!gApplicationError.empty() ||
                          !gAudioDeviceError.empty())
                             ? kRed
                             : kOrange);
    } else if (control == gUi.updateStatus) {
        switch (gUpdateGateState) {
        case UpdateGateState::UpToDate:
            SetTextColor(dc, kGreen);
            break;
        case UpdateGateState::Checking:
            SetTextColor(dc, kBlue);
            break;
        case UpdateGateState::UpdateAvailable:
            SetTextColor(dc, kOrange);
            break;
        case UpdateGateState::Failed:
            SetTextColor(dc, kRed);
            break;
        }
    } else if (control == gUi.limiter && gRouter != nullptr) {
        SetTextColor(dc, gRouter->Snapshot().limiting ? kOrange : kMutedText);
    } else if (control == gUi.status && gRouter != nullptr) {
        const RouterSnapshot snapshot = gRouter->Snapshot();
        if (snapshot.state == RouterState::Running) {
            SetTextColor(dc, snapshot.muted ? kOrange : kGreen);
        } else if (snapshot.state == RouterState::Starting) {
            SetTextColor(dc, kBlue);
        } else if (snapshot.state == RouterState::Error) {
            SetTextColor(dc, kRed);
        }
    }
    return reinterpret_cast<LRESULT>(gBackgroundBrush);
}

void ClearSavedSelectionForUserChoice(int controlId) {
    switch (controlId) {
    case IDC_APPLICATION:
        if (SelectedApplication() == nullptr) {
            gSavedSettings.sourceApplicationPath.clear();
        }
        break;
    case IDC_DESTINATION:
        if (SelectedDevice(gUi.destinationCombo, gCableDestinations) == nullptr) {
            gSavedSettings.destinationId.clear();
        }
        break;
    case IDC_MICROPHONE:
        if (SelectedDevice(gUi.microphoneCombo, gMicrophones) == nullptr) {
            gSavedSettings.microphoneId.clear();
        }
        break;
    case IDC_MONITOR:
        if (SelectedDevice(gUi.monitorCombo, gPhysicalMonitors) == nullptr) {
            gSavedSettings.monitorId.clear();
        }
        break;
    default:
        break;
    }
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam,
                                 LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        gMainWindow = window;
        if (!CreateMainControls()) {
            return -1;
        }
        RefreshApplications(false);
        RefreshAudioDevices(false);
        UpdateControlState();
        SetTimer(window, kStatusTimer, 100, nullptr);
        BeginUpdateCheck(false);
        return 0;
    case kUpdateCheckCompleteMessage:
        HandleUpdateCheckComplete();
        return 0;
    case WM_SIZE:
        LayoutControls();
        return 0;
    case WM_VSCROLL: {
        SCROLLINFO scrollInfo{};
        scrollInfo.cbSize = sizeof(scrollInfo);
        scrollInfo.fMask = SIF_ALL;
        GetScrollInfo(window, SB_VERT, &scrollInfo);
        int position = gVerticalScroll;
        switch (LOWORD(wParam)) {
        case SB_TOP:
            position = 0;
            break;
        case SB_BOTTOM:
            position = scrollInfo.nMax;
            break;
        case SB_LINEUP:
            position -= 30;
            break;
        case SB_LINEDOWN:
            position += 30;
            break;
        case SB_PAGEUP:
            position -= static_cast<int>(scrollInfo.nPage);
            break;
        case SB_PAGEDOWN:
            position += static_cast<int>(scrollInfo.nPage);
            break;
        case SB_THUMBPOSITION:
        case SB_THUMBTRACK:
            position = scrollInfo.nTrackPos;
            break;
        default:
            break;
        }
        ScrollTo(position);
        return 0;
    }
    case WM_MOUSEWHEEL:
        ScrollTo(gVerticalScroll -
                 GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA * 60);
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        const int notification = HIWORD(wParam);
        if (id == IDC_UPDATE_RETRY && notification == BN_CLICKED) {
            BeginUpdateCheck(false);
        } else if (id == IDC_UPDATE_NOW && notification == BN_CLICKED) {
            OpenValidatedUpdate();
        } else if (id == IDC_REFRESH_APPS && notification == BN_CLICKED) {
            RefreshApplications(true);
        } else if (id == IDC_REFRESH_DEVICES && notification == BN_CLICKED) {
            RefreshAudioDevices(true);
        } else if (id == IDC_START && notification == BN_CLICKED) {
            RequestStartRouting();
        } else if (id == IDC_STOP && notification == BN_CLICKED) {
            StopRouting();
        } else if (id == IDC_MUTE && notification == BN_CLICKED) {
            gRouter->SetMuted(IsChecked(gUi.mute));
        } else if (id == IDC_MIC_ENABLE && notification == BN_CLICKED) {
            if (!IsChecked(gUi.microphoneEnable)) {
                SendMessageW(gUi.monitorEnable, BM_SETCHECK, BST_UNCHECKED, 0);
            }
            UpdateSetupGuidance();
            UpdateControlState();
        } else if (id == IDC_MONITOR_ENABLE && notification == BN_CLICKED) {
            UpdateSetupGuidance();
            UpdateControlState();
        } else if (id == IDC_VOICE_EFFECT && notification == CBN_SELCHANGE) {
            gRouter->SetVoiceEffect(SelectedVoiceEffect());
            SaveSettings();
        } else if ((id == IDC_APPLICATION || id == IDC_DESTINATION ||
                    id == IDC_MICROPHONE || id == IDC_MONITOR) &&
                   notification == CBN_SELCHANGE) {
            ClearSavedSelectionForUserChoice(id);
            gApplicationNotice.clear();
            gAudioDeviceNotice.clear();
            UpdateSetupGuidance();
            SaveSettings();
            UpdateControlState();
        } else if (id == IDC_SOUND_SETTINGS && notification == BN_CLICKED) {
            ShellExecuteW(window, L"open", L"ms-settings:sound", nullptr,
                          nullptr, SW_SHOWNORMAL);
        } else if (id == IDC_MIC_PRIVACY && notification == BN_CLICKED) {
            ShellExecuteW(window, L"open", L"ms-settings:privacy-microphone",
                          nullptr, nullptr, SW_SHOWNORMAL);
        } else if (id == IDC_GET_CABLE && notification == BN_CLICKED) {
            ShellExecuteW(window, L"open",
                          L"https://vb-audio.com/Cable/index.htm", nullptr,
                          nullptr, SW_SHOWNORMAL);
        }
        return 0;
    }
    case WM_HSCROLL:
        if (reinterpret_cast<HWND>(lParam) == gUi.applicationGain ||
            reinterpret_cast<HWND>(lParam) == gUi.microphoneGain) {
            UpdateGainLabels(true);
            SaveSettings();
        }
        return 0;
    case WM_TIMER:
        if (wParam == kStatusTimer &&
            !gIsClosing.load(std::memory_order_acquire)) {
            if (gUpdateResultReady.load(std::memory_order_acquire)) {
                HandleUpdateCheckComplete();
            }
            UpdateStatusUi();
        }
        return 0;
    case WM_CTLCOLORSTATIC:
        return OnControlColor(reinterpret_cast<HDC>(wParam),
                              reinterpret_cast<HWND>(lParam));
    case WM_ERASEBKGND: {
        RECT rectangle{};
        GetClientRect(window, &rectangle);
        FillRect(reinterpret_cast<HDC>(wParam), &rectangle, gBackgroundBrush);
        return 1;
    }
    case WM_GETMINMAXINFO: {
        auto* information = reinterpret_cast<MINMAXINFO*>(lParam);
        RECT minimum{0, 0, ScaleUi(kMinimumClientWidth),
                     ScaleUi(kMinimumClientHeight)};
        AdjustWindowRectExForDpi(&minimum, kMainWindowStyle, FALSE, 0, gUiDpi);
        information->ptMinTrackSize.x = minimum.right - minimum.left;
        information->ptMinTrackSize.y = minimum.bottom - minimum.top;
        return 0;
    }
    case WM_CLOSE:
        if (!gIsClosing.exchange(true, std::memory_order_acq_rel)) {
            KillTimer(window, kStatusTimer);
            ShowWindow(window, SW_HIDE);
            SaveSettings();
            gRouter->Stop();
            if (gUpdateThread.joinable()) {
                gUpdateThread.join();
            }
        }
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        gIsClosing.store(true, std::memory_order_release);
        if (gUpdateThread.joinable()) {
            gUpdateThread.join();
        }
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

void CleanupUiResources() {
    DeleteObject(gTitleFont);
    DeleteObject(gHeadingFont);
    DeleteObject(gBodyFont);
    DeleteObject(gSmallFont);
    DeleteObject(gBackgroundBrush);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    gInstance = instance;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    gUiDpi = GetDpiForSystem();
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comResult)) {
        MessageBoxW(nullptr, L"Windows audio initialization failed.",
                    L"ChromeMic", MB_OK | MB_ICONERROR);
        return 1;
    }
    INITCOMMONCONTROLSEX commonControls{
        sizeof(commonControls),
        ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&commonControls);
    gBackgroundBrush = CreateSolidBrush(kBackground);
    gRouter = std::make_unique<AudioRouter>();
    gSavedSettings = LoadSettings();
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hbrBackground = gBackgroundBrush;
    windowClass.lpszClassName = kWindowClass;
    if (RegisterClassExW(&windowClass) == 0) {
        CleanupUiResources();
        CoUninitialize();
        return 1;
    }
    RECT desired{0, 0, ScaleUi(kInitialClientWidth),
                 ScaleUi(kInitialClientHeight)};
    AdjustWindowRectExForDpi(&desired, kMainWindowStyle, FALSE, 0, gUiDpi);
    HWND window = CreateWindowExW(
        0, kWindowClass,
        L"ChromeMic 1.2 — application + microphone to game cable",
        kMainWindowStyle, CW_USEDEFAULT, CW_USEDEFAULT,
        desired.right - desired.left, desired.bottom - desired.top,
        nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
        gRouter.reset();
        CleanupUiResources();
        CoUninitialize();
        return 1;
    }
    BOOL darkTitle = FALSE;
    DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE,
                          &darkTitle, sizeof(darkTitle));
    ShowWindow(window, showCommand);
    UpdateWindow(window);
    MSG message{};
    HWND previousFocus = nullptr;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        const HWND focusedControl = GetFocus();
        if (focusedControl != previousFocus) {
            EnsureFocusedControlVisible(focusedControl);
            previousFocus = focusedControl;
        }
    }
    gRouter.reset();
    CleanupUiResources();
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
