#include "CacheManager.h"
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

std::wstring FriendlyExternalToolMessage(
    const std::wstring& output,
    const std::wstring& fallbackMessage)
{
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
    while ((position = json.find(token, position)) != std::wstring::npos)
    {
        position = json.find(L':', position + token.size());
        if (position == std::wstring::npos)
        {
            return false;
        }

        ++position;
        while (position < json.size() && std::iswspace(json[position]))
        {
            ++position;
        }

        if (position >= json.size())
        {
            return false;
        }

        if (json.compare(position, 4, L"null") == 0)
        {
            position += 4;
            continue;
        }

        if (json[position] != L'"')
        {
            ++position;
            continue;
        }

        ++position;
        std::wstring value;
        while (position < json.size())
        {
            const wchar_t ch = json[position++];
            if (ch == L'"')
            {
                break;
            }
            if (ch == L'\\' && position < json.size())
            {
                value.push_back(json[position++]);
            }
            else
            {
                value.push_back(ch);
            }
        }

        value = ToLower(value);
        if (!value.empty() && value != L"none")
        {
            return true;
        }
    }

    return false;
}

int JsonMaxVideoHeight(const std::wstring& json)
{
    int maxHeight = 0;
    const std::wstring heightToken = L"\"height\"";
    size_t position = 0;
    while ((position = json.find(heightToken, position)) != std::wstring::npos)
    {
        const size_t objectStart = json.rfind(L'{', position);
        const size_t objectEnd = json.find(L'}', position);
        if (objectStart == std::wstring::npos || objectEnd == std::wstring::npos || objectStart >= objectEnd)
        {
            position += heightToken.size();
            continue;
        }

        const std::wstring objectText = json.substr(objectStart, objectEnd - objectStart + 1);
        const std::wstring vcodec = ToLower(JsonStringValue(objectText, L"vcodec").value_or(L""));
        if (vcodec.empty() || vcodec == L"none")
        {
            position += heightToken.size();
            continue;
        }

        if (const auto height = JsonNumberValue(objectText, L"height"))
        {
            maxHeight = std::max(maxHeight, static_cast<int>(*height + 0.5));
        }

        position = objectEnd + 1;
    }

    return maxHeight;
}

bool JsonContainsKey(const std::wstring& json, const std::wstring& key)
{
    return json.find(L"\"" + key + L"\"") != std::wstring::npos;
}

bool LooksLikeCollectionMetadata(const std::wstring& json)
{
    const std::wstring type = ToLower(JsonStringValue(json, L"_type").value_or(L""));
    if (type == L"playlist" || type == L"multi_video" || type == L"channel" || type == L"url")
    {
        return true;
    }

    const std::wstring extractor = ToLower(JsonStringValue(json, L"extractor_key").value_or(L""));
    if (extractor.find(L"playlist") != std::wstring::npos ||
        extractor.find(L"tab") != std::wstring::npos ||
        extractor.find(L"user") != std::wstring::npos ||
        extractor.find(L"set") != std::wstring::npos)
    {
        return true;
    }

    return JsonContainsKey(json, L"entries");
}

std::wstring DurationLabel(double secondsValue)
{
    const int totalSeconds = std::max(0, static_cast<int>(secondsValue + 0.5));
    const int hours = totalSeconds / 3600;
    const int minutes = (totalSeconds % 3600) / 60;
    const int seconds = totalSeconds % 60;

    wchar_t buffer[32] {};
    if (hours > 0)
    {
        swprintf_s(buffer, L"%d:%02d:%02d", hours, minutes, seconds);
    }
    else
    {
        swprintf_s(buffer, L"%d:%02d", minutes, seconds);
    }
    return buffer;
}

std::wstring FirstHttpLine(const std::wstring& text)
{
    size_t position = 0;
    while (position < text.size())
    {
        size_t end = text.find_first_of(L"\r\n", position);
        if (end == std::wstring::npos)
        {
            end = text.size();
        }

        std::wstring line = TrimCopy(text.substr(position, end - position));
        if (StartsWithHttp(line))
        {
            return line;
        }

        position = end + 1;
    }
    return {};
}

std::filesystem::path TemporaryPathWithExtension(const std::wstring& extension)
{
    std::filesystem::path directory = CacheManager::AudioAnalysisTemporaryRoot();
    std::error_code directoryError;
    std::filesystem::create_directories(directory, directoryError);
    if (directoryError) directory = std::filesystem::temp_directory_path();

    const std::wstring baseName =
        L"analysis-" +
        std::to_wstring(GetCurrentProcessId()) +
        L"_" +
        std::to_wstring(GetTickCount64());

    for (int index = 0; index < 100; ++index)
    {
        std::filesystem::path candidate = directory / (baseName + L"_" + std::to_wstring(index) + extension);
        if (!std::filesystem::exists(candidate))
        {
            return candidate;
        }
    }

    return directory / (baseName + extension);
}

std::filesystem::path TemporaryRawAudioPath()
{
    return TemporaryPathWithExtension(L".raw");
}

std::filesystem::path TemporaryWavAudioPath()
{
    return TemporaryPathWithExtension(L".wav");
}

std::filesystem::path TemporaryJsonPath()
{
    return TemporaryPathWithExtension(L".json");
}

std::vector<float> ReadPcm16Mono(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return {};
    }

    input.seekg(0, std::ios::end);
    const std::streamoff length = input.tellg();
    input.seekg(0, std::ios::beg);
    if (length <= 1)
    {
        return {};
    }

    std::vector<char> bytes(static_cast<size_t>(length));
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    const size_t bytesRead = static_cast<size_t>(input.gcount());
    const size_t sampleCount = bytesRead / 2;

    std::vector<float> samples;
    samples.reserve(sampleCount);
    for (size_t index = 0; index < sampleCount; ++index)
    {
        const auto low = static_cast<unsigned char>(bytes[index * 2]);
        const auto high = static_cast<unsigned char>(bytes[index * 2 + 1]);
        const auto value = static_cast<int16_t>(low | (high << 8));
        samples.push_back(static_cast<float>(value) / 32768.0f);
    }

    return samples;
}

std::wstring ReadTextFileWide(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return {};
    }

    input.seekg(0, std::ios::end);
    const std::streamoff length = input.tellg();
    input.seekg(0, std::ios::beg);
    if (length <= 0)
    {
        return {};
    }

    std::string bytes(static_cast<size_t>(length), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<size_t>(input.gcount()));
    return BytesToWide(bytes);
}

std::optional<std::wstring> JsonObjectValue(const std::wstring& json, const std::wstring& key)
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

    if (position >= json.size() || json[position] != L'{')
    {
        return std::nullopt;
    }

    const size_t objectStart = position;
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    while (position < json.size())
    {
        const wchar_t ch = json[position++];
        if (inString)
        {
            if (escaped)
            {
                escaped = false;
            }
            else if (ch == L'\\')
            {
                escaped = true;
            }
            else if (ch == L'"')
            {
                inString = false;
            }
            continue;
        }

        if (ch == L'"')
        {
            inString = true;
        }
        else if (ch == L'{')
        {
            ++depth;
        }
        else if (ch == L'}')
        {
            --depth;
            if (depth == 0)
            {
                return json.substr(objectStart, position - objectStart);
            }
        }
    }

    return std::nullopt;
}

