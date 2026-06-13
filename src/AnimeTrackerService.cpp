#include "AnimeTrackerService.h"

#include <windows.h>
#include <winhttp.h>

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
#include <sstream>
#include <system_error>

namespace
{
constexpr DWORD kAniListTimeoutMs = 12000;
constexpr size_t kMaxAniListResponseBytes = 4 * 1024 * 1024;

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
                unsigned int codepoint = 0;
                if (!ParseHexCodepoint(codepoint))
                {
                    return false;
                }
                AppendUtf8Codepoint(output, codepoint);
                break;
            }
            default:
                return false;
            }
        }

        return false;
    }

    bool ParseHexCodepoint(unsigned int& codepoint)
    {
        if (position_ + 4 > text_.size())
        {
            return false;
        }

        codepoint = 0;
        for (int index = 0; index < 4; ++index)
        {
            const char ch = text_[position_++];
            codepoint <<= 4;
            if (ch >= '0' && ch <= '9')
            {
                codepoint += static_cast<unsigned int>(ch - '0');
            }
            else if (ch >= 'a' && ch <= 'f')
            {
                codepoint += static_cast<unsigned int>(ch - 'a' + 10);
            }
            else if (ch >= 'A' && ch <= 'F')
            {
                codepoint += static_cast<unsigned int>(ch - 'A' + 10);
            }
            else
            {
                return false;
            }
        }
        return true;
    }

    bool ParseArray(Value& value)
    {
        if (!Consume('['))
        {
            return false;
        }

        value = {};
        value.type = Type::Array;
        SkipWhitespace();
        if (Consume(']'))
        {
            return true;
        }

        for (;;)
        {
            Value item;
            if (!ParseValue(item))
            {
                return false;
            }
            value.arrayValue.push_back(std::move(item));
            SkipWhitespace();
            if (Consume(']'))
            {
                return true;
            }
            if (!Consume(','))
            {
                return false;
            }
            SkipWhitespace();
        }
    }

    bool ParseObject(Value& value)
    {
        if (!Consume('{'))
        {
            return false;
        }

        value = {};
        value.type = Type::Object;
        SkipWhitespace();
        if (Consume('}'))
        {
            return true;
        }

        for (;;)
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
            Value member;
            if (!ParseValue(member))
            {
                return false;
            }
            value.objectValue[key] = std::move(member);
            SkipWhitespace();
            if (Consume('}'))
            {
                return true;
            }
            if (!Consume(','))
            {
                return false;
            }
            SkipWhitespace();
        }
    }

    bool ParseNumber(Value& value)
    {
        const size_t start = position_;
        if (Peek() == '-')
        {
            ++position_;
        }
        while (!AtEnd() && isdigit(static_cast<unsigned char>(Peek())) != 0)
        {
            ++position_;
        }
        if (!AtEnd() && Peek() == '.')
        {
            ++position_;
            while (!AtEnd() && isdigit(static_cast<unsigned char>(Peek())) != 0)
            {
                ++position_;
            }
        }
        if (!AtEnd() && (Peek() == 'e' || Peek() == 'E'))
        {
            ++position_;
            if (!AtEnd() && (Peek() == '+' || Peek() == '-'))
            {
                ++position_;
            }
            while (!AtEnd() && isdigit(static_cast<unsigned char>(Peek())) != 0)
            {
                ++position_;
            }
        }

        if (start == position_)
        {
            return false;
        }

        value = {};
        value.type = Type::Number;
        value.numberValue = std::strtod(text_.c_str() + start, nullptr);
        return true;
    }

    char Peek() const
    {
        return AtEnd() ? '\0' : text_[position_];
    }

    bool AtEnd() const
    {
        return position_ >= text_.size();
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

    void SkipWhitespace()
    {
        while (!AtEnd() && isspace(static_cast<unsigned char>(Peek())) != 0)
        {
            ++position_;
        }
    }

    const std::string& text_;
    size_t position_ = 0;
};

const Value* Member(const Value& value, const char* name)
{
    if (value.type != Type::Object)
    {
        return nullptr;
    }
    const auto found = value.objectValue.find(name);
    return found == value.objectValue.end() ? nullptr : &found->second;
}

const Value* At(const Value& value, const char* name)
{
    return Member(value, name);
}

std::wstring String(const Value* value)
{
    if (!value || value->type == Type::Null)
    {
        return {};
    }
    if (value->type == Type::String)
    {
        return Utf8ToWide(value->stringValue);
    }
    return {};
}

