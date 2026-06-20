#include "SmartFileTransferService.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <natupnp.h>
#include <oleauto.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <unordered_map>
#include <vector>

#ifdef REX_TOOLKIT_HAS_LIBDATACHANNEL
#include <rtc/rtc.hpp>
#endif

namespace
{
constexpr size_t kSocketBufferSize = 64 * 1024;
constexpr int kListenBacklog = 8;

long long EpochSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

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

std::string WideToUtf8(const std::wstring& text)
{
    if (text.empty())
    {
        return {};
    }

    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0)
    {
        return {};
    }

    std::string utf8(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), length, nullptr, nullptr);
    return utf8;
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

std::wstring NormalizeTransferCodeText(const std::wstring& text)
{
    auto isKnownField = [](const std::wstring& line) -> bool
    {
        static const std::array<const wchar_t*, 11> prefixes = {
            L"session=",
            L"token=",
            L"expires=",
            L"lan=",
            L"direct=",
            L"capabilities=",
            L"role=",
            L"description=",
            L"sdp=",
            L"candidates=",
            L"checksum="
        };
        return std::any_of(prefixes.begin(), prefixes.end(), [&](const wchar_t* prefix)
        {
            return line.rfind(prefix, 0) == 0;
        });
    };

    std::wstring normalized;
    normalized.reserve(text.size());
    for (wchar_t ch : text)
    {
        if (ch == L'\r')
        {
            continue;
        }
        normalized.push_back(ch);
    }

    std::wistringstream input(Trim(normalized));
    std::vector<std::wstring> lines;
    std::wstring line;
    while (std::getline(input, line))
    {
        line = Trim(line);
        if (line.empty())
        {
            continue;
        }

        const bool startsNewField = isKnownField(line) ||
            line == L"RXT1" ||
            line == L"RXP2P1";
        if (startsNewField || lines.empty())
        {
            lines.push_back(line);
        }
        else
        {
            lines.back() += line;
        }
    }

    std::wstring output;
    for (const std::wstring& normalizedLine : lines)
    {
        if (!output.empty())
        {
            output.push_back(L'\n');
        }
        output += normalizedLine;
    }
    return output;
}

std::wstring ToLower(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch)
    {
        return static_cast<wchar_t>(towlower(ch));
    });
    return value;
}

std::wstring BytesLabel(unsigned long long bytes)
{
    const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    double value = static_cast<double>(bytes);
    int unitIndex = 0;
    while (value >= 1024.0 && unitIndex < 4)
    {
        value /= 1024.0;
        ++unitIndex;
    }

    std::wostringstream output;
    if (unitIndex == 0)
    {
        output << static_cast<unsigned long long>(value) << L" " << units[unitIndex];
    }
    else
    {
        output.setf(std::ios::fixed);
        output.precision(1);
        output << value << L" " << units[unitIndex];
    }
    return output.str();
}

std::string JsonEscapeUtf8(const std::wstring& value)
{
    std::string text = WideToUtf8(value);
    std::string escaped;
    escaped.reserve(text.size() + 8);
    for (char ch : text)
    {
        switch (ch)
        {
        case '"':
            escaped += "\\\"";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        case '\b':
            escaped += "\\b";
            break;
        case '\f':
            escaped += "\\f";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20)
            {
                escaped += '?';
            }
            else
            {
                escaped += ch;
            }
            break;
        }
    }
    return escaped;
}

std::wstring UrlEncode(const std::wstring& value)
{
    const std::string utf8 = WideToUtf8(value);
    std::wostringstream output;
    output << std::uppercase << std::hex;
    for (unsigned char ch : utf8)
    {
        if ((ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~')
        {
            output << static_cast<wchar_t>(ch);
        }
        else
        {
            output << L'%' << std::setw(2) << std::setfill(L'0') << static_cast<int>(ch);
        }
    }
    return output.str();
}

std::wstring UrlDecodeAscii(const std::string& value)
{
    std::string decoded;
    decoded.reserve(value.size());
    for (size_t index = 0; index < value.size(); ++index)
    {
        if (value[index] == '%' && index + 2 < value.size())
        {
            const std::string hex = value.substr(index + 1, 2);
            char* end = nullptr;
            const long parsed = std::strtol(hex.c_str(), &end, 16);
            if (end && *end == '\0')
            {
                decoded.push_back(static_cast<char>(parsed));
                index += 2;
                continue;
            }
        }
        else if (value[index] == '+')
        {
            decoded.push_back(' ');
            continue;
        }
        decoded.push_back(value[index]);
    }
    return Utf8ToWide(decoded);
}

std::wstring LocalIpv4Address()
{
    char hostName[256] {};
    if (gethostname(hostName, static_cast<int>(std::size(hostName))) != 0)
    {
        return L"127.0.0.1";
    }

    addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* results = nullptr;
    if (getaddrinfo(hostName, nullptr, &hints, &results) != 0 || !results)
    {
        return L"127.0.0.1";
    }

    std::wstring fallback = L"127.0.0.1";
    for (addrinfo* item = results; item; item = item->ai_next)
    {
        auto* address = reinterpret_cast<sockaddr_in*>(item->ai_addr);
        char buffer[INET_ADDRSTRLEN] {};
        if (!inet_ntop(AF_INET, &address->sin_addr, buffer, static_cast<DWORD>(std::size(buffer))))
        {
            continue;
        }
        std::wstring ip = Utf8ToWide(buffer);
        if (ip.rfind(L"127.", 0) != 0)
        {
            freeaddrinfo(results);
            return ip;
        }
        fallback = ip;
    }

    freeaddrinfo(results);
    return fallback;
}

std::wstring QueryValue(const std::string& query, const std::string& key)
{
    size_t position = 0;
    while (position < query.size())
    {
        const size_t next = query.find('&', position);
        const std::string pair = query.substr(position, next == std::string::npos ? std::string::npos : next - position);
        const size_t separator = pair.find('=');
        if (separator != std::string::npos && pair.substr(0, separator) == key)
        {
            return UrlDecodeAscii(pair.substr(separator + 1));
        }
        if (next == std::string::npos)
        {
            break;
        }
        position = next + 1;
    }
    return {};
}

std::vector<std::string> SplitString(const std::string& text, char delimiter)
{
    std::vector<std::string> parts;
    size_t position = 0;
    while (position <= text.size())
    {
        const size_t next = text.find(delimiter, position);
        parts.push_back(text.substr(position, next == std::string::npos ? std::string::npos : next - position));
        if (next == std::string::npos)
        {
            break;
        }
        position = next + 1;
    }
    return parts;
}

unsigned long long Fnv1aWide(const std::wstring& text)
{
    unsigned long long checksum = 1469598103934665603ull;
    for (wchar_t ch : text)
    {
        checksum ^= static_cast<unsigned long long>(ch);
        checksum *= 1099511628211ull;
    }
    return checksum;
}

std::wstring Base64Encode(const std::string& bytes)
{
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::wstring output;
    output.reserve(((bytes.size() + 2) / 3) * 4);
    for (size_t index = 0; index < bytes.size(); index += 3)
    {
        const unsigned int b0 = static_cast<unsigned char>(bytes[index]);
        const unsigned int b1 = index + 1 < bytes.size() ? static_cast<unsigned char>(bytes[index + 1]) : 0;
        const unsigned int b2 = index + 2 < bytes.size() ? static_cast<unsigned char>(bytes[index + 2]) : 0;
        output.push_back(static_cast<wchar_t>(table[(b0 >> 2) & 0x3F]));
        output.push_back(static_cast<wchar_t>(table[((b0 & 0x03) << 4) | ((b1 >> 4) & 0x0F)]));
        output.push_back(index + 1 < bytes.size() ? static_cast<wchar_t>(table[((b1 & 0x0F) << 2) | ((b2 >> 6) & 0x03)]) : L'=');
        output.push_back(index + 2 < bytes.size() ? static_cast<wchar_t>(table[b2 & 0x3F]) : L'=');
    }
    return output;
}

bool Base64Decode(const std::wstring& text, std::string& bytes)
{
    auto valueOf = [](wchar_t ch) -> int
    {
        if (ch >= L'A' && ch <= L'Z') return static_cast<int>(ch - L'A');
        if (ch >= L'a' && ch <= L'z') return static_cast<int>(ch - L'a') + 26;
        if (ch >= L'0' && ch <= L'9') return static_cast<int>(ch - L'0') + 52;
        if (ch == L'+') return 62;
        if (ch == L'/') return 63;
        if (ch == L'=') return -2;
        return -1;
    };

    bytes.clear();
    int buffer = 0;
    int bits = -8;
    for (wchar_t ch : text)
    {
        if (iswspace(ch))
        {
            continue;
        }
        const int value = valueOf(ch);
        if (value == -2)
        {
            break;
        }
        if (value < 0)
        {
            return false;
        }
        buffer = (buffer << 6) | value;
        bits += 6;
        if (bits >= 0)
        {
            bytes.push_back(static_cast<char>((buffer >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return true;
}

struct WebRtcPairingCode
{
    std::wstring role;
    std::wstring sessionId;
    std::wstring descriptionType;
    std::string sdp;
    std::vector<std::pair<std::string, std::string>> candidates;
    long long expiresAt = 0;
};

std::wstring EncodeWebRtcPairingCode(const WebRtcPairingCode& code)
{
    std::string candidateText;
    for (const auto& candidate : code.candidates)
    {
        if (!candidateText.empty())
        {
            candidateText += '\n';
        }
        candidateText += candidate.first;
        candidateText += '|';
        candidateText += candidate.second;
    }

    std::wstring payload =
        L"RXP2P1\nrole=" + code.role +
        L"\nsession=" + code.sessionId +
        L"\nexpires=" + std::to_wstring(code.expiresAt) +
        L"\ndescription=" + code.descriptionType +
        L"\nsdp=" + Base64Encode(code.sdp) +
        L"\ncandidates=" + Base64Encode(candidateText);

    std::wostringstream output;
    output << payload << L"\nchecksum=" << std::hex << Fnv1aWide(payload);
    return output.str();
}

bool DecodeWebRtcPairingCode(const std::wstring& text, WebRtcPairingCode& code, std::wstring& errorMessage)
{
    code = {};
    const std::wstring trimmedCode = NormalizeTransferCodeText(text);
    const std::wstring checksumMarker = L"\nchecksum=";
    const size_t checksumPosition = trimmedCode.rfind(checksumMarker);
    if (checksumPosition == std::wstring::npos)
    {
        errorMessage = L"Invalid pairing code. The integrity check is missing.";
        return false;
    }

    const std::wstring payload = trimmedCode.substr(0, checksumPosition);
    const std::wstring expectedChecksum = Trim(trimmedCode.substr(checksumPosition + checksumMarker.size()));
    std::wostringstream checksumText;
    checksumText << std::hex << Fnv1aWide(payload);
    if (ToLower(checksumText.str()) != ToLower(expectedChecksum))
    {
        errorMessage = L"Invalid pairing code. The integrity check failed.";
        return false;
    }

    std::wistringstream stream(payload);
    std::wstring firstLine;
    std::getline(stream, firstLine);
    if (Trim(firstLine) != L"RXP2P1")
    {
        errorMessage = L"Unsupported pairing code version.";
        return false;
    }

    std::map<std::wstring, std::wstring> values;
    std::wstring line;
    while (std::getline(stream, line))
    {
        const size_t separator = line.find(L'=');
        if (separator == std::wstring::npos)
        {
            continue;
        }
        values[Trim(line.substr(0, separator))] = Trim(line.substr(separator + 1));
    }

    code.role = values[L"role"];
    code.sessionId = values[L"session"];
    code.descriptionType = values[L"description"];
    try
    {
        code.expiresAt = std::stoll(values[L"expires"]);
    }
    catch (...)
    {
        code.expiresAt = 0;
    }
    if (TransferSecurityService::IsExpired(code.expiresAt))
    {
        errorMessage = L"That pairing code has expired.";
        return false;
    }

    std::string decodedSdp;
    std::string decodedCandidates;
    if (!Base64Decode(values[L"sdp"], decodedSdp) || decodedSdp.empty())
    {
        errorMessage = L"Invalid pairing code. The connection description could not be read.";
        return false;
    }
    if (!Base64Decode(values[L"candidates"], decodedCandidates))
    {
        errorMessage = L"Invalid pairing code. The network candidates could not be read.";
        return false;
    }

    code.sdp = decodedSdp;
    for (const std::string& lineText : SplitString(decodedCandidates, '\n'))
    {
        if (lineText.empty())
        {
            continue;
        }
        const size_t separator = lineText.find('|');
        if (separator == std::string::npos)
        {
            continue;
        }
        code.candidates.emplace_back(lineText.substr(0, separator), lineText.substr(separator + 1));
    }
    return !code.sessionId.empty() && !code.role.empty() && !code.descriptionType.empty();
}

bool SendAll(SOCKET socket, const char* data, size_t size)
{
    size_t sent = 0;
    while (sent < size)
    {
        const int chunk = send(socket, data + sent, static_cast<int>(std::min<size_t>(size - sent, 64 * 1024)), 0);
        if (chunk <= 0)
        {
            return false;
        }
        sent += static_cast<size_t>(chunk);
    }
    return true;
}

void SendTextResponse(SOCKET socket, int status, const char* statusText, const std::string& body, const char* contentType = "text/plain; charset=utf-8")
{
    std::ostringstream response;
    response << "HTTP/1.1 " << status << ' ' << statusText << "\r\n"
        << "Connection: close\r\n"
        << "Content-Type: " << contentType << "\r\n"
        << "Content-Length: " << body.size() << "\r\n\r\n"
        << body;
    const std::string text = response.str();
    SendAll(socket, text.data(), text.size());
}

std::string ReadHttpRequest(SOCKET socket)
{
    std::string request;
    std::array<char, 2048> buffer {};
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384)
    {
        const int read = recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (read <= 0)
        {
            break;
        }
        request.append(buffer.data(), static_cast<size_t>(read));
    }
    return request;
}

std::wstring WinHttpErrorMessage()
{
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        GetLastError(),
        0,
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);
    std::wstring message = length > 0 && buffer ? std::wstring(buffer, length) : L"Network request failed.";
    if (buffer)
    {
        LocalFree(buffer);
    }
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L'.'))
    {
        message.pop_back();
    }
    return message;
}

struct ParsedHttpUrl
{
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTP_PORT;
    DWORD flags = 0;
};

std::optional<ParsedHttpUrl> ParseHttpUrl(const std::wstring& url)
{
    URL_COMPONENTSW components {};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &components))
    {
        return std::nullopt;
    }

    if (components.nScheme != INTERNET_SCHEME_HTTP && components.nScheme != INTERNET_SCHEME_HTTPS)
    {
        return std::nullopt;
    }

    ParsedHttpUrl parsed;
    parsed.host.assign(components.lpszHostName, components.dwHostNameLength);
    parsed.path.assign(components.lpszUrlPath, components.dwUrlPathLength);
    parsed.path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    if (parsed.path.empty())
    {
        parsed.path = L"/";
    }
    parsed.port = components.nPort;
    parsed.flags = components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    return parsed;
}

bool HttpGetText(
    const std::wstring& url,
    DWORD resolveTimeoutMs,
    DWORD connectTimeoutMs,
    DWORD sendTimeoutMs,
    DWORD receiveTimeoutMs,
    std::string& body,
    unsigned long& statusCode,
    std::wstring& errorMessage)
{
    body.clear();
    statusCode = 0;
    const auto parsed = ParseHttpUrl(url);
    if (!parsed)
    {
        errorMessage = L"Invalid transfer URL.";
        return false;
    }

    HINTERNET session = WinHttpOpen(L"RexToolkitSmartTransfer/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
    {
        errorMessage = WinHttpErrorMessage();
        return false;
    }
    WinHttpSetTimeouts(session, static_cast<int>(resolveTimeoutMs), static_cast<int>(connectTimeoutMs), static_cast<int>(sendTimeoutMs), static_cast<int>(receiveTimeoutMs));
    HINTERNET connection = WinHttpConnect(session, parsed->host.c_str(), parsed->port, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", parsed->path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, parsed->flags) : nullptr;
    const bool sent = request &&
        WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr);
    if (!sent)
    {
        errorMessage = WinHttpErrorMessage();
        if (request) WinHttpCloseHandle(request);
        if (connection) WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

    for (;;)
    {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0)
        {
            break;
        }
        std::string buffer(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), available, &read) || read == 0)
        {
            break;
        }
        body.append(buffer.data(), read);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return true;
}

bool Ipv4ToUInt32(const std::wstring& value, unsigned long& address)
{
    in_addr parsed {};
    if (InetPtonW(AF_INET, value.c_str(), &parsed) != 1)
    {
        return false;
    }
    address = ntohl(parsed.S_un.S_addr);
    return true;
}

bool Ipv4InRange(unsigned long address, unsigned long prefix, int bits)
{
    const unsigned long mask = bits == 0 ? 0 : (0xFFFFFFFFul << (32 - bits));
    return (address & mask) == (prefix & mask);
}

bool IsPublicIpv4Address(const std::wstring& value)
{
    unsigned long address = 0;
    if (!Ipv4ToUInt32(value, address))
    {
        return false;
    }

    return
        !Ipv4InRange(address, 0x00000000ul, 8) &&
        !Ipv4InRange(address, 0x0A000000ul, 8) &&
        !Ipv4InRange(address, 0x64400000ul, 10) &&
        !Ipv4InRange(address, 0x7F000000ul, 8) &&
        !Ipv4InRange(address, 0xA9FE0000ul, 16) &&
        !Ipv4InRange(address, 0xAC100000ul, 12) &&
        !Ipv4InRange(address, 0xC0A80000ul, 16) &&
        !Ipv4InRange(address, 0xC6120000ul, 15) &&
        !Ipv4InRange(address, 0xE0000000ul, 4) &&
        address != 0xFFFFFFFFul;
}

std::wstring JsonStringValue(const std::string& object, const std::string& key)
{
    const std::string needle = "\"" + key + "\"";
    size_t position = object.find(needle);
    if (position == std::string::npos)
    {
        return {};
    }
    position = object.find(':', position + needle.size());
    if (position == std::string::npos)
    {
        return {};
    }
    position = object.find('"', position + 1);
    if (position == std::string::npos)
    {
        return {};
    }
    ++position;

    std::string value;
    while (position < object.size())
    {
        const char ch = object[position++];
        if (ch == '"')
        {
            break;
        }
        if (ch == '\\' && position < object.size())
        {
            const char escaped = object[position++];
            switch (escaped)
            {
            case '"':
            case '\\':
            case '/':
                value.push_back(escaped);
                break;
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            default:
                value.push_back('?');
                break;
            }
        }
        else
        {
            value.push_back(ch);
        }
    }
    return Utf8ToWide(value);
}

unsigned long long JsonNumberValue(const std::string& object, const std::string& key)
{
    const std::string needle = "\"" + key + "\"";
    size_t position = object.find(needle);
    if (position == std::string::npos)
    {
        return 0;
    }
    position = object.find(':', position + needle.size());
    if (position == std::string::npos)
    {
        return 0;
    }
    ++position;
    while (position < object.size() && std::isspace(static_cast<unsigned char>(object[position])) != 0)
    {
        ++position;
    }
    unsigned long long value = 0;
    while (position < object.size() && std::isdigit(static_cast<unsigned char>(object[position])) != 0)
    {
        value = (value * 10) + static_cast<unsigned long long>(object[position] - '0');
        ++position;
    }
    return value;
}

std::vector<std::string> JsonObjectsInArray(const std::string& json, const std::string& key)
{
    std::vector<std::string> objects;
    const std::string needle = "\"" + key + "\"";
    size_t position = json.find(needle);
    if (position == std::string::npos)
    {
        return objects;
    }
    position = json.find('[', position + needle.size());
    if (position == std::string::npos)
    {
        return objects;
    }

    int depth = 0;
    size_t objectStart = std::string::npos;
    bool inString = false;
    bool escaped = false;
    for (; position < json.size(); ++position)
    {
        const char ch = json[position];
        if (inString)
        {
            if (escaped)
            {
                escaped = false;
            }
            else if (ch == '\\')
            {
                escaped = true;
            }
            else if (ch == '"')
            {
                inString = false;
            }
            continue;
        }
        if (ch == '"')
        {
            inString = true;
        }
        else if (ch == '{')
        {
            if (depth == 0)
            {
                objectStart = position;
            }
            ++depth;
        }
        else if (ch == '}')
        {
            --depth;
            if (depth == 0 && objectStart != std::string::npos)
            {
                objects.push_back(json.substr(objectStart, position - objectStart + 1));
                objectStart = std::string::npos;
            }
        }
        else if (ch == ']' && depth == 0)
        {
            break;
        }
    }
    return objects;
}

}

