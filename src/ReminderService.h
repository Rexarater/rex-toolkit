#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

enum class ReminderPriority
{
    Low,
    Normal,
    High
};

enum class ReminderRepeatType
{
    None,
    Daily,
    Weekly,
    Monthly,
    Yearly
};

enum class ReminderStatus
{
    Upcoming,
    DueSoon,
    DueNow,
    Overdue,
    Completed,
    Snoozed
};

enum class ReminderAlertSeverity
{
    DueSoon,
    DueNow,
    Overdue
};

enum class ReminderFilter
{
    All,
    Today,
    Upcoming,
    Overdue,
    Completed,
    Recurring,
    Birthdays
};

enum class ReminderSort
{
    SoonestFirst,
    Priority,
    CreatedDate,
    Title
};

struct RecurrenceRule
{
    ReminderRepeatType type = ReminderRepeatType::None;
    int interval = 1;
};

struct Reminder
{
    std::wstring id;
    std::wstring title;
    std::wstring description;
    std::wstring dueAt;
    bool allDay = false;
    ReminderPriority priority = ReminderPriority::Normal;
    std::wstring category = L"General";
    RecurrenceRule repeat;
    int alertBeforeMinutes = 15;
    std::vector<int> alertBeforeMinutesList;
    std::wstring snoozedUntil;
    std::wstring createdAt;
    std::wstring updatedAt;
    std::wstring completedAt;
    std::wstring dismissedBannerUntil;
};

struct ReminderList
{
    int version = 1;
    std::vector<Reminder> reminders;
};

struct ReminderAlert
{
    bool hasValue = false;
    std::wstring reminderId;
    std::wstring title;
    std::wstring message;
    std::wstring subtext;
    ReminderAlertSeverity severity = ReminderAlertSeverity::DueSoon;
};

struct ReminderOperationResult
{
    bool success = false;
    std::wstring message;
};

class ReminderStorage
{
public:
    ReminderList Load(const std::filesystem::path& path, std::wstring& warning) const;
    bool Save(const std::filesystem::path& path, const ReminderList& list, std::wstring& errorMessage) const;
};

class ReminderScheduler
{
public:
    static ReminderStatus StatusFor(const Reminder& reminder, std::chrono::system_clock::time_point now);
    static ReminderAlert BuildAlert(const Reminder& reminder, std::chrono::system_clock::time_point now);
    static bool ShouldShowBanner(const Reminder& reminder, std::chrono::system_clock::time_point now);
};

class ReminderBannerManager
{
public:
    static ReminderAlert ChooseAlert(const ReminderList& list, std::chrono::system_clock::time_point now);
};

class ReminderService
{
public:
    ReminderList LoadReminders(const std::filesystem::path& path, std::wstring& warning) const;
    bool SaveReminders(const std::filesystem::path& path, const ReminderList& list, std::wstring& errorMessage) const;

    Reminder CreateReminder(
        const std::wstring& title,
        const std::wstring& description,
        std::chrono::system_clock::time_point dueAt,
        bool allDay,
        ReminderPriority priority,
        const std::wstring& category,
        RecurrenceRule repeat,
        int alertBeforeMinutes) const;

    ReminderOperationResult CompleteReminder(Reminder& reminder, std::chrono::system_clock::time_point now) const;
    ReminderOperationResult SnoozeReminder(Reminder& reminder, std::chrono::system_clock::time_point until) const;
    ReminderOperationResult DismissBanner(Reminder& reminder, std::chrono::system_clock::time_point until) const;

    static std::chrono::system_clock::time_point Now();
    static std::wstring FormatLocalIso(std::chrono::system_clock::time_point value);
    static std::optional<std::chrono::system_clock::time_point> ParseLocalIso(const std::wstring& value);
    static std::optional<std::chrono::system_clock::time_point> ParseDateAndTime(const std::wstring& dateText, const std::wstring& timeText, bool allDay);
    static bool TryParseNaturalDue(const std::wstring& text, std::chrono::system_clock::time_point now, std::chrono::system_clock::time_point& dueAt, bool& allDay);

    static std::wstring PriorityLabel(ReminderPriority priority);
    static std::wstring RepeatLabel(const Reminder& reminder);
    static std::wstring RepeatTypeLabel(ReminderRepeatType repeat);
    static std::wstring StatusLabel(ReminderStatus status);
    static std::wstring DueLabel(const Reminder& reminder, std::chrono::system_clock::time_point now);
    static std::wstring CountdownLabel(const Reminder& reminder, std::chrono::system_clock::time_point now);
    static std::wstring AlertLabel(int minutes, bool allDay);
    static ReminderPriority PriorityFromString(const std::wstring& value);
    static ReminderRepeatType RepeatTypeFromString(const std::wstring& value);

private:
    ReminderStorage storage_;
};