std::optional<double> EstimateBpmCore(const std::vector<float>& samples, int sampleRate)
{
    if (samples.size() < static_cast<size_t>(sampleRate * 8))
    {
        return std::nullopt;
    }

    constexpr int frameSize = 1024;
    constexpr int hopSize = 512;
    const size_t frameCount = samples.size() > frameSize ? (samples.size() - frameSize) / hopSize : 0;
    if (frameCount < 32)
    {
        return std::nullopt;
    }

    std::vector<double> energy(frameCount, 0.0);
    for (size_t frame = 0; frame < frameCount; ++frame)
    {
        const size_t start = frame * hopSize;
        double sum = 0.0;
        for (int offset = 0; offset < frameSize; ++offset)
        {
            const double sample = samples[start + offset];
            sum += sample * sample;
        }
        energy[frame] = std::log1p((sum / frameSize) * 1000.0);
    }

    std::vector<double> novelty(frameCount, 0.0);
    double noveltyTotal = 0.0;
    for (size_t index = 1; index < frameCount; ++index)
    {
        const double difference = energy[index] - energy[index - 1];
        if (difference > 0.0)
        {
            novelty[index] = difference;
            noveltyTotal += difference;
        }
    }

    if (noveltyTotal <= 0.001)
    {
        return std::nullopt;
    }

    std::vector<double> smoothed = novelty;
    for (size_t index = 1; index + 1 < frameCount; ++index)
    {
        smoothed[index] = (novelty[index - 1] + novelty[index] + novelty[index + 1]) / 3.0;
    }

    const double noveltyRate = static_cast<double>(sampleRate) / hopSize;
    const int minLag = std::max(1, static_cast<int>((noveltyRate * 60.0 / 220.0) + 0.5));
    const int maxLag = std::min(static_cast<int>(frameCount / 2), static_cast<int>((noveltyRate * 60.0 / 55.0) + 0.5));
    if (maxLag <= minLag)
    {
        return std::nullopt;
    }

    auto correlationAtLag = [&](int lag)
    {
        double score = 0.0;
        int count = 0;
        for (size_t index = static_cast<size_t>(lag); index < frameCount; ++index)
        {
            score += smoothed[index] * smoothed[index - lag];
            ++count;
        }
        return count > 0 ? score / count : 0.0;
    };

    int bestLag = 0;
    double bestScore = 0.0;
    for (int lag = minLag; lag <= maxLag; ++lag)
    {
        double score = correlationAtLag(lag);
        if (lag * 2 <= maxLag)
        {
            score += correlationAtLag(lag * 2) * 0.35;
        }
        if (lag / 2 >= minLag)
        {
            score += correlationAtLag(lag / 2) * 0.15;
        }

        if (score > bestScore)
        {
            bestScore = score;
            bestLag = lag;
        }
    }

    if (bestLag <= 0 || bestScore <= 0.000001)
    {
        return std::nullopt;
    }

    return 60.0 * noveltyRate / bestLag;
}

double NormalizeBpm(double bpm)
{
    while (bpm < 70.0 && bpm * 2.0 <= 220.0)
    {
        bpm *= 2.0;
    }
    while (bpm > 180.0 && bpm / 2.0 >= 55.0)
    {
        bpm /= 2.0;
    }
    return bpm;
}

std::optional<double> EstimateBpm(const std::vector<float>& samples, int sampleRate)
{
    if (samples.size() < static_cast<size_t>(sampleRate * 8))
    {
        return std::nullopt;
    }

    std::vector<double> candidates;
    if (auto fullSample = EstimateBpmCore(samples, sampleRate))
    {
        candidates.push_back(NormalizeBpm(*fullSample));
    }

    const size_t windowSize = static_cast<size_t>(sampleRate * 45);
    if (samples.size() >= windowSize + static_cast<size_t>(sampleRate * 10))
    {
        const size_t hopSize = static_cast<size_t>(sampleRate * 22);
        for (size_t start = 0; start + windowSize <= samples.size(); start += hopSize)
        {
            std::vector<float> window(samples.begin() + static_cast<std::ptrdiff_t>(start),
                                      samples.begin() + static_cast<std::ptrdiff_t>(start + windowSize));
            if (auto windowBpm = EstimateBpmCore(window, sampleRate))
            {
                candidates.push_back(NormalizeBpm(*windowBpm));
            }
        }
    }

    if (candidates.empty())
    {
        return std::nullopt;
    }

    std::sort(candidates.begin(), candidates.end());
    double median = candidates[candidates.size() / 2];
    if (candidates.size() % 2 == 0)
    {
        median = (candidates[candidates.size() / 2 - 1] + median) / 2.0;
    }
    return median;
}

std::optional<std::wstring> EstimateMusicalKey(const std::vector<float>& samples, int sampleRate)
{
    if (samples.size() < static_cast<size_t>(sampleRate * 8))
    {
        return std::nullopt;
    }

    constexpr int frameSize = 4096;
    constexpr int hopSize = 4096;
    constexpr double pi = 3.14159265358979323846;
    const size_t frameCount = samples.size() > frameSize ? (samples.size() - frameSize) / hopSize : 0;
    if (frameCount < 8)
    {
        return std::nullopt;
    }

    std::array<double, frameSize> window {};
    for (int index = 0; index < frameSize; ++index)
    {
        window[static_cast<size_t>(index)] = 0.5 - 0.5 * std::cos((2.0 * pi * index) / (frameSize - 1));
    }

    std::array<double, 12> chroma {};
    int framesUsed = 0;
    for (size_t frame = 0; frame < frameCount; ++frame)
    {
        const size_t start = frame * hopSize;
        double frameEnergy = 0.0;
        for (int offset = 0; offset < frameSize; ++offset)
        {
            const double sample = samples[start + offset];
            frameEnergy += sample * sample;
        }
        if (frameEnergy / frameSize < 0.00001)
        {
            continue;
        }

        for (int midi = 36; midi <= 84; ++midi)
        {
            const double frequency = 440.0 * std::pow(2.0, (midi - 69) / 12.0);
            const double coefficient = 2.0 * std::cos(2.0 * pi * frequency / sampleRate);
            double q1 = 0.0;
            double q2 = 0.0;
            for (int offset = 0; offset < frameSize; ++offset)
            {
                const double q0 = samples[start + offset] * window[static_cast<size_t>(offset)] + coefficient * q1 - q2;
                q2 = q1;
                q1 = q0;
            }

            const double magnitude = std::max(0.0, q1 * q1 + q2 * q2 - coefficient * q1 * q2);
            chroma[static_cast<size_t>(midi % 12)] += std::sqrt(magnitude);
        }
        ++framesUsed;
    }

    if (framesUsed < 8)
    {
        return std::nullopt;
    }

    double chromaTotal = 0.0;
    for (double value : chroma)
    {
        chromaTotal += value;
    }
    if (chromaTotal <= 0.0)
    {
        return std::nullopt;
    }

    static constexpr std::array<double, 12> majorProfile {
        6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88
    };
    static constexpr std::array<double, 12> minorProfile {
        6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17
    };
    static constexpr std::array<const wchar_t*, 12> keyNames {
        L"C", L"C#", L"D", L"Eb", L"E", L"F", L"F#", L"G", L"Ab", L"A", L"Bb", L"B"
    };

    auto scoreProfile = [&](int tonic, const std::array<double, 12>& profile)
    {
        std::array<double, 12> values {};
        double valueMean = 0.0;
        double profileMean = 0.0;
        for (int index = 0; index < 12; ++index)
        {
            values[static_cast<size_t>(index)] = std::log1p(chroma[static_cast<size_t>((tonic + index) % 12)]);
            valueMean += values[static_cast<size_t>(index)];
            profileMean += profile[static_cast<size_t>(index)];
        }
        valueMean /= 12.0;
        profileMean /= 12.0;

        double numerator = 0.0;
        double valueEnergy = 0.0;
        double profileEnergy = 0.0;
        for (int index = 0; index < 12; ++index)
        {
            const double value = values[static_cast<size_t>(index)] - valueMean;
            const double profileValue = profile[static_cast<size_t>(index)] - profileMean;
            numerator += value * profileValue;
            valueEnergy += value * value;
            profileEnergy += profileValue * profileValue;
        }

        if (valueEnergy <= 0.0 || profileEnergy <= 0.0)
        {
            return 0.0;
        }
        return numerator / std::sqrt(valueEnergy * profileEnergy);
    };

    int bestTonic = 0;
    bool bestIsMinor = false;
    double bestScore = -2.0;
    for (int tonic = 0; tonic < 12; ++tonic)
    {
        const double majorScore = scoreProfile(tonic, majorProfile);
        if (majorScore > bestScore)
        {
            bestScore = majorScore;
            bestTonic = tonic;
            bestIsMinor = false;
        }

        const double minorScore = scoreProfile(tonic, minorProfile);
        if (minorScore > bestScore)
        {
            bestScore = minorScore;
            bestTonic = tonic;
            bestIsMinor = true;
        }
    }

    if (bestScore < 0.05)
    {
        return std::nullopt;
    }

    return std::wstring(keyNames[static_cast<size_t>(bestTonic)]) + (bestIsMinor ? L" minor" : L" major");
}

