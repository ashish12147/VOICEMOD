#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

inline constexpr std::string_view kChromeMicCurrentVersion = "1.2.0";
inline constexpr std::wstring_view kChromeMicUpdateManifestUrl =
    L"https://raw.githubusercontent.com/ashish12147/VOICEMOD/main/update.json";
inline constexpr std::size_t kMaximumUpdateManifestBytes = 16U * 1024U;

struct SemanticVersion {
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t patch = 0;
};

// Strictly accepts MAJOR.MINOR.PATCH, with no whitespace, leading zeroes,
// prerelease suffix, or build metadata. The output is replaced only on success.
bool ParseSemanticVersion(std::string_view text,
                          SemanticVersion& version) noexcept;

// Returns -1, 0, or 1 when left is older than, equal to, or newer than right.
int CompareSemanticVersions(const SemanticVersion& left,
                            const SemanticVersion& right) noexcept;

// Parses and compares two strict semantic versions. comparison is zeroed when
// either input is invalid.
bool CompareSemanticVersionStrings(std::string_view left,
                                   std::string_view right,
                                   int& comparison) noexcept;

enum class UpdateManifestError {
    None,
    ManifestTooLarge,
    InvalidJson,
    DuplicateField,
    UnexpectedField,
    MissingVersion,
    MissingDownloadUrl,
    InvalidVersion,
    InvalidDownloadUrl,
    OutOfMemory
};

struct UpdateManifest {
    std::string version;
    std::string downloadUrl;
    SemanticVersion parsedVersion;
};

// Parses the two-field release manifest. Unknown and duplicate fields are
// rejected. The download URL must name ChromeMic's matching GitHub release ZIP.
// The output is replaced only on success.
UpdateManifestError ParseUpdateManifest(std::string_view json,
                                        UpdateManifest& manifest) noexcept;
const wchar_t* UpdateManifestErrorMessage(UpdateManifestError error) noexcept;

enum class UpdateCheckStatus {
    UpdateAvailable,
    UpToDate,
    TimedOut,
    NetworkError,
    SecureConnectionError,
    HttpStatusError,
    InvalidContentType,
    ResponseTooLarge,
    InvalidManifest,
    InvalidVersion,
    InvalidDownloadUrl,
    InternalError
};

struct UpdateCheckResult {
    UpdateCheckStatus status = UpdateCheckStatus::InternalError;
    UpdateManifest manifest;
    std::uint32_t nativeError = 0;
    std::uint32_t httpStatus = 0;
    std::wstring message;

    [[nodiscard]] bool Succeeded() const noexcept;
};

// Performs a synchronous HTTPS check using per-call WinHTTP handles. It is safe
// to invoke concurrently, but should be called from a worker rather than a UI or
// real-time audio thread. It never downloads or executes an update.
UpdateCheckResult CheckForChromeMicUpdates() noexcept;
