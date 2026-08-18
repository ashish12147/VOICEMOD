#include "update_checker.h"

#include <Windows.h>
#include <winhttp.h>

#include <array>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr wchar_t kUpdateHost[] = L"raw.githubusercontent.com";
constexpr wchar_t kUpdatePath[] =
    L"/ashish12147/VOICEMOD/main/update.json";
constexpr char kDownloadPrefix[] =
    "https://github.com/ashish12147/VOICEMOD/releases/download/v";
constexpr char kDownloadNamePrefix[] = "/ChromeMic-";
constexpr char kDownloadNameSuffix[] = "-win-x64.zip";
constexpr wchar_t kNoCacheHeaders[] =
    L"Cache-Control: no-cache\r\n"
    L"Pragma: no-cache\r\n";
constexpr DWORD kNoCacheHeadersLength =
    static_cast<DWORD>((sizeof(kNoCacheHeaders) / sizeof(wchar_t)) - 1U);
constexpr std::size_t kMaximumVersionBytes = 32;
constexpr std::size_t kMaximumDownloadUrlBytes = 1024;
constexpr int kResolveTimeoutMilliseconds = 5000;
constexpr int kConnectTimeoutMilliseconds = 5000;
constexpr int kSendTimeoutMilliseconds = 5000;
constexpr int kReceiveTimeoutMilliseconds = 8000;

class WinHttpHandle final {
public:
    explicit WinHttpHandle(HINTERNET handle = nullptr) noexcept
        : handle_(handle) {}

    ~WinHttpHandle() {
        if (handle_ != nullptr) {
            WinHttpCloseHandle(handle_);
        }
    }

    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;

    [[nodiscard]] HINTERNET Get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != nullptr;
    }

private:
    HINTERNET handle_ = nullptr;
};

[[nodiscard]] constexpr bool IsAsciiDigit(char value) noexcept {
    return value >= '0' && value <= '9';
}

[[nodiscard]] constexpr int HexDigitValue(char value) noexcept {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return 10 + (value - 'a');
    }
    if (value >= 'A' && value <= 'F') {
        return 10 + (value - 'A');
    }
    return -1;
}

class JsonCursor final {
public:
    explicit JsonCursor(std::string_view text) noexcept : text_(text) {}

    void SkipWhitespace() noexcept {
        while (position_ < text_.size()) {
            const char value = text_[position_];
            if (value != ' ' && value != '\t' && value != '\r' &&
                value != '\n') {
                break;
            }
            ++position_;
        }
    }

    [[nodiscard]] bool Consume(char expected) noexcept {
        if (position_ >= text_.size() || text_[position_] != expected) {
            return false;
        }
        ++position_;
        return true;
    }

    [[nodiscard]] bool AtEnd() const noexcept {
        return position_ == text_.size();
    }

    [[nodiscard]] bool ParseAsciiString(std::string& output,
                                        std::size_t maximumBytes) {
        if (!Consume('"')) {
            return false;
        }

        output.clear();
        while (position_ < text_.size()) {
            const unsigned char value =
                static_cast<unsigned char>(text_[position_++]);
            if (value == static_cast<unsigned char>('"')) {
                return true;
            }
            if (value < 0x20U || value > 0x7EU) {
                return false;
            }

            char decoded = static_cast<char>(value);
            if (decoded == '\\') {
                if (position_ >= text_.size()) {
                    return false;
                }
                const char escape = text_[position_++];
                switch (escape) {
                case '"':
                case '\\':
                case '/':
                    decoded = escape;
                    break;
                case 'b':
                    decoded = '\b';
                    break;
                case 'f':
                    decoded = '\f';
                    break;
                case 'n':
                    decoded = '\n';
                    break;
                case 'r':
                    decoded = '\r';
                    break;
                case 't':
                    decoded = '\t';
                    break;
                case 'u': {
                    if (text_.size() - position_ < 4U) {
                        return false;
                    }
                    unsigned int codePoint = 0;
                    for (std::size_t index = 0; index < 4U; ++index) {
                        const int digit = HexDigitValue(text_[position_++]);
                        if (digit < 0) {
                            return false;
                        }
                        codePoint = (codePoint << 4U) |
                                    static_cast<unsigned int>(digit);
                    }
                    if (codePoint < 0x20U || codePoint > 0x7EU) {
                        return false;
                    }
                    decoded = static_cast<char>(codePoint);
                    break;
                }
                default:
                    return false;
                }
            }

            if (output.size() >= maximumBytes) {
                return false;
            }
            output.push_back(decoded);
        }
        return false;
    }

private:
    std::string_view text_;
    std::size_t position_ = 0;
};

