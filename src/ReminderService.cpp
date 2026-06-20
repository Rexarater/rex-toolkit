#include "ReminderService.h"

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <sstream>
#include <system_error>

namespace
{
constexpr int kMaxNaturalReminderMinutes = 10 * 366 * 24 * 60;

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
    auto first = std::find_if_not(value.begin(), value.end(), [](wchar_t ch)
    {
        return iswspace(ch) != 0;
    });
    auto last = std::find_if_not(value.rbegin(), value.rend(), [](wchar_t ch)
    {
        return iswspace(ch) != 0;
    }).base();
    if (first >= last)
    {
        return {};
    }
    return std::wstring(first, last);
}

std::wstring ToLower(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch)
    {
        return static_cast<wchar_t>(towlower(ch));
    });
    return value;
}

std::string JsonEscape(const std::wstring& value)
{
    const std::string utf8 = WideToUtf8(value);
    std::string output;
    output.reserve(utf8.size() + 8);
    output.push_back('"');
    for (unsigned char ch : utf8)
    {
        switch (ch)
        {
        case '"':
            output += "\\\"";
            break;
        case '\\':
            output += "\\\\";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (ch < 0x20)
            {
                std::ostringstream stream;
                stream << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch);
                output += stream.str();
            }
            else
            {
                output.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    output.push_back('"');
    return output;
}

std::wstring UnescapeJsonString(const std::string& text)
{
    std::string output;
    output.reserve(text.size());
    for (size_t index = 0; index < text.size(); ++index)
    {
        const char ch = text[index];
        if (ch != '\\' || index + 1 >= text.size())
        {
            output.push_back(ch);
            continue;
        }

        const char escaped = text[++index];
        switch (escaped)
        {
        case '"':
        case '\\':
        case '/':
            output.push_back(escaped);
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
        default:
            break;
        }
    }
    return Utf8ToWide(output);
}

size_t FindField(const std::string& object, const char* key)
{
    const std::string quotedKey = std::string("\"") + key + "\"";
    const size_t keyPosition = object.find(quotedKey);
    if (keyPosition == std::string::npos)
    {
        return std::string::npos;
    }
    const size_t colon = object.find(':', keyPosition + quotedKey.size());
    return colon == std::string::npos ? std::string::npos : colon + 1;
}

size_t SkipSpaces(const std::string& text, size_t position)
{
    while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position])) != 0)
    {
        ++position;
    }
    return position;
}

std::wstring ExtractString(const std::string& object, const char* key)
{
    size_t position = SkipSpaces(object, FindField(object, key));
    if (position == std::string::npos || position >= object.size() || object[position] != '"')
    {
        return {};
    }

    ++position;
    std::string value;
    bool escaped = false;
    for (; position < object.size(); ++position)
    {
        const char ch = object[position];
        if (escaped)
        {
            value.push_back('\\');
            value.push_back(ch);
            escaped = false;
            continue;
        }
        if (ch == '\\')
        {
            escaped = true;
            continue;
        }
        if (ch == '"')
        {
            break;
        }
        value.push_back(ch);
    }
    return UnescapeJsonString(value);
}

int ExtractInt(const std::string& object, const char* key, int fallback = 0)
{
    size_t position = SkipSpaces(object, FindField(object, key));
    if (position == std::string::npos || position >= object.size())
    {
        return fallback;
    }
    return std::strtol(object.c_str() + position, nullptr, 10);
}

std::vector<int> ExtractIntArray(const std::string& object, const char* key)
{
    std::vector<int> values;
    size_t position = SkipSpaces(object, FindField(object, key));
    if (position == std::string::npos || position >= object.size() || object[position] != '[')
    {
        return values;
    }

    ++position;
    while (position < object.size())
    {
        position = SkipSpaces(object, position);
        if (position >= object.size() || object[position] == ']')
        {
            break;
        }
        char* end = nullptr;
        const long value = std::strtol(object.c_str() + position, &end, 10);
        if (end == object.c_str() + position)
        {
            break;
        }
        values.push_back(static_cast<int>(std::max<long>(0, value)));
        position = static_cast<size_t>(end - object.c_str());
        position = SkipSpaces(object, position);
        if (position < object.size() && object[position] == ',')
        {
            ++position;
        }
    }

    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

bool ExtractBool(const std::string& object, const char* key, bool fallback = false)
{
    size_t position = SkipSpaces(object, FindField(object, key));
    if (position == std::string::npos || position >= object.size())
    {
        return fallback;
    }
    if (object.compare(position, 4, "true") == 0)
    {
        return true;
    }
    if (object.compare(position, 5, "false") == 0)
    {
        return false;
    }
    return fallback;
}

std::string ExtractObject(const std::string& object, const char* key)
{
    size_t position = SkipSpaces(object, FindField(object, key));
    if (position == std::string::npos || position >= object.size() || object[position] != '{')
    {
        return {};
    }

    int depth = 0;
    bool inString = false;
    bool escaped = false;
    const size_t start = position;
    for (; position < object.size(); ++position)
    {
        const char ch = object[position];
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
            ++depth;
        }
        else if (ch == '}')
        {
            --depth;
            if (depth == 0)
            {
                return object.substr(start, position - start + 1);
            }
        }
    }
    return {};
}