std::wstring TransferSecurityService::GenerateHexToken(size_t bytes)
{
    std::vector<unsigned char> randomBytes(bytes, 0);
    if (BCryptGenRandom(nullptr, randomBytes.data(), static_cast<ULONG>(randomBytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
    {
        for (unsigned char& value : randomBytes)
        {
            value = static_cast<unsigned char>(std::chrono::high_resolution_clock::now().time_since_epoch().count() & 0xff);
        }
    }

    std::wostringstream output;
    output << std::hex << std::setfill(L'0');
    for (unsigned char value : randomBytes)
    {
        output << std::setw(2) << static_cast<int>(value);
    }
    return output.str();
}

std::wstring TransferSecurityService::CreateFileId(size_t index)
{
    return L"file_" + std::to_wstring(index + 1) + L"_" + GenerateHexToken(4);
}

std::wstring TransferSecurityService::SafeFileName(const std::wstring& name)
{
    std::wstring safe;
    safe.reserve(name.size());
    for (wchar_t ch : name)
    {
        if (ch < 32 || ch == L'<' || ch == L'>' || ch == L':' || ch == L'"' ||
            ch == L'/' || ch == L'\\' || ch == L'|' || ch == L'?' || ch == L'*')
        {
            safe.push_back(L'_');
        }
        else
        {
            safe.push_back(ch);
        }
    }
    safe = Trim(safe);
    while (!safe.empty() && (safe.back() == L'.' || safe.back() == L' '))
    {
        safe.pop_back();
    }
    return safe.empty() ? L"download" : safe;
}

std::filesystem::path TransferSecurityService::AutoRenamePath(const std::filesystem::path& requestedPath)
{
    if (!std::filesystem::exists(requestedPath))
    {
        return requestedPath;
    }

    const std::filesystem::path folder = requestedPath.parent_path();
    const std::wstring stem = requestedPath.stem().wstring();
    const std::wstring extension = requestedPath.extension().wstring();
    for (int index = 1; index < 10000; ++index)
    {
        std::filesystem::path candidate = folder / (stem + L"_" + std::to_wstring(index) + extension);
        if (!std::filesystem::exists(candidate))
        {
            return candidate;
        }
    }
    return requestedPath;
}

bool TransferSecurityService::IsExpired(long long expiresAt)
{
    return expiresAt > 0 && EpochSeconds() > expiresAt;
}

std::wstring TransferCodeService::Encode(const SmartTransferInvite& invite)
{
    std::wstring payload =
        L"RXT1\nsession=" + invite.sessionId +
        L"\ntoken=" + invite.token +
        L"\nexpires=" + std::to_wstring(invite.expiresAt) +
        L"\nlan=" + invite.lanUrl +
        L"\ndirect=" + invite.directUrl +
        L"\ncapabilities=" + invite.capabilities;

    unsigned long long checksum = 1469598103934665603ull;
    for (wchar_t ch : payload)
    {
        checksum ^= static_cast<unsigned long long>(ch);
        checksum *= 1099511628211ull;
    }
    std::wostringstream output;
    output << payload << L"\nchecksum=" << std::hex << checksum;
    return output.str();
}

bool TransferCodeService::Decode(const std::wstring& code, SmartTransferInvite& invite, std::wstring& errorMessage)
{
    const std::wstring trimmedCode = NormalizeTransferCodeText(code);
    const std::wstring checksumMarker = L"\nchecksum=";
    const size_t checksumPosition = trimmedCode.rfind(checksumMarker);
    if (checksumPosition == std::wstring::npos)
    {
        errorMessage = L"Transfer code is missing its integrity check.";
        return false;
    }

    const std::wstring payload = trimmedCode.substr(0, checksumPosition);
    const std::wstring expectedChecksum = Trim(trimmedCode.substr(checksumPosition + checksumMarker.size()));
    unsigned long long checksum = 1469598103934665603ull;
    for (wchar_t ch : payload)
    {
        checksum ^= static_cast<unsigned long long>(ch);
        checksum *= 1099511628211ull;
    }
    std::wostringstream checksumText;
    checksumText << std::hex << checksum;
    if (ToLower(checksumText.str()) != ToLower(expectedChecksum))
    {
        errorMessage = L"Transfer code integrity check failed.";
        return false;
    }

    std::wistringstream stream(payload);
    std::wstring firstLine;
    std::getline(stream, firstLine);
    if (Trim(firstLine) != L"RXT1")
    {
        errorMessage = L"Unsupported or invalid transfer code.";
        return false;
    }

    std::map<std::wstring, std::wstring> values;
    std::wstring line;
    while (std::getline(stream, line))
    {
        const size_t separator = line.find(L'=');
        if (separator == std::wstring::npos)
        {
            continue;
        }
        values[Trim(line.substr(0, separator))] = Trim(line.substr(separator + 1));
    }

    invite.sessionId = values[L"session"];
    invite.token = values[L"token"];
    invite.lanUrl = values[L"lan"];
    invite.directUrl = values[L"direct"];
    invite.activeUrl.clear();
    invite.capabilities = values[L"capabilities"];
    try
    {
        invite.expiresAt = std::stoll(values[L"expires"]);
    }
    catch (...)
    {
        invite.expiresAt = 0;
    }

    if (invite.sessionId.empty() || invite.token.size() < 32 || invite.lanUrl.empty())
    {
        errorMessage = L"Transfer code is missing required connection details.";
        return false;
    }
    if (TransferSecurityService::IsExpired(invite.expiresAt))
    {
        errorMessage = L"That transfer code has expired.";
        return false;
    }
    return true;
}

TransferHostServer::TransferHostServer() = default;

TransferHostServer::~TransferHostServer()
{
    Stop();
}

bool TransferHostServer::Start(
    const std::vector<SmartTransferFile>& files,
    const SmartTransferSendOptions& options,
    std::wstring& errorMessage)
{
    Stop();
    if (files.empty())
    {
        errorMessage = L"Select at least one file before creating a transfer.";
        return false;
    }

    WSADATA data {};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
    {
        errorMessage = L"Could not start Windows networking.";
        return false;
    }
    winsockStarted_ = true;

    SOCKET serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serverSocket == INVALID_SOCKET)
    {
        errorMessage = L"Could not create the transfer server socket.";
        Stop();
        return false;
    }

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = 0;
    if (bind(serverSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        listen(serverSocket, kListenBacklog) == SOCKET_ERROR)
    {
        closesocket(serverSocket);
        errorMessage = L"Could not start the temporary local transfer server. The port may be blocked.";
        Stop();
        return false;
    }

    int addressLength = sizeof(address);
    if (getsockname(serverSocket, reinterpret_cast<sockaddr*>(&address), &addressLength) == SOCKET_ERROR)
    {
        closesocket(serverSocket);
        errorMessage = L"Could not read the transfer server port.";
        Stop();
        return false;
    }

    const unsigned short serverPort = ntohs(address.sin_port);
    const std::wstring localIp = LocalIpv4Address();

    SmartTransferInvite invite;
    invite.sessionId = TransferSecurityService::GenerateHexToken(12);
    invite.token = TransferSecurityService::GenerateHexToken(24);
    invite.expiresAt = options.expiration == SmartTransferExpiration::Manual
        ? 0
        : EpochSeconds() + (static_cast<int>(options.expiration) * 60);
    invite.lanUrl = L"http://" + localIp + L":" + std::to_wstring(serverPort);
    const bool webRtcAvailable = options.enableWebRtcFallback && WebRtcTransport::IsAvailable();
    invite.capabilities = webRtcAvailable
        ? L"lan_http,direct_host_unavailable,webrtc_manual"
        : L"lan_http,direct_host_unavailable,webrtc_unavailable";

    {
        std::lock_guard<std::mutex> lock(mutex_);
        files_ = files;
        servedFileIds_.clear();
        options_ = options;
        snapshot_ = {};
        snapshot_.status = SmartTransferHostStatus::Hosting;
        snapshot_.sessionId = invite.sessionId;
        snapshot_.token = invite.token;
        snapshot_.transferCode = TransferCodeService::Encode(invite);
        snapshot_.localUrl = invite.lanUrl;
        snapshot_.expiresAt = invite.expiresAt;
        snapshot_.port = serverPort;
        snapshot_.directHostRequested = options.tryDirectHost;
        snapshot_.directHostMessage = options.tryDirectHost
            ? L"Direct Host: preparing temporary internet access..."
            : L"Direct Host is off. Enable it before creating a transfer to try internet hosting.";
        snapshot_.webRtcFallbackEnabled = options.enableWebRtcFallback;
        snapshot_.webRtcDependencyAvailable = WebRtcTransport::IsAvailable();
        snapshot_.webRtcStatus = !options.enableWebRtcFallback
            ? SmartTransferWebRtcStatus::Disabled
            : (WebRtcTransport::IsAvailable() ? SmartTransferWebRtcStatus::ReadyForFallback : SmartTransferWebRtcStatus::DependencyMissing);
        snapshot_.webRtcMessage = !options.enableWebRtcFallback
            ? L"Manual P2P fallback is disabled in settings."
            : (WebRtcTransport::IsAvailable()
                ? L"Manual P2P fallback is available if LAN and Direct Host fail."
                : WebRtcTransport::DependencyMessage());
        if (options.showWebRtcDiagnostics)
        {
            snapshot_.webRtcDiagnostics = L"ICE: unavailable / Data channel: unavailable / Backend: " +
                std::wstring(WebRtcTransport::IsAvailable() ? L"ready" : L"missing native WebRTC library");
        }
        snapshot_.message = options.tryDirectHost
            ? L"Local network transfer is ready. Preparing Direct Host..."
            : L"Waiting for receiver. Local network transfer is ready.";
        for (const SmartTransferFile& file : files_)
        {
            snapshot_.totalBytes += file.size;
        }
    }

    listenSocket_ = static_cast<uintptr_t>(serverSocket);
    stopRequested_ = false;
    serverThread_ = std::thread(&TransferHostServer::ServerLoop, this);

    std::wstring directHostMessage;
    if (options.tryDirectHost && PrepareDirectHost(invite, localIp, serverPort, directHostMessage))
    {
        invite.capabilities = webRtcAvailable
            ? L"lan_http,direct_host,webrtc_manual"
            : L"lan_http,direct_host,webrtc_unavailable";
    }
    else if (options.tryDirectHost)
    {
        invite.capabilities = webRtcAvailable
            ? L"lan_http,direct_host_unavailable,webrtc_manual"
            : L"lan_http,direct_host_unavailable,webrtc_unavailable";
    }

    if (options.tryDirectHost)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.transferCode = TransferCodeService::Encode(invite);
        snapshot_.directUrl = invite.directUrl;
        snapshot_.directHostAvailable = !invite.directUrl.empty();
        snapshot_.portMappingActive = portMappingActive_;
        snapshot_.directHostMessage = directHostMessage.empty()
            ? (invite.directUrl.empty() ? L"Direct Host unavailable." : L"Direct Host is ready.")
            : directHostMessage;
        snapshot_.message = invite.directUrl.empty()
            ? L"LAN transfer is ready. Direct Host is unavailable for this network."
            : L"Transfer code ready. LAN and Direct Host are available.";
    }
    return true;
}

void TransferHostServer::Stop()
{
    stopRequested_ = true;
    std::shared_ptr<WebRtcSenderSession> webRtcSender;
    {
        std::lock_guard<std::mutex> lock(webRtcSenderMutex_);
        webRtcSender = std::move(webRtcSender_);
    }
    if (webRtcSender)
    {
        webRtcSender->Stop();
    }
    CloseListenSocket();
    RemovePortMapping();
    if (serverThread_.joinable() && serverThread_.get_id() != std::this_thread::get_id())
    {
        serverThread_.join();
    }
    if (winsockStarted_)
    {
        WSACleanup();
        winsockStarted_ = false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot_.status != SmartTransferHostStatus::Complete && snapshot_.status != SmartTransferHostStatus::Failed)
    {
        snapshot_.status = SmartTransferHostStatus::Idle;
    }
}

void TransferHostServer::AllowPendingReceiver()
{
    std::shared_ptr<WebRtcSenderSession> webRtcSender;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.approved = true;
        snapshot_.denied = false;
        snapshot_.approvalPending = false;
        snapshot_.status = SmartTransferHostStatus::Hosting;
        snapshot_.message = L"Receiver approved. Waiting for download request.";
    }
    {
        std::lock_guard<std::mutex> lock(webRtcSenderMutex_);
        webRtcSender = webRtcSender_;
    }
    if (webRtcSender)
    {
        webRtcSender->SetApproval(true, false);
    }
}

void TransferHostServer::DenyPendingReceiver()
{
    std::shared_ptr<WebRtcSenderSession> webRtcSender;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.approved = false;
        snapshot_.denied = true;
        snapshot_.approvalPending = false;
        snapshot_.status = SmartTransferHostStatus::Hosting;
        snapshot_.message = L"Receiver denied.";
    }
    {
        std::lock_guard<std::mutex> lock(webRtcSenderMutex_);
        webRtcSender = webRtcSender_;
    }
    if (webRtcSender)
    {
        webRtcSender->SetApproval(false, true);
    }
}