int Int(const Value* value)
{
    if (!value || value->type == Type::Null)
    {
        return 0;
    }
    if (value->type == Type::Number)
    {
        return static_cast<int>(value->numberValue);
    }
    return 0;
}

long long Int64(const Value* value)
{
    if (!value || value->type == Type::Null)
    {
        return 0;
    }
    if (value->type == Type::Number)
    {
        return static_cast<long long>(value->numberValue);
    }
    return 0;
}

bool Bool(const Value* value)
{
    return value && value->type == Type::Bool && value->boolValue;
}
}

std::string JsonEscape(const std::wstring& value)
{
    const std::string utf8 = WideToUtf8(value);
    std::string escaped;
    escaped.reserve(utf8.size() + 8);
    escaped.push_back('"');
    for (unsigned char ch : utf8)
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
            if (ch < 0x20)
            {
                std::ostringstream stream;
                stream << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch);
                escaped += stream.str();
            }
            else
            {
                escaped.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    escaped.push_back('"');
    return escaped;
}

std::wstring ValueOrFallback(const std::wstring& preferred, const std::wstring& fallback)
{
    return preferred.empty() ? fallback : preferred;
}

AnimeDate ParseDate(const Json::Value* value)
{
    AnimeDate date;
    if (!value)
    {
        return date;
    }
    date.year = Json::Int(Json::At(*value, "year"));
    date.month = Json::Int(Json::At(*value, "month"));
    date.day = Json::Int(Json::At(*value, "day"));
    return date;
}

AiringInfo ParseAiringInfo(const Json::Value* value)
{
    AiringInfo info;
    if (!value || value->type == Json::Type::Null)
    {
        return info;
    }
    info.hasValue = true;
    info.episode = Json::Int(Json::At(*value, "episode"));
    info.airingAt = Json::Int64(Json::At(*value, "airingAt"));
    info.timeUntilAiring = Json::Int(Json::At(*value, "timeUntilAiring"));
    return info;
}

AnimeRelation ParseRelation(const Json::Value& edgeValue)
{
    AnimeRelation relation;
    relation.relationType = Json::String(Json::At(edgeValue, "relationType"));
    const Json::Value* node = Json::At(edgeValue, "node");
    if (!node)
    {
        return relation;
    }

    relation.anilistId = Json::Int(Json::At(*node, "id"));
    const Json::Value* title = Json::At(*node, "title");
    if (title)
    {
        relation.title = ValueOrFallback(
            Json::String(Json::At(*title, "userPreferred")),
            ValueOrFallback(Json::String(Json::At(*title, "english")), Json::String(Json::At(*title, "romaji"))));
    }
    relation.format = Json::String(Json::At(*node, "format"));
    relation.status = Json::String(Json::At(*node, "status"));
    relation.episodes = Json::Int(Json::At(*node, "episodes"));
    relation.season = Json::String(Json::At(*node, "season"));
    relation.seasonYear = Json::Int(Json::At(*node, "seasonYear"));
    relation.startDate = ParseDate(Json::At(*node, "startDate"));
    const Json::Value* cover = Json::At(*node, "coverImage");
    if (cover)
    {
        relation.coverImageUrl = ValueOrFallback(Json::String(Json::At(*cover, "large")), Json::String(Json::At(*cover, "medium")));
    }
    relation.siteUrl = Json::String(Json::At(*node, "siteUrl"));
    relation.nextAiringEpisode = ParseAiringInfo(Json::At(*node, "nextAiringEpisode"));
    return relation;
}