std::vector<std::string> ExtractReminderObjects(const std::string& json)
{
    std::vector<std::string> objects;
    const size_t remindersKey = json.find("\"reminders\"");
    if (remindersKey == std::string::npos)
    {
        return objects;
    }
    const size_t arrayStart = json.find('[', remindersKey);
    if (arrayStart == std::string::npos)
    {
        return objects;
    }

    int depth = 0;
    bool inString = false;
    bool escaped = false;
    size_t objectStart = std::string::npos;
    for (size_t index = arrayStart + 1; index < json.size(); ++index)
    {
        const char ch = json[index];
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
                objectStart = index;
            }
            ++depth;
        }
        else if (ch == '}')
        {
            --depth;
            if (depth == 0 && objectStart != std::string::npos)
            {
                objects.push_back(json.substr(objectStart, index - objectStart + 1));
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

std::wstring GenerateId()
{
    GUID guid {};
    if (SUCCEEDED(CoCreateGuid(&guid)))
    {
        wchar_t buffer[48] {};
        if (StringFromGUID2(guid, buffer, static_cast<int>(std::size(buffer))) > 0)
        {
            std::wstring value = buffer;
            if (!value.empty() && value.front() == L'{')
            {
                value.erase(value.begin());
            }
            if (!value.empty() && value.back() == L'}')
            {
                value.pop_back();
            }
            return value;
        }
    }

    const auto now = std::chrono::system_clock::now().time_since_epoch().count();
    return L"reminder-" + std::to_wstring(now);
}

std::tm LocalTm(std::chrono::system_clock::time_point value)
{
    const std::time_t timeValue = std::chrono::system_clock::to_time_t(value);
    std::tm local {};
    localtime_s(&local, &timeValue);
    return local;
}

std::chrono::system_clock::time_point FromLocalTm(std::tm local)
{
    local.tm_isdst = -1;
    const std::time_t timeValue = std::mktime(&local);
    return std::chrono::system_clock::from_time_t(timeValue);
}

int DaysInMonth(int year, int month)
{
    static constexpr int days[] { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month == 2)
    {
        const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        return leap ? 29 : 28;
    }
    if (month < 1 || month > 12)
    {
        return 31;
    }
    return days[month - 1];
}

std::chrono::system_clock::time_point AddMonths(std::chrono::system_clock::time_point value, int months)
{
    std::tm local = LocalTm(value);
    const int originalDay = local.tm_mday;
    int year = local.tm_year + 1900;
    int month = local.tm_mon + 1 + months;
    while (month > 12)
    {
        month -= 12;
        ++year;
    }
    while (month < 1)
    {
        month += 12;
        --year;
    }

    local.tm_year = year - 1900;
    local.tm_mon = month - 1;
    local.tm_mday = std::min(originalDay, DaysInMonth(year, month));
    return FromLocalTm(local);
}

std::chrono::system_clock::time_point AddYears(std::chrono::system_clock::time_point value, int years)
{
    std::tm local = LocalTm(value);
    const int targetYear = local.tm_year + 1900 + years;
    const int targetMonth = local.tm_mon + 1;
    local.tm_year += years;
    local.tm_mday = std::min(local.tm_mday, DaysInMonth(targetYear, targetMonth));
    return FromLocalTm(local);
}

std::chrono::system_clock::time_point NextOccurrence(const Reminder& reminder, std::chrono::system_clock::time_point now)
{
    auto dueAt = ReminderService::ParseLocalIso(reminder.dueAt).value_or(now);
    const int interval = std::max(1, reminder.repeat.interval);
    do
    {
        switch (reminder.repeat.type)
        {
        case ReminderRepeatType::Daily:
            dueAt += std::chrono::hours(24 * interval);
            break;
        case ReminderRepeatType::Weekly:
            dueAt += std::chrono::hours(24 * 7 * interval);
            break;
        case ReminderRepeatType::Monthly:
            dueAt = AddMonths(dueAt, interval);
            break;
        case ReminderRepeatType::Yearly:
            dueAt = AddYears(dueAt, interval);
            break;
        case ReminderRepeatType::None:
            return dueAt;
        }
    } while (dueAt <= now);
    return dueAt;
}

bool SameLocalDate(std::chrono::system_clock::time_point left, std::chrono::system_clock::time_point right)
{
    const std::tm leftTm = LocalTm(left);
    const std::tm rightTm = LocalTm(right);
    return leftTm.tm_year == rightTm.tm_year &&
        leftTm.tm_mon == rightTm.tm_mon &&
        leftTm.tm_mday == rightTm.tm_mday;
}

std::chrono::system_clock::time_point StartOfLocalDay(std::chrono::system_clock::time_point value)
{
    std::tm local = LocalTm(value);
    local.tm_hour = 0;
    local.tm_min = 0;
    local.tm_sec = 0;
    return FromLocalTm(local);
}

std::vector<int> ReminderAlertMinutes(const Reminder& reminder)
{
    std::vector<int> values = reminder.alertBeforeMinutesList;
    if (values.empty())
    {
        values.push_back(reminder.alertBeforeMinutes);
    }
    for (int& value : values)
    {
        value = std::max(0, value);
    }
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

int EarliestReminderAlertMinutes(const Reminder& reminder)
{
    const std::vector<int> values = ReminderAlertMinutes(reminder);
    return values.empty() ? 0 : values.back();
}

Reminder ReminderFromObject(const std::string& object)
{
    Reminder reminder;
    reminder.id = ExtractString(object, "id");
    reminder.title = ExtractString(object, "title");
    reminder.description = ExtractString(object, "description");
    reminder.dueAt = ExtractString(object, "dueAt");
    reminder.allDay = ExtractBool(object, "allDay");
    reminder.priority = ReminderService::PriorityFromString(ExtractString(object, "priority"));
    reminder.category = ExtractString(object, "category");
    if (reminder.category.empty())
    {
        reminder.category = L"General";
    }
    const std::string repeatObject = ExtractObject(object, "repeat");
    reminder.repeat.type = ReminderService::RepeatTypeFromString(ExtractString(repeatObject, "type"));
    reminder.repeat.interval = std::max(1, ExtractInt(repeatObject, "interval", 1));
    reminder.alertBeforeMinutes = std::max(0, ExtractInt(object, "alertBeforeMinutes", reminder.allDay ? 1440 : 15));
    reminder.snoozedUntil = ExtractString(object, "snoozedUntil");
    reminder.createdAt = ExtractString(object, "createdAt");
    reminder.updatedAt = ExtractString(object, "updatedAt");
    reminder.completedAt = ExtractString(object, "completedAt");
    reminder.dismissedBannerUntil = ExtractString(object, "dismissedBannerUntil");
    reminder.alertBeforeMinutesList = ExtractIntArray(object, "alertBeforeMinutesList");
    if (reminder.alertBeforeMinutesList.empty())
    {
        reminder.alertBeforeMinutesList.push_back(reminder.alertBeforeMinutes);
    }
    return reminder;
}

void WriteStringField(std::ostringstream& output, const char* indent, const char* key, const std::wstring& value, bool comma = true)
{
    output << indent << "\"" << key << "\": " << JsonEscape(value);
    if (comma)
    {
        output << ",";
    }
    output << "\n";
}

void WriteNullableStringField(std::ostringstream& output, const char* indent, const char* key, const std::wstring& value, bool comma = true)
{
    output << indent << "\"" << key << "\": ";
    if (value.empty())
    {
        output << "null";
    }
    else
    {
        output << JsonEscape(value);
    }
    if (comma)
    {
        output << ",";
    }
    output << "\n";
}

std::wstring MonthName(int month)
{
    static constexpr wchar_t names[][4] {
        L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
        L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"
    };
    if (month < 1 || month > 12)
    {
        return L"";
    }
    return names[month - 1];
}

std::optional<int> MonthFromName(const std::wstring& text)
{
    static const std::map<std::wstring, int> months {
        { L"january", 1 }, { L"jan", 1 },
        { L"february", 2 }, { L"feb", 2 },
        { L"march", 3 }, { L"mar", 3 },
        { L"april", 4 }, { L"apr", 4 },
        { L"may", 5 },
        { L"june", 6 }, { L"jun", 6 },
        { L"july", 7 }, { L"jul", 7 },
        { L"august", 8 }, { L"aug", 8 },
        { L"september", 9 }, { L"sep", 9 },
        { L"october", 10 }, { L"oct", 10 },
        { L"november", 11 }, { L"nov", 11 },
        { L"december", 12 }, { L"dec", 12 }
    };
    const auto found = months.find(text);
    if (found == months.end())
    {
        return std::nullopt;
    }
    return found->second;
}

bool ParseHourMinuteText(const std::wstring& text, int& hour, int& minute)
{
    std::wstring value = ToLower(Trim(text));
    const bool hasPm = value.find(L"pm") != std::wstring::npos;
    const bool hasAm = value.find(L"am") != std::wstring::npos;
    value.erase(std::remove_if(value.begin(), value.end(), [](wchar_t ch)
    {
        return ch == L'a' || ch == L'p' || ch == L'm' || iswspace(ch) != 0;
    }), value.end());

    int parsedHour = 0;
    int parsedMinute = 0;
    if (swscanf_s(value.c_str(), L"%d:%d", &parsedHour, &parsedMinute) < 1)
    {
        return false;
    }

    if (hasPm && parsedHour < 12)
    {
        parsedHour += 12;
    }
    if (hasAm && parsedHour == 12)
    {
        parsedHour = 0;
    }
    if (!hasAm && !hasPm && parsedHour >= 1 && parsedHour <= 7)
    {
        parsedHour += 12;
    }
    if (parsedHour < 0 || parsedHour > 23 || parsedMinute < 0 || parsedMinute > 59)
    {
        return false;
    }

    hour = parsedHour;
    minute = parsedMinute;
    return true;
}
}

ReminderList ReminderStorage::Load(const std::filesystem::path& path, std::wstring& warning) const
{
    ReminderList list;
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

    const std::vector<std::string> objects = ExtractReminderObjects(json);
    if (json.find("\"reminders\"") == std::string::npos)
    {
        warning = L"Reminders could not read the saved file, so it started empty.";
        return list;
    }

    for (const std::string& object : objects)
    {
        Reminder reminder = ReminderFromObject(object);
        if (!reminder.id.empty() && !reminder.title.empty() && ReminderService::ParseLocalIso(reminder.dueAt))
        {
            list.reminders.push_back(std::move(reminder));
        }
    }
    return list;
}

bool ReminderStorage::Save(const std::filesystem::path& path, const ReminderList& list, std::wstring& errorMessage) const
{
    std::error_code fileError;
    std::filesystem::create_directories(path.parent_path(), fileError);
    if (fileError)
    {
        errorMessage = L"Could not save reminders. The app data folder could not be created.";
        return false;
    }

    if (std::filesystem::exists(path, fileError))
    {
        std::filesystem::copy_file(path, path.wstring() + L".bak", std::filesystem::copy_options::overwrite_existing, fileError);
        fileError.clear();
    }

    std::ostringstream output;
    output << "{\n  \"version\": 1,\n  \"reminders\": [\n";
    const auto now = ReminderService::Now();
    for (size_t index = 0; index < list.reminders.size(); ++index)
    {
        const Reminder& reminder = list.reminders[index];
        output << "    {\n";
        WriteStringField(output, "      ", "id", reminder.id);
        WriteStringField(output, "      ", "title", reminder.title);
        WriteStringField(output, "      ", "description", reminder.description);
        WriteStringField(output, "      ", "dueAt", reminder.dueAt);
        output << "      \"allDay\": " << (reminder.allDay ? "true" : "false") << ",\n";
        WriteStringField(output, "      ", "status", ReminderService::StatusLabel(ReminderScheduler::StatusFor(reminder, now)));
        WriteStringField(output, "      ", "priority", ReminderService::PriorityLabel(reminder.priority));
        WriteStringField(output, "      ", "category", reminder.category);
        output << "      \"repeat\": {\n";
        WriteStringField(output, "        ", "type", ReminderService::RepeatTypeLabel(reminder.repeat.type));
        output << "        \"interval\": " << std::max(1, reminder.repeat.interval) << "\n";
        output << "      },\n";
        output << "      \"alertBeforeMinutes\": " << reminder.alertBeforeMinutes << ",\n";
        output << "      \"alertBeforeMinutesList\": [";
        const std::vector<int> alertMinutes = reminder.alertBeforeMinutesList.empty()
            ? std::vector<int> { reminder.alertBeforeMinutes }
            : reminder.alertBeforeMinutesList;
        for (size_t alertIndex = 0; alertIndex < alertMinutes.size(); ++alertIndex)
        {
            if (alertIndex > 0)
            {
                output << ", ";
            }
            output << std::max(0, alertMinutes[alertIndex]);
        }
        output << "],\n";
        WriteNullableStringField(output, "      ", "snoozedUntil", reminder.snoozedUntil);
        WriteStringField(output, "      ", "createdAt", reminder.createdAt);
        WriteStringField(output, "      ", "updatedAt", reminder.updatedAt);
        WriteNullableStringField(output, "      ", "completedAt", reminder.completedAt);
        WriteNullableStringField(output, "      ", "dismissedBannerUntil", reminder.dismissedBannerUntil, false);
        output << "    }";
        if (index + 1 < list.reminders.size())
        {
            output << ",";
        }
        output << "\n";
    }
    output << "  ]\n}\n";

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
    {
        errorMessage = L"Could not save reminders. Permission was denied.";
        return false;
    }
    file << output.str();
    if (!file)
    {
        errorMessage = L"Could not save reminders.";
        return false;
    }
    return true;
}

ReminderStatus ReminderScheduler::StatusFor(const Reminder& reminder, std::chrono::system_clock::time_point now)
{
    if (!reminder.completedAt.empty())
    {
        return ReminderStatus::Completed;
    }

    if (const auto snoozedUntil = ReminderService::ParseLocalIso(reminder.snoozedUntil))
    {
        if (*snoozedUntil > now)
        {
            return ReminderStatus::Snoozed;
        }
    }

    const auto dueAt = ReminderService::ParseLocalIso(reminder.dueAt);
    if (!dueAt)
    {
        return ReminderStatus::Upcoming;
    }

    const int alertMinutes = reminder.allDay
        ? std::max(0, EarliestReminderAlertMinutes(reminder))
        : EarliestReminderAlertMinutes(reminder);
    const auto alertAt = *dueAt - std::chrono::minutes(std::max(0, alertMinutes));
    if (now < alertAt)
    {
        return ReminderStatus::Upcoming;
    }
    if (now < *dueAt)
    {
        return ReminderStatus::DueSoon;
    }
    if (now < *dueAt + std::chrono::minutes(5))
    {
        return ReminderStatus::DueNow;
    }
    return ReminderStatus::Overdue;
}

ReminderAlert ReminderScheduler::BuildAlert(const Reminder& reminder, std::chrono::system_clock::time_point now)
{
    ReminderAlert alert;
    const ReminderStatus status = StatusFor(reminder, now);
    if (status != ReminderStatus::DueSoon && status != ReminderStatus::DueNow && status != ReminderStatus::Overdue)
    {
        return alert;
    }

    alert.hasValue = true;
    alert.reminderId = reminder.id;
    alert.title = reminder.title;
    alert.subtext = ReminderService::CountdownLabel(reminder, now);
    if (status == ReminderStatus::DueSoon)
    {
        alert.severity = ReminderAlertSeverity::DueSoon;
        alert.message = L"Reminder due soon: " + reminder.title;
    }
    else if (status == ReminderStatus::DueNow)
    {
        alert.severity = ReminderAlertSeverity::DueNow;
        alert.message = L"Reminder due now: " + reminder.title;
    }
    else
    {
        alert.severity = ReminderAlertSeverity::Overdue;
        alert.message = L"Overdue reminder: " + reminder.title;
    }
    return alert;
}

bool ReminderScheduler::ShouldShowBanner(const Reminder& reminder, std::chrono::system_clock::time_point now)
{
    const ReminderStatus status = StatusFor(reminder, now);
    if (status != ReminderStatus::DueSoon && status != ReminderStatus::DueNow && status != ReminderStatus::Overdue)
    {
        return false;
    }

    if (const auto dismissedUntil = ReminderService::ParseLocalIso(reminder.dismissedBannerUntil))
    {
        if (*dismissedUntil > now)
        {
            return false;
        }
    }
    return true;
}

ReminderAlert ReminderBannerManager::ChooseAlert(const ReminderList& list, std::chrono::system_clock::time_point now)
{
    const Reminder* best = nullptr;
    auto priorityOf = [](ReminderStatus status)
    {
        if (status == ReminderStatus::Overdue) return 0;
        if (status == ReminderStatus::DueNow) return 1;
        if (status == ReminderStatus::DueSoon) return 2;
        return 3;
    };

    for (const Reminder& reminder : list.reminders)
    {
        if (!ReminderScheduler::ShouldShowBanner(reminder, now))
        {
            continue;
        }
        if (!best)
        {
            best = &reminder;
            continue;
        }
        const ReminderStatus currentStatus = ReminderScheduler::StatusFor(reminder, now);
        const ReminderStatus bestStatus = ReminderScheduler::StatusFor(*best, now);
        if (priorityOf(currentStatus) < priorityOf(bestStatus))
        {
            best = &reminder;
            continue;
        }
        if (priorityOf(currentStatus) == priorityOf(bestStatus) && reminder.dueAt < best->dueAt)
        {
            best = &reminder;
        }
    }
    return best ? ReminderScheduler::BuildAlert(*best, now) : ReminderAlert {};
}

ReminderList ReminderService::LoadReminders(const std::filesystem::path& path, std::wstring& warning) const
{
    return storage_.Load(path, warning);
}

bool ReminderService::SaveReminders(const std::filesystem::path& path, const ReminderList& list, std::wstring& errorMessage) const
{
    return storage_.Save(path, list, errorMessage);
}

Reminder ReminderService::CreateReminder(
    const std::wstring& title,
    const std::wstring& description,
    std::chrono::system_clock::time_point dueAt,
    bool allDay,
    ReminderPriority priority,
    const std::wstring& category,
    RecurrenceRule repeat,
    int alertBeforeMinutes) const
{
    Reminder reminder;
    reminder.id = GenerateId();
    reminder.title = Trim(title);
    reminder.description = Trim(description);
    reminder.dueAt = FormatLocalIso(dueAt);
    reminder.allDay = allDay;
    reminder.priority = priority;
    reminder.category = Trim(category).empty() ? L"General" : Trim(category);
    reminder.repeat = repeat;
    reminder.repeat.interval = std::max(1, reminder.repeat.interval);
    reminder.alertBeforeMinutes = std::max(0, alertBeforeMinutes);
    reminder.alertBeforeMinutesList = { reminder.alertBeforeMinutes };
    reminder.createdAt = FormatLocalIso(Now());
    reminder.updatedAt = reminder.createdAt;
    return reminder;
}

ReminderOperationResult ReminderService::CompleteReminder(Reminder& reminder, std::chrono::system_clock::time_point now) const
{
    if (reminder.repeat.type == ReminderRepeatType::None)
    {
        reminder.completedAt = FormatLocalIso(now);
        reminder.updatedAt = reminder.completedAt;
        reminder.snoozedUntil.clear();
        reminder.dismissedBannerUntil.clear();
        return { true, L"Reminder completed." };
    }

    reminder.dueAt = FormatLocalIso(NextOccurrence(reminder, now));
    reminder.completedAt.clear();
    reminder.snoozedUntil.clear();
    reminder.dismissedBannerUntil.clear();
    reminder.updatedAt = FormatLocalIso(now);
    return { true, L"Recurring reminder advanced to the next occurrence." };
}

ReminderOperationResult ReminderService::SnoozeReminder(Reminder& reminder, std::chrono::system_clock::time_point until) const
{
    reminder.snoozedUntil = FormatLocalIso(until);
    reminder.dismissedBannerUntil = reminder.snoozedUntil;
    reminder.updatedAt = FormatLocalIso(Now());
    return { true, L"Reminder snoozed." };
}

ReminderOperationResult ReminderService::DismissBanner(Reminder& reminder, std::chrono::system_clock::time_point until) const
{
    reminder.dismissedBannerUntil = FormatLocalIso(until);
    reminder.updatedAt = FormatLocalIso(Now());
    return { true, L"Reminder banner dismissed." };
}

std::chrono::system_clock::time_point ReminderService::Now()
{
    return std::chrono::system_clock::now();
}

std::wstring ReminderService::FormatLocalIso(std::chrono::system_clock::time_point value)
{
    const std::tm local = LocalTm(value);
    wchar_t buffer[32] {};
    wcsftime(buffer, std::size(buffer), L"%Y-%m-%dT%H:%M:%S", &local);
    return buffer;
}

std::optional<std::chrono::system_clock::time_point> ReminderService::ParseLocalIso(const std::wstring& value)
{
    if (value.empty())
    {
        return std::nullopt;
    }

    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (swscanf_s(value.c_str(), L"%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6)
    {
        return std::nullopt;
    }
    if (year < 1970 || month < 1 || month > 12 || day < 1 || day > DaysInMonth(year, month) ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59)
    {
        return std::nullopt;
    }

    std::tm local {};
    local.tm_year = year - 1900;
    local.tm_mon = month - 1;
    local.tm_mday = day;
    local.tm_hour = hour;
    local.tm_min = minute;
    local.tm_sec = second;
    return FromLocalTm(local);
}

std::optional<std::chrono::system_clock::time_point> ReminderService::ParseDateAndTime(const std::wstring& dateText, const std::wstring& timeText, bool allDay)
{
    int year = 0;
    int month = 0;
    int day = 0;
    std::wstring normalizedDate = Trim(dateText);
    std::replace(normalizedDate.begin(), normalizedDate.end(), L'/', L'-');
    if (swscanf_s(normalizedDate.c_str(), L"%d-%d-%d", &year, &month, &day) != 3)
    {
        return std::nullopt;
    }
    if (year < 1970 || month < 1 || month > 12 || day < 1 || day > DaysInMonth(year, month))
    {
        return std::nullopt;
    }

    int hour = allDay ? 9 : 0;
    int minute = 0;
    if (!allDay && !ParseHourMinuteText(timeText, hour, minute))
    {
        return std::nullopt;
    }

    std::tm local {};
    local.tm_year = year - 1900;
    local.tm_mon = month - 1;
    local.tm_mday = day;
    local.tm_hour = hour;
    local.tm_min = minute;
    return FromLocalTm(local);
}

bool ReminderService::TryParseNaturalDue(const std::wstring& text, std::chrono::system_clock::time_point now, std::chrono::system_clock::time_point& dueAt, bool& allDay)
{
    const std::wstring lower = ToLower(text);
    std::wistringstream stream(lower);
    std::wstring token;
    std::vector<std::wstring> tokens;
    while (stream >> token)
    {
        while (!token.empty() && iswpunct(token.front()) != 0)
        {
            token.erase(token.begin());
        }
        while (!token.empty() && token.back() != L':' && iswpunct(token.back()) != 0)
        {
            token.pop_back();
        }
        if (!token.empty())
        {
            tokens.push_back(token);
        }
    }

    auto parseAmountWord = [](const std::wstring& value, int& amount)
    {
        if (value.empty())
        {
            return false;
        }

        static const std::map<std::wstring, int> wordAmounts {
            { L"a", 1 }, { L"an", 1 }, { L"one", 1 },
            { L"two", 2 }, { L"three", 3 }, { L"four", 4 }, { L"five", 5 },
            { L"six", 6 }, { L"seven", 7 }, { L"eight", 8 }, { L"nine", 9 },
            { L"ten", 10 }, { L"eleven", 11 }, { L"twelve", 12 },
            { L"thirteen", 13 }, { L"fourteen", 14 }, { L"fifteen", 15 },
            { L"sixteen", 16 }, { L"seventeen", 17 }, { L"eighteen", 18 },
            { L"nineteen", 19 }, { L"twenty", 20 }, { L"thirty", 30 },
            { L"forty", 40 }, { L"fifty", 50 }, { L"sixty", 60 }
        };
        if (const auto found = wordAmounts.find(value); found != wordAmounts.end())
        {
            amount = found->second;
            return amount > 0;
        }

        return false;
    };

    auto parseAmount = [&](const std::wstring& value, int& amount)
    {
        if (parseAmountWord(value, amount))
        {
            return true;
        }

        if (value.empty())
        {
            return false;
        }

        if (std::any_of(value.begin(), value.end(), [](wchar_t ch)
        {
            return iswdigit(ch) == 0;
        }))
        {
            return false;
        }

        long long parsed = 0;
        for (wchar_t ch : value)
        {
            parsed = parsed * 10 + static_cast<long long>(ch - L'0');
            if (parsed > kMaxNaturalReminderMinutes)
            {
                return false;
            }
        }

        amount = static_cast<int>(parsed);
        return amount > 0;
    };

    auto parseAmountAt = [&](size_t index, int& amount, size_t& consumed)
    {
        consumed = 0;
        if (index >= tokens.size())
        {
            return false;
        }

        if (parseAmount(tokens[index], amount))
        {
            consumed = 1;

            int ones = 0;
            if (index + 1 < tokens.size() &&
                amount >= 20 &&
                amount % 10 == 0 &&
                parseAmountWord(tokens[index + 1], ones) &&
                ones > 0 &&
                ones < 10)
            {
                amount += ones;
                consumed = 2;
            }

            return true;
        }

        return false;
    };

    auto unitToMinutes = [](const std::wstring& unit)
    {
        if (unit.find(L"minute") == 0 || unit == L"min" || unit == L"mins" || unit == L"mn" || unit == L"m")
        {
            return 1;
        }
        if (unit.find(L"hour") == 0 || unit == L"hr" || unit == L"hrs" || unit == L"h")
        {
            return 60;
        }
        if (unit.find(L"day") == 0 || unit == L"d")
        {
            return 24 * 60;
        }
        if (unit.find(L"week") == 0 || unit == L"wk" || unit == L"wks" || unit == L"w")
        {
            return 24 * 7 * 60;
        }
        return 0;
    };

    auto addMinutes = [](long long& totalMinutes, int amount, int unitMinutes)
    {
        if (amount <= 0 || unitMinutes <= 0)
        {
            return false;
        }
        const long long addition = static_cast<long long>(amount) * unitMinutes;
        if (addition <= 0 || totalMinutes + addition > kMaxNaturalReminderMinutes)
        {
            return false;
        }
        totalMinutes += addition;
        return true;
    };

    auto parseCompactDurationToken = [&](const std::wstring& value, int& minutes)
    {
        minutes = 0;
        long long totalMinutes = 0;
        bool parsedAny = false;
        size_t position = 0;
        while (position < value.size())
        {
            const size_t amountStart = position;
            while (position < value.size() && iswdigit(value[position]) != 0)
            {
                ++position;
            }
            if (position == amountStart)
            {
                return false;
            }

            int amount = 0;
            if (!parseAmount(value.substr(amountStart, position - amountStart), amount))
            {
                return false;
            }

            const size_t unitStart = position;
            while (position < value.size() && iswalpha(value[position]) != 0)
            {
                ++position;
            }
            if (position == unitStart || position < value.size())
            {
                return false;
            }

            const int unitMinutes = unitToMinutes(value.substr(unitStart, position - unitStart));
            if (!addMinutes(totalMinutes, amount, unitMinutes))
            {
                return false;
            }
            parsedAny = true;
        }

        if (!parsedAny || totalMinutes <= 0 || totalMinutes > kMaxNaturalReminderMinutes)
        {
            return false;
        }
        minutes = static_cast<int>(totalMinutes);
        return true;
    };

    auto parseRelativeDuration = [&](size_t startIndex, int& totalMinutes)
    {
        bool parsedAny = false;
        long long accumulatedMinutes = 0;

        for (size_t index = startIndex; index < tokens.size();)
        {
            if (tokens[index] == L"and")
            {
                if (!parsedAny)
                {
                    return false;
                }
                ++index;
                continue;
            }

            if (tokens[index] == L"half")
            {
                size_t unitIndex = index + 1;
                if (unitIndex < tokens.size() && (tokens[unitIndex] == L"a" || tokens[unitIndex] == L"an"))
                {
                    ++unitIndex;
                }
                if (unitIndex < tokens.size())
                {
                    const int unitMinutes = unitToMinutes(tokens[unitIndex]);
                    if (unitMinutes > 0)
                    {
                        if (!addMinutes(accumulatedMinutes, std::max(1, unitMinutes / 2), 1))
                        {
                            return false;
                        }
                        parsedAny = true;
                        index = unitIndex + 1;
                        continue;
                    }
                }
            }

            int amount = 0;
            size_t amountTokens = 0;
            if (parseAmountAt(index, amount, amountTokens) &&
                index + amountTokens < tokens.size())
            {
                const int unitMinutes = unitToMinutes(tokens[index + amountTokens]);
                if (unitMinutes > 0)
                {
                    if (!addMinutes(accumulatedMinutes, amount, unitMinutes))
                    {
                        return false;
                    }
                    parsedAny = true;
                    index += amountTokens + 1;
                    continue;
                }
            }

            int compactMinutes = 0;
            if (parseCompactDurationToken(tokens[index], compactMinutes))
            {
                if (!addMinutes(accumulatedMinutes, compactMinutes, 1))
                {
                    return false;
                }
                parsedAny = true;
                ++index;
                continue;
            }

            if (parsedAny &&
                index + 2 < tokens.size() &&
                (tokens[index] == L"a" || tokens[index] == L"an") &&
                tokens[index + 1] == L"half")
            {
                const int unitMinutes = unitToMinutes(tokens[index + 2]);
                if (unitMinutes > 0)
                {
                    if (!addMinutes(accumulatedMinutes, std::max(1, unitMinutes / 2), 1))
                    {
                        return false;
                    }
                    index += 3;
                    continue;
                }
            }

            break;
        }

        if (!parsedAny || accumulatedMinutes <= 0 || accumulatedMinutes > kMaxNaturalReminderMinutes)
        {
            totalMinutes = 0;
            return false;
        }
        totalMinutes = static_cast<int>(accumulatedMinutes);
        return true;
    };

    for (size_t index = 0; index < tokens.size(); ++index)
    {
        int totalMinutes = 0;
        if (tokens[index] == L"in" &&
            index + 1 < tokens.size() &&
            parseRelativeDuration(index + 1, totalMinutes))
        {
            dueAt = now + std::chrono::minutes(totalMinutes);
            allDay = false;
            return true;
        }
        if (parseRelativeDuration(index, totalMinutes))
        {
            dueAt = now + std::chrono::minutes(totalMinutes);
            allDay = false;
            return true;
        }
    }

    if (lower.find(L"tomorrow") != std::wstring::npos)
    {
        std::tm local = LocalTm(now + std::chrono::hours(24));
        local.tm_hour = 9;
        local.tm_min = 0;
        local.tm_sec = 0;
        const size_t atPosition = lower.find(L" at ");
        if (atPosition != std::wstring::npos)
        {
            int hour = 0;
            int minute = 0;
            if (ParseHourMinuteText(lower.substr(atPosition + 4), hour, minute))
            {
                local.tm_hour = hour;
                local.tm_min = minute;
                allDay = false;
            }
        }
        else
        {
            allDay = true;
        }
        dueAt = FromLocalTm(local);
        return true;
    }

    static const std::vector<std::pair<std::wstring, int>> weekdays {
        { L"sunday", 0 }, { L"monday", 1 }, { L"tuesday", 2 }, { L"wednesday", 3 },
        { L"thursday", 4 }, { L"friday", 5 }, { L"saturday", 6 }
    };
    for (const auto& weekday : weekdays)
    {
        const std::wstring pattern = L"next " + weekday.first;
        if (lower.find(pattern) != std::wstring::npos)
        {
            std::tm local = LocalTm(now);
            int daysAhead = (weekday.second - local.tm_wday + 7) % 7;
            if (daysAhead == 0)
            {
                daysAhead = 7;
            }
            dueAt = StartOfLocalDay(now) + std::chrono::hours(24 * daysAhead + 9);
            allDay = true;
            return true;
        }
    }

    std::wistringstream monthStream(lower);
    std::wstring word;
    while (monthStream >> word)
    {
        const auto month = MonthFromName(word);
        if (!month)
        {
            continue;
        }
        int day = 0;
        if (!(monthStream >> day) || day < 1 || day > 31)
        {
            continue;
        }
        std::tm local = LocalTm(now);
        int year = local.tm_year + 1900;
        if (lower.find(L"next year") != std::wstring::npos)
        {
            ++year;
        }
        if (day > DaysInMonth(year, *month))
        {
            continue;
        }
        std::tm target {};
        target.tm_year = year - 1900;
        target.tm_mon = *month - 1;
        target.tm_mday = day;
        target.tm_hour = 9;
        dueAt = FromLocalTm(target);
        if (dueAt <= now)
        {
            target.tm_year += 1;
            dueAt = FromLocalTm(target);
        }
        allDay = true;
        return true;
    }

    return false;
}

std::wstring ReminderService::PriorityLabel(ReminderPriority priority)
{
    switch (priority)
    {
    case ReminderPriority::Low:
        return L"Low";
    case ReminderPriority::Normal:
        return L"Normal";
    case ReminderPriority::High:
        return L"High";
    }
    return L"Normal";
}

std::wstring ReminderService::RepeatTypeLabel(ReminderRepeatType repeat)
{
    switch (repeat)
    {
    case ReminderRepeatType::None:
        return L"None";
    case ReminderRepeatType::Daily:
        return L"Daily";
    case ReminderRepeatType::Weekly:
        return L"Weekly";
    case ReminderRepeatType::Monthly:
        return L"Monthly";
    case ReminderRepeatType::Yearly:
        return L"Yearly";
    }
    return L"None";
}

std::wstring ReminderService::RepeatLabel(const Reminder& reminder)
{
    if (reminder.repeat.type == ReminderRepeatType::None)
    {
        return L"Does not repeat";
    }
    if (reminder.repeat.type == ReminderRepeatType::Weekly)
    {
        const auto dueAt = ParseLocalIso(reminder.dueAt);
        if (dueAt)
        {
            static constexpr wchar_t weekdays[][10] {
                L"Sunday", L"Monday", L"Tuesday", L"Wednesday", L"Thursday", L"Friday", L"Saturday"
            };
            const std::tm local = LocalTm(*dueAt);
            return L"Repeats every " + std::wstring(weekdays[local.tm_wday]);
        }
    }
    if (reminder.repeat.type == ReminderRepeatType::Monthly)
    {
        const auto dueAt = ParseLocalIso(reminder.dueAt);
        if (dueAt)
        {
            const std::tm local = LocalTm(*dueAt);
            return L"Repeats monthly on the " + std::to_wstring(local.tm_mday);
        }
    }
    return L"Repeats " + ToLower(RepeatTypeLabel(reminder.repeat.type));
}

std::wstring ReminderService::StatusLabel(ReminderStatus status)
{
    switch (status)
    {
    case ReminderStatus::Upcoming:
        return L"Upcoming";
    case ReminderStatus::DueSoon:
        return L"Due Soon";
    case ReminderStatus::DueNow:
        return L"Due Now";
    case ReminderStatus::Overdue:
        return L"Overdue";
    case ReminderStatus::Completed:
        return L"Completed";
    case ReminderStatus::Snoozed:
        return L"Snoozed";
    }
    return L"Upcoming";
}

std::wstring ReminderService::DueLabel(const Reminder& reminder, std::chrono::system_clock::time_point now)
{
    const auto dueAt = ParseLocalIso(reminder.dueAt);
    if (!dueAt)
    {
        return L"Invalid date";
    }

    const std::tm due = LocalTm(*dueAt);
    const bool today = SameLocalDate(*dueAt, now);
    const bool tomorrow = SameLocalDate(*dueAt, now + std::chrono::hours(24));

    wchar_t timeBuffer[32] {};
    wcsftime(timeBuffer, std::size(timeBuffer), L"%I:%M %p", &due);
    std::wstring timeText = timeBuffer;
    if (!timeText.empty() && timeText.front() == L'0')
    {
        timeText.erase(timeText.begin());
    }

    if (today)
    {
        return reminder.allDay ? L"Today" : L"Today at " + timeText;
    }
    if (tomorrow)
    {
        return reminder.allDay ? L"Tomorrow" : L"Tomorrow at " + timeText;
    }

    std::wstring dateText = MonthName(due.tm_mon + 1) + L" " + std::to_wstring(due.tm_mday) + L", " + std::to_wstring(due.tm_year + 1900);
    return reminder.allDay ? dateText : dateText + L" at " + timeText;
}

std::wstring ReminderService::CountdownLabel(const Reminder& reminder, std::chrono::system_clock::time_point now)
{
    const auto dueAt = ParseLocalIso(reminder.dueAt);
    if (!dueAt)
    {
        return L"Invalid date";
    }

    const bool overdue = now > *dueAt;
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(overdue ? now - *dueAt : *dueAt - now).count();
    const long long days = seconds / 86400;
    const long long hours = (seconds % 86400) / 3600;
    const long long minutes = (seconds % 3600) / 60;

    auto unitLabel = [](long long value, const wchar_t* singular, const wchar_t* plural)
    {
        return std::to_wstring(value) + L" " + (value == 1 ? singular : plural);
    };

    std::wstring label;
    if (days > 0)
    {
        label = unitLabel(days, L"day", L"days");
        if (hours > 0)
        {
            label += L" " + unitLabel(hours, L"hour", L"hours");
        }
    }
    else if (hours > 0)
    {
        label = unitLabel(hours, L"hour", L"hours");
        if (minutes > 0)
        {
            label += L" " + unitLabel(minutes, L"minute", L"minutes");
        }
    }
    else
    {
        label = unitLabel(std::max<long long>(1, minutes), L"minute", L"minutes");
    }
    return overdue ? label + L" overdue" : L"in " + label;
}

std::wstring ReminderService::AlertLabel(int minutes, bool allDay)
{
    if (allDay && minutes >= 1440)
    {
        return L"1 day before";
    }
    if (minutes <= 0)
    {
        return L"At time";
    }
    if (minutes < 60)
    {
        return std::to_wstring(minutes) + L" minutes before";
    }
    if (minutes == 60)
    {
        return L"1 hour before";
    }
    if (minutes == 1440)
    {
        return L"1 day before";
    }
    return std::to_wstring(minutes / 60) + L" hours before";
}

ReminderPriority ReminderService::PriorityFromString(const std::wstring& value)
{
    const std::wstring lower = ToLower(value);
    if (lower == L"low")
    {
        return ReminderPriority::Low;
    }
    if (lower == L"high")
    {
        return ReminderPriority::High;
    }
    return ReminderPriority::Normal;
}

ReminderRepeatType ReminderService::RepeatTypeFromString(const std::wstring& value)
{
    const std::wstring lower = ToLower(value);
    if (lower == L"daily")
    {
        return ReminderRepeatType::Daily;
    }
    if (lower == L"weekly")
    {
        return ReminderRepeatType::Weekly;
    }
    if (lower == L"monthly")
    {
        return ReminderRepeatType::Monthly;
    }
    if (lower == L"yearly")
    {
        return ReminderRepeatType::Yearly;
    }
    return ReminderRepeatType::None;
}
