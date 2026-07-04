#pragma once

#include "AnimeTrackerService.h"
#include "FileConversionService.h"
#include "MacroRecorderService.h"
#include "MediaDownloadService.h"
#include "ReminderService.h"
#include "SmartFileTransferService.h"
#include "UpdateChecker.h"
#include "VideoCompressionService.h"

#include <windows.h>
#include <gdiplus.h>

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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
    VideoCompressor
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
    SmartTransferExpiration,
    MacroMouseMode,
    MacroCaptureRate,
    MacroPlaybackSpeed,
    MacroPlaybackBackend,
    MacroLoopMode,
    MacroStartDelay,
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
    Search,
    Anime
};

enum class SmartTransferTab
{
    Send,
    Receive
};

enum class SettingsSection
{
    General,
    SmartTransfer,
    MacroRecorder,
    Appearance,
    Updates,
    About
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
    bool minimizeToTrayOnClose = false;
    bool startWithWindowsToTray = false;
    bool smartTransferWebRtcFallback = true;
    bool smartTransferWebRtcDiagnostics = false;
    std::wstring smartTransferStunServers = L"stun:stun.l.google.com:19302";
    long long updateNotificationDismissedUntil = 0;
};

class ToolkitApp
{
public:
    explicit ToolkitApp(HINSTANCE instance);

    static bool ActivateExistingInstanceIfRunning(bool restoreExisting = true);

