#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

enum class AnimeUserStatus
{
    Watching,
    Planned,
    Completed,
    OnHold,
    Dropped
};

struct AnimeDate
{
    int year = 0;
    int month = 0;
    int day = 0;
};

struct AiringInfo
{
    bool hasValue = false;
    int episode = 0;
    long long airingAt = 0;
    int timeUntilAiring = 0;
};

struct AnimeRelation
{
    int anilistId = 0;
    std::wstring relationType;
    std::wstring title;
    std::wstring format;
    std::wstring status;
    int episodes = 0;
    std::wstring season;
    int seasonYear = 0;
    AnimeDate startDate;
    std::wstring coverImageUrl;
    std::wstring siteUrl;
    AiringInfo nextAiringEpisode;
};

struct AnimeSearchResult
{
    int anilistId = 0;
    int idMal = 0;
    std::wstring title;
    std::wstring englishTitle;
    std::wstring nativeTitle;
    std::wstring coverImageUrl;
    std::wstring bannerImageUrl;
    std::wstring description;
    std::wstring format;
    std::wstring status;
    int episodes = 0;
    int duration = 0;
    std::wstring season;
    int seasonYear = 0;
    AnimeDate startDate;
    AnimeDate endDate;
    std::vector<std::wstring> genres;
    int averageScore = 0;
    int popularity = 0;
    std::wstring siteUrl;
    AiringInfo nextAiringEpisode;
    std::vector<AnimeRelation> relations;
};

struct AnimeEntry
{
    int anilistId = 0;
    int idMal = 0;
    std::wstring title;
    std::wstring coverImageUrl;
    std::wstring format;
    std::wstring airingStatus;
    AnimeUserStatus userStatus = AnimeUserStatus::Planned;
    int currentEpisode = 0;
    int totalEpisodes = 0;
    AiringInfo nextAiringEpisode;
    std::vector<AnimeRelation> relations;
    std::wstring notes;
    bool favorite = false;
    std::wstring lastRefreshed;
    std::wstring siteUrl;
};

struct AnimeWatchList
{
    int version = 1;
    std::vector<AnimeEntry> anime;
};

struct AnimeSearchResponse
{
    std::vector<AnimeSearchResult> results;
    int currentPage = 1;
    bool hasNextPage = false;
};

struct AnimeOperationResult
{
    bool success = false;
    std::wstring message;
};

class AniListApiClient
{
public:
    AnimeSearchResponse SearchAnime(const std::wstring& searchText, int page, int perPage, std::wstring& errorMessage) const;
    std::optional<AnimeSearchResult> FetchAnimeById(int anilistId, std::wstring& errorMessage) const;

private:
    bool ExecuteGraphQl(const std::string& requestBody, std::string& responseBody, std::wstring& errorMessage) const;
};

class AnimeListStorage
{
public:
    AnimeWatchList Load(const std::filesystem::path& path, std::wstring& warning) const;
    bool Save(const std::filesystem::path& path, const AnimeWatchList& list, std::wstring& errorMessage) const;
};

class AnimeRelationTracker
{
public:
    static std::vector<AnimeRelation> UpcomingSequels(const AnimeWatchList& list);
    static bool IsUpcomingSequelCandidate(const AnimeRelation& relation);
};

class AnimeTrackerService
{
public:
    AnimeTrackerService();

    AnimeWatchList LoadWatchList(const std::filesystem::path& path, std::wstring& warning) const;
    bool SaveWatchList(const std::filesystem::path& path, const AnimeWatchList& list, std::wstring& errorMessage) const;

    AnimeSearchResponse SearchAnime(const std::wstring& searchText, int page, int perPage, std::wstring& errorMessage) const;
    std::optional<AnimeSearchResult> RefreshAnime(int anilistId, std::wstring& errorMessage) const;

    static AnimeEntry EntryFromSearchResult(const AnimeSearchResult& result);
    static void ApplyMetadata(AnimeEntry& entry, const AnimeSearchResult& result);
    static bool ContainsAnime(const AnimeWatchList& list, int anilistId);
    static std::wstring UserStatusLabel(AnimeUserStatus status);
    static AnimeUserStatus UserStatusFromString(const std::wstring& text);
    static AnimeUserStatus NextUserStatus(AnimeUserStatus status);
    static std::wstring DateLabel(const AnimeDate& date);
    static std::wstring AiringDateLabel(long long unixTimestamp);
    static std::wstring CountdownLabel(long long unixTimestamp);
    static std::wstring IsoNowUtc();

private:
    AniListApiClient apiClient_;
    AnimeListStorage storage_;
};