SmartTransferHostSnapshot TransferHostServer::Snapshot() const
{
    SmartTransferHostSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = snapshot_;
    }
    std::shared_ptr<WebRtcSenderSession> webRtcSender;
    {
        std::lock_guard<std::mutex> lock(webRtcSenderMutex_);
        webRtcSender = webRtcSender_;
    }
    if (webRtcSender)
    {
        SmartTransferWebRtcSnapshot webRtc = webRtcSender->Snapshot();
        snapshot.webRtcStatus = webRtc.status;
        snapshot.webRtcMessage = webRtc.message.empty() ? snapshot.webRtcMessage : webRtc.message;
        snapshot.webRtcDiagnostics = webRtc.diagnostics;
        snapshot.senderPairingCode = webRtc.senderPairingCode;
        snapshot.webRtcDependencyAvailable = WebRtcTransport::IsAvailable();
    }
    return snapshot;
}

bool TransferHostServer::CreateWebRtcSenderPairingCode(std::wstring& pairingCode, std::wstring& errorMessage)
{
    if (!WebRtcTransport::IsAvailable())
    {
        errorMessage = WebRtcTransport::DependencyMessage();
        return false;
    }

    std::vector<SmartTransferFile> files;
    SmartTransferSendOptions options;
    SmartTransferInvite invite;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (snapshot_.sessionId.empty() || snapshot_.token.empty())
        {
            errorMessage = L"Create a transfer before creating a sender pairing code.";
            return false;
        }
        files = files_;
        options = options_;
        invite.sessionId = snapshot_.sessionId;
        invite.token = snapshot_.token;
        invite.expiresAt = snapshot_.expiresAt;
        invite.lanUrl = snapshot_.localUrl;
        invite.directUrl = snapshot_.directUrl;
        invite.capabilities = L"lan_http,direct_host_unavailable,webrtc_manual";
        snapshot_.webRtcStatus = SmartTransferWebRtcStatus::Pairing;
        snapshot_.webRtcMessage = L"Creating sender pairing code...";
    }

    std::shared_ptr<WebRtcSenderSession> webRtcSender;
    {
        std::lock_guard<std::mutex> lock(webRtcSenderMutex_);
        if (!webRtcSender_)
        {
            webRtcSender_ = std::make_shared<WebRtcSenderSession>();
        }
        webRtcSender = webRtcSender_;
    }

    const auto approvalCallback = [this](const std::wstring& remote, const std::wstring& requestLabel)
    {
        return ApprovalAccepted(remote, requestLabel);
    };

    if (!webRtcSender->Start(files, options, invite, approvalCallback, pairingCode, errorMessage))
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.webRtcStatus = SmartTransferWebRtcStatus::Failed;
        snapshot_.webRtcMessage = errorMessage.empty() ? L"Could not create sender pairing code." : errorMessage;
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.senderPairingCode = pairingCode;
        snapshot_.webRtcStatus = SmartTransferWebRtcStatus::WaitingForReceiverResponse;
        snapshot_.webRtcMessage = L"Sender pairing code ready. Send it to the receiver, then paste their response code.";
    }
    return true;
}

bool TransferHostServer::ApplyWebRtcReceiverResponse(const std::wstring& responseCode, std::wstring& errorMessage)
{
    std::shared_ptr<WebRtcSenderSession> webRtcSender;
    {
        std::lock_guard<std::mutex> lock(webRtcSenderMutex_);
        webRtcSender = webRtcSender_;
    }
    if (!webRtcSender)
    {
        errorMessage = L"Create a sender pairing code before applying a receiver response.";
        return false;
    }
    if (!webRtcSender->ApplyReceiverResponse(responseCode, errorMessage))
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.webRtcStatus = SmartTransferWebRtcStatus::Failed;
        snapshot_.webRtcMessage = errorMessage.empty() ? L"Could not apply receiver response code." : errorMessage;
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.webRtcStatus = SmartTransferWebRtcStatus::Pairing;
        snapshot_.webRtcMessage = L"Receiver response accepted. Waiting for P2P connection...";
    }
    return true;
}

SmartTransferWebRtcSnapshot TransferHostServer::WebRtcSnapshot() const
{
    std::shared_ptr<WebRtcSenderSession> webRtcSender;
    {
        std::lock_guard<std::mutex> lock(webRtcSenderMutex_);
        webRtcSender = webRtcSender_;
    }
    if (webRtcSender)
    {
        return webRtcSender->Snapshot();
    }
    SmartTransferWebRtcSnapshot snapshot;
    snapshot.status = WebRtcTransport::IsAvailable()
        ? SmartTransferWebRtcStatus::ReadyForFallback
        : SmartTransferWebRtcStatus::DependencyMissing;
    snapshot.message = WebRtcTransport::StatusMessage();
    return snapshot;
}

void TransferHostServer::CloseListenSocket()
{
    const SOCKET serverSocket = static_cast<SOCKET>(listenSocket_);
    if (serverSocket != INVALID_SOCKET)
    {
        shutdown(serverSocket, SD_BOTH);
        closesocket(serverSocket);
        listenSocket_ = static_cast<uintptr_t>(INVALID_SOCKET);
    }
}

bool TransferHostServer::PrepareDirectHost(
    SmartTransferInvite& invite,
    const std::wstring& localIp,
    unsigned short port,
    std::wstring& directHostMessage)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.message = L"Detecting public IP for Direct Host...";
        snapshot_.directHostMessage = L"Direct Host: detecting public IP...";
    }

    std::wstring publicIp;
    std::wstring errorMessage;
    if (!QueryPublicIpv4(publicIp, errorMessage))
    {
        directHostMessage = L"Direct Host unavailable: " + (errorMessage.empty()
            ? L"could not detect a public IPv4 address."
            : errorMessage);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.publicIp = publicIp;
        snapshot_.directHostMessage = L"Direct Host: public IP detected. Asking router to temporarily map the transfer port...";
    }

    if (!AddUpnpPortMapping(port, port, localIp, errorMessage))
    {
        directHostMessage = L"Direct Host unavailable: " + (errorMessage.empty()
            ? L"automatic router port mapping was not accepted. The network may block inbound connections or use CGNAT."
            : errorMessage);
        return false;
    }

    const std::wstring publicUrl = L"http://" + publicIp + L":" + std::to_wstring(port);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.directUrl = publicUrl;
        snapshot_.portMappingActive = true;
        snapshot_.directHostMessage = L"Direct Host: testing public reachability...";
    }

    if (!TestPublicReachability(publicUrl, invite.token, errorMessage))
    {
        RemovePortMapping();
        directHostMessage = L"Direct Host unavailable: the public IP and port did not answer Rex's Toolkit health check. The network may block inbound connections, lack NAT loopback, or use CGNAT.";
        return false;
    }

    invite.directUrl = publicUrl;
    directHostMessage = L"Direct Host ready. The temporary router mapping will be removed when this transfer stops.";
    return true;
}

