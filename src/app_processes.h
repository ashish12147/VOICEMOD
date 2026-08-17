#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct AppProcessInfo {
    std::uint32_t processId = 0;
    std::uint64_t creationTime = 0;
    std::wstring windowTitle;
    std::wstring executableName;
    std::wstring executablePath;
};

// Lists visible, unowned top-level desktop applications. Inaccessible and
// short-lived processes are skipped rather than turning a partial list into an
// error. No process contents are read; only the image path and window caption
// are queried.
bool EnumerateDesktopApplications(std::vector<AppProcessInfo>& applications,
                                  std::wstring& errorMessage);

// Pure helpers used by the process picker and its tests.
bool IsChromeExecutableName(std::wstring_view executableNameOrPath) noexcept;
std::wstring BuildApplicationDisplayLabel(const AppProcessInfo& application);

// An active selection must match PID, path, and (when provided) creation time.
// A saved path or first-run Chrome preference is restored only when its match is
// unique. Ambiguous and stale preferences fail closed.
int FindPreferredApplicationIndex(const std::vector<AppProcessInfo>& applications,
                                  std::uint32_t preferredProcessId = 0,
                                  std::wstring_view preferredExecutablePath = {},
                                  std::uint64_t preferredCreationTime = 0) noexcept;