struct LocalMusicAnalysis
{
    std::wstring musicalKey;
    std::wstring bpm;
    std::wstring source;
    double bpmValue = 0.0;
    double keyStrength = 0.0;
    double keyWeight = 1.0;
    int keyVotes = 0;
    int keyCandidateCount = 0;
    bool fullTrack = false;
};

struct MusicAnalysisWindow
{
    int startSeconds = 0;
    int durationSeconds = 120;
    bool fullTrack = false;
    double keyWeight = 1.0;
};

MusicAnalysisWindow ChooseMusicAnalysisWindow(double durationSeconds, bool preferFullTrack)
{
    constexpr int kFullTrackLimitSeconds = 8 * 60;
    constexpr int kEssentiaSampleSeconds = 4 * 60;
    constexpr int kFallbackSampleSeconds = 3 * 60;
    const int maxSampleSeconds = preferFullTrack ? kEssentiaSampleSeconds : kFallbackSampleSeconds;

    if (durationSeconds <= 0.0)
    {
        return { 0, maxSampleSeconds, false };
    }

    const int roundedDuration = std::max(1, static_cast<int>(durationSeconds + 0.5));
    if (preferFullTrack && roundedDuration <= kFullTrackLimitSeconds)
    {
        return { 0, roundedDuration, true };
    }

    const int sampleSeconds = std::min(maxSampleSeconds, roundedDuration);
    const int maxStart = std::max(0, roundedDuration - sampleSeconds);
    int startSeconds = 0;
    if (maxStart > 0)
    {
        const int introBuffer = roundedDuration > 180
            ? std::max(30, static_cast<int>(roundedDuration * 0.2))
            : (roundedDuration > 70 ? 10 : 0);
        startSeconds = std::min(introBuffer, maxStart);
    }

    return { startSeconds, std::max(1, sampleSeconds), false };
}

std::wstring EssentiaSourceLabel(const MusicAnalysisWindow& window)
{
    return window.fullTrack ? L"Essentia full-track analysis" : L"Essentia sample analysis";
}

void AddAnalysisWindow(
    std::vector<MusicAnalysisWindow>& windows,
    int startSeconds,
    int durationSeconds,
    int trackDurationSeconds,
    bool fullTrack,
    double keyWeight)
{
    if (durationSeconds < 20)
    {
        return;
    }

    if (trackDurationSeconds > 0)
    {
        durationSeconds = std::min(durationSeconds, trackDurationSeconds);
        startSeconds = std::clamp(startSeconds, 0, std::max(0, trackDurationSeconds - durationSeconds));
    }
    else
    {
        startSeconds = std::max(0, startSeconds);
    }

    for (const MusicAnalysisWindow& window : windows)
    {
        if (std::abs(window.startSeconds - startSeconds) <= 8 &&
            std::abs(window.durationSeconds - durationSeconds) <= 12)
        {
            return;
        }
    }

    windows.push_back({ startSeconds, std::max(20, durationSeconds), fullTrack, keyWeight });
}

std::vector<MusicAnalysisWindow> BuildEssentiaAnalysisWindows(double durationSeconds)
{
    std::vector<MusicAnalysisWindow> windows;
    if (durationSeconds <= 0.0)
    {
        AddAnalysisWindow(windows, 0, 180, 0, false, 1.0);
        return windows;
    }

    const int trackSeconds = std::max(1, static_cast<int>(durationSeconds + 0.5));
    if (trackSeconds <= 90)
    {
        AddAnalysisWindow(windows, 0, trackSeconds, trackSeconds, true, 1.2);
        return windows;
    }

    if (trackSeconds <= 300)
    {
        AddAnalysisWindow(windows, 0, trackSeconds, trackSeconds, true, 0.9);
    }

    const int sectionSeconds = trackSeconds <= 240 ? 75 : (trackSeconds <= 480 ? 90 : 120);
    const std::array<double, 4> fractions { 0.18, 0.42, 0.62, 0.76 };
    const int sectionCount = trackSeconds <= 300 ? 2 : (trackSeconds <= 480 ? 3 : 4);
    for (int index = 0; index < sectionCount; ++index)
    {
        const int start = static_cast<int>((trackSeconds * fractions[static_cast<size_t>(index)]) + 0.5);
        const double weight = index == 0 ? 1.0 : (index == 1 ? 1.25 : 1.1);
        AddAnalysisWindow(windows, start, sectionSeconds, trackSeconds, false, weight);
    }

    return windows;
}

std::wstring FormatEssentiaKey(const std::wstring& key, const std::wstring& scale)
{
    std::wstring cleanKey = TrimCopy(key);
    std::wstring cleanScale = ToLower(TrimCopy(scale));
    if (cleanKey.empty() || ToLower(cleanKey) == L"none")
    {
        return {};
    }
    if (cleanScale.empty() || cleanScale == L"none")
    {
        return cleanKey;
    }
    return cleanKey + L" " + cleanScale;
}

std::optional<LocalMusicAnalysis> ParseEssentiaMusicJson(const std::wstring& json)
{
    if (json.empty())
    {
        return std::nullopt;
    }

    LocalMusicAnalysis analysis;
    if (auto rhythm = JsonObjectValue(json, L"rhythm"))
    {
        if (auto bpm = JsonNumberValue(*rhythm, L"bpm"))
        {
            analysis.bpmValue = *bpm;
            analysis.bpm = BpmLabel(*bpm);
        }
    }
    if (analysis.bpm.empty())
    {
        if (auto bpm = JsonNumberValue(json, L"rhythm.bpm"))
        {
            analysis.bpmValue = *bpm;
            analysis.bpm = BpmLabel(*bpm);
        }
        else if (auto bpmValue = FirstJsonStringOrNumberValue(json, { L"bpm" }))
        {
            analysis.bpm = *bpmValue;
        }
    }

    if (auto tonal = JsonObjectValue(json, L"tonal"))
    {
        std::wstring bestKey;
        const std::array<std::wstring, 4> keyAlgorithms {
            L"key_edma",
            L"key_krumhansl",
            L"key_temperley",
            L"key"
        };

        struct KeyCandidate
        {
            std::wstring label;
            double strength = 0.0;
            size_t preference = 0;
        };
        std::vector<KeyCandidate> candidates;

        for (size_t index = 0; index < keyAlgorithms.size(); ++index)
        {
            const std::wstring& algorithm = keyAlgorithms[index];
            if (auto keyObject = JsonObjectValue(*tonal, algorithm))
            {
                const std::wstring key = JsonStringValue(*keyObject, L"key").value_or(L"");
                const std::wstring scale = JsonStringValue(*keyObject, L"scale").value_or(L"");
                const double strength = JsonNumberValue(*keyObject, L"strength").value_or(0.0);
                const std::wstring label = FormatEssentiaKey(key, scale);
                if (!label.empty())
                {
                    candidates.push_back({ label, std::max(0.0, strength), index });
                }
            }
        }

        int bestVotes = 0;
        double bestStrength = -1.0;
        size_t bestPreference = keyAlgorithms.size();
        for (const KeyCandidate& candidate : candidates)
        {
            const std::wstring normalizedLabel = ToLower(candidate.label);
            int votes = 0;
            double totalStrength = 0.0;
            size_t preference = keyAlgorithms.size();
            for (const KeyCandidate& other : candidates)
            {
                if (ToLower(other.label) == normalizedLabel)
                {
                    ++votes;
                    totalStrength += other.strength;
                    preference = std::min(preference, other.preference);
                }
            }

            if (votes > bestVotes ||
                (votes == bestVotes && preference < bestPreference) ||
                (votes == bestVotes && preference == bestPreference && totalStrength > bestStrength))
            {
                bestVotes = votes;
                bestStrength = totalStrength;
                bestPreference = preference;
                bestKey = candidate.label;
            }
        }

        if (bestKey.empty())
        {
            const std::wstring key = JsonStringValue(*tonal, L"chords_key").value_or(L"");
            const std::wstring scale = JsonStringValue(*tonal, L"chords_scale").value_or(L"");
            bestKey = FormatEssentiaKey(key, scale);
            if (!bestKey.empty())
            {
                bestVotes = 1;
                bestStrength = 0.35;
            }
        }

        analysis.musicalKey = bestKey;
        analysis.keyVotes = bestKey.empty() ? 0 : bestVotes;
        analysis.keyStrength = bestKey.empty() ? 0.0 : std::max(0.0, bestStrength);
        analysis.keyCandidateCount = static_cast<int>(candidates.size());
    }

    if (analysis.bpm.empty() && analysis.musicalKey.empty())
    {
        return std::nullopt;
    }

    analysis.source = L"Essentia analysis";
    return analysis;
}

