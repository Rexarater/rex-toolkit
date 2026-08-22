#include "EqualizerAtomicFile.h"
#include "EqualizerService.h"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cwctype>
#include <exception>
#include <fstream>
#include <iomanip>
#include <locale>
#include <limits>
#include <regex>
#include <sstream>
#include <system_error>

namespace rex::equalizer
{
namespace
{
using Json = nlohmann::json;

constexpr double kPi = 3.14159265358979323846;
constexpr size_t kMaximumProfileBytes = 1024 * 1024;
constexpr size_t kMaximumMeasurementBytes = 2 * 1024 * 1024;
constexpr size_t kMaximumMeasurementPoints = 20000;
constexpr size_t kMaximumFilters = 64;
constexpr wchar_t kAutoEqIndexUrl[] =
    L"https://raw.githubusercontent.com/jaakkopasanen/AutoEq/master/results/INDEX.md";
constexpr wchar_t kAutoEqResultsBaseUrl[] =
    L"https://raw.githubusercontent.com/jaakkopasanen/AutoEq/master/results/";

double ClampFinite(double value, double minimum, double maximum, double fallback = 0.0)
{
    return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
}

bool TryParseDouble(const std::string& text, double& value)
{
    try
    {
        size_t consumed = 0;
        const double parsed = std::stod(text, &consumed);
        if (consumed != text.size() || !std::isfinite(parsed)) return false;
        value = parsed;
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

std::wstring Trim(std::wstring value)
{
    const auto isSpace = [](wchar_t ch) { return std::iswspace(ch) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), isSpace));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), isSpace).base(), value.end());
    return value;
}

std::string Trim(std::string value)
{
    const auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), isSpace));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), isSpace).base(), value.end());
    return value;
}

std::wstring Lower(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

std::wstring SearchKey(const std::wstring& value)
{
    std::wstring key;
    key.reserve(value.size());
    for (wchar_t ch : Lower(value))
    {
        if (std::iswalnum(ch))
        {
            key.push_back(ch);
        }
    }
    return key;
}

std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty())
    {
        return {};
    }
    const int required = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0)
    {
        return {};
    }
    std::string result(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), required, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string& value)
{
    if (value.empty())
    {
        return {};
    }
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0)
    {
        return {};
    }
    std::wstring result(static_cast<size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), required);
    return result;
}

std::wstring ErrorFromCode(DWORD code)
{
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        0,
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);
    if (!length || !buffer)
    {
        return L"Windows error " + std::to_wstring(code);
    }
    std::wstring message(buffer, length);
    LocalFree(buffer);
    return Trim(message);
}

bool ReadBytes(const std::filesystem::path& path, size_t maximumBytes, std::string& bytes, std::wstring& errorMessage)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec)
    {
        errorMessage = L"Could not read the selected file.";
        return false;
    }
    if (size > maximumBytes)
    {
        errorMessage = L"The selected file is larger than the supported limit.";
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        errorMessage = L"Could not open the selected file.";
        return false;
    }
    bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF)
    {
        bytes.erase(0, 3);
    }
    return true;
}

bool WriteBytesAtomically(const std::filesystem::path& path, const std::string& bytes, std::wstring& errorMessage)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec)
    {
        errorMessage = L"Could not create the Equalizer data folder.";
        return false;
    }
    const std::filesystem::path temporary = path.wstring() + L".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            detail::RemoveTemporaryFile(temporary);
            errorMessage = L"Could not write the Equalizer data file.";
            return false;
        }
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        file.flush();
        if (!file)
        {
            file.close();
            detail::RemoveTemporaryFile(temporary);
            errorMessage = L"The Equalizer data file could not be written completely.";
            return false;
        }
    }

    DWORD replaceError = ERROR_SUCCESS;
    if (!detail::CommitTemporaryFile(temporary, path, replaceError))
    {
        errorMessage = L"Could not replace the Equalizer data file: " + ErrorFromCode(replaceError);
        return false;
    }
    return true;
}

bool HttpGet(const std::wstring& url, size_t maximumBytes, std::string& body, std::wstring& errorMessage)
{
    URL_COMPONENTSW components {};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &components))
    {
        errorMessage = L"The profile source URL is invalid.";
        return false;
    }
    if (components.nScheme != INTERNET_SCHEME_HTTPS)
    {
        errorMessage = L"Headphone profile downloads require HTTPS.";
        return false;
    }

    const std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength)
    {
        path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }

    HINTERNET session = WinHttpOpen(
        L"RexToolkitEqualizer/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!session)
    {
        errorMessage = L"Could not initialize the profile download.";
        return false;
    }
    WinHttpSetTimeouts(session, 7000, 7000, 12000, 12000);
    HINTERNET connection = WinHttpConnect(session, host.c_str(), components.nPort, 0);
    if (!connection)
    {
        errorMessage = L"Could not connect to the profile source.";
        WinHttpCloseHandle(session);
        return false;
    }
    const DWORD flags = components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(
        connection,
        L"GET",
        path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        flags);
    bool ok = request &&
        WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr);
    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (ok)
    {
        ok = WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status,
            &statusSize,
            WINHTTP_NO_HEADER_INDEX) && status >= 200 && status < 300;
    }
    body.clear();
    while (ok)
    {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available))
        {
            ok = false;
            break;
        }
        if (available == 0)
        {
            break;
        }
        if (body.size() + available > maximumBytes)
        {
            errorMessage = L"The downloaded profile data exceeded the supported size limit.";
            ok = false;
            break;
        }
        const size_t offset = body.size();
        body.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request, body.data() + offset, available, &read))
        {
            ok = false;
            break;
        }
        body.resize(offset + read);
    }
    if (!ok && errorMessage.empty())
    {
        errorMessage = status
            ? L"The profile source returned HTTP " + std::to_wstring(status) + L"."
            : L"Could not download headphone profile data: " + ErrorFromCode(GetLastError());
    }
    if (request) WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return ok;
}

std::string FilterTypeId(FilterType type)
{
    switch (type)
    {
    case FilterType::Peaking: return "peaking";
    case FilterType::LowShelf: return "low_shelf";
    case FilterType::HighShelf: return "high_shelf";
    case FilterType::LowPass: return "low_pass";
    case FilterType::HighPass: return "high_pass";
    case FilterType::Notch: return "notch";
    }
    return "peaking";
}

std::optional<FilterType> ParseFilterType(const std::string& value)
{
    if (value == "peaking") return FilterType::Peaking;
    if (value == "low_shelf") return FilterType::LowShelf;
    if (value == "high_shelf") return FilterType::HighShelf;
    if (value == "low_pass") return FilterType::LowPass;
    if (value == "high_pass") return FilterType::HighPass;
    if (value == "notch") return FilterType::Notch;
    return std::nullopt;
}

std::string EditorModeId(EditorMode mode)
{
    switch (mode)
    {
    case EditorMode::Simple: return "simple";
    case EditorMode::Graphic: return "graphic";
    case EditorMode::Parametric: return "parametric";
    }
    return "simple";
}

EditorMode ParseEditorMode(const std::string& value)
{
    if (value == "graphic") return EditorMode::Graphic;
    if (value == "parametric") return EditorMode::Parametric;
    return EditorMode::Simple;
}

