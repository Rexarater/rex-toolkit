#pragma once

#include "AnimeTrackerService.h"
#include "FileConversionService.h"
#include "MacroRecorderService.h"
#include "MediaEditorPage.h"
#include "MediaDownloadService.h"
#include "ReminderService.h"
#include "SmartFileTransferService.h"
#include "UpdateChecker.h"
#include "UiComponents.h"
#include "VideoCompressionService.h"

#include <windows.h>
#include <gdiplus.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

enum class Page
{
    Favorites,
    AllTools,
    Settings,
    Tool
};

enum class ToolKind
{
    None,
    AutoClicker,
    FileConverter,
    MediaDownloader,
    AnimeTracker,
    Reminders,
    SmartFileTransfer,
    MacroRecorder,
    VideoCompressor,
    MediaEditor
};

enum class OutputMouseButton
{
    Left,
    Right,
    Middle
};

enum class OutputInputKind
{
    Keyboard,
    MouseButton
};

enum class ActivationInputKind
{
    Keyboard,
    MouseButton
};

enum class ActivationMouseButton
{
    Left,
    Right,
    Middle,
    X1,
    X2
};

enum class DropdownKind
{
    None,
    FileOutputFormat,
    FileConflictBehavior,
    FileJpgBackground,
    FileFormatOptions,
    MediaFormat,
    MediaQuality,
    AnimeFilter,
    AnimeManagement,
    ReminderFilter,
    ReminderSort,
    ReminderPreset,
    ReminderPriority,
    ReminderRepeat,
    ReminderAlert,
    ReminderSnooze,
    ReminderAmPm,
    SettingsStartPage,
    SettingsClockFormat,
    SettingsTheme,
    SettingsBackgroundType,
    SettingsBackgroundPreset,
    SettingsBackgroundScale,
    SettingsBackgroundPosition,
    SettingsBackgroundVignette,
    SettingsGradientDirection,
    SmartTransferExpiration,
    MacroMouseMode,
    MacroCaptureRate,
    MacroPlaybackSpeed,
    MacroPlaybackBackend,
    MacroLoopMode,
    MacroStartDelay,
    VideoCompressionTargetPreset,
    VideoCompressionUnit,
    VideoCompressionMode,
    VideoCompressionResolution,
    VideoCompressionFps,
    VideoCompressionAudio,
    VideoCompressionPreset,
    VideoCompressionEncoder,
    VideoCompressionConflict
};

enum class AnimeTrackerTab
{
    Watchlist,
    Airing,
    Favorites
};

enum class AnimeDetailTab
{
    Overview,
    Episodes,
    Sequels,
    Notes
};

enum class SmartTransferTab
{
    Send,
    Receive
};

enum class MacroRecorderTab
{
    Record,
    Playback,
    Library
};

enum class SettingsSection
{
    General,
    SmartTransfer,
    WindowsIntegration,
    MacroRecorder,
    Appearance,
    Updates,
    About
};

enum class AppearanceTab
{
    Themes,
    BackgroundStudio,
    Interface
};

enum class DefaultStartPage
{
    Favorites,
    AllTools
};

enum class ClockFormat
{
    MonthDay24,
    MonthDay12,
    Iso24,
    Friendly12
};

enum class AppTheme
{
    Dark,
    Light,
    Midnight,
    Forest,
    Rose,
    HighContrast
};

enum class BackgroundType
{
    Default,
    CustomImage,
    Gradient
};

enum class BackgroundScaleMode
{
    Cover,
    Contain,
    Stretch,
    Tile,
    Original
};

enum class BackgroundPosition
{
    Center,
    Top,
    Bottom,
    Left,
    Right,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
    Custom
};

enum class BackgroundVignette
{
    Off,
    Low,
    Medium,
    High
};

enum class BackgroundPreset
{
    Subtle,
    Blurred,
    DarkGlass,
    Cinematic,
    Vibrant,
    Minimal
};

enum class GradientDirection
{
    TopToBottom,
    LeftToRight,
    Diagonal,
    Radial
};

enum class UiSurfaceStyle
{
    Solid,
    Translucent,
    Glass
};

enum class AppearanceSlider
{
    None,
    Opacity,
    Blur,
    Darken,
    Tint,
    GradientIntensity,
    ImageZoom,
    ImageRotation,
    UiSurfaceOpacity
};

struct AppearanceSettings
{
    BackgroundType backgroundType = BackgroundType::Default;
    bool backgroundEnabled = true;
    std::wstring imageRelativePath;
    int opacity = 30;
    int blur = 12;
    int darken = 45;
    int tintStrength = 50;
    bool blendWithTheme = true;
    BackgroundScaleMode scaleMode = BackgroundScaleMode::Cover;
    BackgroundPosition position = BackgroundPosition::Center;
    int offsetX = 0;
    int offsetY = 0;
    int imageZoom = 100;
    int imageRotation = 0;
    BackgroundVignette vignette = BackgroundVignette::Low;
    BackgroundPreset preset = BackgroundPreset::Subtle;
    bool protectReadability = true;
    COLORREF gradientColor1 = RGB(23, 54, 99);
    COLORREF gradientColor2 = RGB(18, 20, 28);
    GradientDirection gradientDirection = GradientDirection::Diagonal;
    int gradientIntensity = 80;
    bool gradientBlendWithTheme = true;
    UiSurfaceStyle uiSurfaceStyle = UiSurfaceStyle::Solid;
    int uiSurfaceOpacity = 100;
    bool borderEdgeGlowEnabled = true;
    bool smoothScrollingEnabled = true;
};

struct ToolDefinition
{
    std::wstring id;
    std::wstring name;
    std::wstring description;
    bool favorite;
    ToolKind kind;
};

struct AutoClickerState
{
    int clicksPerSecond = 8;
    ActivationInputKind activationKind = ActivationInputKind::Keyboard;
    UINT activationKey = VK_F6;
    ActivationMouseButton activationMouseButton = ActivationMouseButton::X1;
    OutputInputKind outputKind = OutputInputKind::MouseButton;
    UINT outputKey = VK_SPACE;
    OutputMouseButton outputButton = OutputMouseButton::Left;
    OutputInputKind alternateOutputKind = OutputInputKind::MouseButton;
    UINT alternateOutputKey = VK_RETURN;
    OutputMouseButton alternateOutputButton = OutputMouseButton::Right;
    bool alternateOutputEnabled = false;
    bool toggleModeEnabled = false;
    bool running = false;
};

struct AppSettings
{
    std::filesystem::path defaultOutputFolder;
    DefaultStartPage startPage = DefaultStartPage::Favorites;
    ClockFormat clockFormat = ClockFormat::MonthDay24;
    AppTheme theme = AppTheme::Dark;
    AppearanceSettings appearance;
    bool minimizeToTrayOnClose = false;
    bool startWithWindowsToTray = false;
    bool smartTransferWebRtcFallback = true;
    bool smartTransferWebRtcDiagnostics = false;
    std::wstring smartTransferStunServers = L"stun:stun.l.google.com:19302";
    bool mediaEditorExplorerIntegration = true;
    long long updateNotificationDismissedUntil = 0;
};

class ToolkitApp
{
public:
    explicit ToolkitApp(HINSTANCE instance);

    static bool ActivateExistingInstanceIfRunning(
        bool restoreExisting = true,
        const std::filesystem::path& editPath = {});

    int Run(int showCommand);
    void SetStartMinimizedToTray(bool value);

    void SetPendingEditPath(std::filesystem::path path);
private:
    struct ToolCardCacheEntry
    {
        ToolKind tool = ToolKind::None;
        bool hovered = false;
        bool favoriteHovered = false;
        bool favorite = false;
        int width = 0;
        int height = 0;
        HDC dc = nullptr;
        HBITMAP bitmap = nullptr;
        HBITMAP previousBitmap = nullptr;
    };

    struct AnimeBitmapCacheEntry
    {
        std::unique_ptr<Gdiplus::Bitmap> bitmap;
        std::uintmax_t bytes = 0;
        std::uint64_t lastUse = 0;
    };