std::optional<int> KeyNameToSemitone(std::wstring keyName)
{
    keyName = ToLower(TrimCopy(keyName));
    if (keyName == L"c" || keyName == L"b#") return 0;
    if (keyName == L"c#" || keyName == L"db") return 1;
    if (keyName == L"d") return 2;
    if (keyName == L"d#" || keyName == L"eb") return 3;
    if (keyName == L"e" || keyName == L"fb") return 4;
    if (keyName == L"f" || keyName == L"e#") return 5;
    if (keyName == L"f#" || keyName == L"gb") return 6;
    if (keyName == L"g") return 7;
    if (keyName == L"g#" || keyName == L"ab") return 8;
    if (keyName == L"a") return 9;
    if (keyName == L"a#" || keyName == L"bb") return 10;
    if (keyName == L"b" || keyName == L"cb") return 11;
    return std::nullopt;
}

struct ParsedKeyLabel
{
    int semitone = 0;
    bool minor = false;
};

std::optional<ParsedKeyLabel> ParseKeyLabel(const std::wstring& label)
{
    const std::wstring trimmed = TrimCopy(label);
    const std::wstring lower = ToLower(trimmed);
    const bool isMinor = lower.find(L"minor") != std::wstring::npos;
    const bool isMajor = lower.find(L"major") != std::wstring::npos;
    if (!isMinor && !isMajor)
    {
        return std::nullopt;
    }

    const size_t separator = trimmed.find(L' ');
    const std::wstring keyName = separator == std::wstring::npos ? trimmed : trimmed.substr(0, separator);
    if (auto semitone = KeyNameToSemitone(keyName))
    {
        return ParsedKeyLabel { *semitone, isMinor };
    }
    return std::nullopt;
}

bool AreRelativeKeys(const std::wstring& left, const std::wstring& right)
{
    const auto first = ParseKeyLabel(left);
    const auto second = ParseKeyLabel(right);
    if (!first || !second || first->minor == second->minor)
    {
        return false;
    }

    if (!first->minor)
    {
        return second->semitone == ((first->semitone + 9) % 12);
    }
    return second->semitone == ((first->semitone + 3) % 12);
}

struct KeyVoteSummary
{
    std::wstring label;
    double score = 0.0;
    int sections = 0;
};

std::wstring ConfidenceLabel(const KeyVoteSummary& best, double secondScore, int totalSections)
{
    const double ratio = secondScore > 0.0 ? best.score / secondScore : 99.0;
    if (best.sections >= std::max(2, totalSections - 1) && ratio >= 1.35)
    {
        return L"high confidence";
    }
    if (best.sections >= 2 || ratio >= 1.2)
    {
        return L"medium confidence";
    }
    return L"low confidence";
}

std::optional<LocalMusicAnalysis> CombineEssentiaAnalyses(const std::vector<LocalMusicAnalysis>& analyses)
{
    if (analyses.empty())
    {
        return std::nullopt;
    }

    LocalMusicAnalysis combined;
    std::vector<double> bpms;
    std::map<std::wstring, KeyVoteSummary> keyVotes;

    for (const LocalMusicAnalysis& analysis : analyses)
    {
        if (analysis.bpmValue > 0.0)
        {
            bpms.push_back(NormalizeBpm(analysis.bpmValue));
        }

        if (!analysis.musicalKey.empty())
        {
            const std::wstring normalizedKey = ToLower(analysis.musicalKey);
            KeyVoteSummary& vote = keyVotes[normalizedKey];
            if (vote.label.empty())
            {
                vote.label = analysis.musicalKey;
            }

            const double agreementBonus = std::min(3, std::max(1, analysis.keyVotes)) * 0.18;
            const double strengthBonus = std::min(2.0, std::max(0.0, analysis.keyStrength)) * 0.22;
            vote.score += analysis.keyWeight * (1.0 + agreementBonus + strengthBonus);
            ++vote.sections;
        }
    }

    if (!bpms.empty())
    {
        std::sort(bpms.begin(), bpms.end());
        double bpm = bpms[bpms.size() / 2];
        if (bpms.size() % 2 == 0)
        {
            bpm = (bpms[bpms.size() / 2 - 1] + bpm) / 2.0;
        }
        combined.bpmValue = bpm;
        combined.bpm = BpmLabel(bpm);
    }

    std::vector<KeyVoteSummary> sortedKeys;
    for (const auto& [_, vote] : keyVotes)
    {
        sortedKeys.push_back(vote);
    }
    std::sort(
        sortedKeys.begin(),
        sortedKeys.end(),
        [](const KeyVoteSummary& left, const KeyVoteSummary& right)
        {
            if (std::abs(left.score - right.score) > 0.0001)
            {
                return left.score > right.score;
            }
            return left.sections > right.sections;
        });

    std::wstring confidence = L"BPM only";
    if (!sortedKeys.empty())
    {
        const KeyVoteSummary& best = sortedKeys.front();
        const double secondScore = sortedKeys.size() > 1 ? sortedKeys[1].score : 0.0;
        bool relativeAmbiguity = false;
        if (sortedKeys.size() > 1 &&
            sortedKeys[1].score >= best.score * 0.72 &&
            AreRelativeKeys(best.label, sortedKeys[1].label))
        {
            relativeAmbiguity = true;
        }

        if (relativeAmbiguity)
        {
            combined.musicalKey = best.label + L" / " + sortedKeys[1].label;
            confidence = L"relative key ambiguity";
        }
        else
        {
            combined.musicalKey = best.label;
            confidence = ConfidenceLabel(best, secondScore, static_cast<int>(analyses.size()));
        }
    }

    if (combined.bpm.empty() && combined.musicalKey.empty())
    {
        return std::nullopt;
    }

    combined.source = L"Essentia deep scan: " + confidence +
        L" (" + std::to_wstring(analyses.size()) + L" passes)";
    return combined;
}

std::optional<LocalMusicAnalysis> AnalyzeMusicWithEssentia(
    const std::wstring& streamUrl,
    const ExternalToolStatus& tools,
    const ProcessRunner& processRunner,
    const std::atomic_bool& cancelRequested,
    int sampleStart,
    int sampleSeconds,
    const std::wstring& sourceLabel,
    double keyWeight,
    bool fullTrack)
{
    if (!tools.essentiaFound || cancelRequested.load())
    {
        return std::nullopt;
    }

    const std::filesystem::path wavPath = TemporaryWavAudioPath();
    const std::filesystem::path jsonPath = TemporaryJsonPath();
    ProcessResult ffmpegResult = processRunner.Run(
        tools.ffmpegPath,
        {
            L"-y",
            L"-nostdin",
            L"-hide_banner",
            L"-loglevel",
            L"error",
            L"-ss",
            std::to_wstring(sampleStart),
            L"-i",
            streamUrl,
            L"-t",
            std::to_wstring(sampleSeconds),
            L"-vn",
            L"-ac",
            L"2",
            L"-ar",
            L"44100",
            wavPath.wstring()
        },
        cancelRequested,
        nullptr);

    if (!ffmpegResult.cancelled && ffmpegResult.exitCode == 0)
    {
        const ProcessResult essentiaResult = processRunner.Run(
            tools.essentiaPath,
            {
                wavPath.wstring(),
                jsonPath.wstring()
            },
            cancelRequested,
            nullptr);

        if (!essentiaResult.cancelled && essentiaResult.exitCode == 0)
        {
            std::wstring json = ReadTextFileWide(jsonPath);
            std::error_code cleanupError;
            std::filesystem::remove(wavPath, cleanupError);
            std::filesystem::remove(jsonPath, cleanupError);
            auto analysis = ParseEssentiaMusicJson(json);
            if (analysis && !sourceLabel.empty())
            {
                analysis->source = sourceLabel;
            }
            if (analysis)
            {
                analysis->keyWeight = keyWeight;
                analysis->fullTrack = fullTrack;
            }
            return analysis;
        }
    }

    std::error_code cleanupError;
    std::filesystem::remove(wavPath, cleanupError);
    std::filesystem::remove(jsonPath, cleanupError);
    return std::nullopt;
}

