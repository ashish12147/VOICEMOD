#include "app_processes.h"

#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>

namespace {

constexpr int kMaximumWindowTitleCharacters = 4096;
constexpr DWORD kProcessQueryAccess = PROCESS_QUERY_LIMITED_INFORMATION;

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle = nullptr) noexcept : handle_(handle) {}

    ~ScopedHandle() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    [[nodiscard]] HANDLE Get() const noexcept { return handle_; }

private:
    HANDLE handle_ = nullptr;
};

wchar_t LowerCharacter(wchar_t character) noexcept {
    return static_cast<wchar_t>(std::towlower(static_cast<wint_t>(character)));
}

bool EqualsIgnoringCase(std::wstring_view left, std::wstring_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t index = 0; index < left.size(); ++index) {
        if (LowerCharacter(left[index]) != LowerCharacter(right[index])) {
            return false;
        }
    }
    return true;
}

bool LessIgnoringCase(std::wstring_view left, std::wstring_view right) noexcept {
    const size_t commonLength = std::min(left.size(), right.size());
    for (size_t index = 0; index < commonLength; ++index) {
        const wchar_t leftCharacter = LowerCharacter(left[index]);
        const wchar_t rightCharacter = LowerCharacter(right[index]);
        if (leftCharacter < rightCharacter) {
            return true;
        }
        if (leftCharacter > rightCharacter) {
            return false;
        }
    }
    return left.size() < right.size();
}

std::wstring_view BaseName(std::wstring_view path) noexcept {
    const size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring_view::npos ? path : path.substr(separator + 1);
}

std::wstring NormalizeCaption(std::wstring caption) {
    for (wchar_t& character : caption) {
        if (character == L'\r' || character == L'\n' || character == L'\t') {
            character = L' ';
        }
    }

    const auto isWhitespace = [](wchar_t character) {
        return std::iswspace(static_cast<wint_t>(character)) != 0;
    };
    const auto first = std::find_if_not(caption.begin(), caption.end(), isWhitespace);
    if (first == caption.end()) {
        return {};
    }
    const auto last = std::find_if_not(caption.rbegin(), caption.rend(), isWhitespace).base();

    std::wstring normalized;
    normalized.reserve(static_cast<size_t>(std::distance(first, last)));
    bool previousWasWhitespace = false;
    for (auto iterator = first; iterator != last; ++iterator) {
        const bool whitespace = isWhitespace(*iterator);
        if (!whitespace || !previousWasWhitespace) {
            normalized.push_back(whitespace ? L' ' : *iterator);
        }
        previousWasWhitespace = whitespace;
    }
    return normalized;
}

std::wstring WindowCaption(HWND window) {
    const int reportedLength = GetWindowTextLengthW(window);
    if (reportedLength <= 0) {
        return {};
    }

    const int requestedCharacters = reportedLength >= kMaximumWindowTitleCharacters - 1
        ? kMaximumWindowTitleCharacters : reportedLength + 1;
    std::wstring buffer(static_cast<size_t>(requestedCharacters), L'\0');
    const int copiedCharacters = GetWindowTextW(window, buffer.data(), requestedCharacters);
    if (copiedCharacters <= 0) {
        return {};
    }
    buffer.resize(static_cast<size_t>(copiedCharacters));
    return NormalizeCaption(std::move(buffer));
}

bool ProcessDetails(DWORD processId, std::wstring& path, std::uint64_t& creationTime) {
    ScopedHandle process(OpenProcess(kProcessQueryAccess, FALSE, processId));
    if (process.Get() == nullptr) {
        return false;
    }

    path.assign(32768, L'\0');
    DWORD characters = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process.Get(), 0, path.data(), &characters) ||
        characters == 0 || static_cast<size_t>(characters) > path.size()) {
        path.clear();
        return false;
    }
    path.resize(static_cast<size_t>(characters));

    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(process.Get(), &created, &exited, &kernel, &user)) {
        path.clear();
        return false;
    }
    ULARGE_INTEGER value{};
    value.LowPart = created.dwLowDateTime;
    value.HighPart = created.dwHighDateTime;
    creationTime = value.QuadPart;
    return creationTime != 0;
}

bool IsApplicationWindow(HWND window, DWORD ownProcessId) noexcept {
    if (window == nullptr || window == GetShellWindow() || !IsWindowVisible(window)) {
        return false;
    }
    if (GetWindow(window, GW_OWNER) != nullptr) {
        return false;
    }
    const LONG_PTR extendedStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((extendedStyle & static_cast<LONG_PTR>(WS_EX_TOOLWINDOW)) != 0) {
        return false;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    return processId != 0 && processId != ownProcessId;
}

struct EnumerationContext {
    std::vector<AppProcessInfo>* applications = nullptr;
    std::unordered_set<DWORD> seenProcesses;
    DWORD ownProcessId = 0;
    bool callbackFailed = false;
};

BOOL CALLBACK CollectApplicationWindow(HWND window, LPARAM parameter) noexcept {
    auto* context = reinterpret_cast<EnumerationContext*>(parameter);
    if (context == nullptr || context->applications == nullptr) {
        return FALSE;
    }

    try {
        if (!IsApplicationWindow(window, context->ownProcessId)) {
            return TRUE;
        }

        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        if (processId == 0 || context->seenProcesses.contains(processId)) {
            return TRUE;
        }

        std::wstring title = WindowCaption(window);
        if (title.empty()) {
            return TRUE;
        }
        std::wstring executablePath;
        std::uint64_t creationTime = 0;
        if (!ProcessDetails(processId, executablePath, creationTime)) {
            return TRUE;
        }

        AppProcessInfo application;
        application.processId = static_cast<std::uint32_t>(processId);
        application.creationTime = creationTime;
        application.windowTitle = std::move(title);
        application.executableName = std::wstring(BaseName(executablePath));
        application.executablePath = std::move(executablePath);
        if (application.executableName.empty()) {
            return TRUE;
        }

        context->seenProcesses.emplace(processId);
        context->applications->push_back(std::move(application));
        return TRUE;
    } catch (...) {
        context->callbackFailed = true;
        return FALSE;
    }
}

} // namespace