    struct AnimeSearchCoverCacheEntry
    {
        std::unique_ptr<Gdiplus::Bitmap> bitmap;
        int width = 0;
        int height = 0;
        int radius = 0;
        std::uint64_t lastUse = 0;
    };


    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK MacroOverlayWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK KeyboardHookProc(int code, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK MouseHookProc(int code, WPARAM wParam, LPARAM lParam);
    static VOID CALLBACK ClockTimerCallback(PVOID context, BOOLEAN timerOrWaitFired);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMacroOverlayMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    bool RegisterWindowClass();
    bool CreateMainWindow(int showCommand);
    bool ClaimSingleInstance();
    void ReleaseSingleInstance();

    void ApplyDarkTitleBar();
    void LoadLogoResource();
    std::unique_ptr<Gdiplus::Bitmap> LoadPngResource(int resourceId) const;
    void LoadToolIconResources();
    std::unique_ptr<Gdiplus::Bitmap> CreateTintedBitmap(Gdiplus::Bitmap* bitmap, COLORREF tint) const;
    void RefreshTintedIconResources();
    void LoadFavorites();
    void SaveFavorites() const;
    void LoadWindowSettings();
    void SaveWindowSettings() const;
    void LoadAppSettings();
    void SaveAppSettings() const;
    void ApplyStartupRegistration();
    bool SetStartWithWindowsRegistration(bool enabled, std::wstring& errorMessage) const;
    std::filesystem::path AppExecutablePath() const;
    std::wstring StartupLaunchCommand() const;
    std::filesystem::path BundledInterceptionInstallerPath() const;
    bool PromptAndLaunchInterceptionInstaller();
    void LoadMediaDownloadSettings();
    void SaveMediaDownloadSettings() const;
    void LoadAutoClickerSettings();
    void SaveAutoClickerSettings() const;
    std::wstring SettingsDirectory() const;
    std::wstring FavoritesFilePath() const;
    std::wstring WindowSettingsFilePath() const;
    std::wstring AppSettingsFilePath() const;
    std::wstring MediaDownloadSettingsFilePath() const;
    std::wstring AutoClickerSettingsFilePath() const;
    void RecalculateLayout();
    void ClampScrollOffset();
    void SetScrollOffset(int offset);
    void ScrollToOffset(int offset, bool animate);
    void StopSmoothScroll(bool resetWheelRemainder = true);
    void CommitSmoothScrollAtVisualOffset();
    bool StepSmoothScroll();
    void RefreshScrollableChildControls();
    void UpdateScrollBarThumbForOffset(int offset);
    void PaintScrollBar(HDC hdc);
    bool IsScrollBarVisible() const;
    void SelectPage(Page page);
    void OpenTool(ToolKind tool);
    void Paint(HDC hdc, const RECT& paintRect);
    bool EnsureBackBuffer(HDC hdc, int width, int height);
    void ReleaseBackBuffer();
    void PaintHeader(HDC hdc);
    void PaintVersionFooter(HDC hdc);
    void PaintLogo(HDC hdc, const RECT& bounds);
    void PaintBitmap(HDC hdc, Gdiplus::Bitmap* bitmap, const RECT& bounds);
    void PaintBitmapTinted(HDC hdc, Gdiplus::Bitmap* bitmap, const RECT& bounds, COLORREF tint);
    void PaintBitmapCover(HDC hdc, Gdiplus::Bitmap* bitmap, const RECT& bounds, int radius);
    void PaintHiddenEditText(
        HDC hdc,
        HWND edit,
        const RECT& editWindowRect,
        HFONT font,
        COLORREF color,
        bool multiline = false,
        const wchar_t* emptyText = nullptr);
    void PaintResizePlaceholder(HDC hdc);
    void PaintContent(HDC hdc);
    void PaintNavItem(HDC hdc, const RECT& bounds, const wchar_t* label, bool selected);
    void PaintNavItemEdgeGlow(HDC hdc, const RECT& bounds, POINT cursorPoint);
    void PaintEmptyState(HDC hdc, const wchar_t* title, const wchar_t* subtitle);
    void PaintToolCards(HDC hdc, const std::vector<const ToolDefinition*>& tools);
    void PaintToolCardBase(
        HDC hdc,
        const ToolDefinition& tool,
        const RECT& card,
        bool hovered,
        bool favoriteHovered);
    ToolCardCacheEntry* EnsureToolCardCache(
        HDC referenceDc,
        const ToolDefinition& tool,
        int width,
        int height,
        bool hovered,
        bool favoriteHovered);
    bool BuildToolCardCacheEntry(
        HDC referenceDc,
        const ToolDefinition& tool,
        int width,
        int height,
        bool hovered,
        bool favoriteHovered,
        ToolCardCacheEntry& entry);
    bool PaintCachedToolCard(
        HDC hdc,
        const ToolDefinition& tool,
        const RECT& card,
        bool hovered,
        bool favoriteHovered);
    void ReleaseToolCardCacheEntry(ToolCardCacheEntry& entry);
    void ReleaseToolCardCache();
    void InvalidateToolCardCache();
    void PaintToolCardEdgeGlow(HDC hdc, const RECT& card, POINT cursorPoint);
    void InvalidateToolCardGlow(const RECT& card);
    void PaintToolIcon(HDC hdc, ToolKind tool, const RECT& bounds);
    void PaintSearchIcon(HDC hdc, const RECT& bounds, COLORREF color);
    void PaintRightArrow(HDC hdc, const RECT& bounds, COLORREF color);
    void PaintCircularArrowIcon(HDC hdc, const RECT& bounds, COLORREF color, bool clockwise);
    void PaintAniListIcon(HDC hdc, const RECT& bounds);
    void PaintReminderIcon(HDC hdc, const RECT& bounds);
    void PaintReminderCalendar(HDC hdc);
    void PaintFavoriteStar(HDC hdc, const RECT& bounds, bool favorite, COLORREF activeColor);
    void PaintAutoClicker(HDC hdc);
    void PaintFileConverter(HDC hdc);
    void PaintMediaDownloader(HDC hdc);
    void PaintAnimeTracker(HDC hdc);
    void PaintAnimeImportChoiceOverlay(HDC hdc);
    void PaintReminders(HDC hdc);
    void PaintSmartFileTransfer(HDC hdc);
    void PaintMacroRecorder(HDC hdc);
    void PaintVideoCompressor(HDC hdc);
    void PaintVideoCompressorIcon(HDC hdc, const RECT& bounds);
    void PaintMediaEditor(HDC hdc);
    void PaintMacroOverlay(HWND hwnd, HDC hdc);
    void PaintSettings(HDC hdc);
    void PaintReminderBanner(HDC hdc);
    void PaintUpdateBanner(HDC hdc);
    void PaintSettingsSectionTab(HDC hdc, const RECT& bounds, const wchar_t* label, SettingsSection section);
    void PaintProgressBar(HDC hdc, const RECT& bounds, double progress);
    void PaintButton(HDC hdc, const RECT& bounds, const wchar_t* label, bool primary, bool active = false, bool enabled = true);
    void PaintBackButton(HDC hdc, const RECT& bounds);
    void PaintDropdownButton(HDC hdc, const RECT& bounds, const wchar_t* label, bool enabled = true, bool down = true);
    void PaintDropdownChevron(HDC hdc, const RECT& bounds, bool expanded, COLORREF color);
    void PaintChevron(HDC hdc, const RECT& bounds, bool down, COLORREF color);
    void PaintXIcon(HDC hdc, const RECT& bounds, COLORREF color);
    void PaintSlider(HDC hdc);
    void PaintModernSlider(HDC hdc, const RECT& track, const RECT& thumb, bool enabled = true);
    void PaintToggleSwitch(HDC hdc, const RECT& bounds, bool enabled, UINT_PTR animationKey, bool hovered, bool pressed);
    bool StepToggleSwitchAnimations();
    void OnMouseMove(POINT point);
    void OnLeftButtonDown(POINT point);
    void OnLeftButtonUp(POINT point);
    void OnMouseWheel(int delta, POINT screenPoint);
    void OnMouseButtonForBinding(OutputMouseButton button);
    void OnKeyboardForOutputBinding(UINT virtualKey);
    void OnMouseButtonForActivationBinding(ActivationMouseButton button);
    void UpdateAutoClickerSpeedFromPoint(int x);
    void SetAutoClickerRunning(bool running);
    void UpdateAutoClickerTimer();
    void AutoClickerLoop();
    void PerformAutoClick();
    void PaintCheckbox(HDC hdc, const RECT& bounds, bool checked, const wchar_t* label);
    void SetActivationKey(UINT virtualKey);
    void SetActivationMouseButton(ActivationMouseButton button);
    void AddFilesToConverterQueue(const std::vector<std::filesystem::path>& paths);
    void BrowseConverterFiles();
    std::optional<std::filesystem::path> PromptForSingleConverterOutputPath(const ConversionJob& job) const;
    std::optional<std::filesystem::path> PromptForBatchConverterOutputFolder() const;
    void StartFileConversion();
    void CancelFileConversion();
    void FinishConversionThread();
    void ApplyConversionResult(const ConversionResult& result);
    void UpdateFileConverterSummary();
    int VisibleConverterQueueRows() const;
    RECT ConverterQueueRowRect(size_t visibleIndex) const;
    RECT ConverterQueueRemoveRect(size_t visibleIndex) const;
    void ShowOutputFormatDropdown();
    void ShowConflictBehaviorDropdown();
    void ShowJpgBackgroundDropdown();
    void ShowFormatOptionsDropdown();
    void CycleConflictBehavior();
    void CycleJpgBackground();
    void ToggleWebpLossless();
    void UpdateConverterQualityFromPoint(int x);
    void CreateMediaDownloaderControls();
    void UpdateMediaDownloaderControls();
    void CreateSmartFileTransferControls();
    void UpdateSmartFileTransferControls();
    void AddFilesToSmartTransferQueue(const std::vector<std::filesystem::path>& paths);
    void BrowseSmartTransferFiles();
    void StartSmartTransferHosting();
    void StopSmartTransferHosting();
    void CopySmartTransferCode();
    void BrowseSmartTransferSaveFolder();
    void StartSmartTransferConnect();
    void StartSmartTransferDownload();
    void StartSmartTransferCreatePairingCode();
    void StartSmartTransferCreateReceiverResponse();
    void StartSmartTransferApplyReceiverResponse();
    void FinishSmartTransferThread();
    void ApplySmartTransferConnectResult(const SmartTransferConnectResult& result);
    void ApplySmartTransferDownloadProgress(const SmartTransferDownloadProgress& progress);
    void PollSmartTransferWebRtc();
    void ShowSmartTransferExpirationDropdown();
    void LoadMacroRecorderData();
    void SaveMacroRecorderSettings() const;
    std::wstring MacroRecorderSettingsFilePath() const;
    std::filesystem::path MacroDirectory() const;
    void CreateAllToolsSearchControl();
    void UpdateAllToolsSearchControl();
    void CreateMacroRecorderControls();
    void UpdateMacroRecorderControls();
    void ShowMacroOverlay();
    void ToggleMacroOverlay();
    void HideMacroOverlay();
    void LayoutMacroOverlay(HWND hwnd);
    void InvalidateMacroOverlay();
    void ApplyTitleBarTheme(HWND hwnd);
    void RegisterMacroHotkeys();
    void UnregisterMacroHotkeys();
    void BeginMacroHotkeyBinding(int hotkeyId);
    void CancelMacroHotkeyBinding();
    void HandleMacroHotkey(int id);
    void StartMacroRecording();
    void StopMacroRecording();
    void CancelMacroRecording();
    void SaveMacroRecordingOrDetails();
    void StartMacroPlayback();
    void StopMacroPlayback();
    void ImportMacro();
    void ExportSelectedMacro();
    void DeleteSelectedMacro();
    void DuplicateSelectedMacro();
    void SelectMacro(size_t index);
    void NewMacroDraft();
    void ShowMacroMouseModeDropdown();
    void ShowMacroCaptureRateDropdown();
    void ShowMacroPlaybackSpeedDropdown();
    void ShowMacroPlaybackBackendDropdown();
    void ShowMacroLoopModeDropdown();
    void ShowMacroStartDelayDropdown();
    std::wstring MacroRecordHotkeyLabel() const;
    std::wstring MacroPlayHotkeyLabel() const;
    std::wstring MacroStopHotkeyLabel() const;
    std::wstring MacroMouseModeLabel() const;
    std::wstring MacroCaptureRateLabel() const;
    std::wstring MacroPlaybackSpeedLabel() const;
    std::wstring MacroPlaybackBackendLabel() const;
    std::wstring MacroLoopModeLabel() const;
    std::wstring MacroStartDelayLabel() const;
    std::wstring MacroRecorderStatusLabel() const;
    std::wstring MacroPlaybackStatusLabel() const;
    double MacroPlaybackProgress() const;
    const MacroDefinition* MacroForPlayback() const;
    const MacroDefinition* SelectedMacro() const;
    MacroDefinition* SelectedMacro();
    RECT MacroListRowRect(size_t index) const;
    void CreateVideoCompressorControls();
    void UpdateVideoCompressorControls();
    void BrowseVideoCompressorInput();
    void SelectVideoCompressorInput(const std::filesystem::path& path);
    void ClearVideoCompressorInput();
    void BrowseVideoCompressorOutputFolder();
    void StartVideoCompressorAnalysis(const std::filesystem::path& path);
    void StartVideoCompression();
    void CancelVideoCompression();
    void FinishVideoCompressorThread();
    void ApplyVideoCompressionProgress(const VideoCompressionProgress& progress);
    void ApplyVideoCompressionResult(const VideoCompressionResult& result);
    void RecalculateVideoCompressionPlan();
    void ResetVideoCompressor();
    void OpenVideoCompressorOutputFile();
    void OpenVideoCompressorOutputFolder();
    unsigned long long VideoCompressorTargetBytes() const;
    std::wstring VideoCompressorOutputPreview() const;
    void ShowVideoCompressionTargetPresetDropdown();
    void ShowVideoCompressionUnitDropdown();
    void ShowVideoCompressionModeDropdown();
    void ShowVideoCompressionResolutionDropdown();
    void ShowVideoCompressionFpsDropdown();
    void ShowVideoCompressionAudioDropdown();
    void ShowVideoCompressionPresetDropdown();
    void ShowVideoCompressionEncoderDropdown();
    void ShowVideoCompressionConflictDropdown();
    void LoadVideoCompressorSettings();
    void SaveVideoCompressorSettings() const;
    std::wstring VideoCompressorSettingsFilePath() const;
    void BrowseMediaOutputFolder();
    void BrowseDefaultOutputFolder();
    void AnalyzeMediaUrl();
    void StartMediaMusicAnalysis();
    void StartMediaDownload();
    void CancelMediaDownload();
    void FinishMediaThread();
    void ApplyMediaJobUpdate(const MediaDownloadJob& job);
    void StartUpdateCheck(bool silent = false);
    void FinishUpdateThread();
    void ApplyUpdateCheckResult(const UpdateCheckResult& result);
    void StartUpdateInstall();
    std::wstring ResolveUpdatePackageUrl() const;
    bool DownloadUpdatePackage(const std::wstring& downloadUrl, std::filesystem::path& packagePath, std::wstring& errorMessage) const;
    bool CreateAndLaunchUpdateInstaller(const std::filesystem::path& packagePath, std::wstring& errorMessage) const;
    void ShowMediaFormatDropdown();
    void ShowMediaQualityDropdown();
    void ShowSettingsStartPageDropdown();
    void ShowSettingsClockFormatDropdown();
    void ShowSettingsThemeDropdown();
    void ShowSettingsBackgroundTypeDropdown();
    void ShowSettingsBackgroundPresetDropdown();
    void ShowSettingsBackgroundScaleDropdown();
    void ShowSettingsBackgroundPositionDropdown();
    void ShowSettingsBackgroundVignetteDropdown();
    void ShowSettingsGradientDirectionDropdown();
    void ChooseAppearanceBackgroundImage();
    void RemoveAppearanceBackgroundImage();
    void ChooseAppearanceGradientColor(bool firstColor);
    void ApplyAppearancePreset(BackgroundPreset preset);
    void ResetAppearanceBackground();
    void ResetAppearance();
    void UpdateAppearanceSliderFromPoint(POINT point);
    void SaveAndApplyAppearance(bool rebuildImage = true);
    void InvalidateAppearanceBackgroundCache(bool reloadSource = false);
    void EnsureAppearanceBackgroundCache();
    void ReleaseAppearanceBackgroundNativeCache();
    void PaintAppBackground(HDC hdc);
    void PaintAppearancePreview(HDC hdc, const RECT& bounds);
    void ResetAppearanceImageTransform();
    bool LoadAppearanceSourceImage(std::wstring& errorMessage);
    std::filesystem::path AppearanceBackgroundsDirectory() const;
    std::filesystem::path AppearanceImagePath() const;
    void LoadRemindersData();
    void SaveRemindersData();
    std::wstring RemindersFilePath() const;
    void CreateReminderControls();
    void UpdateReminderControls();
    RECT ReminderEditWindowRect(HWND edit, const RECT& rect, bool multiline = false) const;
    void SetReminderFormDefaults();
    void LoadReminderFormFromSelection();
    void SaveReminderFromForm();
    void CompleteReminderAt(size_t index);
    void DeleteReminderAt(size_t index);
    void SnoozeReminderAt(size_t index, int minutes);
    void SnoozeReminderAlert(int minutes);
    void DismissReminderBanner();
    void ViewReminderFromBanner();
    void UpdateReminderBanner();
    void HandleReminderDueNowNotification();
    bool ShouldShowUpdateNotification(const UpdateCheckResult& result) const;
    void DismissUpdateNotification();
    void PlayReminderNotificationSound() const;
    void BringReminderWindowToFront();
    std::wstring ReminderNotificationSoundFilePath() const;
    int FindReminderIndexById(const std::wstring& id) const;
    void ShowReminderFilterDropdown();
    void ShowReminderSortDropdown();
    void ShowReminderPresetDropdown();
    void ShowReminderPriorityDropdown();
    void ShowReminderRepeatDropdown();
    void ShowReminderAlertDropdown();
    void ShowReminderSnoozeDropdown(int reminderIndex, const RECT& anchor);
    void ShowReminderAmPmDropdown();
    void ShowReminderCalendar();
    bool HandleReminderCalendarClick(POINT point);
    RECT ReminderCalendarDayRect(int day) const;
    void SetReminderDateFromCalendar(int day);
    std::vector<size_t> VisibleReminderIndexes() const;
    RECT ReminderRowRect(size_t visibleIndex) const;
    RECT ReminderActionRect(size_t visibleIndex, int actionIndex) const;
    void LoadAnimeTrackerData();
    void SaveAnimeTrackerData();
    std::wstring AnimeTrackerFilePath() const;
    void CreateAnimeTrackerControls();
    void UpdateAnimeTrackerControls();
    void StartAnimeSearch(bool appendResults);
    void StartAnimeImport();
    void FinishAnimeThread();
    Gdiplus::Bitmap* FindAnimeBitmap(const std::wstring& imageUrl);
    Gdiplus::Bitmap* FindAnimeSearchCoverBitmap(
        const std::wstring& imageUrl,
        int width,
        int height,
        int radius);
    void TrimAnimeSearchCoverCache();
    void StoreAnimeBitmap(const std::wstring& imageUrl, std::unique_ptr<Gdiplus::Bitmap> bitmap);
    void TrimAnimeBitmapCache();
    void ApplyAnimeSearchResponse(const AnimeSearchResponse& response, const std::wstring& message, bool appendResults);
    void ApplyAnimeImportResult(const AnimeImportResult& result, const std::wstring& userName, AnimeImportSource source, const std::wstring& message);
    void ShowAnimeImportSourceChoice(
        const std::wstring& userName,
        const AnimeImportResult& aniListResult,
        const std::wstring& aniListMessage,
        const AnimeImportResult& myAnimeListResult,
        const std::wstring& myAnimeListMessage);
    void ApplyPendingAnimeImportChoice(AnimeImportSource source);
    void CancelPendingAnimeImportChoice();
    void AddAnimeFromSearch(size_t index);
    void AddAnimeFromRelation(size_t index);
    void AddAnimeAiringReminder(const std::wstring& animeTitle, const AiringInfo& airingInfo);
    bool HasAnimeAiringReminder(const std::wstring& animeTitle, const AiringInfo& airingInfo) const;
    void RefreshAnimeEntry(size_t index);
    void RefreshAllAnime();
    void ApplyAnimeRefreshResult(const AnimeSearchResult& result, int listIndex, const std::wstring& message);
    void SelectAnimeEntry(int index);
    void OpenAnimeSearchResult(size_t index);
    void RemoveAnimeEntry(size_t index);
    void IncrementAnimeEpisode(size_t index);
    void DecrementSelectedAnimeEpisode();
    void CycleSelectedAnimeStatus();
    void ToggleSelectedAnimeFavorite();
    void SaveSelectedAnimeNotes();
    void ShowAnimeFilterDropdown();
    void ShowAnimeManagementDropdown();
    std::vector<size_t> VisibleAnimeEntryIndexes() const;
    std::vector<AnimeRelation> VisibleUpcomingSequels() const;
    std::wstring AnimeSearchText() const;
    std::wstring AnimeStatusText(const AnimeEntry& entry) const;
    std::wstring AnimeProgressText(const AnimeEntry& entry) const;
    RECT AnimeSearchResultCardRect(size_t index) const;
    RECT AnimeSearchResultAddRect(size_t index) const;
    RECT AnimeListRowRect(size_t visibleIndex) const;
    RECT AnimeListActionRect(size_t visibleIndex, int actionIndex) const;
    RECT AnimeSequelActionRect(size_t index, int actionIndex) const;
    RECT AnimeEpisodeRect(size_t episode) const;
    std::optional<int> AnimeEpisodeAtPoint(POINT point, int totalEpisodes) const;
    void EnableAutomationInputs();
    void DisableAutomationInputs();
    void InstallInputHooks();
    void RemoveInputHooks();
    bool HandleKeyboardHook(WPARAM message, const KBDLLHOOKSTRUCT& keyboard);
    bool HandleMouseHook(WPARAM message, const MSLLHOOKSTRUCT& mouse);
    bool IsActivationMouseMessage(WPARAM message, const MSLLHOOKSTRUCT& mouse, bool down) const;

    void OpenDropdown(DropdownKind kind, const RECT& anchor, const std::vector<std::wstring>& labels, const std::vector<int>& values, const std::vector<bool>& enabled, int selectedValue);
    void CloseDropdown();
    void UpdateCurrentToolControls();
    void PaintDropdown(HDC hdc);
    void UpdateMediaEditorPage();
    bool HandleDropdownClick(POINT point);
    RECT ButtonRectAtPoint(POINT point) const;
    void UpdateButtonHover(POINT point);
    bool IsPointOverInteractiveSurface(POINT point) const;
    void ApplyCursorForPoint(POINT point) const;
    void RebuildVisibleToolsCache();
    const std::vector<const ToolDefinition*>& VisibleToolsForCurrentPage() const;
    const ToolDefinition* FindTool(ToolKind tool) const;
    RECT ToolCardRect(size_t index) const;
    RECT ToolFavoriteRect(const RECT& card) const;
    void ToggleFavorite(ToolKind tool);
    std::wstring ActivationKeyLabel() const;
    std::wstring OutputButtonLabel() const;
    std::wstring StatusLabel(ConversionStatus status) const;
    std::wstring OutputFormatLabel(ImageFormat format) const;
    std::wstring ConflictBehaviorLabel() const;
    double ConverterProgress() const;
    bool CanStartMediaDownload() const;
    std::wstring MediaQualityLabel() const;
    std::wstring MediaSetupMessage() const;
    std::wstring CurrentDateTimeLabel() const;
    void RefreshClockDisplay();
    std::wstring StartPageLabel() const;
    std::wstring ClockFormatLabel() const;
    std::wstring ThemeLabel() const;
    std::wstring BackgroundTypeLabel() const;
    std::wstring BackgroundPresetLabel() const;
    std::wstring BackgroundScaleLabel() const;
    std::wstring BackgroundPositionLabel() const;
    std::wstring BackgroundVignetteLabel() const;
    std::wstring GradientDirectionLabel() const;
    std::wstring UiSurfaceStyleLabel() const;
    std::wstring SmartTransferExpirationLabel() const;
    std::wstring SmartTransferHostStatusLabel() const;
    std::wstring SmartTransferClientStatusLabel() const;
    void ApplyTheme();
    std::wstring ReminderFilterLabel() const;
    std::wstring ReminderSortLabel() const;
    std::wstring ReminderCategoryText() const;
    std::wstring ReminderAlertSummaryLabel() const;
    void MinimizeToTray();
    void RestoreFromTray();
    void AddTrayIcon();
    void RemoveTrayIcon();
    void ShowTrayMenu(POINT screenPoint);
    int Dips(int value) const;
    int MacroOverlayDips(int value) const;

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    ULONG_PTR gdiplusToken_ = 0;
    std::unique_ptr<Gdiplus::Bitmap> logo_;
    std::unique_ptr<Gdiplus::Bitmap> autoClickerIcon_;
    std::unique_ptr<Gdiplus::Bitmap> fileConverterIcon_;
    std::unique_ptr<Gdiplus::Bitmap> mediaDownloaderIcon_;
    std::unique_ptr<Gdiplus::Bitmap> remindersIcon_;
    std::unique_ptr<Gdiplus::Bitmap> smartFileTransferIcon_;
    std::unique_ptr<Gdiplus::Bitmap> macroRecorderIcon_;
    std::unique_ptr<Gdiplus::Bitmap> videoCompressorIcon_;
    std::unique_ptr<Gdiplus::Bitmap> mediaEditorIcon_;
    std::unique_ptr<Gdiplus::Bitmap> allToolsIcon_;
    std::unique_ptr<Gdiplus::Bitmap> settingsIcon_;
    std::unique_ptr<Gdiplus::Bitmap> customImageIcon_;
    std::unique_ptr<Gdiplus::Bitmap> autoClickerIconTinted_;
    std::unique_ptr<Gdiplus::Bitmap> fileConverterIconTinted_;
    std::unique_ptr<Gdiplus::Bitmap> mediaDownloaderIconTinted_;
    std::unique_ptr<Gdiplus::Bitmap> remindersIconTinted_;
    std::unique_ptr<Gdiplus::Bitmap> smartFileTransferIconTinted_;
    std::unique_ptr<Gdiplus::Bitmap> macroRecorderIconTinted_;
    std::unique_ptr<Gdiplus::Bitmap> videoCompressorIconTinted_;
    std::unique_ptr<Gdiplus::Bitmap> mediaEditorIconTinted_;
    std::unique_ptr<Gdiplus::Bitmap> allToolsIconTinted_;
    std::unique_ptr<Gdiplus::Bitmap> settingsIconTinted_;
    std::unique_ptr<Gdiplus::Bitmap> customImageIconTinted_;
    HHOOK keyboardHook_ = nullptr;
    HHOOK mouseHook_ = nullptr;
    HANDLE singleInstanceMutex_ = nullptr;
    Page currentPage_ = Page::Favorites;
    ToolKind currentTool_ = ToolKind::None;
    std::vector<ToolDefinition> tools_;
    std::vector<const ToolDefinition*> visibleToolsCache_;
    AppSettings appSettings_;
    AutoClickerState autoClicker_;
    FileConversionService fileConversionService_;
    MediaDownloadService mediaDownloadService_;
    VideoCompressionService videoCompressionService_;
    AnimeTrackerService animeTrackerService_;
    ShellIntegrationService shellIntegrationService_;
    ReminderService reminderService_;
    SmartFileTransferService smartFileTransferService_;
    MacroStorageService macroStorageService_;
    MacroRecorderService macroRecorderService_;
    MacroPlaybackService macroPlaybackService_;
    MacroHotkeyService macroHotkeyService_;
    std::vector<ConversionJob> conversionJobs_;
    MediaEditorPage mediaEditorPage_;
    ConversionOptions conversionOptions_;
    MediaDownloadOptions mediaDownloadOptions_;
    MediaDownloadJob mediaDownloadJob_;
    ExternalToolStatus mediaExternalTools_;
    ExternalToolStatus videoCompressionExternalTools_;
    VideoAnalysis videoAnalysis_;
    VideoCompressionOptions videoCompressionOptions_;
    VideoCompressionPlan videoCompressionPlan_;
    VideoCompressionProgress videoCompressionProgress_;
    VideoCompressionResult videoCompressionResult_;
    UpdateChecker updateChecker_;
    AnimeWatchList animeWatchList_;
    AnimeSearchResponse animeSearchResponse_;
    std::vector<AnimeSearchResult> animeSearchResults_;
    AnimeImportResult pendingAnimeAniListImport_;
    AnimeImportResult pendingAnimeMyAnimeListImport_;
    ReminderList reminderList_;
    ReminderAlert reminderAlert_;
    SmartTransferTab smartTransferTab_ = SmartTransferTab::Send;
    MacroRecorderTab macroRecorderTab_ = MacroRecorderTab::Record;
    SmartTransferSendOptions smartTransferOptions_;
    SmartTransferHostSnapshot smartTransferHostSnapshot_;
    SmartTransferManifest smartTransferReceiveManifest_;
    SmartTransferInvite smartTransferReceiveInvite_;
    SmartTransferDownloadProgress smartTransferDownloadProgress_;
    SmartTransferWebRtcSnapshot smartTransferWebRtcSnapshot_;
    MacroRecorderSnapshot macroRecorderSnapshot_;
    MacroPlaybackSnapshot macroPlaybackSnapshot_;
    std::vector<SmartTransferFile> smartTransferFiles_;
    std::vector<MacroDefinition> macros_;
    std::filesystem::path smartTransferSaveFolder_;
    MacroDefinition pendingRecordedMacro_;
    std::filesystem::path pendingEditPath_;
    std::wstring smartTransferSenderPairingCode_;
    std::wstring smartTransferReceiverResponseCode_;
    std::wstring macroStatusMessage_;
    std::wstring animeStatusMessage_;
    std::wstring pendingAnimeImportUserName_;
    std::wstring pendingAnimeAniListMessage_;
    std::wstring pendingAnimeMyAnimeListMessage_;
    std::wstring reminderStatusMessage_;
    std::wstring smartTransferStatusMessage_;
    std::wstring smartTransferReceiveStatusMessage_;
    std::wstring smartTransferReceiveWebRtcMessage_;
    std::wstring lastReminderNotificationKey_;
    std::map<std::wstring, AnimeBitmapCacheEntry> animeCoverCache_;
    std::set<std::wstring> animeCoverCacheMisses_;
    std::map<std::wstring, AnimeSearchCoverCacheEntry> animeSearchCoverCache_;
    std::map<int, AnimeSearchResult> animeDetailsCache_;
    std::set<int> animeDetailsRequested_;
    std::uintmax_t animeCoverCacheBytes_ = 0;
    std::uint64_t animeCoverCacheUseCounter_ = 0;
    std::vector<ImageFormat> supportedOutputFormats_;
    std::uint64_t animeSearchCoverCacheUseCounter_ = 0;
    int selectedConversionJob_ = -1;
    int selectedAnimeSearchIndex_ = -1;
    std::wstring fileConverterSummary_;
    bool fileConverterConverting_ = false;
    std::thread conversionThread_;
    std::atomic_bool conversionCancelRequested_ = false;
    std::thread mediaThread_;
    std::atomic_bool mediaCancelRequested_ = false;
    std::thread videoCompressionThread_;
    std::atomic_bool videoCompressionCancelRequested_ = false;
    std::thread updateThread_;
    std::thread animeThread_;
    std::thread smartTransferThread_;
    std::thread autoClickThread_;
    std::mutex autoClickMutex_;
    std::condition_variable autoClickCondition_;
    std::atomic_bool autoClickThreadStop_ = true;
    std::atomic<int> autoClickerCps_ = 8;
    std::atomic<int> autoClickerOutputKind_ = static_cast<int>(OutputInputKind::MouseButton);
    std::atomic<unsigned int> autoClickerOutputKey_ = VK_SPACE;
    std::atomic<int> autoClickerOutputButton_ = static_cast<int>(OutputMouseButton::Left);
    std::atomic<int> autoClickerAlternateOutputKind_ = static_cast<int>(OutputInputKind::MouseButton);
    std::atomic<unsigned int> autoClickerAlternateOutputKey_ = VK_RETURN;
    std::atomic<int> autoClickerAlternateOutputButton_ = static_cast<int>(OutputMouseButton::Right);
    std::atomic_bool autoClickerAlternateOutputEnabled_ = false;
    std::atomic_bool autoClickerUseAlternateNext_ = false;
    bool autoClickerActivationHeld_ = false;
    std::atomic_bool smartTransferCancelRequested_ = false;
    bool mediaAnalyzing_ = false;
    bool mediaMusicAnalyzing_ = false;
    bool mediaDownloading_ = false;
    bool videoCompressorAnalyzing_ = false;
    bool videoCompressorCompressing_ = false;
    bool videoCompressorAdvancedOpen_ = false;
    bool videoCompressorTargetInGb_ = false;
    std::wstring videoCompressorStatusText_;
    std::wstring mediaStatusText_;
    UpdateCheckResult updateResult_;
    bool updateChecking_ = false;
    bool updateInstalling_ = false;
    bool updateCheckSilent_ = false;
    bool updateNotificationVisible_ = false;
    std::wstring allToolsSearchText_;
    bool allToolsSearchPlaceholderActive_ = false;
    bool animeSearchPlaceholderActive_ = false;
    bool smartTransferConnecting_ = false;
    bool smartTransferDownloading_ = false;
    bool smartTransferHosting_ = false;
    bool smartTransferWebRtcFallbackOffered_ = false;
    bool smartTransferWebRtcDependencyMissing_ = false;
    bool smartTransferWebRtcBusy_ = false;
    bool smartTransferWebRtcReceiverActive_ = false;
    bool macroRecorderArmed_ = false;
    bool macroHasPendingRecording_ = false;
    bool awaitingMacroRecordHotkey_ = false;
    bool awaitingMacroPlayHotkey_ = false;
    bool awaitingMacroStopHotkey_ = false;
    bool animeSearching_ = false;
    bool animeRefreshing_ = false;
    bool animeImporting_ = false;
    bool animeImportSourceChoiceOpen_ = false;
    bool animeImportPanelOpen_ = false;
    bool animeSearchDropdownOpen_ = false;
    bool animeSearchPending_ = false;
    bool animeSearchHasRun_ = false;
    bool animeCanLoadMore_ = false;
    bool animeAppendSearch_ = false;
    bool hasUpdateResult_ = false;
    std::wstring updateInstallStatus_;
    int animeCurrentPage_ = 1;
    int selectedAnimeIndex_ = -1;
    int animeSearchKeyboardIndex_ = -1;
    int selectedMacroIndex_ = -1;
    MacroRecordingMode macroRecordingMode_;
    MacroHotkey macroRecordHotkey_ { 0, VK_F8 };
    MacroHotkey macroPlayHotkey_ { 0, VK_F9 };
    MacroHotkey macroStopHotkey_ { 0, VK_ESCAPE };
    MacroPlaybackOptions macroPlaybackOptions_;
    AnimeUserStatus animeFilter_ = AnimeUserStatus::Watching;
    bool animeFilterAll_ = true;
    AnimeTrackerTab animeTrackerTab_ = AnimeTrackerTab::Watchlist;
    AnimeDetailTab animeDetailTab_ = AnimeDetailTab::Overview;
    std::wstring animeSearchRequestText_;
    ReminderFilter reminderFilter_ = ReminderFilter::All;
    ReminderSort reminderSort_ = ReminderSort::SoonestFirst;
    ReminderPriority reminderFormPriority_ = ReminderPriority::Normal;
    ReminderRepeatType reminderFormRepeat_ = ReminderRepeatType::None;
    int reminderFormAlertMinutes_ = 15;
    std::vector<int> reminderFormAlertMinutesList_ { 15 };
    bool reminderFormPm_ = false;
    int selectedReminderIndex_ = -1;
    int pendingReminderSnoozeIndex_ = -1;
    bool reminderFormAllDay_ = false;
    bool reminderFormBirthday_ = false;
    bool editingReminder_ = false;
    bool reminderMoreOptionsOpen_ = false;
    bool reminderNotificationsArmed_ = false;
    bool reminderCalendarOpen_ = false;
    int reminderCalendarYear_ = 2026;
    int reminderCalendarMonth_ = 1;
    SettingsSection settingsSection_ = SettingsSection::General;
    AppearanceTab appearanceTab_ = AppearanceTab::Themes;
    bool suppressAnimeNotesChange_ = false;
    std::wstring animeNotesStatusText_;
    SIZE savedWindowSize_ { 1060, 680 };
    bool savedWindowMaximized_ = false;

    RECT clientRect_ {};
    RECT headerRect_ {};
    RECT headerNavGroupRect_ {};
    RECT favoritesNavRect_ {};
    RECT allToolsNavRect_ {};
    RECT settingsNavRect_ {};
    RECT dateTimeRect_ {};
    RECT reminderBannerRect_ {};
    RECT reminderBannerSnoozeButtonRect_ {};
    RECT reminderBannerCompleteButtonRect_ {};
    RECT reminderBannerViewButtonRect_ {};
    RECT reminderBannerCloseButtonRect_ {};
    RECT updateBannerUpdateButtonRect_ {};
    RECT updateBannerCloseButtonRect_ {};
    RECT contentRect_ {};
    RECT backButtonRect_ {};
    RECT allToolsSearchRect_ {};
    RECT allToolsSearchEditRect_ {};
    RECT startStopButtonRect_ {};
    RECT activationKeyButtonRect_ {};
    RECT outputButtonButtonRect_ {};
    RECT alternateOutputToggleRect_ {};
    RECT toggleModeToggleRect_ {};
    RECT alternateOutputButtonRect_ {};
    RECT speedSliderTrackRect_ {};
    RECT speedSliderThumbRect_ {};
    RECT converterDropZoneRect_ {};
    RECT converterBrowseButtonRect_ {};
    RECT converterFormatButtonRect_ {};
    RECT converterAdvancedToggleRect_ {};
    RECT converterConflictButtonRect_ {};
    RECT converterJpgBackgroundButtonRect_ {};
    RECT converterQualityTrackRect_ {};
    RECT converterQualityThumbRect_ {};
    RECT converterWebpLosslessRect_ {};
    RECT converterConvertButtonRect_ {};
    RECT converterCancelButtonRect_ {};
    RECT converterClearButtonRect_ {};
    RECT converterRemoveFailedButtonRect_ {};
    RECT converterProgressRect_ {};
    RECT converterQueueRect_ {};
    RECT mediaUrlEditRect_ {};
    RECT mediaAnalyzeButtonRect_ {};
    RECT mediaClearButtonRect_ {};
    RECT mediaMetadataRect_ {};
    RECT mediaMusicPanelRect_ {};
    RECT mediaMusicAnalyzeButtonRect_ {};
    RECT mediaFormatButtonRect_ {};
    RECT mediaQualityButtonRect_ {};
    RECT mediaOutputFolderRect_ {};
    RECT mediaBrowseButtonRect_ {};
    RECT mediaFileNameEditRect_ {};
    RECT mediaDownloadButtonRect_ {};
    RECT mediaCancelButtonRect_ {};
    RECT mediaProgressRect_ {};
    RECT mediaOpenFileButtonRect_ {};
    RECT mediaOpenFolderButtonRect_ {};
    RECT mediaCopyPathButtonRect_ {};
    RECT videoCompressorDropZoneRect_ {};
    RECT videoCompressorBrowseButtonRect_ {};
    RECT videoCompressorClearButtonRect_ {};
    RECT videoCompressorInfoRect_ {};
    RECT videoCompressorTargetPanelRect_ {};
    RECT videoCompressorTargetEditRect_ {};
    RECT videoCompressorUnitButtonRect_ {};
    RECT videoCompressorModeButtonRect_ {};
    RECT videoCompressorOutputFolderRect_ {};
    RECT videoCompressorBrowseOutputButtonRect_ {};
    RECT videoCompressorCompressButtonRect_ {};
    RECT videoCompressorCancelButtonRect_ {};
    RECT videoCompressorQuickPresetButtonRect_ {};
    RECT videoCompressorCpuButtonRect_ {};
    RECT videoCompressorGpuButtonRect_ {};
    RECT videoCompressorAdvancedToggleRect_ {};
    RECT videoCompressorAdvancedPanelRect_ {};
    RECT videoCompressorResolutionButtonRect_ {};
    RECT videoCompressorFpsButtonRect_ {};
    RECT videoCompressorAudioButtonRect_ {};
    RECT videoCompressorPresetButtonRect_ {};
    RECT videoCompressorEncoderButtonRect_ {};
    RECT videoCompressorConflictButtonRect_ {};
    RECT videoCompressorVerifyToggleRect_ {};
    RECT videoCompressorRetryToggleRect_ {};
    RECT videoCompressorMetadataToggleRect_ {};
    RECT videoCompressorEstimateRect_ {};
    RECT videoCompressorProgressRect_ {};
    RECT videoCompressorOpenFileButtonRect_ {};
    RECT videoCompressorOpenFolderButtonRect_ {};
    RECT videoCompressorAnotherButtonRect_ {};
    RECT smartTransferSendTabRect_ {};
    RECT smartTransferReceiveTabRect_ {};
    RECT smartTransferDropZoneRect_ {};
    RECT smartTransferBrowseButtonRect_ {};
    RECT smartTransferClearButtonRect_ {};
    RECT smartTransferCreateButtonRect_ {};
    RECT smartTransferStopButtonRect_ {};
    RECT smartTransferCopyCodeButtonRect_ {};
    RECT smartTransferExpirationButtonRect_ {};
    RECT smartTransferApprovalToggleRect_ {};
    RECT smartTransferStopAfterToggleRect_ {};
    RECT smartTransferMultiReceiverToggleRect_ {};
    RECT smartTransferDirectHostToggleRect_ {};
    RECT smartTransferNameEditRect_ {};
    RECT smartTransferCodeBoxRect_ {};
    RECT smartTransferAllowButtonRect_ {};
    RECT smartTransferDenyButtonRect_ {};
    RECT smartTransferSendFileListRect_ {};
    RECT smartTransferWebRtcSendPanelRect_ {};
    RECT smartTransferCopyPairingButtonRect_ {};
    RECT smartTransferReceiverResponseEditRect_ {};
    RECT smartTransferApplyResponseButtonRect_ {};
    RECT smartTransferReceiveCodeEditRect_ {};
    RECT smartTransferConnectButtonRect_ {};
    RECT smartTransferClearCodeButtonRect_ {};
    RECT smartTransferWebRtcReceivePanelRect_ {};
    RECT smartTransferSenderPairingEditRect_ {};
    RECT smartTransferGenerateResponseButtonRect_ {};
    RECT smartTransferCopyResponseButtonRect_ {};
    RECT smartTransferManifestRect_ {};
    RECT smartTransferSaveFolderRect_ {};
    RECT smartTransferBrowseSaveButtonRect_ {};
    RECT smartTransferDownloadButtonRect_ {};
    RECT smartTransferCancelButtonRect_ {};
    RECT smartTransferOpenFolderButtonRect_ {};
    RECT smartTransferProgressRect_ {};
    RECT macroRecordTabRect_ {};
    RECT macroPlaybackTabRect_ {};
    RECT macroLibraryTabRect_ {};
    RECT macroStatusRect_ {};
    RECT macroListPanelRect_ {};
    RECT macroNewButtonRect_ {};
    RECT macroImportButtonRect_ {};
    RECT macroExportButtonRect_ {};
    RECT macroDeleteButtonRect_ {};
    RECT macroDuplicateButtonRect_ {};
    RECT macroOverlayButtonRect_ {};
    RECT macroDetailsPanelRect_ {};
    RECT macroNameEditRect_ {};
    RECT macroSaveButtonRect_ {};
    RECT macroRecordPanelRect_ {};
    RECT macroRecordHotkeyButtonRect_ {};
    RECT macroPlayHotkeyButtonRect_ {};
    RECT macroStopHotkeyButtonRect_ {};
    RECT macroArmButtonRect_ {};
    RECT macroStartRecordButtonRect_ {};
    RECT macroStopRecordButtonRect_ {};
    RECT macroCancelRecordButtonRect_ {};
    RECT macroPlaybackPanelRect_ {};
    RECT macroPlayOnceButtonRect_ {};
    RECT macroLoopButtonRect_ {};
    RECT macroLoopUntilStoppedToggleRect_ {};
    RECT macroStopPlaybackButtonRect_ {};
    RECT macroPlaybackSpeedButtonRect_ {};
    RECT macroPlaybackBackendButtonRect_ {};
    RECT macroStartDelayButtonRect_ {};
    RECT macroRequireTargetToggleRect_ {};
    RECT macroProgressRect_ {};
    RECT macroSettingsPanelRect_ {};
    RECT macroMouseModeButtonRect_ {};
    RECT macroCaptureRateButtonRect_ {};
    RECT macroEventPreviewRect_ {};
    RECT macroOverlayArmButtonRect_ {};
    RECT macroOverlayStartButtonRect_ {};
    RECT macroOverlayStopRecordButtonRect_ {};
    RECT macroOverlayPlayButtonRect_ {};
    RECT macroOverlayStopPlaybackButtonRect_ {};
    RECT macroOverlayCloseButtonRect_ {};
    RECT settingsCheckUpdatesButtonRect_ {};
    RECT settingsDownloadUpdateButtonRect_ {};
    RECT settingsGeneralTabRect_ {};
    RECT settingsSmartTransferTabRect_ {};
    RECT settingsMacroRecorderTabRect_ {};
    RECT settingsWindowsIntegrationTabRect_ {};
    RECT settingsAppearanceTabRect_ {};
    RECT settingsUpdatesTabRect_ {};
    RECT settingsAboutTabRect_ {};
    RECT settingsDefaultFolderRect_ {};
    RECT settingsBrowseDefaultFolderButtonRect_ {};
    RECT settingsStartPageButtonRect_ {};
    RECT settingsMinimizeToTrayToggleRect_ {};
    RECT settingsStartWithWindowsToggleRect_ {};
    RECT settingsWebRtcFallbackToggleRect_ {};
    RECT settingsExplorerContextToggleRect_ {};
    RECT settingsWebRtcDiagnosticsToggleRect_ {};
    RECT settingsWebRtcStunServersRect_ {};
    RECT settingsMacroRecordHotkeyButtonRect_ {};
    RECT settingsMacroPlayHotkeyButtonRect_ {};
    RECT settingsMacroStopHotkeyButtonRect_ {};
    RECT settingsMacroMouseModeButtonRect_ {};
    RECT settingsMacroCaptureRateButtonRect_ {};
    RECT settingsMacroPlaybackSpeedButtonRect_ {};
    RECT settingsMacroStartDelayButtonRect_ {};
    RECT settingsMacroLoopToggleRect_ {};
    RECT settingsMacroTargetToggleRect_ {};
    RECT settingsClockFormatButtonRect_ {};
    RECT settingsClockPreviewRect_ {};
    RECT settingsThemeButtonRect_ {};
    std::array<RECT, 6> settingsThemePresetRects_ {};
    RECT settingsUseDefaultThemeButtonRect_ {};
    RECT settingsBackgroundTypeButtonRect_ {};
    std::array<RECT, 3> settingsBackgroundModeRects_ {};
    RECT settingsBackgroundEnabledToggleRect_ {};
    RECT settingsBackgroundChooseButtonRect_ {};
    RECT settingsBackgroundRemoveButtonRect_ {};
    RECT settingsAppearanceThemeSectionRect_ {};
    std::array<RECT, 3> settingsAppearancePageTabRects_ {};
    RECT settingsAppearanceBackgroundSectionRect_ {};
    RECT settingsAppearanceEditorRect_ {};
    RECT settingsAppearanceInterfaceSectionRect_ {};
    RECT settingsAppearanceMotionSectionRect_ {};
    RECT settingsAppearancePreviewRect_ {};
    RECT settingsAppearancePreviewCanvasRect_ {};
    RECT settingsAppearancePreviewFitButtonRect_ {};
    RECT settingsAppearancePreviewCenterButtonRect_ {};
    RECT settingsAppearancePreviewResetButtonRect_ {};
    RECT settingsAppearancePreviewRotateLeftButtonRect_ {};
    RECT settingsAppearancePreviewRotateRightButtonRect_ {};
    RECT settingsAppearanceImageZoomTrackRect_ {};
    RECT settingsAppearanceImageZoomThumbRect_ {};
    RECT settingsAppearanceImageRotationTrackRect_ {};
    RECT settingsAppearanceImageRotationThumbRect_ {};
    RECT settingsBackgroundPresetButtonRect_ {};
    RECT settingsBackgroundOpacityTrackRect_ {};
    RECT settingsBackgroundOpacityThumbRect_ {};
    RECT settingsBackgroundBlurTrackRect_ {};
    RECT settingsBackgroundBlurThumbRect_ {};
    RECT settingsBackgroundDarkenTrackRect_ {};
    RECT settingsBackgroundDarkenThumbRect_ {};
    RECT settingsBackgroundTintTrackRect_ {};
    RECT settingsBackgroundTintThumbRect_ {};
    RECT settingsBackgroundBlendToggleRect_ {};
    RECT settingsBackgroundReadabilityToggleRect_ {};
    RECT settingsBackgroundScaleButtonRect_ {};
    RECT settingsBackgroundPositionButtonRect_ {};
    RECT settingsBackgroundVignetteButtonRect_ {};
    RECT settingsGradientColor1ButtonRect_ {};
    RECT settingsGradientColor2ButtonRect_ {};
    RECT settingsGradientDirectionButtonRect_ {};
    RECT settingsGradientIntensityTrackRect_ {};
    RECT settingsGradientIntensityThumbRect_ {};
    RECT settingsGradientBlendToggleRect_ {};
    RECT settingsBackgroundOffsetXTrackRect_ {};
    RECT settingsBackgroundOffsetXThumbRect_ {};
    RECT settingsBackgroundOffsetYTrackRect_ {};
    RECT settingsBackgroundOffsetYThumbRect_ {};
    std::array<RECT, 3> settingsUiSurfaceStyleRects_ {};
    RECT settingsUiSurfaceOpacityTrackRect_ {};
    RECT settingsUiSurfaceOpacityThumbRect_ {};
    RECT settingsBorderEdgeGlowToggleRect_ {};
    RECT settingsSmoothScrollingToggleRect_ {};
    RECT settingsResetBackgroundButtonRect_ {};
    RECT settingsResetAppearanceButtonRect_ {};
    RECT settingsGithubButtonRect_ {};
    RECT settingsReportIssueButtonRect_ {};
    RECT animeSearchTabRect_ {};
    RECT animeListTabRect_ {};
    RECT animeAiringTabRect_ {};
    RECT animeFavoritesTabRect_ {};
    RECT animeSearchEditRect_ {};
    RECT animeSearchButtonRect_ {};
    RECT animeLoadMoreButtonRect_ {};
    RECT animeSearchDetailBackButtonRect_ {};
    RECT animeSearchDetailAddButtonRect_ {};
    RECT animeSearchDetailReminderButtonRect_ {};
    RECT animeSearchDetailOpenButtonRect_ {};
    RECT animeResultsRect_ {};
    RECT animeSearchDropdownRect_ {};
    RECT animeListRect_ {};
    RECT animeUpcomingRect_ {};
    RECT animeSequelsRect_ {};
    RECT animeRefreshAllButtonRect_ {};
    RECT animeFilterButtonRect_ {};
    RECT animeImportEditRect_ {};
    RECT animeImportButtonRect_ {};
    RECT animeImportPanelRect_ {};
    RECT animeImportSubmitButtonRect_ {};
    RECT animeImportCancelButtonRect_ {};
    RECT animeImportChoicePanelRect_ {};
    RECT animeImportChoiceAniListButtonRect_ {};
    RECT animeImportChoiceMyAnimeListButtonRect_ {};
    RECT animeImportChoiceCancelButtonRect_ {};
    RECT animeStatusButtonRect_ {};
    RECT animeEpisodeMinusButtonRect_ {};
    RECT animeEpisodePlusButtonRect_ {};
    RECT animeFavoriteButtonRect_ {};
    RECT animeReminderButtonRect_ {};
    RECT animeRefreshSelectedButtonRect_ {};
    RECT animeRemoveButtonRect_ {};
    std::array<RECT, 4> animeDetailTabRects_ {};
    RECT animeSaveNotesButtonRect_ {};
    RECT animeNotesEditRect_ {};
    RECT reminderNewButtonRect_ {};
    RECT reminderQuickPanelRect_ {};
    RECT reminderTitleEditRect_ {};
    RECT reminderDateEditRect_ {};
    RECT reminderDatePickerButtonRect_ {};
    RECT reminderTimeEditRect_ {};
    RECT reminderTimeAmPmButtonRect_ {};
    RECT reminderPresetButtonRect_ {};
    RECT reminderAddButtonRect_ {};
    RECT reminderCategoryEditRect_ {};
    RECT reminderPriorityButtonRect_ {};
    RECT reminderRepeatButtonRect_ {};
    RECT reminderAlertButtonRect_ {};
    RECT reminderMoreOptionsToggleRect_ {};
    RECT reminderAllDayToggleRect_ {};
    RECT reminderBirthdayToggleRect_ {};
    RECT reminderNotesEditRect_ {};
    RECT reminderListRect_ {};
    RECT reminderSearchEditRect_ {};
    RECT reminderFilterButtonRect_ {};
    RECT reminderSortButtonRect_ {};
    RECT reminderCalendarRect_ {};
    RECT reminderCalendarPrevButtonRect_ {};
    RECT reminderCalendarNextButtonRect_ {};
    RECT scrollBarTrackRect_ {};
    RECT scrollBarThumbRect_ {};
    RECT hoveredButtonRect_ {};
    RECT pressedButtonRect_ {};
    bool hasHoveredButton_ = false;
    bool hasPressedButton_ = false;
    DropdownKind activeDropdown_ = DropdownKind::None;
    RECT dropdownRect_ {};
    RECT dropdownAnchorRect_ {};
    std::vector<std::wstring> dropdownLabels_;
    std::vector<int> dropdownValues_;
    std::vector<bool> dropdownEnabled_;
    int dropdownSelectedValue_ = 0;
    int hoverDropdownIndex_ = -1;
    HWND mediaUrlEdit_ = nullptr;
    HWND mediaFileNameEdit_ = nullptr;
    HWND videoCompressorTargetEdit_ = nullptr;
    HWND smartTransferNameEdit_ = nullptr;
    HWND smartTransferCodeEdit_ = nullptr;
    HWND smartTransferReceiverResponseEdit_ = nullptr;
    HWND smartTransferSenderPairingEdit_ = nullptr;
    HWND allToolsSearchEdit_ = nullptr;
    HWND animeSearchEdit_ = nullptr;
    HWND animeImportEdit_ = nullptr;
    HWND animeNotesEdit_ = nullptr;
    HWND reminderTitleEdit_ = nullptr;
    HWND reminderDateEdit_ = nullptr;
    HWND reminderTimeEdit_ = nullptr;
    HWND reminderCategoryEdit_ = nullptr;
    HWND reminderNotesEdit_ = nullptr;
    HWND reminderSearchEdit_ = nullptr;
    HWND macroNameEdit_ = nullptr;
    HWND macroOverlayWindow_ = nullptr;
    HBRUSH editBackgroundBrush_ = nullptr;
    HDC backBufferDc_ = nullptr;
    HBITMAP backBufferBitmap_ = nullptr;
    HBITMAP backBufferPreviousBitmap_ = nullptr;
    SIZE backBufferSize_ {};
    std::vector<ToolCardCacheEntry> toolCardCache_;
    int hoverNavIndex_ = -1;
    int hoverToolIndex_ = -1;
    int hoverFavoriteIndex_ = -1;
    int hoverAnimeSearchResultIndex_ = -1;
    POINT navHoverPoint_ { -32768, -32768 };
    POINT navGlowPaintPoint_ { -32768, -32768 };
    POINT toolHoverPoint_ { -32768, -32768 };
    POINT toolGlowPaintPoint_ { -32768, -32768 };
    ULONGLONG lastToolGlowRepaintTick_ = 0;
    bool toolGlowRedrawPending_ = false;
    std::map<UINT_PTR, rex::ui::SwitchAnimationState> toggleSwitchAnimations_;
    std::wstring lastClockLabel_;
    HANDLE clockTimerQueueTimer_ = nullptr;
    std::atomic_bool clockTickPosted_ { false };
    bool scrollBarHovered_ = false;
    bool speedSliderDragging_ = false;
    bool converterQualityDragging_ = false;
    bool scrollBarDragging_ = false;
    AppearanceSlider appearanceSliderDragging_ = AppearanceSlider::None;
    bool appearancePreviewDragging_ = false;
    bool appearancePreviewRedrawPending_ = false;
    POINT appearancePreviewDragStart_ {};
    int appearancePreviewDragStartOffsetX_ = 0;
    int appearancePreviewDragStartOffsetY_ = 0;
    bool liveResize_ = false;
    bool resizeLayoutPending_ = false;
    bool minimizedToTray_ = false;
    bool trayIconVisible_ = false;
    bool trayRestoreMaximized_ = false;
    bool startMinimizedToTray_ = false;
    bool automationInputsEnabled_ = false;
    bool forceClose_ = false;
    int scrollBarDragOffsetY_ = 0;
    int scrollOffsetY_ = 0;
    int allToolsScrollOffsetY_ = 0;
    int smoothScrollVisualOffsetY_ = 0;
    int smoothScrollTargetOffsetY_ = 0;
    int scrollWheelDeltaRemainder_ = 0;
    int appearanceZoomWheelDeltaRemainder_ = 0;
    bool smoothScrollActive_ = false;
    int animeSearchResultsScrollOffset_ = 0;
    int maxScrollOffsetY_ = 0;
    int scrollContentHeight_ = 0;
    std::unique_ptr<Gdiplus::Bitmap> appearanceSourceBitmap_;
    std::unique_ptr<Gdiplus::Bitmap> appearanceBackgroundCache_;
    std::unique_ptr<Gdiplus::Bitmap> appearancePreviewBackgroundCache_;
    SIZE appearancePreviewBackgroundCacheSize_ {};
    bool appearancePreviewBackgroundCacheDirty_ = true;
    bool appearancePreviewBackgroundCachePositioning_ = false;
    bool appearancePreviewBackgroundCacheDraft_ = false;
    HDC appearanceBackgroundCacheDc_ = nullptr;
    HBITMAP appearanceBackgroundCacheBitmap_ = nullptr;
    HBITMAP appearanceBackgroundCachePreviousBitmap_ = nullptr;
    SIZE appearanceBackgroundCacheSize_ {};
    bool appearanceBackgroundCacheDirty_ = true;
    bool appearanceSourceReloadPending_ = true;
    std::wstring appearanceStatusMessage_;
    bool fileConverterAdvancedOpen_ = false;
    bool mouseLeaveTracking_ = false;
    bool testButtonHeld_ = false;
    bool awaitingActivationKey_ = false;
    bool awaitingOutputButton_ = false;
    bool awaitingAlternateOutputButton_ = false;

    HFONT titleFont_ = nullptr;
    HFONT navFont_ = nullptr;
    HFONT headingFont_ = nullptr;
    HFONT bodyFont_ = nullptr;
    HFONT searchInputFont_ = nullptr;
    HFONT monospaceFont_ = nullptr;
    HFONT animeItemFont_ = nullptr;
    HFONT animeBodyFont_ = nullptr;
    int dpi_ = 96;
    int macroOverlayDpi_ = 96;
};