std::optional<LocalMusicAnalysis> AnalyzeMusicWithEssentiaDeepScan(
    const std::wstring& streamUrl,
    const ExternalToolStatus& tools,
    const ProcessRunner& processRunner,
    const std::atomic_bool& cancelRequested,
    double durationSeconds)
{
    if (!tools.essentiaFound || cancelRequested.load())
    {
        return std::nullopt;
    }

    const std::vector<MusicAnalysisWindow> windows = BuildEssentiaAnalysisWindows(durationSeconds);
    std::vector<LocalMusicAnalysis> analyses;
    for (const MusicAnalysisWindow& window : windows)
    {
        if (cancelRequested.load())
        {
            return std::nullopt;
        }

        if (auto analysis = AnalyzeMusicWithEssentia(
            streamUrl,
            tools,
            processRunner,
            cancelRequested,
            window.startSeconds,
            window.durationSeconds,
            EssentiaSourceLabel(window),
            window.keyWeight,
            window.fullTrack))
        {
            analyses.push_back(*analysis);
        }
    }

    return CombineEssentiaAnalyses(analyses);
}

std::optional<LocalMusicAnalysis> AnalyzeMusicFromAudioSample(
    const std::wstring& url,
    const ExternalToolStatus& tools,
    const ProcessRunner& processRunner,
    const std::atomic_bool& cancelRequested,
    double durationSeconds)
{
    if (!tools.ytDlpFound || !tools.ffmpegFound || cancelRequested.load())
    {
        return std::nullopt;
    }

    const ProcessResult streamResult = processRunner.Run(
        tools.ytDlpPath,
        {
            L"--no-warnings",
            L"--no-playlist",
            L"-f",
            L"ba/bestaudio",
            L"--get-url",
            url
        },
        cancelRequested,
        nullptr);
    if (streamResult.cancelled || streamResult.exitCode != 0)
    {
        return std::nullopt;
    }

    const std::wstring streamUrl = FirstHttpLine(streamResult.output);
    if (streamUrl.empty())
    {
        return std::nullopt;
    }

    if (auto essentiaAnalysis = AnalyzeMusicWithEssentiaDeepScan(
        streamUrl,
        tools,
        processRunner,
        cancelRequested,
        durationSeconds))
    {
        return essentiaAnalysis;
    }

    const MusicAnalysisWindow fallbackWindow = ChooseMusicAnalysisWindow(durationSeconds, false);
    const std::filesystem::path rawPath = TemporaryRawAudioPath();

    ProcessResult ffmpegResult = processRunner.Run(
        tools.ffmpegPath,
        {
            L"-y",
            L"-nostdin",
            L"-hide_banner",
            L"-loglevel",
            L"error",
            L"-ss",
            std::to_wstring(fallbackWindow.startSeconds),
            L"-i",
            streamUrl,
            L"-t",
            std::to_wstring(fallbackWindow.durationSeconds),
            L"-vn",
            L"-ac",
            L"1",
            L"-ar",
            L"22050",
            L"-f",
            L"s16le",
            rawPath.wstring()
        },
        cancelRequested,
        nullptr);
    if (ffmpegResult.cancelled || ffmpegResult.exitCode != 0)
    {
        std::error_code cleanupError;
        std::filesystem::remove(rawPath, cleanupError);
        return std::nullopt;
    }

    std::vector<float> samples = ReadPcm16Mono(rawPath);
    std::error_code cleanupError;
    std::filesystem::remove(rawPath, cleanupError);
    if (samples.empty() || cancelRequested.load())
    {
        return std::nullopt;
    }

    LocalMusicAnalysis analysis;
    if (auto bpm = EstimateBpm(samples, 22050))
    {
        analysis.bpm = BpmLabel(*bpm);
    }
    if (auto musicalKey = EstimateMusicalKey(samples, 22050))
    {
        analysis.musicalKey = *musicalKey;
    }

    if (analysis.bpm.empty() && analysis.musicalKey.empty())
    {
        return std::nullopt;
    }

    analysis.source = L"Built-in local estimate";
    return analysis;
}

std::wstring ExtensionFor(MediaOutputFormat format)
{
    switch (format)
    {
    case MediaOutputFormat::Mp4:
        return L".mp4";
    case MediaOutputFormat::Mp3:
        return L".mp3";
    case MediaOutputFormat::Wav:
        return L".wav";
    }
    return L".mp4";
}

std::wstring Mp4FormatSelector(Mp4Quality quality)
{
    switch (quality)
    {
    case Mp4Quality::Best:
        return L"bv*+ba[ext=m4a]/bv*+ba/best";
    case Mp4Quality::P4320:
        return L"bv*[height<=4320]+ba[ext=m4a]/bv*[height<=4320]+ba/b[height<=4320]/best[height<=4320]";
    case Mp4Quality::P2160:
        return L"bv*[height<=2160]+ba[ext=m4a]/bv*[height<=2160]+ba/b[height<=2160]/best[height<=2160]";
    case Mp4Quality::P1440:
        return L"bv*[height<=1440]+ba[ext=m4a]/bv*[height<=1440]+ba/b[height<=1440]/best[height<=1440]";
    case Mp4Quality::P1080:
        return L"bv*[height<=1080]+ba[ext=m4a]/bv*[height<=1080]+ba/b[height<=1080]/best[height<=1080]";
    case Mp4Quality::P720:
        return L"bv*[height<=720]+ba[ext=m4a]/bv*[height<=720]+ba/b[height<=720]/best[height<=720]";
    case Mp4Quality::P480:
        return L"bv*[height<=480]+ba[ext=m4a]/bv*[height<=480]+ba/b[height<=480]/best[height<=480]";
    }
    return L"bv*+ba[ext=m4a]/bv*+ba/best";
}

std::wstring BitrateValue(Mp3Bitrate bitrate)
{
    switch (bitrate)
    {
    case Mp3Bitrate::K320:
        return L"320K";
    case Mp3Bitrate::K256:
        return L"256K";
    case Mp3Bitrate::K192:
        return L"192K";
    case Mp3Bitrate::K128:
        return L"128K";
    }
    return L"320K";
}

std::filesystem::path ResolveConflict(const std::filesystem::path& requestedPath)
{
    if (!std::filesystem::exists(requestedPath))
    {
        return requestedPath;
    }

    const std::filesystem::path directory = requestedPath.parent_path();
    const std::wstring stem = requestedPath.stem().wstring();
    const std::wstring extension = requestedPath.extension().wstring();
    for (int suffix = 1; suffix < 10000; ++suffix)
    {
        std::filesystem::path candidate = directory / (stem + L"_" + std::to_wstring(suffix) + extension);
        if (!std::filesystem::exists(candidate))
        {
            return candidate;
        }
    }

    return requestedPath;
}