[[nodiscard]] bool IsExpectedDownloadUrl(const UpdateManifest& manifest) {
    std::string expected;
    expected.reserve((sizeof(kDownloadPrefix) - 1U) + manifest.version.size() +
                     (sizeof(kDownloadNamePrefix) - 1U) +
                     manifest.version.size() +
                     (sizeof(kDownloadNameSuffix) - 1U));
    expected.append(kDownloadPrefix);
    expected.append(manifest.version);
    expected.append(kDownloadNamePrefix);
    expected.append(manifest.version);
    expected.append(kDownloadNameSuffix);
    return manifest.downloadUrl == expected;
}

[[nodiscard]] wchar_t AsciiLower(wchar_t value) noexcept {
    if (value >= L'A' && value <= L'Z') {
        return static_cast<wchar_t>(value + (L'a' - L'A'));
    }
    return value;
}

[[nodiscard]] bool EqualsAsciiCaseInsensitive(std::wstring_view left,
                                              std::wstring_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (AsciiLower(left[index]) != AsciiLower(right[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::wstring_view TrimHttpWhitespace(
    std::wstring_view value) noexcept {
    while (!value.empty() && (value.front() == L' ' || value.front() == L'\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == L' ' || value.back() == L'\t')) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] bool IsAcceptableContentType(std::wstring_view value) noexcept {
    const std::size_t separator = value.find(L';');
    const std::wstring_view mediaType = TrimHttpWhitespace(value.substr(0, separator));
    if (!EqualsAsciiCaseInsensitive(mediaType, L"application/json") &&
        !EqualsAsciiCaseInsensitive(mediaType, L"text/plain")) {
        return false;
    }

    if (separator == std::wstring_view::npos) {
        return true;
    }
    const std::wstring_view parameter =
        TrimHttpWhitespace(value.substr(separator + 1U));
    return EqualsAsciiCaseInsensitive(parameter, L"charset=utf-8");
}

[[nodiscard]] bool IsSecureConnectionError(DWORD error) noexcept {
    const bool certificateError =
        error == ERROR_WINHTTP_SECURE_FAILURE ||
        error == ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED ||
        error == ERROR_WINHTTP_SECURE_CERT_DATE_INVALID ||
        error == ERROR_WINHTTP_SECURE_CERT_CN_INVALID ||
        error == ERROR_WINHTTP_SECURE_INVALID_CA ||
        error == ERROR_WINHTTP_SECURE_INVALID_CERT ||
        error == ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE ||
        error == ERROR_WINHTTP_SECURE_CERT_REV_FAILED ||
        error == ERROR_WINHTTP_SECURE_CERT_REVOKED ||
        error == ERROR_WINHTTP_SECURE_CHANNEL_ERROR;
#ifdef ERROR_WINHTTP_SECURE_FAILURE_PROXY
    return certificateError || error == ERROR_WINHTTP_SECURE_FAILURE_PROXY;
#else
    return certificateError;
#endif
}

[[nodiscard]] UpdateCheckResult MakeFailure(UpdateCheckStatus status,
                                            std::wstring_view message,
                                            DWORD nativeError = 0,
                                            DWORD httpStatus = 0) {
    UpdateCheckResult result;
    result.status = status;
    result.nativeError = nativeError;
    result.httpStatus = httpStatus;
    result.message.assign(message);
    return result;
}

[[nodiscard]] UpdateCheckResult MakeWinHttpFailure(DWORD error) {
    if (error == ERROR_WINHTTP_TIMEOUT) {
        return MakeFailure(
            UpdateCheckStatus::TimedOut,
            L"The update check timed out. Check the connection and try again.",
            error);
    }
    if (IsSecureConnectionError(error)) {
        return MakeFailure(
            UpdateCheckStatus::SecureConnectionError,
            L"GitHub's secure connection could not be verified. Check the "
            L"system clock, proxy, or security software, then try again.",
            error);
    }
    return MakeFailure(
        UpdateCheckStatus::NetworkError,
        L"ChromeMic could not reach GitHub. Check the connection, proxy, or "
        L"firewall and try again.",
        error);
}

[[nodiscard]] UpdateCheckStatus StatusForManifestError(
    UpdateManifestError error) noexcept {
    switch (error) {
    case UpdateManifestError::ManifestTooLarge:
        return UpdateCheckStatus::ResponseTooLarge;
    case UpdateManifestError::InvalidVersion:
        return UpdateCheckStatus::InvalidVersion;
    case UpdateManifestError::InvalidDownloadUrl:
        return UpdateCheckStatus::InvalidDownloadUrl;
    default:
        return UpdateCheckStatus::InvalidManifest;
    }
}

} // namespace

bool ParseSemanticVersion(std::string_view text,
                          SemanticVersion& version) noexcept {
    if (text.empty() || text.size() > kMaximumVersionBytes) {
        return false;
    }

    std::array<std::uint32_t, 3> components{};
    std::size_t position = 0;
    for (std::size_t component = 0; component < components.size(); ++component) {
        if (position >= text.size() || !IsAsciiDigit(text[position])) {
            return false;
        }

        if (text[position] == '0' && position + 1U < text.size() &&
            IsAsciiDigit(text[position + 1U])) {
            return false;
        }

        std::uint32_t value = 0;
        while (position < text.size() && IsAsciiDigit(text[position])) {
            const std::uint32_t digit =
                static_cast<std::uint32_t>(text[position] - '0');
            if (value > ((std::numeric_limits<std::uint32_t>::max)() - digit) /
                            10U) {
                return false;
            }
            value = value * 10U + digit;
            ++position;
        }
        components[component] = value;

        if (component + 1U < components.size()) {
            if (position >= text.size() || text[position] != '.') {
                return false;
            }
            ++position;
        }
    }

    if (position != text.size()) {
        return false;
    }

    version = SemanticVersion{components[0], components[1], components[2]};
    return true;
}

int CompareSemanticVersions(const SemanticVersion& left,
                            const SemanticVersion& right) noexcept {
    if (left.major != right.major) {
        return left.major < right.major ? -1 : 1;
    }
    if (left.minor != right.minor) {
        return left.minor < right.minor ? -1 : 1;
    }
    if (left.patch != right.patch) {
        return left.patch < right.patch ? -1 : 1;
    }
    return 0;
}

bool CompareSemanticVersionStrings(std::string_view left,
                                   std::string_view right,
                                   int& comparison) noexcept {
    comparison = 0;
    SemanticVersion parsedLeft;
    SemanticVersion parsedRight;
    if (!ParseSemanticVersion(left, parsedLeft) ||
        !ParseSemanticVersion(right, parsedRight)) {
        return false;
    }
    comparison = CompareSemanticVersions(parsedLeft, parsedRight);
    return true;
}

UpdateManifestError ParseUpdateManifest(std::string_view json,
                                        UpdateManifest& manifest) noexcept {
    if (json.size() > kMaximumUpdateManifestBytes) {
        return UpdateManifestError::ManifestTooLarge;
    }

    try {
        JsonCursor cursor(json);
        cursor.SkipWhitespace();
        if (!cursor.Consume('{')) {
            return UpdateManifestError::InvalidJson;
        }

        UpdateManifest candidate;
        bool hasVersion = false;
        bool hasDownloadUrl = false;
        cursor.SkipWhitespace();
        if (!cursor.Consume('}')) {
            while (true) {
                std::string key;
                if (!cursor.ParseAsciiString(key, 64U)) {
                    return UpdateManifestError::InvalidJson;
                }
                cursor.SkipWhitespace();
                if (!cursor.Consume(':')) {
                    return UpdateManifestError::InvalidJson;
                }
                cursor.SkipWhitespace();

                if (key == "version") {
                    if (hasVersion) {
                        return UpdateManifestError::DuplicateField;
                    }
                    if (!cursor.ParseAsciiString(candidate.version,
                                                 kMaximumVersionBytes)) {
                        return UpdateManifestError::InvalidJson;
                    }
                    hasVersion = true;
                } else if (key == "download_url") {
                    if (hasDownloadUrl) {
                        return UpdateManifestError::DuplicateField;
                    }
                    if (!cursor.ParseAsciiString(candidate.downloadUrl,
                                                 kMaximumDownloadUrlBytes)) {
                        return UpdateManifestError::InvalidJson;
                    }
                    hasDownloadUrl = true;
                } else {
                    return UpdateManifestError::UnexpectedField;
                }

                cursor.SkipWhitespace();
                if (cursor.Consume('}')) {
                    break;
                }
                if (!cursor.Consume(',')) {
                    return UpdateManifestError::InvalidJson;
                }
                cursor.SkipWhitespace();
            }
        }

        cursor.SkipWhitespace();
        if (!cursor.AtEnd()) {
            return UpdateManifestError::InvalidJson;
        }
        if (!hasVersion) {
            return UpdateManifestError::MissingVersion;
        }
        if (!hasDownloadUrl) {
            return UpdateManifestError::MissingDownloadUrl;
        }
        if (!ParseSemanticVersion(candidate.version,
                                  candidate.parsedVersion)) {
            return UpdateManifestError::InvalidVersion;
        }
        if (!IsExpectedDownloadUrl(candidate)) {
            return UpdateManifestError::InvalidDownloadUrl;
        }

        manifest = std::move(candidate);
        return UpdateManifestError::None;
    } catch (const std::bad_alloc&) {
        return UpdateManifestError::OutOfMemory;
    } catch (...) {
        return UpdateManifestError::InvalidJson;
    }
}

const wchar_t* UpdateManifestErrorMessage(UpdateManifestError error) noexcept {
    switch (error) {
    case UpdateManifestError::None:
        return L"The update manifest is valid.";
    case UpdateManifestError::ManifestTooLarge:
        return L"The update manifest exceeded ChromeMic's 16 KiB safety limit.";
    case UpdateManifestError::InvalidJson:
        return L"The update manifest was not valid strict JSON.";
    case UpdateManifestError::DuplicateField:
        return L"The update manifest contained a duplicate field.";
    case UpdateManifestError::UnexpectedField:
        return L"The update manifest contained an unexpected field.";
    case UpdateManifestError::MissingVersion:
        return L"The update manifest did not contain a version.";
    case UpdateManifestError::MissingDownloadUrl:
        return L"The update manifest did not contain a download URL.";
    case UpdateManifestError::InvalidVersion:
        return L"The update manifest contained an invalid semantic version.";
    case UpdateManifestError::InvalidDownloadUrl:
        return L"The update download URL did not match ChromeMic's GitHub "
               L"release path.";
    case UpdateManifestError::OutOfMemory:
        return L"There was not enough memory to read the update manifest.";
    default:
        return L"The update manifest could not be validated.";
    }
}

bool UpdateCheckResult::Succeeded() const noexcept {
    return status == UpdateCheckStatus::UpdateAvailable ||
           status == UpdateCheckStatus::UpToDate;
}

UpdateCheckResult CheckForChromeMicUpdates() noexcept {
    try {
        const WinHttpHandle session(WinHttpOpen(
            L"ChromeMic/1.2.0 update checker",
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0));
        if (!session) {
            return MakeWinHttpFailure(GetLastError());
        }
        if (!WinHttpSetTimeouts(session.Get(),
                                kResolveTimeoutMilliseconds,
                                kConnectTimeoutMilliseconds,
                                kSendTimeoutMilliseconds,
                                kReceiveTimeoutMilliseconds)) {
            return MakeWinHttpFailure(GetLastError());
        }

        const WinHttpHandle connection(WinHttpConnect(session.Get(),
                                                       kUpdateHost,
                                                       INTERNET_DEFAULT_HTTPS_PORT,
                                                       0));
        if (!connection) {
            return MakeWinHttpFailure(GetLastError());
        }

        const wchar_t* acceptedTypes[] = {
            L"application/json", L"text/plain", nullptr};
        const WinHttpHandle request(WinHttpOpenRequest(
            connection.Get(),
            L"GET",
            kUpdatePath,
            nullptr,
            WINHTTP_NO_REFERER,
            acceptedTypes,
            WINHTTP_FLAG_SECURE | WINHTTP_FLAG_REFRESH));
        if (!request) {
            return MakeWinHttpFailure(GetLastError());
        }

        DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        if (!WinHttpSetOption(request.Get(),
                              WINHTTP_OPTION_REDIRECT_POLICY,
                              &redirectPolicy,
                              sizeof(redirectPolicy))) {
            return MakeWinHttpFailure(GetLastError());
        }

        // WinHTTP's normal certificate checks do not enable revocation checks by
        // default. Updates fail closed when that stronger validation cannot be
        // enabled or when the server certificate has been revoked.
        DWORD enabledFeature = WINHTTP_ENABLE_SSL_REVOCATION;
        if (!WinHttpSetOption(request.Get(),
                              WINHTTP_OPTION_ENABLE_FEATURE,
                              &enabledFeature,
                              sizeof(enabledFeature))) {
            return MakeWinHttpFailure(GetLastError());
        }

        if (!WinHttpSendRequest(request.Get(),
                                kNoCacheHeaders,
                                kNoCacheHeadersLength,
                                WINHTTP_NO_REQUEST_DATA,
                                0,
                                0,
                                0)) {
            return MakeWinHttpFailure(GetLastError());
        }
        if (!WinHttpReceiveResponse(request.Get(), nullptr)) {
            return MakeWinHttpFailure(GetLastError());
        }

        DWORD statusCode = 0;
        DWORD statusCodeBytes = sizeof(statusCode);
        if (!WinHttpQueryHeaders(
                request.Get(),
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &statusCode,
                &statusCodeBytes,
                WINHTTP_NO_HEADER_INDEX)) {
            return MakeWinHttpFailure(GetLastError());
        }
        if (statusCode != HTTP_STATUS_OK) {
            return MakeFailure(
                UpdateCheckStatus::HttpStatusError,
                L"GitHub did not return the update manifest. Try again later.",
                0,
                statusCode);
        }

        std::array<wchar_t, 256> contentTypeBuffer{};
        DWORD contentTypeBytes =
            static_cast<DWORD>(sizeof(contentTypeBuffer));
        if (!WinHttpQueryHeaders(request.Get(),
                                 WINHTTP_QUERY_CONTENT_TYPE,
                                 WINHTTP_HEADER_NAME_BY_INDEX,
                                 contentTypeBuffer.data(),
                                 &contentTypeBytes,
                                 WINHTTP_NO_HEADER_INDEX)) {
            return MakeFailure(
                UpdateCheckStatus::InvalidContentType,
                L"GitHub returned an update response without a supported "
                L"content type.",
                GetLastError(),
                statusCode);
        }
        if ((contentTypeBytes % sizeof(wchar_t)) != 0U) {
            return MakeFailure(
                UpdateCheckStatus::InvalidContentType,
                L"GitHub returned a malformed update content type.",
                0,
                statusCode);
        }
        std::size_t contentTypeCharacters =
            static_cast<std::size_t>(contentTypeBytes / sizeof(wchar_t));
        if (contentTypeCharacters > 0U &&
            contentTypeBuffer[contentTypeCharacters - 1U] == L'\0') {
            --contentTypeCharacters;
        }
        const std::wstring_view contentType(contentTypeBuffer.data(),
                                            contentTypeCharacters);
        if (!IsAcceptableContentType(contentType)) {
            return MakeFailure(
                UpdateCheckStatus::InvalidContentType,
                L"GitHub returned an unsupported update content type.",
                0,
                statusCode);
        }

        DWORD contentLength = 0;
        DWORD contentLengthBytes = sizeof(contentLength);
        if (WinHttpQueryHeaders(
                request.Get(),
                WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &contentLength,
                &contentLengthBytes,
                WINHTTP_NO_HEADER_INDEX)) {
            if (contentLength > kMaximumUpdateManifestBytes) {
                return MakeFailure(
                    UpdateCheckStatus::ResponseTooLarge,
                    L"GitHub's update response exceeded ChromeMic's 16 KiB "
                    L"safety limit.",
                    0,
                    statusCode);
            }
        } else {
            const DWORD contentLengthError = GetLastError();
            if (contentLengthError != ERROR_WINHTTP_HEADER_NOT_FOUND) {
                return MakeWinHttpFailure(contentLengthError);
            }
        }

        std::string body;
        if (contentLength != 0U) {
            body.reserve(static_cast<std::size_t>(contentLength));
        }
        std::array<char, 4096> buffer{};
        while (true) {
            DWORD bytesRead = 0;
            if (!WinHttpReadData(request.Get(),
                                 buffer.data(),
                                 static_cast<DWORD>(buffer.size()),
                                 &bytesRead)) {
                return MakeWinHttpFailure(GetLastError());
            }
            if (bytesRead == 0U) {
                break;
            }
            const std::size_t received = static_cast<std::size_t>(bytesRead);
            if (body.size() > kMaximumUpdateManifestBytes - received) {
                return MakeFailure(
                    UpdateCheckStatus::ResponseTooLarge,
                    L"GitHub's update response exceeded ChromeMic's 16 KiB "
                    L"safety limit.",
                    0,
                    statusCode);
            }
            body.append(buffer.data(), received);
        }

        UpdateManifest manifest;
        const UpdateManifestError manifestError =
            ParseUpdateManifest(body, manifest);
        if (manifestError != UpdateManifestError::None) {
            return MakeFailure(StatusForManifestError(manifestError),
                               UpdateManifestErrorMessage(manifestError),
                               0,
                               statusCode);
        }

        SemanticVersion localVersion;
        if (!ParseSemanticVersion(kChromeMicCurrentVersion, localVersion)) {
            return MakeFailure(
                UpdateCheckStatus::InternalError,
                L"ChromeMic's built-in version is invalid. Reinstall ChromeMic.",
                0,
                statusCode);
        }

        UpdateCheckResult result;
        result.manifest = std::move(manifest);
        result.httpStatus = statusCode;
        const int versionComparison = CompareSemanticVersions(
            result.manifest.parsedVersion, localVersion);
        if (versionComparison > 0) {
            result.status = UpdateCheckStatus::UpdateAvailable;
            result.message =
                L"A newer ChromeMic release is available on GitHub.";
        } else if (versionComparison == 0) {
            result.status = UpdateCheckStatus::UpToDate;
            result.message = L"ChromeMic is already up to date.";
        } else {
            result.status = UpdateCheckStatus::InvalidVersion;
            result.message =
                L"GitHub reports an older version. Routing is locked to "
                L"prevent a stale or rollback manifest.";
        }
        return result;
    } catch (const std::bad_alloc&) {
        return UpdateCheckResult{};
    } catch (...) {
        return UpdateCheckResult{};
    }
}
