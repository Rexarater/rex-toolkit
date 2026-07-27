#include "VideoCompressionService.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <system_error>

namespace
{
constexpr double kSafetyRatio = 0.97;
constexpr long long kMinimumVideoBitrate = 80000;
constexpr double kQualityWarningSizeRatio = 0.85;
constexpr double kHeavyCompressionBitsPerPixel = 0.025;
constexpr double kAggressiveCompressionBitsPerPixel = 0.05;
constexpr long long kHeavyCompressionBitrate = 350000;
constexpr long long kAggressiveCompressionBitrate = 900000;

std::wstring Trim(std::wstring value)
{
    const auto isSpace = [](wchar_t ch) { return std::iswspace(ch) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), isSpace));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), isSpace).base(), value.end());
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"')
    {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

std::wstring Lower(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return value;
}

double ParseDouble(const std::wstring& value)
{
    try
    {
        return std::stod(Trim(value));
    }
    catch (...)
    {
        return 0.0;
    }
}

long long ParseInteger(const std::wstring& value)
{
    try
    {
        return std::stoll(Trim(value));
    }
    catch (...)
    {
        return 0;
    }
}

double ParseRate(const std::wstring& value)
{
    const std::wstring cleaned = Trim(value);
    const size_t slash = cleaned.find(L'/');
    if (slash == std::wstring::npos)
    {
        return ParseDouble(cleaned);
    }
    const double numerator = ParseDouble(cleaned.substr(0, slash));
    const double denominator = ParseDouble(cleaned.substr(slash + 1));
    return denominator > 0.0 ? numerator / denominator : 0.0;
}

int EvenDimension(double value)
{
    int dimension = std::max(2, static_cast<int>(std::round(value)));
    return dimension % 2 == 0 ? dimension : dimension - 1;
}

long long AudioBitrateForMode(VideoAudioMode mode, long long totalBitrate, bool hasAudio)
{
    if (!hasAudio || mode == VideoAudioMode::Mute)
    {
        return 0;
    }
    switch (mode)
    {
    case VideoAudioMode::K192: return 192000;
    case VideoAudioMode::K160: return 160000;
    case VideoAudioMode::K128: return 128000;
    case VideoAudioMode::K96: return 96000;
    case VideoAudioMode::K64: return 64000;
    case VideoAudioMode::Auto:
        if (totalBitrate >= 5000000) return 192000;
        if (totalBitrate >= 2500000) return 160000;
        if (totalBitrate >= 900000) return 128000;
        if (totalBitrate >= 450000) return 96000;
        return 64000;
    case VideoAudioMode::Mute:
        break;
    }
    return 0;
}

int ResolutionCap(VideoResolutionMode mode, long long videoBitrate, int sourceHeight)
{
    switch (mode)
    {
    case VideoResolutionMode::Original: return sourceHeight;
    case VideoResolutionMode::Max1080: return 1080;
    case VideoResolutionMode::Max720: return 720;
    case VideoResolutionMode::Max480: return 480;
    case VideoResolutionMode::Auto:
        if (videoBitrate < 600000) return 480;
        if (videoBitrate < 1600000) return 720;
        if (videoBitrate < 3500000) return 1080;
        return sourceHeight;
    }
    return sourceHeight;
}

double SelectedFps(VideoFpsMode mode, double sourceFps, long long videoBitrate)
{
    switch (mode)
    {
    case VideoFpsMode::Fps60: return std::min(60.0, sourceFps);
    case VideoFpsMode::Fps30: return std::min(30.0, sourceFps);
    case VideoFpsMode::Fps24: return std::min(24.0, sourceFps);
    case VideoFpsMode::Auto:
        if (sourceFps > 50.0 && videoBitrate < 2500000) return 30.0;
        return sourceFps;
    case VideoFpsMode::Original:
        return sourceFps;
    }
    return sourceFps;
}

std::wstring PresetArgument(VideoEncodingPreset preset)
{
    switch (preset)
    {
    case VideoEncodingPreset::Slow: return L"slow";
    case VideoEncodingPreset::Medium: return L"medium";
    case VideoEncodingPreset::Fast: return L"fast";
    }
    return L"slow";
}

std::wstring EncoderName(VideoEncoderMode mode)
{
    (void)mode;
    return L"libx264";
}

void AppendEncoderArguments(
    std::vector<std::wstring>& arguments,
    const std::wstring& encoder,
    VideoEncodingPreset preset)
{
    arguments.insert(arguments.end(), { L"-c:v", encoder });
    if (encoder == L"h264_nvenc")
    {
        arguments.insert(arguments.end(), { L"-preset", preset == VideoEncodingPreset::Fast ? L"p2" : preset == VideoEncodingPreset::Medium ? L"p4" : L"p6" });
    }
    else if (encoder == L"h264_amf")
    {
        arguments.insert(arguments.end(), { L"-quality", preset == VideoEncodingPreset::Fast ? L"speed" : preset == VideoEncodingPreset::Medium ? L"balanced" : L"quality" });
    }
    else
    {
        arguments.insert(arguments.end(), { L"-preset", PresetArgument(preset) });
    }
}

std::wstring BitrateArgument(long long bitrate)
{
    return std::to_wstring(std::max<long long>(1, bitrate / 1000)) + L"k";
}

std::wstring SanitizeStem(std::wstring value)
{
    static constexpr wchar_t invalid[] = L"<>:\"/\\|?*";
    for (wchar_t& ch : value)
    {
        if (wcschr(invalid, ch) || ch < 32)
        {
            ch = L'_';
        }
    }
    while (!value.empty() && (value.back() == L'.' || value.back() == L' '))
    {
        value.pop_back();
    }
    return value.empty() ? L"video" : value;
}

std::filesystem::path ResolveOutputConflict(std::filesystem::path path, VideoConflictBehavior behavior)
{
    std::error_code existsError;
    const bool exists = std::filesystem::exists(path, existsError);
    if (behavior == VideoConflictBehavior::Overwrite || (!existsError && !exists))
    {
        return path;
    }
    const std::filesystem::path parent = path.parent_path();
    const std::wstring stem = path.stem().wstring();
    const std::wstring extension = path.extension().wstring();
    for (int index = 1; index < 10000; ++index)
    {
        std::filesystem::path candidate = parent / (stem + L" (" + std::to_wstring(index) + L")" + extension);
        existsError.clear();
        if (!std::filesystem::exists(candidate, existsError) && !existsError)
        {
            return candidate;
        }
    }
    return parent / (stem + L"_new" + extension);
}

std::filesystem::path CreateWorkingOutputPath(const std::filesystem::path& destination)
{
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::wstring fileName = L".rextoolkit-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(stamp) + L".tmp.mp4";
    return destination.parent_path() / fileName;
}

std::filesystem::path CreateJobTempDirectory()
{
    wchar_t tempPath[MAX_PATH] {};
    if (GetTempPathW(static_cast<DWORD>(std::size(tempPath)), tempPath) == 0)
    {
        return {};
    }
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path result = std::filesystem::path(tempPath) /
        (L"RexToolkitVideoCompression_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(stamp));
    std::error_code error;
    std::filesystem::create_directories(result, error);
    return error ? std::filesystem::path {} : result;
}

void CleanupPath(const std::filesystem::path& path, bool recursive = false)
{
    if (path.empty()) return;
    std::error_code error;
    if (recursive)
    {
        std::filesystem::remove_all(path, error);
    }
    else
    {
        std::filesystem::remove(path, error);
    }
}

void CleanupStaleWorkingOutputs(const std::filesystem::path& folder)
{
    std::error_code error;
    std::filesystem::directory_iterator iterator(folder, error);
    const std::filesystem::directory_iterator end;
    while (!error && iterator != end)
    {
        const std::filesystem::path path = iterator->path();
        const std::wstring name = Lower(path.filename().wstring());
        if (name.rfind(L".rextoolkit-", 0) == 0 && name.size() > 8 &&
            name.compare(name.size() - 8, 8, L".tmp.mp4") == 0)
        {
            std::error_code removeError;
            std::filesystem::remove(path, removeError);
        }
        iterator.increment(error);
    }
}

std::vector<std::wstring> BaseVideoArguments(
    const VideoAnalysis& analysis,
    const VideoCompressionOptions& options,
    const VideoCompressionPlan& plan,
    const std::wstring& encoder,
    long long videoBitrate)
{
    std::vector<std::wstring> arguments {
        L"-hide_banner", L"-loglevel", L"error", L"-y", L"-i", analysis.filePath.wstring(),
        L"-map", L"0:" + std::to_wstring(analysis.videoStreamIndex)
    };
    AppendEncoderArguments(arguments, encoder, options.preset);
    arguments.insert(arguments.end(), { L"-b:v", BitrateArgument(videoBitrate), L"-pix_fmt", L"yuv420p" });
    if (plan.height > 0 && plan.height < analysis.height)
    {
        arguments.insert(arguments.end(), { L"-vf", L"scale=-2:" + std::to_wstring(plan.height) });
    }
    if (plan.fps > 0.0 && analysis.fps > 0.0 && plan.fps + 0.1 < analysis.fps)
    {
        std::wostringstream fps;
        fps << std::fixed << std::setprecision(3) << plan.fps;
        arguments.insert(arguments.end(), { L"-r", fps.str() });
    }
    return arguments;
}

class ProgressParser
{
public:
    ProgressParser(double durationSeconds, double start, double span)
        : duration_(durationSeconds), start_(start), span_(span), current_(start) {}

    void Consume(const std::wstring& chunk, double& progress, std::wstring& speed)
    {
        pending_ += chunk;
        size_t newline = 0;
        while ((newline = pending_.find_first_of(L"\r\n")) != std::wstring::npos)
        {
            const std::wstring line = pending_.substr(0, newline);
            pending_.erase(0, newline + 1);
            if (line.rfind(L"out_time_ms=", 0) == 0 && duration_ > 0.0)
            {
                const double microseconds = ParseDouble(line.substr(12));
                const double phaseProgress = std::clamp(microseconds / (duration_ * 1000000.0), 0.0, 1.0);
                current_ = start_ + phaseProgress * span_;
            }
            else if (line.rfind(L"speed=", 0) == 0)
            {
                speed = Trim(line.substr(6));
            }
        }
        progress = current_;
    }

private:
    double duration_ = 0.0;
    double start_ = 0.0;
    double span_ = 1.0;
    double current_ = 0.0;
    std::wstring pending_;
};
}

VideoCompressionPlan VideoCompressionPlanner::CreatePlan(const VideoAnalysis& analysis, const VideoCompressionOptions& options) const
{
    VideoCompressionPlan plan;
    plan.targetSizeBytes = options.targetSizeBytes;
    if (!std::isfinite(analysis.durationSeconds) || analysis.durationSeconds <= 0.0)
    {
        plan.errorMessage = L"The video duration could not be determined.";
        return plan;
    }
    if (analysis.width <= 0 || analysis.height <= 0)
    {
        plan.errorMessage = L"No usable video stream was found.";
        return plan;
    }
    if (analysis.videoStreamIndex < 0)
    {
        plan.errorMessage = L"The video's main stream could not be identified.";
        return plan;
    }
    if (options.targetSizeBytes < 1024ULL * 1024ULL)
    {
        plan.errorMessage = L"Choose a target size of at least 1 MB.";
        return plan;
    }

    plan.alreadyUnderTarget = analysis.fileSizeBytes > 0 && analysis.fileSizeBytes <= options.targetSizeBytes;
    plan.safeTargetSizeBytes = static_cast<unsigned long long>(static_cast<double>(options.targetSizeBytes) * kSafetyRatio);
    const long double calculatedBitrate = static_cast<long double>(plan.safeTargetSizeBytes) * 8.0L / analysis.durationSeconds;
    if (!std::isfinite(calculatedBitrate) || calculatedBitrate > static_cast<long double>(std::numeric_limits<long long>::max()))
    {
        plan.errorMessage = L"The selected target size is too large for this video.";
        return plan;
    }
    long long totalBitrate = static_cast<long long>(calculatedBitrate);
    if (plan.alreadyUnderTarget && analysis.totalBitrate > 0)
    {
        totalBitrate = std::min(totalBitrate, static_cast<long long>(analysis.totalBitrate * 0.95));
    }
    plan.audioBitrate = AudioBitrateForMode(options.audioMode, totalBitrate, analysis.hasAudio);
    plan.videoBitrate = totalBitrate - plan.audioBitrate;
    if (plan.videoBitrate < kMinimumVideoBitrate)
    {
        plan.errorMessage = L"The target is too small for this video's duration, even at the lowest practical bitrate.";
        return plan;
    }

    const int capHeight = std::min(analysis.height, ResolutionCap(options.resolutionMode, plan.videoBitrate, analysis.height));
    plan.height = EvenDimension(capHeight);
    plan.width = EvenDimension(static_cast<double>(analysis.width) * plan.height / analysis.height);
    plan.fps = SelectedFps(options.fpsMode, analysis.fps, plan.videoBitrate);
    plan.audioCodec = plan.audioBitrate > 0 ? L"AAC" : L"None";
    plan.hardwareEncoding = options.encoderMode != VideoEncoderMode::Cpu;
    plan.encoderName = EncoderName(options.encoderMode);
    plan.videoCodec = plan.hardwareEncoding ? L"H.264 (GPU accelerated)" : L"H.264 (CPU)";
    plan.estimatedOutputSizeBytes = plan.alreadyUnderTarget
        ? std::min(plan.safeTargetSizeBytes, static_cast<unsigned long long>(analysis.fileSizeBytes * 0.98))
        : plan.safeTargetSizeBytes;

    const double bitsPerPixel = plan.width > 0 && plan.height > 0 && plan.fps > 0.0
        ? static_cast<double>(plan.videoBitrate) / (static_cast<double>(plan.width) * plan.height * plan.fps)
        : 0.0;
    const double requestedSizeRatio = analysis.fileSizeBytes > 0
        ? static_cast<double>(options.targetSizeBytes) / static_cast<double>(analysis.fileSizeBytes)
        : 0.0;
    const bool meaningfulSizeReduction = analysis.fileSizeBytes == 0 || requestedSizeRatio < kQualityWarningSizeRatio;
    const bool heavilyConstrained = plan.videoBitrate < kHeavyCompressionBitrate ||
        (bitsPerPixel > 0.0 && bitsPerPixel < kHeavyCompressionBitsPerPixel);
    const bool aggressivelyConstrained = plan.videoBitrate < kAggressiveCompressionBitrate ||
        (bitsPerPixel > 0.0 && bitsPerPixel < kAggressiveCompressionBitsPerPixel);
    if (plan.alreadyUnderTarget)
    {
        plan.warnings.push_back(L"This file is already under the selected target size. No compression is needed.");
    }
    else if (meaningfulSizeReduction && heavilyConstrained)
    {
        plan.warnings.push_back(L"This target is very small for the video's length and resolution. The output may look heavily compressed.");
    }
    else if (meaningfulSizeReduction && aggressivelyConstrained)
    {
        plan.warnings.push_back(L"This target requires aggressive compression. A larger target may noticeably improve quality.");
    }
    if (analysis.audioStreamCount > 1)
    {
        plan.warnings.push_back(L"Multiple audio streams detected. Version 1 uses the first audio stream.");
    }
    if (analysis.subtitleStreamCount > 0)
    {
        plan.warnings.push_back(L"Subtitles were detected but are not preserved in this version.");
    }
    if (plan.hardwareEncoding)
    {
        plan.warnings.push_back(L"GPU encoding is faster, but CPU encoding usually provides better compression at the same size.");
    }
    plan.valid = true;
    return plan;
}

ExternalToolStatus VideoCompressionService::CheckExternalTools() const
{
    return externalToolService_.CheckTools();
}

std::wstring VideoCompressionService::SelectAvailableEncoder(
    VideoEncoderMode mode,
    const std::atomic_bool& cancelRequested) const
{
    if (mode == VideoEncoderMode::Cpu)
    {
        return L"libx264";
    }
    {
        std::lock_guard<std::mutex> lock(encoderCacheMutex_);
        if (automaticEncoderCached_)
        {
            return automaticEncoder_;
        }
    }

    const ExternalToolStatus tools = CheckExternalTools();
    if (!tools.ffmpegFound || cancelRequested.load())
    {
        return L"libx264";
    }

    std::wstring selected = L"libx264";
    for (const std::wstring& candidate : { L"h264_nvenc", L"h264_qsv", L"h264_amf" })
    {
        const std::vector<std::wstring> testArguments {
            L"-hide_banner", L"-loglevel", L"error",
            L"-f", L"lavfi", L"-i", L"color=c=black:s=256x256:d=0.1",
            L"-frames:v", L"1", L"-an", L"-c:v", candidate,
            L"-f", L"null", L"NUL"
        };
        const ProcessResult test = processRunner_.Run(
            tools.ffmpegPath,
            testArguments,
            cancelRequested,
            {});
        if (test.cancelled || cancelRequested.load())
        {
            return L"libx264";
        }
        if (test.exitCode == 0)
        {
            selected = candidate;
            break;
        }
    }

    {
        std::lock_guard<std::mutex> lock(encoderCacheMutex_);
        automaticEncoder_ = selected;
        automaticEncoderCached_ = true;
    }
    return selected;
}

VideoAnalysis VideoCompressionService::Analyze(
    const std::filesystem::path& inputPath,
    const std::atomic_bool& cancelRequested,
    std::wstring& errorMessage) const
{
    VideoAnalysis analysis;
    if (!IsSupportedInput(inputPath))
    {
        errorMessage = L"Choose a supported video file.";
        return analysis;
    }
    std::error_code inputError;
    const bool inputExists = std::filesystem::exists(inputPath, inputError);
    const bool regularFile = inputExists && std::filesystem::is_regular_file(inputPath, inputError);
    if (inputError || !regularFile)
    {
        errorMessage = L"The selected video could not be found.";
        return analysis;
    }

    const ExternalToolStatus tools = CheckExternalTools();
    if (!tools.ffprobeFound)
    {
        errorMessage = L"FFprobe is missing. Restore the bundled tools folder or add FFprobe to PATH.";
        return analysis;
    }

    const std::vector<std::wstring> arguments {
        L"-v", L"error",
        L"-show_entries", L"stream=index,codec_type,codec_name,pix_fmt,width,height,avg_frame_rate,bit_rate:stream_disposition=attached_pic:format=duration,size,bit_rate",
        L"-of", L"flat",
        inputPath.wstring()
    };
    const ProcessResult result = processRunner_.Run(tools.ffprobePath, arguments, cancelRequested, {});
    if (result.cancelled)
    {
        errorMessage = L"Analysis cancelled.";
        return analysis;
    }
    if (result.exitCode != 0)
    {
        errorMessage = L"This video could not be analyzed. It may be corrupt or unsupported.";
        return analysis;
    }

    struct StreamInfo
    {
        int index = -1;
        std::wstring type;
        std::wstring codec;
        std::wstring pixelFormat;
        int width = 0;
        int height = 0;
        double fps = 0.0;
        long long bitrate = 0;
        bool attachedPicture = false;
    };
    std::map<int, StreamInfo> streams;
    std::wistringstream lines(result.output);
    std::wstring line;
    double formatDuration = 0.0;
    long long formatBitrate = 0;
    while (std::getline(lines, line))
    {
        line = Trim(line);
        const size_t equals = line.find(L'=');
        if (equals == std::wstring::npos) continue;
        const std::wstring key = line.substr(0, equals);
        const std::wstring value = Trim(line.substr(equals + 1));
        if (key == L"format.duration") formatDuration = ParseDouble(value);
        else if (key == L"format.bit_rate") formatBitrate = ParseInteger(value);
        else if (key.rfind(L"streams.stream.", 0) == 0)
        {
            const size_t indexStart = 15;
            const size_t propertyDot = key.find(L'.', indexStart);
            if (propertyDot == std::wstring::npos) continue;
            const int index = static_cast<int>(ParseInteger(key.substr(indexStart, propertyDot - indexStart)));
            const std::wstring property = key.substr(propertyDot + 1);
            StreamInfo& stream = streams[index];
            if (property == L"index") stream.index = static_cast<int>(ParseInteger(value));
            else if (property == L"codec_type") stream.type = value;
            else if (property == L"codec_name") stream.codec = value;
            else if (property == L"pix_fmt") stream.pixelFormat = value;
            else if (property == L"width") stream.width = static_cast<int>(ParseInteger(value));
            else if (property == L"height") stream.height = static_cast<int>(ParseInteger(value));
            else if (property == L"avg_frame_rate") stream.fps = ParseRate(value);
            else if (property == L"bit_rate") stream.bitrate = ParseInteger(value);
            else if (property == L"disposition.attached_pic") stream.attachedPicture = ParseInteger(value) != 0;
        }
    }

    for (const auto& pair : streams)
    {
        const StreamInfo& stream = pair.second;
        if (stream.type == L"video" && !stream.attachedPicture && analysis.videoCodec.empty())
        {
            analysis.videoCodec = stream.codec;
            analysis.pixelFormat = stream.pixelFormat;
            analysis.width = stream.width;
            analysis.height = stream.height;
            analysis.fps = stream.fps;
            analysis.videoStreamIndex = stream.index >= 0 ? stream.index : pair.first;
        }
        else if (stream.type == L"audio")
        {
            ++analysis.audioStreamCount;
            if (!analysis.hasAudio)
            {
                analysis.hasAudio = true;
                analysis.audioCodec = stream.codec;
                analysis.audioBitrate = stream.bitrate;
            }
        }
        else if (stream.type == L"subtitle")
        {
            ++analysis.subtitleStreamCount;
        }
    }
    if (analysis.videoCodec.empty() || analysis.width <= 0 || analysis.height <= 0)
    {
        errorMessage = L"The selected file does not contain a usable video stream.";
        return {};
    }
    if (!std::isfinite(formatDuration) || formatDuration <= 0.0)
    {
        errorMessage = L"The video duration is missing or zero.";
        return {};
    }

    std::error_code sizeError;
    analysis.filePath = inputPath;
    analysis.fileName = inputPath.filename().wstring();
    analysis.fileSizeBytes = std::filesystem::file_size(inputPath, sizeError);
    if (sizeError || analysis.fileSizeBytes == 0)
    {
        errorMessage = L"The selected video's file size could not be read.";
        return {};
    }
    analysis.durationSeconds = formatDuration;
    if (formatBitrate > 0)
    {
        analysis.totalBitrate = formatBitrate;
    }
    else
    {
        const long double derivedBitrate = static_cast<long double>(analysis.fileSizeBytes) * 8.0L / analysis.durationSeconds;
        if (!std::isfinite(derivedBitrate) || derivedBitrate > static_cast<long double>(std::numeric_limits<long long>::max()))
        {
            errorMessage = L"The video's bitrate could not be determined safely.";
            return {};
        }
        analysis.totalBitrate = static_cast<long long>(derivedBitrate);
    }
    if (!std::isfinite(analysis.fps) || analysis.fps <= 0.0) analysis.fps = 30.0;
    return analysis;
}

std::shared_ptr<const std::vector<double>> VideoCompressionService::BuildFrameTimestampIndex(
    const VideoAnalysis& analysis,
    const std::atomic_bool& cancelRequested) const
{
    constexpr size_t kMaximumIndexedFrames = 1000000;
    if (analysis.filePath.empty() || analysis.videoStreamIndex < 0 ||
        !std::isfinite(analysis.durationSeconds) || analysis.durationSeconds <= 0.0)
    {
        return {};
    }

    const double estimatedFrames = analysis.durationSeconds * std::max(1.0, analysis.fps);
    if (!std::isfinite(estimatedFrames) || estimatedFrames > kMaximumIndexedFrames)
    {
        return {};
    }

    const ExternalToolStatus tools = CheckExternalTools();
    if (!tools.ffprobeFound)
    {
        return {};
    }

    const std::vector<std::wstring> arguments {
        L"-v", L"error",
        L"-select_streams", std::to_wstring(analysis.videoStreamIndex),
        L"-read_intervals", L"0%+#1000001",
        L"-show_packets",
        L"-show_entries", L"packet=pts_time",
        L"-of", L"csv=p=0",
        analysis.filePath.wstring()
    };
    const ProcessResult result = processRunner_.Run(
        tools.ffprobePath,
        arguments,
        cancelRequested,
        {});
    if (result.cancelled || result.exitCode != 0)
    {
        return {};
    }

    std::vector<double> timestamps;
    timestamps.reserve(static_cast<size_t>(std::min(estimatedFrames + 16.0, 65536.0)));
    std::wistringstream lines(result.output);
    std::wstring line;
    while (std::getline(lines, line))
    {
        std::wistringstream value(line);
        double timestamp = 0.0;
        if (value >> timestamp && std::isfinite(timestamp))
        {
            timestamps.push_back(timestamp);
            if (timestamps.size() > kMaximumIndexedFrames) return {};
        }
    }
    if (timestamps.size() < 2)
    {
        return {};
    }

    std::sort(timestamps.begin(), timestamps.end());
    timestamps.erase(
        std::unique(
            timestamps.begin(),
            timestamps.end(),
            [](double left, double right) { return std::abs(left - right) <= 0.0000001; }),
        timestamps.end());
    const double origin = timestamps.front();
    for (double& timestamp : timestamps) timestamp = std::max(0.0, timestamp - origin);
    return std::make_shared<const std::vector<double>>(std::move(timestamps));
}

VideoCompressionPlan VideoCompressionService::Plan(const VideoAnalysis& analysis, const VideoCompressionOptions& options) const
{
    return planner_.CreatePlan(analysis, options);
}

VideoCompressionResult VideoCompressionService::Compress(
    const VideoAnalysis& analysis,
    const VideoCompressionOptions& options,
    const std::filesystem::path& requestedOutputPath,
    const std::atomic_bool& cancelRequested,
    const ProgressCallback& progressCallback) const
{
    VideoCompressionResult finalResult;
    finalResult.originalSizeBytes = analysis.fileSizeBytes;
    finalResult.plan = Plan(analysis, options);
    if (!finalResult.plan.valid)
    {
        finalResult.errorMessage = finalResult.plan.errorMessage;
        return finalResult;
    }

    std::error_code inputError;
    const bool inputExists = std::filesystem::exists(analysis.filePath, inputError);
    const bool inputIsFile = inputExists && std::filesystem::is_regular_file(analysis.filePath, inputError);
    if (inputError || !inputIsFile)
    {
        finalResult.errorMessage = L"The selected video is no longer available. Add it again and retry.";
        return finalResult;
    }
    const unsigned long long currentInputSize = std::filesystem::file_size(analysis.filePath, inputError);
    if (inputError || currentInputSize == 0 ||
        (analysis.fileSizeBytes > 0 && currentInputSize != analysis.fileSizeBytes))
    {
        finalResult.errorMessage = L"The selected video changed after it was analyzed. Add it again before compressing.";
        return finalResult;
    }

    const ExternalToolStatus tools = CheckExternalTools();
    if (!tools.ffmpegFound)
    {
        finalResult.errorMessage = L"FFmpeg is missing. Restore the bundled tools folder or add FFmpeg to PATH.";
        return finalResult;
    }

    std::wstring selectedEncoder = SelectAvailableEncoder(options.encoderMode, cancelRequested);
    if (options.encoderMode == VideoEncoderMode::AutomaticGpu && selectedEncoder == L"libx264")
    {
        finalResult.plan.warnings.push_back(L"No compatible GPU encoder was available, so this job used the CPU.");
    }
    if (cancelRequested.load())
    {
        finalResult.cancelled = true;
        finalResult.errorMessage = L"Compression cancelled.";
        return finalResult;
    }
    finalResult.plan.encoderName = selectedEncoder;
    finalResult.plan.hardwareEncoding = selectedEncoder != L"libx264";
    finalResult.plan.videoCodec = finalResult.plan.hardwareEncoding ? L"H.264 (GPU accelerated)" : L"H.264 (CPU)";
    std::error_code outputFolderError;
    const bool outputFolderExists = !options.outputFolder.empty() &&
        std::filesystem::exists(options.outputFolder, outputFolderError);
    const bool outputFolderIsDirectory = outputFolderExists &&
        std::filesystem::is_directory(options.outputFolder, outputFolderError);
    if (outputFolderError || !outputFolderIsDirectory)
    {
        finalResult.errorMessage = L"Choose a valid output folder.";
        return finalResult;
    }

    CleanupStaleWorkingOutputs(options.outputFolder);

    std::error_code spaceError;
    const auto availableSpace = std::filesystem::space(options.outputFolder, spaceError).available;
    constexpr unsigned long long reserveBytes = 64ULL * 1024ULL * 1024ULL;
    const unsigned long long requiredSpace = options.targetSizeBytes > std::numeric_limits<unsigned long long>::max() - reserveBytes
        ? std::numeric_limits<unsigned long long>::max()
        : options.targetSizeBytes + reserveBytes;
    if (!spaceError && availableSpace < requiredSpace)
    {
        finalResult.errorMessage = L"There is not enough free space in the output folder for this compression job.";
        return finalResult;
    }

    finalResult.outputPath = ResolveOutputConflict(requestedOutputPath, options.conflictBehavior);
    const std::filesystem::path tempDirectory = CreateJobTempDirectory();
    if (tempDirectory.empty())
    {
        finalResult.errorMessage = L"Could not create temporary files for the compression job.";
        return finalResult;
    }
    const std::filesystem::path workingOutputPath = CreateWorkingOutputPath(finalResult.outputPath);
    const std::filesystem::path passLog = tempDirectory / L"pass";
    const auto started = std::chrono::steady_clock::now();

    auto report = [&](VideoCompressionPhase phase, double progress, const std::wstring& message, const std::wstring& speed, int retry)
    {
        if (!progressCallback) return;
        VideoCompressionProgress update;
        update.phase = phase;
        update.progress = std::clamp(progress, 0.0, 1.0);
        update.message = message;
        update.speed = speed;
        update.retryNumber = retry;
        update.elapsedSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        if (update.progress > 0.01 && update.progress < 1.0)
        {
            update.estimatedRemainingSeconds = update.elapsedSeconds * (1.0 - update.progress) / update.progress;
        }
        std::error_code outputSizeError;
        if (std::filesystem::exists(workingOutputPath, outputSizeError))
        {
            update.currentOutputSizeBytes = std::filesystem::file_size(workingOutputPath, outputSizeError);
        }
        progressCallback(update);
    };

    long long videoBitrate = finalResult.plan.videoBitrate;
    const int attempts = options.retryIfTooLarge ? std::max(1, options.maxRetries + 1) : 1;
    bool gpuFallbackAttempted = false;
    for (int attempt = 0; attempt < attempts; ++attempt)
    {
        if (cancelRequested.load())
        {
            finalResult.cancelled = true;
            break;
        }
        CleanupPath(workingOutputPath);
        if (attempt > 0)
        {
            report(VideoCompressionPhase::Retrying, 0.0, L"Retrying with a lower bitrate...", L"", attempt);
        }

        ProcessResult encodeResult;
        if (options.mode == VideoCompressionMode::Accurate && !finalResult.plan.hardwareEncoding)
        {
            std::vector<std::wstring> pass1 = BaseVideoArguments(analysis, options, finalResult.plan, selectedEncoder, videoBitrate);
            pass1.insert(pass1.end(), {
                L"-an", L"-pass", L"1", L"-passlogfile", passLog.wstring(),
                L"-progress", L"pipe:1", L"-nostats", L"-f", L"null", L"NUL"
            });
            ProgressParser parser1(analysis.durationSeconds, 0.0, 0.5);
            std::wstring speed;
            report(VideoCompressionPhase::Pass1, 0.0, L"Encoding pass 1 of 2...", L"", attempt);
            ProcessResult pass1Result = processRunner_.Run(
                tools.ffmpegPath,
                pass1,
                cancelRequested,
                [&](const std::wstring& chunk)
                {
                    double value = 0.0;
                    parser1.Consume(chunk, value, speed);
                    report(VideoCompressionPhase::Pass1, value, L"Encoding pass 1 of 2...", speed, attempt);
                });
            if (pass1Result.cancelled || pass1Result.exitCode != 0)
            {
                encodeResult = pass1Result;
            }
            else
            {
                std::vector<std::wstring> pass2 = BaseVideoArguments(analysis, options, finalResult.plan, selectedEncoder, videoBitrate);
                pass2.insert(pass2.end(), { L"-pass", L"2", L"-passlogfile", passLog.wstring() });
                if (finalResult.plan.audioBitrate > 0)
                {
                    pass2.insert(pass2.end(), { L"-map", L"0:a:0?", L"-c:a", L"aac", L"-b:a", BitrateArgument(finalResult.plan.audioBitrate) });
                }
                else
                {
                    pass2.push_back(L"-an");
                }
                if (!options.preserveMetadata)
                {
                    pass2.insert(pass2.end(), { L"-map_metadata", L"-1" });
                }
                pass2.insert(pass2.end(), { L"-sn", L"-movflags", L"+faststart", L"-progress", L"pipe:1", L"-nostats", workingOutputPath.wstring() });
                ProgressParser parser2(analysis.durationSeconds, 0.5, 0.5);
                report(VideoCompressionPhase::Pass2, 0.5, L"Encoding pass 2 of 2...", L"", attempt);
                encodeResult = processRunner_.Run(
                    tools.ffmpegPath,
                    pass2,
                    cancelRequested,
                    [&](const std::wstring& chunk)
                    {
                        double value = 0.5;
                        parser2.Consume(chunk, value, speed);
                        report(VideoCompressionPhase::Pass2, value, L"Encoding pass 2 of 2...", speed, attempt);
                    });
            }
        }
        else
        {
            std::vector<std::wstring> arguments = BaseVideoArguments(analysis, options, finalResult.plan, selectedEncoder, videoBitrate);
            arguments.insert(arguments.end(), {
                L"-maxrate", BitrateArgument(videoBitrate),
                L"-bufsize", BitrateArgument(videoBitrate * 2)
            });
            if (finalResult.plan.audioBitrate > 0)
            {
                arguments.insert(arguments.end(), { L"-map", L"0:a:0?", L"-c:a", L"aac", L"-b:a", BitrateArgument(finalResult.plan.audioBitrate) });
            }
            else
            {
                arguments.push_back(L"-an");
            }
            if (!options.preserveMetadata)
            {
                arguments.insert(arguments.end(), { L"-map_metadata", L"-1" });
            }
            arguments.insert(arguments.end(), { L"-sn", L"-movflags", L"+faststart", L"-progress", L"pipe:1", L"-nostats", workingOutputPath.wstring() });
            ProgressParser parser(analysis.durationSeconds, 0.0, 1.0);
            std::wstring speed;
            report(VideoCompressionPhase::Encoding, 0.0, L"Encoding video...", L"", attempt);
            encodeResult = processRunner_.Run(
                tools.ffmpegPath,
                arguments,
                cancelRequested,
                [&](const std::wstring& chunk)
                {
                    double value = 0.0;
                    parser.Consume(chunk, value, speed);
                    report(VideoCompressionPhase::Encoding, value, L"Encoding video...", speed, attempt);
                });
        }

        if (encodeResult.cancelled || cancelRequested.load())
        {
            finalResult.cancelled = true;
            break;
        }
        std::error_code outputExistsError;
        const bool outputExists = std::filesystem::exists(workingOutputPath, outputExistsError);
        if (encodeResult.exitCode != 0 || outputExistsError || !outputExists)
        {
            if (finalResult.plan.hardwareEncoding && !gpuFallbackAttempted)
            {
                gpuFallbackAttempted = true;
                selectedEncoder = L"libx264";
                finalResult.plan.encoderName = selectedEncoder;
                finalResult.plan.hardwareEncoding = false;
                finalResult.plan.videoCodec = L"H.264 (CPU)";
                const std::wstring gpuQualityWarning = L"GPU encoding is faster, but CPU encoding usually provides better compression at the same size.";
                finalResult.plan.warnings.erase(
                    std::remove(finalResult.plan.warnings.begin(), finalResult.plan.warnings.end(), gpuQualityWarning),
                    finalResult.plan.warnings.end());
                finalResult.plan.warnings.push_back(L"GPU encoding failed for this video, so the job automatically used the CPU instead.");
                CleanupPath(workingOutputPath);
                report(VideoCompressionPhase::Retrying, 0.0, L"GPU encoding failed. Retrying with the CPU...", L"", attempt);
                videoBitrate = finalResult.plan.videoBitrate;
                finalResult.retriesUsed = 0;
                attempt = -1;
                continue;
            }
            finalResult.errorMessage = L"FFmpeg could not compress this video with the selected settings.";
            break;
        }

        report(VideoCompressionPhase::Verifying, 1.0, L"Verifying final size...", L"", attempt);
        std::error_code fileSizeError;
        finalResult.finalSizeBytes = std::filesystem::file_size(workingOutputPath, fileSizeError);
        if (fileSizeError || finalResult.finalSizeBytes == 0)
        {
            finalResult.errorMessage = L"The compressed output file was not created correctly.";
            break;
        }
        if (!options.verifyFinalSize || finalResult.finalSizeBytes <= options.targetSizeBytes)
        {
            finalResult.success = true;
            finalResult.retriesUsed = attempt;
            break;
        }
        if (attempt + 1 < attempts)
        {
            const double measuredRatio = static_cast<double>(options.targetSizeBytes) / finalResult.finalSizeBytes;
            videoBitrate = std::max<long long>(kMinimumVideoBitrate, static_cast<long long>(videoBitrate * measuredRatio * 0.96));
            finalResult.retriesUsed = attempt + 1;
            continue;
        }
        finalResult.errorMessage = L"The output could not be kept under the target size with the selected settings. Try a lower resolution, lower FPS, lower audio bitrate, or a larger target.";
    }

    if (finalResult.success)
    {
        if (options.conflictBehavior == VideoConflictBehavior::AutoRename)
        {
            finalResult.outputPath = ResolveOutputConflict(finalResult.outputPath, options.conflictBehavior);
        }
        DWORD moveFlags = MOVEFILE_WRITE_THROUGH;
        if (options.conflictBehavior == VideoConflictBehavior::Overwrite)
        {
            moveFlags |= MOVEFILE_REPLACE_EXISTING;
        }
        if (!MoveFileExW(workingOutputPath.c_str(), finalResult.outputPath.c_str(), moveFlags))
        {
            finalResult.success = false;
            finalResult.errorMessage = L"The compressed video was created, but it could not replace or save the destination file. Close any app using that file and try again.";
        }
    }

    CleanupPath(tempDirectory, true);
    if (finalResult.cancelled)
    {
        CleanupPath(workingOutputPath);
        finalResult.errorMessage = L"Compression cancelled.";
        report(VideoCompressionPhase::Cancelled, 0.0, finalResult.errorMessage, L"", finalResult.retriesUsed);
    }
    else if (!finalResult.success)
    {
        CleanupPath(workingOutputPath);
        if (finalResult.errorMessage.empty())
        {
            finalResult.errorMessage = L"Compression failed before an output file could be created.";
        }
        report(VideoCompressionPhase::Failed, 0.0, finalResult.errorMessage, L"", finalResult.retriesUsed);
    }
    else
    {
        report(VideoCompressionPhase::Complete, 1.0, L"Compression complete.", L"", finalResult.retriesUsed);
    }
    return finalResult;
}

bool VideoCompressionService::IsSupportedInput(const std::filesystem::path& path)
{
    const std::wstring extension = Lower(path.extension().wstring());
    return extension == L".mp4" || extension == L".mov" || extension == L".mkv" ||
        extension == L".webm" || extension == L".avi" || extension == L".m4v" ||
        extension == L".wmv" || extension == L".flv" || extension == L".mpeg" ||
        extension == L".mpg" || extension == L".ts" || extension == L".m2ts" ||
        extension == L".mts" || extension == L".3gp" || extension == L".3g2" ||
        extension == L".ogv" || extension == L".vob" || extension == L".mxf" ||
        extension == L".asf" || extension == L".f4v";
}

std::filesystem::path VideoCompressionService::DefaultOutputPath(
    const VideoAnalysis& analysis,
    const VideoCompressionOptions& options)
{
    const unsigned long long targetMb = std::max<unsigned long long>(1, options.targetSizeBytes / (1024ULL * 1024ULL));
    const std::wstring stem = SanitizeStem(analysis.filePath.stem().wstring());
    return options.outputFolder / (stem + L"_compressed_" + std::to_wstring(targetMb) + L"mb.mp4");
}

std::wstring VideoCompressionService::FormatBytes(unsigned long long bytes)
{
    static constexpr const wchar_t* units[] { L"B", L"KB", L"MB", L"GB", L"TB" };
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units))
    {
        value /= 1024.0;
        ++unit;
    }
    std::wostringstream output;
    output << std::fixed << std::setprecision(unit == 0 ? 0 : 1) << value << L" " << units[unit];
    return output.str();
}