AnimeSearchResult ParseMedia(const Json::Value& mediaValue)
{
    AnimeSearchResult result;
    result.anilistId = Json::Int(Json::At(mediaValue, "id"));
    result.idMal = Json::Int(Json::At(mediaValue, "idMal"));

    const Json::Value* title = Json::At(mediaValue, "title");
    if (title)
    {
        result.title = ValueOrFallback(
            Json::String(Json::At(*title, "userPreferred")),
            ValueOrFallback(Json::String(Json::At(*title, "english")), Json::String(Json::At(*title, "romaji"))));
        result.englishTitle = Json::String(Json::At(*title, "english"));
        result.nativeTitle = Json::String(Json::At(*title, "native"));
    }

    const Json::Value* cover = Json::At(mediaValue, "coverImage");
    if (cover)
    {
        result.coverImageUrl = ValueOrFallback(Json::String(Json::At(*cover, "large")), Json::String(Json::At(*cover, "medium")));
    }

    result.bannerImageUrl = Json::String(Json::At(mediaValue, "bannerImage"));
    result.description = TrimHtml(Json::String(Json::At(mediaValue, "description")));
    result.format = Json::String(Json::At(mediaValue, "format"));
    result.status = Json::String(Json::At(mediaValue, "status"));
    result.episodes = Json::Int(Json::At(mediaValue, "episodes"));
    result.duration = Json::Int(Json::At(mediaValue, "duration"));
    result.season = Json::String(Json::At(mediaValue, "season"));
    result.seasonYear = Json::Int(Json::At(mediaValue, "seasonYear"));
    result.startDate = ParseDate(Json::At(mediaValue, "startDate"));
    result.endDate = ParseDate(Json::At(mediaValue, "endDate"));
    result.averageScore = Json::Int(Json::At(mediaValue, "averageScore"));
    result.popularity = Json::Int(Json::At(mediaValue, "popularity"));
    result.siteUrl = Json::String(Json::At(mediaValue, "siteUrl"));
    result.nextAiringEpisode = ParseAiringInfo(Json::At(mediaValue, "nextAiringEpisode"));

    const Json::Value* genres = Json::At(mediaValue, "genres");
    if (genres && genres->type == Json::Type::Array)
    {
        for (const Json::Value& genre : genres->arrayValue)
        {
            if (genre.type == Json::Type::String)
            {
                result.genres.push_back(Utf8ToWide(genre.stringValue));
            }
        }
    }

    const Json::Value* relations = Json::At(mediaValue, "relations");
    const Json::Value* edges = relations ? Json::At(*relations, "edges") : nullptr;
    if (edges && edges->type == Json::Type::Array)
    {
        for (const Json::Value& edge : edges->arrayValue)
        {
            AnimeRelation relation = ParseRelation(edge);
            if (relation.anilistId != 0)
            {
                result.relations.push_back(std::move(relation));
            }
        }
    }

    return result;
}

std::string AnimeQuery()
{
    return R"(query ($search: String, $id: Int, $page: Int, $perPage: Int) {
  Page(page: $page, perPage: $perPage) {
    pageInfo { currentPage hasNextPage }
    media(search: $search, id: $id, type: ANIME, sort: POPULARITY_DESC) {
      id
      idMal
      title { romaji english native userPreferred }
      coverImage { large medium color }
      bannerImage
      description(asHtml: false)
      format
      status
      episodes
      duration
      season
      seasonYear
      startDate { year month day }
      endDate { year month day }
      genres
      averageScore
      popularity
      siteUrl
      nextAiringEpisode { airingAt timeUntilAiring episode }
      relations {
        edges {
          relationType
          node {
            id
            title { userPreferred english romaji }
            format
            status
            episodes
            season
            seasonYear
            startDate { year month day }
            coverImage { large medium }
            siteUrl
            nextAiringEpisode { airingAt timeUntilAiring episode }
          }
        }
      }
    }
  }
})";
}

std::string BuildSearchBody(const std::wstring& searchText, int page, int perPage)
{
    std::ostringstream body;
    body << "{\"query\":" << JsonEscape(Utf8ToWide(AnimeQuery())) << ",\"variables\":{"
        << "\"search\":" << JsonEscape(searchText) << ","
        << "\"page\":" << page << ","
        << "\"perPage\":" << perPage
        << "}}";
    return body.str();
}

std::string BuildIdBody(int anilistId)
{
    std::ostringstream body;
    body << "{\"query\":" << JsonEscape(Utf8ToWide(AnimeQuery())) << ",\"variables\":{"
        << "\"id\":" << anilistId << ","
        << "\"page\":1,"
        << "\"perPage\":1"
        << "}}";
    return body.str();
}

std::string SerializeAiringInfo(const AiringInfo& info, const char* indent)
{
    std::ostringstream output;
    output << indent << "\"nextAiringEpisode\": " << (info.hasValue ? info.episode : 0) << ",\n"
        << indent << "\"nextAiringAt\": " << (info.hasValue ? info.airingAt : 0);
    return output.str();
}

void WriteJsonStringField(std::ostringstream& output, const char* indent, const char* key, const std::wstring& value, bool comma = true)
{
    output << indent << "\"" << key << "\": " << JsonEscape(value);
    if (comma)
    {
        output << ",";
    }
    output << "\n";
}

