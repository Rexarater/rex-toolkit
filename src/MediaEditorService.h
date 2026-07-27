#pragma once

#include "FileConversionService.h"
#include "VideoCompressionService.h"

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

enum class MediaEditorMediaKind
{
    Unsupported,
    Image,
    Video
};

class MediaTypeDetector
{
public:
    static MediaEditorMediaKind KindFromExtension(const std::filesystem::path& path);
    static bool IsSupportedImage(const std::filesystem::path& path);
    static bool IsSupportedVideo(const std::filesystem::path& path);
    static bool ValidateFile(
        const std::filesystem::path& path,
        MediaEditorMediaKind expectedKind,
        std::wstring& errorMessage);
};

struct MediaEditorImageBuffer
{
    UINT width = 0;
    UINT height = 0;
    UINT stride = 0;
    std::vector<BYTE> pixels;

    bool IsValid() const;
};

struct ClipboardMediaAvailability
{
    bool hasImagePixels = false;
    bool hasSupportedFiles = false;
    bool hasImageFile = false;
    bool hasVideoFiles = false;
    size_t videoFileCount = 0;

    bool HasCompatibleMedia() const;
    std::wstring Summary() const;
};

struct ClipboardMediaPayload
{
    std::optional<MediaEditorImageBuffer> image;
    std::vector<std::filesystem::path> files;
};

class ClipboardMediaService
{
public:
    ClipboardMediaAvailability DetectCompatibleMedia() const;
    std::optional<ClipboardMediaPayload> ReadCompatibleMedia(std::wstring& errorMessage) const;
    bool CopyImage(const MediaEditorImageBuffer& image, std::wstring& errorMessage) const;
};

struct MediaEditorPoint
{
    float x = 0.0f;
    float y = 0.0f;
};

struct MediaEditorCropRect
{
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    float Width() const;
    float Height() const;
};

struct MediaEditorDrawingStroke
{
    std::vector<MediaEditorPoint> points;
    COLORREF color = RGB(255, 74, 92);
    float thickness = 6.0f;
    float opacity = 1.0f;
    bool eraser = false;
};

struct MediaEditorImageSnapshot
{
    MediaEditorCropRect crop;
    std::vector<MediaEditorDrawingStroke> strokes;
    int rotationQuarterTurns = 0;
};

class ImageEditingSession
{
public:
    bool LoadFromFile(const std::filesystem::path& path, std::wstring& errorMessage);
    bool LoadFromBuffer(
        MediaEditorImageBuffer image,
        const std::filesystem::path& sourcePath,
        std::wstring& errorMessage);
    void Reset();

    bool IsLoaded() const;
    UINT Width() const;
    UINT Height() const;
    const MediaEditorImageBuffer& Image() const;
    const std::filesystem::path& SourcePath() const;
    const MediaEditorCropRect& Crop() const;
    const std::vector<MediaEditorDrawingStroke>& Strokes() const;
    int RotationQuarterTurns() const;

    void BeginEdit();
    void CommitEdit();
    void CancelEdit();
    void SetCrop(MediaEditorCropRect crop);
    void ResetCrop();
    bool AddStroke(MediaEditorDrawingStroke stroke);
    bool RotateClockwise();

    bool CanUndo() const;
    bool CanRedo() const;
    bool Undo();
    bool Redo();

    bool Flatten(MediaEditorImageBuffer& result, std::wstring& errorMessage) const;
    bool SaveAs(
        const std::filesystem::path& path,
        ImageFormat format,
        std::wstring& errorMessage) const;

private:
    MediaEditorImageSnapshot CaptureSnapshot() const;
    bool RestoreSnapshot(const MediaEditorImageSnapshot& snapshot);
    bool RebuildOrientedImage();
    MediaEditorCropRect ClampCrop(MediaEditorCropRect crop) const;

    MediaEditorImageBuffer originalImage_;
    MediaEditorImageBuffer orientedImage_;
    std::filesystem::path sourcePath_;
    MediaEditorCropRect crop_;
    std::vector<MediaEditorDrawingStroke> strokes_;
    int rotationQuarterTurns_ = 0;
    bool editActive_ = false;
    MediaEditorImageSnapshot editStart_;
    std::vector<MediaEditorImageSnapshot> undoStack_;
    std::vector<MediaEditorImageSnapshot> redoStack_;
};

struct VideoEditorClipModel
{
    unsigned long long id = 0;
    std::filesystem::path sourcePath;
    VideoAnalysis analysis;
    double trimInSeconds = 0.0;
    double trimOutSeconds = 0.0;
    double timelineOffsetSeconds = 0.0;

    double Duration() const;
    double TimelineStart() const;
    double TimelineEnd() const;
};

struct VideoTimelineSnapshot
{
    std::vector<VideoEditorClipModel> clips;
    int selectedIndex = -1;
    double playheadSeconds = 0.0;
    double durationSeconds = 0.0;
};

struct VideoTimelineLocation
{
    int clipIndex = -1;
    double sourceSeconds = 0.0;
    double timelineSeconds = 0.0;
};