bool TransferHostServer::QueryPublicIpv4(std::wstring& publicIp, std::wstring& errorMessage) const
{
    const std::vector<std::wstring> endpoints {
        L"https://api4.ipify.org",
        L"https://ipv4.icanhazip.com"
    };

    std::wstring lastError;
    for (const std::wstring& endpoint : endpoints)
    {
        std::string body;
        unsigned long statusCode = 0;
        std::wstring requestError;
        if (!HttpGetText(endpoint, 2500, 2500, 2500, 3500, body, statusCode, requestError) || statusCode != 200)
        {
            lastError = requestError.empty() ? L"public IP lookup did not return a usable response." : requestError;
            continue;
        }

        std::wstring candidate = Trim(Utf8ToWide(body));
        if (IsPublicIpv4Address(candidate))
        {
            publicIp = candidate;
            return true;
        }
        lastError = L"public IP lookup did not return a public IPv4 address.";
    }

    errorMessage = lastError.empty() ? L"could not detect a public IPv4 address." : lastError;
    return false;
}

bool TransferHostServer::AddUpnpPortMapping(unsigned short externalPort, unsigned short internalPort, const std::wstring& localIp, std::wstring& errorMessage)
{
    HRESULT initResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool shouldUninitialize = SUCCEEDED(initResult);
    if (FAILED(initResult) && initResult != RPC_E_CHANGED_MODE)
    {
        errorMessage = L"Windows could not initialize UPnP port mapping.";
        return false;
    }

    IUPnPNAT* nat = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(UPnPNAT), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUPnPNAT), reinterpret_cast<void**>(&nat));
    if (FAILED(hr) || !nat)
    {
        if (shouldUninitialize) CoUninitialize();
        errorMessage = L"UPnP is not available on this PC or router.";
        return false;
    }

    IStaticPortMappingCollection* mappings = nullptr;
    hr = nat->get_StaticPortMappingCollection(&mappings);
    if (FAILED(hr) || !mappings)
    {
        nat->Release();
        if (shouldUninitialize) CoUninitialize();
        errorMessage = L"The router did not expose UPnP port mapping. Direct Host may be blocked or require manual router setup.";
        return false;
    }

    BSTR protocol = SysAllocString(L"TCP");
    BSTR internalClient = SysAllocString(localIp.c_str());
    BSTR description = SysAllocString(L"Rex's Toolkit Smart File Transfer");
    IStaticPortMapping* mapping = nullptr;
    hr = mappings->Add(
        static_cast<long>(externalPort),
        protocol,
        static_cast<long>(internalPort),
        internalClient,
        VARIANT_TRUE,
        description,
        &mapping);

    if (mapping)
    {
        mapping->Release();
    }
    SysFreeString(description);
    SysFreeString(internalClient);
    SysFreeString(protocol);
    mappings->Release();
    nat->Release();
    if (shouldUninitialize) CoUninitialize();

    if (FAILED(hr))
    {
        errorMessage = L"The router rejected the temporary UPnP port mapping. The network may block inbound connections or use CGNAT.";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mappingMutex_);
        portMappingActive_ = true;
        mappedExternalPort_ = externalPort;
        mappedProtocol_ = L"TCP";
    }
    return true;
}

void TransferHostServer::RemovePortMapping()
{
    unsigned short externalPort = 0;
    std::wstring protocolText;
    {
        std::lock_guard<std::mutex> lock(mappingMutex_);
        if (!portMappingActive_ || mappedExternalPort_ == 0)
        {
            return;
        }
        externalPort = mappedExternalPort_;
        protocolText = mappedProtocol_.empty() ? L"TCP" : mappedProtocol_;
        portMappingActive_ = false;
        mappedExternalPort_ = 0;
        mappedProtocol_.clear();
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.portMappingActive = false;
    }

    HRESULT initResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool shouldUninitialize = SUCCEEDED(initResult);
    if (FAILED(initResult) && initResult != RPC_E_CHANGED_MODE)
    {
        return;
    }

    IUPnPNAT* nat = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(UPnPNAT), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUPnPNAT), reinterpret_cast<void**>(&nat));
    if (SUCCEEDED(hr) && nat)
    {
        IStaticPortMappingCollection* mappings = nullptr;
        if (SUCCEEDED(nat->get_StaticPortMappingCollection(&mappings)) && mappings)
        {
            BSTR protocol = SysAllocString(protocolText.c_str());
            mappings->Remove(static_cast<long>(externalPort), protocol);
            SysFreeString(protocol);
            mappings->Release();
        }
        nat->Release();
    }

    if (shouldUninitialize)
    {
        CoUninitialize();
    }
}

bool TransferHostServer::TestPublicReachability(const std::wstring& publicUrl, const std::wstring& token, std::wstring& errorMessage) const
{
    std::string body;
    unsigned long statusCode = 0;
    const std::wstring healthUrl = publicUrl + L"/health?token=" + UrlEncode(token);
    if (!HttpGetText(healthUrl, 2500, 2500, 2500, 3500, body, statusCode, errorMessage))
    {
        return false;
    }
    if (statusCode != 200 || body != "OK")
    {
        errorMessage = L"Direct Host health check failed.";
        return false;
    }
    return true;
}

void TransferHostServer::ServerLoop()
{
    while (!stopRequested_)
    {
        sockaddr_in remote {};
        int remoteLength = sizeof(remote);
        SOCKET client = accept(static_cast<SOCKET>(listenSocket_), reinterpret_cast<sockaddr*>(&remote), &remoteLength);
        if (client == INVALID_SOCKET)
        {
            if (!stopRequested_)
            {
                Sleep(50);
            }
            continue;
        }

        char remoteBuffer[INET_ADDRSTRLEN] {};
        inet_ntop(AF_INET, &remote.sin_addr, remoteBuffer, static_cast<DWORD>(std::size(remoteBuffer)));
        HandleClient(static_cast<uintptr_t>(client), Utf8ToWide(remoteBuffer));
        closesocket(client);
    }
}

void TransferHostServer::HandleClient(uintptr_t clientSocketValue, std::wstring remoteAddress)
{
    SOCKET client = static_cast<SOCKET>(clientSocketValue);
    const std::string request = ReadHttpRequest(client);
    std::istringstream stream(request);
    std::string method;
    std::string target;
    std::string version;
    stream >> method >> target >> version;

    if (method != "GET" || target.empty())
    {
        SendTextResponse(client, 405, "Method Not Allowed", "Only GET requests are supported.");
        return;
    }

    const size_t queryStart = target.find('?');
    const std::string path = queryStart == std::string::npos ? target : target.substr(0, queryStart);
    const std::string query = queryStart == std::string::npos ? std::string() : target.substr(queryStart + 1);
    const std::wstring token = QueryValue(query, "token");
    if (!TokenAccepted(token))
    {
        SendTextResponse(client, 403, "Forbidden", "TOKEN_INVALID");
        return;
    }

    bool expired = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (TransferSecurityService::IsExpired(snapshot_.expiresAt))
        {
            snapshot_.status = SmartTransferHostStatus::Failed;
            snapshot_.message = L"Transfer expired.";
            expired = true;
        }
    }
    if (expired)
    {
        SendTextResponse(client, 410, "Gone", "TRANSFER_EXPIRED");
        stopRequested_ = true;
        CloseListenSocket();
        RemovePortMapping();
        return;
    }

    if (path == "/health")
    {
        SendTextResponse(client, 200, "OK", "OK");
        return;
    }

    if (!ApprovalAccepted(remoteAddress, path == "/manifest" ? L"manifest" : L"download"))
    {
        SmartTransferHostSnapshot snapshot = Snapshot();
        SendTextResponse(client, snapshot.denied ? 403 : 202, snapshot.denied ? "Forbidden" : "Accepted", snapshot.denied ? "DENIED" : "PENDING_APPROVAL");
        return;
    }

    if (path == "/manifest")
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.receiverCount = std::max(1, snapshot_.receiverCount);
            snapshot_.message = L"Receiver connected.";
            snapshot_.status = SmartTransferHostStatus::Hosting;
        }
        const std::string body = BuildManifestJson();
        SendTextResponse(client, 200, "OK", body, "application/json; charset=utf-8");
        return;
    }

    constexpr char downloadPrefix[] = "/download/";
    if (path.rfind(downloadPrefix, 0) != 0)
    {
        SendTextResponse(client, 404, "Not Found", "Route not found.");
        return;
    }

    const std::wstring fileId = UrlDecodeAscii(path.substr(std::size(downloadPrefix) - 1));
    const std::optional<SmartTransferFile> file = FileById(fileId);
    if (!file)
    {
        SendTextResponse(client, 404, "Not Found", "FILE_NOT_FOUND");
        return;
    }

    std::ifstream input(file->path, std::ios::binary);
    if (!input)
    {
        SendTextResponse(client, 404, "Not Found", "SOURCE_FILE_UNAVAILABLE");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.status = SmartTransferHostStatus::Sending;
        snapshot_.currentFile = file->name;
        snapshot_.message = L"Sending " + file->name + L"...";
    }

    std::ostringstream header;
    header << "HTTP/1.1 200 OK\r\n"
        << "Connection: close\r\n"
        << "Content-Type: application/octet-stream\r\n"
        << "Content-Length: " << file->size << "\r\n"
        << "X-Rex-File-Name: " << JsonEscapeUtf8(file->name) << "\r\n\r\n";
    const std::string headerText = header.str();
    if (!SendAll(client, headerText.data(), headerText.size()))
    {
        return;
    }

    std::vector<char> buffer(kSocketBufferSize);
    unsigned long long sentForFile = 0;
    while (input && !stopRequested_)
    {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize read = input.gcount();
        if (read <= 0)
        {
            break;
        }
        if (!SendAll(client, buffer.data(), static_cast<size_t>(read)))
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.status = SmartTransferHostStatus::Failed;
            snapshot_.message = L"Receiver disconnected during download.";
            return;
        }
        sentForFile += static_cast<unsigned long long>(read);
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.bytesSent += static_cast<unsigned long long>(read);
    }

    if (sentForFile == file->size)
    {
        bool shouldStopHosting = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            servedFileIds_.insert(file->id);
            snapshot_.message = L"Sent " + file->name + L".";
            if (options_.stopAfterFirstCompletedDownload && servedFileIds_.size() >= files_.size())
            {
                snapshot_.status = SmartTransferHostStatus::Complete;
                snapshot_.message = L"Transfer complete. Hosting stopped.";
                stopRequested_ = true;
                shouldStopHosting = true;
            }
            else
            {
                snapshot_.status = SmartTransferHostStatus::Hosting;
            }
        }
        if (shouldStopHosting)
        {
            CloseListenSocket();
            RemovePortMapping();
        }
    }
}

std::string TransferHostServer::BuildManifestJson() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream json;
    json << "{\"version\":1,"
        << "\"transferName\":\"" << JsonEscapeUtf8(options_.transferName.empty() ? L"Rex Transfer" : options_.transferName) << "\","
        << "\"expiresAt\":" << snapshot_.expiresAt << ","
        << "\"files\":[";
    for (size_t index = 0; index < files_.size(); ++index)
    {
        if (index > 0)
        {
            json << ',';
        }
        json << "{\"id\":\"" << JsonEscapeUtf8(files_[index].id) << "\","
            << "\"name\":\"" << JsonEscapeUtf8(files_[index].name) << "\","
            << "\"extension\":\"" << JsonEscapeUtf8(files_[index].extension) << "\","
            << "\"size\":" << files_[index].size << "}";
    }
    json << "]}";
    return json.str();
}

std::optional<SmartTransferFile> TransferHostServer::FileById(const std::wstring& id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = std::find_if(files_.begin(), files_.end(), [&](const SmartTransferFile& file)
    {
        return file.id == id;
    });
    if (found == files_.end())
    {
        return std::nullopt;
    }
    return *found;
}

bool TransferHostServer::TokenAccepted(const std::wstring& token) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return !snapshot_.token.empty() && token == snapshot_.token;
}

bool TransferHostServer::ApprovalAccepted(const std::wstring& remoteAddress, const std::wstring& requestLabel)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!options_.requireApproval || snapshot_.approved)
    {
        return true;
    }
    if (snapshot_.denied)
    {
        return false;
    }

    snapshot_.approvalPending = true;
    snapshot_.receiverAddress = remoteAddress;
    snapshot_.status = SmartTransferHostStatus::WaitingForApproval;
    snapshot_.message = L"Receiver " + remoteAddress + L" is requesting the " + requestLabel + L".";
    return false;
}

SmartTransferConnectResult TransferClient::Connect(const std::wstring& code) const
{
    SmartTransferConnectResult result;
    std::wstring errorMessage;
    if (!TransferCodeService::Decode(code, result.invite, errorMessage))
    {
        result.message = errorMessage;
        return result;
    }

    std::wstring lanError;
    std::wstring directError;
    auto tryEndpoint = [&](const std::wstring& baseUrl, const wchar_t* label, std::wstring& endpointError) -> bool
    {
        if (baseUrl.empty())
        {
            return false;
        }

        std::string body;
        unsigned long statusCode = 0;
        const std::wstring manifestUrl = baseUrl + L"/manifest?token=" + UrlEncode(result.invite.token);
        if (!HttpGet(manifestUrl, body, statusCode, endpointError))
        {
            return false;
        }

        result.invite.activeUrl = baseUrl;
        if (statusCode == 202)
        {
            result.waitingForApproval = true;
            result.message = L"Waiting for the sender to approve this receiver.";
            return true;
        }
        if (statusCode == 403)
        {
            result.message = body == "DENIED" ? L"The sender denied this transfer request." : L"The transfer token was rejected.";
            return true;
        }
        if (statusCode == 410)
        {
            result.message = L"That transfer has expired.";
            return true;
        }
        if (statusCode != 200)
        {
            result.message = L"Sender returned an unexpected response.";
            return true;
        }

        if (!ParseManifest(body, result.manifest, endpointError))
        {
            result.message = endpointError;
            return true;
        }

        result.success = true;
        result.message = std::wstring(L"Connected using ") + label + L".";
        return true;
    };

    if (tryEndpoint(result.invite.lanUrl, L"local network", lanError))
    {
        return result;
    }

    if (tryEndpoint(result.invite.directUrl, L"Direct Host", directError))
    {
        return result;
    }

    const std::wstring capabilities = ToLower(result.invite.capabilities);
    const bool codeOffersWebRtc = capabilities.find(L"webrtc_manual") != std::wstring::npos;
    if (codeOffersWebRtc && WebRtcTransport::IsAvailable())
    {
        result.webRtcFallbackOffered = true;
        result.webRtcMessage = L"LAN and Direct Host could not connect. Try manual P2P pairing?";
        result.message = result.webRtcMessage;
        return result;
    }

    if (codeOffersWebRtc && !WebRtcTransport::IsAvailable())
    {
        result.webRtcFallbackOffered = true;
        result.webRtcDependencyMissing = true;
        result.webRtcMessage = WebRtcTransport::DependencyMessage();
        result.message = L"LAN and Direct Host could not connect. Manual P2P pairing is unavailable because the native WebRTC backend is not bundled.";
        return result;
    }

    if (!result.invite.directUrl.empty())
    {
        result.webRtcDependencyMissing = !WebRtcTransport::IsAvailable();
        result.webRtcMessage = WebRtcTransport::DependencyMessage();
        result.message = L"Could not reach the sender over LAN or Direct Host. WebRTC P2P fallback is disabled or unavailable.";
    }
    else
    {
        result.webRtcDependencyMissing = !WebRtcTransport::IsAvailable();
        result.webRtcMessage = WebRtcTransport::DependencyMessage();
        result.message = L"Could not reach the sender on the local network. Direct Host is not available for this transfer, and WebRTC P2P fallback is disabled or unavailable.";
    }
    return result;
}