Json ProfileToJson(const EqualizerProfile& profile)
{
    Json filters = Json::array();
    for (const auto& filter : profile.filters)
    {
        filters.push_back({
            { "id", filter.id },
            { "enabled", filter.enabled },
            { "type", FilterTypeId(filter.type) },
            { "frequencyHz", filter.frequencyHz },
            { "gainDb", filter.gainDb },
            { "q", filter.q }
        });
    }
    return {
        { "format", "rex-equalizer-profile" },
        { "schemaVersion", profile.schemaVersion },
        { "name", WideToUtf8(profile.name) },
        { "headphoneProfileId", WideToUtf8(profile.headphoneProfileId) },
        { "targetCurveId", WideToUtf8(profile.targetCurveId) },
        { "preferenceProfileId", WideToUtf8(profile.preferenceProfileId) },
        { "source", WideToUtf8(profile.source) },
        { "sourceUrl", WideToUtf8(profile.sourceUrl) },
        { "profileVersion", WideToUtf8(profile.profileVersion) },
        { "preampDb", profile.preampDb },
        { "automaticPreamp", profile.automaticPreamp },
        { "filters", filters }
    };
}

bool ProfileFromJson(const Json& json, EqualizerProfile& profile, std::wstring& errorMessage)
{
    try
    {
        if (!json.is_object() || json.value("format", std::string {}) != "rex-equalizer-profile")
        {
            errorMessage = L"This is not a Rex Equalizer profile.";
            return false;
        }
        const int version = json.value("schemaVersion", 0);
        if (version < 1 || version > 1)
        {
            errorMessage = L"This Equalizer profile version is not supported.";
            return false;
        }
        if (!json.contains("filters") || !json["filters"].is_array() || json["filters"].size() > kMaximumFilters)
        {
            errorMessage = L"The Equalizer profile has an invalid filter list.";
            return false;
        }
        EqualizerProfile parsed;
        parsed.schemaVersion = version;
        parsed.name = Utf8ToWide(json.value("name", std::string { "Imported profile" }));
        parsed.headphoneProfileId = Utf8ToWide(json.value("headphoneProfileId", std::string {}));
        parsed.targetCurveId = Utf8ToWide(json.value("targetCurveId", std::string { "balanced" }));
        parsed.preferenceProfileId = Utf8ToWide(json.value("preferenceProfileId", std::string { "custom" }));
        parsed.source = Utf8ToWide(json.value("source", std::string {}));
        parsed.sourceUrl = Utf8ToWide(json.value("sourceUrl", std::string {}));
        parsed.profileVersion = Utf8ToWide(json.value("profileVersion", std::string {}));
        parsed.preampDb = ClampFinite(json.value("preampDb", 0.0), -40.0, 12.0);
        parsed.automaticPreamp = json.value("automaticPreamp", true);
        for (const auto& item : json["filters"])
        {
            if (!item.is_object())
            {
                errorMessage = L"The Equalizer profile contains a malformed filter.";
                return false;
            }
            auto type = ParseFilterType(item.value("type", std::string {}));
            if (!type)
            {
                errorMessage = L"The Equalizer profile contains an unsupported filter type.";
                return false;
            }
            EqualizerFilter filter;
            filter.id = item.value("id", static_cast<int>(parsed.filters.size()) + 1);
            filter.enabled = item.value("enabled", true);
            filter.type = *type;
            filter.frequencyHz = item.value("frequencyHz", 1000.0);
            filter.gainDb = item.value("gainDb", 0.0);
            filter.q = item.value("q", 1.0);
            if (!AutoEqService::ValidateFilter(filter, errorMessage))
            {
                return false;
            }
            parsed.filters.push_back(filter);
        }
        if (parsed.name.empty() || parsed.name.size() > 160)
        {
            errorMessage = L"The Equalizer profile name is invalid.";
            return false;
        }
        profile = std::move(parsed);
        return true;
    }
    catch (const std::exception&)
    {
        errorMessage = L"The Equalizer profile could not be parsed.";
        return false;
    }
}

double InterpolateMeasurement(const std::vector<MeasurementPoint>& points, double frequency)
{
    if (points.empty()) return 0.0;
    if (frequency <= points.front().frequencyHz) return points.front().rawDb;
    if (frequency >= points.back().frequencyHz) return points.back().rawDb;
    const auto upper = std::lower_bound(points.begin(), points.end(), frequency, [](const MeasurementPoint& point, double value) {
        return point.frequencyHz < value;
    });
    const auto lower = upper - 1;
    const double logLow = std::log(lower->frequencyHz);
    const double logHigh = std::log(upper->frequencyHz);
    const double amount = (std::log(frequency) - logLow) / std::max(0.000001, logHigh - logLow);
    return lower->rawDb + (upper->rawDb - lower->rawDb) * amount;
}

double SmoothedMeasurement(const std::vector<MeasurementPoint>& points, double frequency)
{
    static constexpr std::array<double, 7> offsets { -0.18, -0.12, -0.06, 0.0, 0.06, 0.12, 0.18 };
    static constexpr std::array<double, 7> weights { 0.06, 0.12, 0.20, 0.24, 0.20, 0.12, 0.06 };
    double value = 0.0;
    for (size_t index = 0; index < offsets.size(); ++index)
    {
        value += InterpolateMeasurement(points, frequency * std::pow(2.0, offsets[index])) * weights[index];
    }
    return value;
}

std::wstring DecodeMarkdownLink(std::wstring value)
{
    for (size_t position = 0; (position = value.find(L"%20", position)) != std::wstring::npos;)
    {
        value.replace(position, 3, L" ");
        ++position;
    }
    return value;
}

std::wstring EncodeUrlPath(const std::wstring& value)
{
    static constexpr wchar_t hex[] = L"0123456789ABCDEF";
    const std::string utf8 = WideToUtf8(value);
    std::wstring result;
    result.reserve(utf8.size());
    for (unsigned char byte : utf8)
    {
        const bool unreserved =
            std::isalnum(byte) != 0 ||
            byte == '-' || byte == '_' || byte == '.' || byte == '~' ||
            byte == '/' || byte == '%';
        if (unreserved)
        {
            result.push_back(static_cast<wchar_t>(byte));
        }
        else
        {
            result.push_back(L'%');
            result.push_back(hex[(byte >> 4) & 0x0F]);
            result.push_back(hex[byte & 0x0F]);
        }
    }
    return result;
}

std::wstring ProfileCacheFileName(const std::wstring& value)
{
    std::wstring result;
    for (wchar_t ch : value)
    {
        if (std::iswalnum(ch) || ch == L'-' || ch == L'_') result.push_back(ch);
        else if (std::iswspace(ch)) result.push_back(L'_');
    }
    if (result.empty()) result = L"profile";
    if (result.size() > 64) result.resize(64);

    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : WideToUtf8(value))
    {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    std::wostringstream suffix;
    suffix << L'_' << std::hex << std::setw(16) << std::setfill(L'0') << hash << L".rexeq";
    return result + suffix.str();
}