void ParseProgressText(const std::wstring& text, MediaDownloadJob& job)
{
    if (text.find(L"[ExtractAudio]") != std::wstring::npos ||
        text.find(L"[Merger]") != std::wstring::npos ||
        text.find(L"ffmpeg") != std::wstring::npos)
    {
        job.status = MediaDownloadStatus::Converting;
    }
    else if (text.find(L"[download]") != std::wstring::npos)
    {
        job.status = MediaDownloadStatus::Downloading;
    }

    const size_t percentPosition = text.find(L'%');
    if (percentPosition != std::wstring::npos)
    {
        size_t start = percentPosition;
        while (start > 0 && (std::iswdigit(text[start - 1]) || text[start - 1] == L'.' || text[start - 1] == L' '))
        {
            --start;
        }

        try
        {
            const double percent = std::stod(text.substr(start, percentPosition - start));
            job.progress = std::clamp(percent / 100.0, 0.0, 1.0);
        }
        catch (...)
        {
        }
    }

    const size_t speedMarker = text.find(L" at ");
    if (speedMarker != std::wstring::npos)
    {
        size_t speedStart = speedMarker + 4;
        size_t speedEnd = text.find(L" ETA ", speedStart);
        if (speedEnd == std::wstring::npos)
        {
            speedEnd = text.find(L'\n', speedStart);
        }
        if (speedEnd != std::wstring::npos && speedEnd > speedStart)
        {
            job.speed = text.substr(speedStart, speedEnd - speedStart);
        }
    }

    const size_t etaMarker = text.find(L" ETA ");
    if (etaMarker != std::wstring::npos)
    {
        size_t etaStart = etaMarker + 5;
        size_t etaEnd = text.find_first_of(L"\r\n", etaStart);
        if (etaEnd == std::wstring::npos)
        {
            etaEnd = text.size();
        }
        if (etaEnd > etaStart)
        {
            job.eta = text.substr(etaStart, etaEnd - etaStart);
        }
    }
}

std::wstring JoinMissingToolsMessage(const ExternalToolStatus& tools)
{
    std::wstring message;
    if (!tools.ytDlpFound)
    {
        message += L"yt-dlp";
    }
    if (!tools.ffmpegFound)
    {
        if (!message.empty())
        {
            message += L" and ";
        }
        message += L"FFmpeg";
    }
    if (!message.empty())
    {
        message += L" must be installed and available on PATH.";
    }
    return message;
}
}

MediaPlatform SupportedPlatformRegistry::DetectPlatform(const std::wstring& url)
{
    if (!StartsWithHttp(url))
    {
        return MediaPlatform::Unknown;
    }

    const std::wstring lower = ToLower(url);
    if (lower.find(L"youtube.com/") != std::wstring::npos ||
        lower.find(L"youtu.be/") != std::wstring::npos ||
        lower.find(L"music.youtube.com/") != std::wstring::npos)
    {
        return MediaPlatform::YouTube;
    }

    if (lower.find(L"soundcloud.com/") != std::wstring::npos)
    {
        return MediaPlatform::SoundCloud;
    }

    return MediaPlatform::Unknown;
}

bool SupportedPlatformRegistry::IsSupportedUrl(const std::wstring& url)
{
    return DetectPlatform(url) != MediaPlatform::Unknown;
}

bool SupportedPlatformRegistry::IsLikelyDirectMediaUrl(const std::wstring& url)
{
    const MediaPlatform platform = DetectPlatform(url);
    if (platform == MediaPlatform::Unknown)
    {
        return false;
    }

    const std::wstring lower = ToLower(url);
    const std::wstring path = UrlPathOnly(url);
    const std::vector<std::wstring> segments = UrlPathSegments(url);

    if (platform == MediaPlatform::YouTube)
    {
        if (lower.find(L"youtu.be/") != std::wstring::npos)
        {
            return !segments.empty();
        }
        if (path == L"/watch")
        {
            return UrlQueryHasParameter(url, L"v");
        }
        if (!segments.empty() &&
            (segments[0] == L"shorts" || segments[0] == L"live" || segments[0] == L"embed"))
        {
            return segments.size() >= 2 && !segments[1].empty();
        }
        return false;
    }

    if (platform == MediaPlatform::SoundCloud)
    {
        if (segments.size() < 2)
        {
            return false;
        }

        const std::wstring& section = segments[1];
        if (section == L"sets" ||
            section == L"albums" ||
            section == L"tracks" ||
            section == L"likes" ||
            section == L"reposts" ||
            section == L"popular-tracks")
        {
            return false;
        }

        return true;
    }

    return false;
}

std::wstring SupportedPlatformRegistry::PlatformLabel(MediaPlatform platform)
{
    switch (platform)
    {
    case MediaPlatform::YouTube:
        return L"YouTube";
    case MediaPlatform::SoundCloud:
        return L"SoundCloud";
    case MediaPlatform::Unknown:
        break;
    }
    return L"Unknown";
}

ExternalToolStatus ExternalToolService::CheckTools() const
{
    ExternalToolStatus status;
    if (const auto ytDlp = FindBesideExecutable({ L"tools\\yt-dlp.exe", L"yt-dlp.exe" }))
    {
        status.ytDlpFound = true;
        status.ytDlpPath = *ytDlp;
    }
    else if (const auto ytDlpOnPath = FindOnPath(L"yt-dlp.exe"))
    {
        status.ytDlpFound = true;
        status.ytDlpPath = *ytDlpOnPath;
    }
    else if (const auto ytDlpNoExtension = FindOnPath(L"yt-dlp"))
    {
        status.ytDlpFound = true;
        status.ytDlpPath = *ytDlpNoExtension;
    }

    if (const auto ffmpeg = FindBesideExecutable({ L"tools\\ffmpeg\\bin\\ffmpeg.exe", L"tools\\ffmpeg.exe", L"ffmpeg.exe" }))
    {
        status.ffmpegFound = true;
        status.ffmpegPath = *ffmpeg;
    }
    else if (const auto ffmpegOnPath = FindOnPath(L"ffmpeg.exe"))
    {
        status.ffmpegFound = true;
        status.ffmpegPath = *ffmpegOnPath;
    }
    else if (const auto ffmpegNoExtension = FindOnPath(L"ffmpeg"))
    {
        status.ffmpegFound = true;
        status.ffmpegPath = *ffmpegNoExtension;
    }

    if (const auto ffprobe = FindBesideExecutable({ L"tools\\ffmpeg\\bin\\ffprobe.exe", L"tools\\ffprobe.exe", L"ffprobe.exe" }))
    {
        status.ffprobeFound = true;
        status.ffprobePath = *ffprobe;
    }
    else if (const auto ffprobeOnPath = FindOnPath(L"ffprobe.exe"))
    {
        status.ffprobeFound = true;
        status.ffprobePath = *ffprobeOnPath;
    }
    else if (const auto ffprobeNoExtension = FindOnPath(L"ffprobe"))
    {
        status.ffprobeFound = true;
        status.ffprobePath = *ffprobeNoExtension;
    }

    if (const auto essentia = FindBesideExecutable({
        L"tools\\essentia_streaming_extractor_music.exe",
        L"tools\\essentia\\essentia_streaming_extractor_music.exe",
        L"essentia_streaming_extractor_music.exe"
    }))
    {
        status.essentiaFound = true;
        status.essentiaPath = *essentia;
    }
    else if (const auto essentiaOnPath = FindOnPath(L"essentia_streaming_extractor_music.exe"))
    {
        status.essentiaFound = true;
        status.essentiaPath = *essentiaOnPath;
    }
    else if (const auto essentiaNoExtension = FindOnPath(L"essentia_streaming_extractor_music"))
    {
        status.essentiaFound = true;
        status.essentiaPath = *essentiaNoExtension;
    }

    return status;
}

std::filesystem::path ExternalToolService::DefaultDownloadsFolder()
{
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_DEFAULT, nullptr, &path)) && path)
    {
        std::filesystem::path downloads = path;
        CoTaskMemFree(path);
        return downloads;
    }

    wchar_t userProfile[MAX_PATH] {};
    const DWORD length = GetEnvironmentVariableW(L"USERPROFILE", userProfile, static_cast<DWORD>(std::size(userProfile)));
    if (length > 0 && length < std::size(userProfile))
    {
        return std::filesystem::path(userProfile) / L"Downloads";
    }

    return std::filesystem::current_path();
}