class VideoTimelineModel
{
public:
    const std::vector<VideoEditorClipModel>& Clips() const;
    int SelectedIndex() const;
    double PlayheadSeconds() const;
    double DurationSeconds() const;

    void Reset();
    void AddClip(VideoAnalysis analysis);
    bool SelectClip(int index);
    void SetPlayhead(double seconds);
    bool SetTrim(int index, double trimInSeconds, double trimOutSeconds, bool checkpoint = true);
    bool SplitSelectedAtPlayhead();
    bool DeleteSelected();
    bool MoveSelected(int destinationIndex);
    bool MoveSelectedTo(double timelineStartSeconds, bool checkpoint = true);

    void BeginEdit();
    void CommitEdit();
    void CancelEdit();
    bool CanUndo() const;
    bool CanRedo() const;
    bool Undo();
    bool Redo();

    std::optional<VideoTimelineLocation> Locate(double timelineSeconds) const;
    double ClipStartTime(int index) const;

private:
    VideoTimelineSnapshot CaptureSnapshot() const;
    void RestoreSnapshot(const VideoTimelineSnapshot& snapshot);
    void PushCheckpoint();
    void ClampState();
    void SortClipsByTimeline(unsigned long long selectedId);
    double NearestAvailableStart(int index, double desiredStart) const;

    std::vector<VideoEditorClipModel> clips_;
    int selectedIndex_ = -1;
    double playheadSeconds_ = 0.0;
    double durationSeconds_ = 0.0;
    unsigned long long nextClipId_ = 1;
    bool editActive_ = false;
    bool editChanged_ = false;
    VideoTimelineSnapshot editStart_;
    std::vector<VideoTimelineSnapshot> undoStack_;
    std::vector<VideoTimelineSnapshot> redoStack_;
};

enum class VideoEditorExportMode
{
    KeepOriginalQuality,
    MakeFileSmaller,
    FitUnderSizeLimit
};

enum class VideoEditorExportPhase
{
    Idle,
    Preparing,
    Exporting,
    Compressing,
    Verifying,
    Complete,
    Failed,
    Cancelled
};

struct VideoEditorExportOptions
{
    VideoEditorExportMode mode = VideoEditorExportMode::KeepOriginalQuality;
    unsigned long long targetSizeBytes = 300ULL * 1024ULL * 1024ULL;
    std::filesystem::path outputPath;
    VideoEncoderMode encoderMode = VideoEncoderMode::AutomaticGpu;
};

struct VideoEditorExportProgress
{
    VideoEditorExportPhase phase = VideoEditorExportPhase::Idle;
    double progress = 0.0;
    double elapsedSeconds = 0.0;
    double estimatedRemainingSeconds = 0.0;
    std::wstring message;
};

struct VideoEditorExportResult
{
    bool success = false;
    bool cancelled = false;
    std::filesystem::path outputPath;
    unsigned long long outputSizeBytes = 0;
    std::wstring details;
    std::wstring errorMessage;
};

class VideoEditorExportService
{
public:
    using ProgressCallback = std::function<void(const VideoEditorExportProgress&)>;

    VideoEditorExportResult Export(
        const std::vector<VideoEditorClipModel>& clips,
        const VideoEditorExportOptions& options,
        const std::atomic_bool& cancelRequested,
        const ProgressCallback& progressCallback) const;

    static std::filesystem::path SuggestedOutputPath(
        const std::vector<VideoEditorClipModel>& clips,
        VideoEditorExportMode mode,
        unsigned long long targetSizeBytes,
        const std::filesystem::path& folder);
    static std::wstring ModeLabel(VideoEditorExportMode mode);
    static std::wstring PhaseLabel(VideoEditorExportPhase phase);

private:
    ExternalToolService externalToolService_;
    ProcessRunner processRunner_;
    VideoCompressionService compressionService_;
};

class VideoPreviewService
{
public:
    VideoPreviewService();
    ~VideoPreviewService();

    VideoPreviewService(const VideoPreviewService&) = delete;
    VideoPreviewService& operator=(const VideoPreviewService&) = delete;

    bool Initialize(HWND videoHost, HWND notificationWindow, UINT readyMessage, UINT errorMessage);
    void Shutdown();
    void Close();
    bool Open(const std::filesystem::path& path, double trimInSeconds, double trimOutSeconds);
    void Play();
    void Pause();
    void TogglePlayback();
    void Seek(double sourceSeconds);
    bool StepFrame(int direction);
    void SetPlaybackRange(double trimInSeconds, double trimOutSeconds);
    void SetVolume(float volume);
    void SetMuted(bool muted);
    bool Tick();
    void UpdateVideo();

    bool IsReady() const;
    bool IsPlaying() const;
    bool IsNavigationPending() const;
    bool IsMuted() const;
    double PositionSeconds() const;
    double DurationSeconds() const;
    const std::wstring& ErrorMessage() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class ShellIntegrationService
{
public:
    bool SetMediaEditorContextMenuEnabled(
        bool enabled,
        const std::filesystem::path& executablePath,
        std::wstring& errorMessage) const;
    bool IsMediaEditorContextMenuEnabled() const;
};
