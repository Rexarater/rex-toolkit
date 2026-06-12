#include "UpdateChecker.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cwctype>
#include <limits>
#include <map>
#include <sstream>

namespace
{
constexpr DWORD kRequestTimeoutMs = 8000;
constexpr size_t kMaxManifestBytes = 512 * 1024;

std::wstring Utf8ToWide(const std::string& text)
{
    if (text.empty())
    {
        return {};
    }

    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0)
    {
        return {};
    }

    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::wstring Trim(std::wstring value)
{
    const auto first = std::find_if_not(value.begin(), value.end(), [](wchar_t ch)
    {
        return iswspace(ch) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](wchar_t ch)
    {
        return iswspace(ch) != 0;
    }).base();

    if (first >= last)
    {
        return {};
    }
    return std::wstring(first, last);
}

bool IsTimeoutError()
{
    return GetLastError() == ERROR_WINHTTP_TIMEOUT;
}

struct WinHttpHandle
{
    HINTERNET handle = nullptr;

    explicit WinHttpHandle(HINTERNET value = nullptr) : handle(value) {}
    ~WinHttpHandle()
    {
        if (handle)
        {
            WinHttpCloseHandle(handle);
        }
    }

    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;

    operator HINTERNET() const
    {
        return handle;
    }
};

class JsonReader
{
public:
    explicit JsonReader(const std::string& text) : text_(text) {}

    bool ParseObject(std::map<std::string, std::string>& strings, std::map<std::string, std::vector<std::string>>& arrays)
    {
        SkipWhitespace();
        if (!Consume('{'))
        {
            return false;
        }

        SkipWhitespace();
        while (!AtEnd() && Peek() != '}')
        {
            std::string key;
            if (!ParseString(key))
            {
                return false;
            }
            SkipWhitespace();
            if (!Consume(':'))
            {
                return false;
            }
            SkipWhitespace();

            if (Peek() == '"')
            {
                std::string value;
                if (!ParseString(value))
                {
                    return false;
                }
                strings[key] = value;
            }
            else if (Peek() == '[')
            {
                std::vector<std::string> values;
                if (!ParseStringArray(values))
                {
                    return false;
                }
                arrays[key] = values;
            }
            else
            {
                return false;
            }

            SkipWhitespace();
            if (Consume(','))
            {
                SkipWhitespace();
                if (Peek() == '}')
                {
                    break;
                }
                continue;
            }
            break;
        }

        if (!Consume('}'))
        {
            return false;
        }
        SkipWhitespace();
        return AtEnd();
    }

private:
    char Peek() const
    {
        return AtEnd() ? '\0' : text_[position_];
    }

    bool AtEnd() const
    {
        return position_ >= text_.size();
    }

    void SkipWhitespace()
    {
        while (!AtEnd() && std::isspace(static_cast<unsigned char>(text_[position_])) != 0)
        {
            ++position_;
        }
    }

    bool Consume(char expected)
    {
        if (Peek() != expected)
        {
            return false;
        }
        ++position_;
        return true;
    }

    bool ParseString(std::string& output)
    {
        if (!Consume('"'))
        {
            return false;
        }

        output.clear();
        while (!AtEnd())
        {
            const char ch = text_[position_++];
            if (ch == '"')
            {
                return true;
            }
            if (static_cast<unsigned char>(ch) < 0x20)
            {
                return false;
            }
            if (ch != '\\')
            {
                output.push_back(ch);
                continue;
            }

            if (AtEnd())
            {
                return false;
            }
            const char escaped = text_[position_++];
            switch (escaped)
            {
            case '"':
            case '\\':
            case '/':
                output.push_back(escaped);
                break;
            case 'b':
                output.push_back('\b');
                break;
            case 'f':
                output.push_back('\f');
                break;
            case 'n':
                output.push_back('\n');
                break;
            case 'r':
                output.push_back('\r');
                break;
            case 't':
                output.push_back('\t');
                break;
            case 'u':
                if (!SkipUnicodeEscape())
                {
                    return false;
                }
                output.push_back('?');
                break;
            default:
                return false;
            }
        }

        return false;
    }

    bool ParseStringArray(std::vector<std::string>& output)
    {
        if (!Consume('['))
        {
            return false;
        }

        output.clear();
        SkipWhitespace();
        while (!AtEnd() && Peek() != ']')
        {
            std::string value;
            if (!ParseString(value))
            {
                return false;
            }
            output.push_back(value);

            SkipWhitespace();
            if (Consume(','))
            {
                SkipWhitespace();
                if (Peek() == ']')
                {
                    break;
                }
                continue;
            }
            break;
        }

        return Consume(']');
    }

    bool SkipUnicodeEscape()
    {
        if (position_ + 4 > text_.size())
        {
            return false;
        }
        for (int index = 0; index < 4; ++index)
        {
            const unsigned char ch = static_cast<unsigned char>(text_[position_++]);
            if (std::isxdigit(ch) == 0)
            {
                return false;
            }
        }
        return true;
    }