ProcessResult ProcessRunner::Run(
    const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments,
    const std::atomic_bool& cancelRequested,
    const OutputCallback& outputCallback) const
{
    ProcessResult result;

    SECURITY_ATTRIBUTES securityAttributes {};
    securityAttributes.nLength = sizeof(securityAttributes);
    securityAttributes.bInheritHandle = TRUE;

    ScopedHandle readPipe;
    ScopedHandle writePipe;
    if (!CreatePipe(readPipe.Put(), writePipe.Put(), &securityAttributes, 0))
    {
        result.output = L"Could not create process pipe.";
        return result;
    }
    SetHandleInformation(readPipe.Get(), HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startupInfo {};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.hStdOutput = writePipe.Get();
    startupInfo.hStdError = writePipe.Get();
    startupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION processInformation {};
    std::wstring commandLine = BuildCommandLine(executable, arguments);

    const BOOL created = CreateProcessW(
        executable.c_str(),
        commandLine.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startupInfo,
        &processInformation);

    writePipe.Reset();

    if (!created)
    {
        result.output = L"Could not start external tool.";
        return result;
    }

    ScopedHandle process(processInformation.hProcess);
    ScopedHandle thread(processInformation.hThread);

    std::string pendingBytes;
    std::array<char, 4096> buffer {};
    bool processRunning = true;
    while (processRunning)
    {
        if (cancelRequested.load())
        {
            TerminateProcess(process.Get(), 1);
            result.cancelled = true;
        }

        DWORD available = 0;
        while (PeekNamedPipe(readPipe.Get(), nullptr, 0, nullptr, &available, nullptr) && available > 0)
        {
            DWORD bytesRead = 0;
            const DWORD bytesToRead = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
            if (!ReadFile(readPipe.Get(), buffer.data(), bytesToRead, &bytesRead, nullptr) || bytesRead == 0)
            {
                break;
            }

            std::string chunk(buffer.data(), buffer.data() + bytesRead);
            pendingBytes += chunk;
            const std::wstring wideChunk = BytesToWide(chunk);
            result.output += wideChunk;
            if (outputCallback)
            {
                outputCallback(wideChunk);
            }
            available = 0;
        }

        const DWORD waitResult = WaitForSingleObject(process.Get(), 50);
        processRunning = waitResult == WAIT_TIMEOUT;
    }

    DWORD bytesRead = 0;
    while (ReadFile(readPipe.Get(), buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr) && bytesRead > 0)
    {
        std::string chunk(buffer.data(), buffer.data() + bytesRead);
        const std::wstring wideChunk = BytesToWide(chunk);
        result.output += wideChunk;
        if (outputCallback)
        {
            outputCallback(wideChunk);
        }
    }

    GetExitCodeProcess(process.Get(), &result.exitCode);
    if (result.cancelled)
    {
        result.exitCode = 1;
    }
    return result;
}

std::optional<MediaDownloadJob> MediaMetadataService::Analyze(
    const std::wstring& url,
    const ExternalToolStatus& tools,
    const std::atomic_bool& cancelRequested,
    std::wstring& errorMessage) const
{
    if (!SupportedPlatformRegistry::IsSupportedUrl(url))
    {
        errorMessage = L"Unsupported URL. Please enter a YouTube or SoundCloud link.";
        return std::nullopt;
    }

    if (!SupportedPlatformRegistry::IsLikelyDirectMediaUrl(url))
    {
        errorMessage = L"That link looks like a profile, channel, playlist, or collection. Please paste a direct YouTube video or SoundCloud track link.";
        return std::nullopt;
    }

    if (!tools.ytDlpFound)
    {
        errorMessage = L"yt-dlp must be installed and available on PATH.";
        return std::nullopt;
    }

    std::vector<std::wstring> arguments {
        L"--dump-single-json",
        L"--skip-download",
        L"--no-warnings",
        L"--no-playlist",
        url
    };

    const ProcessResult result = processRunner_.Run(tools.ytDlpPath, arguments, cancelRequested, nullptr);
    if (result.cancelled)
    {
        errorMessage = L"Analysis cancelled.";
        return std::nullopt;
    }

    if (result.exitCode != 0)
    {
        errorMessage = FriendlyExternalToolMessage(result.output, L"Could not analyze this URL.");
        return std::nullopt;
    }

    if (LooksLikeCollectionMetadata(result.output))
    {
        errorMessage = L"That link looks like a profile, channel, playlist, or collection. Please paste a direct YouTube video or SoundCloud track link.";
        return std::nullopt;
    }

    MediaDownloadJob job;
    job.url = url;
    job.platform = SupportedPlatformRegistry::DetectPlatform(url);
    job.title = JsonStringValue(result.output, L"title").value_or(L"Untitled media");
    job.uploader = JsonStringValue(result.output, L"uploader").value_or(JsonStringValue(result.output, L"channel").value_or(L"Unknown"));
    job.thumbnailUrl = JsonStringValue(result.output, L"thumbnail").value_or(L"");
    const auto duration = JsonNumberValue(result.output, L"duration");
    const double durationSeconds = duration.value_or(0.0);
    job.durationSeconds = durationSeconds;
    if (duration)
    {
        job.duration = DurationLabel(*duration);
    }
    else
    {
        job.duration = L"Unknown";
    }

    const std::wstring vcodec = ToLower(JsonStringValue(result.output, L"vcodec").value_or(L""));
    const bool hasVideoFormat = JsonHasVideoFormat(result.output);
    job.maxVideoHeight = JsonMaxVideoHeight(result.output);
    if (job.platform == MediaPlatform::SoundCloud)
    {
        job.mediaType = MediaType::Audio;
    }
    else if (hasVideoFormat || (!vcodec.empty() && vcodec != L"none"))
    {
        job.mediaType = MediaType::Video;
    }
    else if (vcodec == L"none")
    {
        job.mediaType = MediaType::Audio;
    }
    else
    {
        job.mediaType = job.platform == MediaPlatform::YouTube ? MediaType::Video : MediaType::Unknown;
    }

    if (!duration && job.mediaType == MediaType::Unknown)
    {
        errorMessage = L"That link does not appear to be a direct downloadable media item. Please paste a YouTube video or SoundCloud track link.";
        return std::nullopt;
    }

    job.status = MediaDownloadStatus::Ready;
    return job;
}

ExternalToolStatus MediaDownloadService::CheckExternalTools() const
{
    return externalToolService_.CheckTools();
}

std::optional<MediaDownloadJob> MediaDownloadService::Analyze(
    const std::wstring& url,
    const std::atomic_bool& cancelRequested,
    std::wstring& errorMessage) const
{
    const ExternalToolStatus tools = CheckExternalTools();
    return metadataService_.Analyze(url, tools, cancelRequested, errorMessage);
}

MediaDownloadJob MediaDownloadService::AnalyzeMusic(
    MediaDownloadJob job,
    const std::atomic_bool& cancelRequested,
    std::wstring& errorMessage) const
{
    if (!SupportedPlatformRegistry::IsSupportedUrl(job.url) ||
        !SupportedPlatformRegistry::IsLikelyDirectMediaUrl(job.url))
    {
        job.status = MediaDownloadStatus::Ready;
        errorMessage = L"Analyze a direct YouTube video or SoundCloud track first.";
        job.errorMessage = errorMessage;
        return job;
    }

    const ExternalToolStatus tools = CheckExternalTools();
    const std::wstring missingTools = JoinMissingToolsMessage(tools);
    if (!missingTools.empty())
    {
        job.status = MediaDownloadStatus::Ready;
        errorMessage = missingTools;
        job.errorMessage = errorMessage;
        return job;
    }

    if (auto analysis = AnalyzeMusicFromAudioSample(job.url, tools, processRunner_, cancelRequested, job.durationSeconds))
    {
        if (!analysis->musicalKey.empty())
        {
            job.musicalKey = analysis->musicalKey;
        }
        if (!analysis->bpm.empty())
        {
            job.bpm = analysis->bpm;
        }
        job.musicMetadataSource = analysis->source.empty() ? L"Local estimate" : analysis->source;
        job.errorMessage.clear();
    }
    else
    {
        errorMessage = cancelRequested.load()
            ? L"Music analysis cancelled."
            : L"Could not estimate BPM/key from this audio.";
        job.errorMessage = errorMessage;
    }

    job.status = MediaDownloadStatus::Ready;
    return job;
}

MediaDownloadJob MediaDownloadService::Download(
    MediaDownloadJob job,
    const MediaDownloadOptions& options,
    const std::atomic_bool& cancelRequested,
    const ProgressCallback& progressCallback) const
{
    const ExternalToolStatus tools = CheckExternalTools();
    const std::wstring missingTools = JoinMissingToolsMessage(tools);
    if (!missingTools.empty())
    {
        job.status = MediaDownloadStatus::Failed;
        job.errorMessage = missingTools;
        return job;
    }

    if (!SupportedPlatformRegistry::IsSupportedUrl(job.url))
    {
        job.status = MediaDownloadStatus::Failed;
        job.errorMessage = L"Unsupported URL. Please enter a YouTube or SoundCloud link.";
        return job;
    }

    if (!SupportedPlatformRegistry::IsLikelyDirectMediaUrl(job.url))
    {
        job.status = MediaDownloadStatus::Failed;
        job.errorMessage = L"That link looks like a profile, channel, playlist, or collection. Please paste a direct YouTube video or SoundCloud track link.";
        return job;
    }

    if (options.outputFolder.empty() || !std::filesystem::exists(options.outputFolder))
    {
        job.status = MediaDownloadStatus::Failed;
        job.errorMessage = L"Output folder does not exist.";
        return job;
    }

    if (options.outputFormat == MediaOutputFormat::Mp4 &&
        job.platform == MediaPlatform::SoundCloud &&
        job.mediaType == MediaType::Audio)
    {
        job.status = MediaDownloadStatus::Failed;
        job.errorMessage = L"This source appears to be audio-only. Choose MP3 or WAV instead.";
        return job;
    }

    job.outputFormat = options.outputFormat;
    job.mp4Quality = options.mp4Quality;
    job.mp3Bitrate = options.mp3Bitrate;
    job.outputFolder = options.outputFolder;
    job.progress = 0.0;
    job.speed.clear();
    job.eta.clear();
    job.errorMessage.clear();

    std::wstring baseName = options.customFileName.empty() ? job.title : options.customFileName;
    baseName = SanitizeFileName(baseName);
    const std::filesystem::path requestedPath = options.outputFolder / (baseName + ExtensionFor(options.outputFormat));
    job.outputFilePath = ResolveConflict(requestedPath);

    std::filesystem::path outputTemplate = job.outputFilePath.parent_path() / (job.outputFilePath.stem().wstring() + L".%(ext)s");

    std::vector<std::wstring> arguments {
        L"--no-playlist",
        L"--newline",
        L"--ffmpeg-location",
        tools.ffmpegPath.parent_path().wstring(),
        L"-o",
        outputTemplate.wstring()
    };

    if (options.outputFormat == MediaOutputFormat::Mp4)
    {
        arguments.push_back(L"-f");
        arguments.push_back(Mp4FormatSelector(options.mp4Quality));
        arguments.push_back(L"--merge-output-format");
        arguments.push_back(L"mp4");
        arguments.push_back(L"--remux-video");
        arguments.push_back(L"mp4");
    }
    else if (options.outputFormat == MediaOutputFormat::Mp3)
    {
        arguments.push_back(L"-x");
        arguments.push_back(L"--audio-format");
        arguments.push_back(L"mp3");
        arguments.push_back(L"--audio-quality");
        arguments.push_back(BitrateValue(options.mp3Bitrate));
    }
    else
    {
        arguments.push_back(L"-x");
        arguments.push_back(L"--audio-format");
        arguments.push_back(L"wav");
    }

    arguments.push_back(job.url);

    job.status = MediaDownloadStatus::Downloading;
    if (progressCallback)
    {
        progressCallback(job);
    }

    ProcessResult result = processRunner_.Run(
        tools.ytDlpPath,
        arguments,
        cancelRequested,
        [&](const std::wstring& output)
        {
            ParseProgressText(output, job);
            if (progressCallback)
            {
                progressCallback(job);
            }
        });

    if (result.cancelled)
    {
        job.status = MediaDownloadStatus::Cancelled;
        job.errorMessage = L"Download cancelled.";
        return job;
    }

    if (result.exitCode != 0)
    {
        job.status = MediaDownloadStatus::Failed;
        job.errorMessage = FriendlyExternalToolMessage(result.output, L"Download failed.");
        return job;
    }

    if (!std::filesystem::exists(job.outputFilePath))
    {
        const std::filesystem::path fallbackPath = job.outputFilePath.parent_path() /
            (job.outputFilePath.stem().wstring() + ExtensionFor(options.outputFormat));
        if (std::filesystem::exists(fallbackPath))
        {
            job.outputFilePath = fallbackPath;
        }
    }

    job.progress = 1.0;
    job.status = MediaDownloadStatus::Complete;
    job.errorMessage.clear();
    return job;
}

std::wstring MediaDownloadService::FormatLabel(MediaOutputFormat format)
{
    switch (format)
    {
    case MediaOutputFormat::Mp4:
        return L"MP4";
    case MediaOutputFormat::Mp3:
        return L"MP3";
    case MediaOutputFormat::Wav:
        return L"WAV";
    }
    return L"MP4";
}

std::wstring MediaDownloadService::Mp4QualityLabel(Mp4Quality quality)
{
    switch (quality)
    {
    case Mp4Quality::Best:
        return L"Best available";
    case Mp4Quality::P4320:
        return L"4320p / 8K";
    case Mp4Quality::P2160:
        return L"2160p / 4K";
    case Mp4Quality::P1440:
        return L"1440p";
    case Mp4Quality::P1080:
        return L"1080p";
    case Mp4Quality::P720:
        return L"720p";
    case Mp4Quality::P480:
        return L"480p";
    }
    return L"Best available";
}

std::wstring MediaDownloadService::Mp3BitrateLabel(Mp3Bitrate bitrate)
{
    switch (bitrate)
    {
    case Mp3Bitrate::K320:
        return L"320 kbps";
    case Mp3Bitrate::K256:
        return L"256 kbps";
    case Mp3Bitrate::K192:
        return L"192 kbps";
    case Mp3Bitrate::K128:
        return L"128 kbps";
    }
    return L"320 kbps";
}

std::wstring MediaDownloadService::MediaTypeLabel(MediaType mediaType)
{
    switch (mediaType)
    {
    case MediaType::Video:
        return L"Video";
    case MediaType::Audio:
        return L"Audio";
    case MediaType::Unknown:
        break;
    }
    return L"Unknown";
}

std::wstring MediaDownloadService::StatusLabel(MediaDownloadStatus status)
{
    switch (status)
    {
    case MediaDownloadStatus::Idle:
        return L"Idle";
    case MediaDownloadStatus::Analyzing:
        return L"Analyzing";
    case MediaDownloadStatus::Ready:
        return L"Ready";
    case MediaDownloadStatus::Downloading:
        return L"Downloading";
    case MediaDownloadStatus::Converting:
        return L"Converting";
    case MediaDownloadStatus::Complete:
        return L"Complete";
    case MediaDownloadStatus::Failed:
        return L"Failed";
    case MediaDownloadStatus::Cancelled:
        return L"Cancelled";
    }
    return L"Idle";
}

std::wstring MediaDownloadService::SanitizeFileName(const std::wstring& value)
{
    std::wstring sanitized;
    for (wchar_t ch : value)
    {
        if (ch < 32 || wcschr(L"<>:\"/\\|?*", ch))
        {
            sanitized.push_back(L'_');
        }
        else
        {
            sanitized.push_back(ch);
        }
    }

    while (!sanitized.empty() && (sanitized.back() == L'.' || sanitized.back() == L' '))
    {
        sanitized.pop_back();
    }

    while (!sanitized.empty() && sanitized.front() == L' ')
    {
        sanitized.erase(sanitized.begin());
    }

    if (sanitized.empty())
    {
        sanitized = L"media";
    }

    if (sanitized.size() > 160)
    {
        sanitized.resize(160);
    }

    return sanitized;
}