    int Run(int showCommand);
    void SetStartMinimizedToTray(bool value);

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK MacroOverlayWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK KeyboardHookProc(int code, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK MouseHookProc(int code, WPARAM wParam, LPARAM lParam);
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
    void PaintResizePlaceholder(HDC hdc);
    void PaintContent(HDC hdc);
    void PaintNavItem(HDC hdc, const RECT& bounds, const wchar_t* label, bool selected);
    void PaintEmptyState(HDC hdc, const wchar_t* title, const wchar_t* subtitle);
    void PaintToolCards(HDC hdc, const std::vector<ToolDefinition>& tools);
    void PaintToolIcon(HDC hdc, ToolKind tool, const RECT& bounds);
    void PaintAniListIcon(HDC hdc, const RECT& bounds);
    void PaintReminderIcon(HDC hdc, const RECT& bounds);
    void PaintReminderCalendar(HDC hdc);
    void PaintFavoriteStar(HDC hdc, const RECT& bounds, bool favorite);
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
    void PaintMacroOverlay(HWND hwnd, HDC hdc);
    void PaintSettings(HDC hdc);
    void PaintReminderBanner(HDC hdc);
    void PaintUpdateBanner(HDC hdc);
    void PaintSettingsSectionTab(HDC hdc, const RECT& bounds, const wchar_t* label, SettingsSection section);
    void PaintProgressBar(HDC hdc, const RECT& bounds, double progress);
    void PaintButton(HDC hdc, const RECT& bounds, const wchar_t* label, bool primary, bool active = false, bool enabled = true);
    void PaintBackButton(HDC hdc, const RECT& bounds);
    void PaintDropdownButton(HDC hdc, const RECT& bounds, const wchar_t* label, bool enabled = true, bool down = true);
    void PaintChevron(HDC hdc, const RECT& bounds, bool down, COLORREF color);
    void PaintXIcon(HDC hdc, const RECT& bounds, COLORREF color);
    void PaintSlider(HDC hdc);
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
    void ApplyUpdateCheckResult(constγz¶‰ΛkΊwµη@€‰½½°Νµ…ΙΡQΙ…ΉΝ™•Ι!½ΝΡ¥Ή|€τ™…±Ν”μ(€€€‰½½°Νµ…ΙΡQΙ…ΉΝ™•Ι]•‰IΡ…±±‰…­=™™•Ι•‘|€τ™…±Ν”μ(€€€‰½½°Νµ…ΙΡQΙ…ΉΝ™•Ι]•‰IΡ•Α•Ή‘•Ήε5¥ΝΝ¥Ή|€τ™…±Ν”μ(€€€‰½½°Νµ…ΙΡQΙ…ΉΝ™•Ι]•‰IΡ	ΥΝε|€τ™…±Ν”μ(€€€‰½½°Νµ…ΙΡQΙ…ΉΝ™•Ι]•‰IΡI••¥Ω•ΙΡ¥Ω•|€τ™…±Ν”μ(€€€‰½½°µ…Ι½I•½Ι‘•ΙΙµ•‘|€τ™…±Ν”μ(€€€‰½½°µ…Ι½!…ΝA•Ή‘¥ΉI•½Ι‘¥Ή|€τ™…±Ν”μ(€€€‰½½°…έ…¥Ρ¥Ή5…Ι½I•½Ι‘!½Ρ­•ε|€τ™…±Ν”μ(€€€‰½½°…έ…¥Ρ¥Ή5…Ι½A±…ε!½Ρ­•ε|€τ™…±Ν”μ(€€€‰½½°…έ…¥Ρ¥Ή5…Ι½MΡ½Α!½Ρ­•ε|€τ™…±Ν”μ(€€€‰½½°…Ή¥µ•M•…Ι΅¥Ή|€τ™…±Ν”μ(€€€‰½½°…Ή¥µ•I•™Ι•Ν΅¥Ή|€τ™…±Ν”μ(€€€‰½½°…Ή¥µ•%µΑ½ΙΡ¥Ή|€τ™…±Ν”μ(€€€‰½½°…Ή¥µ•%µΑ½ΙΡM½ΥΙ•΅½¥•=Α•Ή|€τ™…±Ν”μ(€€€‰½½°…Ή¥µ•M•…Ι΅!…ΝIΥΉ|€τ™…±Ν”μ(€€€‰½½°…Ή¥µ•…Ή1½…‘5½Ι•|€τ™…±Ν”μ(€€€‰½½°…Ή¥µ•ΑΑ•Ή‘M•…Ι΅|€τ™…±Ν”μ(€€€‰½½°΅…ΝUΑ‘…Ρ•I•ΝΥ±Ρ|€τ™…±Ν”μ(€€€ΝΡθιέΝΡΙ¥ΉΥΑ‘…Ρ•%ΉΝΡ…±±MΡ…ΡΥΝ|μ(€€€¥ΉΠ…Ή¥µ•ΥΙΙ•ΉΡA…•|€τ€Δμ(€€€¥ΉΠΝ•±•Ρ•‘Ή¥µ•%Ή‘•α|€τ€΄Δμ(€€€¥ΉΠΝ•±•Ρ•‘5…Ι½%Ή‘•α|€τ€΄Δμ(€€€5…Ι½I•½Ι‘¥Ή5½‘”µ…Ι½I•½Ι‘¥Ή5½‘•|μ(€€€5…Ι½!½Ρ­•δµ…Ι½I•½Ι‘!½Ρ­•ε|μ€ΐ°Y-}ΰτμ(€€€5…Ι½!½Ρ­•δµ…Ι½A±…ε!½Ρ­•ε|μ€ΐ°Y-}δτμ(€€€5…Ι½!½Ρ­•δµ…Ι½MΡ½Α!½Ρ­•ε|μ€ΐ°Y-}MAτμ(€€€5…Ι½A±…ε‰…­=ΑΡ¥½ΉΜµ…Ι½A±…ε‰…­=ΑΡ¥½ΉΝ|μ(€€€Ή¥µ•UΝ•ΙMΡ…ΡΥΜ…Ή¥µ•¥±Ρ•Ι|€τΉ¥µ•UΝ•ΙMΡ…ΡΥΜθι]…Ρ΅¥Ήμ(€€€‰½½°…Ή¥µ•¥±Ρ•Ι±±|€τΡΙΥ”μ(€€€Ή¥µ•QΙ…­•ΙQ……Ή¥µ•QΙ…­•ΙQ…‰|€τΉ¥µ•QΙ…­•ΙQ…θιM•…Ι μ(€€€I•µ¥Ή‘•Ι¥±Ρ•ΘΙ•µ¥Ή‘•Ι¥±Ρ•Ι|€τI•µ¥Ή‘•Ι¥±Ρ•Θθι±°μ(€€€I•µ¥Ή‘•ΙM½ΙΠΙ•µ¥Ή‘•ΙM½ΙΡ|€τI•µ¥Ή‘•ΙM½ΙΠθιM½½Ή•ΝΡ¥ΙΝΠμ(€€€I•µ¥Ή‘•ΙAΙ¥½Ι¥ΡδΙ•µ¥Ή‘•Ι½ΙµAΙ¥½Ι¥Ρε|€τI•µ¥Ή‘•ΙAΙ¥½Ι¥Ρδθι9½Ιµ…°μ(€€€I•µ¥Ή‘•ΙI•Α•…ΡQεΑ”Ι•µ¥Ή‘•Ι½ΙµI•Α•…Ρ|€τI•µ¥Ή‘•ΙI•Α•…ΡQεΑ”θι9½Ή”μ(€€€¥ΉΠΙ•µ¥Ή‘•Ι½Ιµ±•ΙΡ5¥ΉΥΡ•Ν|€τ€ΔΤμ(€€€ΝΡθιΩ•Ρ½Θρ¥ΉΠψΙ•µ¥Ή‘•Ι½Ιµ±•ΙΡ5¥ΉΥΡ•Ν1¥ΝΡ|μ€ΔΤτμ(€€€‰½½°Ι•µ¥Ή‘•Ι½ΙµAµ|€τ™…±Ν”μ(€€€¥ΉΠΝ•±•Ρ•‘I•µ¥Ή‘•Ι%Ή‘•α|€τ€΄Δμ(€€€¥ΉΠΑ•Ή‘¥ΉI•µ¥Ή‘•ΙMΉ½½ι•%Ή‘•α|€τ€΄Δμ(€€€‰½½°Ι•µ¥Ή‘•Ι½Ιµ±±…ε|€τ™…±Ν”μ(€€€‰½½°Ι•µ¥Ή‘•Ι½Ιµ	¥ΙΡ΅‘…ε|€τ™…±Ν”μ(€€€‰½½°•‘¥Ρ¥ΉI•µ¥Ή‘•Ι|€τ™…±Ν”μ(€€€‰½½°Ι•µ¥Ή‘•Ι5½Ι•=ΑΡ¥½ΉΝ=Α•Ή|€τ™…±Ν”μ(€€€‰½½°Ι•µ¥Ή‘•Ι9½Ρ¥™¥…Ρ¥½ΉΝΙµ•‘|€τ™…±Ν”μ(€€€‰½½°Ι•µ¥Ή‘•Ι…±•Ή‘…Ι=Α•Ή|€τ™…±Ν”μ(€€€¥ΉΠΙ•µ¥Ή‘•Ι…±•Ή‘…Ιe•…Ι|€τ€ΘΐΘΨμ(€€€¥ΉΠΙ•µ¥Ή‘•Ι…±•Ή‘…Ι5½ΉΡ΅|€τ€Δμ(€€€M•ΡΡ¥ΉΝM•Ρ¥½ΈΝ•ΡΡ¥ΉΝM•Ρ¥½Ή|€τM•ΡΡ¥ΉΝM•Ρ¥½Έθι•Ή•Ι…°μ(€€€‰½½°ΝΥΑΑΙ•ΝΝΉ¥µ•9½Ρ•Ν΅…Ή•|€τ™…±Ν”μ(€€€ΝΡθιέΝΡΙ¥Ή…Ή¥µ•9½Ρ•ΝMΡ…ΡΥΝQ•αΡ|μ(€€€M%iΝ…Ω•‘]¥Ή‘½έM¥ι•|μ€ΔΐΨΐ°€Ψΰΐτμ(€€€‰½½°Ν…Ω•‘]¥Ή‘½έ5…α¥µ¥ι•‘|€τ™…±Ν”μ((€€€IP±¥•ΉΡI•Ρ|ντμ(€€€IP΅•…‘•ΙI•Ρ|ντμ(€€€IP™…Ω½Ι¥Ρ•Ν9…ΩI•Ρ|ντμ(€€€IP…±±Q½½±Ν9…ΩI•Ρ|ντμ(€€€IPΝ•ΡΡ¥ΉΝ9…ΩI•Ρ|ντμ(€€€IP‘…Ρ•Q¥µ•I•Ρ|ντμ(€€€IPΙ•µ¥Ή‘•Ι	…ΉΉ•ΙI•Ρ|ντμ(€€€IPΙ•µ¥Ή‘•Ι	…ΉΉ•ΙMΉ½½ι•	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΙ•µ¥Ή‘•Ι	…ΉΉ•Ι½µΑ±•Ρ•	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΙ•µ¥Ή‘•Ι	…ΉΉ•ΙY¥•έ	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΙ•µ¥Ή‘•Ι	…ΉΉ•Ι±½Ν•	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΥΑ‘…Ρ•	…ΉΉ•ΙUΑ‘…Ρ•	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΥΑ‘…Ρ•	…ΉΉ•Ι±½Ν•	ΥΡΡ½ΉI•Ρ|ντμ(€€€IP½ΉΡ•ΉΡI•Ρ|ντμ(€€€IP‰…­	ΥΡΡ½ΉI•Ρ|ντμ(€€€IP…±±Q½½±ΝM•…Ι΅I•Ρ|ντμ(€€€IP…±±Q½½±ΝM•…Ι΅‘¥ΡI•Ρ|ντμ(€€€IPΝΡ…ΙΡMΡ½Α	ΥΡΡ½ΉI•Ρ|ντμ(€€€IP…Ρ¥Ω…Ρ¥½Ή-•ε	ΥΡΡ½ΉI•Ρ|ντμ(€€€IP½ΥΡΑΥΡ	ΥΡΡ½Ή	ΥΡΡ½ΉI•Ρ|ντμ(€€€IP…±Ρ•ΙΉ…Ρ•=ΥΡΑΥΡQ½±•I•Ρ|ντμ(€€€IPΡ½±•5½‘•Q½±•I•Ρ|ντμ(€€€IP…±Ρ•ΙΉ…Ρ•=ΥΡΑΥΡ	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΝΑ••‘M±¥‘•ΙQΙ…­I•Ρ|ντμ(€€€IPΝΑ••‘M±¥‘•ΙQ΅Υµ‰I•Ρ|ντμ(€€€IP½ΉΩ•ΙΡ•ΙΙ½Αi½Ή•I•Ρ|ντμ(€€€IP½ΉΩ•ΙΡ•Ι	Ι½έΝ•	ΥΡΡ½ΉI•Ρ|ντμ(€€€IP½ΉΩ•ΙΡ•Ι½Ιµ…Ρ	ΥΡΡ½ΉI•Ρ|ντμ(€€€IP½ΉΩ•ΙΡ•Ι‘Ω…Ή•‘Q½±•I•Ρ|ντμ(€€€IP½ΉΩ•ΙΡ•Ι½Ή™±¥Ρ	ΥΡΡ½ΉI•Ρ|ντμ(€€€IP½ΉΩ•ΙΡ•Ι)Α	…­Ι½ΥΉ‘	ΥΡΡ½ΉI•Ρ|ντμ(€€€IP½ΉΩ•ΙΡ•ΙEΥ…±¥ΡεQΙ…­I•Ρ|ντμ(€€€IP½ΉΩ•ΙΡ•ΙEΥ…±¥ΡεQ΅Υµ‰I•Ρ|ντμ(€€€IP½ΉΩ•ΙΡ•Ι]•‰Α1½ΝΝ±•ΝΝI•Ρ|ντμ(€€€IP½ΉΩ•ΙΡ•Ι½ΉΩ•ΙΡ	ΥΡΡ½ΉI•Ρ|ντμ(€€€IP½ΉΩ•ΙΡ•Ι…Ή•±	ΥΡΡ½ΉI•Ρ|ντμ(€€€IP½ΉΩ•ΙΡ•Ι±•…Ι	ΥΡΡ½ΉI•Ρ|ντμ(€€€IP½ΉΩ•ΙΡ•ΙI•µ½Ω•…¥±•‘	ΥΡΡ½ΉI•Ρ|ντμ(€€€IP½ΉΩ•ΙΡ•ΙAΙ½Ι•ΝΝI•Ρ|ντμ(€€€IP½ΉΩ•ΙΡ•ΙEΥ•Υ•I•Ρ|ντμ(€€€IPµ•‘¥…UΙ±‘¥ΡI•Ρ|ντμ(€€€IPµ•‘¥…Ή…±ει•	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPµ•‘¥…±•…Ι	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPµ•‘¥…5•Ρ…‘…Ρ…I•Ρ|ντμ(€€€IPµ•‘¥…5ΥΝ¥A…Ή•±I•Ρ|ντμ(€€€IPµ•‘¥…5ΥΝ¥Ή…±ει•	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPµ•‘¥…½Ιµ…Ρ	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPµ•‘¥…EΥ…±¥Ρε	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPµ•‘¥…=ΥΡΑΥΡ½±‘•ΙI•Ρ|ντμ(€€€IPµ•‘¥…	Ι½έΝ•	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPµ•‘¥…¥±•9…µ•‘¥ΡI•Ρ|ντμ(€€€IPµ•‘¥…½έΉ±½…‘	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPµ•‘¥……Ή•±	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPµ•‘¥…AΙ½Ι•ΝΝI•Ρ|ντμ(€€€IPµ•‘¥…=Α•Ή¥±•	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPµ•‘¥…=Α•Ή½±‘•Ι	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPµ•‘¥…½ΑεA…Ρ΅	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΩ¥‘•½½µΑΙ•ΝΝ½ΙΙ½Αi½Ή•I•Ρ|ντμ(€€€IPΩ¥‘•½½µΑΙ•ΝΝ½Ι	Ι½έΝ•	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΩ¥‘•½½µΑΙ•ΝΝ½Ι±•…Ι	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΩ¥‘•½½µΑΙ•ΝΝ½Ι%Ή™½I•Ρ|ντμ(€€€IPΩ¥‘•½½µΑΙ•ΝΝ½ΙQ…Ι•ΡA…Ή•±I•Ρ|ντμ(€€€IPΩ¥‘•½½µΑΙ•ΝΝ½ΙQ…Ι•Ρ‘¥ΡI•Ρ|ντμ(€€€IPΩ¥‘•½½µΑΙ•ΝΝ½ΙUΉ¥Ρ	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΩ¥‘•½½µΑΙ•ΝΝ½Ι5½‘•	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΩ¥‘•½½µΑΙ•ΝΝ½Ι=ΥΡΑΥΡ½±‘•ΙI•Ρ|ντμ(€€€IPΩ¥‘•½½µΑΙ•ΝΝ½Ι	Ι½έΝ•=ΥΡΑΥΡ	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΩ¥‘•½½µΑΙ•ΝΝ½Ι½µΑΙ•ΝΝ	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΩ¥‘•½½µΑΙ•ΝΝ½Ι…Ή•±	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΩ¥‘•½½µΑΙ•ΝΝ½ΙAΙ•Ν•ΠΔΑI•Ρ|ντμ(€€€IPΩ¥‘•½½µΑΙ•ΝΝ½ΙAΙ•Ν•ΠΘΥI•Ρ|ντμ(€€€IPΩ¥‘•½½µΑΙ•ΝΝ½ΙAΙ•Ν•ΠΤΑI•Ρ|ντμ(€€€IPΩ¥‘•½½µΑΙ•ΝΝ½ΙAΙ•Ν•ΠΔΐΑI•Ρ|ντμ(€€€IPΩ¥‘•½½µΑΙ•ΝΝ½ΙAΙ•Ν•ΠΜΐΑI•Ρ|ντμ(€€€IPΩ¥‘•½½µΑΙ•ΝΝ½ΙAΙ•Ν•ΠΤΐΑI•Ρ|ντμ(€€€IPΩ¥‘•½½µΑΙ•ΝΝ½Ι‘Ω…Ή•‘Q½±•I•Ρ|ντμ(€€€IPΩ¥‘•½½µΑΙ•ΝΝ½Ι‘Ω…Ή•‘A…Ή•±I•Ρ|ντμ(€€€IPΩ¥‘•½½µΑΙ•ΝΝ½ΙI•Ν½±ΥΡ¥½Ή	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΩ¥‘•½½µΑΙ•ΝΝ½ΙΑΝ	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΩ¥‘•½½µΑΙ•ΝΝ½ΙΥ‘¥½	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΩ¥‘•½½µΑΙ•ΝΝ½ΙAΙ•Ν•Ρ	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΩ¥‘•½½µΑΙ•ΝΝ½ΙΉ½‘•Ι	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΩ¥‘•½½µΑΙ•ΝΝ½Ι½Ή™±¥Ρ	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΩ¥‘•½½µΑΙ•ΝΝ½ΙY•Ι¥™εQ½±•I•Ρ|ντμ(€€€IPΩ¥‘•½½µΑΙ•ΝΝ½ΙI•ΡΙεQ½±•I•Ρ|ντμ(€€€IPΩ¥‘•½½µΑΙ•ΝΝ½Ι5•Ρ…‘…Ρ…Q½±•I•Ρ|ντμ(€€€IPΩ¥‘•½½µΑΙ•ΝΝ½ΙΝΡ¥µ…Ρ•I•Ρ|ντμ(€€€IPΩ¥‘•½½µΑΙ•ΝΝ½ΙAΙ½Ι•ΝΝI•Ρ|ντμ(€€€IPΩ¥‘•½½µΑΙ•ΝΝ½Ι=Α•Ή¥±•	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΩ¥‘•½½µΑΙ•ΝΝ½Ι=Α•Ή½±‘•Ι	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΩ¥‘•½½µΑΙ•ΝΝ½ΙΉ½Ρ΅•Ι	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΝµ…ΙΡQΙ…ΉΝ™•ΙM•Ή‘Q…‰I•Ρ|ντμ(€€€IPΝµ…ΙΡQΙ…ΉΝ™•ΙI••¥Ω•Q…‰I•Ρ|ντμ(€€€IPΝµ…ΙΡQΙ…ΉΝ™•ΙΙ½Αi½Ή•I•Ρ|ντμ(€€€IPΝµ…ΙΡQΙ…ΉΝ™•Ι	Ι½έΝ•	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΝµ…ΙΡQΙ…ΉΝ™•Ι±•…Ι	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΝµ…ΙΡQΙ…ΉΝ™•ΙΙ•…Ρ•	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΝµ…ΙΡQΙ…ΉΝ™•ΙMΡ½Α	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΝµ…ΙΡQΙ…ΉΝ™•Ι½Αε½‘•	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΝµ…ΙΡQΙ…ΉΝ™•ΙαΑ¥Ι…Ρ¥½Ή	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΝµ…ΙΡQΙ…ΉΝ™•ΙΑΑΙ½Ω…±Q½±•I•Ρ|ντμ(€€€IPΝµ…ΙΡQΙ…ΉΝ™•ΙMΡ½Α™Ρ•ΙQ½±•I•Ρ|ντμ(€€€IPΝµ…ΙΡQΙ…ΉΝ™•Ι5Υ±Ρ¥I••¥Ω•ΙQ½±•I•Ρ|ντμ(€€€IPΝµ…ΙΡQΙ…ΉΝ™•Ι¥Ι•Ρ!½ΝΡQ½±•I•Ρ|ντμ(€€€IPΝµ…ΙΡQΙ…ΉΝ™•Ι9…µ•‘¥ΡI•Ρ|ντμ(€€€IPΝµ…ΙΡQΙ…ΉΝ™•Ι½‘•	½αI•Ρ|ντμ(€€€IPΝµ…ΙΡQΙ…ΉΝ™•Ι±±½έ	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΝµ…ΙΡQΙ…ΉΝ™•Ι•Ήε	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΝµ…ΙΡQΙ…ΉΝ™•ΙM•Ή‘¥±•1¥ΝΡI•Ρ|ντμ(€€€IPΝµ…ΙΡQΙ…ΉΝ™•Ι]•‰IΡM•Ή‘A…Ή•±I•Ρ|ντμ(€€€IPΝµ…ΙΡQΙ…ΉΝ™•Ι½ΑεA…¥Ι¥Ή	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΝµ…ΙΡQΙ…ΉΝ™•ΙI••¥Ω•ΙI•ΝΑ½ΉΝ•‘¥ΡI•Ρ|ντμ(€€€IPΝµ…ΙΡQΙ…ΉΝ™•ΙΑΑ±εI•ΝΑ½ΉΝ•	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΝµ…ΙΡQΙ…ΉΝ™•ΙI••¥Ω•½‘•‘¥ΡI•Ρ|ντμ(€€€IPΝµ…ΙΡQΙ…ΉΝ™•Ι½ΉΉ•Ρ	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΝµ…ΙΡQΙ…ΉΝ™•Ι±•…Ι½‘•	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΝµ…ΙΡQΙ…ΉΝ™•Ι]•‰IΡI••¥Ω•A…Ή•±I•Ρ|ντμ(€€€IPΝµ…ΙΡQΙ…ΉΝ™•ΙM•Ή‘•ΙA…¥Ι¥Ή‘¥ΡI•Ρ|ντμ(€€€IPΝµ…ΙΡQΙ…ΉΝ™•Ι•Ή•Ι…Ρ•I•ΝΑ½ΉΝ•	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΝµ…ΙΡQΙ…ΉΝ™•Ι½ΑεI•ΝΑ½ΉΝ•	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΝµ…ΙΡQΙ…ΉΝ™•Ι5…Ή¥™•ΝΡI•Ρ|ντμ(€€€IPΝµ…ΙΡQΙ…ΉΝ™•ΙM…Ω•½±‘•ΙI•Ρ|ντμ(€€€IPΝµ…ΙΡQΙ…ΉΝ™•Ι	Ι½έΝ•M…Ω•	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΝµ…ΙΡQΙ…ΉΝ™•Ι½έΉ±½…‘	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΝµ…ΙΡQΙ…ΉΝ™•Ι…Ή•±	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΝµ…ΙΡQΙ…ΉΝ™•Ι=Α•Ή½±‘•Ι	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΝµ…ΙΡQΙ…ΉΝ™•ΙAΙ½Ι•ΝΝI•Ρ|ντμ(€€€IPµ…Ι½1¥ΝΡA…Ή•±I•Ρ|ντμ(€€€IPµ…Ι½9•έ	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPµ…Ι½%µΑ½ΙΡ	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPµ…Ι½αΑ½ΙΡ	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPµ…Ι½•±•Ρ•	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPµ…Ι½ΥΑ±¥…Ρ•	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPµ…Ι½=Ω•Ι±…ε	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPµ…Ι½•Ρ…¥±ΝA…Ή•±I•Ρ|ντμ(€€€IPµ…Ι½9…µ•‘¥ΡI•Ρ|ντμ(€€€IPµ…Ι½M…Ω•	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPµ…Ι½I•½Ι‘A…Ή•±I•Ρ|ντμ(€€€IPµ…Ι½I•½Ι‘!½Ρ­•ε	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPµ…Ι½A±…ε!½Ρ­•ε	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPµ…Ι½MΡ½Α!½Ρ­•ε	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPµ…Ι½Ιµ	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPµ…Ι½MΡ…ΙΡI•½Ι‘	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPµ…Ι½MΡ½ΑI•½Ι‘	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPµ…Ι½…Ή•±I•½Ι‘	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPµ…Ι½A±…ε‰…­A…Ή•±I•Ρ|ντμ(€€€IPµ…Ι½A±…ε=Ή•	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPµ…Ι½1½½Α	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPµ…Ι½1½½ΑUΉΡ¥±MΡ½ΑΑ•‘Q½±•I•Ρ|ντμ(€€€IPµ…Ι½MΡ½ΑA±…ε‰…­	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPµ…Ι½A±…ε‰…­MΑ••‘	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPµ…Ι½A±…ε‰…­	…­•Ή‘	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPµ…Ι½MΡ…ΙΡ•±…ε	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPµ…Ι½I•ΕΥ¥Ι•Q…Ι•ΡQ½±•I•Ρ|ντμ(€€€IPµ…Ι½AΙ½Ι•ΝΝI•Ρ|ντμ(€€€IPµ…Ι½M•ΡΡ¥ΉΝA…Ή•±I•Ρ|ντμ(€€€IPµ…Ι½5½ΥΝ•5½‘•	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPµ…Ι½…ΑΡΥΙ•I…Ρ•	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPµ…Ι½Ω•ΉΡAΙ•Ω¥•έI•Ρ|ντμ(€€€IPµ…Ι½=Ω•Ι±…εΙµ	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPµ…Ι½=Ω•Ι±…εMΡ…ΙΡ	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPµ…Ι½=Ω•Ι±…εMΡ½ΑI•½Ι‘	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPµ…Ι½=Ω•Ι±…εA±…ε	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPµ…Ι½=Ω•Ι±…εMΡ½ΑA±…ε‰…­	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPµ…Ι½=Ω•Ι±…ε±½Ν•	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΝ•ΡΡ¥ΉΝ΅•­UΑ‘…Ρ•Ν	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΝ•ΡΡ¥ΉΝ½έΉ±½…‘UΑ‘…Ρ•	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΝ•ΡΡ¥ΉΝ•Ή•Ι…±Q…‰I•Ρ|ντμ(€€€IPΝ•ΡΡ¥ΉΝMµ…ΙΡQΙ…ΉΝ™•ΙQ…‰I•Ρ|ντμ(€€€IPΝ•ΡΡ¥ΉΝ5…Ι½I•½Ι‘•ΙQ…‰I•Ρ|ντμ(€€€IPΝ•ΡΡ¥ΉΝΑΑ•…Ι…Ή•Q…‰I•Ρ|ντμ(€€€IPΝ•ΡΡ¥ΉΝUΑ‘…Ρ•ΝQ…‰I•Ρ|ντμ(€€€IPΝ•ΡΡ¥ΉΝ‰½ΥΡQ…‰I•Ρ|ντμ(€€€IPΝ•ΡΡ¥ΉΝ•™…Υ±Ρ½±‘•ΙI•Ρ|ντμ(€€€IPΝ•ΡΡ¥ΉΝ	Ι½έΝ••™…Υ±Ρ½±‘•Ι	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΝ•ΡΡ¥ΉΝMΡ…ΙΡA…•	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΝ•ΡΡ¥ΉΝ5¥Ή¥µ¥ι•Q½QΙ…εQ½±•I•Ρ|ντμ(€€€IPΝ•ΡΡ¥ΉΝMΡ…ΙΡ]¥Ρ΅]¥Ή‘½έΝQ½±•I•Ρ|ντμ(€€€IPΝ•ΡΡ¥ΉΝ]•‰IΡ…±±‰…­Q½±•I•Ρ|ντμ(€€€IPΝ•ΡΡ¥ΉΝ]•‰IΡ¥…Ή½ΝΡ¥ΝQ½±•I•Ρ|ντμ(€€€IPΝ•ΡΡ¥ΉΝ]•‰IΡMΡΥΉM•ΙΩ•ΙΝI•Ρ|ντμ(€€€IPΝ•ΡΡ¥ΉΝ5…Ι½I•½Ι‘!½Ρ­•ε	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΝ•ΡΡ¥ΉΝ5…Ι½A±…ε!½Ρ­•ε	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΝ•ΡΡ¥ΉΝ5…Ι½MΡ½Α!½Ρ­•ε	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΝ•ΡΡ¥ΉΝ5…Ι½5½ΥΝ•5½‘•	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΝ•ΡΡ¥ΉΝ5…Ι½…ΑΡΥΙ•I…Ρ•	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΝ•ΡΡ¥ΉΝ5…Ι½A±…ε‰…­MΑ••‘	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΝ•ΡΡ¥ΉΝ5…Ι½MΡ…ΙΡ•±…ε	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΝ•ΡΡ¥ΉΝ5…Ι½1½½ΑQ½±•I•Ρ|ντμ(€€€IPΝ•ΡΡ¥ΉΝ5…Ι½Q…Ι•ΡQ½±•I•Ρ|ντμ(€€€IPΝ•ΡΡ¥ΉΝ±½­½Ιµ…Ρ	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΝ•ΡΡ¥ΉΝQ΅•µ•	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΝ•ΡΡ¥ΉΝ¥Ρ΅Υ‰	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΝ•ΡΡ¥ΉΝI•Α½ΙΡ%ΝΝΥ•	ΥΡΡ½ΉI•Ρ|ντμ(€€€IP…Ή¥µ•M•…Ι΅Q…‰I•Ρ|ντμ(€€€IP…Ή¥µ•1¥ΝΡQ…‰I•Ρ|ντμ(€€€IP…Ή¥µ•M•…Ι΅‘¥ΡI•Ρ|ντμ(€€€IP…Ή¥µ•M•…Ι΅	ΥΡΡ½ΉI•Ρ|ντμ(€€€IP…Ή¥µ•1½…‘5½Ι•	ΥΡΡ½ΉI•Ρ|ντμ(€€€IP…Ή¥µ•M•…Ι΅•Ρ…¥±	…­	ΥΡΡ½ΉI•Ρ|ντμ(€€€IP…Ή¥µ•M•…Ι΅•Ρ…¥±‘‘	ΥΡΡ½ΉI•Ρ|ντμ(€€€IP…Ή¥µ•M•…Ι΅•Ρ…¥±I•µ¥Ή‘•Ι	ΥΡΡ½ΉI•Ρ|ντμ(€€€IP…Ή¥µ•M•…Ι΅•Ρ…¥±=Α•Ή	ΥΡΡ½ΉI•Ρ|ντμ(€€€IP…Ή¥µ•I•ΝΥ±ΡΝI•Ρ|ντμ(€€€IP…Ή¥µ•1¥ΝΡI•Ρ|ντμ(€€€IP…Ή¥µ•UΑ½µ¥ΉI•Ρ|ντμ(€€€IP…Ή¥µ•M•ΕΥ•±ΝI•Ρ|ντμ(€€€IP…Ή¥µ•I•™Ι•Ν΅±±	ΥΡΡ½ΉI•Ρ|ντμ(€€€IP…Ή¥µ•¥±Ρ•Ι	ΥΡΡ½ΉI•Ρ|ντμ(€€€IP…Ή¥µ•%µΑ½ΙΡ‘¥ΡI•Ρ|ντμ(€€€IP…Ή¥µ•%µΑ½ΙΡ	ΥΡΡ½ΉI•Ρ|ντμ(€€€IP…Ή¥µ•%µΑ½ΙΡ΅½¥•A…Ή•±I•Ρ|ντμ(€€€IP…Ή¥µ•%µΑ½ΙΡ΅½¥•Ή¥1¥ΝΡ	ΥΡΡ½ΉI•Ρ|ντμ(€€€IP…Ή¥µ•%µΑ½ΙΡ΅½¥•5εΉ¥µ•1¥ΝΡ	ΥΡΡ½ΉI•Ρ|ντμ(€€€IP…Ή¥µ•%µΑ½ΙΡ΅½¥•…Ή•±	ΥΡΡ½ΉI•Ρ|ντμ(€€€IP…Ή¥µ•MΡ…ΡΥΝ	ΥΡΡ½ΉI•Ρ|ντμ(€€€IP…Ή¥µ•Α¥Ν½‘•5¥ΉΥΝ	ΥΡΡ½ΉI•Ρ|ντμ(€€€IP…Ή¥µ•Α¥Ν½‘•A±ΥΝ	ΥΡΡ½ΉI•Ρ|ντμ(€€€IP…Ή¥µ•…Ω½Ι¥Ρ•	ΥΡΡ½ΉI•Ρ|ντμ(€€€IP…Ή¥µ•I•µ¥Ή‘•Ι	ΥΡΡ½ΉI•Ρ|ντμ(€€€IP…Ή¥µ•M…Ω•9½Ρ•Ν	ΥΡΡ½ΉI•Ρ|ντμ(€€€IP…Ή¥µ•9½Ρ•Ν‘¥ΡI•Ρ|ντμ(€€€IPΙ•µ¥Ή‘•Ι9•έ	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΙ•µ¥Ή‘•ΙEΥ¥­A…Ή•±I•Ρ|ντμ(€€€IPΙ•µ¥Ή‘•ΙQ¥Ρ±•‘¥ΡI•Ρ|ντμ(€€€IPΙ•µ¥Ή‘•Ι…Ρ•‘¥ΡI•Ρ|ντμ(€€€IPΙ•µ¥Ή‘•Ι…Ρ•A¥­•Ι	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΙ•µ¥Ή‘•ΙQ¥µ•‘¥ΡI•Ρ|ντμ(€€€IPΙ•µ¥Ή‘•ΙQ¥µ•µAµ	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΙ•µ¥Ή‘•ΙAΙ•Ν•Ρ	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΙ•µ¥Ή‘•Ι‘‘	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΙ•µ¥Ή‘•Ι…Ρ•½Ιε‘¥ΡI•Ρ|ντμ(€€€IPΙ•µ¥Ή‘•ΙAΙ¥½Ι¥Ρε	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΙ•µ¥Ή‘•ΙI•Α•…Ρ	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΙ•µ¥Ή‘•Ι±•ΙΡ	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΙ•µ¥Ή‘•Ι5½Ι•=ΑΡ¥½ΉΝQ½±•I•Ρ|ντμ(€€€IPΙ•µ¥Ή‘•Ι±±…εQ½±•I•Ρ|ντμ(€€€IPΙ•µ¥Ή‘•Ι	¥ΙΡ΅‘…εQ½±•I•Ρ|ντμ(€€€IPΙ•µ¥Ή‘•Ι9½Ρ•Ν‘¥ΡI•Ρ|ντμ(€€€IPΙ•µ¥Ή‘•Ι1¥ΝΡI•Ρ|ντμ(€€€IPΙ•µ¥Ή‘•ΙM•…Ι΅‘¥ΡI•Ρ|ντμ(€€€IPΙ•µ¥Ή‘•Ι¥±Ρ•Ι	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΙ•µ¥Ή‘•ΙM½ΙΡ	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΙ•µ¥Ή‘•Ι…±•Ή‘…ΙI•Ρ|ντμ(€€€IPΙ•µ¥Ή‘•Ι…±•Ή‘…ΙAΙ•Ω	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΙ•µ¥Ή‘•Ι…±•Ή‘…Ι9•αΡ	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΝΙ½±±	…ΙQΙ…­I•Ρ|ντμ(€€€IPΝΙ½±±	…ΙQ΅Υµ‰I•Ρ|ντμ(€€€IP΅½Ω•Ι•‘	ΥΡΡ½ΉI•Ρ|ντμ(€€€IPΑΙ•ΝΝ•‘	ΥΡΡ½ΉI•Ρ|ντμ(€€€‰½½°΅…Ν!½Ω•Ι•‘	ΥΡΡ½Ή|€τ™…±Ν”μ(€€€‰½½°΅…ΝAΙ•ΝΝ•‘	ΥΡΡ½Ή|€τ™…±Ν”μ(€€€Ι½Α‘½έΉ-¥Ή…Ρ¥Ω•Ι½Α‘½έΉ|€τΙ½Α‘½έΉ-¥Ήθι9½Ή”μ(€€€IP‘Ι½Α‘½έΉI•Ρ|ντμ(€€€ΝΡθιΩ•Ρ½ΘρΝΡθιέΝΡΙ¥Ήψ‘Ι½Α‘½έΉ1…‰•±Ν|μ(€€€ΝΡθιΩ•Ρ½Θρ¥ΉΠψ‘Ι½Α‘½έΉY…±Υ•Ν|μ(€€€ΝΡθιΩ•Ρ½Θρ‰½½°ψ‘Ι½Α‘½έΉΉ…‰±•‘|μ(€€€¥ΉΠ‘Ι½Α‘½έΉM•±•Ρ•‘Y…±Υ•|€τ€ΐμ(€€€¥ΉΠ΅½Ω•ΙΙ½Α‘½έΉ%Ή‘•α|€τ€΄Δμ(€€€!]9µ•‘¥…UΙ±‘¥Ρ|€τΉΥ±±ΑΡΘμ(€€€!]9µ•‘¥…¥±•9…µ•‘¥Ρ|€τΉΥ±±ΑΡΘμ(€€€!]9Ω¥‘•½½µΑΙ•ΝΝ½ΙQ…Ι•Ρ‘¥Ρ|€τΉΥ±±ΑΡΘμ(€€€!]9Νµ…ΙΡQΙ…ΉΝ™•Ι9…µ•‘¥Ρ|€τΉΥ±±ΑΡΘμ(€€€!]9Νµ…ΙΡQΙ…ΉΝ™•Ι½‘•‘¥Ρ|€τΉΥ±±ΑΡΘμ(€€€!]9Νµ…ΙΡQΙ…ΉΝ™•ΙI••¥Ω•ΙI•ΝΑ½ΉΝ•‘¥Ρ|€τΉΥ±±ΑΡΘμ(€€€!]9Νµ…ΙΡQΙ…ΉΝ™•ΙM•Ή‘•ΙA…¥Ι¥Ή‘¥Ρ|€τΉΥ±±ΑΡΘμ(€€€!]9…±±Q½½±ΝM•…Ι΅‘¥Ρ|€τΉΥ±±ΑΡΘμ(€€€!]9…Ή¥µ•M•…Ι΅‘¥Ρ|€τΉΥ±±ΑΡΘμ(€€€!]9…Ή¥µ•%µΑ½ΙΡ‘¥Ρ|€τΉΥ±±ΑΡΘμ(€€€!]9…Ή¥µ•9½Ρ•Ν‘¥Ρ|€τΉΥ±±ΑΡΘμ(€€€!]9Ι•µ¥Ή‘•ΙQ¥Ρ±•‘¥Ρ|€τΉΥ±±ΑΡΘμ(€€€!]9Ι•µ¥Ή‘•Ι…Ρ•‘¥Ρ|€τΉΥ±±ΑΡΘμ(€€€!]9Ι•µ¥Ή‘•ΙQ¥µ•‘¥Ρ|€τΉΥ±±ΑΡΘμ(€€€!]9Ι•µ¥Ή‘•Ι…Ρ•½Ιε‘¥Ρ|€τΉΥ±±ΑΡΘμ(€€€!]9Ι•µ¥Ή‘•Ι9½Ρ•Ν‘¥Ρ|€τΉΥ±±ΑΡΘμ(€€€!]9Ι•µ¥Ή‘•ΙM•…Ι΅‘¥Ρ|€τΉΥ±±ΑΡΘμ(€€€!]9µ…Ι½9…µ•‘¥Ρ|€τΉΥ±±ΑΡΘμ(€€€!]9µ…Ι½=Ω•Ι±…ε]¥Ή‘½έ|€τΉΥ±±ΑΡΘμ(€€€!	IUM •‘¥Ρ	…­Ι½ΥΉ‘	ΙΥΝ΅|€τΉΥ±±ΑΡΘμ(€€€!‰…­	Υ™™•Ι|€τΉΥ±±ΑΡΘμ(€€€!	%Q5@‰…­	Υ™™•Ι	¥Ρµ…Α|€τΉΥ±±ΑΡΘμ(€€€!	%Q5@‰…­	Υ™™•ΙAΙ•Ω¥½ΥΝ	¥Ρµ…Α|€τΉΥ±±ΑΡΘμ(€€€M%i‰…­	Υ™™•ΙM¥ι•|ντμ(€€€¥ΉΠ΅½Ω•Ι9…Ω%Ή‘•α|€τ€΄Δμ(€€€¥ΉΠ΅½Ω•ΙQ½½±%Ή‘•α|€τ€΄Δμ(€€€¥ΉΠ΅½Ω•ΙΉ¥µ•M•…Ι΅I•ΝΥ±Ρ%Ή‘•α|€τ€΄Δμ(€€€‰½½°ΝΑ••‘M±¥‘•ΙΙ…¥Ή|€τ™…±Ν”μ(€€€‰½½°½ΉΩ•ΙΡ•ΙEΥ…±¥ΡεΙ…¥Ή|€τ™…±Ν”μ(€€€‰½½°ΝΙ½±±	…ΙΙ…¥Ή|€τ™…±Ν”μ(€€€‰½½°±¥Ω•I•Ν¥ι•|€τ™…±Ν”μ(€€€‰½½°Ι•Ν¥ι•1…ε½ΥΡA•Ή‘¥Ή|€τ™…±Ν”μ(€€€‰½½°µ¥Ή¥µ¥ι•‘Q½QΙ…ε|€τ™…±Ν”μ(€€€‰½½°ΡΙ…ε%½ΉY¥Ν¥‰±•|€τ™…±Ν”μ(€€€‰½½°ΡΙ…εI•ΝΡ½Ι•5…α¥µ¥ι•‘|€τ™…±Ν”μ(€€€‰½½°ΝΡ…ΙΡ5¥Ή¥µ¥ι•‘Q½QΙ…ε|€τ™…±Ν”μ(€€€‰½½°…ΥΡ½µ…Ρ¥½Ή%ΉΑΥΡΝΉ…‰±•‘|€τ™…±Ν”μ(€€€‰½½°™½Ι•±½Ν•|€τ™…±Ν”μ(€€€¥ΉΠΝΙ½±±	…ΙΙ…=™™Ν•Ρe|€τ€ΐμ(€€€¥ΉΠΝΙ½±±=™™Ν•Ρe|€τ€ΐμ(€€€¥ΉΠ…Ή¥µ•M•…Ι΅I•ΝΥ±ΡΝMΙ½±±=™™Ν•Ρ|€τ€ΐμ(€€€¥ΉΠµ…αMΙ½±±=™™Ν•Ρe|€τ€ΐμ(€€€¥ΉΠΝΙ½±±½ΉΡ•ΉΡ!•¥΅Ρ|€τ€ΐμ(€€€‰½½°™¥±•½ΉΩ•ΙΡ•Ι‘Ω…Ή•‘=Α•Ή|€τ™…±Ν”μ(€€€‰½½°µ½ΥΝ•1•…Ω•QΙ…­¥Ή|€τ™…±Ν”μ(€€€‰½½°Ρ•ΝΡ	ΥΡΡ½Ή!•±‘|€τ™…±Ν”μ(€€€‰½½°…έ…¥Ρ¥ΉΡ¥Ω…Ρ¥½Ή-•ε|€τ™…±Ν”μ(€€€‰½½°…έ…¥Ρ¥Ή=ΥΡΑΥΡ	ΥΡΡ½Ή|€τ™…±Ν”μ(€€€‰½½°…έ…¥Ρ¥Ή±Ρ•ΙΉ…Ρ•=ΥΡΑΥΡ	ΥΡΡ½Ή|€τ™…±Ν”μ((€€€!=9PΡ¥Ρ±•½ΉΡ|€τΉΥ±±ΑΡΘμ(€€€!=9PΉ…Ω½ΉΡ|€τΉΥ±±ΑΡΘμ(€€€!=9P΅•…‘¥Ή½ΉΡ|€τΉΥ±±ΑΡΘμ(€€€!=9P‰½‘ε½ΉΡ|€τΉΥ±±ΑΡΘμ(€€€!=9PΝ•…Ι΅%ΉΑΥΡ½ΉΡ|€τΉΥ±±ΑΡΘμ(€€€!=9Pµ½Ή½ΝΑ…•½ΉΡ|€τΉΥ±±ΑΡΘμ(€€€¥ΉΠ‘Α¥|€τ€δΨμ(€€€¥ΉΠµ…Ι½=Ω•Ι±…εΑ¥|€τ€δΨμ)τμ(