bool EnumerateDesktopApplications(std::vector<AppProcessInfo>& applications,
                                  std::wstring& errorMessage) {
    applications.clear();
    errorMessage.clear();

    EnumerationContext context;
    context.applications = &applications;
    context.ownProcessId = GetCurrentProcessId();

    SetLastError(ERROR_SUCCESS);
    const BOOL result = EnumWindows(CollectApplicationWindow, reinterpret_cast<LPARAM>(&context));
    if (!result) {
        applications.clear();
        errorMessage = context.callbackFailed
            ? L"Desktop applications could not be listed because memory was unavailable."
            : L"Windows could not list desktop applications.";
        return false;
    }

    try {
        std::stable_sort(applications.begin(), applications.end(),
            [](const AppProcessInfo& left, const AppProcessInfo& right) {
                const bool leftIsChrome = IsChromeExecutableName(left.executableName);
                const bool rightIsChrome = IsChromeExecutableName(right.executableName);
                if (leftIsChrome != rightIsChrome) {
                    return leftIsChrome;
                }
                if (!EqualsIgnoringCase(left.windowTitle, right.windowTitle)) {
                    return LessIgnoringCase(left.windowTitle, right.windowTitle);
                }
                if (!EqualsIgnoringCase(left.executableName, right.executableName)) {
                    return LessIgnoringCase(left.executableName, right.executableName);
                }
                return left.processId < right.processId;
            });
    } catch (...) {
        applications.clear();
        errorMessage = L"Desktop applications could not be sorted because memory was unavailable.";
        return false;
    }
    return true;
}

bool IsChromeExecutableName(std::wstring_view executableNameOrPath) noexcept {
    return EqualsIgnoringCase(BaseName(executableNameOrPath), L"chrome.exe");
}

std::wstring BuildApplicationDisplayLabel(const AppProcessInfo& application) {
    if (IsChromeExecutableName(application.executableName) ||
        IsChromeExecutableName(application.executablePath)) {
        std::wstring title = NormalizeCaption(application.windowTitle);
        constexpr size_t kMaximumTitleLength = 72;
        if (title.size() > kMaximumTitleLength) {
            title.resize(kMaximumTitleLength - 1);
            title += L"…";
        }
        std::wstring label = L"Google Chrome — all tabs/windows in this process tree";
        if (!title.empty()) {
            label += L" — " + title;
        }
        return label + L"  [PID " + std::to_wstring(application.processId) + L"]";
    }
    const std::wstring title = NormalizeCaption(application.windowTitle);
    std::wstring executableName = NormalizeCaption(application.executableName);
    if (executableName.empty()) {
        executableName = std::wstring(BaseName(application.executablePath));
    }

    std::wstring label;
    if (!title.empty()) {
        label = title;
    }
    if (!executableName.empty() && !EqualsIgnoringCase(title, executableName)) {
        if (!label.empty()) {
            label += L"  —  ";
        }
        label += executableName;
    }
    if (label.empty()) {
        label = L"Application";
    }
    label += L"  [PID " + std::to_wstring(application.processId) + L"]";
    return label;
}

int FindPreferredApplicationIndex(const std::vector<AppProcessInfo>& applications,
                                  std::uint32_t preferredProcessId,
                                  std::wstring_view preferredExecutablePath,
                                  std::uint64_t preferredCreationTime) noexcept {
    if (applications.empty()) {
        return -1;
    }

    if (preferredProcessId != 0) {
        for (size_t index = 0; index < applications.size(); ++index) {
            const auto& application = applications[index];
            if (application.processId == preferredProcessId &&
                (preferredExecutablePath.empty() ||
                 EqualsIgnoringCase(application.executablePath, preferredExecutablePath)) &&
                (preferredCreationTime == 0 || application.creationTime == preferredCreationTime)) {
                return index <= static_cast<size_t>(std::numeric_limits<int>::max())
                    ? static_cast<int>(index) : -1;
            }
        }
        return -1;
    }

    if (!preferredExecutablePath.empty()) {
        size_t matchingIndex = 0;
        size_t matchingCount = 0;
        for (size_t index = 0; index < applications.size(); ++index) {
            if (EqualsIgnoringCase(applications[index].executablePath, preferredExecutablePath)) {
                matchingIndex = index;
                ++matchingCount;
            }
        }
        return matchingCount == 1 && matchingIndex <= static_cast<size_t>(std::numeric_limits<int>::max())
            ? static_cast<int>(matchingIndex) : -1;
    }

    size_t chromeIndex = 0;
    size_t chromeCount = 0;
    for (size_t index = 0; index < applications.size(); ++index) {
        if (IsChromeExecutableName(applications[index].executableName)) {
            chromeIndex = index;
            ++chromeCount;
        }
    }
    return chromeCount == 1 && chromeIndex <= static_cast<size_t>(std::numeric_limits<int>::max())
        ? static_cast<int>(chromeIndex) : -1;
}