bool ParseAutoEqIndexDocument(
    const std::string& document,
    const std::filesystem::path& cacheDirectory,
    std::vector<HeadphoneProfileSummary>& profiles,
    std::wstring& errorMessage)
{
    errorMessage.clear();
    profiles.clear();
    const bool recognizedHeading =
        document.find("# Index") != std::string::npos ||
        document.find("# Headphone Results") != std::string::npos;
    if (!recognizedHeading)
    {
        errorMessage = L"The headphone directory does not contain a recognized index heading.";
        return false;
    }

    std::istringstream stream(document);
    std::string line;
    while (std::getline(stream, line))
    {
        if (line.rfind("- [", 0) != 0) continue;
        const size_t titleEnd = line.find("](", 3);
        size_t pathEnd = titleEnd == std::string::npos
            ? std::string::npos
            : line.find(") by ", titleEnd + 2);
        if (pathEnd == std::string::npos && titleEnd != std::string::npos)
        {
            pathEnd = line.rfind(')');
        }
        if (titleEnd == std::string::npos || pathEnd == std::string::npos) continue;
        const std::wstring displayName = Utf8ToWide(line.substr(3, titleEnd - 3));
        std::wstring relativePath = Utf8ToWide(line.substr(titleEnd + 2, pathEnd - titleEnd - 2));
        if (relativePath.rfind(L"./", 0) == 0) relativePath.erase(0, 2);
        if (displayName.empty() ||
            relativePath.empty() ||
            relativePath.front() == L'/' ||
            relativePath.find(L"..") != std::wstring::npos ||
            relativePath.find(L"://") != std::wstring::npos)
        {
            continue;
        }
        const size_t by = line.find(" by ", pathEnd);
        std::wstring source;
        if (by != std::string::npos)
        {
            const size_t rig = line.find(" on ", by + 4);
            source = Utf8ToWide(line.substr(
                by + 4,
                rig == std::string::npos ? std::string::npos : rig - by - 4));
        }
        HeadphoneProfileSummary summary;
        summary.id = relativePath;
        const size_t firstSpace = displayName.find(L' ');
        summary.manufacturer = firstSpace == std::wstring::npos ? L"" : displayName.substr(0, firstSpace);
        summary.model = firstSpace == std::wstring::npos ? displayName : displayName.substr(firstSpace + 1);
        summary.measurementSource = source;
        summary.sourceUrl = L"https://github.com/jaakkopasanen/AutoEq/tree/master/results/" + EncodeUrlPath(relativePath);
        summary.recommendedEqAvailable = true;
        summary.cached = std::filesystem::exists(cacheDirectory / ProfileCacheFileName(relativePath));
        profiles.push_back(std::move(summary));
    }
    if (profiles.empty())
    {
        errorMessage = L"The headphone directory does not contain any usable profiles.";
        return false;
    }
    return true;
}

std::wstring PresetDisplayName(const std::wstring& presetId)
{
    if (presetId == L"bass_plus") return L"Bass+";
    if (presetId == L"bass_reduce") return L"Bass Reduce";
    if (presetId == L"warm") return L"Warm";
    if (presetId == L"bright") return L"Bright";
    if (presetId == L"music") return L"Music";
    if (presetId == L"movies") return L"Movies";
    if (presetId == L"voice") return L"Voice";
    if (presetId == L"gaming") return L"Gaming";
    if (presetId == L"footsteps") return L"Footsteps";
    if (presetId == L"late_night") return L"Late Night";
    if (presetId == L"custom") return L"Custom";
    return L"Balanced";
}

std::vector<EqualizerFilter> PreferenceFilters(const std::wstring& preset)
{
    const auto filter = [](FilterType type, double frequency, double gain, double q) {
        EqualizerFilter result;
        result.type = type;
        result.frequencyHz = frequency;
        result.gainDb = gain;
        result.q = q;
        return result;
    };
    if (preset == L"bass_plus") return { filter(FilterType::LowShelf, 95.0, 5.0, 0.70) };
    if (preset == L"bass_reduce") return { filter(FilterType::LowShelf, 105.0, -3.0, 0.70) };
    if (preset == L"warm") return { filter(FilterType::LowShelf, 180.0, 2.0, 0.65), filter(FilterType::HighShelf, 5500.0, -0.8, 0.70) };
    if (preset == L"bright") return { filter(FilterType::HighShelf, 4200.0, 2.0, 0.70) };
    if (preset == L"music") return { filter(FilterType::LowShelf, 90.0, 1.2, 0.70), filter(FilterType::HighShelf, 6000.0, 0.8, 0.70) };
    if (preset == L"movies") return { filter(FilterType::LowShelf, 90.0, 1.8, 0.70), filter(FilterType::Peaking, 2600.0, 1.0, 0.85) };
    if (preset == L"voice") return { filter(FilterType::LowShelf, 160.0, -2.0, 0.70), filter(FilterType::Peaking, 2300.0, 2.0, 0.90) };
    if (preset == L"gaming") return { filter(FilterType::LowShelf, 105.0, -1.0, 0.70), filter(FilterType::Peaking, 2800.0, 1.4, 0.85) };
    if (preset == L"footsteps") return { filter(FilterType::LowShelf, 120.0, -2.5, 0.70), filter(FilterType::Peaking, 3200.0, 2.0, 0.85) };
    if (preset == L"late_night") return { filter(FilterType::LowShelf, 120.0, -2.0, 0.70), filter(FilterType::HighShelf, 6500.0, -1.0, 0.70) };
    return {};
}
}

std::wstring HeadphoneProfileSummary::DisplayName() const
{
    std::wstring value = manufacturer;
    if (!value.empty() && !model.empty()) value += L" ";
    value += model;
    if (!variant.empty()) value += L" (" + variant + L")";
    return value.empty() ? L"Unnamed headphone" : value;
}

bool AutoEqService::ValidateFilter(const EqualizerFilter& filter, std::wstring& errorMessage)
{
    if (!std::isfinite(filter.frequencyHz) || filter.frequencyHz < 20.0 || filter.frequencyHz > 20000.0)
    {
        errorMessage = L"Filter frequencies must be between 20 Hz and 20 kHz.";
        return false;
    }
    if (!std::isfinite(filter.gainDb) || filter.gainDb < -20.0 || filter.gainDb > 20.0)
    {
        errorMessage = L"Filter gain must be between -20 dB and +20 dB.";
        return false;
    }
    if (!std::isfinite(filter.q) || filter.q < 0.1 || filter.q > 20.0)
    {
        errorMessage = L"Filter Q must be between 0.1 and 20.";
        return false;
    }
    return true;
}