void WriteJsonIntField(std::ostringstream& output, const char* indent, const char* key, int value, bool comma = true)
{
    output << indent << "\"" << key << "\": " << value;
    if (comma)
    {
        output << ",";
    }
    output << "\n";
}

void WriteRelation(std::ostringstream& output, const AnimeRelation& relation, const char* indent, bool comma)
{
    output << indent << "{\n";
    WriteJsonIntField(output, "        ", "anilistId", relation.anilistId);
    WriteJsonStringField(output, "        ", "relationType", relation.relationType);
    WriteJsonStringField(output, "        ", "title", relation.title);
    WriteJsonStringField(output, "        ", "format", relation.format);
    WriteJsonStringField(output, "        ", "status", relation.status);
    WriteJsonIntField(output, "        ", "episodes", relation.episodes);
    WriteJsonStringField(output, "        ", "season", relation.season);
    WriteJsonIntField(output, "        ", "seasonYear", relation.seasonYear);
    WriteJsonIntField(output, "        ", "startYear", relation.startDate.year);
    WriteJsonIntField(output, "        ", "startMonth", relation.startDate.month);
    WriteJsonIntField(output, "        ", "startDay", relation.startDate.day);
    WriteJsonStringField(output, "        ", "coverImage", relation.coverImageUrl);
    WriteJsonStringField(output, "        ", "siteUrl", relation.siteUrl);
    output << SerializeAiringInfo(relation.nextAiringEpisode, "        ") << "\n";
    output << indent << "}";
    if (comma)
    {
        output << ",";
    }
    output << "\n";
}

AnimeRelation RelationFromJson(const Json::Value& value)
{
    AnimeRelation relation;
    relation.anilistId = Json::Int(Json::At(value, "anilistId"));
    relation.relationType = Json::String(Json::At(value, "relationType"));
    relation.title = Json::String(Json::At(value, "title"));
    relation.format = Json::String(Json::At(value, "format"));
    relation.status = Json::String(Json::At(value, "status"));
    relation.episodes = Json::Int(Json::At(value, "episodes"));
    relation.season = Json::String(Json::At(value, "season"));
    relation.seasonYear = Json::Int(Json::At(value, "seasonYear"));
    relation.startDate.year = Json::Int(Json::At(value, "startYear"));
    relation.startDate.month = Json::Int(Json::At(value, "startMonth"));
    relation.startDate.day = Json::Int(Json::At(value, "startDay"));
    relation.coverImageUrl = Json::String(Json::At(value, "coverImage"));
    relation.siteUrl = Json::String(Json::At(value, "siteUrl"));
    relation.nextAiringEpisode.episode = Json::Int(Json::At(value, "nextAiringEpisode"));
    relation.nextAiringEpisode.airingAt = Json::Int64(Json::At(value, "nextAiringAt"));
    relation.nextAiringEpisode.hasValue = relation.nextAiringEpisode.airingAt > 0 || relation.nextAiringEpisode.episode > 0;
    return relation;
}

AnimeEntry EntryFromJson(const Json::Value& value)
{
    AnimeEntry entry;
    entry.anilistId = Json::Int(Json::At(value, "anilistId"));
    entry.idMal = Json::Int(Json::At(value, "idMal"));
    entry.title = Json::String(Json::At(value, "title"));
    entry.coverImageUrl = Json::String(Json::At(value, "coverImage"));
    entry.format = Json::String(Json::At(value, "format"));
    entry.airingStatus = Json::String(Json::At(value, "airingStatus"));
    entry.userStatus = AnimeTrackerService::UserStatusFromString(Json::String(Json::At(value, "userStatus")));
    entry.currentEpisode = Json::Int(Json::At(value, "currentEpisode"));
    entry.totalEpisodes = Json::Int(Json::At(value, "totalEpisodes"));
    entry.nextAiringEpisode.episode = Json::Int(Json::At(value, "nextAiringEpisode"));
    entry.nextAiringEpisode.airingAt = Json::Int64(Json::At(value, "nextAiringAt"));
    entry.nextAiringEpisode.hasValue = entry.nextAiringEpisode.airingAt > 0 || entry.nextAiringEpisode.episode > 0;
    entry.notes = Json::String(Json::At(value, "notes"));
    entry.favorite = Json::Bool(Json::At(value, "favorite"));
    entry.lastRefreshed = Json::String(Json::At(value, "lastRefreshed"));
    entry.siteUrl = Json::String(Json::At(value, "siteUrl"));

    const Json::Value* relations = Json::At(value, "relations");
    if (relations && relations->type == Json::Type::Array)
    {
        for (const Json::Value& relationValue : relations->arrayValue)
        {
            AnimeRelation relation = RelationFromJson(relationValue);
            if (relation.anilistId != 0)
            {
                entry.relations.push_back(std::move(relation));
            }
        }
    }

    return entry;
}
}

