#pragma once

#include "FileConversionService.h"
#include "MediaDownloadService.h"
#include "UpdateChecker.h"

#include <windows.h>
#include <gdiplus.h>

#include <atomic>
#include <condition_variable>
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
    MediaDownloader
};

enum class OutputMouseButton
{
    Left,
    Right,
    Middle
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
    MediaQuality
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
    OutputMouseButton outputButton = OutputMouseButton::Left;
    bool running = false;
};

class ToolkitApp
{
public:
    explicit ToolkitApp(HINSTANCE instance);

    int Run(int showCommand);

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK KeyboardHookProc(int code, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK MouseHookProc(int code, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    bool RegisterWindowClass();
    bool CreateMainWindow(int showCommand);

    void ApplyDarkTitleBar();
    void LoadLogoResource();
    std::unique_ptr<Gdiplus::Bitmap> LoadPngResource(int resourceId) const;
    void LoadToolIconResources();
    void LoadFavorites();
    void SaveFavorites() const;
    void LoadWindowSettings();
    void SaveWindowSettings() const;
    void LoadMediaDownloadSettings();
    void SaveMediaDownloadSettings() const;
    void LoadAutoClickerSettings();
    void SaveAutoClickerSettings() const;
    std::wstring SettingsDirectory() const;
    std::wstring FavoritesFilePath() const;
    std::wstring WindowSettingsFilePath() const;
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
    void PaintContent(HDC hdc);
    void PaintNavItem(HDC hdc, const RECT& bounds, const wchar_t* label, bool selected);
    void PaintEmptyState(HDC hdc, const wchar_t* title, const wchar_t* subtitle);
    void PaintToolCards(HDC hdc, const std::vector<ToolDefinition>& tools);
    void PaintToolIcon(HDC hdc, ToolKind tool, const RECT& bounds);
    void PaintFavoriteStar(HDC hdc, const RECT& bounds, bool favorite);
    void PaintAutoClicker(HDC hdc);
    void PaintFileConverter(HDC hdc);
    void PaintMediaDownloader(HDC hdc);
    void PaintSettings(HDC hdc);
    void PaintProgressBar(HDC hdc, const RECT& bounds, double progress);
    void PaintButton(HDC hdc, const RECT& bounds, const wchar_t* label, bool primary, bool active = false, bool enabled = true);
    void PaintBackButton(HDC hdc, const RECT& bounds);
    void PaintDropdownButton(HDC hdc, const RECT& bounds, const wchar_t* label, bool enabled = true, bool down = true);
    void PaintChevron(HDC hdc, const RECT& bounds, bool down, COLORREF color);
    void PaintSlider(HDC hdc);
    void OnMouseMove(POINT point);
    void OnLeftButtonDown(POINT point);
    void OnLeftButtonUp(POINT point);
    void OnMouseWheel(int delta, POINT screenPoint);
    void OnMouseButtonForBinding(OutputMouseButton button);
    void OnMouseButtonForActivationBinding(ActivationMouseButton button);
    void UpdateAutoClickerSpeedFromPoint(int x);
    void SetAutoClickerRunning(bool running);
    void UpdateAutoClickerTimer();
    void AutoClickerLoop();
    void PerformAutoClick();
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
    void BrowseMediaOutputFolder();
    void AnalyzeMediaUrl();
    void StartMediaDownload();
    void CancelMediaDownload();
    void FinishMediaThread();
    void ApplyMediaJobUpdate(const MediaDownloadJob& job);
    void StartUpdateCheck();
    void FinishUpdateThread();
    void ApplyUpdateCheckResult(const UpdateCheckResult& result);
    void StartUpdateInstall();
    std::wstring ResolveUpdatePackageUrl() const;
    bool DownloadUpdatePackage(const std::wstring& downloadUrl, std::filesystem::path& packagePath, std::wstring& errorMessage) const;
    bool CreateAndLaunchUpdateInstaller(const std::filesystem::path& packagePath, std::wstring& errorMessage) const;
    void ShowMediaFormatDropdown();
    void ShowMediaQualityDropdown();
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
    int Dips(int value) const;

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    ULONG_PTR gdiplusToken_ = 0;
    std::unique_ptr<Gdiplus::Bitmap> logo_;
    std::unique_ptr<Gdiplus::Bitmap> autoClickerIcon_;
    std::unique_ptr<Gdiplus::Bitmap> fileConverterIcon_;
    std::unique_ptr<Gdiplus::Bitmap> mediaDownloaderIcon_;
    std::unique_ptr<Gdiplus::Bitmap> allToolsIcon_;
    std::unique_ptr<Gdiplus::Bitmap> settingsIcon_;
    HHOOK keyboardHook_ = nullptr;
    HHOOK mouseHook_ = nullptr;
    Page currentPage_ = Page::Favorites;
    ToolKind currentTool_ = ToolKind::None;
    std::vector<ToolDefinition> tools_;
    AutoClickerState autoClicker_;
    FileConversionService fileConversionService_;
    MediaDownloadService mediaDownloadService_;
    std::vector<ConversionJob> conversionJobs_;
    ConversionOptions conversionOptions_;
    MediaDownloadOptions mediaDownloadOptions_;
    MediaDownloadJob mediaDownloadJob_;
    ExternalToolStatus mediaExternalTools_;
    UpdateChecker updateChecker_;
    std::vector<ImageFormat> supportedOutputFormats_;
    int selectedConversionJob_ = -1;
    std::wstring fileConverterSummary_;
    bool fileConverterConverting_ = false;
    std::thread conversionThread_;
    std::atomic_bool conversionCancelRequested_ = false;
    std::thread mediaThread_;
    std::atomic_bool mediaCancelRequested_ = false;
    std::thread updateThread_;
    std::thread autoClickThread_;
    std::mutex autoClickMutex_;
    std::condition_variable autoClickCondition_;
    std::atomic_bool autoClickThreadStop_ = true;
    std::atomic<int> autoClickerCps_ = 8;
    std::atomic<int> autoClickerOutputButton_ = static_cast<int>(OutputMouseButton::Left);
    bool mediaAnalyzing_ = false;
    bool mediaDownloading_ = false;
    std::wstring mediaStatusText_;
    UpdateCheckResult updateResult_;
    bool updateChecking_ = false;
    bool updateInstalling_ = false;
    bool hasUpdateResult_ = false;
    std::wstring updateInstallStatus_;
    SIZE savedWindowSize_ { 1060, 680 };
    bool savedWindowMaximized_ = false;

    RECT clientRect_ {};
    RECT headerRect_ {};
    RECT favoritesNavRect_ {};
    RECT allToolsNavRect_ {};
    RECT settingsNavRect_ {};
    RECT contentRect_ {};
    RECT backButtonRect_ {};
    RECT startStopButtonRect_ {};
    RECT activationKeyButtonRect_ {};
    RECT outputButtonButtonRect_ {};
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
    RECT settingsCheckUpdatesButtonRect_ {};
    RECT settingsDownloadUpdateButtonRect_ {};
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
    HBRUSH editBackgroundBrush_ = nullptr;
    HDC backBufferDc_ = nullptr;
    HBITMAP backBufferBitmap_ = nullptr;
    HBITMAP backBufferPreviousBitmap_ = nullptr;
    SIZE backBufferSize_ {};
    int hoverNavIndex_ = -1;
    int hoverToolIndex_ = -1;
    bool speedSliderDragging_ = false;
    bool converterQualityDragging_ = false;
    bool scrollBarDragging_ = false;
    int scrollBarDragOffsetY_ = 0;
    int scrollOffsetY_ = 0;
    int maxScrollOffsetY_ = 0;
    int scrollContentHeight_ = 0;
    bool fileConverterAdvancedOpen_ = false;
    bool mouseLeaveTracking_ = false;
    bool testButtonHeld_ = false;
    bool awaitingActivationKey_ = false;
    bool awaitingOutputButton_ = false;

    HFONT titleFont_ = nullptr;
    HFONT navFont_ = nullptr;
    HFONT headingFont_ = nullptr;
    HFONT bodyFont_ = nullptr;
    HFONT monospaceFont_ = nullptr;
    int dpi_ = 96;
};
