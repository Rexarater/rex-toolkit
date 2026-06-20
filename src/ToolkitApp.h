#pragma once

#include "AnimeTrackerService.h"
#include "FileConversionService.h"
#include "MediaDownloadService.h"
#include "ReminderService.h"
#include "SmartFileTransferService.h"
#include "UpdateChecker.h"

#include <windows.h>
#include <gdiplus.h>

#include <atomic>
#include <condition_variable>
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
    SmartFileTransfer
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
    SmartTransferExpiration
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
    bool running = false;
};

struct AppSettings
{
    std::filesystem::path defaultOutputFolder;
    DefaultStartPage startPage = DefaultStartPage::Favorites;
    ClockFormat clockFormat = ClockFormat::MonthDay24;
    AppTheme theme = AppTheme::Dark;
    bool minimizeToTrayOnClose = false;
    bool smartTransferWebRtcFallback = true;
    bool smartTransferWebRtcDiagnostics = false;
    std::wstring smartTransferStunServers = L"stun:stun.l.google.com:19302";
    long long updateNotificationDismissedUntil = 0;
};

class ToolkitApp
{
public:
    explicit ToolkitApp(HINSTANCE instance);

    static bool ActivateExistingInstanceIfRunning();

    int Run(int showCommand);

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK KeyboardHookProc(int code, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK MouseHookProc(int code, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

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
    void PaintReminders(HDC hdc);
    void PaintSmartFileTransfer(HDC hdc);
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
    void LoadRemindersData();
    void SaveRemindersData();
    std::wstring RemindersFilePath() const;
    void CreateReminderControls();
    void UpdateReminderControls();
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
    void ApplyAnimeSearchResponse(const AnimeSearchResponse& response, const std::wstring& message, bool appendResults);
    void ApplyAnimeImportResult(const AnimeImportResult& result, const std::wstring& userName, const std::wstring& message);
    void AddAnimeFromSearch(size_t index);
    void AddAnimeFromRelation(size_t index);
    void RefreshAnimeEntry(size_t index);
    void RefreshAllAnime();
    void ApplyAnimeRefreshResult(const AnimeSearchResult& result, int listIndex, const std::wstring& message);
    void SelectAnimeEntry(int index);
    void RemoveAnimeEntry(size_t index);
    void IncrementAnimeEpisode(size_t index);
    void DecrementSelectedAnimeEpisode();
    void CycleSelectedAnimeStatus();
    void ToggleSelectedAnimeFavorite();
    void SaveSelectedAnimeNotes();
    void ShowAnimeFilterDropdown();
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
    void InstallInputHooks();
    void RemoveInputHooks();
    bool HandleKeyboardHook(WPARAM message, const KBDLLHOOKSTRUCT& keyboard);
    bool HandleMouseHook(WPARAM message, const MSLLHOOKSTRUCT& mouse);
    bool IsActivationMouseMessage(WPARAM message, const MSLLHOOKSTRUCT& mouse, bool down) const;

    void OpenDropdown(DropdownKind kind, const RECT& anchor, const std::vector<std::wstring>& labels, const std::vector<int>& values, const std::vector<bool>& enabled, int selectedValue);
    void CloseDropdown();
    void PaintDropdown(HDC hdc);
    bool HandleDropdownClick(POINT point);
    RECT ButtonRectAtPoint(POINT point) const;
    void UpdateButtonHover(POINT point);
    std::vector<ToolDefinition> VisibleToolsForCurrentPage() const;
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
    std::wstring StartPageLabel() const;
    std::wstring ClockFormatLabel() const;
    std::wstring ThemeLabel() const;
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

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    ULONG_PTR gdiplusToken_ = 0;
    std::unique_ptr<Gdiplus::Bitmap> logo_;
    std::unique_ptr<Gdiplus::Bitmap> autoClickerIcon_;
    std::unique_ptr<Gdiplus::Bitmap> fileConverterIcon_;
    std::unique_ptr<Gdiplus::Bitmap> mediaDownloaderIcon_;
    std::unique_ptr<Gdiplus::Bitmap> remindersIcon_;
    std::unique_ptr<Gdiplus::Bitmap> smartFileTransferIcon_;
    std::unique_ptr<Gdiplus::Bitmap> allToolsIcon_;
    std::unique_ptr<Gdiplus::Bitmap> settingsIcon_;
    std::unique_ptr<Gdiplus::Bitmap> autoClickerIconTinted_;
    std::unique_ptr<Gdiplus::Bitmap> fileConverterIconTinted_;
    std::unique_ptr<Gdiplus::Bitmap> mediaDownloaderIconTinted_;
    std::unique_ptr<Gdiplus::Bitmap> remindersIconTinted_;
    std::unique_ptr<Gdiplus::Bitmap> smartFileTransferIconTinted_;
    std::unique_ptr<Gdiplus::Bitmap> allToolsIconTinted_;
    std::unique_ptr<Gdiplus::Bitmap> settingsIconTinted_;
    HHOOK keyboardHook_ = nullptr;
    HHOOK mouseHook_ = nullptr;
    HANDLE singleInstanceMutex_ = nullptr;
    Page currentPage_ = Page::Favorites;
    ToolKind currentTool_ = ToolKind::None;
    std::vector<ToolDefinition> tools_;
    AppSettings appSettings_;
    AutoClickerState autoClicker_;
    FileConversionService fileConversionService_;
    MediaDownloadService mediaDownloadService_;
    AnimeTrackerService animeTrackerService_;
    ReminderService reminderService_;
    SmartFileTransferService smartFileTransferService_;
    std::vector<ConversionJob> conversionJobs_;
    ConversionOptions conversionOptions_;
    MediaDownloadOptions mediaDownloadOptions_;
    MediaDownloadJob mediaDownloadJob_;
    ExternalToolStatus mediaExternalTools_;
    UpdateChecker updateChecker_;
    AnimeWatchList animeWatchList_;
    AnimeSearchResponse animeSearchResponse_;
    std::vector<AnimeSearchResult> animeSearchResults_;
    ReminderList reminderList_;
    ReminderAlert reminderAlert_;
    SmartTransferTab smartTransferTab_ = SmartTransferTab::Send;
    SmartTransferSendOptions smartTransferOptions_;
    SmartTransferHostSnapshot smartTransferHostSnapshot_;
    SmartTransferManifest smartTransferReceiveManifest_;
    SmartTransferInvite smartTransferReceiveInvite_;
    SmartTransferDownloadProgress smartTransferDownloadProgress_;
    SmartTransferWebRtcSnapshot smartTransferWebRtcSnapshot_;
    std::vector<SmartTransferFile> smartTransferFiles_;
    std::filesystem::path smartTransferSaveFolder_;
    std::wstring smartTransferSenderPairingCode_;
    std::wstring smartTransferReceiverResponseCode_;
    std::wstring animeStatusMessage_;
    std::wstring reminderStatusMessage_;
    std::wstring smartTransferStatusMessage_;
    std::wstring smartTransferReceiveStatusMessage_;
    std::wstring smartTransferReceiveWebRtcMessage_;
    std::wstring lastReminderNotificationKey_;
    std::map<std::wstring, std::unique_ptr<Gdiplus::Bitmap>> animeCoverCache_;
    std::vector<ImageFormat> supportedOutputFormats_;
    int selectedConversionJob_ = -1;
    int selectedAnimeSearchIndex_ = -1;
    std::wstring fileConverterSummary_;
    bool fileConverterConverting_ = false;
    std::thread conversionThread_;
    std::atomic_bool conversionCancelRequested_ = false;
    std::thread mediaThread_;
    std::atomic_bool mediaCancelRequested_ = false;
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
    std::atomic_bool smartTransferCancelRequested_ = false;
    bool mediaAnalyzing_ = false;
    bool mediaMusicAnalyzing_ = false;
    bool mediaDownloading_ = false;
    std::wstring mediaStatusText_;
    UpdateCheckResult updateResult_;
    bool updateChecking_ = false;
    bool updateInstalling_ = false;
    bool updateCheckSilent_ = false;
    bool updateNotificationVisible_ = false;
    bool smartTransferConnecting_ = false;
    bool smartTransferDownloading_ = false;
    bool smartTransferHosting_ = false;
    bool smartTransferWebRtcFallbackOffered_ = false;
    bool smartTransferWebRtcDependencyMissing_ = false;
    bool smartTransferWebRtcBusy_ = false;
    bool smartTransferWebRtcReceiverActive_ = false;
    bool animeSearching_ = false;
    bool animeRefreshing_ = false;
    bool animeImporting_ = false;
    bool animeSearchHasRun_ = false;
    bool animeCanLoadMore_ = false;
    bool animeAppendSearch_ = false;
    bool hasUpdateResult_ = false;
    std::wstring updateInstallStatus_;
    int animeCurrentPage_ = 1;
    int selectedAnimeIndex_ = -1;
    AnimeUserStatus animeFilter_ = AnimeUserStatus::Watching;
    bool animeFilterAll_ = true;
    AnimeTrackerTab animeTrackerTab_ = AnimeTrackerTab::Search;
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
    bool suppressAnimeNotesChange_ = false;
    std::wstring animeNotesStatusText_;
    SIZE savedWindowSize_ { 1060, 680 };
    bool savedWindowMaximized_ = false;

    RECT clientRect_ {};
    RECT headerRect_ {};
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
    RECT startStopButtonRect_ {};
    RECT activationKeyButtonRect_ {};
    RECT outputButtonButtonRect_ {};
    RECT alternateOutputToggleRect_ {};
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
    RECT settingsCheckUpdatesButtonRect_ {};
    RECT settingsDownloadUpdateButtonRect_ {};
    RECT settingsGeneralTabRect_ {};
    RECT settingsSmartTransferTabRect_ {};
    RECT settingsAppearanceTabRect_ {};
    RECT settingsUpdatesTabRect_ {};
    RECT settingsAboutTabRect_ {};
    RECT settingsDefaultFolderRect_ {};
    RECT settingsBrowseDefaultFolderButtonRect_ {};
    RECT settingsStartPageButtonRect_ {};
    RECT settingsMinimizeToTrayToggleRect_ {};
    RECT settingsWebRtcFallbackToggleRect_ {};
    RECT settingsWebRtcDiagnosticsToggleRect_ {};
    RECT settingsWebRtcStunServersRect_ {};
    RECT settingsClockFormatButtonRect_ {};
    RECT settingsThemeButtonRect_ {};
    RECT settingsGithubButtonRect_ {};
    RECT settingsReportIssueButtonRect_ {};
    RECT animeSearchTabRect_ {};
    RECT animeListTabRect_ {};
    RECT animeSearchEditRect_ {};
    RECT animeSearchButtonRect_ {};
    RECT animeLoadMoreButtonRect_ {};
    RECT animeSearchDetailBackButtonRect_ {};
    RECT animeSearchDetailAddButtonRect_ {};
    RECT animeSearchDetailOpenButtonRect_ {};
    RECT animeResultsRect_ {};
    RECT animeListRect_ {};
    RECT animeUpcomingRect_ {};
    RECT animeSequelsRect_ {};
    RECT animeRefreshAllButtonRect_ {};
    RECT animeFilterButtonRect_ {};
    RECT animeImportEditRect_ {};
    RECT animeImportButtonRect_ {};
    RECT animeStatusButtonRect_ {};
    RECT animeEpisodeMinusButtonRect_ {};
    RECT animeEpisodePlusButtonRect_ {};
    RECT animeFavoriteButtonRect_ {};
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
    std::vector<std::wstring> dropdownLabels_;
    std::vector<int> dropdownValues_;
    std::vector<bool> dropdownEnabled_;
    int dropdownSelectedValue_ = 0;
    int hoverDropdownIndex_ = -1;
    HWND mediaUrlEdit_ = nullptr;
    HWND mediaFileNameEdit_ = nullptr;
    HWND smartTransferNameEdit_ = nullptr;
    HWND smartTransferCodeEdit_ = nullptr;
    HWND smartTransferReceiverResponseEdit_ = nullptr;
    HWND smartTransferSenderPairingEdit_ = nullptr;
    HWND animeSearchEdit_ = nullptr;
    HWND animeImportEdit_ = nullptr;
    HWND animeNotesEdit_ = nullptr;
    HWND reminderTitleEdit_ = nullptr;
    HWND reminderDateEdit_ = nullptr;
    HWND reminderTimeEdit_ = nullptr;
    HWND reminderCategoryEdit_ = nullptr;
    HWND reminderNotesEdit_ = nullptr;
    HWND reminderSearchEdit_ = nullptr;
    HBRUSH editBackgroundBrush_ = nullptr;
    HDC backBufferDc_ = nullptr;
    HBITMAP backBufferBitmap_ = nullptr;
    HBITMAP backBufferPreviousBitmap_ = nullptr;
    SIZE backBufferSize_ {};
    int hoverNavIndex_ = -1;
    int hoverToolIndex_ = -1;
    int hoverAnimeSearchResultIndex_ = -1;
    bool speedSliderDragging_ = false;
    bool converterQualityDragging_ = false;
    bool scrollBarDragging_ = false;
    bool liveResize_ = false;
    bool resizeLayoutPending_ = false;
    bool minimizedToTray_ = false;
    bool trayIconVisible_ = false;
    bool trayRestoreMaximized_ = false;
    bool forceClose_ = false;
    int scrollBarDragOffsetY_ = 0;
    int scrollOffsetY_ = 0;
    int animeSearchResultsScrollOffset_ = 0;
    int maxScrollOffsetY_ = 0;
    int scrollContentHeight_ = 0;
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
    int dpi_ = 96;
};