    const std::string& text_;
    size_t position_ = 0;
};

std::wstring StatusMessage(UpdateCheckStatus status)
{
    switch (status)
    {
    case UpdateCheckStatus::InvalidUrl:
        return L"Could not check for updates. The update URL is invalid.";
    case UpdateCheckStatus::InvalidJson:
        return L"Could not check for updates. The update information is invalid.";
    case UpdateCheckStatus::MissingLatestVersion:
        return L"Could not check for updates. The update information is missing latestVersion.";
    case UpdateCheckStatus::MissingDownloadUrl:
        return L"Could not check for updates. The update information is missing downloadUrl.";
    case UpdateCheckStatus::ServerError:
        return L"Could not check for updates. The update server returned an error.";
    case UpdateCheckStatus::Timeout:
        return L"Could not check for updates. The request timed out.";
    case UpdateCheckStatus::VersionComparisonFailed:
        return L"Could not check for updates. Version comparison failed.";
    default:
        return L"Could not check for updates. Please check your internet connection and try again.";
    }
}
}

bool SemanticVersion::TryParse(const std::wstring& text, SemanticVersion& version)
{
    const std::wstring trimmed = Trim(text);
    std::vector<int> parts;
    size_t start = 0;

    while (start <= trimmed.size())
    {
        const size_t dot = trimmed.find(L'.', start);
        const size_t end = dot == std::wstring::npos ? trimmed.size() : dot;
        if (end == start)
        {
            return false;
        }

        long long value = 0;
        for (size_t index = start; index < end; ++index)
        {
            const wchar_t ch = trimmed[index];
            if (ch < L'0' || ch > L'9')
            {
                return false;
            }
            value = (value * 10) + (ch - L'0');
            if (value > std::numeric_limits<int>::max())
            {
                return false;
            }
        }
        parts.push_back(static_cast<int>(value));

        if (dot == std::wstring::npos)
        {
            break;
        }
        start = dot + 1;
    }

    if (parts.size() != 3)
    {
        return false;
    }

    version.major = parts[0];
    version.minor = parts[1];
    version.patch = parts[2];
    return true;
}

int SemanticVersion::CompareTo(const SemanticVersion& other) const
{
    if (major != other.major)
    {
        return major < other.major ? -1 : 1;
    }
    if (minor != other.minor)
    {
        return minor < other.minor ? -1 : 1;
    }
    if (patch != other.patch)
    {
        return patch < other.patch ? -1 : 1;
    }
    return 0;
}

UpdateCheckResult UpdateChecker::CheckForUpdates(const std::wstring& currentVersion, const std::wstring& manifestUrl) const
{
    UpdateCheckResult result;
    result.currentVersion = currentVersion;

    if (!IsSafeHttpUrl(manifestUrl))
    {
        result.status = UpdateCheckStatus::InvalidUrl;
        result.errorMessage = StatusMessage(result.status);
        return result;
    }

    std::string responseBody;
    UpdateCheckStatus fetchStatus = UpdateCheckStatus::NetworkError;
    std::wstring fetchError;
    if (!FetchUrl(manifestUrl, responseBody, fetchError, fetchStatus))
    {
        result.status = fetchStatus;
        result.errorMessage = fetchError.empty() ? StatusMessage(fetchStatus) : fetchError;
        return result;
    }

    UpdateInfo info;
    if (!ParseUpdateInfo(responseBody, info, result))
    {
        if (result.errorMessage.empty())
        {
            result.errorMessage = StatusMessage(result.status);
        }
        return result;
    }

    result.latestVersion = info.latestVersion;
    result.downloadUrl = info.downloadUrl;
    result.releaseNotes = info.releaseNotes;

    if (!IsSafeHttpUrl(result.downloadUrl))
    {
        result.status = UpdateCheckStatus::InvalidUrl;
        result.errorMessage = L"Could not check for updates. The download URL is invalid.";
        return result;
    }

    SemanticVersion localVersion;
    SemanticVersion remoteVersion;
    if (!SemanticVersion::TryParse(currentVersion, localVersion) ||
        !SemanticVersion::TryParse(result.latestVersion, remoteVersion))
    {
        result.status = UpdateCheckStatus::VersionComparisonFailed;
        result.errorMessage = StatusMessage(result.status);
        return result;
    }

    result.status = localVersion.CompareTo(remoteVersion) < 0
        ? UpdateCheckStatus::UpdateAvailable
        : UpdateCheckStatus::UpToDate;
    return result;
}

bool UpdateChecker::IsSafeHttpUrl(const std::wstring& url)
{
    URL_COMPONENTSW components {};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &components))
    {
        return false;
    }

    if (components.dwHostNameLength == 0)
    {
        return false;
    }

    return components.nScheme == INTERNET_SCHEME_HTTP ||
        components.nScheme == INTERNET_SCHEME_HTTPS;
}

