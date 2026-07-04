#include "AnimeTrackerService.h"

#include <windows.h>
#include <winhttp.h>
#include <wininet.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <system_error>

namespace
{
constexpr DWORD kAniListTimeoutMs = 12000;
constexpr size_t kMaxAniListResponseBytes = 4 * 1024 * 1024;
constexpr size_t kMaxMyAnimeListResponseBytes = 8 * 1024 * 1024;
constexpr int kAniListMalMapBatchSize = 50;

std::wstring AniListHttpErrorMessage(DWORD statusCode)
{
    if (statusCode == 400)
    {
        return L"AniList rejected the search request. Please try again in a moment.";
    }
    if (statusCode == 403)
    {
        return L"AniList blocked the request. Please try again later.";
    }
    if (statusCode >= 500)
    {
        return L"AniList is having trouble right now. Please try again later.";
    }
    return L"AniList returned HTTP " + std::to_wstring(statusCode) + L". Please try again later.";
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

struct InternetHandle
{
    HINTERNET handle = nullptr;

    explicit InternetHandle(HINTERNET value = nullptr) : handle(value) {}
    ~InternetHandle()
    {
        if (handle)
        {
            InternetCloseHandle(handle);
        }
    }

    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;

    operator HINTERNET() const
    {
        return handle;
    }
};

std::string WideToUtf8(const std::wstring& text);

bool ReadInternetResponse(HINTERNET request, std::string& responseBody, std::wstring& errorMessage)
{
    responseBody.clear();
    for (;;)
    {
        char buffer[8192] {};
        DWORD read = 0;
        if (!InternetReadFile(request, buffer, static_cast<DWORD>(sizeof(buffer)), &read))
        {
            errorMessage = L"Could not read the AniList response.";
            return false;
        }
        if (read == 0)
        {
            break;
        }
        if (responseBody.size() + read > kMaxAniListResponseBytes)
        {
            errorMessage = L"AniList returned too much data. Try a narrower search.";
            return false;
        }
        responseBody.append(buffer, buffer + read);
    }
    return true;
}

bool ReadInternetResponseLimited(
    HINTERNET request,
    size_t maxBytes,
    const wchar_t* serviceName,
    std::string& responseBody,
    std::wstring& errorMessage)
{
    responseBody.clear();
    for (;;)
    {
        char buffer[8192] {};
        DWORD read = 0;
        if (!InternetReadFile(request, buffer, static_cast<DWORD>(sizeof(buffer)), &read))
        {
            errorMessage = L"Could not read the ";
            errorMessage += serviceName;
            errorMessage += L" response.";
            return false;
        }
        if (read == 0)
        {
            break;
        }
        if (responseBody.size() + read > maxBytes)
        {
            errorMessage = serviceName;
            errorMessage += L" returned too much data. Try a smaller public list.";
            return false;
        }
        responseBody.append(buffer, buffer + read);
    }
    return true;
}

bool ExecuteGraphQlWithWinInet(const std::string& requestBody, std::string& responseBody, std::wstring& errorMessage)
{
    InternetHandle session(InternetOpenW(
        L"RexToolkitAnimeTracker/1.0",
        INTERNET_OPEN_TYPE_PRECONFIG,
        nullptr,
        nullptr,
        0));
    if (!session)
    {
        errorMessage = L"Could not reach AniList. Check your internet connection and try again.";
        return false;
    }

    DWORD timeout = kAniListTimeoutMs;
    InternetSetOptionW(session, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionW(session, INTERNET_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionW(session, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

    InternetHandle connection(InternetConnectW(
        session,
        L"graphql.anilist.co",
        INTERNET_DEFAULT_HTTPS_PORT,
        nullptr,
        nullptr,
        INTERNET_SERVICE_HTTP,
        0,
        0));
    if (!connection)
    {
        errorMessage = L"Could not reach AniList. Check your internet connection and try again.";
        return false;
    }

    const wchar_t* acceptTypes[] = { L"application/json", nullptr };
    InternetHandle request(HttpOpenRequestW(
        connection,
        L"POST",
        L"/",
        nullptr,
        nullptr,
        acceptTypes,
        INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE,
        0));
    if (!request)
    {
        errorMessage = L"Could not reach AniList. Check your internet connection and try again.";
        return false;
    }

    const wchar_t headers[] = L"Content-Type: application/json\r\nAccept: application/json\r\n";
    if (!HttpSendRequestW(
            request,
            headers,
            static_cast<DWORD>(-1),
            const_cast<char*>(requestBody.data()),
            static_cast<DWORD>(requestBody.size())))
    {
        errorMessage = L"Could not reach AniList. Check your internet connection and try again.";
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    if (!HttpQueryInfoW(
            request,
            HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
            &statusCode,
            &statusCodeSize,
            nullptr))
    {
        errorMessage = L"Could not read the AniList response.";
        return false;
    }

    if (statusCode == 429)
    {
        errorMessage = L"AniList is rate limiting requests. Please wait a moment and try again.";
        return false;
    }
    if (statusCode < 200 || statusCode >= 300)
    {
        errorMessage = AniListHttpErrorMessage(statusCode);
        return false;
    }

    return ReadInternetResponse(request, responseBody, errorMessage);
}

std::wstring UrlEncodePathSegment(const std::wstring& value)
{
    const std::string utf8 = WideToUtf8(value);
    std::wostringstream output;
    output << std::uppercase << std::hex;
    for (unsigned char ch : utf8)
    {
        const bool unreserved =
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '-' ||
            ch == '_' ||
            ch == '.' ||
            ch == '~';
        if (unreserved)
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

bool ExecuteMyAnimeListGet(const std::wstring& userName, int offset, std::string& responseBody, std::wstring& errorMessage)
{
    InternetHandle session(InternetOpenW(
        L"RexToolkitAnimeTracker/1.0",
        INTERNET_OPEN_TYPE_PRECONFIG,
        nullptr,
        nullptr,
        0));
    if (!session)
    {
        errorMessage = L"Could not reach MyAnimeList. Check your internet connection and try again.";
        return false;
    }

    DWORD timeout = kAniListTimeoutMs;
    InternetSetOptionW(session, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionW(session, INTERNET_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionW(session, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

    const std::wstring url =
        L"https://myanimelist.net/animelist/" +
        UrlEncodePathSegment(userName) +
        L"/load.json?offset=" +
        std::to_wstring(std::max(0, offset)) +
        L"&status=7";
    const wchar_t headers[] = L"Accept: application/json\r\nCache-Control: no-cache\r\n";
    InternetHandle request(InternetOpenUrlW(
        session,
        url.c_str(),
        headers,
        static_cast<DWORD>(-1),
        INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE,
        0));
    if (!request)
    {
        errorMessage = L"Could not reach that MyAnimeList profile. Make sure the username is correct and the anime list is public.";
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    if (HttpQueryInfoW(
            request,
            HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
            &statusCode,
            &statusCodeSize,
            nullptr))
    {
        if (statusCode == 404)
        {
            errorMessage = L"MyAnimeList could not find that profile.";
            return false;
        }
        if (statusCode == 403)
        {
            errorMessage = L"MyAnimeList blocked that public list request or the list is private.";
            return false;
        }
        if (statusCode < 200 || statusCode >= 300)
        {
            errorMessage = L"MyAnimeList returned HTTP " + std::to_wstring(statusCode) + L". Please try again later.";
            return false;
        }
    }

    return ReadInternetResponseLimited(
        request,
        kMaxMyAnimeListResponseBytes,
        L"MyAnimeList",
        responseBody,
        errorMessage);
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

std::wstring TrimHtml(const std::wstring& value)
{
    std::wstring output;
    output.reserve(value.size());
    bool insideTag = false;
    for (wchar_t ch : value)
    {
        if (ch == L'<')
        {
            insideTag = true;
            continue;
        }
        if (ch == L'>')
        {
            insideTag = false;
            continue;
        }
        if (!insideTag)
        {
            output.push_back(ch);
        }
    }
    return output;
}

void AppendUtf8Codepoint(std::string& output, unsigned int codepoint)
{
    if (codepoint <= 0x7F)
    {
        output.push_back(static_cast<char>(codepoint));
    }
    else if (codepoint <= 0x7FF)
    {
        output.push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
    else
    {
        output.push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

namespace Json
{
enum class Type
{
    Null,
    Bool,
    Number,
    String,
    Array,
    Object
};

struct Value
{
    Type type = Type::Null;
    bool boolValue = false;
    double numberValue = 0.0;
    std::string stringValue;
    std::vector<Value> arrayValue;
    std::map<std::string, Value> objectValue;
};

class Parser
{
public:
    explicit Parser(const std::string& text) : text_(text) {}

    bool Parse(Value& value)
    {
        SkipWhitespace();
        if (!ParseValue(value))
        {
            return false;
        }
        SkipWhitespace();
        return position_ == text_.size();
    }

private:
    bool ParseValue(Value& value)
    {
        if (AtEnd())
        {
            return false;
        }

        const char ch = Peek();
        if (ch == 'n')
        {
            return ParseLiteral("null", Type::Null, value);
        }
        if (ch == 't')
        {
            if (!ParseLiteral("true", Type::Bool, value))
            {
                return false;
            }
            value.boolValue = true;
            return true;
        }
        if (ch == 'f')
        {
            if (!ParseLiteral("false", Type::Bool, value))
            {
                return false;
            }
            value.boolValue = false;
            return true;
        }
        if (ch == '"')
        {
            value.type = Type::String;
            return ParseString(value.stringValue);
        }
        if (ch == '[')
        {
            return ParseArray(value);
        }
        if (ch == '{')
        {
            return ParseObject(value);
        }
        return ParseNumber(value);
    }

    bool ParseLiteral(const char* literal, Type type, Value& value)
    {
        const size_t length = strlen(literal);
        if (text_.compare(position_, length, literal) != 0)
        {
            return false;
        }
        position_ += length;
        value = {};
        value.type = type;
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
            {
                unsigned int codepoint ×~4òÚ$z{-®éÜj×"Â”åDU$äUEôDTdTÅEô…EE5õõ%BÂ’“°¢–b‚6öææV7F–öâ¢°¢&WGW&âG'•v–ä–æWDfÆÆ&6²‚“°¢Ð ¢v–ä‡GG†æFÆR&WVW7B…v–ä‡GG÷Vå&WVW7B€¢6öææV7F–öâÀ¢Â%õ5B"À¢Â"ò"À¢çVÆÇG"À¢t”ä…EEôäõõ$TdU$U"À¢t”ä…EEôDTdTÅEô44UEõE•U2À¢t”ä…EEôdÄuõ4T5U$R’“°¢–b‚&WVW7B¢°¢&WGW&âG'•v–ä–æWDfÆÆ&6²‚“°¢Ð ¢6öç7Bv6†%÷B†VFW'5µÒÒÂ$6öçFVçBÕG—S¢Æ–6F–öâö§6öåÇ%Æä66WC¢Æ–6F–öâö§6öåÇ%Æâ#°¢–b‚v–ä‡GG6VæE&WVW7B€¢&WVW7BÀ¢†VFW'2À¢7FF–5ö67CÄEtõ$Câ‚Ó’À¢6öç7Eö67CÆ6†"£â‡&WVW7D&öG’æFF‚’’À¢7FF–5ö67CÄEtõ$Câ‡&WVW7D&öG’ç6—¦R‚’’À¢7FF–5ö67CÄEtõ$Câ‡&WVW7D&öG’ç6—¦R‚’’À¢’ÇÀ¢v–ä‡GG&V6V—fU&W7öç6R‡&WVW7BÂçVÆÇG"’¢°¢&WGW&âG'•v–ä–æWDfÆÆ&6²‚“°¢Ð ¢Etõ$B7FGW46öFRÒ°¢Etõ$B7FGW46öFU6—¦RÒ6—¦Vöb‡7FGW46öFR“°¢–b‚v–ä‡GGVW'”†VFW'2€¢&WVW7BÀ¢t”ä…EEõTU%•õ5DEU5ô4ôDRÂt”ä…EEõTU%•ôdÄuôåTÔ$U"À¢t”ä…EEô„TDU%ôäÔUô%•ô”äDU‚À¢g7FGW46öFRÀ¢g7FGW46öFU6—¦RÀ¢t”ä…EEôäõô„TDU%ô”äDU‚’¢°¢&WGW&âG'•v–ä–æWDfÆÆ&6²‚“°¢Ð ¢–b‡7FGW46öFRÓÒC#’¢°¢W'&÷$ÖW76vRÒÂ$æ”Æ—7B—2&FRÆ–Ö—F–ær&WVW7G2âÆV6Rv—BÖöÖVçBæBG'’v–ââ#°¢&WGW&âfÇ6S°¢Ð¢–b‡7FGW46öFRÂ#ÇÂ7FGW46öFRãÒ3¢°¢W'&÷$ÖW76vRÒæ”Æ—7D‡GGW'&÷$ÖW76vR‡7FGW46öFR“°¢&WGW&âfÇ6S°¢Ð ¢&W7öç6T&öG’æ6ÆV"‚“°¢f÷"ƒ³²¢°¢Etõ$Bf–Æ&ÆRÒ°¢–b‚v–ä‡GGVW'”FFf–Æ&ÆR‡&WVW7BÂff–Æ&ÆR’¢°¢W'&÷$ÖW76vRÒÂ$6÷VÆBæ÷B&VBF†Ræ”Æ—7B&W7öç6Râ#°¢&WGW&âfÇ6S°¢Ð¢–b†f–Æ&ÆRÓÒ¢°¢'&V³°¢Ð¢–b‡&W7öç6T&öG’ç6—¦R‚’²f–Æ&ÆRâ´Ö„æ”Æ—7E&W7öç6T'—FW2¢°¢W'&÷$ÖW76vRÒÂ$æ”Æ—7B&WGW&æVBFöò×V6‚FFâG'’æ'&÷vW"6V&6‚â#°¢&WGW&âfÇ6S°¢Ð ¢7FC£§7G&–ær'VffW"†f–Æ&ÆRÂuÃr“°¢Etõ$B&VBÒ°¢–b‚v–ä‡GG&VDFF‡&WVW7BÂ'VffW"æFF‚’Âf–Æ&ÆRÂg&VB’¢°¢W'&÷$ÖW76vRÒÂ$6÷VÆBæ÷B&VBF†Ræ”Æ—7B&W7öç6Râ#°¢&WGW&âfÇ6S°¢Ð¢'VffW"ç&W6—¦R‡&VB“°¢&W7öç6T&öG’³Ò'VffW#°¢Ð ¢&WGW&âG'VS°§Ð ¤æ–ÖUvF6„Æ—7Bæ–ÖTÆ—7E7F÷&vS£¤ÆöB†6öç7B7FC£¦f–ÆW7—7FVÓ£§F‚bF‚Â7FC£§w7G&–ærbv&æ–ær’6öç7@§°¢æ–ÖUvF6„Æ—7BÆ—7C°¢7FC£¦–g7G&VÒf–ÆR‡F‚Â7FC£¦–÷3£¦&–æ'’“°¢–b‚f–ÆR¢°¢&WGW&âÆ—7C°¢Ð ¢7FC£§7G&–ær§6öâ‚‡7FC£¦—7G&VÖ'Veö—FW&F÷#Æ6†#â†f–ÆR’’Â7FC£¦—7G&VÖ'Veö—FW&F÷#Æ6†#â‚’“°¢–b†§6öâç6—¦R‚’ãÒ2b`¢7FF–5ö67CÇVç6–væVB6†#â†§6öå³Ò’ÓÒ„Tbb`¢7FF–5ö67CÇVç6–væVB6†#â†§6öå³Ò’ÓÒ„$"b`¢7FF–5ö67CÇVç6–væVB6†#â†§6öå³%Ò’ÓÒ„$b¢°¢§6öâæW&6RƒÂ2“°¢Ð ¢§6öã£¥fÇVR&ö÷C°¢§6öã£¥'6W"'6W"†§6öâ“°¢–b‚'6W"å'6R‡&ö÷B’¢°¢v&æ–ærÒÂ$æ–ÖRG&6¶W"6÷VÆBæ÷B&VBF†R6fVBÆ—7BÂ6ò—B7F'FVBV×G’â#°¢&WGW&âÆ—7C°¢Ð ¢Æ—7BçfW'6–öâÒ7FC£¦Ö‚ƒÂ§6öã£¤–çB„§6öã£¤B‡&ö÷BÂ'fW'6–öâ"’’“°¢6öç7B§6öã£¥fÇVR¢æ–ÖRÒ§6öã£¤B‡&ö÷BÂ&æ–ÖR"“°¢–b†æ–ÖRbbæ–ÖRÓçG—RÓÒ§6öã£¥G—S£¤'&’¢°¢f÷"†6öç7B§6öã£¥fÇVRbVçG'•fÇVR¢æ–ÖRÓæ'&•fÇVR¢°¢æ–ÖTVçG'’VçG'’ÒVçG'”g&öÔ§6öâ†VçG'•fÇVR“°¢–b†VçG'’ææ–Æ—7D–BÒ¢°¢Æ—7Bææ–ÖRçW6…ö&6²‡7FC£¦Ö÷fR†VçG'’’“°¢Ð¢Ð¢Ð¢&WGW&âÆ—7C°§Ð ¦&ööÂæ–ÖTÆ—7E7F÷&vS£¥6fR†6öç7B7FC£¦f–ÆW7—7FVÓ£§F‚bF‚Â6öç7Bæ–ÖUvF6„Æ—7BbÆ—7BÂ7FC£§w7G&–ærbW'&÷$ÖW76vR’6öç7@§°¢7FC£¦W'&÷%ö6öFRf–ÆTW'&÷#°¢7FC£¦f–ÆW7—7FVÓ£¦7&VFUöF—&V7F÷&–W2‡F‚ç&VçE÷F‚‚’Âf–ÆTW'&÷"“°¢–b†f–ÆTW'&÷"¢°¢W'&÷$ÖW76vRÒÂ$6÷VÆBæ÷B6fRæ–ÖRG&6¶W"FFâF†RFFföÆFW"6÷VÆBæ÷B&R7&VFVBâ#°¢&WGW&âfÇ6S°¢Ð ¢–b‡7FC£¦f–ÆW7—7FVÓ£¦W†—7G2‡F‚Âf–ÆTW'&÷"’¢°¢7FC£¦f–ÆW7—7FVÓ£¦6÷•öf–ÆR‡F‚ÂF‚çw7G&–ær‚’²Â"æ&²"Â7FC£¦f–ÆW7—7FVÓ£¦6÷•ö÷F–öç3£¦÷fW'w&—FUöW†—7F–ærÂf–ÆTW'&÷"“°¢f–ÆTW'&÷"æ6ÆV"‚“°¢Ð ¢7FC£¦÷7G&–æw7G&VÒ÷WGWC°¢÷WGWBÃÂ'µÆâÂ'fW'6–öåÂ#¢ÅÆâÂ&æ–ÖUÂ#¢µÆâ#°¢f÷"‡6—¦U÷B–æFW‚Ò²–æFW‚ÂÆ—7Bææ–ÖRç6—¦R‚“²²¶–æFW‚¢°¢6öç7Bæ–ÖTVçG'’bVçG'’ÒÆ—7Bææ–ÖU¶–æFW…Ó°¢÷WGWBÃÂ"µÆâ#°¢w&—FT§6öä–çDf–VÆB†÷WGWBÂ""Â&æ–Æ—7D–B"ÂVçG'’ææ–Æ—7D–B“°¢w&—FT§6öä–çDf–VÆB†÷WGWBÂ""Â&–DÖÂ"ÂVçG'’æ–DÖÂ“°¢w&—FT§6öå7G&–ætf–VÆB†÷WGWBÂ""Â'F—FÆR"ÂVçG'’çF—FÆR“°¢w&—FT§6öå7G&–ætf–VÆB†÷WGWBÂ""Â&6÷fW$–ÖvR"ÂVçG'’æ6÷fW$–ÖvUW&Â“°¢w&—FT§6öå7G&–ætf–VÆB†÷WGWBÂ""Â&f÷&ÖB"ÂVçG'’æf÷&ÖB“°¢w&—FT§6öå7G&–ætf–VÆB†÷WGWBÂ""Â&—&–æu7FGW2"ÂVçG'’æ—&–æu7FGW2“°¢w&—FT§6öå7G&–ætf–VÆB†÷WGWBÂ""Â'W6W%7FGW2"Âæ–ÖUG&6¶W%6W'f–6S£¥W6W%7FGW4Æ&VÂ†VçG'’çW6W%7FGW2’“°¢w&—FT§6öä–çDf–VÆB†÷WGWBÂ""Â&7W'&VçDW—6öFR"ÂVçG'’æ7W'&VçDW—6öFR“°¢w&—FT§6öä–çDf–VÆB†÷WGWBÂ""Â'F÷FÄW—6öFW2"ÂVçG'’çF÷FÄW—6öFW2“°¢÷WGWBÃÂ6W&–Æ—¦T—&–æt–æfò†VçG'’ææW‡D—&–ætW—6öFRÂ""’ÃÂ"ÅÆâ#°¢÷WGWBÃÂ"Â'&VÆF–öç5Â#¢µÆâ#°¢f÷"‡6—¦U÷B&VÆF–öä–æFW‚Ò²&VÆF–öä–æFW‚ÂVçG'’ç&VÆF–öç2ç6—¦R‚“²²·&VÆF–öä–æFW‚¢°¢w&—FU&VÆF–öâ†÷WGWBÂVçG'’ç&VÆF–öç5·&VÆF–öä–æFW…ÒÂ""Â&VÆF–öä–æFW‚²ÂVçG'’ç&VÆF–öç2ç6—¦R‚’“°¢Ð¢÷WGWBÃÂ"ÒÅÆâ#°¢w&—FT§6öå7G&–ætf–VÆB†÷WGWBÂ""Â&æ÷FW2"ÂVçG'’ææ÷FW2“°¢÷WGWBÃÂ"Â&ff÷&—FUÂ#¢"ÃÂ†VçG'’æff÷&—FRò'G'VR"¢&fÇ6R"’ÃÂ"ÅÆâ#°¢w&—FT§6öå7G&–ætf–VÆB†÷WGWBÂ""Â&Æ7E&Vg&W6†VB"ÂVçG'’æÆ7E&Vg&W6†VB“°¢w&—FT§6öå7G&–ætf–VÆB†÷WGWBÂ""Â'6—FUW&Â"ÂVçG'’ç6—FUW&ÂÂfÇ6R“°¢÷WGWBÃÂ"Ò#°¢–b†–æFW‚²ÂÆ—7Bææ–ÖRç6—¦R‚’¢°¢÷WGWBÃÂ"Â#°¢Ð¢÷WGWBÃÂ%Æâ#°¢Ð¢÷WGWBÃÂ"ÕÆçÕÆâ#° ¢7FC£¦ög7G&VÒf–ÆR‡F‚Â7FC£¦–÷3£¦&–æ'’Â7FC£¦–÷3£§G'Væ2“°¢–b‚f–ÆR¢°¢W'&÷$ÖW76vRÒÂ$6÷VÆBæ÷B6fRæ–ÖRG&6¶W"FFâW&Ö—76–öâv2FVæ–VBâ#°¢&WGW&âfÇ6S°¢Ð¢f–ÆRÃÂ÷WGWBç7G"‚“°¢–b‚f–ÆR¢°¢W'&÷$ÖW76vRÒÂ$6÷VÆBæ÷B6fRæ–ÖRG&6¶W"FFâ#°¢&WGW&âfÇ6S°¢Ð¢&WGW&âG'VS°§Ð §7FC£§fV7F÷#Äæ–ÖU&VÆF–öãâæ–ÖU&VÆF–öåG&6¶W#£¥W6öÖ–æu6WVVÇ2†6öç7Bæ–ÖUvF6„Æ—7BbÆ—7B§°¢7FC£§fV7F÷#Äæ–ÖU&VÆF–öãâ&VÆF–öç3°¢f÷"†6öç7Bæ–ÖTVçG'’bVçG'’¢Æ—7Bææ–ÖR¢°¢f÷"†6öç7Bæ–ÖU&VÆF–öâb&VÆF–öâ¢VçG'’ç&VÆF–öç2¢°¢–b„—5W6öÖ–æu6WVVÄ6æF–FFR‡&VÆF–öâ’b`¢7FC£¦æöæUööb‡&VÆF–öç2æ&Vv–â‚’Â&VÆF–öç2æVæB‚’Â²g&VÆF–öåÒ†6öç7Bæ–ÖU&VÆF–öâbW†—7F–ær¢°¢&WGW&âW†—7F–ærææ–Æ—7D–BÓÒ&VÆF–öâææ–Æ—7D–C°¢Ò’b`¢æ–ÖUG&6¶W%6W'f–6S£¤6öçF–ç4æ–ÖR†Æ—7BÂ&VÆF–öâææ–Æ—7D–B’¢°¢&VÆF–öç2çW6…ö&6²‡&VÆF–öâ“°¢Ð¢Ð¢Ð ¢7FC£§6÷'B‡&VÆF–öç2æ&Vv–â‚’Â&VÆF–öç2æVæB‚’ÂµÒ†6öç7Bæ–ÖU&VÆF–öâbÆVgBÂ6öç7Bæ–ÖU&VÆF–öâb&–v‡B¢°¢6öç7BÆöærÆöærÆVgD—&–ærÒÆVgBææW‡D—&–ætW—6öFRæ†5fÇVRòÆVgBææW‡D—&–ætW—6öFRæ—&–ætB¢°¢6öç7BÆöærÆöær&–v‡D—&–ærÒ&–v‡BææW‡D—&–ætW—6öFRæ†5fÇVRò&–v‡BææW‡D—&–ætW—6öFRæ—&–ætB¢°¢–b†ÆVgD—&–ærÒ&–v‡D—&–ær¢°¢&WGW&âÆVgD—&–ærÒbb‡&–v‡D—&–ærÓÒÇÂÆVgD—&–ærÂ&–v‡D—&–ær“°¢Ð¢–b†ÆVgBç6V6öå–V"Ò&–v‡Bç6V6öå–V"¢°¢&WGW&âÆVgBç6V6öå–V"Â&–v‡Bç6V6öå–V#°¢Ð¢&WGW&âÆVgBçF—FÆRÂ&–v‡BçF—FÆS°¢Ò“° ¢&WGW&â&VÆF–öç3°§Ð ¦&ööÂæ–ÖU&VÆF–öåG&6¶W#£¤—5W6öÖ–æu6WVVÄ6æF–FFR†6öç7Bæ–ÖU&VÆF–öâb&VÆF–öâ§°¢–b‡&VÆF–öâç&VÆF–öåG—RÒÂ%4UTTÂ"¢°¢&WGW&âfÇ6S°¢Ð¢–b‡&VÆF–öâç7FGW2ÓÒÂ$äõEõ”UEõ$TÄT4TB"ÇÂ&VÆF–öâç7FGW2ÓÒÂ%$TÄT4”är"ÇÂ&VÆF–öâææW‡D—&–ætW—6öFRæ†5fÇVR¢°¢&WGW&âG'VS°¢Ð ¢5•5DTÕD”ÔRÆö6ÅF–ÖR·Ó°¢vWDÆö6ÅF–ÖR‚fÆö6ÅF–ÖR“°¢&WGW&â&VÆF–öâç7F'DFFRç–V"ãÒ7FF–5ö67CÆ–çCâ†Æö6ÅF–ÖRçu–V"’ÇÀ¢&VÆF–öâç6V6öå–V"ãÒ7FF–5ö67CÆ–çCâ†Æö6ÅF–ÖRçu–V"“°§Ð ¤æ–ÖUG&6¶W%6W'f–6S£¤æ–ÖUG&6¶W%6W'f–6R‚’ÒFVfVÇC° ¤æ–ÖUvF6„Æ—7Bæ–ÖUG&6¶W%6W'f–6S£¤ÆöEvF6„Æ—7B†6öç7B7FC£¦f–ÆW7—7FVÓ£§F‚bF‚Â7FC£§w7G&–ærbv&æ–ær’6öç7@§°¢&WGW&â7F÷&vUòäÆöB‡F‚Âv&æ–ær“°§Ð ¦&ööÂæ–ÖUG&6¶W%6W'f–6S£¥6fUvF6„Æ—7B†6öç7B7FC£¦f–ÆW7—7FVÓ£§F‚bF‚Â6öç7Bæ–ÖUvF6„Æ—7BbÆ—7BÂ7FC£§w7G&–ærbW'&÷$ÖW76vR’6öç7@§°¢&WGW&â7F÷&vUòå6fR‡F‚ÂÆ—7BÂW'&÷$ÖW76vR“°§Ð ¤æ–ÖU6V&6…&W7öç6Ræ–ÖUG&6¶W%6W'f–6S£¥6V&6„æ–ÖR†6öç7B7FC£§w7G&–ærb6V&6…FW‡BÂ–çBvRÂ–çBW%vRÂ7FC£§w7G&–ærbW'&÷$ÖW76vR’6öç7@§°¢&WGW&â”6Æ–VçEòå6V&6„æ–ÖR‡6V&6…FW‡BÂvRÂW%vRÂW'&÷$ÖW76vR“°§Ð §7FC£¦÷F–öæÃÄæ–ÖU6V&6…&W7VÇCâæ–ÖUG&6¶W%6W'f–6S£¥&Vg&W6„æ–ÖR†–çBæ–Æ—7D–BÂ7FC£§w7G&–ærbW'&÷$ÖW76vR’6öç7@§°¢&WGW&â”6Æ–VçEòäfWF6„æ–ÖT'”–B†æ–Æ—7D–BÂW'&÷$ÖW76vR“°§Ð ¤æ–ÖT–×÷'E&W7VÇBæ–ÖUG&6¶W%6W'f–6S£¤–×÷'EV&Æ–4æ–ÖTÆ—7B†6öç7B7FC£§w7G&–ærbW6W$æÖRÂ7FC£§w7G&–ærbW'&÷$ÖW76vR’6öç7@§°¢&WGW&â–×÷'EV&Æ–4æ–ÖTÆ—7B‡W6W$æÖRÂæ–ÖT–×÷'E6÷W&6S£¤æ”Æ—7BÂW'&÷$ÖW76vR“°§Ð ¤æ–ÖT–×÷'E&W7VÇBæ–ÖUG&6¶W%6W'f–6S£¤–×÷'EV&Æ–4æ–ÖTÆ—7B€¢6öç7B7FC£§w7G&–ærbW6W$æÖRÀ¢æ–ÖT–×÷'E6÷W&6R6÷W&6RÀ¢7FC£§w7G&–ærbW'&÷$ÖW76vR’6öç7@§°¢7v—F6‚‡6÷W&6R¢°¢66Ræ–ÖT–×÷'E6÷W&6S£¤×”æ–ÖTÆ—7C ¢&WGW&â×”æ–ÖTÆ—7D6Æ–VçEòä–×÷'EV&Æ–4æ–ÖTÆ—7B‡W6W$æÖRÂ”6Æ–VçEòÂW'&÷$ÖW76vR“°¢66Ræ–ÖT–×÷'E6÷W&6S£¤æ”Æ—7C ¢66Ræ–ÖT–×÷'E6÷W&6S£¤WFó ¢FVfVÇC ¢&WGW&â”6Æ–VçEòä–×÷'EV&Æ–4æ–ÖTÆ—7B‡W6W$æÖRÂW'&÷$ÖW76vR“°¢Ð§Ð ¤æ–ÖTVçG'’æ–ÖUG&6¶W%6W'f–6S£¤VçG'”g&öÕ6V&6…&W7VÇB†6öç7Bæ–ÖU6V&6…&W7VÇBb&W7VÇB§°¢æ–ÖTVçG'’VçG'“°¢VçG'’ææ–Æ—7D–BÒ&W7VÇBææ–Æ—7D–C°¢VçG'’æ–DÖÂÒ&W7VÇBæ–DÖÃ°¢VçG'’çF—FÆRÒ&W7VÇBçF—FÆS°¢VçG'’æ6÷fW$–ÖvUW&ÂÒ&W7VÇBæ6÷fW$–ÖvUW&Ã°¢VçG'’æf÷&ÖBÒ&W7VÇBæf÷&ÖC°¢VçG'’æ—&–æu7FGW2Ò&W7VÇBç7FGW3°¢VçG'’çW6W%7FGW2Òæ–ÖUW6W%7FGW3£¥ÆææVC°¢VçG'’æ7W'&VçDW—6öFRÒ°¢VçG'’çF÷FÄW—6öFW2Ò&W7VÇBæW—6öFW3°¢VçG'’ææW‡D—&–ætW—6öFRÒ&W7VÇBææW‡D—&–ætW—6öFS°¢VçG'’ç&VÆF–öç2Ò&W7VÇBç&VÆF–öç3°¢VçG'’æÆ7E&Vg&W6†VBÒ—6ôæ÷uWF2‚“°¢VçG'’ç6—FUW&ÂÒ&W7VÇBç6—FUW&Ã°¢&WGW&âVçG'“°§Ð §fö–Bæ–ÖUG&6¶W%6W'f–6S£¤Ç”ÖWFFF„æ–ÖTVçG'’bVçG'’Â6öç7Bæ–ÖU6V&6…&W7VÇBb&W7VÇB§°¢VçG'’æ–DÖÂÒ&W7VÇBæ–DÖÃ°¢VçG'’çF—FÆRÒ&W7VÇBçF—FÆS°¢VçG'’æ6÷fW$–ÖvUW&ÂÒ&W7VÇBæ6÷fW$–ÖvUW&Ã°¢VçG'’æf÷&ÖBÒ&W7VÇBæf÷&ÖC°¢VçG'’æ—&–æu7FGW2Ò&W7VÇBç7FGW3°¢VçG'’çF÷FÄW—6öFW2Ò&W7VÇBæW—6öFW3°¢VçG'’ææW‡D—&–ætW—6öFRÒ&W7VÇBææW‡D—&–ætW—6öFS°¢VçG'’ç&VÆF–öç2Ò&W7VÇBç&VÆF–öç3°¢VçG'’ç6—FUW&ÂÒ&W7VÇBç6—FUW&Ã°¢VçG'’æÆ7E&Vg&W6†VBÒ—6ôæ÷uWF2‚“°¢–b†VçG'’çF÷FÄW—6öFW2âbbVçG'’æ7W'&VçDW—6öFRâVçG'’çF÷FÄW—6öFW2¢°¢VçG'’æ7W'&VçDW—6öFRÒVçG'’çF÷FÄW—6öFW3°¢Ð§Ð ¦&ööÂæ–ÖUG&6¶W%6W'f–6S£¤6öçF–ç4æ–ÖR†6öç7Bæ–ÖUvF6„Æ—7BbÆ—7BÂ–çBæ–Æ—7D–B§°¢&WGW&â7FC£¦ç•ööb†Æ—7Bææ–ÖRæ&Vv–â‚’ÂÆ—7Bææ–ÖRæVæB‚’Â¶æ–Æ—7D–EÒ†6öç7Bæ–ÖTVçG'’bVçG'’¢°¢&WGW&âVçG'’ææ–Æ—7D–BÓÒæ–Æ—7D–C°¢Ò“°§Ð §7FC£§w7G&–æræ–ÖUG&6¶W%6W'f–6S£¥W6W%7FGW4Æ&VÂ„æ–ÖUW6W%7FGW27FGW2§°¢7v—F6‚‡7FGW2¢°¢66Ræ–ÖUW6W%7FGW3£¥vF6†–æs ¢&WGW&âÂ%vF6†–ær#°¢66Ræ–ÖUW6W%7FGW3£¥ÆææVC ¢&WGW&âÂ%ÆææVB#°¢66Ræ–ÖUW6W%7FGW3£¤6ö×ÆWFVC ¢&WGW&âÂ$6ö×ÆWFVB#°¢66Ræ–ÖUW6W%7FGW3£¤öä†öÆC ¢&WGW&âÂ$öâ†öÆB#°¢66Ræ–ÖUW6W%7FGW3£¤G&÷VC ¢&WGW&âÂ$G&÷VB#°¢Ð¢&WGW&âÂ%ÆææVB#°§Ð ¤æ–ÖUW6W%7FGW2æ–ÖUG&6¶W%6W'f–6S£¥W6W%7FGW4g&öÕ7G&–ær†6öç7B7FC£§w7G&–ærbFW‡B§°¢–b‡FW‡BÓÒÂ%vF6†–ær"¢°¢&WGW&âæ–ÖUW6W%7FGW3£¥vF6†–æs°¢Ð¢–b‡FW‡BÓÒÂ$6ö×ÆWFVB"¢°¢&WGW&âæ–ÖUW6W%7FGW3£¤6ö×ÆWFVC°¢Ð¢–b‡FW‡BÓÒÂ$öâ†öÆB"¢°¢&WGW&âæ–ÖUW6W%7FGW3£¤öä†öÆC°¢Ð¢–b‡FW‡BÓÒÂ$G&÷VB"¢°¢&WGW&âæ–ÖUW6W%7FGW3£¤G&÷VC°¢Ð¢&WGW&âæ–ÖUW6W%7FGW3£¥ÆææVC°§Ð ¤æ–ÖUW6W%7FGW2æ–ÖUG&6¶W%6W'f–6S£¤æW‡EW6W%7FGW2„æ–ÖUW6W%7FGW27FGW2§°¢7v—F6‚‡7FGW2¢°¢66Ræ–ÖUW6W%7FGW3£¥vF6†–æs ¢&WGW&âæ–ÖUW6W%7FGW3£¥ÆææVC°¢66Ræ–ÖUW6W%7FGW3£¥ÆææVC ¢&WGW&âæ–ÖUW6W%7FGW3£¤6ö×ÆWFVC°¢66Ræ–ÖUW6W%7FGW3£¤6ö×ÆWFVC ¢&WGW&âæ–ÖUW6W%7FGW3£¤öä†öÆC°¢66Ræ–ÖUW6W%7FGW3£¤öä†öÆC ¢&WGW&âæ–ÖUW6W%7FGW3£¤G&÷VC°¢66Ræ–ÖUW6W%7FGW3£¤G&÷VC ¢&WGW&âæ–ÖUW6W%7FGW3£¥vF6†–æs°¢Ð¢&WGW&âæ–ÖUW6W%7FGW3£¥ÆææVC°§Ð §7FC£§w7G&–æræ–ÖUG&6¶W%6W'f–6S£¤FFTÆ&VÂ†6öç7Bæ–ÖTFFRbFFR§°¢–b†FFRç–V"ÓÒ¢°¢&WGW&âÂ%&VÆV6RFFRVæ¶æ÷vâ#°¢Ð ¢7FC£§v÷7G&–æw7G&VÒ÷WGWC°¢÷WGWBÃÂFFRç–V#°¢–b†FFRæÖöçF‚â¢°¢÷WGWBÃÂÂ"Ò"ÃÂ7FC£§6WGrƒ"’ÃÂ7FC£§6WFf–ÆÂ„Âsr’ÃÂFFRæÖöçFƒ°¢Ð¢–b†FFRæF’â¢°¢÷WGWBÃÂÂ"Ò"ÃÂ7FC£§6WGrƒ"’ÃÂ7FC£§6WFf–ÆÂ„Âsr’ÃÂFFRæF“°¢Ð¢&WGW&â÷WGWBç7G"‚“°§Ð §7FC£§w7G&–æræ–ÖUG&6¶W%6W'f–6S£¤—&–ætFFTÆ&VÂ†ÆöærÆöærVæ—…F–ÖW7F×§°¢–b‡Væ—…F–ÖW7F×ÃÒ¢°¢&WGW&âÂ%&VÆV6RFFRVæ¶æ÷vâ#°¢Ð ¢6öç7B7FC£§F–ÖU÷BF–ÖUfÇVRÒ7FF–5ö67CÇ7FC£§F–ÖU÷Câ‡Væ—…F–ÖW7F×“°¢7FC£§FÒÆö6Â·Ó°¢Æö6ÇF–ÖU÷2‚fÆö6ÂÂgF–ÖUfÇVR“° ¢v6†%÷B'VffW%³“eÒ·Ó°¢v76gF–ÖR†'VffW"Â7FC£§6—¦R†'VffW"’ÂÂ"VÂV"VBBT“¢TÒW"ÂfÆö6Â“°¢&WGW&â'VffW#°§Ð §7FC£§w7G&–æræ–ÖUG&6¶W%6W'f–6S£¤6÷VçFF÷väÆ&VÂ†ÆöærÆöærVæ—…F–ÖW7F×§°¢–b‡Væ—…F–ÖW7F×ÃÒ¢°¢&WGW&âÂ%&VÆV6RFFRVæ¶æ÷vâ#°¢Ð ¢6öç7BWFòæ÷rÒ7FC£¦6‡&öæó£§7—7FVÕö6Æö6³£¦æ÷r‚“°¢6öç7BWFòF&vWBÒ7FC£¦6‡&öæó£§7—7FVÕö6Æö6³£¦g&öÕ÷F–ÖU÷B‡7FF–5ö67CÇ7FC£§F–ÖU÷Câ‡Væ—…F–ÖW7F×’“°¢–b‡F&vWBÃÒæ÷r¢°¢&WGW&âÂ&—'26ööâ#°¢Ð ¢6öç7BWFò6V6öæG2Ò7FC£¦6‡&öæó£¦GW&F–öåö67CÇ7FC£¦6‡&öæó£§6V6öæG3â‡F&vWBÒæ÷r’æ6÷VçB‚“°¢6öç7BÆöærÆöærF—2Ò6V6öæG2òƒcC°¢6öç7BÆöærÆöær†÷W'2Ò‡6V6öæG2RƒcC’ò3c°¢–b†F—2â¢°¢&WGW&âÂ&—'2–â"²7FC£§Fõ÷w7G&–ær†F—2’²†F—2ÓÒòÂ"F’"¢Â"F—2"“°¢Ð¢–b††÷W'2â¢°¢&WGW&âÂ&—'2–â"²7FC£§Fõ÷w7G&–ær††÷W'2’²††÷W'2ÓÒòÂ"†÷W""¢Â"†÷W'2"“°¢Ð¢6öç7BÆöærÆöærÖ–çWFW2Ò7FC£¦ÖƒÆÆöærÆöæsâƒÂ‡6V6öæG2R3c’òc“°¢&WGW&âÂ&—'2–â"²7FC£§Fõ÷w7G&–ær†Ö–çWFW2’²†Ö–çWFW2ÓÒòÂ"Ö–çWFR"¢Â"Ö–çWFW2"“°§Ð §7FC£§w7G&–æræ–ÖUG&6¶W%6W'f–6S£¤—6ôæ÷uWF2‚§°¢6öç7B7FC£§F–ÖU÷Bæ÷rÒ7FC£§F–ÖR†çVÆÇG"“°¢7FC£§FÒWF2·Ó°¢v×F–ÖU÷2‚gWF2Âfæ÷r“° ¢v6†%÷B'VffW%³3%Ò·Ó°¢v76gF–ÖR†'VffW"Â7FC£§6—¦R†'VffW"’ÂÂ"U’ÒVÒÒVEBTƒ¢TÓ¢U5¢"ÂgWF2“°¢&WGW&â'VffW#°§Ð