std::wstring VideoCompressionService::FormatDuration(double seconds)
{
    const int total = std::max(0, static_cast<int>(std::round(seconds)));
    const int hours = total / 3600;
    const int minutes = (total % 3600) / 60;
    const int remaining = total % 60;
    wchar_t buffer[32] {};
    if (hours > 0) swprintf_s(buffer, L"%d:%02d:%02d", hours, minutes, remaining);
    else swprintf_s(buffer, L"%d:%02d", minutes, remaining);
    return buffer;
}

std::wstring VideoCompressionService::FormatBitrate(long long bitsPerSecond)
{
    if (bitsPerSecond >= 1000000)
    {
        std::wostringstream output;
        output << std::fixed << std::setprecision(2) << static_cast<double>(bitsPerSecond) / 1000000.0 << L" Mbps";
        return output.str();
    }
    return std::to_wstring(std::max<long long>(0, bitsPerSecond / 1000)) + L" kbps";
}

std::wstring VideoCompressionService::PhaseLabel(VideoCompressionPhase phase)
{
    switch (phase)
    {
    case VideoCompressionPhase::Idle: return L"Ready";
    case VideoCompressionPhase::Analyzing: return L"Analyzing";
    case VideoCompressionPhase::Calculating: return L"Calculating settings";
    case VideoCompressionPhase::Pass1: return L"Encoding pass 1";
    case VideoCompressionPhase::Pass2: return L"Encoding pass 2";
    case VideoCompressionPhase::Encoding: return L"Encoding";
    case VideoCompressionPhase::Verifying: return L"Verifying output size";
    case VideoCompressionPhase::Retrying: return L"Retrying with lower bitrate";
    case VideoCompressionPhase::Complete: return L"Complete";
    case VideoCompressionPhase::Failed: return L"Failed";
    case VideoCompressionPhase::Cancelled: return L"Cancelled";
    }
    return L"Ready";
}

