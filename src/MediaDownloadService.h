#pragma once

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

enum class MediaPlatform
{
    Unknown,
    YouTube,
    SoundCloud
};

enum class MediaType
{
    Unknown,
    Video,
    Audio
};

enum class MediaOutputFormat
{
    Mp4,
    Mp3,
    Wav
};

enum class Mp4Quality
{
    Best = 0,
    P1080 = 1,
    P720 = 2,
    P480 = 3,
    P1440 = 4,
    P2160 = 5,
    P4320 = 6
};

enum class Mp3Bitrate
{
    K320,
    K256,
    K192,
    K128
};

enum class MediaDownloadStatus
{
    Idle,
    Analyzing,
    Ready,
    Downloading,
    Converting,
    Complete,
    Failed,
    Cancelled
};

enum class MediaConflictBehavior
{
    AutoRename
};

struct ExternalToolStatus
{
    bool ytDlpFound = false;
    bool ffmpegFound = false;
    bool essentiaFound = false;
    std::filesystem::path ytDlpPath;
    std::filesystem::path ffmpegPath;
    std::filesystem::path essentiaPath;
};

struct MediaDownloadOptions
{
    MediaOutputFormat outputFormat = MediaOutputFormat::Mp4;
    Mp4Quality mp4Quality = Mp4Quality::Best;
    Mp3Bitrate mp3Bitrate = Mp3Bitrate::K320;
    std::filesystem::path outputFolder;
    std::wstring customFileName;
    MediaConflictBehavior conflictBehavior = MediaConflictBehavior::AutoRename;
};

struct MediaDownloadJob
{
    int id = 0;
    std::wstring url;
    MediaPlatform platform = MediaPlatform::Unknown;
    std::wstring title;
    std::wstring uploader;
    std::wstring duration;
    double durationSeconds = 0.0;
    std::wstring thumbnailUrl;
    std::wstring musicalKey;
    std::wstring bpm;
    std::wstring musicMetadataSource;
    MediaType mediaType = MediaType::Unknown;
    int maxVideoHeight = 0;
    MediaOutputFormat outputFormat = MediaOutputFormat::Mp4;
    Mp4Quality mp4Quality = Mp4Quality::Best;
    Mp3Bitrate mp3Bitrate = Mp3Bitrate::K320;
    std::filesystem::path outputFolder;
    std::filesystem::path outputFilePath;
    MediaDownloadStatus status = MediaDownloadStatus::Idle;
    double progress = 0.0;
    std::wstring speed;
    std::wstring eta;
    std::wstring errorMessage;
};

struct ProcessResult
{
    DWORD exitCode = 1;
    bool cancelled = false;
    std::wstring output;
};

class SupportedPlatformRegistry
{
public:
    static MediaPlatform DetectPlatform(const std::wstring& url);
    static bool IsSupportedUrl(const std::wstring& url);
    static bool IsLikelyDirectMediaUrl(const std::wstring& url);
    static std::wstring PlatformLabel(MediaPlatform platform);
};

class ExternalToolService
{
public:
    ExternalToolStatus CheckTools() const;
    static std::filesystem::path DefaultDownloadsFolder();
};

class ProcessRunner
{
public:
    using OutputCallback = std::function<void(const std::wstring&)>;

    ProcessResult Run(
        const std::filesystem::path& executable,
        const std::vector<std::wstring>& arguments,
        const std::atomic_bool& cancelRequested,
        const OutputCallback& outputCallback) const;
};

class MediaMetadataService
{
public:
    std::optional<MediaDownloadJob> Analyze(
        const std::wstring& url,
        const ExternalToolStatus& tools,
        const std::atomic_bool& cancelRequested,
        std::wstring& errorMessage) const;

private:
    ProcessRunner processRunner_;
};

class MediaDownloadService
{
public:
    using ProgressCallback = std::function<void(const MediaDownloadJob&)>;

    ExternalToolStatus CheckExternalTools() const;
    std::optional<MediaDownloadJob> Analyze(
        const std::wstring& url,
        const std::atomic_bool& cancelRequested,
        std::wstring& errorMessage) const;
    MediaDownloadJob AnalyzeMusic(
        MediaDownloadJob job,
        const std::atomic_bool& cancelRequested,
        std::wstring& errorMessage) const;
    MediaDownloadJob Download(
        MediaDownloadJob job,
        const MediaDownloadOptions& options,
        const std::atomic_bool& cancelRequested,
        const ProgressCallback& progressCallback) const;

    static std::wstring FormatLabel(MediaOutputFormat format);
    static std::wstring Mp4QualityLabel(Mp4Quality quality);
    static std::wstring Mp3BitrateLabel(Mp3Bitrate bitrate);
    static std::wstring MediaTypeLabel(MediaType mediaType);
    static std::wstring StatusLabel(MediaDownloadStatus status);
    static std::wstring SanitizeFileName(const std::wstring& value);

private:
    ExternalToolService externalToolService_;
    MediaMetadataService metadataService_;
    ProcessRunner processRunner_;
};
