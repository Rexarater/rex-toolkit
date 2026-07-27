#pragma once

#include "MediaDownloadService.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

enum class VideoCompressionMode
{
    Accurate,
    Fast
};

enum class VideoResolutionMode
{
    Auto,
    Original,
    Max1080,
    Max720,
    Max480
};

enum class VideoFpsMode
{
    Auto,
    Original,
    Fps60,
    Fps30,
    Fps24
};

enum class VideoAudioMode
{
    Auto,
    K192,
    K160,
    K128,
    K96,
    K64,
    Mute
};

enum class VideoEncodingPreset
{
    Slow,
    Medium,
    Fast
};

enum class VideoEncoderMode
{
    Cpu,
    AutomaticGpu
};

enum class VideoConflictBehavior
{
    AutoRename,
    Overwrite
};

enum class VideoCompressionPhase
{
    Idle,
    Analyzing,
    Calculating,
    Pass1,
    Pass2,
    Encoding,
    Verifying,
    Retrying,
    Complete,
    Failed,
    Cancelled
};

struct VideoAnalysis
{
    std::filesystem::path filePath;
    std::wstring fileName;
    unsigned long long fileSizeBytes = 0;
    double durationSeconds = 0.0;
    int width = 0;
    int height = 0;
    int videoStreamIndex = -1;
    double fps = 0.0;
    std::wstring videoCodec;
    std::wstring pixelFormat;
    std::wstring audioCodec;
    long long audioBitrate = 0;
    long long totalBitrate = 0;
    bool hasAudio = false;
    int audioStreamCount = 0;
    int subtitleStreamCount = 0;
    std::shared_ptr<const std::vector<double>> frameTimestamps;
};

struct VideoCompressionOptions
{
    unsigned long long targetSizeBytes = 100ULL * 1024ULL * 1024ULL;
    VideoCompressionMode mode = VideoCompressionMode::Accurate;
    std::filesystem::path outputFolder;
    VideoResolutionMode resolutionMode = VideoResolutionMode::Auto;
    VideoFpsMode fpsMode = VideoFpsMode::Auto;
    VideoAudioMode audioMode = VideoAudioMode::Auto;
    VideoEncodingPreset preset = VideoEncodingPreset::Slow;
    VideoEncoderMode encoderMode = VideoEncoderMode::Cpu;
    VideoConflictBehavior conflictBehavior = VideoConflictBehavior::AutoRename;
    bool verifyFinalSize = true;
    bool retryIfTooLarge = true;
    bool preserveMetadata = false;
    int maxRetries = 2;
};

struct VideoCompressionPlan
{
    bool valid = false;
    bool alreadyUnderTarget = false;
    unsigned long long targetSizeBytes = 0;
    unsigned long long safeTargetSizeBytes = 0;
    long long videoBitrate = 0;
    long long audioBitrate = 0;
    int width = 0;
    int height = 0;
    double fps = 0.0;
    std::wstring videoCodec = L"H.264";
    std::wstring encoderName = L"libx264";
    bool hardwareEncoding = false;
    std::wstring audioCodec = L"AAC";
    unsigned long long estimatedOutputSizeBytes = 0;
    std::vector<std::wstring> warnings;
    std::wstring errorMessage;
};

struct VideoCompressionProgress
{
    VideoCompressionPhase phase = VideoCompressionPhase::Idle;
    double progress = 0.0;
    double elapsedSeconds = 0.0;
    double estimatedRemainingSeconds = 0.0;
    unsigned long long currentOutputSizeBytes = 0;
    int retryNumber = 0;
    std::wstring speed;
    std::wstring message;
};

struct VideoCompressionResult
{
    bool success = false;
    bool cancelled = false;
    VideoCompressionPlan plan;
    std::filesystem::path outputPath;
    unsigned long long originalSizeBytes = 0;
    unsigned long long finalSizeBytes = 0;
    int retriesUsed = 0;
    std::wstring errorMessage;
};

class VideoCompressionPlanner
{
public:
    VideoCompressionPlan CreatePlan(const VideoAnalysis& analysis, const VideoCompressionOptions& options) const;
};

class VideoCompressionService
{
public:
    using ProgressCallback = std::function<void(const VideoCompressionProgress&)>;

    ExternalToolStatus CheckExternalTools() const;
    std::wstring SelectAvailableEncoder(
        VideoEncoderMode mode,
        const std::atomic_bool& cancelRequested) const;
    VideoAnalysis Analyze(
        const std::filesystem::path& inputPath,
        const std::atomic_bool& cancelRequested,
        std::wstring& errorMessage) const;
    std::shared_ptr<const std::vector<double>> BuildFrameTimestampIndex(
        const VideoAnalysis& analysis,
        const std::atomic_bool& cancelRequested) const;
    VideoCompressionPlan Plan(const VideoAnalysis& analysis, const VideoCompressionOptions& options) const;
    VideoCompressionResult Compress(
        const VideoAnalysis& analysis,
        const VideoCompressionOptions& options,
        const std::filesystem::path& requestedOutputPath,
        const std::atomic_bool& cancelRequested,
        const ProgressCallback& progressCallback) const;

    static bool IsSupportedInput(const std::filesystem::path& path);
    static std::filesystem::path DefaultOutputPath(
        const VideoAnalysis& analysis,
        const VideoCompressionOptions& options);
    static std::wstring FormatBytes(unsigned long long bytes);
    static std::wstring FormatDuration(double seconds);
    static std::wstring FormatBitrate(long long bitsPerSecond);
    static std::wstring PhaseLabel(VideoCompressionPhase phase);
    static std::wstring ModeLabel(VideoCompressionMode mode);
    static std::wstring ResolutionLabel(VideoResolutionMode mode);
    static std::wstring FpsLabel(VideoFpsMode mode);
    static std::wstring AudioLabel(VideoAudioMode mode);
    static std::wstring PresetLabel(VideoEncodingPreset preset);
    static std::wstring EncoderLabel(VideoEncoderMode mode);

private:
    ExternalToolService externalToolService_;
    ProcessRunner processRunner_;
    VideoCompressionPlanner planner_;
    mutable std::mutex encoderCacheMutex_;
    mutable bool automaticEncoderCached_ = false;
    mutable std::wstring automaticEncoder_ = L"libx264";
};