bool TransferClient::DownloadSelected(
    const SmartTransferInvite& invite,
    const SmartTransferManifest& manifest,
    const std::filesystem::path& saveFolder,
    const std::atomic_bool& cancelRequested,
    const std::function<void(const SmartTransferDownloadProgress&)>& progressCallback,
    std::wstring& errorMessage) const
{
    std::error_code fileError;
    std::filesystem::create_directories(saveFolder, fileError);
    if (fileError)
    {
        errorMessage = L"Could not create the save folder.";
        return false;
    }

    unsigned long long total = 0;
    for (const SmartTransferManifestFile& file : manifest.files)
    {
        if (file.selected)
        {
            total += file.size;
        }
    }
    if (total == 0)
    {
        errorMessage = L"Choose at least one file to download.";
        return false;
    }

    unsigned long long downloadedTotal = 0;
    for (const SmartTransferManifestFile& file : manifest.files)
    {
        if (!file.selected)
        {
            continue;
        }
        if (cancelRequested)
        {
            errorMessage = L"Download cancelled.";
            return false;
        }

        SmartTransferDownloadProgress progress;
        progress.status = SmartTransferClientStatus::Downloading;
        progress.currentFile = file.name;
        progress.totalBytes = total;
        progress.bytesDownloaded = downloadedTotal;
        progress.message = L"Downloading " + file.name + L"...";
        progressCallback(progress);

        const std::filesystem::path requestedPath = saveFolder / TransferSecurityService::SafeFileName(file.name);
        const std::filesystem::path finalPath = TransferSecurityService::AutoRenamePath(requestedPath);
        const std::filesystem::path partialPath = finalPath.wstring() + L".part";
        std::filesystem::remove(partialPath, fileError);

        unsigned long statusCode = 0;
        unsigned long long fileProgress = 0;
        const std::wstring baseUrl = !invite.activeUrl.empty()
            ? invite.activeUrl
            : (!invite.lanUrl.empty() ? invite.lanUrl : invite.directUrl);
        const std::wstring url = baseUrl + L"/download/" + UrlEncode(file.id) + L"?token=" + UrlEncode(invite.token);
        const bool ok = HttpDownloadToFile(
            url,
            partialPath,
            cancelRequested,
            [&](unsigned long long bytesForFile)
            {
                fileProgress = bytesForFile;
                SmartTransferDownloadProgress update;
                update.status = SmartTransferClientStatus::Downloading;
                update.currentFile = file.name;
                update.totalBytes = total;
                update.bytesDownloaded = downloadedTotal + fileProgress;
                update.message = L"Downloading " + file.name + L"...";
                progressCallback(update);
            },
            statusCode,
            errorMessage);

        if (!ok || statusCode != 200)
        {
            std::filesystem::remove(partialPath, fileError);
            if (errorMessage.empty())
            {
                errorMessage = statusCode == 202
                    ? L"Waiting for sender approval."
                    : L"Download failed.";
            }
            return false;
        }

        const auto actualSize = std::filesystem::file_size(partialPath, fileError);
        if (fileError || actualSize != file.size)
        {
            std::filesystem::remove(partialPath, fileError);
            errorMessage = L"Download verification failed for " + file.name + L".";
            return false;
        }

        std::filesystem::rename(partialPath, finalPath, fileError);
        if (fileError)
        {
            std::filesystem::remove(partialPath, fileError);
            errorMessage = L"Could not finalize " + file.name + L".";
            return false;
        }
        downloadedTotal += file.size;
    }

    SmartTransferDownloadProgress complete;
    complete.status = SmartTransferClientStatus::Complete;
    complete.bytesDownloaded = total;
    complete.totalBytes = total;
    complete.message = L"Download complete.";
    progressCallback(complete);
    return true;
}

bool TransferClient::HttpGet(
    const std::wstring& url,
    std::string& body,
    unsigned long& statusCode,
    std::wstring& errorMessage)
{
    body.clear();
    statusCode = 0;
    const auto parsed = ParseHttpUrl(url);
    if (!parsed)
    {
        errorMessage = L"Invalid transfer URL.";
        return false;
    }

    HINTERNET session = WinHttpOpen(L"RexToolkitSmartTransfer/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
    {
        errorMessage = WinHttpErrorMessage();
        return false;
    }
    WinHttpSetTimeouts(session, 3000, 3000, 5000, 5000);
    HINTERNET connection = WinHttpConnect(session, parsed->host.c_str(), parsed->port, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", parsed->path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, parsed->flags) : nullptr;
    const bool sent = request &&
        WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr);
    if (!sent)
    {
        errorMessage = WinHttpErrorMessage();
        if (request) WinHttpCloseHandle(request);
        if (connection) WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

    for (;;)
    {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0)
        {
            break;
        }
        std::string buffer(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), available, &read) || read == 0)
        {
            break;
        }
        body.append(buffer.data(), read);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return true;
}

bool TransferClient::HttpDownloadToFile(
    const std::wstring& url,
    const std::filesystem::path& destination,
    const std::atomic_bool& cancelRequested,
    const std::function<void(unsigned long long)>& progressCallback,
    unsigned long& statusCode,
    std::wstring& errorMessage)
{
    statusCode = 0;
    const auto parsed = ParseHttpUrl(url);
    if (!parsed)
    {
        errorMessage = L"Invalid download URL.";
        return false;
    }

    HINTERNET session = WinHttpOpen(L"RexToolkitSmartTransfer/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
    {
        errorMessage = WinHttpErrorMessage();
        return false;
    }
    WinHttpSetTimeouts(session, 3000, 3000, 30000, 30000);
    HINTERNET connection = WinHttpConnect(session, parsed->host.c_str(), parsed->port, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", parsed->path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, parsed->flags) : nullptr;
    const bool sent = request &&
        WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr);
    if (!sent)
    {
        errorMessage = WinHttpErrorMessage();
        if (request) WinHttpCloseHandle(request);
        if (connection) WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
    if (statusCode != 200)
    {
        std::string body;
        for (;;)
        {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available) || available == 0)
            {
                break;
            }
            std::string buffer(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, buffer.data(), available, &read) || read == 0)
            {
                break;
            }
            body.append(buffer.data(), read);
        }
        errorMessage = Utf8ToWide(body);
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        errorMessage = L"Could not write the download file.";
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    unsigned long long downloaded = 0;
    while (!cancelRequested)
    {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available))
        {
            errorMessage = WinHttpErrorMessage();
            break;
        }
        if (available == 0)
        {
            break;
        }
        std::string buffer(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), available, &read))
        {
            errorMessage = WinHttpErrorMessage();
            break;
        }
        if (read == 0)
        {
            break;
        }
        output.write(buffer.data(), static_cast<std::streamsize>(read));
        downloaded += read;
        progressCallback(downloaded);
    }
    output.close();

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);

    if (cancelRequested)
    {
        errorMessage = L"Download cancelled.";
        return false;
    }
    return errorMessage.empty();
}

bool TransferClient::ParseManifest(const std::string& json, SmartTransferManifest& manifest, std::wstring& errorMessage)
{
    manifest = {};
    manifest.version = static_cast<int>(JsonNumberValue(json, "version"));
    manifest.transferName = JsonStringValue(json, "transferName");
    manifest.expiresAt = static_cast<long long>(JsonNumberValue(json, "expiresAt"));
    for (const std::string& object : JsonObjectsInArray(json, "files"))
    {
        SmartTransferManifestFile file;
        file.id = JsonStringValue(object, "id");
        file.name = JsonStringValue(object, "name");
        file.extension = JsonStringValue(object, "extension");
        file.size = JsonNumberValue(object, "size");
        file.selected = true;
        if (!file.id.empty() && !file.name.empty())
        {
            manifest.files.push_back(file);
        }
    }

    if (manifest.version != 1 || manifest.files.empty())
    {
        errorMessage = L"The sender returned an invalid file manifest.";
        return false;
    }
    if (manifest.transferName.empty())
    {
        manifest.transferName = L"Rex Transfer";
    }
    return true;
}

std::wstring PortMappingService::StatusMessage()
{
    return L"Direct Host can temporarily map the transfer port with UPnP and only appears in a transfer code after Rex's Toolkit confirms the public endpoint responds.";
}

#ifdef REX_TOOLKIT_HAS_LIBDATACHANNEL
namespace
{
constexpr size_t kWebRtcBackpressureBytes = 4 * 1024 * 1024;
constexpr int kWebRtcGatherTimeoutMs = 12000;
constexpr int kWebRtcConnectTimeoutMs = 30000;
std::once_flag gWebRtcLoggerOnce;

void InitWebRtc()
{
    std::call_once(gWebRtcLoggerOnce, []()
    {
        rtc::InitLogger(rtc::LogLevel::Warning);
    });
}

rtc::Configuration CreateWebRtcConfiguration(const SmartTransferSendOptions& options)
{
    rtc::Configuration config;
    config.maxMessageSize = WebRtcChunkSender::PreferredChunkSize;
    for (const std::wstring& server : options.stunServers)
    {
        const std::wstring trimmed = Trim(server);
        if (!trimmed.empty())
        {
            config.iceServers.emplace_back(WideToUtf8(trimmed));
        }
    }
    return config;
}

std::string JoinProtocol(const std::vector<std::string>& parts)
{
    std::string output;
    for (size_t index = 0; index < parts.size(); ++index)
    {
        if (index > 0)
        {
            output += '|';
        }
        output += parts[index];
    }
    return output;
}

std::string ProtocolString(const std::wstring& text)
{
    return Base64Encode(WideToUtf8(text)).empty() ? std::string() : WideToUtf8(Base64Encode(WideToUtf8(text)));
}

std::wstring ProtocolWide(const std::string& text)
{
    std::string decoded;
    if (!Base64Decode(Utf8ToWide(text), decoded))
    {
        return {};
    }
    return Utf8ToWide(decoded);
}

std::wstring WebRtcStateLabel(SmartTransferWebRtcStatus status)
{
    switch (status)
    {
    case SmartTransferWebRtcStatus::Disabled:
        return L"Disabled";
    case SmartTransferWebRtcStatus::DependencyMissing:
        return L"Unavailable";
    case SmartTransferWebRtcStatus::ReadyForFallback:
        return L"Ready";
    case SmartTransferWebRtcStatus::WaitingForReceiverResponse:
        return L"Waiting for receiver response";
    case SmartTransferWebRtcStatus::Pairing:
        return L"Pairing";
    case SmartTransferWebRtcStatus::Connected:
        return L"Connected";
    case SmartTransferWebRtcStatus::Failed:
        return L"Failed";
    case SmartTransferWebRtcStatus::NotNeededYet:
    default:
        return L"Not needed yet";
    }
}
}

struct WebRtcSenderSession::Impl
{
    mutable std::mutex mutex;
    std::mutex workerMutex;
    std::condition_variable cv;
    SmartTransferInvite invite;
    SmartTransferSendOptions options;
    std::vector<SmartTransferFile> files;
    std::function<bool(const std::wstring&, const std::wstring&)> approvalCallback;
    SmartTransferWebRtcSnapshot snapshot;
    std::shared_ptr<rtc::PeerConnection> peer;
    std::shared_ptr<rtc::DataChannel> channel;
    std::vector<std::thread> workers;
    std::string localSdp;
    std::vector<std::pair<std::string, std::string>> localCandidates;
    bool localDescriptionReady = false;
    bool gatheringComplete = false;
    bool stopped = false;
    bool approved = false;
    bool denied = false;

    ~Impl()
    {
        Stop();
    }

    void UpdateStatus(SmartTransferWebRtcStatus status, const std::wstring& message)
    {
        std::lock_guard<std::mutex> lock(mutex);
        snapshot.status = status;
        snapshot.message = message;
        snapshot.connected = status == SmartTransferWebRtcStatus::Connected;
        snapshot.diagnostics = L"WebRTC: " + WebRtcStateLabel(status);
    }

    bool SendString(const std::string& message)
    {
        std::shared_ptr<rtc::DataChannel> active;
        {
            std::lock_guard<std::mutex> lock(mutex);
            active = channel;
        }
        return active && active->isOpen() && active->send(message);
    }

