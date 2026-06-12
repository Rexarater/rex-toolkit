#pragma once

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

enum class ImageFormat
{
    Png,
    Jpg,
    Webp,
    Bmp
};

enum class ConversionStatus
{
    Pending,
    Converting,
    Complete,
    Failed,
    Skipped
};

enum class OutputDirectoryMode
{
    ConvertedSubfolder,
    SameFolder,
    ChosenFolder
};

enum class ConflictBehavior
{
    AutoRename,
    Overwrite,
    Skip
};

enum class JpgBackground
{
    White,
    Black
};

struct ConversionOptions
{
    ImageFormat outputFormat = ImageFormat::Png;
    OutputDirectoryMode outputDirectoryMode = OutputDirectoryMode::ConvertedSubfolder;
    std::filesystem::path outputDirectory;
    ConflictBehavior conflictBehavior = ConflictBehavior::AutoRename;
    int jpgQuality = 90;
    int webpQuality = 90;
    bool webpLossless = false;
    JpgBackground jpgBackground = JpgBackground::White;
};

struct ConversionJob
{
    std::filesystem::path inputPath;
    std::filesystem::path outputPath;
    std::wstring fileName;
    std::wstring inputFormat;
    ImageFormat outputFormat = ImageFormat::Png;
    ConversionStatus status = ConversionStatus::Pending;
    std::wstring errorMessage;
    unsigned long long fileSize = 0;
    unsigned int width = 0;
    unsigned int height = 0;
};

struct ConversionResult
{
    ConversionJob job;
    size_t index = 0;
};

class SupportedFormatRegistry
{
public:
    static bool IsSupportedInputExtension(const std::filesystem::path& path);
    static std::wstring ExtensionFor(ImageFormat format);
    static std::wstring LabelFor(ImageFormat format);
    static std::optional<ImageFormat> ImageFormatFromExtension(const std::filesystem::path& path);
};

class ImageConverter
{
public:
    bool ReadImageInfo(ConversionJob& job, std::wstring& errorMessage) const;
    bool ConvertImage(ConversionJob& job, const ConversionOptions& options, std::wstring& errorMessage) const;
    bool CanEncode(ImageFormat format) const;

private:
    std::filesystem::path BuildOutputPath(const ConversionJob& job, const ConversionOptions& options, std::wstring& errorMessage) const;
    std::filesystem::path ResolveConflict(const std::filesystem::path& requestedPath, ConflictBehavior behavior, bool& skipped) const;
};

class FileConversionService
{
public:
    using ProgressCallback = std::function<void(const ConversionResult&)>;

    std::optional<ConversionJob> CreateJob(const std::filesystem::path& path, std::wstring& warning) const;
    std::vector<ImageFormat> SupportedOutputFormats() const;
    bool CanConvertAny(const std::vector<ConversionJob>& jobs) const;
    void ConvertBatch(
        std::vector<ConversionJob> jobs,
        ConversionOptions options,
        const std::atomic_bool& cancelRequested,
        const ProgressCallback& callback) const;

private:
    ImageConverter imageConverter_;
};
