#pragma once

#include <string>
#include <vector>

struct SemanticVersion
{
    int major = 0;
    int minor = 0;
    int patch = 0;

    static bool TryParse(const std::wstring& text, SemanticVersion& version);
    int CompareTo(const SemanticVersion& other) const;
};

struct UpdateInfo
{
    std::wstring latestVersion;
    std::wstring downloadUrl;
    std::vector<std::wstring> releaseNotes;
};

enum class UpdateCheckStatus
{
    UpToDate,
    UpdateAvailable,
    NetworkError,
    Timeout,
    InvalidUrl,
    InvalidJson,
    MissingLatestVersion,
    MissingDownloadUrl,
    ServerError,
    VersionComparisonFailed
};

struct UpdateCheckResult
{
    UpdateCheckStatus status = UpdateCheckStatus::NetworkError;
    std::wstring currentVersion;
    std::wstring latestVersion;
    std::wstring downloadUrl;
    std::vector<std::wstring> releaseNotes;
    std::wstring errorMessage;
};

class UpdateChecker
{
public:
    UpdateCheckResult CheckForUpdates(const std::wstring& currentVersion, const std::wstring& manifestUrl) const;
    static bool IsSafeHttpUrl(const std::wstring& url);

private:
    static bool FetchUrl(const std::wstring& url, std::string& responseBody, std::wstring& errorMessage, UpdateCheckStatus& errorStatus);
    static bool ParseUpdateInfo(const std::string& json, UpdateInfo& info, UpdateCheckResult& result);
};
