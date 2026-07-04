#include "MediaDownloadService.h"

#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>

namespace
{
class ScopedHandle
{
public:
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE handle) : handle_(handle) {}
    ~ScopedHandle()
    {
        Reset();
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    HANDLE Get() const
    {
        return handle_;
    }

    HANDLE* Put()
    {
        Reset();
        return &handle_;
    }

    HANDLE Release()
    {
        HANDLE handle = handle_;
        handle_ = nullptr;
        return handle;
    }

    void Reset(HANDLE handle = nullptr)
    {
        if (handle_ && handle_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_ = nullptr;
};

std::wstring ToLower(std::wstring value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](wchar_t ch)
        {
            return static_cast<wchar_t>(std::towlower(ch));
        });
    return value;
}

bool StartsWithHttp(const std::wstring& url)
{
    const std::wstring lower = ToLower(url);
    return lower.rfind(L"https://", 0) == 0 || lower.rfind(L"http://", 0) == 0;
}

std::wstring UrlPathAndQuery(const std::wstring& url)
{
    const size_t scheme = url.find(L"://");
    const size_t hostStart = scheme == std::wstring::npos ? 0 : scheme + 3;
    const size_t pathStart = url.find(L'/', hostStart);
    if (pathStart == std::wstring::npos)
    {
        return L"/";
    }

    const size_t fragmentStart = url.find(L'#', pathStart);
    return ToLower(url.substr(pathStart, fragmentStart == std::wstring::npos ? std::wstring::npos : fragmentStart - pathStart));
}

std::wstring UrlPathOnly(const std::wstring& url)
{
    std::wstring path = UrlPathAndQuery(url);
    const size_t queryStart = path.find(L'?');
    if (queryStart != std::wstring::npos)
    {
        path.resize(queryStart);
    }
    while (path.size() > 1 && path.back() == L'/')
    {
        path.pop_back();
    }
    return path;
}

std::vector<std::wstring> UrlPathSegments(const std::wstring& url)
{
    const std::wstring path = UrlPathOnly(url);
    std::vector<std::wstring> segments;
    size_t position = 0;
    while (position < path.size())
    {
        while (position < path.size() && path[position] == L'/')
        {
            ++position;
        }
        const size_t start = position;
        while (position < path.size() && path[position] != L'/')
        {
            ++position;
        }
        if (position > start)
        {
            segments.push_back(path.substr(start, position - start));
        }
    }
    return segments;
}

bool UrlQueryHasParameter(const std::wstring& url, const std::wstring& parameterName)
{
    const std::wstring pathAndQuery = UrlPathAndQuery(url);
    const size_t queryStart = pathAndQuery.find(L'?');
    if (queryStart == std::wstring::npos)
    {
        return false;
    }

    const std::wstring query = L"&" + pathAndQuery.substr(queryStart + 1);
    return query.find(L"&" + ToLower(parameterName) + L"=") != std::wstring::npos;
}

std::optional<std::filesystem::path> FindOnPath(const std::wstring& executableName)
{
    std::array<wchar_t, 32768> buffer {};
    const DWORD length = SearchPathW(
        nullptr,
        executableName.c_str(),
        nullptr,
        static_cast<DWORD>(buffer.size()),
        buffer.data(),
        nullptr);

    if (length == 0 || length >= buffer.size())
    {
        return std::nullopt;
    }

    return std::filesystem::path(buffer.data());
}

std::filesystem::path ExecutableDirectory()
{
    std::array<wchar_t, MAX_PATH> buffer {};
    DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
    {
        return std::filesystem::current_path();
    }

    return std::filesystem::path(buffer.data()).parent_path();
}

std::optional<std::filesystem::path> FindBesideExecutable(const std::vector<std::filesystem::path>& relativePaths)
{
    const std::filesystem::path executableDirectory = ExecutableDirectory();
    for (const std::filesystem::path& relativePath : relativePaths)
    {
        std::filesystem::path candidate = executableDirectory / relativePath;
        if (std::filesystem::exists(candidate))
        {
            return candidate;
        }
    }

    return std::nullopt;
}

std::wstring QuoteProcessArgument(const std::wstring& argument)
{
    if (argument.empty())
    {
        return L"\"\"";
    }

    const bool needsQuotes = argument.find_first_of(L" \t\n\v\"") != std::wstring::npos;
    if (!needsQuotes)
    {
        return argument;
    }

    std::wstring quoted = L"\"";
    size_t backslashes = 0;
    for (wchar_t ch : argument)
    {
        if (ch == L'\\')
        {
            ++backslashes;
            continue;
        }

        if (ch == L'"')
        {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(ch);
            backslashes = 0;
            continue;
        }

        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(ch);
    }

    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::wstring BuildCommandLine(const std::filesystem::path& executable, const std::vector<std::wstring>& arguments)
{
    std::wstring commandLine = QuoteProcessArgument(executable.wstring());
    for (const std::wstring& argument : arguments)
    {
        commandLine.push_back(L' ');
        commandLine += QuoteProcessArgument(argument);
    }
    return commandLine;
}

std::wstring BytesToWide(const std::string& bytes)
{
    if (bytes.empty())
    {
        return {};
    }

    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (length == 0)
    {
        codePage = CP_ACP;
        flags = 0;
        length = MultiByteToWideChar(codePage, flags, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    }

    if (length == 0)
    {
        return {};
    }

    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(codePage, flags, bytes.data(), static_cast<int>(bytes.size()), result.data(), length);
    return result;
}

std::optional<unsigned int> HexDigitValue(wchar_t ch);
std::optional<unsigned int> ParseJsonUnicodeEscape(const std::wstring& json, size_t position);
void AppendJsonCodePoint(std::wstring& value, unsigned int codePoint);

std::optional<std::wstring> JsonStringValue(const std::wstring& json, const std::wstring& key)
{
    const std::wstring token = L"\"" + key + L"\"";
    size_t position = json.find(token);
    if (position == std::wstring::npos)
    {
        return std::nullopt;
    }

    position = json.find(L':', position + token.size());
    if (position == std::wstring::npos)
    {
        return std::nullopt;
    }

    ++position;
    while (position < json.size() && std::iswspace(json[position]))
    {
        ++position;
    }

    if (position >= json.size() || json[position] != L'"')
    {
        return std::nullopt;
    }

    ++position;
    std::wstring value;
    while (position < json.size())
    {
        const wchar_t ch = json[position++];
        if (ch == L'"')
        {
            return value;
        }
        if (ch == L'\\' && position < json.size())
        {
            const wchar_t escaped = json[position++];
            switch (escaped)
            {
            case L'"':
            case L'\\':
            case L'/':
                value.push_back(escaped);
                break;
            case L'n':
                value.push_back(L'\n');
                break;
            case L'r':
                value.push_back(L'\r');
                break;
            case L't':
                value.push_back(L'\t');
                break;
            case L'u':
            {
                const auto codeUnit = ParseJsonUnicodeEscape(json, position);
                if (!codeUnit)
                {
                    value.push_back(escaped);
                    break;
                }
                position += 4;

                if (*codeUnit >= 0xD800 && *codeUnit <= 0xDBFF &&
                    position + 6 <= json.size() &&
                    json[position] == L'\\' &&
                    json[position + 1] == L'u')
                {
                    const auto lowSurrogate = ParseJsonUnicodeEscape(json, position + 2);
                    if (lowSurrogate && *lowSurrogate >= 0xDC00 && *lowSurrogate <= 0xDFFF)
                    {
                        const unsigned int codePoint =
                            0x10000 +
                            ((*codeUnit - 0xD800) << 10) +
                            (*lowSurrogate - 0xDC00);
                        AppendJsonCodePoint(value, codePoint);
                        position += 6;
                        break;
                    }
                }

                AppendJsonCodePoint(value, *codeUnit);
                break;
            }
            default:
                value.push_back(escaped);
                break;
            }
            continue;
        }
        value.push_back(ch);
    }

    return std::nullopt;
}

std::optional<unsigned int> HexDigitValue(wchar_t ch)
{
    if (ch >= L'0' && ch <= L'9')
    {
        return static_cast<unsigned int>(ch - L'0');
    }
    if (ch >= L'a' && ch <= L'f')
    {
        return static_cast<unsigned int>(ch - L'a' + 10);
    }
    if (ch >= L'A' && ch <= L'F')
    {
        return static_cast<unsigned int>(ch - L'A' + 10);
    }
    return std::nullopt;
}

std::optional<unsigned int> ParseJsonUnicodeEscape(const std::wstring& json, size_t position)
{
    if (position + 4 > json.size())
    {
        return std::nullopt;
    }

    unsigned int value = 0;
    for (size_t index = 0; index < 4; ++index)
    {
        const auto digit = HexDigitValue(json[position + index]);
        if (!digit)
        {
            return std::nullopt;
        }
        value = (value << 4) | *digit;
    }
    return value;
}

void AppendJsonCodePoint(std::wstring& value, unsigned int codePoint)
{
    if (codePoint <= 0xFFFF)
    {
        value.push_back(static_cast<wchar_t>(codePoint));
        return;
    }

    codePoint -= 0x10000;
    value.push_back(static_cast<wchar_t>(0xD800 + ((codePoint >> 10) & 0x3FF)));
    value.push_back(static_cast<wchar_t>(0xDC00 + (codePoint & 0x3FF)));
}

std::optional<double> JsonNumberValue(const std::wstring& json, const std::wstring& key)
{
    const std::wstring token = L"\"" + key + L"\"";
    size_t position = json.find(token);
    if (position == std::wstring::npos)
    {
        return std::nullopt;
    }

    position = json.find(L':', position + token.size());
    if (position == std::wstring::npos)
    {
        return std::nullopt;
    }

    ++position;
    while (position < json.size() && std::iswspace(json[position]))
    {
        ++position;
    }

    size_t end = position;
    while (end < json.size() && (std::iswdigit(json[end]) || json[end] == L'.'))
    {
        ++end;
    }

    if (end == position)
    {
        return std::nullopt;
    }

    try
    {
        return std::stod(json.substr(position, end - position));
    }
    catch (...)
    {
        return std::nullopt;
    }
}

std::wstring TrimCopy(std::wstring value)
{
    while (!value.empty() && std::iswspace(value.front()))
    {
        value.erase(value.begin());
    }
    while (!value.empty() && std::iswspace(value.back()))
    {
        value.pop_back();
    }
    return value;
}

std::wstring CleanExternalToolOutput(std::wstring value)
{
    for (wchar_t& ch : value)
    {
        if (ch == L'\r' || ch == L'\n' || ch == L'\t')
        {
            ch = L' ';
        }
    }

    std::wstring condensed;
    bool previousWasSpace = false;
    for (wchar_t ch : value)
    {
        if (std::iswspace(ch))
        {
            if (!previousWasSpace)
            {
                condensed.push_back(L' ');
            }
            previousWasSpace = true;
        }
        else
        {
            condensed.push_back(ch);
            previousWasSpace = false;
        }
    }

    condensed = TrimCopy(condensed);
    while (condensed.size() >= 4 && ToLower(condensed.substr(condensed.size() - 4)) == L"null")
    {
        condensed.resize(condensed.size() - 4);
        condensed = TrimCopy(condensed);
    }
    return condensed;
}

std::optional<std::wstring> FriendlyExternalToolError(const std::wstring& output, MediaPlatform platform)
{
    const std::wstring lower = ToLower(output);
    if (lower.find(L"drm protected") != std::wstring::npos ||
        lower.find(L"drm-protected") != std::wstring::npos)
    {
        if (platform == MediaPlatform::SoundCloud)
        {
            return L"This SoundCloud track is DRM protected, so Rex's Toolkit cannot download it.";
        }
        if (platform == MediaPlatform::YouTube)
        {
            return L"This YouTube video is DRM protected, so Rex's Toolkit cannot download it.";
        }
        return L"This media is DRM protected, so Rex's Toolkit cannot download it.";
    }

    return std::nullopt;
}

std::wstring FriendlyExternalToolMessage(
    const std::wstring& output,
    MediaPlatform platform,
    const std::wstring& fallbackMessage)
{
    if (auto friendly = FriendlyExternalToolError(output, platform))
    {
        return *friendly;
    }

    const std::wstring cleaned = CleanExternalToolOutput(output);
    return cleaned.empty() ? fallbackMessage : cleaned;
}

std::wstring BpmLabel(double value)
{
    if (value <= 0.0)
    {
        return {};
    }

    const int rounded = static_cast<int>(value + 0.5);
    return rounded > 0 ? std::to_wstring(rounded) : std::wstring {};
}

std::optional<std::wstring> FirstJsonStringOrNumberValue(const std::wstring& json, const std::vector<std::wstring>& keys)
{
    for (const std::wstring& key : keys)
    {
        if (auto value = JsonStringValue(json, key))
        {
            std::wstring trimmed = TrimCopy(*value);
            if (!trimmed.empty())
            {
                return trimmed;
            }
        }
        if (auto value = JsonNumberValue(json, key))
        {
            std::wstring label = BpmLabel(*value);
            if (!label.empty())
            {
                return label;
            }
        }
    }
    return std::nullopt;
}

bool JsonHasVideoFormat(const std::wstring& json)
{
    const std::wstring token = L"\"vcodec\"";
    size_t position = 0;
    while ((position = json.find(token, position)) != std::wstring::npos×­¶ÚÚ$z{-®éÜj×çVÆÇG"À¢g7F'GW–æfòÀ¢g&ö6W74–æf÷&ÖF–öâ“° ¢w&—FU—Rå&W6WB‚“° ¢–b‚7&VFVB¢°¢&W7VÇBæ÷WGWBÒÂ$6÷VÆBæ÷B7F'BW‡FW&æÂFööÂâ#°¢&WGW&â&W7VÇC°¢Ğ ¢66÷VD†æFÆR&ö6W72‡&ö6W74–æf÷&ÖF–öâæ…&ö6W72“°¢66÷VD†æFÆRF‡&VB‡&ö6W74–æf÷&ÖF–öâæ…F‡&VB“° ¢7FC£§7G&–ærVæF–æt'—FW3°¢7FC£¦'&“Æ6†"ÂC“câ'VffW"·Ó°¢&ööÂ&ö6W75'Vææ–ærÒG'VS°¢v†–ÆR‡&ö6W75'Vææ–ær¢°¢–b†6æ6VÅ&WVW7FVBæÆöB‚’¢°¢FW&Ö–æFU&ö6W72‡&ö6W72ävWB‚’Â“°¢&W7VÇBæ6æ6VÆÆVBÒG'VS°¢Ğ ¢Etõ$Bf–Æ&ÆRÒ°¢v†–ÆR…VV´æÖVE—R‡&VE—RävWB‚’ÂçVÆÇG"ÂÂçVÆÇG"Âff–Æ&ÆRÂçVÆÇG"’bbf–Æ&ÆRâ¢°¢Etõ$B'—FW5&VBÒ°¢6öç7BEtõ$B'—FW5Fõ&VBÒ7FC£¦Ö–ãÄEtõ$Câ†f–Æ&ÆRÂ7FF–5ö67CÄEtõ$Câ†'VffW"ç6—¦R‚’’“°¢–b‚&VDf–ÆR‡&VE—RävWB‚’Â'VffW"æFF‚’Â'—FW5Fõ&VBÂf'—FW5&VBÂçVÆÇG"’ÇÂ'—FW5&VBÓÒ¢°¢'&V³°¢Ğ ¢7FC£§7G&–ær6‡Væ²†'VffW"æFF‚’Â'VffW"æFF‚’²'—FW5&VB“°¢VæF–æt'—FW2³Ò6‡Væ³°¢6öç7B7FC£§w7G&–ærv–FT6‡Væ²Ò'—FW5Fõv–FR†6‡Væ²“°¢&W7VÇBæ÷WGWB³Òv–FT6‡Væ³°¢–b†÷WGWD6ÆÆ&6²¢°¢÷WGWD6ÆÆ&6²‡v–FT6‡Væ²“°¢Ğ¢f–Æ&ÆRÒ°¢Ğ ¢6öç7BEtõ$Bv—E&W7VÇBÒv—Df÷%6–ævÆTö&¦V7B‡&ö6W72ävWB‚’ÂS“°¢&ö6W75'Vææ–ærÒv—E&W7VÇBÓÒt•EõD”ÔTõUC°¢Ğ ¢Etõ$B'—FW5&VBÒ°¢v†–ÆR…&VDf–ÆR‡&VE—RävWB‚’Â'VffW"æFF‚’Â7FF–5ö67CÄEtõ$Câ†'VffW"ç6—¦R‚’’Âf'—FW5&VBÂçVÆÇG"’bb'—FW5&VBâ¢°¢7FC£§7G&–ær6‡Væ²†'VffW"æFF‚’Â'VffW"æFF‚’²'—FW5&VB“°¢6öç7B7FC£§w7G&–ærv–FT6‡Væ²Ò'—FW5Fõv–FR†6‡Væ²“°¢&W7VÇBæ÷WGWB³Òv–FT6‡Væ³°¢–b†÷WGWD6ÆÆ&6²¢°¢÷WGWD6ÆÆ&6²‡v–FT6‡Væ²“°¢Ğ¢Ğ ¢vWDW†—D6öFU&ö6W72‡&ö6W72ävWB‚’Âg&W7VÇBæW†—D6öFR“°¢–b‡&W7VÇBæ6æ6VÆÆVB¢°¢&W7VÇBæW†—D6öFRÒ°¢Ğ¢&WGW&â&W7VÇC°§Ğ §7FC£¦÷F–öæÃÄÖVF–F÷væÆöD¦ö#âÖVF–ÖWFFF6W'f–6S£¤æÇ—¦R€¢6öç7B7FC£§w7G&–ærbW&ÂÀ¢6öç7BW‡FW&æÅFööÅ7FGW2bFööÇ2À¢6öç7B7FC£¦FöÖ–5ö&ööÂb6æ6VÅ&WVW7FVBÀ¢7FC£§w7G&–ærbW'&÷$ÖW76vR’6öç7@§°¢–b‚7W÷'FVEÆFf÷&Õ&Vv—7G'“£¤—57W÷'FVEW&Â‡W&Â’¢°¢W'&÷$ÖW76vRÒÂ%Vç7W÷'FVBU$ÂâÆV6RVçFW"–÷UGV&R÷"6÷VæD6Æ÷VBÆ–æ²â#°¢&WGW&â7FC£¦çVÆÆ÷C°¢Ğ ¢–b‚7W÷'FVEÆFf÷&Õ&Vv—7G'“£¤—4Æ–¶VÇ”F—&V7DÖVF–W&Â‡W&Â’¢°¢W'&÷$ÖW76vRÒÂ%F†BÆ–æ²Æöö·2Æ–¶R&öf–ÆRÂ6†ææVÂÂÆ–Æ—7BÂ÷"6öÆÆV7F–öââÆV6R7FRF—&V7B–÷UGV&Rf–FVò÷"6÷VæD6Æ÷VBG&6²Æ–æ²â#°¢&WGW&â7FC£¦çVÆÆ÷C°¢Ğ ¢–b‚FööÇ2ç—DFÇf÷VæB¢°¢W'&÷$ÖW76vRÒÂ'—BÖFÇ×W7B&R–ç7FÆÆVBæBf–Æ&ÆRöâD‚â#°¢&WGW&â7FC£¦çVÆÆ÷C°¢Ğ ¢7FC£§fV7F÷#Ç7FC£§w7G&–æsâ&wVÖVçG2°¢Â"ÒÖGV××6–ævÆRÖ§6öâ"À¢Â"Ò×6¶—ÖF÷væÆöB"À¢Â"ÒÖæò×v&æ–æw2"À¢Â"ÒÖæò×Æ–Æ—7B"À¢W&À¢Ó° ¢6öç7B&ö6W75&W7VÇB&W7VÇBÒ&ö6W75'VææW%òå'Vâ‡FööÇ2ç—DFÇF‚Â&wVÖVçG2Â6æ6VÅ&WVW7FVBÂçVÆÇG"“°¢–b‡&W7VÇBæ6æ6VÆÆVB¢°¢W'&÷$ÖW76vRÒÂ$æÇ—6—26æ6VÆÆVBâ#°¢&WGW&â7FC£¦çVÆÆ÷C°¢Ğ ¢–b‡&W7VÇBæW†—D6öFRÒ¢°¢6öç7BÖVF–ÆFf÷&ÒÆFf÷&ÒÒ7W÷'FVEÆFf÷&Õ&Vv—7G'“£¤FWFV7EÆFf÷&Ò‡W&Â“°¢W'&÷$ÖW76vRÒg&–VæFÇ”W‡FW&æÅFööÄÖW76vR‡&W7VÇBæ÷WGWBÂÆFf÷&ÒÂÂ$6÷VÆBæ÷BæÇ—¦RF†—2U$Ââ"“°¢&WGW&â7FC£¦çVÆÆ÷C°¢Ğ ¢–b„Æöö·4Æ–¶T6öÆÆV7F–öäÖWFFF‡&W7VÇBæ÷WGWB’¢°¢W'&÷$ÖW76vRÒÂ%F†BÆ–æ²Æöö·2Æ–¶R&öf–ÆRÂ6†ææVÂÂÆ–Æ—7BÂ÷"6öÆÆV7F–öââÆV6R7FRF—&V7B–÷UGV&Rf–FVò÷"6÷VæD6Æ÷VBG&6²Æ–æ²â#°¢&WGW&â7FC£¦çVÆÆ÷C°¢Ğ ¢ÖVF–F÷væÆöD¦ö"¦ö#°¢¦ö"çW&ÂÒW&Ã°¢¦ö"çÆFf÷&ÒÒ7W÷'FVEÆFf÷&Õ&Vv—7G'“£¤FWFV7EÆFf÷&Ò‡W&Â“°¢¦ö"çF—FÆRÒ§6öå7G&–æufÇVR‡&W7VÇBæ÷WGWBÂÂ'F—FÆR"’çfÇVUö÷"„Â%VçF—FÆVBÖVF–"“°¢¦ö"çWÆöFW"Ò§6öå7G&–æufÇVR‡&W7VÇBæ÷WGWBÂÂ'WÆöFW""’çfÇVUö÷"„§6öå7G&–æufÇVR‡&W7VÇBæ÷WGWBÂÂ&6†ææVÂ"’çfÇVUö÷"„Â%Væ¶æ÷vâ"’“°¢¦ö"çF‡VÖ&æ–ÅW&ÂÒ§6öå7G&–æufÇVR‡&W7VÇBæ÷WGWBÂÂ'F‡VÖ&æ–Â"’çfÇVUö÷"„Â""“°¢6öç7BWFòGW&F–öâÒ§6öäçVÖ&W%fÇVR‡&W7VÇBæ÷WGWBÂÂ&GW&F–öâ"“°¢6öç7BF÷V&ÆRGW&F–öå6V6öæG2ÒGW&F–öâçfÇVUö÷"ƒã“°¢¦ö"æGW&F–öå6V6öæG2ÒGW&F–öå6V6öæG3°¢–b†GW&F–öâ¢°¢¦ö"æGW&F–öâÒGW&F–öäÆ&VÂ‚¦GW&F–öâ“°¢Ğ¢VÇ6P¢°¢¦ö"æGW&F–öâÒÂ%Væ¶æ÷vâ#°¢Ğ ¢6öç7B7FC£§w7G&–ærf6öFV2ÒFôÆ÷vW"„§6öå7G&–æufÇVR‡&W7VÇBæ÷WGWBÂÂ'f6öFV2"’çfÇVUö÷"„Â""’“°¢6öç7B&ööÂ†5f–FVôf÷&ÖBÒ§6öä†5f–FVôf÷&ÖB‡&W7VÇBæ÷WGWB“°¢¦ö"æÖ…f–FVô†V–v‡BÒ§6öäÖ…f–FVô†V–v‡B‡&W7VÇBæ÷WGWB“°¢–b†¦ö"çÆFf÷&ÒÓÒÖVF–ÆFf÷&Ó£¥6÷VæD6Æ÷VB¢°¢¦ö"æÖVF–G—RÒÖVF–G—S£¤VF–ó°¢Ğ¢VÇ6R–b††5f–FVôf÷&ÖBÇÂ‚f6öFV2æV×G’‚’bbf6öFV2ÒÂ&æöæR"’¢°¢¦ö"æÖVF–G—RÒÖVF–G—S£¥f–FVó°¢Ğ¢VÇ6R–b‡f6öFV2ÓÒÂ&æöæR"¢°¢¦ö"æÖVF–G—RÒÖVF–G—S£¤VF–ó°¢Ğ¢VÇ6P¢°¢¦ö"æÖVF–G—RÒ¦ö"çÆFf÷&ÒÓÒÖVF–ÆFf÷&Ó£¥–÷UGV&RòÖVF–G—S£¥f–FVò¢ÖVF–G—S£¥Væ¶æ÷vã°¢Ğ ¢–b‚GW&F–öâbb¦ö"æÖVF–G—RÓÒÖVF–G—S£¥Væ¶æ÷vâ¢°¢W'&÷$ÖW76vRÒÂ%F†BÆ–æ²FöW2æ÷BV"Fò&RF—&V7BF÷væÆöF&ÆRÖVF–—FVÒâÆV6R7FR–÷UGV&Rf–FVò÷"6÷VæD6Æ÷VBG&6²Æ–æ²â#°¢&WGW&â7FC£¦çVÆÆ÷C°¢Ğ ¢¦ö"ç7FGW2ÒÖVF–F÷væÆöE7FGW3£¥&VG“°¢&WGW&â¦ö#°§Ğ ¤W‡FW&æÅFööÅ7FGW2ÖVF–F÷væÆöE6W'f–6S£¤6†V6´W‡FW&æÅFööÇ2‚’6öç7@§°¢&WGW&âW‡FW&æÅFööÅ6W'f–6Uòä6†V6µFööÇ2‚“°§Ğ §7FC£¦÷F–öæÃÄÖVF–F÷væÆöD¦ö#âÖVF–F÷væÆöE6W'f–6S£¤æÇ—¦R€¢6öç7B7FC£§w7G&–ærbW&ÂÀ¢6öç7B7FC£¦FöÖ–5ö&ööÂb6æ6VÅ&WVW7FVBÀ¢7FC£§w7G&–ærbW'&÷$ÖW76vR’6öç7@§°¢6öç7BW‡FW&æÅFööÅ7FGW2FööÇ2Ò6†V6´W‡FW&æÅFööÇ2‚“°¢&WGW&âÖWFFF6W'f–6UòäæÇ—¦R‡W&ÂÂFööÇ2Â6æ6VÅ&WVW7FVBÂW'&÷$ÖW76vR“°§Ğ ¤ÖVF–F÷væÆöD¦ö"ÖVF–F÷væÆöE6W'f–6S£¤æÇ—¦T×W6–2€¢ÖVF–F÷væÆöD¦ö"¦ö"À¢6öç7B7FC£¦FöÖ–5ö&ööÂb6æ6VÅ&WVW7FVBÀ¢7FC£§w7G&–ærbW'&÷$ÖW76vR’6öç7@§°¢–b‚7W÷'FVEÆFf÷&Õ&Vv—7G'“£¤—57W÷'FVEW&Â†¦ö"çW&Â’ÇÀ¢7W÷'FVEÆFf÷&Õ&Vv—7G'“£¤—4Æ–¶VÇ”F—&V7DÖVF–W&Â†¦ö"çW&Â’¢°¢¦ö"ç7FGW2ÒÖVF–F÷væÆöE7FGW3£¥&VG“°¢W'&÷$ÖW76vRÒÂ$æÇ—¦RF—&V7B–÷UGV&Rf–FVò÷"6÷VæD6Æ÷VBG&6²f—'7Bâ#°¢¦ö"æW'&÷$ÖW76vRÒW'&÷$ÖW76vS°¢&WGW&â¦ö#°¢Ğ ¢6öç7BW‡FW&æÅFööÅ7FGW2FööÇ2Ò6†V6´W‡FW&æÅFööÇ2‚“°¢6öç7B7FC£§w7G&–ærÖ—76–æuFööÇ2Ò¦ö–äÖ—76–æuFööÇ4ÖW76vR‡FööÇ2“°¢–b‚Ö—76–æuFööÇ2æV×G’‚’¢°¢¦ö"ç7FGW2ÒÖVF–F÷væÆöE7FGW3£¥&VG“°¢W'&÷$ÖW76vRÒÖ—76–æuFööÇ3°¢¦ö"æW'&÷$ÖW76vRÒW'&÷$ÖW76vS°¢&WGW&â¦ö#°¢Ğ ¢–b†WFòæÇ—6—2ÒæÇ—¦T×W6–4g&öÔVF–õ6×ÆR†¦ö"çW&ÂÂFööÇ2Â&ö6W75'VææW%òÂ6æ6VÅ&WVW7FVBÂ¦ö"æGW&F–öå6V6öæG2’¢°¢–b‚æÇ—6—2Óæ×W6–6Ä¶W’æV×G’‚’¢°¢¦ö"æ×W6–6Ä¶W’ÒæÇ—6—2Óæ×W6–6Ä¶W“°¢Ğ¢–b‚æÇ—6—2Óæ'ÒæV×G’‚’¢°¢¦ö"æ'ÒÒæÇ—6—2Óæ'Ó°¢Ğ¢¦ö"æ×W6–4ÖWFFF6÷W&6RÒæÇ—6—2Óç6÷W&6RæV×G’‚’òÂ$Æö6ÂW7F–ÖFR"¢æÇ—6—2Óç6÷W&6S°¢¦ö"æW'&÷$ÖW76vRæ6ÆV"‚“°¢Ğ¢VÇ6P¢°¢W'&÷$ÖW76vRÒ6æ6VÅ&WVW7FVBæÆöB‚¢òÂ$×W6–2æÇ—6—26æ6VÆÆVBâ ¢¢Â$6÷VÆBæ÷BW7F–ÖFR%Òö¶W’g&öÒF†—2VF–òâ#°¢¦ö"æW'&÷$ÖW76vRÒW'&÷$ÖW76vS°¢Ğ ¢¦ö"ç7FGW2ÒÖVF–F÷væÆöE7FGW3£¥&VG“°¢&WGW&â¦ö#°§Ğ ¤ÖVF–F÷væÆöD¦ö"ÖVF–F÷væÆöE6W'f–6S£¤F÷væÆöB€¢ÖVF–F÷væÆöD¦ö"¦ö"À¢6öç7BÖVF–F÷væÆöD÷F–öç2b÷F–öç2À¢6öç7B7FC£¦FöÖ–5ö&ööÂb6æ6VÅ&WVW7FVBÀ¢6öç7B&öw&W746ÆÆ&6²b&öw&W746ÆÆ&6²’6öç7@§°¢6öç7BW‡FW&æÅFööÅ7FGW2FööÇ2Ò6†V6´W‡FW&æÅFööÇ2‚“°¢6öç7B7FC£§w7G&–ærÖ—76–æuFööÇ2Ò¦ö–äÖ—76–æuFööÇ4ÖW76vR‡FööÇ2“°¢–b‚Ö—76–æuFööÇ2æV×G’‚’¢°¢¦ö"ç7FGW2ÒÖVF–F÷væÆöE7FGW3£¤f–ÆVC°¢¦ö"æW'&÷$ÖW76vRÒÖ—76–æuFööÇ3°¢&WGW&â¦ö#°¢Ğ ¢–b‚7W÷'FVEÆFf÷&Õ&Vv—7G'“£¤—57W÷'FVEW&Â†¦ö"çW&Â’¢°¢¦ö"ç7FGW2ÒÖVF–F÷væÆöE7FGW3£¤f–ÆVC°¢¦ö"æW'&÷$ÖW76vRÒÂ%Vç7W÷'FVBU$ÂâÆV6RVçFW"–÷UGV&R÷"6÷VæD6Æ÷VBÆ–æ²â#°¢&WGW&â¦ö#°¢Ğ ¢–b‚7W÷'FVEÆFf÷&Õ&Vv—7G'“£¤—4Æ–¶VÇ”F—&V7DÖVF–W&Â†¦ö"çW&Â’¢°¢¦ö"ç7FGW2ÒÖVF–F÷væÆöE7FGW3£¤f–ÆVC°¢¦ö"æW'&÷$ÖW76vRÒÂ%F†BÆ–æ²Æöö·2Æ–¶R&öf–ÆRÂ6†ææVÂÂÆ–Æ—7BÂ÷"6öÆÆV7F–öââÆV6R7FRF—&V7B–÷UGV&Rf–FVò÷"6÷VæD6Æ÷VBG&6²Æ–æ²â#°¢&WGW&â¦ö#°¢Ğ ¢–b†÷F–öç2æ÷WGWDföÆFW"æV×G’‚’ÇÂ7FC£¦f–ÆW7—7FVÓ£¦W†—7G2†÷F–öç2æ÷WGWDföÆFW"’¢°¢¦ö"ç7FGW2ÒÖVF–F÷væÆöE7FGW3£¤f–ÆVC°¢¦ö"æW'&÷$ÖW76vRÒÂ$÷WGWBföÆFW"FöW2æ÷BW†—7Bâ#°¢&WGW&â¦ö#°¢Ğ ¢–b†÷F–öç2æ÷WGWDf÷&ÖBÓÒÖVF–÷WGWDf÷&ÖC£¤×Bb`¢¦ö"çÆFf÷&ÒÓÒÖVF–ÆFf÷&Ó£¥6÷VæD6Æ÷VBb`¢¦ö"æÖVF–G—RÓÒÖVF–G—S£¤VF–ò¢°¢¦ö"ç7FGW2ÒÖVF–F÷væÆöE7FGW3£¤f–ÆVC°¢¦ö"æW'&÷$ÖW76vRÒÂ%F†—26÷W&6RV'2Fò&RVF–òÖöæÇ’â6†ö÷6RÕ2÷"tb–ç7FVBâ#°¢&WGW&â¦ö#°¢Ğ ¢¦ö"æ÷WGWDf÷&ÖBÒ÷F–öç2æ÷WGWDf÷&ÖC°¢¦ö"æ×EVÆ—G’Ò÷F–öç2æ×EVÆ—G“°¢¦ö"æ×4&—G&FRÒ÷F–öç2æ×4&—G&FS°¢¦ö"æ÷WGWDföÆFW"Ò÷F–öç2æ÷WGWDföÆFW#°¢¦ö"ç&öw&W72Òã°¢¦ö"ç7VVBæ6ÆV"‚“°¢¦ö"æWFæ6ÆV"‚“°¢¦ö"æW'&÷$ÖW76vRæ6ÆV"‚“° ¢7FC£§w7G&–ær&6TæÖRÒ÷F–öç2æ7W7FöÔf–ÆTæÖRæV×G’‚’ò¦ö"çF—FÆR¢÷F–öç2æ7W7FöÔf–ÆTæÖS°¢&6TæÖRÒ6æ—F—¦Tf–ÆTæÖR†&6TæÖR“°¢6öç7B7FC£¦f–ÆW7—7FVÓ£§F‚&WVW7FVEF‚Ò÷F–öç2æ÷WGWDföÆFW"ò†&6TæÖR²W‡FVç6–öäf÷"†÷F–öç2æ÷WGWDf÷&ÖB’“°¢¦ö"æ÷WGWDf–ÆUF‚Ò&W6öÇfT6öæfÆ–7B‡&WVW7FVEF‚“° ¢7FC£¦f–ÆW7—7FVÓ£§F‚÷WGWEFV×ÆFRÒ¦ö"æ÷WGWDf–ÆUF‚ç&VçE÷F‚‚’ò†¦ö"æ÷WGWDf–ÆUF‚ç7FVÒ‚’çw7G&–ær‚’²Â"âR†W‡B—2"“° ¢7FC£§fV7F÷#Ç7FC£§w7G&–æsâ&wVÖVçG2°¢Â"ÒÖæò×Æ–Æ—7B"À¢Â"ÒÖæWvÆ–æR"À¢Â"ÒÖff×VrÖÆö6F–öâ"À¢FööÇ2æff×VuF‚ç&VçE÷F‚‚’çw7G&–ær‚’À¢Â"Öò"À¢÷WGWEFV×ÆFRçw7G&–ær‚¢Ó° ¢–b†÷F–öç2æ÷WGWDf÷&ÖBÓÒÖVF–÷WGWDf÷&ÖC£¤×B¢°¢&wVÖVçG2çW6…ö&6²„Â"Öb"“°¢&wVÖVçG2çW6…ö&6²„×Df÷&ÖE6VÆV7F÷"†÷F–öç2æ×EVÆ—G’’“°¢&wVÖVçG2çW6…ö&6²„Â"ÒÖÖW&vRÖ÷WGWBÖf÷&ÖB"“°¢&wVÖVçG2çW6…ö&6²„Â&×B"“°¢Ğ¢VÇ6R–b†÷F–öç2æ÷WGWDf÷&ÖBÓÒÖVF–÷WGWDf÷&ÖC£¤×2¢°¢&wVÖVçG2çW6…ö&6²„Â"×‚"“°¢&wVÖVçG2çW6…ö&6²„Â"ÒÖVF–òÖf÷&ÖB"“°¢&wVÖVçG2çW6…ö&6²„Â&×2"“°¢&wVÖVçG2çW6…ö&6²„Â"ÒÖVF–ò×VÆ—G’"“°¢&wVÖVçG2çW6…ö&6²„&—G&FUfÇVR†÷F–öç2æ×4&—G&FR’“°¢Ğ¢VÇ6P¢°¢&wVÖVçG2çW6…ö&6²„Â"×‚"“°¢&wVÖVçG2çW6…ö&6²„Â"ÒÖVF–òÖf÷&ÖB"“°¢&wVÖVçG2çW6…ö&6²„Â'vb"“°¢Ğ ¢&wVÖVçG2çW6…ö&6²†¦ö"çW&Â“° ¢¦ö"ç7FGW2ÒÖVF–F÷væÆöE7FGW3£¤F÷væÆöF–æs°¢–b‡&öw&W746ÆÆ&6²¢°¢&öw&W746ÆÆ&6²†¦ö"“°¢Ğ ¢&ö6W75&W7VÇB&W7VÇBÒ&ö6W75'VææW%òå'Vâ€¢FööÇ2ç—DFÇF‚À¢&wVÖVçG2À¢6æ6VÅ&WVW7FVBÀ¢²eÒ†6öç7B7FC£§w7G&–ærb÷WGWB¢°¢'6U&öw&W75FW‡B†÷WGWBÂ¦ö"“°¢–b‡&öw&W746ÆÆ&6²¢°¢&öw&W746ÆÆ&6²†¦ö"“°¢Ğ¢Ò“° ¢–b‡&W7VÇBæ6æ6VÆÆVB¢°¢¦ö"ç7FGW2ÒÖVF–F÷væÆöE7FGW3£¤6æ6VÆÆVC°¢¦ö"æW'&÷$ÖW76vRÒÂ$F÷væÆöB6æ6VÆÆVBâ#°¢&WGW&â¦ö#°¢Ğ ¢–b‡&W7VÇBæW†—D6öFRÒ¢°¢¦ö"ç7FGW2ÒÖVF–F÷væÆöE7FGW3£¤f–ÆVC°¢¦ö"æW'&÷$ÖW76vRÒg&–VæFÇ”W‡FW&æÅFööÄÖW76vR‡&W7VÇBæ÷WGWBÂ¦ö"çÆFf÷&ÒÂÂ$F÷væÆöBf–ÆVBâ"“°¢&WGW&â¦ö#°¢Ğ ¢–b‚7FC£¦f–ÆW7—7FVÓ£¦W†—7G2†¦ö"æ÷WGWDf–ÆUF‚’¢°¢6öç7B7FC£¦f–ÆW7—7FVÓ£§F‚fÆÆ&6µF‚Ò¦ö"æ÷WGWDf–ÆUF‚ç&VçE÷F‚‚’ğ¢†¦ö"æ÷WGWDf–ÆUF‚ç7FVÒ‚’çw7G&–ær‚’²W‡FVç6–öäf÷"†÷F–öç2æ÷WGWDf÷&ÖB’“°¢–b‡7FC£¦f–ÆW7—7FVÓ£¦W†—7G2†fÆÆ&6µF‚’¢°¢¦ö"æ÷WGWDf–ÆUF‚ÒfÆÆ&6µFƒ°¢Ğ¢Ğ ¢¦ö"ç&öw&W72Òã°¢¦ö"ç7FGW2ÒÖVF–F÷væÆöE7FGW3£¤6ö×ÆWFS°¢¦ö"æW'&÷$ÖW76vRæ6ÆV"‚“°¢&WGW&â¦ö#°§Ğ §7FC£§w7G&–ærÖVF–F÷væÆöE6W'f–6S£¤f÷&ÖDÆ&VÂ„ÖVF–÷WGWDf÷&ÖBf÷&ÖB§°¢7v—F6‚†f÷&ÖB¢°¢66RÖVF–÷WGWDf÷&ÖC£¤×C ¢&WGW&âÂ$ÕB#°¢66RÖVF–÷WGWDf÷&ÖC£¤×3 ¢&WGW&âÂ$Õ2#°¢66RÖVF–÷WGWDf÷&ÖC£¥vc ¢&WGW&âÂ%tb#°¢Ğ¢&WGW&âÂ$ÕB#°§Ğ §7FC£§w7G&–ærÖVF–F÷væÆöE6W'f–6S£¤×EVÆ—G”Æ&VÂ„×EVÆ—G’VÆ—G’§°¢7v—F6‚‡VÆ—G’¢°¢66R×EVÆ—G“£¤&W7C ¢&WGW&âÂ$&W7Bf–Æ&ÆR#°¢66R×EVÆ—G“£¥C3# ¢&WGW&âÂ#C3#ò„²#°¢66R×EVÆ—G“£¥#c ¢&WGW&âÂ##còD²#°¢66R×EVÆ—G“£¥CC ¢&WGW&âÂ#CC#°¢66R×EVÆ—G“£¥ƒ ¢&WGW&âÂ#ƒ#°¢66R×EVÆ—G“£¥s# ¢&WGW&âÂ#s##°¢66R×EVÆ—G“£¥Cƒ ¢&WGW&âÂ#Cƒ#°¢Ğ¢&WGW&âÂ$&W7Bf–Æ&ÆR#°§Ğ §7FC£§w7G&–ærÖVF–F÷væÆöE6W'f–6S£¤×4&—G&FTÆ&VÂ„×4&—G&FR&—G&FR§°¢7v—F6‚†&—G&FR¢°¢66R×4&—G&FS£¤³3# ¢&WGW&âÂ#3#¶'2#°¢66R×4&—G&FS£¤³#Sc ¢&WGW&âÂ##Sb¶'2#°¢66R×4&—G&FS£¤³“# ¢&WGW&âÂ#“"¶'2#°¢66R×4&—G&FS£¤³#ƒ ¢&WGW&âÂ##‚¶'2#°¢Ğ¢&WGW&âÂ#3#¶'2#°§Ğ §7FC£§w7G&–ærÖVF–F÷væÆöE6W'f–6S£¤ÖVF–G—TÆ&VÂ„ÖVF–G—RÖVF–G—R§°¢7v—F6‚†ÖVF–G—R¢°¢66RÖVF–G—S£¥f–FVó ¢&WGW&âÂ%f–FVò#°¢66RÖVF–G—S£¤VF–ó ¢&WGW&âÂ$VF–ò#°¢66RÖVF–G—S£¥Væ¶æ÷vã ¢'&V³°¢Ğ¢&WGW&âÂ%Væ¶æ÷vâ#°§Ğ §7FC£§w7G&–ærÖVF–F÷væÆöE6W'f–6S£¥7FGW4Æ&VÂ„ÖVF–F÷væÆöE7FGW27FGW2§°¢7v—F6‚‡7FGW2¢°¢66RÖVF–F÷væÆöE7FGW3£¤–FÆS ¢&WGW&âÂ$–FÆR#°¢66RÖVF–F÷væÆöE7FGW3£¤æÇ—¦–æs ¢&WGW&âÂ$æÇ—¦–ær#°¢66RÖVF–F÷væÆöE7FGW3£¥&VG“ ¢&WGW&âÂ%&VG’#°¢66RÖVF–F÷væÆöE7FGW3£¤F÷væÆöF–æs ¢&WGW&âÂ$F÷væÆöF–ær#°¢66RÖVF–F÷væÆöE7FGW3£¤6öçfW'F–æs ¢&WGW&âÂ$6öçfW'F–ær#°¢66RÖVF–F÷væÆöE7FGW3£¤6ö×ÆWFS ¢&WGW&âÂ$6ö×ÆWFR#°¢66RÖVF–F÷væÆöE7FGW3£¤f–ÆVC ¢&WGW&âÂ$f–ÆVB#°¢66RÖVF–F÷væÆöE7FGW3£¤6æ6VÆÆVC ¢&WGW&âÂ$6æ6VÆÆVB#°¢Ğ¢&WGW&âÂ$–FÆR#°§Ğ §7FC£§w7G&–ærÖVF–F÷væÆöE6W'f–6S£¥6æ—F—¦Tf–ÆTæÖR†6öç7B7FC£§w7G&–ærbfÇVR§°¢7FC£§w7G&–ær6æ—F—¦VC°¢f÷"‡v6†%÷B6‚¢fÇVR¢°¢–b†6‚Â3"ÇÂv766‡"„Â#Ãã¥Â"õÅÇÃò¢"Â6‚’¢°¢6æ—F—¦VBçW6…ö&6²„Âuòr“°¢Ğ¢VÇ6P¢°¢6æ—F—¦VBçW6…ö&6²†6‚“°¢Ğ¢Ğ ¢v†–ÆR‚6æ—F—¦VBæV×G’‚’bb‡6æ—F—¦VBæ&6²‚’ÓÒÂrârÇÂ6æ—F—¦VBæ&6²‚’ÓÒÂrr’¢°¢6æ—F—¦VBç÷ö&6²‚“°¢Ğ ¢v†–ÆR‚6æ—F—¦VBæV×G’‚’bb6æ—F—¦VBæg&öçB‚’ÓÒÂrr¢°¢6æ—F—¦VBæW&6R‡6æ—F—¦VBæ&Vv–â‚’“°¢Ğ ¢–b‡6æ—F—¦VBæV×G’‚’¢°¢6æ—F—¦VBÒÂ&ÖVF–#°¢Ğ ¢–b‡6æ—F—¦VBç6—¦R‚’âc¢°¢6æ—F—¦VBç&W6—¦Rƒc“°¢Ğ ¢&WGW&â6æ—F—¦VC°§Ğ