double AutoEqService::FilterResponseDb(const EqualizerFilter& filter, double frequencyHz, double sampleRate)
{
    if (!filter.enabled || sampleRate <= 0.0 || frequencyHz <= 0.0 || frequencyHz >= sampleRate * 0.5)
    {
        return 0.0;
    }
    const double w0 = 2.0 * kPi * filter.frequencyHz / sampleRate;
    const double w = 2.0 * kPi * frequencyHz / sampleRate;
    const double cosine = std::cos(w0);
    const double sine = std::sin(w0);
    const double q = std::clamp(filter.q, 0.1, 20.0);
    const double alpha = sine / (2.0 * q);
    const double a = std::pow(10.0, filter.gainDb / 40.0);
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a0 = 1.0;
    double a1 = 0.0;
    double a2 = 0.0;
    switch (filter.type)
    {
    case FilterType::Peaking:
        b0 = 1.0 + alpha * a; b1 = -2.0 * cosine; b2 = 1.0 - alpha * a;
        a0 = 1.0 + alpha / a; a1 = -2.0 * cosine; a2 = 1.0 - alpha / a;
        break;
    case FilterType::LowShelf:
    {
        const double rootA = std::sqrt(a);
        const double shelfAlpha = sine / 2.0 * std::sqrt(std::max(0.0, (a + 1.0 / a) * (1.0 / q - 1.0) + 2.0));
        b0 = a * ((a + 1.0) - (a - 1.0) * cosine + 2.0 * rootA * shelfAlpha);
        b1 = 2.0 * a * ((a - 1.0) - (a + 1.0) * cosine);
        b2 = a * ((a + 1.0) - (a - 1.0) * cosine - 2.0 * rootA * shelfAlpha);
        a0 = (a + 1.0) + (a - 1.0) * cosine + 2.0 * rootA * shelfAlpha;
        a1 = -2.0 * ((a - 1.0) + (a + 1.0) * cosine);
        a2 = (a + 1.0) + (a - 1.0) * cosine - 2.0 * rootA * shelfAlpha;
        break;
    }
    case FilterType::HighShelf:
    {
        const double rootA = std::sqrt(a);
        const double shelfAlpha = sine / 2.0 * std::sqrt(std::max(0.0, (a + 1.0 / a) * (1.0 / q - 1.0) + 2.0));
        b0 = a * ((a + 1.0) + (a - 1.0) * cosine + 2.0 * rootA * shelfAlpha);
        b1 = -2.0 * a * ((a - 1.0) + (a + 1.0) * cosine);
        b2 = a * ((a + 1.0) + (a - 1.0) * cosine - 2.0 * rootA * shelfAlpha);
        a0 = (a + 1.0) - (a - 1.0) * cosine + 2.0 * rootA * shelfAlpha;
        a1 = 2.0 * ((a - 1.0) - (a + 1.0) * cosine);
        a2 = (a + 1.0) - (a - 1.0) * cosine - 2.0 * rootA * shelfAlpha;
        break;
    }
    case FilterType::LowPass:
        b0 = (1.0 - cosine) / 2.0; b1 = 1.0 - cosine; b2 = (1.0 - cosine) / 2.0;
        a0 = 1.0 + alpha; a1 = -2.0 * cosine; a2 = 1.0 - alpha;
        break;
    case FilterType::HighPass:
        b0 = (1.0 + cosine) / 2.0; b1 = -(1.0 + cosine); b2 = (1.0 + cosine) / 2.0;
        a0 = 1.0 + alpha; a1 = -2.0 * cosine; a2 = 1.0 - alpha;
        break;
    case FilterType::Notch:
        b0 = 1.0; b1 = -2.0 * cosine; b2 = 1.0;
        a0 = 1.0 + alpha; a1 = -2.0 * cosine; a2 = 1.0 - alpha;
        break;
    }
    if (std::abs(a0) < 1e-12) return 0.0;
    b0 /= a0; b1 /= a0; b2 /= a0; a1 /= a0; a2 /= a0;
    const double cosW = std::cos(w);
    const double sinW = std::sin(w);
    const double cos2W = std::cos(2.0 * w);
    const double sin2W = std::sin(2.0 * w);
    const double numeratorReal = b0 + b1 * cosW + b2 * cos2W;
    const double numeratorImag = -b1 * sinW - b2 * sin2W;
    const double denominatorReal = 1.0 + a1 * cosW + a2 * cos2W;
    const double denominatorImag = -a1 * sinW - a2 * sin2W;
    const double numeratorPower = numeratorReal * numeratorReal + numeratorImag * numeratorImag;
    const double denominatorPower = denominatorReal * denominatorReal + denominatorImag * denominatorImag;
    if (numeratorPower <= 0.0 || denominatorPower <= 0.0) return -120.0;
    return 10.0 * std::log10(numeratorPower / denominatorPower);
}

double AutoEqService::ProfileResponseDb(const EqualizerProfile& profile, double frequencyHz, double sampleRate)
{
    double response = profile.preampDb;
    for (const auto& filter : profile.filters)
    {
        response += FilterResponseDb(filter, frequencyHz, sampleRate);
    }
    return response;
}

double AutoEqService::CalculateAutomaticPreamp(const std::vector<EqualizerFilter>& filters, double sampleRate)
{
    const double effectiveSampleRate = std::isfinite(sampleRate) && sampleRate > 0.0
        ? sampleRate
        : 48000.0;
    constexpr double minimumFrequency = 20.0;
    const double maximumFrequency = std::min(20000.0, effectiveSampleRate * 0.5 * 0.999);
    if (maximumFrequency <= minimumFrequency) return 0.0;

    const auto responseAt = [&](double frequency)
    {
        double response = 0.0;
        for (const auto& filter : filters)
        {
            response += FilterResponseDb(filter, frequency, effectiveSampleRate);
        }
        return response;
    };

    // A log grid catches the broad response while exact filter centers and a
    // local refinement pass prevent narrow, high-Q peaks from falling between
    // samples. This keeps the reserved headroom close to the real peak.
    constexpr int coarseSampleCount = 1024;
    const double logMinimum = std::log(minimumFrequency);
    const double logMaximum = std::log(maximumFrequency);
    std::vector<double> frequencies;
    frequencies.reserve(coarseSampleCount + filters.size());
    for (int index = 0; index < coarseSampleCount; ++index)
    {
        const double ratio = static_cast<double>(index) /
            static_cast<double>(coarseSampleCount - 1);
        frequencies.push_back(std::exp(logMinimum + (logMaximum - logMinimum) * ratio));
    }
    for (const auto& filter : filters)
    {
        if (filter.enabled && std::isfinite(filter.frequencyHz) &&
            filter.frequencyHz >= minimumFrequency && filter.frequencyHz <= maximumFrequency)
        {
            frequencies.push_back(filter.frequencyHz);
        }
    }
    std::sort(frequencies.begin(), frequencies.end());
    frequencies.erase(std::unique(frequencies.begin(), frequencies.end()), frequencies.end());

    std::vector<double> responses;
    responses.reserve(frequencies.size());
    double maximum = 0.0;
    for (const double frequency : frequencies)
    {
        const double response = responseAt(frequency);
        responses.push_back(response);
        maximum = std::max(maximum, response);
    }

    for (size_t index = 1; index + 1 < frequencies.size(); ++index)
    {
        if (responses[index] <= 0.0 ||
            responses[index] < responses[index - 1] ||
            responses[index] < responses[index + 1] ||
            (responses[index] == responses[index - 1] &&
                responses[index] == responses[index + 1]))
        {
            continue;
        }
        double low = std::log(frequencies[index - 1]);
        double high = std::log(frequencies[index + 1]);
        for (int iteration = 0; iteration < 18; ++iteration)
        {
            const double third = (high - low) / 3.0;
            const double left = low + third;
            const double right = high - third;
            if (responseAt(std::exp(left)) < responseAt(std::exp(right))) low = left;
            else high = right;
        }
        maximum = std::max(maximum, responseAt(std::exp((low + high) * 0.5)));
    }

    constexpr double safetyMarginDb = 0.05;
    return maximum > 0.0 ? -maximum - safetyMarginDb : 0.0;
}