    bool ValidRequest(const std::vector<std::string>& parts, const std::wstring& requestLabel, std::wstring& errorMessage)
    {
        if (parts.size() < 3 ||
            Utf8ToWide(parts[1]) != invite.sessionId ||
            Utf8ToWide(parts[2]) != invite.token)
        {
            errorMessage = L"Invalid token.";
            return false;
        }
        if (TransferSecurityService::IsExpired(invite.expiresAt))
        {
            errorMessage = L"Transfer expired.";
            return false;
        }
        if (options.requireApproval)
        {
            bool isApproved = false;
            bool isDenied = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                isApproved = approved;
                isDenied = denied;
            }
            if (isDenied)
            {
                errorMessage = L"DENIED";
                return false;
            }
            if (!isApproved)
            {
                if (approvalCallback)
                {
                    approvalCallback(L"WebRTC peer", requestLabel);
                }
                errorMessage = L"PENDING_APPROVAL";
                return false;
            }
        }
        return true;
    }

    std::string BuildManifestJson() const
    {
        std::ostringstream json;
        json << "{\"version\":1,"
            << "\"transferName\":\"" << JsonEscapeUtf8(options.transferName.empty() ? L"Rex Transfer" : options.transferName) << "\","
            << "\"expiresAt\":" << invite.expiresAt << ","
            << "\"files\":[";
        for (size_t index = 0; index < files.size(); ++index)
        {
            if (index > 0)
            {
                json << ',';
            }
            json << "{\"id\":\"" << JsonEscapeUtf8(files[index].id) << "\","
                << "\"name\":\"" << JsonEscapeUtf8(files[index].name) << "\","
                << "\"extension\":\"" << JsonEscapeUtf8(files[index].extension) << "\","
                << "\"size\":" << files[index].size << "}";
        }
        json << "]}";
        return json.str();
    }

    std::optional<SmartTransferFile> FileById(const std::wstring& id) const
    {
        const auto found = std::find_if(files.begin(), files.end(), [&](const SmartTransferFile& file)
        {
            return file.id == id;
        });
        if (found == files.end())
        {
            return std::nullopt;
        }
        return *found;
    }

    void SendManifest()
    {
        const std::string body = BuildManifestJson();
        SendString("manifest_response|" + WideToUtf8(Base64Encode(body)));
        UpdateStatus(SmartTransferWebRtcStatus::Connected, L"Receiver connected over manual P2P.");
    }

    void SendFile(const std::wstring& fileId)
    {
        std::optional<SmartTransferFile> file = FileById(fileId);
        if (!file)
        {
            SendString("error|" + WideToUtf8(Base64Encode("FILE_NOT_FOUND")));
            return;
        }

        std::thread worker([this, file]()
        {
            std::ifstream input(file->path, std::ios::binary);
            if (!input)
            {
                SendString("error|" + WideToUtf8(Base64Encode("SOURCE_FILE_UNAVAILABLE")));
                return;
            }

            SendString("file_begin|" + WideToUtf8(file->id) + "|" + std::to_string(file->size));
            {
                std::lock_guard<std::mutex> lock(mutex);
                snapshot.status = SmartTransferWebRtcStatus::Connected;
                snapshot.message = L"Sending " + file->name + L" over manual P2P...";
            }

            std::vector<char> buffer(static_cast<size_t>(WebRtcChunkSender::PreferredChunkSize));
            unsigned long long sentForFile = 0;
            while (input && !stopped)
            {
                input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const std::streamsize read = input.gcount();
                if (read <= 0)
                {
                    break;
                }

                std::shared_ptr<rtc::DataChannel> active;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    active = channel;
                }
                if (!active || !active->isOpen())
                {
                    UpdateStatus(SmartTransferWebRtcStatus::Failed, L"P2P connection closed while sending.");
                    return;
                }
                while (!stopped && active->bufferedAmount() > kWebRtcBackpressureBytes)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                if (!active->send(reinterpret_cast<const rtc::byte*>(buffer.data()), static_cast<size_t>(read)))
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                sentForFile += static_cast<unsigned long long>(read);
            }

            if (sentForFile == file->size)
            {
                SendString("file_complete|" + WideToUtf8(file->id) + "|" + std::to_string(file->size));
                UpdateStatus(SmartTransferWebRtcStatus::Connected, L"Sent " + file->name + L" over manual P2P.");
            }
            else if (!stopped)
            {
                SendString("error|" + WideToUtf8(Base64Encode("SEND_INCOMPLETE")));
                UpdateStatus(SmartTransferWebRtcStatus::Failed, L"P2P file send did not complete.");
            }
        });
        std::lock_guard<std::mutex> lock(workerMutex);
        workers.push_back(std::move(worker));
    }

    void HandleStringMessage(const std::string& message)
    {
        const std::vector<std::string> parts = SplitString(message, '|');
        if (parts.empty())
        {
            return;
        }

        if (parts[0] == "hello")
        {
            SendString("hello|" + WideToUtf8(invite.sessionId));
            return;
        }

        std::wstring errorMessage;
        if (parts[0] == "manifest_request")
        {
            if (!ValidRequest(parts, L"manifest over WebRTC", errorMessage))
            {
                SendString("error|" + WideToUtf8(Base64Encode(WideToUtf8(errorMessage))));
                return;
            }
            SendManifest();
            return;
        }

        if (parts[0] == "download_request")
        {
            if (parts.size() < 4)
            {
                SendString("error|" + WideToUtf8(Base64Encode("INVALID_DOWNLOAD_REQUEST")));
                return;
            }
            if (!ValidRequest(parts, L"download over WebRTC", errorMessage))
            {
                SendString("error|" + WideToUtf8(Base64Encode(WideToUtf8(errorMessage))));
                return;
            }
            SendFile(Utf8ToWide(parts[3]));
            return;
        }

        if (parts[0] == "cancel")
        {
            UpdateStatus(SmartTransferWebRtcStatus::Connected, L"Receiver cancelled the P2P transfer.");
        }
    }

    void ConfigureChannel(const std::shared_ptr<rtc::DataChannel>& dataChannel)
    {
        channel = dataChannel;
        channel->setBufferedAmountLowThreshold(512 * 1024);
        channel->onOpen([this]()
        {
            UpdateStatus(SmartTransferWebRtcStatus::Connected, L"Manual P2P connected. Waiting for receiver request.");
            SendString("hello|" + WideToUtf8(invite.sessionId));
        });
        channel->onClosed([this]()
        {
            if (!stopped)
            {
                UpdateStatus(SmartTransferWebRtcStatus::Failed, L"P2P connection closed.");
            }
        });
        channel->onError([this](std::string error)
        {
            UpdateStatus(SmartTransferWebRtcStatus::Failed, L"P2P connection error: " + Utf8ToWide(error));
        });
        channel->onMessage([this](rtc::message_variant data)
        {
            if (std::holds_alternative<std::string>(data))
            {
                HandleStringMessage(std::get<std::string>(data));
            }
        });
    }

    bool Start(
        const std::vector<SmartTransferFile>& selectedFiles,
        const SmartTransferSendOptions& selectedOptions,
        const SmartTransferInvite& selectedInvite,
        const std::function<bool(const std::wstring&, const std::wstring&)>& selectedApprovalCallback,
        std::wstring& pairingCode,
        std::wstring& errorMessage)
    {
        Stop();
        InitWebRtc();

        {
            std::lock_guard<std::mutex> lock(mutex);
            files = selectedFiles;
            options = selectedOptions;
            invite = selectedInvite;
            approvalCallback = selectedApprovalCallback;
            approved = !options.requireApproval;
            denied = false;
            stopped = false;
            localSdp.clear();
            localCandidates.clear();
            localDescriptionReady = false;
            gatheringComplete = false;
            snapshot = {};
            snapshot.status = SmartTransferWebRtcStatus::Pairing;
            snapshot.message = L"Creating sender pairing code...";
        }

        peer = std::make_shared<rtc::PeerConnection>(CreateWebRtcConfiguration(options));
        peer->onLocalDescription([this](rtc::Description description)
        {
            std::lock_guard<std::mutex> lock(mutex);
            localSdp = std::string(description);
            localDescriptionReady = true;
            cv.notify_all();
        });
        peer->onLocalCandidate([this](rtc::Candidate candidate)
        {
            std::lock_guard<std::mutex> lock(mutex);
            localCandidates.emplace_back(candidate.mid().empty() ? std::string("0") : candidate.mid(), std::string(candidate));
        });
        peer->onGatheringStateChange([this](rtc::PeerConnection::GatheringState state)
        {
            if (state == rtc::PeerConnection::GatheringState::Complete)
            {
                std::lock_guard<std::mutex> lock(mutex);
                gatheringComplete = true;
                cv.notify_all();
            }
        });
        peer->onStateChange([this](rtc::PeerConnection::State state)
        {
            if (state == rtc::PeerConnection::State::Connected)
            {
                UpdateStatus(SmartTransferWebRtcStatus::Connected, L"Manual P2P connected.");
            }
            else if (state == rtc::PeerConnection::State::Failed || state == rtc::PeerConnection::State::Closed)
            {
                if (!stopped)
                {
                    UpdateStatus(SmartTransferWebRtcStatus::Failed, L"P2P connection failed.");
                }
            }
        });

        ConfigureChannel(peer->createDataChannel("rex-transfer"));
        peer->setLocalDescription(rtc::Description::Type::Offer);

        std::unique_lock<std::mutex> lock(mutex);
        const bool ready = cv.wait_for(lock, std::chrono::milliseconds(kWebRtcGatherTimeoutMs), [this]()
        {
            return localDescriptionReady && gatheringComplete;
        });
        if (!ready || localSdp.empty())
        {
            snapshot.status = SmartTransferWebRtcStatus::Failed;
            snapshot.message = L"Could not create sender pairing code before the timeout.";
            errorMessage = snapshot.message;
            return false;
        }

        WebRtcPairingCode code;
        code.role = L"sender_offer";
        code.sessionId = invite.sessionId;
        code.expiresAt = invite.expiresAt;
        code.descriptionType = L"offer";
        code.sdp = localSdp;
        code.candidates = localCandidates;
        pairingCode = EncodeWebRtcPairingCode(code);
        snapshot.senderPairingCode = pairingCode;
        snapshot.pairingCodeReady = true;
        snapshot.status = SmartTransferWebRtcStatus::WaitingForReceiverResponse;
        snapshot.message = L"Sender pairing code ready. Send it to the receiver, then paste their response code.";
        snapshot.diagnostics = L"ICE gathering complete / Data channel waiting";
        return true;
    }

    bool ApplyReceiverResponse(const std::wstring& responseCode, std::wstring& errorMessage)
    {
        WebRtcPairingCode code;
        if (!DecodeWebRtcPairingCode(responseCode, code, errorMessage))
        {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (code.role != L"receiver_answer" || code.sessionId != invite.sessionId)
            {
                errorMessage = L"That receiver response code does not match this transfer.";
                return false;
            }
            snapshot.status = SmartTransferWebRtcStatus::Pairing;
            snapshot.message = L"Applying receiver response code...";
        }

        try
        {
            peer->setRemoteDescription(rtc::Description(code.sdp, "answer"));
            for (const auto& candidate : code.candidates)
            {
                peer->addRemoteCandidate(rtc::Candidate(candidate.second, candidate.first));
            }
        }
        catch (const std::exception& ex)
        {
            errorMessage = L"WebRTC pairing failed: " + Utf8ToWide(ex.what());
            UpdateStatus(SmartTransferWebRtcStatus::Failed, errorMessage);
            return false;
        }

        UpdateStatus(SmartTransferWebRtcStatus::Pairing, L"Receiver response accepted. Waiting for P2P connection...");
        return true;
    }

    void SetApproval(bool isApproved, bool isDenied)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            approved = isApproved;
            denied = isDenied;
        }
        if (isApproved)
        {
            SendManifest();
        }
    }

    void Stop()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopped)
            {
                return;
            }
            stopped = true;
        }
        if (channel)
        {
            channel->resetCallbacks();
            channel->close();
        }
        if (peer)
        {
            peer->resetCallbacks();
            peer->close();
        }
        std::vector<std::thread> localWorkers;
        {
            std::lock_guard<std::mutex> workerLock(workerMutex);
            localWorkers.swap(workers);
        }
        for (std::thread& worker : localWorkers)
        {
            if (worker.joinable())
            {
                worker.join();
            }
        }
        channel.reset();
        peer.reset();
    }

    SmartTransferWebRtcSnapshot Snapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return snapshot;
    }
};

struct WebRtcReceiverSession::Impl
{
    mutable std::mutex mutex;
    std::mutex workerMutex;
    std::condition_variable cv;
    SmartTransferInvite invite;
    SmartTransferSendOptions options;
    SmartTransferWebRtcSnapshot snapshot;
    std::shared_ptr<rtc::PeerConnection> peer;
    std::shared_ptr<rtc::DataChannel> channel;
    std::vector<std::thread> workers;
    std::string localSdp;
    std::vector<std::pair<std::string, std::string>> localCandidates;
    std::ofstream output;
    std::filesystem::path activePartialPath;
    std::wstring activeFileId;
    std::wstring activeFileName;
    std::function<void(const SmartTransferDownloadProgress&)> activeProgressCallback;
    unsigned long long activeExpectedSize = 0;
    unsigned long long activeReceivedSize = 0;
    unsigned long long completedBeforeActive = 0;
    unsigned long long downloadTotal = 0;
    bool localDescriptionReady = false;
    bool gatheringComplete = false;
    bool connected = false;
    bool stopped = false;
    bool manifestRequested = false;
    bool activeFileComplete = false;
    bool activeFileFailed = false;
    std::wstring activeError;

    ~Impl()
    {
        Stop();
    }