bool UpdateChecker::FetchUrl(const std::wstring& url, std::string& responseBody, std::wstring& errorMessage, UpdateCheckStatus& errorStatus)
{
    URL_COMPONENTSW components {};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &components))
    {
        errorStatus = UpdateCheckStatus::InvalidUrl;
        errorMessage = StatusMessage(errorStatus);
        return false;
    }

    const bool secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    if (!secure && components.nScheme != INTERNET_SCHEME_HTTP)
    {
        errorStatus = UpdateCheckStatus::InvalidUrl;
        errorMessage = StatusMessage(errorStatus);
        return false;
    }

    const std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring path;
    if (components.dwUrlPathLength > 0)
    {
        path.assign(components.lpszUrlPath, components.dwUrlPathLength);
    }
    if (components.dwExtraInfoLength > 0)
    {
        path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    if (path.empty())
    {
        path = L"/";
    }

    WinHttpHandle session(WinHttpOpen(
        L"RexToolkitUpdateChecker/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if (!session)
    {
        errorStatus = IsTimeoutError() ? UpdateCheckStatus::Timeout : UpdateCheckStatus::NetworkError;
        errorMessage = StatusMessage(errorStatus);
        return false;
    }

    WinHttpSetTimeouts(session, kRequestTimeoutMs, kRequestTimeoutMs, kRequestTimeoutMs, kRequestTimeoutMs);

    WinHttpHandle connection(WinHttpConnect(session, host.c_str(), components.nPort, 0));
    if (!connection)
    {
        errorStatus = IsTimeoutError() ? UpdateCheckStatus::Timeout : UpdateCheckStatus::NetworkError;
        errorMessage = StatusMessage(errorStatus);
        return false;
    }

    WinHttpHandle request(WinHttpOpenRequest(
        connection,
        L"GET",
        path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request)
    {
        errorStatus = IsTimeoutError() ? UpdateCheckStatus::Timeout : UpdateCheckStatus::NetworkError;
        errorMessage = StatusMessage(errorStatus);
        return false;
    }

    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr))
    {
        errorStatus = IsTimeoutError() ? UpdateCheckStatus::Timeout : UpdateCheckStatus::NetworkError;
        errorMessage = StatusMessage(errorStatus);
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &statusCode,
            &statusCodeSize,
            WINHTTP_NO_HEADER_INDEX))
    {
        errorStatus = UpdateCheckStatus::ServerError;
        errorMessage = StatusMessage(errorStatus);
        return false;
    }

    if (statusCode < 200 || statusCode >= 300)
    {
        errorStatus = UpdateCheckStatus::ServerError;
        std::wostringstream message;
        message << L"Could not check for updates. The server returned HTTP " << statusCode << L".";
        errorMessage = message.str();
        return false;
    }

    responseBody.clear();
    for (;;)
    {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available))
        {
            errorStatus = IsTimeoutError() ? UpdateCheckStatus::Timeout : UpdateCheckStatus::NetworkError;
            errorMessage = StatusMessage(errorStatus);
            return false;
        }
        if (available == 0)
        {
            break;
        }

        if (responseBody.size() + available > kMaxManifestBytes)
        {
            errorStatus = UpdateCheckStatus::InvalidJson;
            errorMessage = L"Could not check for updates. The update information is too large.";
            return false;
        }

        std::string buffer(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), available, &read))
        {
            errorStatus = IsTimeoutError() ? UpdateCheckStatus::Timeout : UpdateCheckStatus::NetworkError;
            errorMessage = StatusMessage(errorStatus);
            return false;
        }
        buffer.resize(read);
        responseBody += buffer;
    }

    return true;
}

bool UpdateChecker::ParseUpdateInfo(const std::string& json, UpdateInfo& info, UpdateCheckResult& result)
{
    std::map<std::string, std::string> strings;
    std::map<std::string, std::vector<std::string>> arrays;
    JsonReader reader(json);
    if (!reader.ParseObject(strings, arrays))
    {
        result.status = UpdateCheckStatus::InvalidJson;
        result.errorMessage = StatusMessage(result.status);
        return false;
    }

    const auto latest = strings.find("latestVersion");
    if (latest == strings.end() || latest->second.empty())
    {
        result.status = UpdateCheckStatus::MissingLatestVersion;
        result.errorMessage = StatusMessage(result.status);
        return false;
    }

    const auto download = strings.find("downloadUrl");
    if (download == strings.end() || download->second.empty())
    {
        result.status = UpdateCheckStatus::MissingDownloadUrl;
        result.errorMessage = StatusMessage(result.status);
        return false;
    }

    info.latestVersion = Utf8ToWide(latest->second);
    info.downloadUrl = Utf8ToWide(download->second);
    const auto notes = arrays.find("releaseNotes");
    if (notes != arrays.end())
    {
        for (const std::string& note : notes->second)
        {
            info.releaseNotes.push_back(Utf8ToWide(note));
        }
    }

    if (info.latestVersion.empty() || info.downloadUrl.empty())
    {
        result.status = UpdateCheckStatus::InvalidJson;
        result.errorMessage = StatusMessage(result.status);
        return false;
    }

    return true;
}
