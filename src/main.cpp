#include "app_processes.h"
#include "audio_devices.h"
#include "audio_router.h"
#include "dsp.h"

#include <Windows.h>
#include <Objbase.h>
#include <CommCtrl.h>
#include <Dwmapi.h>
#include <Shellapi.h>
#include <Uxtheme.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <memory>
#include <sstream>
#include <string>

namespace {

constexpr wchar_t kWindowClass[] = L"ChromeMicMainWindow";
constexpr wchar_t kRegistryPath[] = L"Software\\ChromeMic";
constexpr UINT_PTR kStatusTimer = 1;

constexpr int IDC_SOURCE = 1001;
constexpr int IDC_DESTINATION = 1002;
constexpr int IDC_REFRESH = 1003;
constexpr int IDC_GAIN = 1004;
constexpr int IDC_GAIN_LABEL = 1005;
constexpr int IDC_MUTE = 1006;
constexpr int IDC_START = 1007;
constexpr int IDC_STOP = 1008;
constexpr int IDC_METER = 1009;
constexpr int IDC_STATUS = 1010;
constexpr int IDC_METRICS = 1011;
constexpr int IDC_GAME_MIC = 1012;
constexpr int IDC_WARNING = 1013;
constexpr int IDC_SOUND_SETTINGS = 1014;
constexpr int IDC_LIMITER = 1015;
constexpr int IDC_GET_CABLE = 1016;

constexpr COLORREF kBackground = RGB(245, 247, 251);
constexpr COLORREF kText = RGB(26, 31, 44);
constexpr COLORREF kMutedText = RGB(91, 101, 122);
constexpr COLORREF kGreen = RGB(16, 154, 91);
constexpr COLORREF kRed = RGB(208, 54, 64);
constexpr COLORREF kOrange = RGB(184, 104, 22);
constexpr COLORREF kBlue = RGB(55, 94, 246);

struct SavedSettings {
    std::wstring sourceApplicationPath;
    std::wstring destinationId;
    int gainTenths = 0;
};

HINSTANCE gInstance = nullptr;
HWND gMainWindow = nullptr;
HWND gSourceCombo = nullptr;
HWND gDestinationCombo = nullptr;
HWND gRefreshButton = nullptr;
HWND gGainSlider = nullptr;
HWND gGainLabel = nullptr;
HWND gMuteCheck = nullptr;
HWND gStartButton = nullptr;
HWND gStopButton = nullptr;
HWND gMeter = nullptr;
HWND gStatus = nullptr;
HWND gMetrics = nullptr;
HWND gGameMic = nullptr;
HWND gWarning = nullptr;
HWND gSoundSettings = nullptr;
HWND gLimiter = nullptr;
HWND gGetCable = nullptr;
HFONT gTitleFont = nullptr;
HFONT gHeadingFont = nullptr;
HFONT gBodyFont = nullptr;
HFONT gSmallFont = nullptr;
HBRUSH gBackgroundBrush = nullptr;

AudioDeviceInventory gInventory;
std::vector<AppProcessInfo> gApplications;
std::unique_ptr<AudioRouter> gRouter;
SavedSettings gSavedSettings;
bool gIsClosing = false;
UINT gUiDpi = 96;
std::wstring gDeviceError;
std::wstring gDeviceNotice;

void SetControlsRouting(bool routing);

int ScaleUi(int logicalPixels) {
    return MulDiv(logicalPixels, static_cast<int>(gUiDpi), 96);
}

std::wstring Lowercase(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

bool ContainsIgnoringCase(const std::wstring& value, const std::wstring& fragment) {
    return Lowercase(value).find(Lowercase(fragment)) != std::wstring::npos;
}

HFONT CreateUiFont(int pointSize, int weight) {
    const int height = -MulDiv(pointSize, static_cast<int>(gUiDpi), 72);
    return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void ApplyFont(HWND control, HFONT font) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

HWND MakeControl(const wchar_t* className, const wchar_t* text, DWORD style,
                 int x, int y, int width, int height, int id, HFONT font = nullptr) {
    HWND control = CreateWindowExW(0, className, text, style | WS_CHILD | WS_VISIBLE,
                                   ScaleUi(x), ScaleUi(y), ScaleUi(width), ScaleUi(height), gMainWindow,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), gInstance, nullptr);
    if (control != nullptr && font != nullptr) {
        ApplyFont(control, font);
    }
    return control;
}

std::wstring ReadRegistryString(HKEY key, const wchar_t* name) {
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        type != REG_SZ || bytes == 0 || bytes > 16384 || bytes % sizeof(wchar_t) != 0) {
        return {};
    }
    std::wstring value(bytes / sizeof(wchar_t) + 1, L'\0');
    DWORD actualBytes = static_cast<DWORD>(value.size() * sizeof(wchar_t));
    if (RegQueryValueExW(key, name, nullptr, nullptr, reinterpret_cast<BYTE*>(value.data()), &actualBytes) != ERROR_SUCCESS ||
        actualBytes % sizeof(wchar_t) != 0 || actualBytes > bytes) {
        return {};
    }
    value.resize(actualBytes / sizeof(wchar_t));
    while (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    return value;
}

SavedSettings LoadSettings() {
    SavedSettings settings;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return settings;
    }
    settings.sourceApplicationPath = ReadRegistryString(key, L"SourceApplicationPath");
    settings.destinationId = ReadRegistryString(key, L"DestinationEndpointId");
    DWORD type = 0;
    DWORD bytes = sizeof(DWORD);
    DWORD gain = 0;
    if (RegQueryValueExW(key, L"GainTenths", nullptr, &type, reinterpret_cast<BYTE*>(&gain), &bytes) == ERROR_SUCCESS &&
        type == REG_DWORD) {
        settings.gainTenths = std::clamp(static_cast<int>(gain), -120, 60);
    }
    RegCloseKey(key);
    return settings;
}

void WriteRegistryString(HKEY key, const wchar_t* name, const std::wstring& value) {
    const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), bytes);
}