EqualizerProfile AutoEqService::Compose(
    const std::optional<EqualizerProfile>& headphoneCorrection,
    const DeviceEqualizerSettings& settings,
    double sampleRate) const
{
    EqualizerProfile result = headphoneCorrection.value_or(EqualizerProfile {});
    result.name = PresetDisplayName(settings.soundPreset);
    result.preferenceProfileId = settings.soundPreset;
    result.automaticPreamp = settings.automaticPreamp;

    // Parametric mode edits a replacement copy of the measured correction. This
    // keeps the recommended profile available for reset without applying it twice.
    if (settings.editorMode == EditorMode::Parametric && settings.parametricOverrideActive)
    {
        result.filters.clear();
        if (!settings.customProfileName.empty()) result.name = settings.customProfileName;
        result.source = settings.customProfileSource;
        result.sourceUrl = settings.customProfileSourceUrl;
        result.profileVersion = settings.customProfileVersion;
        if (!settings.customTargetCurveId.empty()) result.targetCurveId = settings.customTargetCurveId;
    }

    auto append = [&](EqualizerFilter filter) {
        filter.id = static_cast<int>(result.filters.size()) + 1;
        result.filters.push_back(filter);
    };
    if (settings.editorMode == EditorMode::Parametric && settings.parametricOverrideActive)
    {
        for (auto filter : settings.customFilters)
        {
            std::wstring ignored;
            if (ValidateFilter(filter, ignored)) append(filter);
        }
    }

    for (auto filter : PreferenceFilters(settings.soundPreset)) append(filter);

    const auto addPreference = [&](FilterType type, double frequency, double gain, double q) {
        if (std::abs(gain) < 0.01) return;
        EqualizerFilter filter;
        filter.type = type;
        filter.frequencyHz = frequency;
        filter.gainDb = std::clamp(gain, -6.0, 6.0);
        filter.q = q;
        append(filter);
    };
    addPreference(FilterType::LowShelf, 95.0, settings.bassDb, 0.70);
    addPreference(FilterType::Peaking, 280.0, settings.warmthDb, 0.75);
    addPreference(FilterType::Peaking, 2800.0, settings.presenceDb, 0.85);
    addPreference(FilterType::HighShelf, 6000.0, settings.trebleDb, 0.70);

    if (settings.editorMode == EditorMode::Graphic)
    {
        static constexpr std::array<double, 10> frequencies { 31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };
        for (size_t index = 0; index < frequencies.size(); ++index)
        {
            if (std::abs(settings.graphicGains[index]) < 0.01) continue;
            EqualizerFilter filter;
            filter.type = FilterType::Peaking;
            filter.frequencyHz = frequencies[index];
            filter.gainDb = std::clamp(settings.graphicGains[index], -12.0, 12.0);
            filter.q = 1.41;
            append(filter);
        }
    }
    const double effectiveSampleRate = std::isfinite(sampleRate) && sampleRate >= 8000.0
        ? sampleRate
        : 48000.0;
    result.preampDb = settings.preventClipping && settings.automaticPreamp
        ? CalculateAutomaticPreamp(result.filters, effectiveSampleRate)
        : std::clamp(settings.manualPreampDb, -30.0, 6.0);
    return result;
}
EqualizerProfile AutoEqService::OptimizeMeasurement(
    const std::wstring& name,
    const std::vector<MeasurementPoint>& measurement,
    std::wstring& errorMessage) const
{
    if (measurement.size() < 20)
    {
        errorMessage = L"At least 20 valid measurement points are required.";
        return {};
    }
    static constexpr std::array<double, 10> frequencies { 31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };
    const double reference = SmoothedMeasurement(measurement, 1000.0);
    EqualizerProfile profile;
    profile.name = name.empty() ? L"Custom measurement" : name;
    profile.preferenceProfileId = L"balanced";
    profile.targetCurveId = L"balanced";
    profile.source = L"Imported measurement; native Rex AutoEq-style optimizer";
    profile.profileVersion = L"native-1";
    int id = 1;
    for (double frequency : frequencies)
    {
        EqualizerFilter filter;
        filter.id = id++;
        filter.type = FilterType::Peaking;
        filter.frequencyHz = frequency;
        filter.gainDb = std::clamp(-(SmoothedMeasurement(measurement, frequency) - reference), -12.0, 6.0);
        filter.q = 1.41;
        profile.filters.push_back(filter);
    }
    profile.preampDb = CalculateAutomaticPreamp(profile.filters);
    profile.automaticPreamp = true;
    return profile;
}

bool AutoEqService::ParseMeasurementFile(
    const std::filesystem::path& path,
    std::vector<MeasurementPoint>& points,
    std::wstring& errorMessage)
{
    std::string bytes;
    if (!ReadBytes(path, kMaximumMeasurementBytes, bytes, errorMessage)) return false;
    points.clear();
    std::istringstream stream(bytes);
    std::string line;
    size_t lineNumber = 0;
    double previousFrequency = 0.0;
    while (std::getline(stream, line))
    {
        ++lineNumber;
        line = Trim(line);
        if (line.empty() || line[0] == '#') continue;
        for (char& ch : line)
        {
            if (ch == ';' || ch == '\t') ch = ',';
        }
        const size_t comma = line.find(',');
        std::string first;
        std::string second;
        if (comma != std::string::npos)
        {
            first = Trim(line.substr(0, comma));
            second = Trim(line.substr(comma + 1));
        }
        else
        {
            std::istringstream row(line);
            row >> first >> second;
        }
        try
        {
            size_t usedFrequency = 0;
            size_t usedRaw = 0;
            const double frequency = std::stod(first, &usedFrequency);
            const double raw = std::stod(second, &usedRaw);
            if (usedFrequency != first.size() || usedRaw != second.size() || !std::isfinite(frequency) || !std::isfinite(raw))
            {
                throw std::invalid_argument("invalid");
            }
            if (frequency <= previousFrequency)
            {
                errorMessage = L"Measurement frequencies must be strictly increasing (line " + std::to_wstring(lineNumber) + L").";
                return false;
            }
            previousFrequency = frequency;
            if (frequency >= 20.0 && frequency <= 20000.0)
            {
                if (raw < -100.0 || raw > 100.0)
                {
                    errorMessage = L"Measurement values must be between -100 dB and +100 dB.";
                    return false;
                }
                points.push_back({ frequency, raw });
                if (points.size() > kMaximumMeasurementPoints)
                {
                    errorMessage = L"The measurement contains too many points.";
                    return false;
                }
            }
        }
        catch (...)
        {
            const std::wstring lowered = Lower(Utf8ToWide(line));
            if (lineNumber == 1 && lowered.find(L"frequency") != std::wstring::npos) continue;
            errorMessage = L"Could not parse measurement line " + std::to_wstring(lineNumber) + L".";
            return false;
        }
    }
    if (points.size() < 20 || points.front().frequencyHz > 40.0 || points.back().frequencyHz < 12000.0)
    {
        errorMessage = L"The measurement needs at least 20 points covering roughly 20 Hz through 20 kHz.";
        points.clear();
        return false;
    }
    return true;
}

bool HeadphoneProfileService::Initialize(const std::filesystem::path& root, std::wstring& errorMessage)
{
    root_ = root;
    profilesDirectory_ = root_ / L"profiles";
    cacheDirectory_ = root_ / L"cache";
    indexPath_ = profilesDirectory_ / L"autoeq_index.md";
    std::error_code ec;
    std::filesystem::create_directories(profilesDirectory_, ec);
    std::filesystem::create_directories(cacheDirectory_, ec);
    std::filesystem::create_directories(root_ / L"custom", ec);
    if (ec)
    {
        errorMessage = L"Could not create the headphone profile folders.";
        return false;
    }
    return LoadIndex(errorMessage);
}

