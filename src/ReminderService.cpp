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
    if (reïß7¶‰žËkºwµç@€É•ÑÕÉ¸™…±Í”ì(€€€€€€€€€€€ô((€€€€€€€€€€€½¹ÍÐ¥¹ÐÕ¹¥Ñ5¥¹ÕÑ•Ì€ôÕ¹¥ÑQ½5¥¹ÕÑ•Ì¡Ù…±Õ”¹ÍÕ‰ÍÑÈ¡Õ¹¥ÑMÑ…ÉÐ°Á½Í¥Ñ¥½¸€´Õ¹¥ÑMÑ…ÉÐ¤¤ì(€€€€€€€€€€€¥˜€ ……‘‘5¥¹ÕÑ•Ì¡Ñ½Ñ…±5¥¹ÕÑ•Ì°…µ½Õ¹Ð°Õ¹¥Ñ5¥¹ÕÑ•Ì¤¤(€€€€€€€€€€€ì(€€€€€€€€€€€€€€€É•ÑÕÉ¸™…±Í”ì(€€€€€€€€€€€ô(€€€€€€€€€€€Á…ÉÍ•‘¹ä€ôÑÉÕ”ì(€€€€€€€ô((€€€€€€€¥˜€ …Á…ÉÍ•‘¹äñðÑ½Ñ…±5¥¹ÕÑ•Ì€ðô€ÀñðÑ½Ñ…±5¥¹ÕÑ•Ì€ø­5…á9…ÑÕÉ…±I•µ¥¹‘•É5¥¹ÕÑ•Ì¤(€€€€€€€ì(€€€€€€€€€€€É•ÑÕÉ¸™…±Í”ì(€€€€€€€ô(€€€€€€€µ¥¹ÕÑ•Ì€ôÍÑ…Ñ¥}…ÍÐñ¥¹Ðø¡Ñ½Ñ…±5¥¹ÕÑ•Ì¤ì(€€€€€€€É•ÑÕÉ¸ÑÉÕ”ì(€€€ôì((€€€…ÕÑ¼Á…ÉÍ•I•±…Ñ¥Ù•ÕÉ…Ñ¥½¸€ôl™t¡Í¥é•}ÐÍÑ…ÉÑ%¹‘•à°¥¹Ð˜Ñ½Ñ…±5¥¹ÕÑ•Ì¤(€€€ì(€€€€€€€‰½½°Á…ÉÍ•‘¹ä€ô™…±Í”ì(€€€€€€€±½¹œ±½¹œ…ÕµÕ±…Ñ•‘5¥¹ÕÑ•Ì€ô€Àì((€€€€€€€™½È€¡Í¥é•}Ð¥¹‘•à€ôÍÑ…ÉÑ%¹‘•àì¥¹‘•à€ðÑ½­•¹Ì¹Í¥é” ¤ì¤(€€€€€€€ì(€€€€€€€€€€€¥˜€¡Ñ½­•¹Ím¥¹‘•át€ôô0‰…¹ˆ¤(€€€€€€€€€€€ì(€€€€€€€€€€€€€€€¥˜€ …Á…ÉÍ•‘¹ä¤(€€€€€€€€€€€€€€€ì(€€€€€€€€€€€€€€€€€€€É•ÑÕÉ¸™…±Í”ì(€€€€€€€€€€€€€€€ô(€€€€€€€€€€€€€€€€¬­¥¹‘•àì(€€€€€€€€€€€€€€€½¹Ñ¥¹Õ”ì(€€€€€€€€€€€ô((€€€€€€€€€€€¥˜€¡Ñ½­•¹Ím¥¹‘•át€ôô0‰¡…±˜ˆ¤(€€€€€€€€€€€ì(€€€€€€€€€€€€€€€Í¥é•}ÐÕ¹¥Ñ%¹‘•à€ô¥¹‘•à€¬€Äì(€€€€€€€€€€€€€€€¥˜€¡Õ¹¥Ñ%¹‘•à€ðÑ½­•¹Ì¹Í¥é” ¤€˜˜€¡Ñ½­•¹ÍmÕ¹¥Ñ%¹‘•át€ôô0‰„ˆñðÑ½­•¹ÍmÕ¹¥Ñ%¹‘•át€ôô0‰…¸ˆ¤¤(€€€€€€€€€€€€€€€ì(€€€€€€€€€€€€€€€€€€€€¬­Õ¹¥Ñ%¹‘•àì(€€€€€€€€€€€€€€€ô(€€€€€€€€€€€€€€€¥˜€¡Õ¹¥Ñ%¹‘•à€ðÑ½­•¹Ì¹Í¥é” ¤¤(€€€€€€€€€€€€€€€ì(€€€€€€€€€€€€€€€€€€€½¹ÍÐ¥¹ÐÕ¹¥Ñ5¥¹ÕÑ•Ì€ôÕ¹¥ÑQ½5¥¹ÕÑ•Ì¡Ñ½­•¹ÍmÕ¹¥Ñ%¹‘•át¤ì(€€€€€€€€€€€€€€€€€€€¥˜€¡Õ¹¥Ñ5¥¹ÕÑ•Ì€ø€À¤(€€€€€€€€€€€€€€€€€€€ì(€€€€€€€€€€€€€€€€€€€€€€€¥˜€ ……‘‘5¥¹ÕÑ•Ì¡…ÕµÕ±…Ñ•‘5¥¹ÕÑ•Ì°ÍÑèéµ…à Ä°Õ¹¥Ñ5¥¹ÕÑ•Ì€¼€È¤°€Ä¤¤(€€€€€€€€€€€€€€€€€€€€€€€ì(€€€€€€€€€€€€€€€€€€€€€€€€€€€É•ÑÕÉ¸™…±Í”ì(€€€€€€€€€€€€€€€€€€€€€€€ô(€€€€€€€€€€€€€€€€€€€€€€€Á…ÉÍ•‘¹ä€ôÑÉÕ”ì(€€€€€€€€€€€€€€€€€€€€€€€¥¹‘•à€ôÕ¹¥Ñ%¹‘•à€¬€Äì(€€€€€€€€€€€€€€€€€€€€€€€½¹Ñ¥¹Õ”ì(€€€€€€€€€€€€€€€€€€€ô(€€€€€€€€€€€€€€€ô(€€€€€€€€€€€ô((€€€€€€€€€€€¥¹Ð…µ½Õ¹Ð€ô€Àì(€€€€€€€€€€€Í¥é•}Ð…µ½Õ¹ÑQ½­•¹Ì€ô€Àì(€€€€€€€€€€€¥˜€¡Á…ÉÍ•µ½Õ¹ÑÐ¡¥¹‘•à°…µ½Õ¹Ð°…µ½Õ¹ÑQ½­•¹Ì¤€˜˜(€€€€€€€€€€€€€€€¥¹‘•à€¬…µ½Õ¹ÑQ½­•¹Ì€ðÑ½­•¹Ì¹Í¥é” ¤¤(€€€€€€€€€€€ì(€€€€€€€€€€€€€€€½¹ÍÐ¥¹ÐÕ¹¥Ñ5¥¹ÕÑ•Ì€ôÕ¹¥ÑQ½5¥¹ÕÑ•Ì¡Ñ½­•¹Ím¥¹‘•à€¬…µ½Õ¹ÑQ½­•¹Ít¤ì(€€€€€€€€€€€€€€€¥˜€¡Õ¹¥Ñ5¥¹ÕÑ•Ì€ø€À¤(€€€€€€€€€€€€€€€ì(€€€€€€€€€€€€€€€€€€€¥˜€ ……‘‘5¥¹ÕÑ•Ì¡…ÕµÕ±…Ñ•‘5¥¹ÕÑ•Ì°…µ½Õ¹Ð°Õ¹¥Ñ5¥¹ÕÑ•Ì¤¤(€€€€€€€€€€€€€€€€€€€ì(€€€€€€€€€€€€€€€€€€€€€€€É•ÑÕÉ¸™…±Í”ì(€€€€€€€€€€€€€€€€€€€ô(€€€€€€€€€€€€€€€€€€€Á…ÉÍ•‘¹ä€ôÑÉÕ”ì(€€€€€€€€€€€€€€€€€€€¥¹‘•à€¬ô…µ½Õ¹ÑQ½­•¹Ì€¬€Äì(€€€€€€€€€€€€€€€€€€€½¹Ñ¥¹Õ”ì(€€€€€€€€€€€€€€€ô(€€€€€€€€€€€ô((€€€€€€€€€€€¥¹Ð½µÁ…Ñ5¥¹ÕÑ•Ì€ô€Àì(€€€€€€€€€€€¥˜€¡Á…ÉÍ•½µÁ…ÑÕÉ…Ñ¥½¹Q½­•¸¡Ñ½­•¹Ím¥¹‘•át°½µÁ…Ñ5¥¹ÕÑ•Ì¤¤(€€€€€€€€€€€ì(€€€€€€€€€€€€€€€¥˜€ ……‘‘5¥¹ÕÑ•Ì¡…ÕµÕ±…Ñ•‘5¥¹ÕÑ•Ì°½µÁ…Ñ5¥¹ÕÑ•Ì°€Ä¤¤(€€€€€€€€€€€€€€€ì(€€€€€€€€€€€€€€€€€€€É•ÑÕÉ¸™…±Í”ì(€€€€€€€€€€€€€€€ô(€€€€€€€€€€€€€€€Á…ÉÍ•‘¹ä€ôÑÉÕ”ì(€€€€€€€€€€€€€€€€¬­¥¹‘•àì(€€€€€€€€€€€€€€€½¹Ñ¥¹Õ”ì(€€€€€€€€€€€ô((€€€€€€€€€€€¥˜€¡Á…ÉÍ•‘¹ä€˜˜(€€€€€€€€€€€€€€€¥¹‘•à€¬€È€ðÑ½­•¹Ì¹Í¥é” ¤€˜˜(€€€€€€€€€€€€€€€€¡Ñ½­•¹Ím¥¹‘•át€ôô0‰„ˆñðÑ½­•¹Ím¥¹‘•át€ôô0‰…¸ˆ¤€˜˜(€€€€€€€€€€€€€€€Ñ½­•¹Ím¥¹‘•à€¬€Åt€ôô0‰¡…±˜ˆ¤(€€€€€€€€€€€ì(€€€€€€€€€€€€€€€½¹ÍÐ¥¹ÐÕ¹¥Ñ5¥¹ÕÑ•Ì€ôÕ¹¥ÑQ½5¥¹ÕÑ•Ì¡Ñ½­•¹Ím¥¹‘•à€¬€Ét¤ì(€€€€€€€€€€€€€€€¥˜€¡Õ¹¥Ñ5¥¹ÕÑ•Ì€ø€À¤(€€€€€€€€€€€€€€€ì(€€€€€€€€€€€€€€€€€€€¥˜€ ……‘‘5¥¹ÕÑ•Ì¡…ÕµÕ±…Ñ•‘5¥¹ÕÑ•Ì°ÍÑèéµ…à Ä°Õ¹¥Ñ5¥¹ÕÑ•Ì€¼€È¤°€Ä¤¤(€€€€€€€€€€€€€€€€€€€ì(€€€€€€€€€€€€€€€€€€€€€€€É•ÑÕÉ¸™…±Í”ì(€€€€€€€€€€€€€€€€€€€ô(€€€€€€€€€€€€€€€€€€€¥¹‘•à€¬ô€Ìì(€€€€€€€€€€€€€€€€€€€½¹Ñ¥¹Õ”ì(€€€€€€€€€€€€€€€ô(€€€€€€€€€€€ô((€€€€€€€€€€€‰É•…¬ì(€€€€€€€ô((€€€€€€€¥˜€ …Á…ÉÍ•‘¹äñð…ÕµÕ±…Ñ•‘5¥¹ÕÑ•Ì€ðô€Àñð…ÕµÕ±…Ñ•‘5¥¹ÕÑ•Ì€ø­5…á9…ÑÕÉ…±I•µ¥¹‘•É5¥¹ÕÑ•Ì¤(€€€€€€€ì(€€€€€€€€€€€Ñ½Ñ…±5¥¹ÕÑ•Ì€ô€Àì(€€€€€€€€€€€É•ÑÕÉ¸™…±Í”ì(€€€€€€€ô(€€€€€€€Ñ½Ñ…±5¥¹ÕÑ•Ì€ôÍÑ…Ñ¥}…ÍÐñ¥¹Ðø¡…ÕµÕ±…Ñ•‘5¥¹ÕÑ•Ì¤ì(€€€€€€€É•ÑÕÉ¸ÑÉÕ”ì(€€€ôì((€€€™½È€¡Í¥é•}Ð¥¹‘•à€ô€Àì¥¹‘•à€ðÑ½­•¹Ì¹Í¥é” ¤ì€¬­¥¹‘•à¤(€€€ì(€€€€€€€¥¹ÐÑ½Ñ…±5¥¹ÕÑ•Ì€ô€Àì(€€€€€€€¥˜€¡Ñ½­•¹Ím¥¹‘•át€ôô0‰¥¸ˆ€˜˜(€€€€€€€€€€€¥¹‘•à€¬€Ä€ðÑ½­•¹Ì¹Í¥é” ¤€˜˜(€€€€€€€€€€€Á…ÉÍ•I•±…Ñ¥Ù•ÕÉ…Ñ¥½¸¡¥¹‘•à€¬€Ä°Ñ½Ñ…±5¥¹ÕÑ•Ì¤¤(€€€€€€€ì(€€€€€€€€€€€‘Õ•Ð€ô¹½Ü€¬ÍÑèé¡É½¹¼èéµ¥¹ÕÑ•Ì¡Ñ½Ñ…±5¥¹ÕÑ•Ì¤ì(€€€€€€€€€€€…±±…ä€ô™…±Í”ì(€€€€€€€€€€€É•ÑÕÉ¸ÑÉÕ”ì(€€€€€€€ô(€€€€€€€¥˜€¡Á…ÉÍ•I•±…Ñ¥Ù•ÕÉ…Ñ¥½¸¡¥¹‘•à°Ñ½Ñ…±5¥¹ÕÑ•Ì¤¤(€€€€€€€ì(€€€€€€€€€€€‘Õ•Ð€ô¹½Ü€¬ÍÑèé¡É½¹¼èéµ¥¹ÕÑ•Ì¡Ñ½Ñ…±5¥¹ÕÑ•Ì¤ì(€€€€€€€€€€€…±±…ä€ô™…±Í”ì(€€€€€€€€€€€É•ÑÕÉ¸ÑÉÕ”ì(€€€€€€€ô(€€€ô((€€€™½È€¡Í¥é•}Ð¥¹‘•à€ô€Àì¥¹‘•à€ðÑ½­•¹Ì¹Í¥é” ¤ì€¬­¥¹‘•à¤(€€€ì(€€€€€€€¥˜€¡Ñ½­•¹Ím¥¹‘•át€„ô0‰½¸ˆ¤(€€€€€€€ì(€€€€€€€€€€€½¹Ñ¥¹Õ”ì(€€€€€€€ô((€€€€€€€Í¥é•}Ð‘…å%¹‘•à€ô¥¹‘•à€¬€Äì(€€€€€€€¥˜€¡‘…å%¹‘•à€ðÑ½­•¹Ì¹Í¥é” ¤€˜˜Ñ½­•¹Ím‘…å%¹‘•át€ôô0‰Ñ¡”ˆ¤(€€€€€€€ì(€€€€€€€€€€€€¬­‘…å%¹‘•àì(€€€€€€€ô((€€€€€€€¥¹Ð‘…ä€ô€Àì(€€€€€€€¥˜€¡‘…å%¹‘•à€øôÑ½­•¹Ì¹Í¥é” ¤ñð€…Á…ÉÍ•…å=™5½¹Ñ¡Q½­•¸¡Ñ½­•¹Ím‘…å%¹‘•át°‘…ä¤¤(€€€€€€€ì(€€€€€€€€€€€½¹Ñ¥¹Õ”ì(€€€€€€€ô((€€€€€€€ÍÑèéÑ´±½…°€ô1½…±Q´¡¹½Ü¤ì(€€€€€€€±½…°¹Ñµ}µ‘…ä€ô‘…äì(€€€€€€€±½…°¹Ñµ}¡½ÕÈ€ô€äì(€€€€€€€±½…°¹Ñµ}µ¥¸€ô€Àì(€€€€€€€±½…°¹Ñµ}Í•Œ€ô€Àì((€€€€€€€‰½½°Á…ÉÍ•‘±±…ä€ôÑÉÕ”ì(€€€€€€€…ÁÁ±åQ¥µ•™Ñ•ÉQ½­•¸¡‘…å%¹‘•à€¬€Ä°±½…°°Á…ÉÍ•‘±±…ä¤ì((€€€€€€€¥¹Ðå•…È€ô±½…°¹Ñµ}å•…È€¬€ÄäÀÀì(€€€€€€€¥¹Ðµ½¹Ñ €ô±½…°¹Ñµ}µ½¸€¬€Äì(€€€€€€€¥˜€¡‘…ä€ø…åÍ%¹5½¹Ñ ¡å•…È°µ½¹Ñ ¤¤(€€€€€€€ì(€€€€€€€€€€€½¹Ñ¥¹Õ”ì(€€€€€€€ô((€€€€€€€‘Õ•Ð€ôÉ½µ1½…±Q´¡±½…°¤ì(€€€€€€€¥˜€¡‘Õ•Ð€ðô¹½Ü¤(€€€€€€€ì(€€€€€€€€€€€€¬­±½…°¹Ñµ}µ½¸ì(€€€€€€€€€€€å•…È€ô±½…°¹Ñµ}å•…È€¬€ÄäÀÀ€¬€¡±½…°¹Ñµ}µ½¸€¼€ÄÈ¤ì(€€€€€€€€€€€µ½¹Ñ €ô€¡±½…°¹Ñµ}µ½¸€”€ÄÈ¤€¬€Äì(€€€€€€€€€€€¥˜€¡‘…ä€ø…åÍ%¹5½¹Ñ ¡å•…È°µ½¹Ñ ¤¤(€€€€€€€€€€€ì(€€€€€€€€€€€€€€€½¹Ñ¥¹Õ”ì(€€€€€€€€€€€ô(€€€€€€€€€€€‘Õ•Ð€ôÉ½µ1½…±Q´¡±½…°¤ì(€€€€€€€ô((€€€€€€€…±±…ä€ôÁ…ÉÍ•‘±±…äì(€€€€€€€É•ÑÕÉ¸ÑÉÕ”ì(€€€ô((€€€¥˜€¡±½Ý•È¹™¥¹¡0‰Ñ½µ½ÉÉ½Üˆ¤€„ôÍÑèéÝÍÑÉ¥¹œèé¹Á½Ì¤(€€€ì(€€€€€€€ÍÑèéÑ´±½…°€ô1½…±Q´¡¹½Ü€¬ÍÑèé¡É½¹¼èé¡½ÕÉÌ ÈÐ¤¤ì(€€€€€€€±½…°¹Ñµ}¡½ÕÈ€ô€äì(€€€€€€€±½…°¹Ñµ}µ¥¸€ô€Àì(€€€€€€€±½…°¹Ñµ}Í•Œ€ô€Àì(€€€€€€€½¹ÍÐÍ¥é•}Ð…ÑA½Í¥Ñ¥½¸€ô±½Ý•È¹™¥¹¡0ˆ…Ð€ˆ¤ì(€€€€€€€¥˜€¡…ÑA½Í¥Ñ¥½¸€„ôÍÑèéÝÍÑÉ¥¹œèé¹Á½Ì¤(€€€€€€€ì(€€€€€€€€€€€¥¹Ð¡½ÕÈ€ô€Àì(€€€€€€€€€€€¥¹Ðµ¥¹ÕÑ”€ô€Àì(€€€€€€€€€€€¥˜€¡A…ÉÍ•!½ÕÉ5¥¹ÕÑ•Q•áÐ¡±½Ý•È¹ÍÕ‰ÍÑÈ¡…ÑA½Í¥Ñ¥½¸€¬€Ð¤°¡½ÕÈ°µ¥¹ÕÑ”¤¤(€€€€€€€€€€€ì(€€€€€€€€€€€€€€€±½…°¹Ñµ}¡½ÕÈ€ô¡½ÕÈì(€€€€€€€€€€€€€€€±½…°¹Ñµ}µ¥¸€ôµ¥¹ÕÑ”ì(€€€€€€€€€€€€€€€…±±…ä€ô™…±Í”ì(€€€€€€€€€€€ô(€€€€€€€ô(€€€€€€€•±Í”(€€€€€€€ì(€€€€€€€€€€€…±±…ä€ôÑÉÕ”ì(€€€€€€€ô(€€€€€€€‘Õ•Ð€ôÉ½µ1½…±Q´¡±½…°¤ì(€€€€€€€É•ÑÕÉ¸ÑÉÕ”ì(€€€ô((€€€ÍÑ…Ñ¥Œ½¹ÍÐÍÑèéÙ•Ñ½ÈñÍÑèéÁ…¥ÈñÍÑèéÝÍÑÉ¥¹œ°¥¹ÐøøÝ••­‘…åÌì(€€€€€€€ì0‰ÍÕ¹‘…äˆ°€Àô°ì0‰µ½¹‘…äˆ°€Äô°ì0‰ÑÕ•Í‘…äˆ°€Èô°ì0‰Ý•‘¹•Í‘…äˆ°€Ìô°(€€€€€€€ì0‰Ñ¡ÕÉÍ‘…äˆ°€Ðô°ì0‰™É¥‘…äˆ°€Ôô°ì0‰Í…ÑÕÉ‘…äˆ°€Øô(€€€ôì(€€€™½È€¡½¹ÍÐ…ÕÑ¼˜Ý••­‘…ä€èÝ••­‘…åÌ¤(€€€ì(€€€€€€€½¹ÍÐÍÑèéÝÍÑÉ¥¹œÁ…ÑÑ•É¸€ô0‰¹•áÐ€ˆ€¬Ý••­‘…ä¹™¥ÉÍÐì(€€€€€€€¥˜€¡±½Ý•È¹™¥¹¡Á…ÑÑ•É¸¤€„ôÍÑèéÝÍÑÉ¥¹œèé¹Á½Ì¤(€€€€€€€ì(€€€€€€€€€€€ÍÑèéÑ´±½…°€ô1½…±Q´¡¹½Ü¤ì(€€€€€€€€€€€¥¹Ð‘…åÍ¡•…€ô€¡Ý••­‘…ä¹Í•½¹€´±½…°¹Ñµ}Ý‘…ä€¬€Ü¤€”€Üì(€€€€€€€€€€€¥˜€¡‘…åÍ¡•…€ôô€À¤(€€€€€€€€€€€ì(€€€€€€€€€€€€€€€‘…åÍ¡•…€ô€Üì(€€€€€€€€€€€ô(€€€€€€€€€€€‘Õ•Ð€ôMÑ…ÉÑ=™1½…±…ä¡¹½Ü¤€¬ÍÑèé¡É½¹¼èé¡½ÕÉÌ ÈÐ€¨‘…åÍ¡•…€¬€ä¤ì(€€€€€€€€€€€…±±…ä€ôÑÉÕ”ì(€€€€€€€€€€€É•ÑÕÉ¸ÑÉÕ”ì(€€€€€€€ô(€€€ô((€€€ÍÑèéÝ¥ÍÑÉ¥¹ÍÑÉ•…´µ½¹Ñ¡MÑÉ•…´¡±½Ý•È¤ì(€€€ÍÑèéÝÍÑÉ¥¹œÝ½Éì(€€€Ý¡¥±”€¡µ½¹Ñ¡MÑÉ•…´€øøÝ½É¤(€€€ì(€€€€€€€½¹ÍÐ…ÕÑ¼µ½¹Ñ €ô5½¹Ñ¡É½µ9…µ”¡Ý½É¤ì(€€€€€€€¥˜€ …µ½¹Ñ ¤(€€€€€€€ì(€€€€€€€€€€€½¹Ñ¥¹Õ”ì(€€€€€€€ô(€€€€€€€¥¹Ð‘…ä€ô€Àì(€€€€€€€¥˜€ „¡µ½¹Ñ¡MÑÉ•…´€øø‘…ä¤ñð‘…ä€ð€Äñð‘…ä€ø€ÌÄ¤(€€€€€€€ì(€€€€€€€€€€€½¹Ñ¥¹Õ”ì(€€€€€€€ô(€€€€€€€ÍÑèéÑ´±½…°€ô1½…±Q´¡¹½Ü¤ì(€€€€€€€¥¹Ðå•…È€ô±½…°¹Ñµ}å•…È€¬€ÄäÀÀì(€€€€€€€¥˜€¡±½Ý•È¹™¥¹¡0‰¹•áÐå•…Èˆ¤€„ôÍÑèéÝÍÑÉ¥¹œèé¹Á½Ì¤(€€€€€€€ì(€€€€€€€€€€€€¬­å•…Èì(€€€€€€€ô(€€€€€€€¥˜€¡‘…ä€ø…åÍ%¹5½¹Ñ ¡å•…È°€©µ½¹Ñ ¤¤(€€€€€€€ì(€€€€€€€€€€€½¹Ñ¥¹Õ”ì(€€€€€€€ô(€€€€€€€ÍÑèéÑ´Ñ…É•Ðíôì(€€€€€€€Ñ…É•Ð¹Ñµ}å•…È€ôå•…È€´€ÄäÀÀì(€€€€€€€Ñ…É•Ð¹Ñµ}µ½¸€ô€©µ½¹Ñ €´€Äì(€€€€€€€Ñ…É•Ð¹Ñµ}µ‘…ä€ô‘…äì(€€€€€€€Ñ…É•Ð¹Ñµ}¡½ÕÈ€ô€äì(€€€€€€€‘Õ•Ð€ôÉ½µ1½…±Q´¡Ñ…É•Ð¤ì(€€€€€€€¥˜€¡‘Õ•Ð€ðô¹½Ü¤(€€€€€€€ì(€€€€€€€€€€€Ñ…É•Ð¹Ñµ}å•…È€¬ô€Äì(€€€€€€€€€€€‘Õ•Ð€ôÉ½µ1½…±Q´¡Ñ…É•Ð¤ì(€€€€€€€ô(€€€€€€€…±±…ä€ôÑÉÕ”ì(€€€€€€€É•ÑÕÉ¸ÑÉÕ”ì(€€€ô((€€€€¼¼Ñ¥Ñ±”…¸ÍÁ•¥™ä½¹±ä„±½¬Ñ¥µ”°ÍÕ …Ì€‰¼Ñ¼Ñ¡”å´…Ð€ÄèÌÀˆ¸(€€€€¼¼UÍ”Ñ¡”¹•áÐ½ÕÉÉ•¹”Í¼…¸…±É•…‘äµÁ…ÍÍ•Ñ¥µ”¹…ÑÕÉ…±±ä±…¹‘ÌÑ½µ½ÉÉ½Ü¸(€€€™½È€¡Í¥é•}Ð¥¹‘•à€ô€Àì¥¹‘•à€¬€Ä€ðÑ½­•¹Ì¹Í¥é” ¤ì€¬­¥¹‘•à¤(€€€ì(€€€€€€€¥˜€¡Ñ½­•¹Ím¥¹‘•át€„ô0‰…Ðˆ¤(€€€€€€€ì(€€€€€€€€€€€½¹Ñ¥¹Õ”ì(€€€€€€€ô((€€€€€€€ÍÑèéÝÍÑÉ¥¹œÑ¥µ•Q•áÐ€ôÑ½­•¹Ím¥¹‘•à€¬€Åtì(€€€€€€€¥˜€¡¥¹‘•à€¬€È€ðÑ½­•¹Ì¹Í¥é” ¤€˜˜€¡Ñ½­•¹Ím¥¹‘•à€¬€Ét€ôô0‰…´ˆñðÑ½­•¹Ím¥¹‘•à€¬€Ét€ôô0‰Á´ˆ¤¤(€€€€€€€ì(€€€€€€€€€€€Ñ¥µ•Q•áÐ€¬ôÑ½­•¹Ím¥¹‘•à€¬€Étì(€€€€€€€ô((€€€€€€€¥¹Ð¡½ÕÈ€ô€Àì(€€€€€€€¥¹Ðµ¥¹ÕÑ”€ô€Àì(€€€€€€€¥˜€ …A…ÉÍ•!½ÕÉ5¥¹ÕÑ•Q•áÐ¡Ñ¥µ•Q•áÐ°¡½ÕÈ°µ¥¹ÕÑ”¤¤(€€€€€€€ì(€€€€€€€€€€€½¹Ñ¥¹Õ”ì(€€€€€€€ô((€€€€€€€ÍÑèéÑ´±½…°€ô1½…±Q´¡¹½Ü¤ì(€€€€€€€±½…°¹Ñµ}¡½ÕÈ€ô¡½ÕÈì(€€€€€€€±½…°¹Ñµ}µ¥¸€ôµ¥¹ÕÑ”ì(€€€€€€€±½…°¹Ñµ}Í•Œ€ô€Àì(€€€€€€€‘Õ•Ð€ôÉ½µ1½…±Q´¡±½…°¤ì(€€€€€€€¥˜€¡‘Õ•Ð€ðô¹½Ü¤(€€€€€€€ì(€€€€€€€€€€€±½…°¹Ñµ}µ‘…ä€¬ô€Äì(€€€€€€€€€€€‘Õ•Ð€ôÉ½µ1½…±Q´¡±½…°¤ì(€€€€€€€ô((€€€€€€€…±±…ä€ô™…±Í”ì(€€€€€€€É•ÑÕÉ¸ÑÉÕ”ì(€€€ô((€€€É•ÑÕÉ¸™…±Í”ì)ô()ÍÑèéÝÍÑÉ¥¹œI•µ¥¹‘•ÉM•ÉÙ¥”èéAÉ¥½É¥Ñå1…‰•°¡I•µ¥¹‘•ÉAÉ¥½É¥ÑäÁÉ¥½É¥Ñä¤)ì(€€€ÍÝ¥Ñ €¡ÁÉ¥½É¥Ñä¤(€€€ì(€€€…Í”I•µ¥¹‘•ÉAÉ¥½É¥Ñäèé1½Üè(€€€€€€€É•ÑÕÉ¸0‰1½Üˆì(€€€…Í”I•µ¥¹‘•ÉAÉ¥½É¥Ñäèé9½Éµ…°è(€€€€€€€É•ÑÕÉ¸0‰9½Éµ…°ˆì(€€€…Í”I•µ¥¹‘•ÉAÉ¥½É¥Ñäèé!¥ è(€€€€€€€É•ÑÕÉ¸0‰!¥ ˆì(€€€ô(€€€É•ÑÕÉ¸0‰9½Éµ…°ˆì)ô()ÍÑèéÝÍÑÉ¥¹œI•µ¥¹‘•ÉM•ÉÙ¥”èéI•Á•…ÑQåÁ•1…‰•°¡I•µ¥¹‘•ÉI•Á•…ÑQåÁ”É•Á•…Ð¤)ì(€€€ÍÝ¥Ñ €¡É•Á•…Ð¤(€€€ì(€€€…Í”I•µ¥¹‘•ÉI•Á•…ÑQåÁ”èé9½¹”è(€€€€€€€É•ÑÕÉ¸0‰9½¹”ˆì(€€€…Í”I•µ¥¹‘•ÉI•Á•…ÑQåÁ”èé…¥±äè(€€€€€€€É•ÑÕÉ¸0‰…¥±äˆì(€€€…Í”I•µ¥¹‘•ÉI•Á•…ÑQåÁ”èé]••­±äè(€€€€€€€É•ÑÕÉ¸0‰]••­±äˆì(€€€…Í”I•µ¥¹‘•ÉI•Á•…ÑQåÁ”èé5½¹Ñ¡±äè(€€€€€€€É•ÑÕÉ¸0‰5½¹Ñ¡±äˆì(€€€…Í”I•µ¥¹‘•ÉI•Á•…ÑQåÁ”èée•…É±äè(€€€€€€€É•ÑÕÉ¸0‰e•…É±äˆì(€€€ô(€€€É•ÑÕÉ¸0‰9½¹”ˆì)ô()ÍÑèéÝÍÑÉ¥¹œI•µ¥¹‘•ÉM•ÉÙ¥”èéI•Á•…Ñ1…‰•°¡½¹ÍÐI•µ¥¹‘•È˜É•µ¥¹‘•È¤)ì(€€€¥˜€¡É•µ¥¹‘•È¹É•Á•…Ð¹ÑåÁ”€ôôI•µ¥¹‘•ÉI•Á•…ÑQåÁ”èé9½¹”¤(€€€ì(€€€€€€€É•ÑÕÉ¸0‰½•Ì¹½ÐÉ•Á•…Ðˆì(€€€ô(€€€¥˜€¡É•µ¥¹‘•È¹É•Á•…Ð¹ÑåÁ”€ôôI•µ¥¹‘•ÉI•Á•…ÑQåÁ”èé]••­±ä¤(€€€ì(€€€€€€€½¹ÍÐ…ÕÑ¼‘Õ•Ð€ôA…ÉÍ•1½…±%Í¼¡É•µ¥¹‘•È¹‘Õ•Ð¤ì(€€€€€€€¥˜€¡‘Õ•Ð¤(€€€€€€€ì(€€€€€€€€€€€ÍÑ…Ñ¥Œ½¹ÍÑ•áÁÈÝ¡…É}ÐÝ••­‘…åÍmulÄÁtì(€€€€€€€€€€€€€€€0‰MÕ¹‘…äˆ°0‰5½¹‘…äˆ°0‰QÕ•Í‘…äˆ°0‰]•‘¹•Í‘…äˆ°0‰Q¡ÕÉÍ‘…äˆ°0‰É¥‘…äˆ°0‰M…ÑÕÉ‘…äˆ(€€€€€€€€€€€ôì(€€€€€€€€€€€½¹ÍÐÍÑèéÑ´±½…°€ô1½…±Q´ ©‘Õ•Ð¤ì(€€€€€€€€€€€É•ÑÕÉ¸0‰I•Á•…ÑÌ•Ù•Éä€ˆ€¬ÍÑèéÝÍÑÉ¥¹œ¡Ý••­‘…åÍm±½…°¹Ñµ}Ý‘…åt¤ì(€€€€€€€ô(€€€ô(€€€¥˜€¡É•µ¥¹‘•È¹É•Á•…Ð¹ÑåÁ”€ôôI•µ¥¹‘•ÉI•Á•…ÑQåÁ”èé5½¹Ñ¡±ä¤(€€€ì(€€€€€€€½¹ÍÐ…ÕÑ¼‘Õ•Ð€ôA…ÉÍ•1½…±%Í¼¡É•µ¥¹‘•È¹‘Õ•Ð¤ì(€€€€€€€¥˜€¡‘Õ•Ð¤(€€€€€€€ì(€€€€€€€€€€€½¹ÍÐÍÑèéÑ´±½…°€ô1½…±Q´ ©‘Õ•Ð¤ì(€€€€€€€€€€€É•ÑÕÉ¸0‰I•Á•…ÑÌµ½¹Ñ¡±ä½¸Ñ¡”€ˆ€¬ÍÑèéÑ½}ÝÍÑÉ¥¹œ¡±½…°¹Ñµ}µ‘…ä¤ì(€€€€€€€ô(€€€ô(€€€É•ÑÕÉ¸0‰I•Á•…ÑÌ€ˆ€¬Q½1½Ý•È¡I•Á•…ÑQåÁ•1…‰•°¡É•µ¥¹‘•È¹É•Á•…Ð¹ÑåÁ”¤¤ì)ô()ÍÑèéÝÍÑÉ¥¹œI•µ¥¹‘•ÉM•ÉÙ¥”èéMÑ…ÑÕÍ1…‰•°¡I•µ¥¹‘•ÉMÑ…ÑÕÌÍÑ…ÑÕÌ¤)ì(€€€ÍÝ¥Ñ €¡ÍÑ…ÑÕÌ¤(€€€ì(€€€…Í”I•µ¥¹‘•ÉMÑ…ÑÕÌèéUÁ½µ¥¹œè(€€€€€€€É•ÑÕÉ¸0‰UÁ½µ¥¹œˆì(€€€…Í”I•µ¥¹‘•ÉMÑ…ÑÕÌèéÕ•M½½¸è(€€€€€€€É•ÑÕÉ¸0‰Õ”M½½¸ˆì(€€€…Í”I•µ¥¹‘•ÉMÑ…ÑÕÌèéÕ•9½Üè(€€€€€€€É•ÑÕÉ¸0‰Õ”9½Üˆì(€€€…Í”I•µ¥¹‘•ÉMÑ…ÑÕÌèé=Ù•É‘Õ”è(€€€€€€€É•ÑÕÉ¸0‰=Ù•É‘Õ”ˆì(€€€…Í”I•µ¥¹‘•ÉMÑ…ÑÕÌèé½µÁ±•Ñ•è(€€€€€€€É•ÑÕÉ¸0‰½µÁ±•Ñ•ˆì(€€€…Í”I•µ¥¹‘•ÉMÑ…ÑÕÌèéM¹½½é•è(€€€€€€€É•ÑÕÉ¸0‰M¹½½é•ˆì(€€€ô(€€€É•ÑÕÉ¸0‰UÁ½µ¥¹œˆì)ô()ÍÑèéÝÍÑÉ¥¹œI•µ¥¹‘•ÉM•ÉÙ¥”èéÕ•1…‰•°¡½¹ÍÐI•µ¥¹‘•È˜É•µ¥¹‘•È°ÍÑèé¡É½¹¼èéÍåÍÑ•µ}±½¬èéÑ¥µ•}Á½¥¹Ð¹½Ü¤)ì(€€€½¹ÍÐ…ÕÑ¼‘Õ•Ð€ôA…ÉÍ•1½…±%Í¼¡É•µ¥¹‘•È¹‘Õ•Ð¤ì(€€€¥˜€ …‘Õ•Ð¤(€€€ì(€€€€€€€É•ÑÕÉ¸0‰%¹Ù…±¥‘…Ñ”ˆì(€€€ô((€€€½¹ÍÐÍÑèéÑ´‘Õ”€ô1½…±Q´ ©‘Õ•Ð¤ì(€€€½¹ÍÐ‰½½°Ñ½‘…ä€ôM…µ•1½…±…Ñ” ©‘Õ•Ð°¹½Ü¤ì(€€€½¹ÍÐ‰½½°Ñ½µ½ÉÉ½Ü€ôM…µ•1½…±…Ñ” ©‘Õ•Ð°¹½Ü€¬ÍÑèé¡É½¹¼èé¡½ÕÉÌ ÈÐ¤¤ì((€€€Ý¡…É}ÐÑ¥µ•	Õ™™•ÉlÌÉtíôì(€€€ÝÍ™Ñ¥µ”¡Ñ¥µ•	Õ™™•È°ÍÑèéÍ¥é”¡Ñ¥µ•	Õ™™•È¤°0ˆ•$è•4€•Àˆ°€™‘Õ”¤ì(€€€ÍÑèéÝÍÑÉ¥¹œÑ¥µ•Q•áÐ€ôÑ¥µ•	Õ™™•Èì(€€€¥˜€ …Ñ¥µ•Q•áÐ¹•µÁÑä ¤€˜˜Ñ¥µ•Q•áÐ¹™É½¹Ð ¤€ôô0œÀœ¤(€€€ì(€€€€€€€Ñ¥µ•Q•áÐ¹•É…Í”¡Ñ¥µ•Q•áÐ¹‰•¥¸ ¤¤ì(€€€ô((€€€¥˜€¡Ñ½‘…ä¤(€€€ì(€€€€€€€É•ÑÕÉ¸É•µ¥¹‘•È¹…±±…ä€ü0‰Q½‘…äˆ€è0‰Q½‘…ä…Ð€ˆ€¬Ñ¥µ•Q•áÐì(€€€ô(€€€¥˜€¡Ñ½µ½ÉÉ½Ü¤(€€€ì(€€€€€€€É•ÑÕÉ¸É•µ¥¹‘•È¹…±±…ä€ü0‰Q½µ½ÉÉ½Üˆ€è0‰Q½µ½ÉÉ½Ü…Ð€ˆ€¬Ñ¥µ•Q•áÐì(€€€ô((€€€ÍÑèéÝÍÑÉ¥¹œ‘…Ñ•Q•áÐ€ô5½¹Ñ¡9…µ”¡‘Õ”¹Ñµ}µ½¸€¬€Ä¤€¬0ˆ€ˆ€¬ÍÑèéÑ½}ÝÍÑÉ¥¹œ¡‘Õ”¹Ñµ}µ‘…ä¤€¬0ˆ°€ˆ€¬ÍÑèéÑ½}ÝÍÑÉ¥¹œ¡‘Õ”¹Ñµ}å•…È€¬€ÄäÀÀ¤ì(€€€É•ÑÕÉ¸É•µ¥¹‘•È¹…±±…ä€ü‘…Ñ•Q•áÐ€è‘…Ñ•Q•áÐ€¬0ˆ…Ð€ˆ€¬Ñ¥µ•Q•áÐì)ô()ÍÑèéÝÍÑÉ¥¹œI•µ¥¹‘•ÉM•ÉÙ¥”èé½Õ¹Ñ‘½Ý¹1…‰•°¡½¹ÍÐI•µ¥¹‘•È˜É•µ¥¹‘•È°ÍÑèé¡É½¹¼èéÍåÍÑ•µ}±½¬èéÑ¥µ•}Á½¥¹Ð¹½Ü¤)ì(€€€½¹ÍÐ…ÕÑ¼‘Õ•Ð€ôA…ÉÍ•1½…±%Í¼¡É•µ¥¹‘•È¹‘Õ•Ð¤ì(€€€¥˜€ …‘Õ•Ð¤(€€€ì(€€€€€€€É•ÑÕÉ¸0‰%¹Ù…±¥‘…Ñ”ˆì(€€€ô((€€€½¹ÍÐ‰½½°½Ù•É‘Õ”€ô¹½Ü€ø€©‘Õ•Ðì(€€€½¹ÍÐ…ÕÑ¼Í•½¹‘Ì€ôÍÑèé¡É½¹¼èé‘ÕÉ…Ñ¥½¹}…ÍÐñÍÑèé¡É½¹¼èéÍ•½¹‘Ìø¡½Ù•É‘Õ”€ü¹½Ü€´€©‘Õ•Ð€è€©‘Õ•Ð€´¹½Ü¤¹½Õ¹Ð ¤ì(€€€½¹ÍÐ±½¹œ±½¹œ‘…åÌ€ôÍ•½¹‘Ì€¼€àØÐÀÀì(€€€½¹ÍÐ±½¹œ±½¹œ¡½ÕÉÌ€ô€¡Í•½¹‘Ì€”€àØÐÀÀ¤€¼€ÌØÀÀì(€€€½¹ÍÐ±½¹œ±½¹œµ¥¹ÕÑ•Ì€ô€¡Í•½¹‘Ì€”€ÌØÀÀ¤€¼€ØÀì((€€€…ÕÑ¼Õ¹¥Ñ1…‰•°€ômt¡±½¹œ±½¹œÙ…±Õ”°½¹ÍÐÝ¡…É}Ð¨Í¥¹Õ±…È°½¹ÍÐÝ¡…É}Ð¨Á±ÕÉ…°¤(€€€ì(€€€€€€€É•ÑÕÉ¸ÍÑèéÑ½}ÝÍÑÉ¥¹œ¡Ù…±Õ”¤€¬0ˆ€ˆ€¬€¡Ù…±Õ”€ôô€Ä€üÍ¥¹Õ±…È€èÁ±ÕÉ…°¤ì(€€€ôì((€€€ÍÑèéÝÍÑÉ¥¹œ±…‰•°ì(€€€¥˜€¡‘…åÌ€ø€À¤(€€€ì(€€€€€€€±…‰•°€ôÕ¹¥Ñ1…‰•°¡‘…åÌ°0‰‘…äˆ°0‰‘…åÌˆ¤ì(€€€€€€€¥˜€¡¡½ÕÉÌ€ø€À¤(€€€€€€€ì(€€€€€€€€€€€±…‰•°€¬ô0ˆ€ˆ€¬Õ¹¥Ñ1…‰•°¡¡½ÕÉÌ°0‰¡½ÕÈˆ°0‰¡½ÕÉÌˆ¤ì(€€€€€€€ô(€€€ô(€€€•±Í”¥˜€¡¡½ÕÉÌ€ø€À¤(€€€ì(€€€€€€€±…‰•°€ôÕ¹¥Ñ1…‰•°¡¡½ÕÉÌ°0‰¡½ÕÈˆ°0‰¡½ÕÉÌˆ¤ì(€€€€€€€¥˜€¡µ¥¹ÕÑ•Ì€ø€À¤(€€€€€€€ì(€€€€€€€€€€€±…‰•°€¬ô0ˆ€ˆ€¬Õ¹¥Ñ1…‰•°¡µ¥¹ÕÑ•Ì°0‰µ¥¹ÕÑ”ˆ°0‰µ¥¹ÕÑ•Ìˆ¤ì(€€€€€€€ô(€€€ô(€€€•±Í”(€€€ì(€€€€€€€±…‰•°€ôÕ¹¥Ñ1…‰•°¡ÍÑèéµ…àñ±½¹œ±½¹œø Ä°µ¥¹ÕÑ•Ì¤°0‰µ¥¹ÕÑ”ˆ°0‰µ¥¹ÕÑ•Ìˆ¤ì(€€€ô(€€€É•ÑÕÉ¸½Ù•É‘Õ”€ü±…‰•°€¬0ˆ½Ù•É‘Õ”ˆ€è0‰¥¸€ˆ€¬±…‰•°ì)ô()ÍÑèéÝÍÑÉ¥¹œI•µ¥¹‘•ÉM•ÉÙ¥”èé±•ÉÑ1…‰•°¡¥¹Ðµ¥¹ÕÑ•Ì°‰½½°…±±…ä¤)ì(€€€¥˜€¡…±±…ä€˜˜µ¥¹ÕÑ•Ì€øô€ÄÐÐÀ¤(€€€ì(€€€€€€€É•ÑÕÉ¸0ˆÄ‘…ä‰•™½É”ˆì(€€€ô(€€€¥˜€¡µ¥¹ÕÑ•Ì€ðô€À¤(€€€ì(€€€€€€€É•ÑÕÉ¸0‰ÐÑ¥µ”ˆì(€€€ô(€€€¥˜€¡µ¥¹ÕÑ•Ì€ð€ØÀ¤(€€€ì(€€€€€€€É•ÑÕÉ¸ÍÑèéÑ½}ÝÍÑÉ¥¹œ¡µ¥¹ÕÑ•Ì¤€¬0ˆµ¥¹ÕÑ•Ì‰•™½É”ˆì(€€€ô(€€€¥˜€¡µ¥¹ÕÑ•Ì€ôô€ØÀ¤(€€€ì(€€€€€€€É•ÑÕÉ¸0ˆÄ¡½ÕÈ‰•™½É”ˆì(€€€ô(€€€¥˜€¡µ¥¹ÕÑ•Ì€ôô€ÄÐÐÀ¤(€€€ì(€€€€€€€É•ÑÕÉ¸0ˆÄ‘…ä‰•™½É”ˆì(€€€ô(€€€É•ÑÕÉ¸ÍÑèéÑ½}ÝÍÑÉ¥¹œ¡µ¥¹ÕÑ•Ì€¼€ØÀ¤€¬0ˆ¡½ÕÉÌ‰•™½É”ˆì)ô()I•µ¥¹‘•ÉAÉ¥½É¥ÑäI•µ¥¹‘•ÉM•ÉÙ¥”èéAÉ¥½É¥ÑåÉ½µMÑÉ¥¹œ¡½¹ÍÐÍÑèéÝÍÑÉ¥¹œ˜Ù…±Õ”¤)ì(€€€½¹ÍÐÍÑèéÝÍÑÉ¥¹œ±½Ý•È€ôQ½1½Ý•È¡Ù…±Õ”¤ì(€€€¥˜€¡±½Ý•È€ôô0‰±½Üˆ¤(€€€ì(€€€€€€€É•ÑÕÉ¸I•µ¥¹‘•ÉAÉ¥½É¥Ñäèé1½Üì(€€€ô(€€€¥˜€¡±½Ý•È€ôô0‰¡¥ ˆ¤(€€€ì(€€€€€€€É•ÑÕÉ¸I•µ¥¹‘•ÉAÉ¥½É¥Ñäèé!¥ ì(€€€ô(€€€É•ÑÕÉ¸I•µ¥¹‘•ÉAÉ¥½É¥Ñäèé9½Éµ…°ì)ô()I•µ¥¹‘•ÉI•Á•…ÑQåÁ”I•µ¥¹‘•ÉM•ÉÙ¥”èéI•Á•…ÑQåÁ•É½µMÑÉ¥¹œ¡½¹ÍÐÍÑèéÝÍÑÉ¥¹œ˜Ù…±Õ”¤)ì(€€€½¹ÍÐÍÑèéÝÍÑÉ¥¹œ±½Ý•È€ôQ½1½Ý•È¡Ù…±Õ”¤ì(€€€¥˜€¡±½Ý•È€ôô0‰‘…¥±äˆ¤(€€€ì(€€€€€€€É•ÑÕÉ¸I•µ¥¹‘•ÉI•Á•…ÑQåÁ”èé…¥±äì(€€€ô(€€€¥˜€¡±½Ý•È€ôô0‰Ý••­±äˆ¤(€€€ì(€€€€€€€É•ÑÕÉ¸I•µ¥¹‘•ÉI•Á•…ÑQåÁ”èé]••­±äì(€€€ô(€€€¥˜€¡±½Ý•È€ôô0‰µ½¹Ñ¡±äˆ¤(€€€ì(€€€€€€€É•ÑÕÉ¸I•µ¥¹‘•ÉI•Á•…ÑQåÁ”èé5½¹Ñ¡±äì(€€€ô(€€€¥˜€¡±½Ý•È€ôô0‰å•…É±äˆ¤(€€€ì(€€€€€€€É•ÑÕÉ¸I•µ¥¹‘•ÉI•Á•…ÑQåÁ”èée•…É±äì(€€€ô(€€€É•ÑÕÉ¸I•µ¥¹‘•ÉI•Á•…ÑQåÁ”èé9½¹”ì)ô