AnimeSearchResponse AniListApiClient::SearchAnime(const std::wstring& searchText, int page, int perPage, std::wstring& errorMessage) const
{
    AnimeSearchResponse searchResponse;
    std::string responseBody;
    if (!ExecuteGraphQl(BuildSearchBody(searchText, page, perPage), responseBody, errorMessage))
    {
        return searchResponse;
    }

    Json::Value root;
    Json::Parser parser(responseBody);
    if (!parser.Parse(root))
    {
        errorMessage = L"Could not read the AniList response.";
        return searchResponse;
    }

    if (Json::At(root, "errors"))
    {
        errorMessage = L"AniList returned an API error.";
        return searchResponse;
    }

    const Json::Value* data = Json::At(root, "data");
    const Json::Value* pageValue = data ? Json::At(*data, "Page") : nullptr;
    if (!pageValue)
    {
        errorMessage = L"AniList returned an unexpected response.";
        return searchResponse;
    }

    const Json::Value* pageInfo = Json::At(*pageValue, "pageInfo");
    if (pageInfo)
    {
        searchResponse.currentPage = Json::Int(Json::At(*pageInfo, "currentPage"));
        searchResponse.hasNextPage = Json::Bool(Json::At(*pageInfo, "hasNextPage"));
    }

    const Json::Value* media = Json::At(*pageValue, "media");
    if (media && media->type == Json::Type::Array)
    {
        for (const Json::Value& item : media->arrayValue)
        {
            AnimeSearchResult result = ParseMedia(item);
            if (result.anilistId != 0)
            {
                searchResponse.results.push_back(std::move(result));
            }
        }
    }

    return searchResponse;
}

std::optional<AnimeSearchResult> AniListApiClient::FetchAnimeById(int anilistId, std::wstring& errorMessage) const
{
    std::string responseBody;
    if (!ExecuteGraphQl(BuildIdBody(anilistId), responseBody, errorMessage))
    {
        return std::nullopt;
    }

    Json::Value root;
    Json::Parser parser(responseBody);
    if (!parser.Parse(root))
    {
        errorMessage = L"Could not read the AniList response.";
        return std::nullopt;
    }

    const Json::Value* data = Json::At(root, "data");
    const Json::Value* pageValue = data ? Json::At(*data, "Page") : nullptr;
    const Json::Value* media = pageValue ? Json::At(*pageValue, "media") : nullptr;
    if (!media || media->type != Json::Type::Array || media->arrayValue.empty())
    {
        errorMessage = L"AniList did not return that anime.";
        return std::nullopt;
    }

    return ParseMedia(media->arrayValue.front());
}

bool AniListApiClient::ExecuteGraphQl(const std::string& requestBody, std::string& responseBody, std::wstring& errorMessage) const
{
    WinHttpHandle session(WinHttpOpen(
        L"RexToolkitAnimeTracker/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if (!session)
    {
        errorMessage = L"Could not reach AniList. Check your internet connection and try again.";
        return false;
    }

    WinHttpSetTimeouts(session, kAniListTimeoutMs, kAniListTimeoutMs, kAniListTimeoutMs, kAniListTimeoutMs);

    WinHttpHandle connection(WinHttpConnect(session, L"graphql.anilist.co", INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection)
    {
        errorMessage = L"Could not reach AniList. Check your internet connection and try again.";
        return false;
    }

    WinHttpHandle request(WinHttpOpenRequest(
        connection,
        L"POST",
        L"/",
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE));
    if (!request)
    {
        errorMessage = L"Could not reach AniList. Check your internet connection and try again.";
        return false;
    }

    const wchar_t headers[] = L"Content-Type: application/json\r\nAccept: application/json\r\n";
    if (!WinHttpSendRequest(
            request,
            headers,
            static_cast<DWORD>(-1),
            const_cast<char*>(requestBody.data()),
            static_cast<DWORD>(requestBody.size()),
            static_cast<DWORD>(requestBody.size()),
            0) ||
        !WinHttpReceiveResponse(request, nullptr))
    {
        errorMessage = L"Could not reach AniList. Check your internet connection and try again.";
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
        errorMessage = L"Could not reach AniList. Check your internet connection and try again.";
        return false;
    }

    responseBody.clear();
    for (;;)
    {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available))
        {
            errorMessage = L"Could not read the AniList response.";
            return false;
        }
        if (available == 0)
        {
            break;
        }
        if (responseBody.size() + available > kMaxAniListResponseBytes)
        {
            errorMessage = L"AniList returned too much data. Try a narrower search.";
            return false;
        }

        std::string buffer(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), available, &read))
        {
            errorMessage = L"Could not read the AniList response.";
            return false;
        }
        buffer.resize(read);
        responseBody += buffer;
    }

    return true;
}