    void UpdateStatus(SmartTransferWebRtcStatus status, const std::wstring& message)
    {
        std::lock_guard<std::mutex> lock(mutex);
        snapshot.status = status;
        snapshot.message = message;
        snapshot.connected = status == SmartTransferWebRtcStatus::Connected || connected;
        snapshot.diagnostics = L"WebRTC: " + WebRtcStateLabel(status);
    }

    bool SendString(const std::string& message)
    {
        std::shared_ptr<rtc::DataChannel> active;
        {
            std::lock_guard<std::mutex> lock(mutex);
            active = channel;
        }
        return active && active->isOpen() && active->send(message);
    }

    void RequestManifestSoon(int delayMs)
    {
        std::thread worker([this, delayMs]()
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            bool shouldRequest = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                shouldRequest = !stopped && !snapshot.manifestReady && channel && channel->isOpen();
            }
            if (shouldRequest)
            {
                RequestManifest();
            }
        });
        std::lock_guard<std::mutex> lock(workerMutex);
        workers.push_back(std::move(worker));
    }

    void RequestManifest()
    {
        SendString(JoinProtocol({
            "manifest_request",
            WideToUtf8(invite.sessionId),
            WideToUtf8(invite.token)
        }));
        {
            std::lock_guard<std::mutex> lock(mutex);
            manifestRequested = true;
            snapshot.message = L"Waiting for sender approval or manifest...";
        }
    }

    void HandleStringMessage(const std::string& message)
    {
        const std::vector<std::string> parts = SplitString(message, '|');
        if (parts.empty())
        {
            return;
        }

        if (parts[0] == "hello")
        {
            RequestManifest();
            return;
        }

        if (parts[0] == "manifest_response" && parts.size() >= 2)
        {
            std::string json;
            std::wstring errorMessage;
            if (!Base64Decode(Utf8ToWide(parts[1]), json))
            {
                UpdateStatus(SmartTransferWebRtcStatus::Failed, L"Could not read the P2P manifest.");
                return;
            }

            SmartTransferManifest manifest;
            if (!TransferClient::ParseManifest(json, manifest, errorMessage))
            {
                UpdateStatus(SmartTransferWebRtcStatus::Failed, errorMessage.empty() ? L"P2P manifest was invalid." : errorMessage);
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                snapshot.status = SmartTransferWebRtcStatus::Connected;
                snapshot.manifest = manifest;
                snapshot.manifestReady = true;
                snapshot.connected = true;
                snapshot.message = L"Connected using manual P2P.";
            }
            cv.notify_all();
            return;
        }

        if (parts[0] == "file_begin" && parts.size() >= 3)
        {
            return;
        }

        if (parts[0] == "file_complete" && parts.size() >= 3)
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (Utf8ToWide(parts[1]) == activeFileId)
            {
                activeFileComplete = true;
                if (output.is_open())
                {
                    output.close();
                }
                cv.notify_all();
            }
            return;
        }

        if (parts[0] == "error" && parts.size() >= 2)
        {
            std::string decoded;
            Base64Decode(Utf8ToWide(parts[1]), decoded);
            const std::wstring messageText = decoded.empty() ? L"P2P transfer error." : Utf8ToWide(decoded);
            {
                std::lock_guard<std::mutex> lock(mutex);
                snapshot.message = messageText == L"PENDING_APPROVAL"
                    ? L"Waiting for the sender to approve this P2P transfer."
                    : messageText;
                if (messageText == L"DENIED")
                {
                    snapshot.status = SmartTransferWebRtcStatus::Failed;
                    snapshot.message = L"The sender denied this P2P transfer request.";
                }
                if (!activeFileId.empty())
                {
                    activeFileFailed = true;
                    activeError = snapshot.message;
                }
            }
            cv.notify_all();
            if (messageText == L"PENDING_APPROVAL")
            {
                RequestManifestSoon(1500);
            }
            return;
        }
    }

    void HandleBinaryMessage(const rtc::binary& data)
    {
        std::function<void(const SmartTransferDownloadProgress&)> progressCallback;
        SmartTransferDownloadProgress progress;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!output.is_open() || activeFileId.empty() || data.empty())
            {
                return;
            }
            output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            if (!output)
            {
                activeFileFailed = true;
                activeError = L"Could not write the downloaded file.";
                cv.notify_all();
                return;
            }
            activeReceivedSize += static_cast<unsigned long long>(data.size());
            progressCallback = activeProgressCallback;
            progress.status = SmartTransferClientStatus::Downloading;
            progress.currentFile = activeFileName;
            progress.totalBytes = downloadTotal;
            progress.bytesDownloaded = completedBeforeActive + activeReceivedSize;
            progress.message = L"Downloading " + activeFileName + L" over manual P2P...";
        }
        if (progressCallback)
        {
            progressCallback(progress);
        }
    }

    void ConfigureChannel(const std::shared_ptr<rtc::DataChannel>& dataChannel)
    {
        channel = dataChannel;
        channel->onOpen([this]()
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                connected = true;
                snapshot.status = SmartTransferWebRtcStatus::Connected;
                snapshot.connected = true;
                snapshot.message = L"Manual P2P connected. Requesting file list...";
            }
            RequestManifest();
        });
        channel->onClosed([this]()
        {
            if (!stopped)
            {
                UpdateStatus(SmartTransferWebRtcStatus::Failed, L"P2P connection closed.");
            }
        });
        channel->onError([this](std::string error)
        {
            UpdateStatus(SmartTransferWebRtcStatus::Failed, L"P2P connection error: " + Utf8ToWide(error));
        });
        channel->onMessage([this](rtc::message_variant data)
        {
            if (std::holds_alternative<std::string>(data))
            {
                HandleStringMessage(std::get<std::string>(data));
            }
            else
            {
                HandleBinaryMessage(std::get<rtc::binary>(data));
            }
        });
    }

    bool CreateResponse(
        const SmartTransferInvite& selectedInvite,
        const SmartTransferSendOptions& selectedOptions,
        const std::wstring& senderPairingCode,
        std::wstring& responseCode,
        std::wstring& errorMessage)
    {
        Stop();
        InitWebRtc();

        WebRtcPairingCode offer;
        if (!DecodeWebRtcPairingCode(senderPairingCode, offer, errorMessage))
        {
            return false;
        }
        if (offer.role != L"sender_offer" || offer.sessionId != selectedInvite.sessionId)
        {
            errorMessage = L"That sender pairing code does not match this transfer.";
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            invite = selectedInvite;
            options = selectedOptions;
            stopped = false;
            localSdp.clear();
            localCandidates.clear();
            localDescriptionReady = false;
            gatheringComplete = false;
            connected = false;
            manifestRequested = false;
            activeFileComplete = false;
            activeFileFailed = false;
            activeError.clear();
            activeFileId.clear();
            activeFileName.clear();
            activeExpectedSize = 0;
            activeReceivedSize = 0;
            completedBeforeActive = 0;
            downloadTotal = 0;
            snapshot = {};
            snapshot.status = SmartTransferWebRtcStatus::Pairing;
            snapshot.message = L"Creating receiver response code...";
        }

        peer = std::make_shared<rtc::PeerConnection>(CreateWebRtcConfiguration(options));
        peer->onDataChannel([this](std::shared_ptr<rtc::DataChannel> dataChannel)
        {
            std::lock_guard<std::mutex> lock(mutex);
            ConfigureChannel(dataChannel);
        });
        peer->onLocalDescription([this](rtc::Description description)
        {
            std::lock_guard<std::mutex> lock(mutex);
            localSdp = std::string(description);
            localDescriptionReady = true;
            cv.notify_all();
        });
        peer->onLocalCandidate([this](rtc::Candidate candidate)
        {
            std::lock_guard<std::mutex> lock(mutex);
            localCandidates.emplace_back(candidate.mid().empty() ? std::string("0") : candidate.mid(), std::string(candidate));
        });
        peer->onGatheringStateChange([this](rtc::PeerConnection::GatheringState state)
        {
            if (state == rtc::PeerConnection::GatheringState::Complete)
            {
                std::lock_guard<std::mutex> lock(mutex);
                gatheringComplete = true;
                cv.notify_all();
            }
        });
        peer->onStateChange([this](rtc::PeerConnection::State state)
        {
            if (state == rtc::PeerConnection::State::Connected)
            {
                UpdateStatus(SmartTransferWebRtcStatus::Connected, L"Manual P2P connected. Requesting file list...");
            }
            else if (state == rtc::PeerConnection::State::Failed || state == rtc::PeerConnection::State::Closed)
            {
                if (!stopped)
                {
                    UpdateStatus(SmartTransferWebRtcStatus::Failed, L"P2P connection failed.");
                }
            }
        });

        try
        {
            peer->setRemoteDescription(rtc::Description(offer.sdp, "offer"));
            for (const auto& candidate : offer.candidates)
            {
                peer->addRemoteCandidate(rtc::Candidate(candidate.second, candidate.first));
            }
            peer->setLocalDescription(rtc::Description::Type::Answer);
        }
        catch (const std::exception& ex)
        {
            errorMessage = L"Could not read the sender pairing code: " + Utf8ToWide(ex.what());
            UpdateStatus(SmartTransferWebRtcStatus::Failed, errorMessage);
            return false;
        }

        std::unique_lock<std::mutex> lock(mutex);
        const bool ready = cv.wait_for(lock, std::chrono::milliseconds(kWebRtcGatherTimeoutMs), [this]()
        {
            return localDescriptionReady && gatheringComplete;
        });
        if (!ready || localSdp.empty())
        {
            snapshot.status = SmartTransferWebRtcStatus::Failed;
            snapshot.message = L"Could not create receiver response code before the timeout.";
            errorMessage = snapshot.message;
            return false;
        }

        WebRtcPairingCode answer;
        answer.role = L"receiver_answer";
        answer.sessionId = invite.sessionId;
        answer.expiresAt = invite.expiresAt;
        answer.descriptionType = L"answer";
        answer.sdp = localSdp;
        answer.candidates = localCandidates;
        responseCode = EncodeWebRtcPairingCode(answer);
        snapshot.receiverResponseCode = responseCode;
        snapshot.responseCodeReady = true;
        snapshot.status = SmartTransferWebRtcStatus::Pairing;
        snapshot.message = L"Receiver response code ready. Send it back to the sender, then wait for the file list.";
        snapshot.diagnostics = L"ICE gathering complete / Waiting for sender to apply response";
        return true;
    }

    bool WaitForManifest(SmartTransferManifest& manifest, std::wstring& errorMessage)
    {
        std::unique_lock<std::mutex> lock(mutex);
        const bool ready = cv.wait_for(lock, std::chrono::milliseconds(kWebRtcConnectTimeoutMs), [this]()
        {
            return stopped || snapshot.manifestReady || snapshot.status == SmartTransferWebRtcStatus::Failed;
        });
        if (!ready || !snapshot.manifestReady)
        {
            errorMessage = snapshot.message.empty()
                ? L"P2P connection did not provide a file list before the timeout."
                : snapshot.message;
            return false;
        }
        manifest = snapshot.manifest;
        return true;
    }

    bool DownloadFile(
        const SmartTransferManifestFile& file,
        const std::filesystem::path& partialPath,
        unsigned long long completedBefore,
        unsigned long long total,
        const std::atomic_bool& cancelRequested,
        const std::function<void(const SmartTransferDownloadProgress&)>& progressCallback,
        std::wstring& errorMessage)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            activeFileId = file.id;
            activeFileName = file.name;
            activeExpectedSize = file.size;
            activeReceivedSize = 0;
            completedBeforeActive = completedBefore;
            downloadTotal = total;
            activeFileComplete = false;
            activeFileFailed = false;
            activeError.clear();
            activeProgressCallback = progressCallback;
            activePartialPath = partialPath;
            output.open(partialPath, std::ios::binary | std::ios::trunc);
            if (!output)
            {
                activeFileId.clear();
                errorMessage = L"Could not write the download file.";
                return false;
            }
        }

        SendString(JoinProtocol({
            "download_request",
            WideToUtf8(invite.sessionId),
            WideToUtf8(invite.token),
            WideToUtf8(file.id)
        }));

        std::unique_lock<std::mutex> lock(mutex);
        while (!stopped && !cancelRequested && !activeFileComplete && !activeFileFailed)
        {
            cv.wait_for(lock, std::chrono::milliseconds(100));
        }
        if (output.is_open())
        {
            output.close();
        }
        if (cancelRequested)
        {
            SendString("cancel");
            errorMessage = L"Download cancelled.";
            return false;
        }
        if (activeFileFailed)
        {
            errorMessage = activeError.empty() ? L"P2P download failed." : activeError;
            return false;
        }
        if (activeReceivedSize != activeExpectedSize)
        {
            errorMessage = L"Download verification failed for " + file.name + L".";
            return false;
        }
        activeFileId.clear();
        return true;
    }

    bool DownloadSelected(
        const SmartTransferManifest& manifest,
        const std::filesystem::path& saveFolder,
        const std::atomic_bool& cancelRequested,
        const std::function<void(const SmartTransferDownloadProgress&)>& progressCallback,
        std::wstring& errorMessage)
    {
        SmartTransferManifest readyManifest;
        {
            std::lock_guard<std::mutex> lock(mutex);
            readyManifest = snapshot.manifestReady ? snapshot.manifest : SmartTransferManifest {};
        }
        if (readyManifest.files.empty() && !WaitForManifest(readyManifest, errorMessage))
        {
            return false;
        }

        std::error_code fileError;
        std::filesystem::create_directories(saveFolder, fileError);
        if (fileError)
        {
            errorMessage = L"Could not create the save folder.";
            return false;
        }

        unsigned long long total = 0;
        for (const SmartTransferManifestFile& file : manifest.files)
        {
            if (file.selected)
            {
                total += file.size;
            }
        }
        if (total == 0)
        {
            errorMessage = L"Choose at least one file to download.";
            return false;
        }

        unsigned long long downloadedTotal = 0;
        for (const SmartTransferManifestFile& file : manifest.files)
        {
            if (!file.selected)
            {
                continue;
            }
            SmartTransferDownloadProgress progress;
            progress.status = SmartTransferClientStatus::Downloading;
            progress.currentFile = file.name;
            progress.totalBytes = total;
            progress.bytesDownloaded = downloadedTotal;
            progress.message = L"Downloading " + file.name + L" over manual P2P...";
            progressCallback(progress);

            const std::filesystem::path requestedPath = saveFolder / TransferSecurityService::SafeFileName(file.name);
            const std::filesystem::path finalPath = TransferSecurityService::AutoRenamePath(requestedPath);
            const std::filesystem::path partialPath = finalPath.wstring() + L".part";
            std::filesystem::remove(partialPath, fileError);

            if (!DownloadFile(file, partialPath, downloadedTotal, total, cancelRequested, progressCallback, errorMessage))
            {
                std::filesystem::remove(partialPath, fileError);
                return false;
            }

            const auto actualSize = std::filesystem::file_size(partialPath, fileError);
            if (fileError || actualSize != file.size)
            {
                std::filesystem::remove(partialPath, fileError);
                errorMessage = L"Download verification failed for " + file.name + L".";
                return false;
            }
            std::filesystem::rename(partialPath, finalPath, fileError);
            if (fileError)
            {
                std::filesystem::remove(partialPath, fileError);
                errorMessage = L"Could not finalize " + file.name + L".";
                return false;
            }
            downloadedTotal += file.size;
        }

        SmartTransferDownloadProgress complete;
        complete.status = SmartTransferClientStatus::Complete;
        complete.bytesDownloaded = total;
        complete.totalBytes = total;
        complete.message = L"Download complete.";
        progressCallback(complete);
        return true;
    }

    void Stop()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopped)
            {
                return;
            }
            stopped = true;
            if (output.is_open())
            {
                output.close();
            }
        }
        if (channel)
        {
            channel->resetCallbacks();
            channel->close();
        }
        if (peer)
        {
            peer->resetCallbacks();
            peer->close();
        }
        std::vector<std::thread> localWorkers;
        {
            std::lock_guard<std::mutex> workerLock(workerMutex);
            localWorkers.swap(workers);
        }
        for (std::thread& worker : localWorkers)
        {
            if (worker.joinable())
            {
                worker.join();
            }
        }
        channel.reset();
        peer.reset();
    }

    SmartTransferWebRtcSnapshot Snapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return snapshot;
    }
};
#endif