const AudioDeviceInfo* SelectedDevice(HWND combo, const std::vector<AudioDeviceInfo>& devices) {
    const LRESULT selection = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (selection == CB_ERR) {
        return nullptr;
    }
    const LRESULT index = SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(selection), 0);
    if (index == CB_ERR || index < 0 || static_cast<size_t>(index) >= devices.size()) {
        return nullptr;
    }
    return &devices[static_cast<size_t>(index)];
}

const AppProcessInfo* SelectedApplication() {
    const LRESULT selection = SendMessageW(gSourceCombo, CB_GETCURSEL, 0, 0);
    if (selection == CB_ERR) {
        return nullptr;
    }
    const LRESULT index = SendMessageW(gSourceCombo, CB_GETITEMDATA, static_cast<WPARAM>(selection), 0);
    if (index == CB_ERR || index < 0 || static_cast<size_t>(index) >= gApplications.size()) {
        return nullptr;
    }
    return &gApplications[static_cast<size_t>(index)];
}

void SaveSettings() {
    HKEY key = nullptr;
    DWORD disposition = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, &disposition) != ERROR_SUCCESS) {
        return;
    }
    if (const auto* source = SelectedApplication()) {
        WriteRegistryString(key, L"SourceApplicationPath", source->executablePath);
        gSavedSettings.sourceApplicationPath = source->executablePath;
    }
    if (const auto* destination = SelectedDevice(gDestinationCombo, gInventory.playback)) {
        WriteRegistryString(key, L"DestinationEndpointId", destination->id);
        gSavedSettings.destinationId = destination->id;
    }
    const DWORD gain = static_cast<DWORD>(static_cast<int32_t>(SendMessageW(gGainSlider, TBM_GETPOS, 0, 0)));
    RegSetValueExW(key, L"GainTenths", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&gain), sizeof(gain));
    gSavedSettings.gainTenths = static_cast<int>(static_cast<int32_t>(gain));
    RegCloseKey(key);
}

std::wstring DeviceDisplayName(const AudioDeviceInfo& device) {
    std::wstring label = device.name;
    if (device.isDefault) {
        label += L"  — Windows default";
    }
    if (device.isGenericCable) {
        label += L"  [possible virtual device]";
    } else if (device.isVoicemod) {
        label += L"  [Voicemod internal]";
    } else if (device.isLikelyVirtual) {
        label += L"  [virtual]";
    }
    return label;
}

