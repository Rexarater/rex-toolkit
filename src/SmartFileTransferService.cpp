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
    for (const std::string& lineText : SplitStrinß^vÒÚ$z{-®éÜj×÷WGWBæ6Æ÷6R‚“°¢Ğ¢–b†6æ6VÅ&WVW7FVB¢°¢6VæE7G&–ær‚&6æ6VÂ"“°¢W'&÷$ÖW76vRÒÂ$F÷væÆöB6æ6VÆÆVBâ#°¢&WGW&âfÇ6S°¢Ğ¢–b†7F—fTf–ÆTf–ÆVB¢°¢W'&÷$ÖW76vRÒ7F—fTW'&÷"æV×G’‚’òÂ%%F÷væÆöBf–ÆVBâ"¢7F—fTW'&÷#°¢&WGW&âfÇ6S°¢Ğ¢–b†7F—fU&V6V—fVE6—¦RÒ7F—fTW‡V7FVE6—¦R¢°¢W'&÷$ÖW76vRÒÂ$F÷væÆöBfW&–f–6F–öâf–ÆVBf÷""²f–ÆRææÖR²Â"â#°¢&WGW&âfÇ6S°¢Ğ¢7F—fTf–ÆT–Bæ6ÆV"‚“°¢&WGW&âG'VS°¢Ğ ¢&ööÂF÷væÆöE6VÆV7FVB€¢6öç7B6Ö'EG&ç6fW$Öæ–fW7BbÖæ–fW7BÀ¢6öç7B7FC£¦f–ÆW7—7FVÓ£§F‚b6fTföÆFW"À¢6öç7B7FC£¦FöÖ–5ö&ööÂb6æ6VÅ&WVW7FVBÀ¢6öç7B7FC£¦gVæ7F–öãÇfö–B†6öç7B6Ö'EG&ç6fW$F÷væÆöE&öw&W72b“âb&öw&W746ÆÆ&6²À¢7FC£§w7G&–ærbW'&÷$ÖW76vR¢°¢6Ö'EG&ç6fW$Öæ–fW7B&VG”Öæ–fW7C°¢°¢7FC£¦Æö6µöwV&CÇ7FC£¦×WFWƒâÆö6²†×WFW‚“°¢&VG”Öæ–fW7BÒ6æ6†÷BæÖæ–fW7E&VG’ò6æ6†÷BæÖæ–fW7B¢6Ö'EG&ç6fW$Öæ–fW7B·Ó°¢Ğ¢–b‡&VG”Öæ–fW7Bæf–ÆW2æV×G’‚’bbv—Df÷$Öæ–fW7B‡&VG”Öæ–fW7BÂW'&÷$ÖW76vR’¢°¢&WGW&âfÇ6S°¢Ğ ¢7FC£¦W'&÷%ö6öFRf–ÆTW'&÷#°¢7FC£¦f–ÆW7—7FVÓ£¦7&VFUöF—&V7F÷&–W2‡6fTföÆFW"Âf–ÆTW'&÷"“°¢–b†f–ÆTW'&÷"¢°¢W'&÷$ÖW76vRÒÂ$6÷VÆBæ÷B7&VFRF†R6fRföÆFW"â#°¢&WGW&âfÇ6S°¢Ğ ¢Vç6–væVBÆöærÆöærF÷FÂÒ°¢f÷"†6öç7B6Ö'EG&ç6fW$Öæ–fW7Df–ÆRbf–ÆR¢Öæ–fW7Bæf–ÆW2¢°¢–b†f–ÆRç6VÆV7FVB¢°¢F÷FÂ³Òf–ÆRç6—¦S°¢Ğ¢Ğ¢–b‡F÷FÂÓÒ¢°¢W'&÷$ÖW76vRÒÂ$6†ö÷6RBÆV7BöæRf–ÆRFòF÷væÆöBâ#°¢&WGW&âfÇ6S°¢Ğ ¢Vç6–væVBÆöærÆöærF÷væÆöFVEF÷FÂÒ°¢f÷"†6öç7B6Ö'EG&ç6fW$Öæ–fW7Df–ÆRbf–ÆR¢Öæ–fW7Bæf–ÆW2¢°¢–b‚f–ÆRç6VÆV7FVB¢°¢6öçF–çVS°¢Ğ¢6Ö'EG&ç6fW$F÷væÆöE&öw&W72&öw&W73°¢&öw&W72ç7FGW2Ò6Ö'EG&ç6fW$6Æ–VçE7FGW3£¤F÷væÆöF–æs°¢&öw&W72æ7W'&VçDf–ÆRÒf–ÆRææÖS°¢&öw&W72çF÷FÄ'—FW2ÒF÷FÃ°¢&öw&W72æ'—FW4F÷væÆöFVBÒF÷væÆöFVEF÷FÃ°¢&öw&W72æÖW76vRÒÂ$F÷væÆöF–ær"²f–ÆRææÖR²Â"÷fW"ÖçVÂ%âââ#°¢&öw&W746ÆÆ&6²‡&öw&W72“° ¢6öç7B7FC£¦f–ÆW7—7FVÓ£§F‚&WVW7FVEF‚Ò6fTföÆFW"òG&ç6fW%6V7W&—G•6W'f–6S£¥6fTf–ÆTæÖR†f–ÆRææÖR“°¢6öç7B7FC£¦f–ÆW7—7FVÓ£§F‚f–æÅF‚ÒG&ç6fW%6V7W&—G•6W'f–6S£¤WFõ&VæÖUF‚‡&WVW7FVEF‚“°¢6öç7B7FC£¦f–ÆW7—7FVÓ£§F‚'F–ÅF‚Òf–æÅF‚çw7G&–ær‚’²Â"ç'B#°¢7FC£¦f–ÆW7—7FVÓ£§&VÖ÷fR‡'F–ÅF‚Âf–ÆTW'&÷"“° ¢–b‚F÷væÆöDf–ÆR†f–ÆRÂ'F–ÅF‚ÂF÷væÆöFVEF÷FÂÂF÷FÂÂ6æ6VÅ&WVW7FVBÂ&öw&W746ÆÆ&6²ÂW'&÷$ÖW76vR’¢°¢7FC£¦f–ÆW7—7FVÓ£§&VÖ÷fR‡'F–ÅF‚Âf–ÆTW'&÷"“°¢&WGW&âfÇ6S°¢Ğ ¢6öç7BWFò7GVÅ6—¦RÒ7FC£¦f–ÆW7—7FVÓ£¦f–ÆU÷6—¦R‡'F–ÅF‚Âf–ÆTW'&÷"“°¢–b†f–ÆTW'&÷"ÇÂ7GVÅ6—¦RÒf–ÆRç6—¦R¢°¢7FC£¦f–ÆW7—7FVÓ£§&VÖ÷fR‡'F–ÅF‚Âf–ÆTW'&÷"“°¢W'&÷$ÖW76vRÒÂ$F÷væÆöBfW&–f–6F–öâf–ÆVBf÷""²f–ÆRææÖR²Â"â#°¢&WGW&âfÇ6S°¢Ğ¢7FC£¦f–ÆW7—7FVÓ£§&VæÖR‡'F–ÅF‚Âf–æÅF‚Âf–ÆTW'&÷"“°¢–b†f–ÆTW'&÷"¢°¢7FC£¦f–ÆW7—7FVÓ£§&VÖ÷fR‡'F–ÅF‚Âf–ÆTW'&÷"“°¢W'&÷$ÖW76vRÒÂ$6÷VÆBæ÷Bf–æÆ—¦R"²f–ÆRææÖR²Â"â#°¢&WGW&âfÇ6S°¢Ğ¢F÷væÆöFVEF÷FÂ³Òf–ÆRç6—¦S°¢Ğ ¢6Ö'EG&ç6fW$F÷væÆöE&öw&W726ö×ÆWFS°¢6ö×ÆWFRç7FGW2Ò6Ö'EG&ç6fW$6Æ–VçE7FGW3£¤6ö×ÆWFS°¢6ö×ÆWFRæ'—FW4F÷væÆöFVBÒF÷FÃ°¢6ö×ÆWFRçF÷FÄ'—FW2ÒF÷FÃ°¢6ö×ÆWFRæÖW76vRÒÂ$F÷væÆöB6ö×ÆWFRâ#°¢&öw&W746ÆÆ&6²†6ö×ÆWFR“°¢&WGW&âG'VS°¢Ğ ¢fö–B7F÷‚¢°¢°¢7FC£¦Æö6µöwV&CÇ7FC£¦×WFWƒâÆö6²†×WFW‚“°¢–b‡7F÷VB¢°¢&WGW&ã°¢Ğ¢7F÷VBÒG'VS°¢–b†÷WGWBæ—5ö÷Vâ‚’¢°¢÷WGWBæ6Æ÷6R‚“°¢Ğ¢Ğ¢–b†6†ææVÂ¢°¢6†ææVÂÓç&W6WD6ÆÆ&6·2‚“°¢6†ææVÂÓæ6Æ÷6R‚“°¢Ğ¢–b‡VW"¢°¢VW"Óç&W6WD6ÆÆ&6·2‚“°¢VW"Óæ6Æ÷6R‚“°¢Ğ¢7FC£§fV7F÷#Ç7FC£§F‡&VCâÆö6Åv÷&¶W'3°¢°¢7FC£¦Æö6µöwV&CÇ7FC£¦×WFWƒâv÷&¶W$Æö6²‡v÷&¶W$×WFW‚“°¢Æö6Åv÷&¶W'2ç7v‡v÷&¶W'2“°¢Ğ¢f÷"‡7FC£§F‡&VBbv÷&¶W"¢Æö6Åv÷&¶W'2¢°¢–b‡v÷&¶W"æ¦ö–æ&ÆR‚’¢°¢v÷&¶W"æ¦ö–â‚“°¢Ğ¢Ğ¢6†ææVÂç&W6WB‚“°¢VW"ç&W6WB‚“°¢Ğ ¢6Ö'EG&ç6fW%vV%'F56æ6†÷B6æ6†÷B‚’6öç7@¢°¢7FC£¦Æö6µöwV&CÇ7FC£¦×WFWƒâÆö6²†×WFW‚“°¢&WGW&â6æ6†÷C°¢Ğ§Ó°¢6VæF–` ¦&ööÂvV%'F5G&ç7÷'C£¤—4f–Æ&ÆR‚§°¢6–fFVb$U…õDôôÄ´•Eô„5ôÄ”$DD4„ääTÀ¢&WGW&âG'VS°¢6VÇ6P¢&WGW&âfÇ6S°¢6VæF–`§Ğ §7FC£§w7G&–ærvV%'F5G&ç7÷'C£¤FWVæFVæ7”ÖW76vR‚§°¢6–fFVb$U…õDôôÄ´•Eô„5ôÄ”$DD4„ääTÀ¢&WGW&âÂ$ÖçVÂ%fÆÆ&6²—2'VæFÆVBæBW6W2Æ–&FF6†ææVÂf÷"Væ7'—FVBvV%%D2FFÖ6†ææVÂG&ç6fW'2â#°¢6VÇ6P¢&WGW&âÂ$ÖçVÂ%fÆÆ&6²æVVG2æF—fRvV%%D2FFÖ6†ææVÂÆ–'&'’7V6‚2Æ–&FF6†ææVÂâ—B—2æ÷B'VæFÆVB–âF†—2'V–ÆBÂ6ò&W‚w2FööÆ¶—Bv–ÆÂæ÷Bf¶RvV%%D2G&ç6fW'2â#°¢6VæF–`§Ğ §7FC£§w7G&–ærvV%'F5G&ç7÷'C£¥7FGW4ÖW76vR‚§°¢–b„—4f–Æ&ÆR‚’¢°¢&WGW&âÂ$ÖçVÂ%fÆÆ&6²—2f–Æ&ÆRâ&W‚w2FööÆ¶—Bv–ÆÂW6R5ETâöæÇ’Âæò6Æ÷VB7F÷&vRÂæBæòEU$â&VÆ’–âF†—2fW'6–öââ#°¢Ğ¢&WGW&âFWVæFVæ7”ÖW76vR‚“°§Ğ §7FC£§w7G&–ærvV%'F4öffW$ç7vW%6W'f–6S£¤FWVæFVæ7”ÖW76vR‚§°¢&WGW&âvV%'F5G&ç7÷'C£¤FWVæFVæ7”ÖW76vR‚“°§Ğ ¦&ööÂvV%'F4öffW$ç7vW%6W'f–6S£¤6ä7&VFU—&–æt6öFW2‚§°¢&WGW&âvV%'F5G&ç7÷'C£¤—4f–Æ&ÆR‚“°§Ğ ¦&ööÂvV%'F56VæFW%6W76–öã£¤—4f–Æ&ÆR‚§°¢&WGW&âvV%'F5G&ç7÷'C£¤—4f–Æ&ÆR‚“°§Ğ ¥vV%'F56VæFW%6W76–öã£¥vV%'F56VæFW%6W76–öâ‚§°¢6–fFVb$U…õDôôÄ´•Eô„5ôÄ”$DD4„ääTÀ¢–×ÅòÒ7FC£¦Ö¶U÷Væ—VSÄ–×Ãâ‚“°¢6VæF–`§Ğ ¥vV%'F56VæFW%6W76–öã£§åvV%'F56VæFW%6W76–öâ‚’ÒFVfVÇC° ¦&ööÂvV%'F56VæFW%6W76–öã£¥7F'B€¢6öç7B7FC£§fV7F÷#Å6Ö'EG&ç6fW$f–ÆSâbf–ÆW2À¢6öç7B6Ö'EG&ç6fW%6VæD÷F–öç2b÷F–öç2À¢6öç7B6Ö'EG&ç6fW$–çf—FRb–çf—FRÀ¢6öç7B7FC£¦gVæ7F–öãÆ&ööÂ†6öç7B7FC£§w7G&–ærbÂ6öç7B7FC£§w7G&–ærb“âb&÷fÄ6ÆÆ&6²À¢7FC£§w7G&–ærb—&–æt6öFRÀ¢7FC£§w7G&–ærbW'&÷$ÖW76vR§°¢6–fFVb$U…õDôôÄ´•Eô„5ôÄ”$DD4„ääTÀ¢–b‚–×Åò¢°¢–×ÅòÒ7FC£¦Ö¶U÷Væ—VSÄ–×Ãâ‚“°¢Ğ¢&WGW&â–×ÅòÓå7F'B†f–ÆW2Â÷F–öç2Â–çf—FRÂ&÷fÄ6ÆÆ&6²Â—&–æt6öFRÂW'&÷$ÖW76vR“°¢6VÇ6P¢W'&÷$ÖW76vRÒvV%'F5G&ç7÷'C£¤FWVæFVæ7”ÖW76vR‚“°¢&WGW&âfÇ6S°¢6VæF–`§Ğ ¦&ööÂvV%'F56VæFW%6W76–öã£¤Ç•&V6V—fW%&W7öç6R†6öç7B7FC£§w7G&–ærb&W7öç6T6öFRÂ7FC£§w7G&–ærbW'&÷$ÖW76vR§°¢6–fFVb$U…õDôôÄ´•Eô„5ôÄ”$DD4„ääTÀ¢&WGW&â–×Åòbb–×ÅòÓäÇ•&V6V—fW%&W7öç6R‡&W7öç6T6öFRÂW'&÷$ÖW76vR“°¢6VÇ6P¢W'&÷$ÖW76vRÒvV%'F5G&ç7÷'C£¤FWVæFVæ7”ÖW76vR‚“°¢&WGW&âfÇ6S°¢6VæF–`§Ğ §fö–BvV%'F56VæFW%6W76–öã£¥6WD&÷fÂ†&ööÂ&÷fVBÂ&ööÂFVæ–VB§°¢6–fFVb$U…õDôôÄ´•Eô„5ôÄ”$DD4„ääTÀ¢–b†–×Åò¢°¢–×ÅòÓå6WD&÷fÂ†&÷fVBÂFVæ–VB“°¢Ğ¢6VÇ6P¢‡fö–B–&÷fVC°¢‡fö–B–FVæ–VC°¢6VæF–`§Ğ §fö–BvV%'F56VæFW%6W76–öã£¥7F÷‚§°¢6–fFVb$U…õDôôÄ´•Eô„5ôÄ”$DD4„ääTÀ¢–b†–×Åò¢°¢–×ÅòÓå7F÷‚“°¢Ğ¢6VæF–`§Ğ ¥6Ö'EG&ç6fW%vV%'F56æ6†÷BvV%'F56VæFW%6W76–öã£¥6æ6†÷B‚’6öç7@§°¢6–fFVb$U…õDôôÄ´•Eô„5ôÄ”$DD4„ääTÀ¢&WGW&â–×Åòò–×ÅòÓå6æ6†÷B‚’¢6Ö'EG&ç6fW%vV%'F56æ6†÷B·Ó°¢6VÇ6P¢6Ö'EG&ç6fW%vV%'F56æ6†÷B6æ6†÷C°¢6æ6†÷Bç7FGW2Ò6Ö'EG&ç6fW%vV%'F57FGW3£¤FWVæFVæ7”Ö—76–æs°¢6æ6†÷BæÖW76vRÒvV%'F5G&ç7÷'C£¤FWVæFVæ7”ÖW76vR‚“°¢&WGW&â6æ6†÷C°¢6VæF–`§Ğ ¦&ööÂvV%'F5&V6V—fW%6W76–öã£¤—4f–Æ&ÆR‚§°¢&WGW&âvV%'F5G&ç7÷'C£¤—4f–Æ&ÆR‚“°§Ğ ¥vV%'F5&V6V—fW%6W76–öã£¥vV%'F5&V6V—fW%6W76–öâ‚§°¢6–fFVb$U…õDôôÄ´•Eô„5ôÄ”$DD4„ääTÀ¢–×ÅòÒ7FC£¦Ö¶U÷Væ—VSÄ–×Ãâ‚“°¢6VæF–`§Ğ ¥vV%'F5&V6V—fW%6W76–öã£§åvV%'F5&V6V—fW%6W76–öâ‚’ÒFVfVÇC° ¦&ööÂvV%'F5&V6V—fW%6W76–öã£¤7&VFU&W7öç6R€¢6öç7B6Ö'EG&ç6fW$–çf—FRb–çf—FRÀ¢6öç7B6Ö'EG&ç6fW%6VæD÷F–öç2b÷F–öç2À¢6öç7B7FC£§w7G&–ærb6VæFW%—&–æt6öFRÀ¢7FC£§w7G&–ærb&W7öç6T6öFRÀ¢7FC£§w7G&–ærbW'&÷$ÖW76vR§°¢6–fFVb$U…õDôôÄ´•Eô„5ôÄ”$DD4„ääTÀ¢–b‚–×Åò¢°¢–×ÅòÒ7FC£¦Ö¶U÷Væ—VSÄ–×Ãâ‚“°¢Ğ¢&WGW&â–×ÅòÓä7&VFU&W7öç6R†–çf—FRÂ÷F–öç2Â6VæFW%—&–æt6öFRÂ&W7öç6T6öFRÂW'&÷$ÖW76vR“°¢6VÇ6P¢W'&÷$ÖW76vRÒvV%'F5G&ç7÷'C£¤FWVæFVæ7”ÖW76vR‚“°¢&WGW&âfÇ6S°¢6VæF–`§Ğ ¦&ööÂvV%'F5&V6V—fW%6W76–öã£¤F÷væÆöE6VÆV7FVB€¢6öç7B6Ö'EG&ç6fW$Öæ–fW7BbÖæ–fW7BÀ¢6öç7B7FC£¦f–ÆW7—7FVÓ£§F‚b6fTföÆFW"À¢6öç7B7FC£¦FöÖ–5ö&ööÂb6æ6VÅ&WVW7FVBÀ¢6öç7B7FC£¦gVæ7F–öãÇfö–B†6öç7B6Ö'EG&ç6fW$F÷væÆöE&öw&W72b“âb&öw&W746ÆÆ&6²À¢7FC£§w7G&–ærbW'&÷$ÖW76vR§°¢6–fFVb$U…õDôôÄ´•Eô„5ôÄ”$DD4„ääTÀ¢&WGW&â–×Åòbb–×ÅòÓäF÷væÆöE6VÆV7FVB†Öæ–fW7BÂ6fTföÆFW"Â6æ6VÅ&WVW7FVBÂ&öw&W746ÆÆ&6²ÂW'&÷$ÖW76vR“°¢6VÇ6P¢W'&÷$ÖW76vRÒvV%'F5G&ç7÷'C£¤FWVæFVæ7”ÖW76vR‚“°¢&WGW&âfÇ6S°¢6VæF–`§Ğ §fö–BvV%'F5&V6V—fW%6W76–öã£¥7F÷‚§°¢6–fFVb$U…õDôôÄ´•Eô„5ôÄ”$DD4„ääTÀ¢–b†–×Åò¢°¢–×ÅòÓå7F÷‚“°¢Ğ¢6VæF–`§Ğ ¥6Ö'EG&ç6fW%vV%'F56æ6†÷BvV%'F5&V6V—fW%6W76–öã£¥6æ6†÷B‚’6öç7@§°¢6–fFVb$U…õDôôÄ´•Eô„5ôÄ”$DD4„ääTÀ¢&WGW&â–×Åòò–×ÅòÓå6æ6†÷B‚’¢6Ö'EG&ç6fW%vV%'F56æ6†÷B·Ó°¢6VÇ6P¢6Ö'EG&ç6fW%vV%'F56æ6†÷B6æ6†÷C°¢6æ6†÷Bç7FGW2Ò6Ö'EG&ç6fW%vV%'F57FGW3£¤FWVæFVæ7”Ö—76–æs°¢6æ6†÷BæÖW76vRÒvV%'F5G&ç7÷'C£¤FWVæFVæ7”ÖW76vR‚“°¢&WGW&â6æ6†÷C°¢6VæF–`§Ğ ¥6Ö'Df–ÆUG&ç6fW%6W'f–6S£§å6Ö'Df–ÆUG&ç6fW%6W'f–6R‚§°¢7F÷†÷7F–ær‚“°¢7FC£§6†&VE÷G#ÅvV%'F5&V6V—fW%6W76–öãâ&V6V—fW#°¢°¢7FC£¦Æö6µöwV&CÇ7FC£¦×WFWƒâÆö6²‡vV%'F5&V6V—fW$×WFW…ò“°¢&V6V—fW"Ò7FC£¦Ö÷fR‡vV%'F5&V6V—fW%ò“°¢Ğ¢–b‡&V6V—fW"¢°¢&V6V—fW"Óå7F÷‚“°¢Ğ§Ğ §7FC£¦÷F–öæÃÅ6Ö'EG&ç6fW$f–ÆSâ6Ö'Df–ÆUG&ç6fW%6W'f–6S£¤7&VFUG&ç6fW$f–ÆR†6öç7B7FC£¦f–ÆW7—7FVÓ£§F‚bF‚Â6—¦U÷B–æFW‚Â7FC£§w7G&–ærbW'&÷$ÖW76vR’6öç7@§°¢7FC£¦W'&÷%ö6öFRf–ÆTW'&÷#°¢6öç7B7FC£¦f–ÆW7—7FVÓ£§F‚'6öÇWFRÒ7FC£¦f–ÆW7—7FVÓ£¦'6öÇWFR‡F‚Âf–ÆTW'&÷"“°¢–b†f–ÆTW'&÷"ÇÂ7FC£¦f–ÆW7—7FVÓ£¦—5÷&VwVÆ%öf–ÆR†'6öÇWFRÂf–ÆTW'&÷"’¢°¢W'&÷$ÖW76vRÒÂ$öæÇ’&VwVÆ"f–ÆW26â&RG&ç6fW'&VBâ#°¢&WGW&â7FC£¦çVÆÆ÷C°¢Ğ ¢6Ö'EG&ç6fW$f–ÆRf–ÆS°¢f–ÆRæ–BÒG&ç6fW%6V7W&—G•6W'f–6S£¤7&VFTf–ÆT–B†–æFW‚“°¢f–ÆRçF‚Ò'6öÇWFS°¢f–ÆRææÖRÒG&ç6fW%6V7W&—G•6W'f–6S£¥6fTf–ÆTæÖR†'6öÇWFRæf–ÆVæÖR‚’çw7G&–ær‚’“°¢f–ÆRæW‡FVç6–öâÒ'6öÇWFRæW‡FVç6–öâ‚’çw7G&–ær‚“°¢f–ÆRç6—¦RÒ7FF–5ö67CÇVç6–væVBÆöærÆöæsâ‡7FC£¦f–ÆW7—7FVÓ£¦f–ÆU÷6—¦R†'6öÇWFRÂf–ÆTW'&÷"’“°¢–b†f–ÆTW'&÷"¢°¢W'&÷$ÖW76vRÒÂ$6÷VÆBæ÷B&VBF†Rf–ÆR6—¦Rf÷""²'6öÇWFRæf–ÆVæÖR‚’çw7G&–ær‚’²Â"â#°¢&WGW&â7FC£¦çVÆÆ÷C°¢Ğ¢&WGW&âf–ÆS°§Ğ ¦&ööÂ6Ö'Df–ÆUG&ç6fW%6W'f–6S£¥7F'D†÷7F–ær€¢6öç7B7FC£§fV7F÷#Å6Ö'EG&ç6fW$f–ÆSâbf–ÆW2À¢6öç7B6Ö'EG&ç6fW%6VæD÷F–öç2b÷F–öç2À¢7FC£§w7G&–ærbW'&÷$ÖW76vR§°¢&WGW&â†÷7E6W'fW%òå7F'B†f–ÆW2Â÷F–öç2ÂW'&÷$ÖW76vR“°§Ğ §fö–B6Ö'Df–ÆUG&ç6fW%6W'f–6S£¥7F÷†÷7F–ær‚§°¢†÷7E6W'fW%òå7F÷‚“°§Ğ §fö–B6Ö'Df–ÆUG&ç6fW%6W'f–6S£¤ÆÆ÷uVæF–æu&V6V—fW"‚§°¢†÷7E6W'fW%òäÆÆ÷uVæF–æu&V6V—fW"‚“°§Ğ §fö–B6Ö'Df–ÆUG&ç6fW%6W'f–6S£¤FVç•VæF–æu&V6V—fW"‚§°¢†÷7E6W'fW%òäFVç•VæF–æu&V6V—fW"‚“°§Ğ ¥6Ö'EG&ç6fW$†÷7E6æ6†÷B6Ö'Df–ÆUG&ç6fW%6W'f–6S£¤†÷7E6æ6†÷B‚’6öç7@§°¢&WGW&â†÷7E6W'fW%òå6æ6†÷B‚“°§Ğ ¥6Ö'EG&ç6fW$6öææV7E&W7VÇB6Ö'Df–ÆUG&ç6fW%6W'f–6S£¤6öææV7B†6öç7B7FC£§w7G&–ærb6öFR’6öç7@§°¢&WGW&â6Æ–VçEòä6öææV7B†6öFR“°§Ğ ¦&ööÂ6Ö'Df–ÆUG&ç6fW%6W'f–6S£¤7&VFUvV%'F56VæFW%—&–æt6öFR‡7FC£§w7G&–ærb—&–æt6öFRÂ7FC£§w7G&–ærbW'&÷$ÖW76vR§°¢&WGW&â†÷7E6W'fW%òä7&VFUvV%'F56VæFW%—&–æt6öFR‡—&–æt6öFRÂW'&÷$ÖW76vR“°§Ğ ¦&ööÂ6Ö'Df–ÆUG&ç6fW%6W'f–6S£¤Ç•vV%'F5&V6V—fW%&W7öç6R†6öç7B7FC£§w7G&–ærb&W7öç6T6öFRÂ7FC£§w7G&–ærbW'&÷$ÖW76vR§°¢&WGW&â†÷7E6W'fW%òäÇ•vV%'F5&V6V—fW%&W7öç6R‡&W7öç6T6öFRÂW'&÷$ÖW76vR“°§Ğ ¦&ööÂ6Ö'Df–ÆUG&ç6fW%6W'f–6S£¤7&VFUvV%'F5&V6V—fW%&W7öç6R€¢6öç7B6Ö'EG&ç6fW$–çf—FRb–çf—FRÀ¢6öç7B7FC£§fV7F÷#Ç7FC£§w7G&–æsâb7GVå6W'fW'2À¢6öç7B7FC£§w7G&–ærb6VæFW%—&–æt6öFRÀ¢7FC£§w7G&–ærb&W7öç6T6öFRÀ¢7FC£§w7G&–ærbW'&÷$ÖW76vR§°¢–b‚vV%'F5G&ç7÷'C£¤—4f–Æ&ÆR‚’¢°¢W'&÷$ÖW76vRÒvV%'F5G&ç7÷'C£¤FWVæFVæ7”ÖW76vR‚“°¢&WGW&âfÇ6S°¢Ğ ¢6Ö'EG&ç6fW%6VæD÷F–öç2÷F–öç3°¢÷F–öç2æVæ&ÆUvV%'F4fÆÆ&6²ÒG'VS°¢–b‚7GVå6W'fW'2æV×G’‚’¢°¢÷F–öç2ç7GVå6W'fW'2Ò7GVå6W'fW'3°¢Ğ¢WFò&V6V—fW"Ò7FC£¦Ö¶U÷6†&VCÅvV%'F5&V6V—fW%6W76–öãâ‚“°¢7FC£§6†&VE÷G#ÅvV%'F5&V6V—fW%6W76–öãâ&Wf–÷W5&V6V—fW#°¢°¢7FC£¦Æö6µöwV&CÇ7FC£¦×WFWƒâÆö6²‡vV%'F5&V6V—fW$×WFW…ò“°¢&Wf–÷W5&V6V—fW"Ò7FC£¦Ö÷fR‡vV%'F5&V6V—fW%ò“°¢vV%'F5&V6V—fW%òÒ&V6V—fW#°¢Ğ¢–b‡&Wf–÷W5&V6V—fW"¢°¢&Wf–÷W5&V6V—fW"Óå7F÷‚“°¢Ğ¢–b‚&V6V—fW"Óä7&VFU&W7öç6R†–çf—FRÂ÷F–öç2Â6VæFW%—&–æt6öFRÂ&W7öç6T6öFRÂW'&÷$ÖW76vR’¢°¢7FC£¦Æö6µöwV&CÇ7FC£¦×WFWƒâÆö6²‡vV%'F5&V6V—fW$×WFW…ò“°¢–b‡vV%'F5&V6V—fW%òÓÒ&V6V—fW"¢°¢vV%'F5&V6V—fW%òç&W6WB‚“°¢Ğ¢&WGW&âfÇ6S°¢Ğ¢&WGW&âG'VS°§Ğ ¥6Ö'EG&ç6fW%vV%'F56æ6†÷B6Ö'Df–ÆUG&ç6fW%6W'f–6S£¥vV%'F56æ6†÷B‚’6öç7@§°¢7FC£§6†&VE÷G#ÅvV%'F5&V6V—fW%6W76–öãâ&V6V—fW#°¢°¢7FC£¦Æö6µöwV&CÇ7FC£¦×WFWƒâÆö6²‡vV%'F5&V6V—fW$×WFW…ò“°¢&V6V—fW"ÒvV%'F5&V6V—fW%ó°¢Ğ¢–b‡&V6V—fW"¢°¢&WGW&â&V6V—fW"Óå6æ6†÷B‚“°¢Ğ¢&WGW&â†÷7E6W'fW%òåvV%'F56æ6†÷B‚“°§Ğ ¦&ööÂ6Ö'Df–ÆUG&ç6fW%6W'f–6S£¤F÷væÆöE6VÆV7FVB€¢6öç7B6Ö'EG&ç6fW$–çf—FRb–çf—FRÀ¢6öç7B6Ö'EG&ç6fW$Öæ–fW7BbÖæ–fW7BÀ¢6öç7B7FC£¦f–ÆW7—7FVÓ£§F‚b6fTföÆFW"À¢6öç7B7FC£¦FöÖ–5ö&ööÂb6æ6VÅ&WVW7FVBÀ¢6öç7B7FC£¦gVæ7F–öãÇfö–B†6öç7B6Ö'EG&ç6fW$F÷væÆöE&öw&W72b“âb&öw&W746ÆÆ&6²À¢7FC£§w7G&–ærbW'&÷$ÖW76vR’6öç7@§°¢–b†–çf—FRæ7F—fUW&ÂÓÒÂ'vV''F3¢òöÖçVÂ"¢°¢7FC£§6†&VE÷G#ÅvV%'F5&V6V—fW%6W76–öãâ&V6V—fW#°¢°¢7FC£¦Æö6µöwV&CÇ7FC£¦×WFWƒâÆö6²‡vV%'F5&V6V—fW$×WFW…ò“°¢&V6V—fW"ÒvV%'F5&V6V—fW%ó°¢Ğ¢–b‚&V6V—fW"¢°¢W'&÷$ÖW76vRÒÂ$ÖçVÂ%—2æ÷B6öææV7FVBâ#°¢&WGW&âfÇ6S°¢Ğ¢&WGW&â&V6V—fW"ÓäF÷væÆöE6VÆV7FVB†Öæ–fW7BÂ6fTföÆFW"Â6æ6VÅ&WVW7FVBÂ&öw&W746ÆÆ&6²ÂW'&÷$ÖW76vR“°¢Ğ¢&WGW&â6Æ–VçEòäF÷væÆöE6VÆV7FVB†–çf—FRÂÖæ–fW7BÂ6fTföÆFW"Â6æ6VÅ&WVW7FVBÂ&öw&W746ÆÆ&6²ÂW'&÷$ÖW76vR“°§Ğ 