AnimeWatchList AnimeListStorage::Load(const std::filesystem::path& path, std::wstring& warning) const
{
    AnimeWatchList list;
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return list;
    }

    std::string json((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (json.size() >= 3 &&
        static_cast<unsigned char>(json[0]) == 0xEF &&
        static_cast<unsigned char>(json[1]) == 0xBB &&
        static_cast<unsigned char>(json[2]) == 0xBF)
    {
        json.erase(0, 3);
    }

    Json::Value root;
    Json::Parser parser(json);
    if (!parser.Parse(root))
    {
        warning = L"Anime Tracker could not read the saved list, so it started empty.";
        return list;
    }

    list.version = std::max(1, Json::Int(Json::At(root, "version")));
    const Json::Value* anime = Json::At(root, "anime");
    if (anime && anime->type == Json::Type::Array)
    {
        for (const Json::Value& entryValue : anime->arrayValue)
        {
            AnimeEntry entry = EntryFromJson(entryValue);
            if (entry.anilistId != 0)
            {
                list.anime.push_back(std::move(entry));
            }
        }
    }
    return list;
}

bool AnimeListStorage::Save(const std::filesystem::path& path, const AnimeWatchList& list, std::wstring& errorMessage) const
{
    std::error_code fileError;
    std::filesystem::create_directories(path.parent_path(), fileError);
    if (fileError)
    {
        errorMessage = L"Could not save Anime Tracker data. The app data folder could not be created.";
        return false;
    }

    if (std::filesystem::exists(path, fileError))
    {
        std::filesystem::copy_file(path, path.wstring() + L".bak", std::filesystem::copy_options::overwrite_existing, fileError);
        fileError.clear();
    }

    std::ostringstream output;
    output << "{\n  \"version\": 1,\n  \"anime\": [\n";
    for (size_t index = 0; index < list.anime.size(); ++index)
    {
        const AnimeEntry& entry = list.anime[index];
        output << "    {\n";
        WriteJsonIntField(output, "      ", "anilistId", entry.anilistId);
        WriteJsonIntField(output, "      ", "idMal", entry.idMal);
        WriteJsonStringField(output, "      ", "title", entry.title);
        WriteJsonStringField(output, "      ", "coverImage", entry.coverImageUrl);
        WriteJsonStringField(output, "      ", "format", entry.format);
        WriteJsonStringField(output, "      ", "airingStatus", entry.airingStatus);
        WriteJsonStringField(output, "      ", "userStatus", AnimeTrackerService::UserStatusLabel(entry.userStatus));
        WriteJsonIntField(output, "      ", "currentEpisode", entry.currentEpisode);
        WriteJsonIntField(output, "      ", "totalEpisodes", entry.totalEpisodes);
        output << SerializeAiringInfo(entry.nextAiringEpisode, "      ") << ",\n";
        output << "      \"relations\": [\n";
        for (size_t relationIndex = 0; relationIndex < entry.relations.size(); ++relationIndex)
        {
            WriteRelation(output, entry.relations[relationIndex], "        ", relationIndex + 1 < entry.relations.size());
        }
        output << "      ],\n";
        WriteJsonStringField(output, "      ", "notes", entry.notes);
        output << "      \"favorite\": " << (entry.favorite ? "true" : "false") << ",\n";
        WriteJsonStringField(output, "      ", "lastRefreshed", entry.lastRefreshed);
        WriteJsonStringField(output, "      ", "siteUrl", entry.siteUrl, false);
        output << "    }";
        if (index + 1 < list.anime.size())
        {
            output << ",";
        }
        output << "\n";
    }
    output << "  ]\n}\n";

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
    {
        errorMessage = L"Could not save Anime Tracker data. Permission was denied.";
        return false;
    }
    file << output.str();
    if (!file)
    {
        errorMessage = L"Could not save Anime Tracker data.";
        return false;
    }
    return true;
}

std::vector<AnimeRelation> AnimeRelationTracker::UpcomingSequels(const AnimeWatchList& list)
{
    std::vector<AnimeRelation> relations;
    for (const AnimeEntry& entry : list.anime)
    {
        for (const AnimeRelation& relation : entry.relations)
        {
            if (IsUpcomingSequelCandidate(relation) &&
                std::none_of(relations.begin(), relations.end(), [&relation](const AnimeRelation& existing)
                {
                    return existing.anilistId == relation.anilistId;
                }) &&
                !AnimeTrackerService::ContainsAnime(list, relation.anilistId))
            {
                relations.push_back(relation);
            }
        }
    }

    std::sort(relations.begin(), relations.end(), [](const AnimeRelation& left, const AnimeRelation& right)
    {
        const long long leftAiring = left.nextAiringEpisode.hasValue ? left.nextAiringEpisode.airingAt : 0;
        const long long rightAiring = right.nextAiringEpisode.hasValue ? right.nextAiringEpisode.airingAt : 0;
        if (leftAiring != rightAiring)
        {
            return leftAiring != 0 && (rightAiring == 0 || leftAiring < rightAiring);
        }
        if (left.seasonYear != right.seasonYear)
        {
            return left.seasonYear < right.seasonYear;
        }
        return left.title < right.title;
    });

    return relations;
}

bool AnimeRelationTracker::IsUpcomingSequelCandidate(const AnimeRelation& relation)
{
    if (relation.relationType != L"SEQUEL")
    {
        return false;
    }
    if (relation.status == L"NOT_YET_RELEASED" || relation.status == L"RELEASING" || relation.nextAiringEpisode.hasValue)
    {
        return true;
    }

    SYSTEMTIME localTime {};
    GetLocalTime(&localTime);
    return relation.startDate.year >= static_cast<int>(localTime.wYear) ||
        relation.seasonYear >= static_cast<int>(localTime.wYear);
}

AnimeTrackerService::AnimeTrackerService() = default;

AnimeWatchList AnimeTrackerService::LoadWatchList(const std::filesystem::path& path, std::wstring& warning) const
{
    return storage_.Load(path, warning);
}

bool AnimeTrackerService::SaveWatchList(const std::filesystem::path& path, const AnimeWatchList& list, std::wstring& errorMessage) const
{
    return storage_.Save(path, list, errorMessage);
}

AnimeSearchResponse AnimeTrackerService::SearchAnime(const std::wstring& searchText, int page, int perPage, std::wstring& errorMessage) const
{
    return apiClient_.SearchAnime(searchText, page, perPage, errorMessage);
}

std::optional<AnimeSearchResult> AnimeTrackerService::RefreshAnime(int anilistId, std::wstring& errorMessage) const
{
    return apiClient_.FetchAnimeById(anilistId, errorMessage);
}

AnimeEntry AnimeTrackerService::EntryFromSearchResult(const AnimeSearchResult& result)
{
    AnimeEntry entry;
    entry.anilistId = result.anilistId;
    entry.idMal = result.idMal;
    entry.title = result.title;
    entry.coverImageUrl = result.coverImageUrl;
    entry.format = result.format;
    entry.airingStatus = result.status;
    entry.userStatus = AnimeUserStatus::Planned;
    entry.currentEpisode = 0;
    entry.totalEpisodes = result.episodes;
    entry.nextAiringEpisode = result.nextAiringEpisode;
    entry.relations = result.relations;
    entry.lastRefreshed = IsoNowUtc();
    entry.siteUrl = result.siteUrl;
    return entry;
}

void AnimeTrackerService::ApplyMetadata(AnimeEntry& entry, const AnimeSearchResult& result)
{
    entry.idMal = result.idMal;
    entry.title = result.title;
    entry.coverImageUrl = result.coverImageUrl;
    entry.format = result.format;
    entry.airingStatus = result.status;
    entry.totalEpisodes = result.episodes;
    entry.nextAiringEpisode = result.nextAiringEpisode;
    entry.relations = result.relations;
    entry.siteUrl = result.siteUrl;
    entry.lastRefreshed = IsoNowUtc();
    if (entry.totalEpisodes > 0 && entry.currentEpisode > entry.totalEpisodes)
    {
        entry.currentEpisode = entry.totalEpisodes;
    }
}

bool AnimeTrackerService::ContainsAnime(const AnimeWatchList& list, int anilistId)
{
    return std::any_of(list.anime.begin(), list.anime.end(), [anilistId](const AnimeEntry& entry)
    {
        return entry.anilistId == anilistId;
    });
}

std::wstring AnimeTrackerService::UserStatusLabel(AnimeUserStatus status)
{
    switch (status)
    {
    case AnimeUserStatus::Watching:
        return L"Watching";
    case AnimeUserStatus::Planned:
        return L"Planned";
    case AnimeUserStatus::Completed:
        return L"Completed";
    case AnimeUserStatus::OnHold:
        return L"On Hold";
    case AnimeUserStatus::Dropped:
        return L"Dropped";
    }
    return L"Planned";
}

AnimeUserStatus AnimeTrackerService::UserStatusFromString(const std::wstring& text)
{
    if (text == L"Watching")
    {
        return AnimeUserStatus::Watching;
    }
    if (text == L"Completed")
    {
        return AnimeUserStatus::Completed;
    }
    if (text == L"On Hold")
    {
        return AnimeUserStatus::OnHold;
    }
    if (text == L"Dropped")
    {
        return AnimeUserStatus::Dropped;
    }
    return AnimeUserStatus::Planned;
}

AnimeUserStatus AnimeTrackerService::NextUserStatus(AnimeUserStatus status)
{
    switch (status)
    {
    case AnimeUserStatus::Watching:
        return AnimeUserStatus::Planned;
    case AnimeUserStatus::Planned:
        return AnimeUserStatus::Completed;
    case AnimeUserStatus::Completed:
        return AnimeUserStatus::OnHold;
    case AnimeUserStatus::OnHold:
        return AnimeUserStatus::Dropped;
    case AnimeUserStatus::Dropped:
        return AnimeUserStatus::Watching;
    }
    return AnimeUserStatus::Planned;
}

std::wstring AnimeTrackerService::DateLabel(const AnimeDate& date)
{
    if (date.year == 0)
    {
        return L"Release date unknown";
    }

    std::wostringstream output;
    output << date.year;
    if (date.month > 0)
    {
        output << L"-" << std::setw(2) << std::setfill(L'0') << date.month;
    }
    if (date.day > 0)
    {
        output << L"-" << std::setw(2) << std::setfill(L'0') << date.day;
    }
    return output.str();
}

std::wstring AnimeTrackerService::AiringDateLabel(long long unixTimestamp)
{
    if (unixTimestamp <= 0)
    {
        return L"Release date unknown";
    }

    const std::time_t timeValue = static_cast<std::time_t>(unixTimestamp);
    std::tm local {};
    localtime_s(&local, &timeValue);

    wchar_t buffer[96] {};
    wcsftime(buffer, std::size(buffer), L"%a, %b %d at %I:%M %p", &local);
    return buffer;
}

std::wstring AnimeTrackerService::CountdownLabel(long long unixTimestamp)
{
    if (unixTimestamp <= 0)
    {
        return L"Release date unknown";
    }

    const auto now = std::chrono::system_clock::now();
    const auto target = std::chrono::system_clock::from_time_t(static_cast<std::time_t>(unixTimestamp));
    if (target <= now)
    {
        return L"airs soon";
    }

    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(target - now).count();
    const long long days = seconds / 86400;
    const long long hours = (seconds % 86400) / 3600;
    if (days > 0)
    {
        return L"airs in " + std::to_wstring(days) + (days == 1 ? L" day" : L" days");
    }
    if (hours > 0)
    {
        return L"airs in " + std::to_wstring(hours) + (hours == 1 ? L" hour" : L" hours");
    }
    const long long minutes = std::max<long long>(1, (seconds % 3600) / 60);
    return L"airs in " + std::to_wstring(minutes) + (minutes == 1 ? L" minute" : L" minutes");
}

std::wstring AnimeTrackerService::IsoNowUtc()
{
    const std::time_t now = std::time(nullptr);
    std::tm utc {};
    gmtime_s(&utc, &now);

    wchar_t buffer[32] {};
    wcsftime(buffer, std::size(buffer), L"%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}