int FindDeviceIndex(const std::vector<AudioDeviceInfo>& devices, const std::wstring& savedId,
                    bool preferDefault, bool preferVirtual, const std::wstring& excludedId = {}) {
    if (!savedId.empty()) {
        for (size_t index = 0; index < devices.size(); ++index) {
            if (devices[index].id == savedId && devices[index].id != excludedId) {
                return static_cast<int>(index);
            }
        }
    }
    if (preferVirtual) {
        int candidate = -1;
        for (size_t index = 0; index < devices.size(); ++index) {
            if (devices[index].isGenericCable && devices[index].id != excludedId) {
                if (candidate >= 0) {
                    return -1;
                }
                candidate = static_cast<int>(index);
            }
        }
        return candidate;
    }
    if (preferDefault) {
        for (size_t index = 0; index < devices.size(); ++index) {
            if (devices[index].isDefault && devices[index].id != excludedId) {
                return static_cast<int>(index);
            }
        }
    }
    for (size_t index = 0; index < devices.size(); ++index) {
        if (devices[index].id != excludedId) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

int FindExactDeviceIndex(const std::vector<AudioDeviceInfo>& devices, const std::wstring& id,
                         const std::wstring& excludedId = {}) {
    if (id.empty()) {
        return -1;
    }
    for (size_t index = 0; index < devices.size(); ++index) {
        if (devices[index].id == id && devices[index].id != excludedId) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

void FillCombo(HWND combo, const std::vector<AudioDeviceInfo>& devices, int desiredIndex) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (size_t index = 0; index < devices.size(); ++index) {
        const std::wstring label = DeviceDisplayName(devices[index]);
        const LRESULT item = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        if (item != CB_ERR && item != CB_ERRSPACE) {
            SendMessageW(combo, CB_SETITEMDATA, static_cast<WPARAM>(item), static_cast<LPARAM>(index));
        }
    }
    if (desiredIndex >= 0) {
        for (int item = 0; item < static_cast<int>(devices.size()); ++item) {
            const LRESULT stored = SendMessageW(combo, CB_GETITEMDATA, item, 0);
            if (stored == desiredIndex) {
                SendMessageW(combo, CB_SETCURSEL, item, 0);
                break;
            }
        }
    }
}

void FillApplicationCombo(int desiredIndex) {
    SendMessageW(gSourceCombo, CB_RESETCONTENT, 0, 0);
    for (size_t index = 0; index < gApplications.size(); ++index) {
        const std::wstring label = BuildApplicationDisplayLabel(gApplications[index]);
        const LRESULT item = SendMessageW(gSourceCombo, CB_ADDSTRING, 0,
                                           reinterpret_cast<LPARAM>(label.c_str()));
        if (item != CB_ERR && item != CB_ERRSPACE) {
            SendMessageW(gSourceCombo, CB_SETITEMDATA, static_cast<WPARAM>(item),
                         static_cast<LPARAM>(index));
        }
    }
    if (desiredIndex >= 0) {
        for (int item = 0; item < static_cast<int>(gApplications.size()); ++item) {
            if (SendMessageW(gSourceCombo, CB_GETITEMDATA, item, 0) == desiredIndex) {
                SendMessageW(gSourceCombo, CB_SETCURSEL, item, 0);
                break;
            }
        }
    }
}

std::wstring SuggestedGameMicrophone() {
    const auto* destination = SelectedDevice(gDestinationCombo, gInventory.playback);
    if (destination == nullptr) {
        return L"Install a generic virtual cable, then press Refresh";
    }
    if (!destination->isGenericCable) {
        return L"Unverified destination — choose its matching recording side manually";
    }

    if (ContainsIgnoringCase(destination->name, L"CABLE Input")) {
        for (const auto& recording : gInventory.recording) {
            if (ContainsIgnoringCase(recording.name, L"CABLE Output")) {
                return L"For VB-CABLE: " + recording.name + L" (verify before chat)";
            }
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
        return L"Likely matching side: " + onlyCandidate->name + L" — verify the vendor pairing";
    }
    if (candidateCount > 1) {
        return L"Choose the recording side matching the selected product (multiple candidates found)";
    }
    return L"No matching recording side is active";
}

void UpdateDestinationGuidance() {
    const auto* source = SelectedApplication();
    const auto* destination = SelectedDevice(gDestinationCombo, gInventory.playback);
    std::wstring warning;
    if (source == nullptr) {
        warning = L"Choose an application. Open Chrome first and press Refresh apps if it is missing.";
    } else if (destination == nullptr) {
        warning = L"No destination selected. Install or enable a trusted virtual-audio cable first.";
    } else if (destination->isVoicemod) {
        warning = L"Voicemod's render/output endpoint is an internal bridge, not a generic cable. Use VB-CABLE or another true render-to-recording cable.";
    } else if (!destination->isGenericCable) {
        warning = L"This destination does not look virtual. It may play through speakers instead of becoming a game microphone.";
    } else {
        warning = IsChromeExecutableName(source->executableName)
            ? L"Chrome-wide capture: all tabs/windows in this Chrome process tree are included; games and other apps are excluded."
            : L"Only this application's process tree is captured; games and unrelated apps are excluded.";
    }
    SetWindowTextW(gWarning, warning.c_str());

    const std::wstring gameMic = L"Game input:  " + SuggestedGameMicrophone();
    SetWindowTextW(gGameMic, gameMic.c_str());
    InvalidateRect(gMainWindow, nullptr, FALSE);
}

void RefreshDevices(bool preserveSelections) {
    RouterSnapshot snapshot = gRouter->Snapshot();
    if (snapshot.state == RouterState::Running || snapshot.state == RouterState::Starting) {
        return;
    }
    if (snapshot.state == RouterState::Error) {
        // Refresh is an explicit acknowledgement of the failed route and joins its finished worker.
        gRouter->Stop();
    }

    const AppProcessInfo* selectedApplication = preserveSelections ? SelectedApplication() : nullptr;
    const uint32_t sourceProcessId = selectedApplication != nullptr ? selectedApplication->processId : 0;
    const uint64_t sourceProcessCreationTime = selectedApplication != nullptr
        ? selectedApplication->creationTime : 0;
    const std::wstring sourceExecutablePath = selectedApplication != nullptr
        ? selectedApplication->executablePath : gSavedSettings.sourceApplicationPath;
    std::wstring destinationId = preserveSelections && SelectedDevice(gDestinationCombo, gInventory.playback)
        ? SelectedDevice(gDestinationCombo, gInventory.playback)->id : gSavedSettings.destinationId;

    std::vector<AppProcessInfo> updatedApplications;
    AudioDeviceInventory updated;
    std::wstring applicationError;
    std::wstring deviceError;
    const bool applicationsListed = EnumerateDesktopApplications(updatedApplications, applicationError);
    const bool devicesListed = EnumerateAudioDevices(updated, deviceError);
    if (!applicationsListed || !devicesListed) {
        gDeviceError = !applicationsListed ? applicationError : deviceError;
        gDeviceNotice.clear();
        gApplications.clear();
        gInventory = {};
        FillApplicationCombo(-1);
        FillCombo(gDestinationCombo, gInventory.playback, -1);
        UpdateDestinationGuidance();
        SetControlsRouting(false);
        return;
    }
    gDeviceError.clear();
    gApplications = std::move(updatedApplications);
    gInventory = std::move(updated);

    const int sourceIndex = FindPreferredApplicationIndex(
        gApplications, sourceProcessId, sourceExecutablePath, sourceProcessCreationTime);
    const bool missingSource = (sourceProcessId != 0 || !sourceExecutablePath.empty()) && sourceIndex < 0;
    int destinationIndex = -1;
    bool missingDestination = false;
    if (destinationId.empty()) {
        destinationIndex = FindDeviceIndex(gInventory.playback, {}, false, true);
    } else {
        destinationIndex = FindExactDeviceIndex(gInventory.playback, destinationId);
        missingDestination = destinationIndex < 0;
    }
    const int safeDestinationIndex = destinationIndex >= 0 &&
        gInventory.playback[static_cast<size_t>(destinationIndex)].isGenericCable ? destinationIndex : -1;
    FillApplicationCombo(sourceIndex);
    FillCombo(gDestinationCombo, gInventory.playback, safeDestinationIndex);

    const size_t genericCableCount = static_cast<size_t>(std::count_if(
        gInventory.playback.begin(), gInventory.playback.end(),
        [](const AudioDeviceInfo& device) { return device.isGenericCable; }));
    if (missingSource && missingDestination) {
        gDeviceNotice = L"Saved application and cable are missing — select both explicitly";
    } else if (missingSource) {
        gDeviceNotice = sourceProcessId != 0
            ? L"The selected application ended or changed — select it again explicitly"
            : L"Saved application is missing or ambiguous — select it explicitly";
    } else if (missingDestination) {
        gDeviceNotice = L"Saved destination is missing — choose a destination explicitly";
    } else if (destinationIndex >= 0 && safeDestinationIndex < 0) {
        gDeviceNotice = L"Saved destination is not a verified cable — choose a destination explicitly";
    } else if (gApplications.empty()) {
        gDeviceNotice = L"No selectable applications found — open Chrome, then press Refresh apps";
    } else if (sourceIndex < 0) {
        gDeviceNotice = L"Open Chrome and press Refresh apps, or choose another application explicitly";
    } else if (gInventory.playback.empty()) {
        gDeviceNotice = L"No active playback devices found";
    } else if (genericCableCount == 0) {
        gDeviceNotice = L"Virtual cable required — install one, then press Refresh";
    } else if (destinationIndex < 0 && genericCableCount > 1) {
        gDeviceNotice = L"Multiple cable candidates found — choose the matching playback side explicitly";
    } else {
        gDeviceNotice.clear();
    }
    UpdateDestinationGuidance();
}

float CurrentGainDb() {
    return static_cast<float>(SendMessageW(gGainSlider, TBM_GETPOS, 0, 0)) / 10.0F;
}

void UpdateGainLabel() {
    const float gainDb = CurrentGainDb();
    wchar_t text[64]{};
    swprintf_s(text, L"%+.1f dB", gainDb);
    SetWindowTextW(gGainLabel, text);
    gRouter->SetGainDb(gainDb);
}

void SetControlsRouting(bool routing) {
    EnableWindow(gSourceCombo, !routing);
    EnableWindow(gDestinationCombo, !routing);
    EnableWindow(gRefreshButton, !routing);
    const bool hasSelections = SelectedApplication() != nullptr &&
        SelectedDevice(gDestinationCombo, gInventory.playback) != nullptr;
    EnableWindow(gStartButton, !routing && gDeviceError.empty() && hasSelections);
    EnableWindow(gStopButton, routing);
}

void StartRouting() {
    const auto* source = SelectedApplication();
    const auto* destination = SelectedDevice(gDestinationCombo, gInventory.playback);
    if (source == nullptr || destination == nullptr) {
        MessageBoxW(gMainWindow, L"Choose both an application and a virtual-cable destination.",
                    L"ChromeMic", MB_OK | MB_ICONWARNING);
        return;
    }
    if (destination->isVoicemod) {
        MessageBoxW(gMainWindow,
            L"Voicemod's render/output endpoint is reserved as its internal bridge and is not a general render-to-microphone cable. Install VB-CABLE (or another true virtual cable), press Refresh, and select its playback side.",
            L"ChromeMic — incompatible endpoint", MB_OK | MB_ICONWARNING);
        return;
    }
    if (!destination->isGenericCable) {
        const int answer = MessageBoxW(gMainWindow,
            L"The selected destination does not look like a virtual-audio device. Continue only if you know it exposes a matching recording endpoint.",
            L"ChromeMic — verify destination", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
        if (answer != IDYES) {
            return;
        }
    }

    gRouter->Stop();
    RouteConfig config;
    config.sourceProcessId = source->processId;
    config.sourceProcessCreationTime = source->creationTime;
    config.sourceExecutablePath = source->executablePath;
    config.sourceName = BuildApplicationDisplayLabel(*source);
    config.destinationId = destination->id;
    config.destinationName = destination->name;
    config.gainDb = CurrentGainDb();
    config.muted = SendMessageW(gMuteCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;

    std::wstring error;
    if (!gRouter->Start(config, error)) {
        MessageBoxW(gMainWindow, error.c_str(), L"ChromeMic could not start", MB_OK | MB_ICONERROR);
        return;
    }
    SaveSettings();
    SetControlsRouting(true);
}

void StopRouting() {
    SetWindowTextW(gStatus, L"●  Stopping and releasing app capture...");
    UpdateWindow(gStatus);
    gRouter->Stop();
    SetControlsRouting(false);
}

int MeterPosition(float peak) {
    if (peak <= 0.000001F) {
        return 0;
    }
    const float db = 20.0F * std::log10(peak);
    return static_cast<int>(std::clamp((db + 60.0F) / 60.0F, 0.0F, 1.0F) * 1000.0F);
}

void UpdateStatusUi() {
    const RouterSnapshot snapshot = gRouter->Snapshot();
    std::wstring prefix;
    std::wstring statusMessage = snapshot.message;
    switch (snapshot.state) {
    case RouterState::Running:
        prefix = snapshot.muted ? L"●  MUTED — " : L"●  LIVE — ";
        break;
    case RouterState::Starting:
        prefix = L"●  STARTING — ";
        break;
    case RouterState::Error:
        prefix = L"●  ERROR — ";
        break;
    default:
        if (!gDeviceError.empty()) {
            prefix = L"●  DEVICE ERROR — ";
            statusMessage = gDeviceError;
        } else if (!gDeviceNotice.empty()) {
            prefix = L"●  SETUP — ";
            statusMessage = gDeviceNotice;
        } else {
            prefix = L"●  ";
        }
        break;
    }
    SetWindowTextW(gStatus, (prefix + statusMessage).c_str());
    SendMessageW(gMeter, PBM_SETPOS, MeterPosition(snapshot.peak), 0);
    SetWindowTextW(gLimiter, snapshot.limiting ? L"LIMITER ACTIVE — lower gain" : L"Safe limiter: −1 dBFS ceiling");

    std::wostringstream metrics;
    if (snapshot.sampleRate != 0) {
        metrics << snapshot.sampleRate / 1000.0 << L" kHz  •  " << snapshot.channels << L" ch"
                << L"  •  estimated routing buffer " << snapshot.latencyMilliseconds << L" ms"
                << L"  •  queue " << snapshot.queuedMilliseconds << L" ms";
        if (snapshot.droppedFrames != 0) {
            metrics << L"  •  dropped " << snapshot.droppedFrames << L" frames";
        }
    } else {
        metrics << L"Audio stays in memory only. Nothing is recorded to disk or sent online.";
    }
    SetWindowTextW(gMetrics, metrics.str().c_str());

    const bool active = snapshot.state == RouterState::Running || snapshot.state == RouterState::Starting;
    SetControlsRouting(active);
    InvalidateRect(gMainWindow, nullptr, FALSE);
}

void CreateMainControls() {
    gTitleFont = CreateUiFont(22, FW_SEMIBOLD);
    gHeadingFont = CreateUiFont(11, FW_SEMIBOLD);
    gBodyFont = CreateUiFont(10, FW_NORMAL);
    gSmallFont = CreateUiFont(9, FW_NORMAL);

    MakeControl(L"STATIC", L"ChromeMic", SS_LEFT, 28, 22, 300, 38, 0, gTitleFont);
    MakeControl(L"STATIC", L"SELECTED APPLICATION  →  VIRTUAL MICROPHONE", SS_LEFT, 30, 61, 560, 22, 0, gSmallFont);
    MakeControl(L"STATIC",
        L"Captures only the selected application's process tree. Choose Chrome for YouTube; game audio and unrelated apps stay excluded.",
        SS_LEFT, 30, 91, 744, 40, 0, gBodyFont);

    MakeControl(L"STATIC", L"1   Choose application (Chrome is preferred)", SS_LEFT, 30, 146, 430, 24, 0, gHeadingFont);
    gSourceCombo = MakeControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
                               30, 173, 638, 280, IDC_SOURCE, gBodyFont);
    gRefreshButton = MakeControl(L"BUTTON", L"Refresh apps", BS_PUSHBUTTON | WS_TABSTOP,
                                 680, 172, 94, 30, IDC_REFRESH, gBodyFont);

    MakeControl(L"STATIC", L"2   Send to virtual cable (playback side)", SS_LEFT,
                30, 220, 420, 24, 0, gHeadingFont);
    gDestinationCombo = MakeControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
                                    30, 247, 744, 280, IDC_DESTINATION, gBodyFont);
    gWarning = MakeControl(L"STATIC", L"", SS_LEFT, 30, 286, 744, 38, IDC_WARNING, gSmallFont);

    MakeControl(L"STATIC", L"3   Output level", SS_LEFT, 30, 331, 180, 24, 0, gHeadingFont);
    gGainSlider = MakeControl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP,
                              30, 360, 420, 38, IDC_GAIN, gBodyFont);
    SendMessageW(gGainSlider, TBM_SETRANGE, TRUE, MAKELONG(-120, 60));
    SendMessageW(gGainSlider, TBM_SETTICFREQ, 30, 0);
    SendMessageW(gGainSlider, TBM_SETPOS, TRUE, gSavedSettings.gainTenths);
    gGainLabel = MakeControl(L"STATIC", L"+0.0 dB", SS_CENTER, 460, 365, 78, 26, IDC_GAIN_LABEL, gHeadingFont);
    gMuteCheck = MakeControl(L"BUTTON", L"Mute output", BS_AUTOCHECKBOX | WS_TABSTOP,
                             563, 361, 150, 28, IDC_MUTE, gBodyFont);

    gMeter = MakeControl(PROGRESS_CLASSW, L"", PBS_SMOOTH, 30, 407, 744, 18, IDC_METER, gBodyFont);
    SendMessageW(gMeter, PBM_SETRANGE32, 0, 1000);
    SendMessageW(gMeter, PBM_SETBARCOLOR, 0, kGreen);
    SendMessageW(gMeter, PBM_SETBKCOLOR, 0, RGB(222, 227, 237));
    gLimiter = MakeControl(L"STATIC", L"Safe limiter: −1 dBFS ceiling", SS_RIGHT,
                           515, 433, 259, 20, IDC_LIMITER, gSmallFont);

    gStartButton = MakeControl(L"BUTTON", L"Start routing", BS_DEFPUSHBUTTON | WS_TABSTOP,
                               30, 462, 218, 42, IDC_START, gHeadingFont);
    gStopButton = MakeControl(L"BUTTON", L"Stop", BS_PUSHBUTTON | WS_TABSTOP,
                              260, 462, 112, 42, IDC_STOP, gHeadingFont);
    gGetCable = MakeControl(L"BUTTON", L"Get VB-CABLE", BS_PUSHBUTTON | WS_TABSTOP,
                            449, 462, 132, 42, IDC_GET_CABLE, gBodyFont);
    gSoundSettings = MakeControl(L"BUTTON", L"Sound settings", BS_PUSHBUTTON | WS_TABSTOP,
                                 591, 462, 183, 42, IDC_SOUND_SETTINGS, gBodyFont);

    gStatus = MakeControl(L"STATIC", L"●  Ready — routing is stopped", SS_LEFT,
                          30, 520, 744, 28, IDC_STATUS, gHeadingFont);
    gMetrics = MakeControl(L"STATIC", L"Audio stays in memory only. Nothing is recorded to disk or sent online.", SS_LEFT,
                           30, 551, 744, 24, IDC_METRICS, gSmallFont);
    gGameMic = MakeControl(L"STATIC", L"Game microphone to select:", SS_LEFT,
                           30, 590, 744, 24, IDC_GAME_MIC, gHeadingFont);
    MakeControl(L"STATIC",
        L"For VB-CABLE: choose CABLE Input here → Start → confirm the meter moves → choose CABLE Output in the game. For other products, use their matching sides.",
        SS_LEFT, 30, 620, 744, 44, 0, gBodyFont);
    MakeControl(L"STATIC",
        L"Chrome selection is browser-wide for that process tree (all tabs/windows), not per-tab. Stop releases capture; Mute keeps it open but sends silence.",
        SS_LEFT, 30, 670, 744, 38, 0, gSmallFont);

    SetWindowTheme(gSourceCombo, L"Explorer", nullptr);
    SetWindowTheme(gDestinationCombo, L"Explorer", nullptr);
    SetWindowTheme(gMeter, L"", L"");
    UpdateGainLabel();
    SetControlsRouting(false);
}

LRESULT OnControlColor(HDC dc, HWND control) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, kText);
    if (control == gWarning) {
        SetTextColor(dc, kOrange);
    } else if (control == gMetrics || control == gLimiter) {
        SetTextColor(dc, kMutedText);
    } else if (control == gStatus && gRouter) {
        const RouterSnapshot snapshot = gRouter->Snapshot();
        if (snapshot.state == RouterState::Running) {
            SetTextColor(dc, snapshot.muted ? kOrange : kGreen);
        } else if (snapshot.state == RouterState::Error) {
            SetTextColor(dc, kRed);
        } else if (snapshot.state == RouterState::Starting) {
            SetTextColor(dc, kBlue);
        } else if (!gDeviceError.empty()) {
            SetTextColor(dc, kRed);
        } else if (!gDeviceNotice.empty()) {
            SetTextColor(dc, kOrange);
        }
    } else if (control == gGameMic) {
        SetTextColor(dc, kBlue);
    }
    return reinterpret_cast<LRESULT>(gBackgroundBrush);
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        gMainWindow = window;
        CreateMainControls();
        RefreshDevices(false);
        SetTimer(window, kStatusTimer, 100, nullptr);
        return 0;

    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        const int notification = HIWORD(wParam);
        if (id == IDC_REFRESH && notification == BN_CLICKED) {
            RefreshDevices(true);
        } else if (id == IDC_START && notification == BN_CLICKED) {
            StartRouting();
        } else if (id == IDC_STOP && notification == BN_CLICKED) {
            StopRouting();
        } else if (id == IDC_MUTE && notification == BN_CLICKED) {
            gRouter->SetMuted(SendMessageW(gMuteCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
        } else if ((id == IDC_SOURCE || id == IDC_DESTINATION) && notification == CBN_SELCHANGE) {
            gDeviceNotice.clear();
            UpdateDestinationGuidance();
            SaveSettings();
            SetControlsRouting(false);
        } else if (id == IDC_SOUND_SETTINGS && notification == BN_CLICKED) {
            ShellExecuteW(window, L"open", L"ms-settings:sound", nullptr, nullptr, SW_SHOWNORMAL);
        } else if (id == IDC_GET_CABLE && notification == BN_CLICKED) {
            ShellExecuteW(window, L"open", L"https://vb-audio.com/Cable/index.htm", nullptr, nullptr, SW_SHOWNORMAL);
        }
        return 0;
    }

    case WM_HSCROLL:
        if (reinterpret_cast<HWND>(lParam) == gGainSlider) {
            UpdateGainLabel();
            SaveSettings();
        }
        return 0;

    case WM_TIMER:
        if (wParam == kStatusTimer && !gIsClosing) {
            UpdateStatusUi();
        }
        return 0;

    case WM_CTLCOLORSTATIC:
        return OnControlColor(reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam));

    case WM_ERASEBKGND: {
        RECT rect{};
        GetClientRect(window, &rect);
        FillRect(reinterpret_cast<HDC>(wParam), &rect, gBackgroundBrush);
        return 1;
    }

    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = ScaleUi(830);
        info->ptMinTrackSize.y = ScaleUi(780);
        return 0;
    }

    case WM_CLOSE:
        if (!gIsClosing) {
            gIsClosing = true;
            KillTimer(window, kStatusTimer);
            SaveSettings();
            gRouter->Stop();
        }
        DestroyWindow(window);
        return 0;

    case WM_DESTROY:
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
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    INITCOMMONCONTROLSEX commonControls{sizeof(commonControls), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS};
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
        CoUninitialize();
        return 1;
    }

    RECT desired{0, 0, ScaleUi(830), ScaleUi(780)};
    AdjustWindowRectExForDpi(&desired, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             FALSE, 0, gUiDpi);
    HWND window = CreateWindowExW(0, kWindowClass, L"ChromeMic — route one application's audio into games",
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                  CW_USEDEFAULT, CW_USEDEFAULT, desired.right - desired.left, desired.bottom - desired.top,
                                  nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
        CleanupUiResources();
        CoUninitialize();
        return 1;
    }

    BOOL darkTitle = FALSE;
    DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkTitle, sizeof(darkTitle));
    ShowWindow(window, showCommand);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    gRouter.reset();
    CleanupUiResources();
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