std::wstring VideoCompressionService::ModeLabel(VideoCompressionMode mode)
{
    return mode == VideoCompressionMode::Accurate ? L"Best Quality / Accurate Size" : L"Fast";
}

std::wstring VideoCompressionService::ResolutionLabel(VideoResolutionMode mode)
{
    switch (mode)
    {
    case VideoResolutionMode::Auto: return L"Auto";
    case VideoResolutionMode::Original: return L"Original";
    case VideoResolutionMode::Max1080: return L"1080p max";
    case VideoResolutionMode::Max720: return L"720p max";
    case VideoResolutionMode::Max480: return L"480p max";
    }
    return L"Auto";
}

std::wstring VideoCompressionService::FpsLabel(VideoFpsMode mode)
{
    switch (mode)
    {
    case VideoFpsMode::Auto: return L"Auto";
    case VideoFpsMode::Original: return L"Original";
    case VideoFpsMode::Fps60: return L"60 FPS max";
    case VideoFpsMode::Fps30: return L"30 FPS max";
    case VideoFpsMode::Fps24: return L"24 FPS max";
    }
    return L"Auto";
}

std::wstring VideoCompressionService::AudioLabel(VideoAudioMode mode)
{
    switch (mode)
    {
    case VideoAudioMode::Auto: return L"Auto";
    case VideoAudioMode::K192: return L"AAC 192 kbps";
    case VideoAudioMode::K160: return L"AAC 160 kbps";
    case VideoAudioMode::K128: return L"AAC 128 kbps";
    case VideoAudioMode::K96: return L"AAC 96 kbps";
    case VideoAudioMode::K64: return L"AAC 64 kbps";
    case VideoAudioMode::Mute: return L"Mute audio";
    }
    return L"Auto";
}

std::wstring VideoCompressionService::PresetLabel(VideoEncodingPreset preset)
{
    switch (preset)
    {
    case VideoEncodingPreset::Slow: return L"Slow / better compression";
    case VideoEncodingPreset::Medium: return L"Medium";
    case VideoEncodingPreset::Fast: return L"Fast";
    }
    return L"Slow / better compression";
}

std::wstring VideoCompressionService::EncoderLabel(VideoEncoderMode mode)
{
    switch (mode)
    {
    case VideoEncoderMode::Cpu: return L"CPU / best compression";
    case VideoEncoderMode::AutomaticGpu: return L"Automatic GPU / CPU fallback";
    }
    return L"CPU / best compression";
}