bool WebRtcTransport::IsAvailable()
{
#ifdef REX_TOOLKIT_HAS_LIBDATACHANNEL
    return true;
#else
    return false;
#endif
}

std::wstring WebRtcTransport::DependencyMessage()
{
#ifdef REX_TOOLKIT_HAS_LIBDATACHANNEL
    return L"Manual P2P fallback is bundled and uses libdatachannel for encrypted WebRTC data-channel transfers.";
#else
    return L"Manual P2P fallback needs a native WebRTC data-channel library such as libdatachannel. It is not bundled in this build, so Rex's Toolkit will not fake WebRTC transfers.";
#endif
}

std::wstring WebRtcTransport::StatusMessage()
{
    if (IsAvailable())
    {
        return L"Manual P2P fallback is available. Rex's Toolkit will use STUN only, no cloud storage, and no TURN relay in this version.";
    }
    return DependencyMessage();
}

std::wstring WebRtcOfferAnswerService::DependencyMessage()
{
    return WebRtcTransport::DependencyMessage();
}

bool WebRtcOfferAnswerService::CanCreatePairingCodes()
{
    return WebRtcTransport::IsAvailable();
}

bool WebRtcSenderSession::IsAvailable()
{
    return WebRtcTransport::IsAvailable();
}

WebRtcSenderSession::WebRtcSenderSession()
{
#ifdef REX_TOOLKIT_HAS_LIBDATACHANNEL
    impl_ = std::make_unique<Impl>();
#endif
}

WebRtcSenderSession::~WebRtcSenderSession() = default;

bool WebRtcSenderSession::Start(
    const std::vector<SmartTransferFile>& files,
    const SmartTransferSendOptions& options,
    const SmartTransferInvite& invite,
    const std::function<bool(const std::wstring&, const std::wstring&)>& approvalCallback,
    std::wstring& pairingCode,
    std::wstring& errorMessage)
{
#ifdef REX_TOOLKIT_HAS_LIBDATACHANNEL
    if (!impl_)
    {
        impl_ = std::make_unique<Impl>();
    }
    return impl_->Start(files, options, invite, approvalCallback, pairingCode, errorMessage);
#else
    errorMessage = WebRtcTransport::DependencyMessage();
    return false;
#endif
}

bool WebRtcSenderSession::ApplyReceiverResponse(const std::wstring& responseCode, std::wstring& errorMessage)
{
#ifdef REX_TOOLKIT_HAS_LIBDATACHANNEL
    return impl_ && impl_->ApplyReceiverResponse(responseCode, errorMessage);
#else
    errorMessage = WebRtcTransport::DependencyMessage();
    return false;
#endif
}

void WebRtcSenderSession::SetApproval(bool approved, bool denied)
{
#ifdef REX_TOOLKIT_HAS_LIBDATACHANNEL
    if (impl_)
    {
        impl_->SetApproval(approved, denied);
    }
#else
    (void)approved;
    (void)denied;
#endif
}

void WebRtcSenderSession::Stop()
{
#ifdef REX_TOOLKIT_HAS_LIBDATACHANNEL
    if (impl_)
    {
        impl_->Stop();
    }
#endif
}

SmartTransferWebRtcSnapshot WebRtcSenderSession::Snapshot() const
{
#ifdef REX_TOOLKIT_HAS_LIBDATACHANNEL
    return impl_ ? impl_->Snapshot() : SmartTransferWebRtcSnapshot {};
#else
    SmartTransferWebRtcSnapshot snapshot;
    snapshot.status = SmartTransferWebRtcStatus::DependencyMissing;
    snapshot.message = WebRtcTransport::DependencyMessage();
    return snapshot;
#endif
}

bool WebRtcReceiverSession::IsAvailable()
{
    return WebRtcTransport::IsAvailable();
}

WebRtcReceiverSession::WebRtcReceiverSession()
{
#ifdef REX_TOOLKIT_HAS_LIBDATACHANNEL
    impl_ = std::make_unique<Impl>();
#endif
}

WebRtcReceiverSession::~WebRtcReceiverSession() = default;

bool WebRtcReceiverSession::CreateResponse(
    const SmartTransferInvite& invite,
    const SmartTransferSendOptions& options,
    const std::wstring& senderPairingCode,
    std::wstring& responseCode,
    std::wstring& errorMessage)
{
#ifdef REX_TOOLKIT_HAS_LIBDATACHANNEL
    if (!impl_)
    {
        impl_ = std::make_unique<Impl>();
    }
    return impl_->CreateResponse(invite, options, senderPairingCode, responseCode, errorMessage);
#else
    errorMessage = WebRtcTransport::DependencyMessage();
    return false;
#endif
}

bool WebRtcReceiverSession::DownloadSelected(
    const SmartTransferManifest& manifest,
    const std::filesystem::path& saveFolder,
    const std::atomic_bool& cancelRequested,
    const std::function<void(const SmartTransferDownloadProgress&)>& progressCallback,
    std::wstring& errorMessage)
{
#ifdef REX_TOOLKIT_HAS_LIBDATACHANNEL
    return impl_ && impl_->DownloadSelected(manifest, saveFolder, cancelRequested, progressCallback, errorMessage);
#else
    errorMessage = WebRtcTransport::DependencyMessage();
    return false;
#endif
}

void WebRtcReceiverSession::Stop()
{
#ifdef REX_TOOLKIT_HAS_LIBDATACHANNEL
    if (impl_)
    {
        impl_->Stop();
    }
#endif
}

SmartTransferWebRtcSnapshot WebRtcReceiverSession::Snapshot() const
{
#ifdef REX_TOOLKIT_HAS_LIBDATACHANNEL
    return impl_ ? impl_->Snapshot() : SmartTransferWebRtcSnapshot {};
#else
    SmartTransferWebRtcSnapshot snapshot;
    snapshot.status = SmartTransferWebRtcStatus::DependencyMissing;
    snapshot.message = WebRtcTransport::DependencyMessage();
    return snapshot;
#endif
}

SmartFileTransferService::~SmartFileTransferService()
{
    StopHosting();
    std::shared_ptr<WebRtcReceiverSession> receiver;
    {
        std::lock_guard<std::mutex> lock(webRtcReceiverMutex_);
        receiver = std::move(webRtcReceiver_);
    }
    if (receiver)
    {
        receiver->Stop();
    }
}

std::optional<SmartTransferFile> SmartFileTransferService::CreateTransferFile(const std::filesystem::path& path, size_t index, std::wstring& errorMessage) const
{
    std::error_code fileError;
    const std::filesystem::path absolute = std::filesystem::absolute(path, fileError);
    if (fileError || !std::filesystem::is_regular_file(absolute, fileError))
    {
        errorMessage = L"Only regular files can be transferred.";
        return std::nullopt;
    }

    SmartTransferFile file;
    file.id = TransferSecurityService::CreateFileId(index);
    file.path = absolute;
    file.name = TransferSecurityService::SafeFileName(absolute.filename().wstring());
    file.extension = absolute.extension().wstring();
    file.size = static_cast<unsigned long long>(std::filesystem::file_size(absolute, fileError));
    if (fileError)
    {
        errorMessage = L"Could not read the file size for " + absolute.filename().wstring() + L".";
        return std::nullopt;
    }
    return file;
}

bool SmartFileTransferService::StartHosting(
    const std::vector<SmartTransferFile>& files,
    const SmartTransferSendOptions& options,
    std::wstring& errorMessage)
{
    return hostServer_.Start(files, options, errorMessage);
}

void SmartFileTransferService::StopHosting()
{
    hostServer_.Stop();
}

void SmartFileTransferService::AllowPendingReceiver()
{
    hostServer_.AllowPendingReceiver();
}

void SmartFileTransferService::DenyPendingReceiver()
{
    hostServer_.DenyPendingReceiver();
}

SmartTransferHostSnapshot SmartFileTransferService::HostSnapshot() const
{
    return hostServer_.Snapshot();
}

SmartTransferConnectResult SmartFileTransferService::Connect(const std::wstring& code) const
{
    return client_.Connect(code);
}

bool SmartFileTransferService::CreateWebRtcSenderPairingCode(std::wstring& pairingCode, std::wstring& errorMessage)
{
    return hostServer_.CreateWebRtcSenderPairingCode(pairingCode, errorMessage);
}

bool SmartFileTransferService::ApplyWebRtcReceiverResponse(const std::wstring& responseCode, std::wstring& errorMessage)
{
    return hostServer_.ApplyWebRtcReceiverResponse(responseCode, errorMessage);
}

bool SmartFileTransferService::CreateWebRtcReceiverResponse(
    const SmartTransferInvite& invite,
    const std::vector<std::wstring>& stunServers,
    const std::wstring& senderPairingCode,
    std::wstring& responseCode,
    std::wstring& errorMessage)
{
    if (!WebRtcTransport::IsAvailable())
    {
        errorMessage = WebRtcTransport::DependencyMessage();
        return false;
    }

    SmartTransferSendOptions options;
    options.enableWebRtcFallback = true;
    if (!stunServers.empty())
    {
        options.stunServers = stunServers;
    }
    auto receiver = std::make_shared<WebRtcReceiverSession>();
    std::shared_ptr<WebRtcReceiverSession> previousReceiver;
    {
        std::lock_guard<std::mutex> lock(webRtcReceiverMutex_);
        previousReceiver = std::move(webRtcReceiver_);
        webRtcReceiver_ = receiver;
    }
    if (previousReceiver)
    {
        previousReceiver->Stop();
    }
    if (!receiver->CreateResponse(invite, options, senderPairingCode, responseCode, errorMessage))
    {
        std::lock_guard<std::mutex> lock(webRtcReceiverMutex_);
        if (webRtcReceiver_ == receiver)
        {
            webRtcReceiver_.reset();
        }
        return false;
    }
    return true;
}

SmartTransferWebRtcSnapshot SmartFileTransferService::WebRtcSnapshot() const
{
    std::shared_ptr<WebRtcReceiverSession> receiver;
    {
        std::lock_guard<std::mutex> lock(webRtcReceiverMutex_);
        receiver = webRtcReceiver_;
    }
    if (receiver)
    {
        return receiver->Snapshot();
    }
    return hostServer_.WebRtcSnapshot();
}

bool SmartFileTransferService::DownloadSelected(
    const SmartTransferInvite& invite,
    const SmartTransferManifest& manifest,
    const std::filesystem::path& saveFolder,
    const std::atomic_bool& cancelRequested,
    const std::function<void(const SmartTransferDownloadProgress&)>& progressCallback,
    std::wstring& errorMessage) const
{
    if (invite.activeUrl == L"webrtc://manual")
    {
        std::shared_ptr<WebRtcReceiverSession> receiver;
        {
            std::lock_guard<std::mutex> lock(webRtcReceiverMutex_);
            receiver = webRtcReceiver_;
        }
        if (!receiver)
        {
            errorMessage = L"Manual P2P is not connected.";
            return false;
        }
        return receiver->DownloadSelected(manifest, saveFolder, cancelRequested, progressCallback, errorMessage);
    }
    return client_.DownloadSelected(invite, manifest, saveFolder, cancelRequested, progressCallback, errorMessage);
}