const std::vector<HeadphoneProfileSummary>& HeadphoneProfileService::Profiles() const
{
    return profiles_;
}

bool HeadphoneProfileService::LoadIndex(std::wstring& errorMessage)
{
    std::vector<HeadphoneProfileSummary> loadedProfiles;
    std::wstring loadedDatabaseVersion = L"Not downloaded";
    std::error_code indexEc;
    if (std::filesystem::exists(indexPath_))
    {
        std::string bytes;
        if (!ReadBytes(indexPath_, 8 * 1024 * 1024, bytes, errorMessage)) return false;
        if (!ParseAutoEqIndexDocument(bytes, cacheDirectory_, loadedProfiles, errorMessage)) return false;
        const auto modified = std::filesystem::last_write_time(indexPath_, indexEc);
        if (!indexEc)
        {
            const auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                modified - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
            const std::time_t time = std::chrono::system_clock::to_time_t(systemTime);
            std::tm local {};
            localtime_s(&local, &time);
            wchar_t date[32] {};
            wcsftime(date, std::size(date), L"%Y-%m-%d", &local);
            loadedDatabaseVersion = date;
        }
    }

    const std::filesystem::path customDirectory = root_ / L"custom";
    std::error_code ec;
    for (std::filesystem::directory_iterator iterator(customDirectory, ec), end; !ec && iterator != end; iterator.increment(ec))
    {
        if (!iterator->is_regular_file() || Lower(iterator->path().extension().wstring()) != L".rexeq") continue;
        EqualizerProfile profile;
        std::wstring ignored;
        if (!ImportProfile(iterator->path(), profile, ignored)) continue;
        HeadphoneProfileSummary summary;
        summary.id = profile.headphoneProfileId.empty() ? L"custom:" + iterator->path().stem().wstring() : profile.headphoneProfileId;
        summary.manufacturer = L"Custom";
        summary.model = profile.name;
        summary.measurementSource = profile.source;
        summary.cached = true;
        summary.recommendedEqAvailable = true;
        loadedProfiles.push_back(std::move(summary));
    }
    profiles_ = std::move(loadedProfiles);
    databaseVersion_ = std::move(loadedDatabaseVersion);
    return true;
}

std::vector<HeadphoneProfileSummary> HeadphoneProfileService::Search(const std::wstring& query, size_t limit) const
{
    const std::wstring key = SearchKey(query);
    struct RankedProfile
    {
        int score = 0;
        bool cached = false;
        const HeadphoneProfileSummary* profile = nullptr;
    };
    std::vector<RankedProfile> ranked;
    if (key.empty()) return {};
    ranked.reserve(std::min<size_t>(profiles_.size(), 512));
    for (const auto& profile : profiles_)
    {
        const std::wstring candidate = SearchKey(profile.DisplayName());
        const size_t position = candidate.find(key);
        if (position == std::wstring::npos) continue;
        int score = position == 0 ? 0 : 100 + static_cast<int>(position);
        score += static_cast<int>(candidate.size() - key.size());
        bool cached = profile.cached;
        if (!cached && profile.id.rfind(L"custom:", 0) != 0)
        {
            std::error_code ec;
            cached = std::filesystem::exists(
                cacheDirectory_ / ProfileCacheFileName(profile.id), ec) && !ec;
        }
        ranked.push_back({ score, cached, &profile });
    }
    std::partial_sort(ranked.begin(), ranked.begin() + std::min(limit, ranked.size()), ranked.end(), [](const auto& left, const auto& right) {
        if (left.cached != right.cached) return left.cached;
        if (left.score != right.score) return left.score < right.score;
        return left.profile->DisplayName() < right.profile->DisplayName();
    });
    std::vector<HeadphoneProfileSummary> results;
    for (size_t index = 0; index < std::min(limit, ranked.size()); ++index)
    {
        HeadphoneProfileSummary result = *ranked[index].profile;
        result.cached = ranked[index].cached;
        results.push_back(std::move(result));
    }
    return results;
}

bool HeadphoneProfileService::UpdateAutoEqIndex(std::wstring& errorMessage)
{
    std::string body;
    if (!HttpGet(kAutoEqIndexUrl, 8 * 1024 * 1024, body, errorMessage)) return false;
    std::vector<HeadphoneProfileSummary> downloadedProfiles;
    if (!ParseAutoEqIndexDocument(body, cacheDirectory_, downloadedProfiles, errorMessage) ||
        body.find("# Index") == std::string::npos ||
        downloadedProfiles.size() < 100)
    {
        if (errorMessage.empty())
        {
            errorMessage = L"The downloaded headphone directory is incomplete or uses an unsupported format.";
        }
        return false;
    }
    if (!WriteBytesAtomically(indexPath_, body, errorMessage)) return false;
    return LoadIndex(errorMessage);
}

bool HeadphoneProfileService::LoadCachedProfile(
    const HeadphoneProfileSummary& summary,
    EqualizerProfile& profile,
    std::wstring& errorMessage) const
{
    if (summary.id.rfind(L"custom:", 0) == 0)
    {
        const std::filesystem::path customDirectory = root_ / L"custom";
        std::error_code ec;
        for (std::filesystem::directory_iterator iterator(customDirectory, ec), end; !ec && iterator != end; iterator.increment(ec))
        {
            if (!iterator->is_regular_file() || Lower(iterator->path().extension().wstring()) != L".rexeq") continue;
            EqualizerProfile candidate;
            std::wstring ignored;
            if (ImportProfile(iterator->path(), candidate, ignored) && candidate.headphoneProfileId == summary.id)
            {
                profile = std::move(candidate);
                return true;
            }
        }
        errorMessage = L"The custom headphone profile file is missing.";
        return false;
    }
    const std::filesystem::path path = cacheDirectory_ / ProfileCacheFileName(summary.id);
    if (!ImportProfile(path, profile, errorMessage)) return false;
    if (profile.headphoneProfileId != summary.id)
    {
        profile = {};
        errorMessage = L"The cached headphone profile does not match the selected directory entry.";
        return false;
    }
    return true;
}

bool HeadphoneProfileService::SaveCachedProfile(
    const HeadphoneProfileSummary& summary,
    const EqualizerProfile& profile,
    std::wstring& errorMessage) const
{
    const std::filesystem::path path = cacheDirectory_ / ProfileCacheFileName(summary.id);
    return ExportProfile(path, profile, errorMessage);
}

