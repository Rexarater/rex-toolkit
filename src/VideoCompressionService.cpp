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
        plan.warnings.pusã^ý¶‰žËkºwµçMÌ¤€¼ÕÁ‘…Ñ”¹ÁÉ½É•ÍÌì(€€€€€€€ô(€€€€€€€ÍÑèé•ÉÉ½É}½‘”½ÕÑÁÕÑM¥é•ÉÉ½Èì(€€€€€€€¥˜€¡ÍÑèé™¥±•ÍåÍÑ•´èé•á¥ÍÑÌ¡Ý½É­¥¹=ÕÑÁÕÑA…Ñ °½ÕÑÁÕÑM¥é•ÉÉ½È¤¤(€€€€€€€ì(€€€€€€€€€€€ÕÁ‘…Ñ”¹ÕÉÉ•¹Ñ=ÕÑÁÕÑM¥é•	åÑ•Ì€ôÍÑèé™¥±•ÍåÍÑ•´èé™¥±•}Í¥é”¡Ý½É­¥¹=ÕÑÁÕÑA…Ñ °½ÕÑÁÕÑM¥é•ÉÉ½È¤ì(€€€€€€€ô(€€€€€€€ÁÉ½É•ÍÍ…±±‰…¬¡ÕÁ‘…Ñ”¤ì(€€€ôì((€€€±½¹œ±½¹œÙ¥‘•½	¥ÑÉ…Ñ”€ô™¥¹…±I•ÍÕ±Ð¹Á±…¸¹Ù¥‘•½	¥ÑÉ…Ñ”ì(€€€½¹ÍÐ¥¹Ð…ÑÑ•µÁÑÌ€ô½ÁÑ¥½¹Ì¹É•ÑÉå%™Q½½1…É”€üÍÑèéµ…à Ä°½ÁÑ¥½¹Ì¹µ…áI•ÑÉ¥•Ì€¬€Ä¤€è€Äì(€€€‰½½°ÁÕ…±±‰…­ÑÑ•µÁÑ•€ô™…±Í”ì(€€€™½È€¡¥¹Ð…ÑÑ•µÁÐ€ô€Àì…ÑÑ•µÁÐ€ð…ÑÑ•µÁÑÌì€¬­…ÑÑ•µÁÐ¤(€€€ì(€€€€€€€¥˜€¡…¹•±I•ÅÕ•ÍÑ•¹±½… ¤¤(€€€€€€€ì(€€€€€€€€€€€™¥¹…±I•ÍÕ±Ð¹…¹•±±•€ôÑÉÕ”ì(€€€€€€€€€€€‰É•…¬ì(€€€€€€€ô(€€€€€€€±•…¹ÕÁA…Ñ ¡Ý½É­¥¹=ÕÑÁÕÑA…Ñ ¤ì(€€€€€€€¥˜€¡…ÑÑ•µÁÐ€ø€À¤(€€€€€€€ì(€€€€€€€€€€€É•Á½ÉÐ¡Y¥‘•½½µÁÉ•ÍÍ¥½¹A¡…Í”èéI•ÑÉå¥¹œ°€À¸À°0‰I•ÑÉå¥¹œÝ¥Ñ „±½Ý•È‰¥ÑÉ…Ñ”¸¸¸ˆ°0ˆˆ°…ÑÑ•µÁÐ¤ì(€€€€€€€ô((€€€€€€€AÉ½•ÍÍI•ÍÕ±Ð•¹½‘•I•ÍÕ±Ðì(€€€€€€€¥˜€¡½ÁÑ¥½¹Ì¹µ½‘”€ôôY¥‘•½½µÁÉ•ÍÍ¥½¹5½‘”èéÕÉ…Ñ”€˜˜€…™¥¹…±I•ÍÕ±Ð¹Á±…¸¹¡…É‘Ý…É•¹½‘¥¹œ¤(€€€€€€€ì(€€€€€€€€€€€ÍÑèéÙ•Ñ½ÈñÍÑèéÝÍÑÉ¥¹œøÁ…ÍÌÄ€ô	…Í•Y¥‘•½ÉÕµ•¹ÑÌ¡…¹…±åÍ¥Ì°½ÁÑ¥½¹Ì°™¥¹…±I•ÍÕ±Ð¹Á±…¸°Í•±•Ñ•‘¹½‘•È°Ù¥‘•½	¥ÑÉ…Ñ”¤ì(€€€€€€€€€€€Á…ÍÌÄ¹¥¹Í•ÉÐ¡Á…ÍÌÄ¹•¹ ¤°ì(€€€€€€€€€€€€€€€0ˆµ…¸ˆ°0ˆµÁ…ÍÌˆ°0ˆÄˆ°0ˆµÁ…ÍÍ±½™¥±”ˆ°Á…ÍÍ1½œ¹ÝÍÑÉ¥¹œ ¤°(€€€€€€€€€€€€€€€0ˆµÁÉ½É•ÍÌˆ°0‰Á¥Á”èÄˆ°0ˆµ¹½ÍÑ…ÑÌˆ°0ˆµ˜ˆ°0‰¹Õ±°ˆ°0‰9U0ˆ(€€€€€€€€€€€ô¤ì(€€€€€€€€€€€AÉ½É•ÍÍA…ÉÍ•ÈÁ…ÉÍ•ÈÄ¡…¹…±åÍ¥Ì¹‘ÕÉ…Ñ¥½¹M•½¹‘Ì°€À¸À°€À¸Ô¤ì(€€€€€€€€€€€ÍÑèéÝÍÑÉ¥¹œÍÁ••ì(€€€€€€€€€€€É•Á½ÉÐ¡Y¥‘•½½µÁÉ•ÍÍ¥½¹A¡…Í”èéA…ÍÌÄ°€À¸À°0‰¹½‘¥¹œÁ…ÍÌ€Ä½˜€È¸¸¸ˆ°0ˆˆ°…ÑÑ•µÁÐ¤ì(€€€€€€€€€€€AÉ½•ÍÍI•ÍÕ±ÐÁ…ÍÌÅI•ÍÕ±Ð€ôÁÉ½•ÍÍIÕ¹¹•É|¹IÕ¸ (€€€€€€€€€€€€€€€Ñ½½±Ì¹™™µÁ•A…Ñ °(€€€€€€€€€€€€€€€Á…ÍÌÄ°(€€€€€€€€€€€€€€€…¹•±I•ÅÕ•ÍÑ•°(€€€€€€€€€€€€€€€l™t¡½¹ÍÐÍÑèéÝÍÑÉ¥¹œ˜¡Õ¹¬¤(€€€€€€€€€€€€€€€ì(€€€€€€€€€€€€€€€€€€€‘½Õ‰±”Ù…±Õ”€ô€À¸Àì(€€€€€€€€€€€€€€€€€€€Á…ÉÍ•ÈÄ¹½¹ÍÕµ”¡¡Õ¹¬°Ù…±Õ”°ÍÁ••¤ì(€€€€€€€€€€€€€€€€€€€É•Á½ÉÐ¡Y¥‘•½½µÁÉ•ÍÍ¥½¹A¡…Í”èéA…ÍÌÄ°Ù…±Õ”°0‰¹½‘¥¹œÁ…ÍÌ€Ä½˜€È¸¸¸ˆ°ÍÁ••°…ÑÑ•µÁÐ¤ì(€€€€€€€€€€€€€€€ô¤ì(€€€€€€€€€€€¥˜€¡Á…ÍÌÅI•ÍÕ±Ð¹…¹•±±•ñðÁ…ÍÌÅI•ÍÕ±Ð¹•á¥Ñ½‘”€„ô€À¤(€€€€€€€€€€€ì(€€€€€€€€€€€€€€€•¹½‘•I•ÍÕ±Ð€ôÁ…ÍÌÅI•ÍÕ±Ðì(€€€€€€€€€€€ô(€€€€€€€€€€€•±Í”(€€€€€€€€€€€ì(€€€€€€€€€€€€€€€ÍÑèéÙ•Ñ½ÈñÍÑèéÝÍÑÉ¥¹œøÁ…ÍÌÈ€ô	…Í•Y¥‘•½ÉÕµ•¹ÑÌ¡…¹…±åÍ¥Ì°½ÁÑ¥½¹Ì°™¥¹…±I•ÍÕ±Ð¹Á±…¸°Í•±•Ñ•‘¹½‘•È°Ù¥‘•½	¥ÑÉ…Ñ”¤ì(€€€€€€€€€€€€€€€Á…ÍÌÈ¹¥¹Í•ÉÐ¡Á…ÍÌÈ¹•¹ ¤°ì0ˆµÁ…ÍÌˆ°0ˆÈˆ°0ˆµÁ…ÍÍ±½™¥±”ˆ°Á…ÍÍ1½œ¹ÝÍÑÉ¥¹œ ¤ô¤ì(€€€€€€€€€€€€€€€¥˜€¡™¥¹…±I•ÍÕ±Ð¹Á±…¸¹…Õ‘¥½	¥ÑÉ…Ñ”€ø€À¤(€€€€€€€€€€€€€€€ì(€€€€€€€€€€€€€€€€€€€Á…ÍÌÈ¹¥¹Í•ÉÐ¡Á…ÍÌÈ¹•¹ ¤°ì0ˆµµ…Àˆ°0ˆÀé„èÀüˆ°0ˆµŒé„ˆ°0‰……Œˆ°0ˆµˆé„ˆ°	¥ÑÉ…Ñ•ÉÕµ•¹Ð¡™¥¹…±I•ÍÕ±Ð¹Á±…¸¹…Õ‘¥½	¥ÑÉ…Ñ”¤ô¤ì(€€€€€€€€€€€€€€€ô(€€€€€€€€€€€€€€€•±Í”(€€€€€€€€€€€€€€€ì(€€€€€€€€€€€€€€€€€€€Á…ÍÌÈ¹ÁÕÍ¡}‰…¬¡0ˆµ…¸ˆ¤ì(€€€€€€€€€€€€€€€ô(€€€€€€€€€€€€€€€¥˜€ …½ÁÑ¥½¹Ì¹ÁÉ•Í•ÉÙ•5•Ñ…‘…Ñ„¤(€€€€€€€€€€€€€€€ì(€€€€€€€€€€€€€€€€€€€Á…ÍÌÈ¹¥¹Í•ÉÐ¡Á…ÍÌÈ¹•¹ ¤°ì0ˆµµ…Á}µ•Ñ…‘…Ñ„ˆ°0ˆ´Äˆô¤ì(€€€€€€€€€€€€€€€ô(€€€€€€€€€€€€€€€Á…ÍÌÈ¹¥¹Í•ÉÐ¡Á…ÍÌÈ¹•¹ ¤°ì0ˆµÍ¸ˆ°0ˆµµ½Ù™±…Ìˆ°0ˆ­™…ÍÑÍÑ…ÉÐˆ°0ˆµÁÉ½É•ÍÌˆ°0‰Á¥Á”èÄˆ°0ˆµ¹½ÍÑ…ÑÌˆ°Ý½É­¥¹=ÕÑÁÕÑA…Ñ ¹ÝÍÑÉ¥¹œ ¤ô¤ì(€€€€€€€€€€€€€€€AÉ½É•ÍÍA…ÉÍ•ÈÁ…ÉÍ•ÈÈ¡…¹…±åÍ¥Ì¹‘ÕÉ…Ñ¥½¹M•½¹‘Ì°€À¸Ô°€À¸Ô¤ì(€€€€€€€€€€€€€€€É•Á½ÉÐ¡Y¥‘•½½µÁÉ•ÍÍ¥½¹A¡…Í”èéA…ÍÌÈ°€À¸Ô°0‰¹½‘¥¹œÁ…ÍÌ€È½˜€È¸¸¸ˆ°0ˆˆ°…ÑÑ•µÁÐ¤ì(€€€€€€€€€€€€€€€•¹½‘•I•ÍÕ±Ð€ôÁÉ½•ÍÍIÕ¹¹•É|¹IÕ¸ (€€€€€€€€€€€€€€€€€€€Ñ½½±Ì¹™™µÁ•A…Ñ °(€€€€€€€€€€€€€€€€€€€Á…ÍÌÈ°(€€€€€€€€€€€€€€€€€€€…¹•±I•ÅÕ•ÍÑ•°(€€€€€€€€€€€€€€€€€€€l™t¡½¹ÍÐÍÑèéÝÍÑÉ¥¹œ˜¡Õ¹¬¤(€€€€€€€€€€€€€€€€€€€ì(€€€€€€€€€€€€€€€€€€€€€€€‘½Õ‰±”Ù…±Õ”€ô€À¸Ôì(€€€€€€€€€€€€€€€€€€€€€€€Á…ÉÍ•ÈÈ¹½¹ÍÕµ”¡¡Õ¹¬°Ù…±Õ”°ÍÁ••¤ì(€€€€€€€€€€€€€€€€€€€€€€€É•Á½ÉÐ¡Y¥‘•½½µÁÉ•ÍÍ¥½¹A¡…Í”èéA…ÍÌÈ°Ù…±Õ”°0‰¹½‘¥¹œÁ…ÍÌ€È½˜€È¸¸¸ˆ°ÍÁ••°…ÑÑ•µÁÐ¤ì(€€€€€€€€€€€€€€€€€€€ô¤ì(€€€€€€€€€€€ô(€€€€€€€ô(€€€€€€€•±Í”(€€€€€€€ì(€€€€€€€€€€€ÍÑèéÙ•Ñ½ÈñÍÑèéÝÍÑÉ¥¹œø…ÉÕµ•¹ÑÌ€ô	…Í•Y¥‘•½ÉÕµ•¹ÑÌ¡…¹…±åÍ¥Ì°½ÁÑ¥½¹Ì°™¥¹…±I•ÍÕ±Ð¹Á±…¸°Í•±•Ñ•‘¹½‘•È°Ù¥‘•½	¥ÑÉ…Ñ”¤ì(€€€€€€€€€€€…ÉÕµ•¹ÑÌ¹¥¹Í•ÉÐ¡…ÉÕµ•¹ÑÌ¹•¹ ¤°ì(€€€€€€€€€€€€€€€0ˆµµ…áÉ…Ñ”ˆ°	¥ÑÉ…Ñ•ÉÕµ•¹Ð¡Ù¥‘•½	¥ÑÉ…Ñ”¤°(€€€€€€€€€€€€€€€0ˆµ‰Õ™Í¥é”ˆ°	¥ÑÉ…Ñ•ÉÕµ•¹Ð¡Ù¥‘•½	¥ÑÉ…Ñ”€¨€È¤(€€€€€€€€€€€ô¤ì(€€€€€€€€€€€¥˜€¡™¥¹…±I•ÍÕ±Ð¹Á±…¸¹…Õ‘¥½	¥ÑÉ…Ñ”€ø€À¤(€€€€€€€€€€€ì(€€€€€€€€€€€€€€€…ÉÕµ•¹ÑÌ¹¥¹Í•ÉÐ¡…ÉÕµ•¹ÑÌ¹•¹ ¤°ì0ˆµµ…Àˆ°0ˆÀé„èÀüˆ°0ˆµŒé„ˆ°0‰……Œˆ°0ˆµˆé„ˆ°	¥ÑÉ…Ñ•ÉÕµ•¹Ð¡™¥¹…±I•ÍÕ±Ð¹Á±…¸¹…Õ‘¥½	¥ÑÉ…Ñ”¤ô¤ì(€€€€€€€€€€€ô(€€€€€€€€€€€•±Í”(€€€€€€€€€€€ì(€€€€€€€€€€€€€€€…ÉÕµ•¹ÑÌ¹ÁÕÍ¡}‰…¬¡0ˆµ…¸ˆ¤ì(€€€€€€€€€€€ô(€€€€€€€€€€€¥˜€ …½ÁÑ¥½¹Ì¹ÁÉ•Í•ÉÙ•5•Ñ…‘…Ñ„¤(€€€€€€€€€€€ì(€€€€€€€€€€€€€€€…ÉÕµ•¹ÑÌ¹¥¹Í•ÉÐ¡…ÉÕµ•¹ÑÌ¹•¹ ¤°ì0ˆµµ…Á}µ•Ñ…‘…Ñ„ˆ°0ˆ´Äˆô¤ì(€€€€€€€€€€€ô(€€€€€€€€€€€…ÉÕµ•¹ÑÌ¹¥¹Í•ÉÐ¡…ÉÕµ•¹ÑÌ¹•¹ ¤°ì0ˆµÍ¸ˆ°0ˆµµ½Ù™±…Ìˆ°0ˆ­™…ÍÑÍÑ…ÉÐˆ°0ˆµÁÉ½É•ÍÌˆ°0‰Á¥Á”èÄˆ°0ˆµ¹½ÍÑ…ÑÌˆ°Ý½É­¥¹=ÕÑÁÕÑA…Ñ ¹ÝÍÑÉ¥¹œ ¤ô¤ì(€€€€€€€€€€€AÉ½É•ÍÍA…ÉÍ•ÈÁ…ÉÍ•È¡…¹…±åÍ¥Ì¹‘ÕÉ…Ñ¥½¹M•½¹‘Ì°€À¸À°€Ä¸À¤ì(€€€€€€€€€€€ÍÑèéÝÍÑÉ¥¹œÍÁ••ì(€€€€€€€€€€€É•Á½ÉÐ¡Y¥‘•½½µÁÉ•ÍÍ¥½¹A¡…Í”èé¹½‘¥¹œ°€À¸À°0‰¹½‘¥¹œÙ¥‘•¼¸¸¸ˆ°0ˆˆ°…ÑÑ•µÁÐ¤ì(€€€€€€€€€€€•¹½‘•I•ÍÕ±Ð€ôÁÉ½•ÍÍIÕ¹¹•É|¹IÕ¸ (€€€€€€€€€€€€€€€Ñ½½±Ì¹™™µÁ•A…Ñ °(€€€€€€€€€€€€€€€…ÉÕµ•¹ÑÌ°(€€€€€€€€€€€€€€€…¹•±I•ÅÕ•ÍÑ•°(€€€€€€€€€€€€€€€l™t¡½¹ÍÐÍÑèéÝÍÑÉ¥¹œ˜¡Õ¹¬¤(€€€€€€€€€€€€€€€ì(€€€€€€€€€€€€€€€€€€€‘½Õ‰±”Ù…±Õ”€ô€À¸Àì(€€€€€€€€€€€€€€€€€€€Á…ÉÍ•È¹½¹ÍÕµ”¡¡Õ¹¬°Ù…±Õ”°ÍÁ••¤ì(€€€€€€€€€€€€€€€€€€€É•Á½ÉÐ¡Y¥‘•½½µÁÉ•ÍÍ¥½¹A¡…Í”èé¹½‘¥¹œ°Ù…±Õ”°0‰¹½‘¥¹œÙ¥‘•¼¸¸¸ˆ°ÍÁ••°…ÑÑ•µÁÐ¤ì(€€€€€€€€€€€€€€€ô¤ì(€€€€€€€ô((€€€€€€€¥˜€¡•¹½‘•I•ÍÕ±Ð¹…¹•±±•ñð…¹•±I•ÅÕ•ÍÑ•¹±½… ¤¤(€€€€€€€ì(€€€€€€€€€€€™¥¹…±I•ÍÕ±Ð¹…¹•±±•€ôÑÉÕ”ì(€€€€€€€€€€€‰É•…¬ì(€€€€€€€ô(€€€€€€€ÍÑèé•ÉÉ½É}½‘”½ÕÑÁÕÑá¥ÍÑÍÉÉ½Èì(€€€€€€€½¹ÍÐ‰½½°½ÕÑÁÕÑá¥ÍÑÌ€ôÍÑèé™¥±•ÍåÍÑ•´èé•á¥ÍÑÌ¡Ý½É­¥¹=ÕÑÁÕÑA…Ñ °½ÕÑÁÕÑá¥ÍÑÍÉÉ½È¤ì(€€€€€€€¥˜€¡•¹½‘•I•ÍÕ±Ð¹•á¥Ñ½‘”€„ô€Àñð½ÕÑÁÕÑá¥ÍÑÍÉÉ½Èñð€…½ÕÑÁÕÑá¥ÍÑÌ¤(€€€€€€€ì(€€€€€€€€€€€¥˜€¡™¥¹…±I•ÍÕ±Ð¹Á±…¸¹¡…É‘Ý…É•¹½‘¥¹œ€˜˜€…ÁÕ…±±‰…­ÑÑ•µÁÑ•¤(€€€€€€€€€€€ì(€€€€€€€€€€€€€€€ÁÕ…±±‰…­ÑÑ•µÁÑ•€ôÑÉÕ”ì(€€€€€€€€€€€€€€€Í•±•Ñ•‘¹½‘•È€ô0‰±¥‰àÈØÐˆì(€€€€€€€€€€€€€€€™¥¹…±I•ÍÕ±Ð¹Á±…¸¹•¹½‘•É9…µ”€ôÍ•±•Ñ•‘¹½‘•Èì(€€€€€€€€€€€€€€€™¥¹…±I•ÍÕ±Ð¹Á±…¸¹¡…É‘Ý…É•¹½‘¥¹œ€ô™…±Í”ì(€€€€€€€€€€€€€€€™¥¹…±I•ÍÕ±Ð¹Á±…¸¹Ù¥‘•½½‘•Œ€ô0‰ ¸ÈØÐ€¡AT¤ˆì(€€€€€€€€€€€€€€€½¹ÍÐÍÑèéÝÍÑÉ¥¹œÁÕEÕ…±¥Ñå]…É¹¥¹œ€ô0‰AT•¹½‘¥¹œ¥Ì™…ÍÑ•È°‰ÕÐAT•¹½‘¥¹œÕÍÕ…±±äÁÉ½Ù¥‘•Ì‰•ÑÑ•È½µÁÉ•ÍÍ¥½¸…ÐÑ¡”Í…µ”Í¥é”¸ˆì(€€€€€€€€€€€€€€€™¥¹…±I•ÍÕ±Ð¹Á±…¸¹Ý…É¹¥¹Ì¹•É…Í” (€€€€€€€€€€€€€€€€€€€ÍÑèéÉ•µ½Ù”¡™¥¹…±I•ÍÕ±Ð¹Á±…¸¹Ý…É¹¥¹Ì¹‰•¥¸ ¤°™¥¹…±I•ÍÕ±Ð¹Á±…¸¹Ý…É¹¥¹Ì¹•¹ ¤°ÁÕEÕ…±¥Ñå]…É¹¥¹œ¤°(€€€€€€€€€€€€€€€€€€€™¥¹…±I•ÍÕ±Ð¹Á±…¸¹Ý…É¹¥¹Ì¹•¹ ¤¤ì(€€€€€€€€€€€€€€€™¥¹…±I•ÍÕ±Ð¹Á±…¸¹Ý…É¹¥¹Ì¹ÁÕÍ¡}‰…¬¡0‰AT•¹½‘¥¹œ™…¥±•™½ÈÑ¡¥ÌÙ¥‘•¼°Í¼Ñ¡”©½ˆ…ÕÑ½µ…Ñ¥…±±äÕÍ•Ñ¡”AT¥¹ÍÑ•…¸ˆ¤ì(€€€€€€€€€€€€€€€±•…¹ÕÁA…Ñ ¡Ý½É­¥¹=ÕÑÁÕÑA…Ñ ¤ì(€€€€€€€€€€€€€€€É•Á½ÉÐ¡Y¥‘•½½µÁÉ•ÍÍ¥½¹A¡…Í”èéI•ÑÉå¥¹œ°€À¸À°0‰AT•¹½‘¥¹œ™…¥±•¸I•ÑÉå¥¹œÝ¥Ñ Ñ¡”AT¸¸¸ˆ°0ˆˆ°…ÑÑ•µÁÐ¤ì(€€€€€€€€€€€€€€€Ù¥‘•½	¥ÑÉ…Ñ”€ô™¥¹…±I•ÍÕ±Ð¹Á±…¸¹Ù¥‘•½	¥ÑÉ…Ñ”ì(€€€€€€€€€€€€€€€™¥¹…±I•ÍÕ±Ð¹É•ÑÉ¥•ÍUÍ•€ô€Àì(€€€€€€€€€€€€€€€…ÑÑ•µÁÐ€ô€´Äì(€€€€€€€€€€€€€€€½¹Ñ¥¹Õ”ì(€€€€€€€€€€€ô(€€€€€€€€€€€™¥¹…±I•ÍÕ±Ð¹•ÉÉ½É5•ÍÍ…”€ô0‰µÁ•œ½Õ±¹½Ð½µÁÉ•ÍÌÑ¡¥ÌÙ¥‘•¼Ý¥Ñ Ñ¡”Í•±•Ñ•Í•ÑÑ¥¹Ì¸ˆì(€€€€€€€€€€€‰É•…¬ì(€€€€€€€ô((€€€€€€€É•Á½ÉÐ¡Y¥‘•½½µÁÉ•ÍÍ¥½¹A¡…Í”èéY•É¥™å¥¹œ°€Ä¸À°0‰Y•É¥™å¥¹œ™¥¹…°Í¥é”¸¸¸ˆ°0ˆˆ°…ÑÑ•µÁÐ¤ì(€€€€€€€ÍÑèé•ÉÉ½É}½‘”™¥±•M¥é•ÉÉ½Èì(€€€€€€€™¥¹…±I•ÍÕ±Ð¹™¥¹…±M¥é•	åÑ•Ì€ôÍÑèé™¥±•ÍåÍÑ•´èé™¥±•}Í¥é”¡Ý½É­¥¹=ÕÑÁÕÑA…Ñ °™¥±•M¥é•ÉÉ½È¤ì(€€€€€€€¥˜€¡™¥±•M¥é•ÉÉ½Èñð™¥¹…±I•ÍÕ±Ð¹™¥¹…±M¥é•	åÑ•Ì€ôô€À¤(€€€€€€€ì(€€€€€€€€€€€™¥¹…±I•ÍÕ±Ð¹•ÉÉ½É5•ÍÍ…”€ô0‰Q¡”½µÁÉ•ÍÍ•½ÕÑÁÕÐ™¥±”Ý…Ì¹½ÐÉ•…Ñ•½ÉÉ•Ñ±ä¸ˆì(€€€€€€€€€€€‰É•…¬ì(€€€€€€€ô(€€€€€€€¥˜€ …½ÁÑ¥½¹Ì¹Ù•É¥™å¥¹…±M¥é”ñð™¥¹…±I•ÍÕ±Ð¹™¥¹…±M¥é•	åÑ•Ì€ðô½ÁÑ¥½¹Ì¹Ñ…É•ÑM¥é•	åÑ•Ì¤(€€€€€€€ì(€€€€€€€€€€€™¥¹…±I•ÍÕ±Ð¹ÍÕ•ÍÌ€ôÑÉÕ”ì(€€€€€€€€€€€™¥¹…±I•ÍÕ±Ð¹É•ÑÉ¥•ÍUÍ•€ô…ÑÑ•µÁÐì(€€€€€€€€€€€‰É•…¬ì(€€€€€€€ô(€€€€€€€¥˜€¡…ÑÑ•µÁÐ€¬€Ä€ð…ÑÑ•µÁÑÌ¤(€€€€€€€ì(€€€€€€€€€€€½¹ÍÐ‘½Õ‰±”µ•…ÍÕÉ•‘I…Ñ¥¼€ôÍÑ…Ñ¥}…ÍÐñ‘½Õ‰±”ø¡½ÁÑ¥½¹Ì¹Ñ…É•ÑM¥é•	åÑ•Ì¤€¼™¥¹…±I•ÍÕ±Ð¹™¥¹…±M¥é•	åÑ•Ìì(€€€€€€€€€€€Ù¥‘•½	¥ÑÉ…Ñ”€ôÍÑèéµ…àñ±½¹œ±½¹œø¡­5¥¹¥µÕµY¥‘•½	¥ÑÉ…Ñ”°ÍÑ…Ñ¥}…ÍÐñ±½¹œ±½¹œø¡Ù¥‘•½	¥ÑÉ…Ñ”€¨µ•…ÍÕÉ•‘I…Ñ¥¼€¨€À¸äØ¤¤ì(€€€€€€€€€€€™¥¹…±I•ÍÕ±Ð¹É•ÑÉ¥•ÍUÍ•€ô…ÑÑ•µÁÐ€¬€Äì(€€€€€€€€€€€½¹Ñ¥¹Õ”ì(€€€€€€€ô(€€€€€€€™¥¹…±I•ÍÕ±Ð¹•ÉÉ½É5•ÍÍ…”€ô0‰Q¡”½ÕÑÁÕÐ½Õ±¹½Ð‰”­•ÁÐÕ¹‘•ÈÑ¡”Ñ…É•ÐÍ¥é”Ý¥Ñ Ñ¡”Í•±•Ñ•Í•ÑÑ¥¹Ì¸QÉä„±½Ý•ÈÉ•Í½±ÕÑ¥½¸°±½Ý•ÈAL°±½Ý•È…Õ‘¥¼‰¥ÑÉ…Ñ”°½È„±…É•ÈÑ…É•Ð¸ˆì(€€€ô((€€€¥˜€¡™¥¹…±I•ÍÕ±Ð¹ÍÕ•ÍÌ¤(€€€ì(€€€€€€€¥˜€¡½ÁÑ¥½¹Ì¹½¹™±¥Ñ	•¡…Ù¥½È€ôôY¥‘•½½¹™±¥Ñ	•¡…Ù¥½ÈèéÕÑ½I•¹…µ”¤(€€€€€€€ì(€€€€€€€€€€€™¥¹…±I•ÍÕ±Ð¹½ÕÑÁÕÑA…Ñ €ôI•Í½±Ù•=ÕÑÁÕÑ½¹™±¥Ð¡™¥¹…±I•ÍÕ±Ð¹½ÕÑÁÕÑA…Ñ °½ÁÑ¥½¹Ì¹½¹™±¥Ñ	•¡…Ù¥½È¤ì(€€€€€€€ô(€€€€€€€]=Iµ½Ù•±…Ì€ô5=Y%1}]I%Q}Q!I=U ì(€€€€€€€¥˜€¡½ÁÑ¥½¹Ì¹½¹™±¥Ñ	•¡…Ù¥½È€ôôY¥‘•½½¹™±¥Ñ	•¡…Ù¥½Èèé=Ù•ÉÝÉ¥Ñ”¤(€€€€€€€ì(€€€€€€€€€€€µ½Ù•±…Ìðô5=Y%1}IA1}a%MQ%9ì(€€€€€€€ô(€€€€€€€¥˜€ …5½Ù•¥±•á\¡Ý½É­¥¹=ÕÑÁÕÑA…Ñ ¹}ÍÑÈ ¤°™¥¹…±I•ÍÕ±Ð¹½ÕÑÁÕÑA…Ñ ¹}ÍÑÈ ¤°µ½Ù•±…Ì¤¤(€€€€€€€ì(€€€€€€€€€€€™¥¹…±I•ÍÕ±Ð¹ÍÕ•ÍÌ€ô™…±Í”ì(€€€€€€€€€€€™¥¹…±I•ÍÕ±Ð¹•ÉÉ½É5•ÍÍ…”€ô0‰Q¡”½µÁÉ•ÍÍ•Ù¥‘•¼Ý…ÌÉ•…Ñ•°‰ÕÐ¥Ð½Õ±¹½ÐÉ•Á±…”½ÈÍ…Ù”Ñ¡”‘•ÍÑ¥¹…Ñ¥½¸™¥±”¸±½Í”…¹ä…ÁÀÕÍ¥¹œÑ¡…Ð™¥±”…¹ÑÉä……¥¸¸ˆì(€€€€€€€ô(€€€ô((€€€±•…¹ÕÁA…Ñ ¡Ñ•µÁ¥É•Ñ½Éä°ÑÉÕ”¤ì(€€€¥˜€¡™¥¹…±I•ÍÕ±Ð¹…¹•±±•¤(€€€ì(€€€€€€€±•…¹ÕÁA…Ñ ¡Ý½É­¥¹=ÕÑÁÕÑA…Ñ ¤ì(€€€€€€€™¥¹…±I•ÍÕ±Ð¹•ÉÉ½É5•ÍÍ…”€ô0‰½µÁÉ•ÍÍ¥½¸…¹•±±•¸ˆì(€€€€€€€É•Á½ÉÐ¡Y¥‘•½½µÁÉ•ÍÍ¥½¹A¡…Í”èé…¹•±±•°€À¸À°™¥¹…±I•ÍÕ±Ð¹•ÉÉ½É5•ÍÍ…”°0ˆˆ°™¥¹…±I•ÍÕ±Ð¹É•ÑÉ¥•ÍUÍ•¤ì(€€€ô(€€€•±Í”¥˜€ …™¥¹…±I•ÍÕ±Ð¹ÍÕ•ÍÌ¤(€€€ì(€€€€€€€±•…¹ÕÁA…Ñ ¡Ý½É­¥¹=ÕÑÁÕÑA…Ñ ¤ì(€€€€€€€¥˜€¡™¥¹…±I•ÍÕ±Ð¹•ÉÉ½É5•ÍÍ…”¹•µÁÑä ¤¤(€€€€€€€ì(€€€€€€€€€€€™¥¹…±I•ÍÕ±Ð¹•ÉÉ½É5•ÍÍ…”€ô0‰½µÁÉ•ÍÍ¥½¸™…¥±•‰•™½É”…¸½ÕÑÁÕÐ™¥±”½Õ±‰”É•…Ñ•¸ˆì(€€€€€€€ô(€€€€€€€É•Á½ÉÐ¡Y¥‘•½½µÁÉ•ÍÍ¥½¹A¡…Í”èé…¥±•°€À¸À°™¥¹…±I•ÍÕ±Ð¹•ÉÉ½É5•ÍÍ…”°0ˆˆ°™¥¹…±I•ÍÕ±Ð¹É•ÑÉ¥•ÍUÍ•¤ì(€€€ô(€€€•±Í”(€€€ì(€€€€€€€É•Á½ÉÐ¡Y¥‘•½½µÁÉ•ÍÍ¥½¹A¡…Í”èé½µÁ±•Ñ”°€Ä¸À°0‰½µÁÉ•ÍÍ¥½¸½µÁ±•Ñ”¸ˆ°0ˆˆ°™¥¹…±I•ÍÕ±Ð¹É•ÑÉ¥•ÍUÍ•¤ì(€€€ô(€€€É•ÑÕÉ¸™¥¹…±I•ÍÕ±Ðì)ô()‰½½°Y¥‘•½½µÁÉ•ÍÍ¥½¹M•ÉÙ¥”èé%ÍMÕÁÁ½ÉÑ•‘%¹ÁÕÐ¡½¹ÍÐÍÑèé™¥±•ÍåÍÑ•´èéÁ…Ñ ˜Á…Ñ ¤)ì(€€€½¹ÍÐÍÑèéÝÍÑÉ¥¹œ•áÑ•¹Í¥½¸€ô1½Ý•È¡Á…Ñ ¹•áÑ•¹Í¥½¸ ¤¹ÝÍÑÉ¥¹œ ¤¤ì(€€€É•ÑÕÉ¸•áÑ•¹Í¥½¸€ôô0ˆ¹µÀÐˆñð•áÑ•¹Í¥½¸€ôô0ˆ¹µ½Øˆñð•áÑ•¹Í¥½¸€ôô0ˆ¹µ­Øˆñð(€€€€€€€•áÑ•¹Í¥½¸€ôô0ˆ¹Ý•‰´ˆñð•áÑ•¹Í¥½¸€ôô0ˆ¹…Ù¤ˆñð•áÑ•¹Í¥½¸€ôô0ˆ¹´ÑØˆì)ô()ÍÑèé™¥±•ÍåÍÑ•´èéÁ…Ñ Y¥‘•½½µÁÉ•ÍÍ¥½¹M•ÉÙ¥”èé•™…Õ±Ñ=ÕÑÁÕÑA…Ñ  (€€€½¹ÍÐY¥‘•½¹…±åÍ¥Ì˜…¹…±åÍ¥Ì°(€€€½¹ÍÐY¥‘•½½µÁÉ•ÍÍ¥½¹=ÁÑ¥½¹Ì˜½ÁÑ¥½¹Ì¤)ì(€€€½¹ÍÐÕ¹Í¥¹•±½¹œ±½¹œÑ…É•Ñ5ˆ€ôÍÑèéµ…àñÕ¹Í¥¹•±½¹œ±½¹œø Ä°½ÁÑ¥½¹Ì¹Ñ…É•ÑM¥é•	åÑ•Ì€¼€ ÄÀÈÑU10€¨€ÄÀÈÑU10¤¤ì(€€€½¹ÍÐÍÑèéÝÍÑÉ¥¹œÍÑ•´€ôM…¹¥Ñ¥é•MÑ•´¡…¹…±åÍ¥Ì¹™¥±•A…Ñ ¹ÍÑ•´ ¤¹ÝÍÑÉ¥¹œ ¤¤ì(€€€É•ÑÕÉ¸½ÁÑ¥½¹Ì¹½ÕÑÁÕÑ½±‘•È€¼€¡ÍÑ•´€¬0‰}½µÁÉ•ÍÍ•‘|ˆ€¬ÍÑèéÑ½}ÝÍÑÉ¥¹œ¡Ñ…É•Ñ5ˆ¤€¬0‰µˆ¹µÀÐˆ¤ì)ô()ÍÑèéÝÍÑÉ¥¹œY¥‘•½½µÁÉ•ÍÍ¥½¹M•ÉÙ¥”èé½Éµ…Ñ	åÑ•Ì¡Õ¹Í¥¹•±½¹œ±½¹œ‰åÑ•Ì¤)ì(€€€ÍÑ…Ñ¥Œ½¹ÍÑ•áÁÈ½¹ÍÐÝ¡…É}Ð¨Õ¹¥ÑÍmtì0‰ˆ°0‰-ˆ°0‰5ˆ°0‰ˆ°0‰Qˆôì(€€€‘½Õ‰±”Ù…±Õ”€ôÍÑ…Ñ¥}…ÍÐñ‘½Õ‰±”ø¡‰åÑ•Ì¤ì(€€€Í¥é•}ÐÕ¹¥Ð€ô€Àì(€€€Ý¡¥±”€¡Ù…±Õ”€øô€ÄÀÈÐ¸À€˜˜Õ¹¥Ð€¬€Ä€ðÍÑèéÍ¥é”¡Õ¹¥ÑÌ¤¤(€€€ì(€€€€€€€Ù…±Õ”€¼ô€ÄÀÈÐ¸Àì(€€€€€€€€¬­Õ¹¥Ðì(€€€ô(€€€ÍÑèéÝ½ÍÑÉ¥¹ÍÑÉ•…´½ÕÑÁÕÐì(€€€½ÕÑÁÕÐ€ððÍÑèé™¥á•€ððÍÑèéÍ•ÑÁÉ•¥Í¥½¸¡Õ¹¥Ð€ôô€À€ü€À€è€Ä¤€ððÙ…±Õ”€ðð0ˆ€ˆ€ððÕ¹¥ÑÍmÕ¹¥Ñtì(€€€É•ÑÕÉ¸½ÕÑÁÕÐ¹ÍÑÈ ¤ì)ô()ÍÑèéÝÍÑÉ¥¹œY¥‘•½½µÁÉ•ÍÍ¥½¹M•ÉÙ¥”èé½Éµ…ÑÕÉ…Ñ¥½¸¡‘½Õ‰±”Í•½¹‘Ì¤)ì(€€€½¹ÍÐ¥¹ÐÑ½Ñ…°€ôÍÑèéµ…à À°ÍÑ…Ñ¥}…ÍÐñ¥¹Ðø¡ÍÑèéÉ½Õ¹¡Í•½¹‘Ì¤¤¤ì(€€€½¹ÍÐ¥¹Ð¡½ÕÉÌ€ôÑ½Ñ…°€¼€ÌØÀÀì(€€€½¹ÍÐ¥¹Ðµ¥¹ÕÑ•Ì€ô€¡Ñ½Ñ…°€”€ÌØÀÀ¤€¼€ØÀì(€€€½¹ÍÐ¥¹ÐÉ•µ…¥¹¥¹œ€ôÑ½Ñ…°€”€ØÀì(€€€Ý¡…É}Ð‰Õ™™•ÉlÌÉtíôì(€€€¥˜€¡¡½ÕÉÌ€ø€À¤ÍÝÁÉ¥¹Ñ™}Ì¡‰Õ™™•È°0ˆ•è”ÀÉè”ÀÉˆ°¡½ÕÉÌ°µ¥¹ÕÑ•Ì°É•µ…¥¹¥¹œ¤ì(€€€•±Í”ÍÝÁÉ¥¹Ñ™}Ì¡‰Õ™™•È°0ˆ•è”ÀÉˆ°µ¥¹ÕÑ•Ì°É•µ…¥¹¥¹œ¤ì(€€€É•ÑÕÉ¸‰Õ™™•Èì)ô()ÍÑèéÝÍÑÉ¥¹œY¥‘•½½µÁÉ•ÍÍ¥½¹M•ÉÙ¥”èé½Éµ…Ñ	¥ÑÉ…Ñ”¡±½¹œ±½¹œ‰¥ÑÍA•ÉM•½¹¤)ì(€€€¥˜€¡‰¥ÑÍA•ÉM•½¹€øô€ÄÀÀÀÀÀÀ¤(€€€ì(€€€€€€€ÍÑèéÝ½ÍÑÉ¥¹ÍÑÉ•…´½ÕÑÁÕÐì(€€€€€€€½ÕÑÁÕÐ€ððÍÑèé™¥á•€ððÍÑèéÍ•ÑÁÉ•¥Í¥½¸ È¤€ððÍÑ…Ñ¥}…ÍÐñ‘½Õ‰±”ø¡‰¥ÑÍA•ÉM•½¹¤€¼€ÄÀÀÀÀÀÀ¸À€ðð0ˆ5‰ÁÌˆì(€€€€€€€É•ÑÕÉ¸½ÕÑÁÕÐ¹ÍÑÈ ¤ì(€€€ô(€€€É•ÑÕÉ¸ÍÑèéÑ½}ÝÍÑÉ¥¹œ¡ÍÑèéµ…àñ±½¹œ±½¹œø À°‰¥ÑÍA•ÉM•½¹€¼€ÄÀÀÀ¤¤€¬0ˆ­‰ÁÌˆì)ô()ÍÑèéÝÍÑÉ¥¹œY¥‘•½½µÁÉ•ÍÍ¥½¹M•ÉÙ¥”èéA¡…Í•1…‰•°¡Y¥‘•½½µÁÉ•ÍÍ¥½¹A¡…Í”Á¡…Í”¤)ì(€€€ÍÝ¥Ñ €¡Á¡…Í”¤(€€€ì(€€€…Í”Y¥‘•½½µÁÉ•ÍÍ¥½¹A¡…Í”èé%‘±”èÉ•ÑÕÉ¸0‰I•…‘äˆì(€€€…Í”Y¥‘•½½µÁÉ•ÍÍ¥½¹A¡…Í”èé¹…±åé¥¹œèÉ•ÑÕÉ¸0‰¹…±åé¥¹œˆì(€€€…Í”Y¥‘•½½µÁÉ•ÍÍ¥½¹A¡…Í”èé…±Õ±…Ñ¥¹œèÉ•ÑÕÉ¸0‰…±Õ±…Ñ¥¹œÍ•ÑÑ¥¹Ìˆì(€€€…Í”Y¥‘•½½µÁÉ•ÍÍ¥½¹A¡…Í”èéA…ÍÌÄèÉ•ÑÕÉ¸0‰¹½‘¥¹œÁ…ÍÌ€Äˆì(€€€…Í”Y¥‘•½½µÁÉ•ÍÍ¥½¹A¡…Í”èéA…ÍÌÈèÉ•ÑÕÉ¸0‰¹½‘¥¹œÁ…ÍÌ€Èˆì(€€€…Í”Y¥‘•½½µÁÉ•ÍÍ¥½¹A¡…Í”èé¹½‘¥¹œèÉ•ÑÕÉ¸0‰¹½‘¥¹œˆì(€€€…Í”Y¥‘•½½µÁÉ•ÍÍ¥½¹A¡…Í”èéY•É¥™å¥¹œèÉ•ÑÕÉ¸0‰Y•É¥™å¥¹œ½ÕÑÁÕÐÍ¥é”ˆì(€€€…Í”Y¥‘•½½µÁÉ•ÍÍ¥½¹A¡…Í”èéI•ÑÉå¥¹œèÉ•ÑÕÉ¸0‰I•ÑÉå¥¹œÝ¥Ñ ±½Ý•È‰¥ÑÉ…Ñ”ˆì(€€€…Í”Y¥‘•½½µÁÉ•ÍÍ¥½¹A¡…Í”èé½µÁ±•Ñ”èÉ•ÑÕÉ¸0‰½µÁ±•Ñ”ˆì(€€€…Í”Y¥‘•½½µÁÉ•ÍÍ¥½¹A¡…Í”èé…¥±•èÉ•ÑÕÉ¸0‰…¥±•ˆì(€€€…Í”Y¥‘•½½µÁÉ•ÍÍ¥½¹A¡…Í”èé…¹•±±•èÉ•ÑÕÉ¸0‰…¹•±±•ˆì(€€€ô(€€€É•ÑÕÉ¸0‰I•…‘äˆì)ô()ÍÑèéÝÍÑÉ¥¹œY¥‘•½½µÁÉ•ÍÍ¥½¹M•ÉÙ¥”èé5½‘•1…‰•°¡Y¥‘•½½µÁÉ•ÍÍ¥½¹5½‘”µ½‘”¤)ì(€€€É•ÑÕÉ¸µ½‘”€ôôY¥‘•½½µÁÉ•ÍÍ¥½¹5½‘”èéÕÉ…Ñ”€ü0‰	•ÍÐEÕ…±¥Ñä€¼ÕÉ…Ñ”M¥é”ˆ€è0‰…ÍÐˆì)ô()ÍÑèéÝÍÑÉ¥¹œY¥‘•½½µÁÉ•ÍÍ¥½¹M•ÉÙ¥”èéI•Í½±ÕÑ¥½¹1…‰•°¡Y¥‘•½I•Í½±ÕÑ¥½¹5½‘”µ½‘”¤)ì(€€€ÍÝ¥Ñ €¡µ½‘”¤(€€€ì(€€€…Í”Y¥‘•½I•Í½±ÕÑ¥½¹5½‘”èéÕÑ¼èÉ•ÑÕÉ¸0‰ÕÑ¼ˆì(€€€…Í”Y¥‘•½I•Í½±ÕÑ¥½¹5½‘”èé=É¥¥¹…°èÉ•ÑÕÉ¸0‰=É¥¥¹…°ˆì(€€€…Í”Y¥‘•½I•Í½±ÕÑ¥½¹5½‘”èé5…àÄÀàÀèÉ•ÑÕÉ¸0ˆÄÀàÁÀµ…àˆì(€€€…Í”Y¥‘•½I•Í½±ÕÑ¥½¹5½‘”èé5…àÜÈÀèÉ•ÑÕÉ¸0ˆÜÈÁÀµ…àˆì(€€€…Í”Y¥‘•½I•Í½±ÕÑ¥½¹5½‘”èé5…àÐàÀèÉ•ÑÕÉ¸0ˆÐàÁÀµ…àˆì(€€€ô(€€€É•ÑÕÉ¸0‰ÕÑ¼ˆì)ô()ÍÑèéÝÍÑÉ¥¹œY¥‘•½½µÁÉ•ÍÍ¥½¹M•ÉÙ¥”èéÁÍ1…‰•°¡Y¥‘•½ÁÍ5½‘”µ½‘”¤)ì(€€€ÍÝ¥Ñ €¡µ½‘”¤(€€€ì(€€€…Í”Y¥‘•½ÁÍ5½‘”èéÕÑ¼èÉ•ÑÕÉ¸0‰ÕÑ¼ˆì(€€€…Í”Y¥‘•½ÁÍ5½‘”èé=É¥¥¹…°èÉ•ÑÕÉ¸0‰=É¥¥¹…°ˆì(€€€…Í”Y¥‘•½ÁÍ5½‘”èéÁÌØÀèÉ•ÑÕÉ¸0ˆØÀALµ…àˆì(€€€…Í”Y¥‘•½ÁÍ5½‘”èéÁÌÌÀèÉ•ÑÕÉ¸0ˆÌÀALµ…àˆì(€€€…Í”Y¥‘•½ÁÍ5½‘”èéÁÌÈÐèÉ•ÑÕÉ¸0ˆÈÐALµ…àˆì(€€€ô(€€€É•ÑÕÉ¸0‰ÕÑ¼ˆì)ô()ÍÑèéÝÍÑÉ¥¹œY¥‘•½½µÁÉ•ÍÍ¥½¹M•ÉÙ¥”èéÕ‘¥½1…‰•°¡Y¥‘•½Õ‘¥½5½‘”µ½‘”¤)ì(€€€ÍÝ¥Ñ €¡µ½‘”¤(€€€ì(€€€…Í”Y¥‘•½Õ‘¥½5½‘”èéÕÑ¼èÉ•ÑÕÉ¸0‰ÕÑ¼ˆì(€€€…Í”Y¥‘•½Õ‘¥½5½‘”èé,ÄäÈèÉ•ÑÕÉ¸0‰€ÄäÈ­‰ÁÌˆì(€€€…Í”Y¥‘•½Õ‘¥½5½‘”èé,ÄØÀèÉ•ÑÕÉ¸0‰€ÄØÀ­‰ÁÌˆì(€€€…Í”Y¥‘•½Õ‘¥½5½‘”èé,ÄÈàèÉ•ÑÕÉ¸0‰€ÄÈà­‰ÁÌˆì(€€€…Í”Y¥‘•½Õ‘¥½5½‘”èé,äØèÉ•ÑÕÉ¸0‰€äØ­‰ÁÌˆì(€€€…Í”Y¥‘•½Õ‘¥½5½‘”èé,ØÐèÉ•ÑÕÉ¸0‰€ØÐ­‰ÁÌˆì(€€€…Í”Y¥‘•½Õ‘¥½5½‘”èé5ÕÑ”èÉ•ÑÕÉ¸0‰5ÕÑ”…Õ‘¥¼ˆì(€€€ô(€€€É•ÑÕÉ¸0‰ÕÑ¼ˆì)ô()ÍÑèéÝÍÑÉ¥¹œY¥‘•½½µÁÉ•ÍÍ¥½¹M•ÉÙ¥”èéAÉ•Í•Ñ1…‰•°¡Y¥‘•½¹½‘¥¹AÉ•Í•ÐÁÉ•Í•Ð¤)ì(€€€ÍÝ¥Ñ €¡ÁÉ•Í•Ð¤(€€€ì(€€€…Í”Y¥‘•½¹½‘¥¹AÉ•Í•ÐèéM±½ÜèÉ•ÑÕÉ¸0‰M±½Ü€¼‰•ÑÑ•È½µÁÉ•ÍÍ¥½¸ˆì(€€€…Í”Y¥‘•½¹½‘¥¹AÉ•Í•Ðèé5•‘¥Õ´èÉ•ÑÕÉ¸0‰5•‘¥Õ´ˆì(€€€…Í”Y¥‘•½¹½‘¥¹AÉ•Í•Ðèé…ÍÐèÉ•ÑÕÉ¸0‰…ÍÐˆì(€€€ô(€€€É•ÑÕÉ¸0‰M±½Ü€¼‰•ÑÑ•È½µÁÉ•ÍÍ¥½¸ˆì)ô()ÍÑèéÝÍÑÉ¥¹œY¥‘•½½µÁÉ•ÍÍ¥½¹M•ÉÙ¥”èé¹½‘•É1…‰•°¡Y¥‘•½¹½‘•É5½‘”µ½‘”¤)ì(€€€ÍÝ¥Ñ €¡µ½‘”¤(€€€ì(€€€…Í”Y¥‘•½¹½‘•É5½‘”èéÁÔèÉ•ÑÕÉ¸0‰AT€¼‰•ÍÐ½µÁÉ•ÍÍ¥½¸ˆì(€€€…Í”Y¥‘•½¹½‘•É5½‘”èéÕÑ½µ…Ñ¥ÁÔèÉ•ÑÕÉ¸0‰ÕÑ½µ…Ñ¥ŒAT€¼AT™…±±‰…¬ˆì(€€€ô(€€€É•ÑÕÉ¸0‰AT€¼‰•ÍÐ½µÁÉ•ÍÍ¥½¸ˆì)ô(