bool HeadphoneProfileService::ParseAutoEqResult(
    const std::string& document,
    const HeadphoneProfileSummary& summary,
    const std::wstring& databaseVersion,
    EqualizerProfile& profile,
    std::wstring& errorMessage)
{
    const size_t sectionStart = document.find("### Parametric EQs");
    if (sectionStart == std::string::npos)
    {
        errorMessage = L"AutoEq did not provide a parametric profile for this entry.";
        return false;
    }
    size_t sectionEnd = document.find("\n### ", sectionStart + 4);
    if (sectionEnd == std::string::npos) sectionEnd = document.size();
    const std::string section = document.substr(sectionStart, sectionEnd - sectionStart);

    std::regex preampExpression(
        R"(preamp of \*{0,2}(-?[0-9]+(?:\.[0-9]+)?)\s*dB)",
        std::regex::icase);
    std::smatch preampMatch;
    double suppliedPreamp = 0.0;
    const bool hasSuppliedPreamp =
        std::regex_search(section, preampMatch, preampExpression);
    if (hasSuppliedPreamp && !TryParseDouble(preampMatch[1].str(), suppliedPreamp))
    {
        errorMessage = L"AutoEq supplied an invalid preamp value for this profile.";
        return false;
    }

    EqualizerProfile parsed;
    parsed.name = summary.DisplayName() + L" Recommended";
    parsed.headphoneProfileId = summary.id;
    parsed.targetCurveId = L"autoeq_recommended";
    parsed.preferenceProfileId = L"balanced";
    parsed.source = L"AutoEq result";
    if (!summary.measurementSource.empty())
    {
        parsed.source += L" from " + summary.measurementSource;
    }
    parsed.sourceUrl = summary.sourceUrl;
    parsed.profileVersion = databaseVersion;

    // Support both AutoEq's numbered table and compatible four-column PEQ tables.
    std::regex filterExpression(
        R"(\|\s*(?:([0-9]+)\s*\|\s*)?(Peaking|LowShelf|HighShelf)\s*\|\s*([0-9]+(?:\.[0-9]+)?)\s*(?:Hz)?\s*\|\s*([0-9]+(?:\.[0-9]+)?)\s*\|\s*(-?[0-9]+(?:\.[0-9]+)?)\s*(?:dB)?\s*\|)",
        std::regex::icase);
    for (std::sregex_iterator filter(section.begin(), section.end(), filterExpression), end;
         filter != end && parsed.filters.size() < 10;
         ++filter)
    {
        EqualizerFilter value;
        value.id = static_cast<int>(parsed.filters.size()) + 1;
        std::string type = (*filter)[2].str();
        std::transform(type.begin(), type.end(), type.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        value.type = type == "lowshelf"
            ? FilterType::LowShelf
            : type == "highshelf"
                ? FilterType::HighShelf
                : FilterType::Peaking;
        if (!TryParseDouble((*filter)[3].str(), value.frequencyHz) ||
            !TryParseDouble((*filter)[4].str(), value.q) ||
            !TryParseDouble((*filter)[5].str(), value.gainDb))
        {
            errorMessage = L"AutoEq supplied an invalid numeric filter value for this profile.";
            return false;
        }
        if (!AutoEqService::ValidateFilter(value, errorMessage)) return false;
        parsed.filters.push_back(value);
    }
    if (parsed.filters.empty())
    {
        errorMessage = L"AutoEq did not provide a usable parametric profile for this entry.";
        return false;
    }

    parsed.automaticPreamp = true;
    const double calculatedPreamp =
        AutoEqService::CalculateAutomaticPreamp(parsed.filters);
    parsed.preampDb = hasSuppliedPreamp
        ? std::min(suppliedPreamp, calculatedPreamp)
        : calculatedPreamp;
    profile = std::move(parsed);
    errorMessage.clear();
    return true;
}

bool HeadphoneProfileService::ResolveCachedProfile(
    const std::wstring& id,
    EqualizerProfile& profile,
    std::wstring& errorMessage) const
{
    profile = {};
    const auto iterator = std::find_if(
        profiles_.begin(), profiles_.end(),
        [&](const auto& candidate) { return candidate.id == id; });
    if (iterator == profiles_.end())
    {
        errorMessage = L"This headphone profile is no longer in the local directory.";
        return false;
    }

    std::wstring loadError;
    if (LoadCachedProfile(*iterator, profile, loadError))
    {
        errorMessage.clear();
        return true;
    }

    errorMessage = iterator->cached && !loadError.empty()
        ? loadError
        : L"This headphone profile has not been downloaded yet.";
    return false;
}

bool HeadphoneProfileService::ResolveProfile(const std::wstring& id, EqualizerProfile& profile, std::wstring& errorMessage)
{
    const auto iterator = std::find_if(profiles_.begin(), profiles_.end(), [&](const auto& candidate) { return candidate.id == id; });
    if (iterator == profiles_.end())
    {
        errorMessage = L"This headphone profile is no longer in the local directory.";
        return false;
    }
    if (iterator->cached && LoadCachedProfile(*iterator, profile, errorMessage)) return true;
    if (id.rfind(L"custom:", 0) == 0) return false;

    const std::wstring url =
        kAutoEqResultsBaseUrl + EncodeUrlPath(iterator->id) + L"/README.md";
    std::string body;
    if (!HttpGet(url, kMaximumProfileBytes, body, errorMessage)) return false;

    EqualizerProfile parsed;
    if (!ParseAutoEqResult(body, *iterator, databaseVersion_, parsed, errorMessage))
    {
        return false;
    }
    if (!SaveCachedProfile(*iterator, parsed, errorMessage)) return false;
    iterator->cached = true;
    profile = std::move(parsed);
    return true;
}
bool HeadphoneProfileService::ImportProfile(
    const std::filesystem::path& path,
    EqualizerProfile& profile,
    std::wstring& errorMessage) const
{
    std::string bytes;
    if (!ReadBytes(path, kMaximumProfileBytes, bytes, errorMessage)) return false;
    try
    {
        return ProfileFromJson(Json::parse(bytes), profile, errorMessage);
    }
    catch (const std::exception&)
    {
        errorMessage = L"The Equalizer profile is not valid JSON.";
        return false;
    }
}

bool HeadphoneProfileService::ExportProfile(
    const std::filesystem::path& path,
    const EqualizerProfile& profile,
    std::wstring& errorMessage) const
{
    std::wstring validation;
    if (profile.filters.size() > kMaximumFilters)
    {
        errorMessage = L"The Equalizer profile contains too many filters.";
        return false;
    }
    for (const auto& filter : profile.filters)
    {
        if (!AutoEqService::ValidateFilter(filter, validation))
        {
            errorMessage = validation;
            return false;
        }
    }
    return WriteBytesAtomically(path, ProfileToJson(profile).dump(2), errorMessage);
}

std::wstring HeadphoneProfileService::DatabaseVersion() const
{
    return databaseVersion_;
}

std::filesystem::path HeadphoneProfileService::ProfileDirectory() const
{
    return profilesDirectory_;
}

EqualizerSettingsRepository::EqualizerSettingsRepository(std::filesystem::path path)
    : path_(std::move(path))
{
}

bool EqualizerSettingsRepository::Load(EqualizerSettings& settings, std::wstring& errorMessage) const
{
    if (!std::filesystem::exists(path_)) return true;
    std::string bytes;
    if (!ReadBytes(path_, kMaximumProfileBytes, bytes, errorMessage)) return false;
    try
    {
        const Json json = Json::parse(bytes);
        if (!json.is_object() || json.value("schemaVersion", 0) != 1)
        {
            errorMessage = L"The saved Equalizer settings use an unsupported version.";
            return false;
        }
        EqualizerSettings loaded;
        loaded.schemaVersion = 1;
        loaded.followWindowsDefault = json.value("followWindowsDefault", true);
        loaded.selectedOutputId = Utf8ToWide(json.value("selectedOutputId", WideToUtf8(kFollowDefaultOutputId)));
        loaded.rememberPerDevice = json.value("rememberPerDevice", true);
        loaded.automaticallyApplyDeviceProfile = json.value("automaticallyApplyDeviceProfile", true);
        loaded.enableOnStartup = json.value("enableOnStartup", false);
        loaded.trayControlsEnabled = json.value("trayControlsEnabled", true);
        loaded.globalHotkeysEnabled = json.value("globalHotkeysEnabled", false);
        loaded.showTechnicalControls = json.value("showTechnicalControls", false);
        loaded.advancedVisible = json.value("advancedVisible", false);
        loaded.maximumPeqFilters = std::clamp(json.value("maximumPeqFilters", 10), 1, 32);
        loaded.defaultSoundPreset = Utf8ToWide(json.value("defaultSoundPreset", std::string { "balanced" }));
        if (json.contains("deviceProfiles") && json["deviceProfiles"].is_object())
        {
            size_t count = 0;
            for (auto iterator = json["deviceProfiles"].begin(); iterator != json["deviceProfiles"].end() && count < 128; ++iterator, ++count)
            {
                if (!iterator.value().is_object()) continue;
                const auto& item = iterator.value();
                DeviceEqualizerSettings device;
                device.enabled = item.value("enabled", false);
                device.headphoneProfileId = Utf8ToWide(item.value("headphoneProfileId", std::string {}));
                device.headphoneDisplayName = Utf8ToWide(item.value("headphoneDisplayName", std::string { "No headphone correction" }));
                device.soundPreset = Utf8ToWide(item.value("soundPreset", std::string { "balanced" }));
                device.bassDb = ClampFinite(item.value("bassDb", 0.0), -6.0, 6.0);
                device.warmthDb = ClampFinite(item.value("warmthDb", 0.0), -6.0, 6.0);
                device.presenceDb = ClampFinite(item.value("presenceDb", 0.0), -6.0, 6.0);
                device.trebleDb = ClampFinite(item.value("trebleDb", 0.0), -6.0, 6.0);
                device.preventClipping = item.value("preventClipping", false);
                device.automaticPreamp = item.value("automaticPreamp", true);
                device.manualPreampDb = ClampFinite(item.value("manualPreampDb", 0.0), -30.0, 6.0);
                device.editorMode = ParseEditorMode(item.value("editorMode", std::string { "simple" }));
                device.customProfileName = Utf8ToWide(item.value("customProfileName", std::string {}));
                device.customProfileSource = Utf8ToWide(item.value("customProfileSource", std::string {}));
                device.customProfileSourceUrl = Utf8ToWide(item.value("customProfileSourceUrl", std::string {}));
                device.customProfileVersion = Utf8ToWide(item.value("customProfileVersion", std::string {}));
                device.customTargetCurveId = Utf8ToWide(item.value("customTargetCurveId", std::string {}));
                if (item.contains("graphicGains") && item["graphicGains"].is_array())
                {
                    for (size_t index = 0; index < std::min<size_t>(10, item["graphicGains"].size()); ++index)
                    {
                        device.graphicGains[index] = ClampFinite(item["graphicGains"][index].get<double>(), -12.0, 12.0);
                    }
                }
                if (item.contains("customFilters") && item["customFilters"].is_array())
                {
                    for (const auto& filterJson : item["customFilters"])
                    {
                        if (device.customFilters.size() >= kMaximumFilters || !filterJson.is_object()) break;
                        auto type = ParseFilterType(filterJson.value("type", std::string {}));
                        if (!type) continue;
                        EqualizerFilter filter;
                        filter.id = filterJson.value("id", static_cast<int>(device.customFilters.size()) + 1);
                        filter.enabled = filterJson.value("enabled", true);
                        filter.type = *type;
                        filter.frequencyHz = filterJson.value("frequencyHz", 1000.0);
                        filter.gainDb = filterJson.value("gainDb", 0.0);
                        filter.q = filterJson.value("q", 1.0);
                        std::wstring ignored;
                        if (AutoEqService::ValidateFilter(filter, ignored)) device.customFilters.push_back(filter);
                    }
                }
                device.parametricOverrideActive = item.contains("parametricOverrideActive")
                    ? item.value("parametricOverrideActive", false)
                    : !device.customFilters.empty();
                loaded.deviceProfiles[Utf8ToWide(iterator.key())] = std::move(device);
            }
        }
        settings = std::move(loaded);
        return true;
    }
    catch (const std::exception&)
    {
        errorMessage = L"The saved Equalizer settings could not be parsed.";
        return false;
    }
}

bool EqualizerSettingsRepository::Save(const EqualizerSettings& settings, std::wstring& errorMessage) const
{
    Json devices = Json::object();
    for (const auto& [id, device] : settings.deviceProfiles)
    {
        Json filters = Json::array();
        for (const auto& filter : device.customFilters)
        {
            filters.push_back({
                { "id", filter.id }, { "enabled", filter.enabled }, { "type", FilterTypeId(filter.type) },
                { "frequencyHz", filter.frequencyHz }, { "gainDb", filter.gainDb }, { "q", filter.q }
            });
        }
        Json gains = Json::array();
        for (double gain : device.graphicGains) gains.push_back(gain);
        devices[WideToUtf8(id)] = {
            { "enabled", device.enabled },
            { "headphoneProfileId", WideToUtf8(device.headphoneProfileId) },
            { "headphoneDisplayName", WideToUtf8(device.headphoneDisplayName) },
            { "soundPreset", WideToUtf8(device.soundPreset) },
            { "bassDb", device.bassDb }, { "warmthDb", device.warmthDb },
            { "presenceDb", device.presenceDb }, { "trebleDb", device.trebleDb },
            { "preventClipping", device.preventClipping },
            { "automaticPreamp", device.automaticPreamp }, { "manualPreampDb", device.manualPreampDb },
            { "editorMode", EditorModeId(device.editorMode) },
            { "customProfileName", WideToUtf8(device.customProfileName) },
            { "customProfileSource", WideToUtf8(device.customProfileSource) },
            { "customProfileSourceUrl", WideToUtf8(device.customProfileSourceUrl) },
            { "customProfileVersion", WideToUtf8(device.customProfileVersion) },
            { "customTargetCurveId", WideToUtf8(device.customTargetCurveId) },
            { "graphicGains", gains }, { "parametricOverrideActive", device.parametricOverrideActive }, { "customFilters", filters }
        };
    }
    const Json json = {
        { "schemaVersion", 1 },
        { "followWindowsDefault", settings.followWindowsDefault },
        { "selectedOutputId", WideToUtf8(settings.selectedOutputId) },
        { "rememberPerDevice", settings.rememberPerDevice },
        { "automaticallyApplyDeviceProfile", settings.automaticallyApplyDeviceProfile },
        { "enableOnStartup", settings.enableOnStartup },
        { "trayControlsEnabled", settings.trayControlsEnabled },
        { "globalHotkeysEnabled", settings.globalHotkeysEnabled },
        { "showTechnicalControls", settings.showTechnicalControls },
        { "advancedVisible", settings.advancedVisible },
        { "maximumPeqFilters", settings.maximumPeqFilters },
        { "defaultSoundPreset", WideToUtf8(settings.defaultSoundPreset) },
        { "deviceProfiles", devices }
    };
    return WriteBytesAtomically(path_, json.dump(2), errorMessage);
}
}
