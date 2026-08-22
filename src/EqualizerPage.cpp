#include "EqualizerPage.h"

#include "EqualizerSetup.h"

#include <commdlg.h>
#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <atomic>
#include <cmath>
#include <cwchar>
#include <exception>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace
{
constexpr wchar_t kPageClassName[] = L"RexToolkit.EqualizerPage";
constexpr UINT kWorkerCompleteMessage = WM_APP + 310;
constexpr UINT_PTR kDeviceTimerId = 1;
constexpr UINT_PTR kApplyTimerId = 2;
constexpr UINT_PTR kSwitchTimerId = 3;
constexpr UINT_PTR kSmoothScrollTimerId = 4;
constexpr UINT_PTR kLivePreviewTimerId = 5;
constexpr UINT kDeviceTimerMs = 750;
constexpr UINT kApplyDelayMs = 170;
constexpr UINT kLivePreviewIntervalMs = 80;
constexpr UINT kSmoothScrollTickMs = 16;
constexpr int kHeadphoneVisibleRows = 8;
constexpr size_t kHeadphoneSearchResultLimit = 256;

// These IDs drive both the visible labels and the full-preset hit indexes.
constexpr std::array<const wchar_t*, 5> kSimplePresetIds {
    L"balanced", L"bass_plus", L"warm", L"bright", L"gaming" };

int ScaleDip(UINT dpi, int value)
{
    return MulDiv(value, static_cast<int>(dpi), 96);
}

bool HasArea(const RECT& rect)
{
    return rect.right > rect.left && rect.bottom > rect.top;
}

bool Contains(const RECT& rect, POINT point)
{
    return HasArea(rect) && PtInRect(&rect, point) != FALSE;
}

RECT Inset(RECT rect, int x, int y)
{
    InflateRect(&rect, -x, -y);
    return rect;
}

COLORREF Blend(COLORREF base, COLORREF tint, int percent)
{
    return rex::ui::BlendColor(base, tint, std::clamp(percent, 0, 100));
}

HFONT MakeFont(UINT dpi, int points, int weight, const wchar_t* face = L"Segoe UI")
{
    LOGFONTW font {};
    font.lfHeight = -MulDiv(points, static_cast<int>(dpi), 72);
    font.lfWeight = weight;
    font.lfQuality = CLEARTYPE_QUALITY;
    wcsncpy_s(font.lfFaceName, face, _TRUNCATE);
    return CreateFontIndirectW(&font);
}

void FillRectColor(HDC hdc, const RECT& rect, COLORREF color)
{
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(hdc, &rect, brush);
    DeleteObject(brush);
}

void FillRounded(HDC hdc, const RECT& rect, int radius, COLORREF color)
{
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_NULL, 0, color);
    HGDIOBJ oldBrush = SelectObject(hdc, brush);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    RoundRect(hdc, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void StrokeRounded(HDC hdc, const RECT& rect, int radius, COLORREF color, int width = 1)
{
    HPEN pen = CreatePen(PS_SOLID, width, color);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    RoundRect(hdc, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

void Text(HDC hdc, const std::wstring& value, RECT rect, HFONT font, COLORREF color, UINT format)
{
    HGDIOBJ oldFont = SelectObject(hdc, font);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, color);
    DrawTextW(hdc, value.c_str(), static_cast<int>(value.size()), &rect, format | DT_NOPREFIX);
    SelectObject(hdc, oldFont);
}

std::wstring FormatDb(double value, bool includePlus = false)
{
    wchar_t text[32] {};
    if (includePlus && value > 0.005)
    {
        swprintf_s(text, L"+%.1f dB", value);
    }
    else
    {
        swprintf_s(text, L"%.1f dB", value);
    }
    return text;
}

std::wstring FilterTypeName(rex::equalizer::FilterType type)
{
    using rex::equalizer::FilterType;
    switch (type)
    {
    case FilterType::LowShelf: return L"Low shelf";
    case FilterType::HighShelf: return L"High shelf";
    case FilterType::LowPass: return L"Low pass";
    case FilterType::HighPass: return L"High pass";
    case FilterType::Notch: return L"Notch";
    case FilterType::Peaking: return L"Peak";
    }
    return L"Peak";
}

rex::equalizer::FilterType NextFilterType(rex::equalizer::FilterType type)
{
    using rex::equalizer::FilterType;
    switch (type)
    {
    case FilterType::Peaking: return FilterType::LowShelf;
    case FilterType::LowShelf: return FilterType::HighShelf;
    case FilterType::HighShelf: return FilterType::Notch;
    case FilterType::Notch: return FilterType::LowPass;
    case FilterType::LowPass: return FilterType::HighPass;
    case FilterType::HighPass: return FilterType::Peaking;
    }
    return FilterType::Peaking;
}

std::optional<std::filesystem::path> OpenPath(HWND owner, const wchar_t* filter)
{
    wchar_t path[32768] {};
    OPENFILENAMEW dialog {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(std::size(path));
    dialog.lpstrFilter = filter;
    dialog.nFilterIndex = 1;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) return std::nullopt;
    return std::filesystem::path(path);
}

std::optional<std::filesystem::path> SavePath(HWND owner)
{
    wchar_t path[32768] {};
    OPENFILENAMEW dialog {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(std::size(path));
    dialog.lpstrFilter = L"Rex Equalizer profile (*.rexeq)\0*.rexeq\0All files (*.*)\0*.*\0\0";
    dialog.lpstrDefExt = L"rexeq";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&dialog)) return std::nullopt;
    return std::filesystem::path(path);
}

bool PutClipboardText(HWND owner, const std::wstring& value)
{
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    const size_t bytes = (value.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory)
    {
        CloseClipboard();
        return false;
    }
    void* destination = GlobalLock(memory);
    memcpy(destination, value.c_str(), bytes);
    GlobalUnlock(memory);
    if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
    CloseClipboard();
    return true;
}
}

class EqualizerPage::Impl
{
public:
    bool Create(HINSTANCE instance, HWND parent);
    void Destroy();
    void Shutdown();
    void SetVisible(bool visible);
    void SetBounds(const RECT& bounds, UINT dpi);
    void SetTheme(const EqualizerTheme& theme);
    void SetBetaFeaturesEnabled(bool enabled);
    void SetBackgroundPainter(std::function<void(HDC, const RECT&)> painter);
    HWND WindowHandle() const { return window_; }

    bool IsInitialized() const { return initialized_; }
    bool IsEnabled() const { return initialized_ && service_.CurrentDeviceSettings().enabled; }
    bool TrayControlsEnabled() const { return initialized_ && service_.Settings().trayControlsEnabled; }
    bool RememberPerDevice() const { return initialized_ && service_.Settings().rememberPerDevice; }
    bool AutomaticallyApplyDeviceProfile() const { return initialized_ && service_.Settings().automaticallyApplyDeviceProfile; }
    bool EnableOnStartup() const { return initialized_ && service_.Settings().enableOnStartup; }
    bool PreventClipping() const { return initialized_ && service_.CurrentDeviceSettings().preventClipping; }
    bool ShowTechnicalControls() const { return initialized_ && service_.Settings().showTechnicalControls; }
    std::wstring CurrentPresetId() const;
    std::wstring CurrentOutputName() const;
    std::wstring CurrentHeadphoneName() const;
    std::wstring HeadphoneDatabaseVersion() const;

    void ToggleEnabled();
    void SelectPreset(const std::wstring& id);
    void SetRememberPerDevice(bool value);
    void SetAutomaticallyApplyDeviceProfile(bool value);
    void SetEnableOnStartup(bool value);
    void SetPreventClipping(bool value);
    void SetTrayControlsEnabled(bool value);
    void SetShowTechnicalControls(bool value);
    void OpenAdvanced(bool diagnostics);
    void BeginBackendSetup();
    void BeginProfileUpdate();

private:
    enum class Action
    {
        None,
        Toggle,
        Output,
        Headphone,
        Setup,
        Advanced,
        Preset,
        Preference,
        PreferenceValue,
        Mode,
        GraphicBand,
        AutomaticPreamp,
        ManualPreamp,
        FilterEnabled,
        FilterType,
        FilterFrequency,
        FilterGain,
        FilterQ,
        FilterRemove,
        FilterAdd,
        ImportProfile,
        ExportProfile,
        ImportMeasurement,
        ResetRecommended,
        ResetCustom,
        ResetDevice,
        ProfileUpdate,
        Reinitialize,
        OpenConfigurator,
        OpenDataFolder,
        CopyDiagnostics
    };

    struct HitTarget
    {
        RECT rect {};
        Action action = Action::None;
        int index = 0;
    };

    enum class WorkerKind { None, Profile, Index, BackendSetup };
    struct WorkerResult
    {
        WorkerKind kind = WorkerKind::None;
        bool success = false;
        DWORD exitCode = 0;
        std::wstring message;
        std::wstring profileId;
        std::wstring displayName;
        rex::equalizer::EqualizerProfile profile;
    };

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK SearchEditProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK NumericEditProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    void RecreateFonts();
    void Layout();
    void Paint(HDC target);
    void PaintContent(HDC hdc);
    int PaintSimple(HDC hdc);
    void PaintAdvanced(HDC hdc, int& y);
    void PaintGraph(HDC hdc, const RECT& rect);
    void PaintDropdowns(HDC hdc);
    void PaintScrollbar(HDC hdc);
    void PaintPanel(HDC hdc, const RECT& rect, bool accent = false);
    void PaintLabel(HDC hdc, const std::wstring& label, const RECT& rect, bool selected = false);
    void PaintButton(HDC hdc, const HitTarget& target, const std::wstring& label, bool primary = false, bool active = false, bool enabled = true);
    void AddHit(const RECT& rect, Action action, int index = 0);
    HitTarget HitAt(POINT point) const;
    void OnClick(POINT point);
    bool ResetSliderAt(POINT point);
    void OnMouseMove(POINT point, WPARAM keys);
    void OnWheel(int delta, POINT point);
    void OnEditChanged();
    void SyncHeadphoneEditFromSelection();
    void UpdateChildBounds();
    void UpdateDeviceTimer();
    void ClampScroll();
    void ScrollToOffset(int offset, bool animate);
    void StopSmoothScroll();
    bool StepSmoothScroll();
    int SetupPanelHeight(int contentWidth) const;
    int SimpleContentBottom(int contentWidth) const;
    int EstimatedContentHeight(int contentWidth) const;
    int PresetColumnCount(int contentWidth) const;
    int VisibleOutputDeviceRows() const;
    void EnsureSelectedOutputVisible();
    void ApplyNow(bool persistSettings = true);
    void QueueApply();
    void QueueLivePreview();
    void FinishLivePreview();
    void BeginWorkerProfile(const rex::equalizer::HeadphoneProfileSummary& summary);
    void BeginSetupWorker(HANDLE process, const std::wstring& statusMessage);
    void PublishWorkerResult(WorkerResult result) noexcept;
    void PublishWorkerFailure(WorkerKind kind, const wchar_t* message) noexcept;
    void CompleteWorker();
    void BeginNumericEdit(Action action, int index, const RECT& rect, double value);
    void CommitNumericEdit();
    void ImportProfile();
    void ExportProfile();
    void ImportMeasurement();
    void ShowMessage(const std::wstring& message, bool error = false);
    rex::ui::Palette Palette() const;

    HINSTANCE instance_ = nullptr;
    HWND parent_ = nullptr;
    HWND window_ = nullptr;
    HWND headphoneEdit_ = nullptr;
    HWND numericEdit_ = nullptr;
    WNDPROC searchOriginalProc_ = nullptr;
    WNDPROC numericOriginalProc_ = nullptr;
    UINT dpi_ = 96;
    EqualizerTheme theme_;
    std::function<void(HDC, const RECT&)> backgroundPainter_;
    RECT parentBounds_ {};
    rex::equalizer::EqualizerService service_;
    bool initialized_ = false;
    bool visible_ = false;
    std::atomic<bool> shuttingDown_ { false };
    bool busy_ = false;
    bool suppressSearchChange_ = false;
    bool outputOpen_ = false;
    bool searchOpen_ = false;
    bool diagnosticsOpen_ = false;
    bool betaFeaturesEnabled_ = false;
    int outputFirstIndex_ = 0;
    int headphoneFirstIndex_ = 0;
    int scrollOffset_ = 0;
    int smoothScrollTargetOffset_ = 0;
    int scrollWheelDeltaRemainder_ = 0;
    bool smoothScrollActive_ = false;
    int contentHeight_ = 0;
    int hoveredHit_ = -1;
    int pressedHit_ = -1;
    int draggingPreference_ = -1;
    bool draggingManualPreamp_ = false;
    bool manualPreampDragMoved_ = false;
    int manualPreampDragStartX_ = 0;
    double manualPreampDragStartDb_ = 0.0;
    RECT manualPreampDragRect_ {};
    int draggingGraphicBand_ = -1;
    bool livePreviewPending_ = false;
    bool livePreviewDirty_ = false;
    bool livePreviewTimerScheduled_ = false;
    Action numericAction_ = Action::None;
    int numericIndex_ = -1;
    RECT numericRect_ {};
    std::uint64_t deviceGeneration_ = 0;
    std::wstring statusMessage_;
    bool statusError_ = false;
    std::vector<HitTarget> hits_;
    std::vector<RECT> preferenceTracks_;
    std::array<RECT, 10> graphicTracks_ {};
    RECT outputRect_ {};
    RECT headphoneRect_ {};
    RECT outputDropdownRect_ {};
    RECT headphoneResultsRect_ {};
    RECT scrollTrack_ {};
    RECT scrollThumb_ {};
    std::vector<rex::equalizer::HeadphoneProfileSummary> searchResults_;
    std::thread worker_;
    std::mutex workerMutex_;
    WorkerResult workerResult_;
    HFONT headingFont_ = nullptr;
    HFONT sectionFont_ = nullptr;
    HFONT bodyFont_ = nullptr;
    HFONT smallFont_ = nullptr;
    HFONT monoFont_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    HDC bufferDc_ = nullptr;
    HBITMAP bufferBitmap_ = nullptr;
    HBITMAP oldBufferBitmap_ = nullptr;
    SIZE bufferSize_ {};
    rex::ui::SwitchAnimationState enabledSwitch_;
    rex::ui::SwitchAnimationState preampSwitch_;
};

EqualizerPage::EqualizerPage() : impl_(std::make_unique<Impl>()) {}
EqualizerPage::~EqualizerPage() = default;
bool EqualizerPage::Create(HINSTANCE instance, HWND parent) { return impl_->Create(instance, parent); }
void EqualizerPage::Destroy() { impl_->Destroy(); }
void EqualizerPage::Shutdown() { impl_->Shutdown(); }
void EqualizerPage::SetVisible(bool visible) { impl_->SetVisible(visible); }
void EqualizerPage::SetBounds(const RECT& bounds, UINT dpi) { impl_->SetBounds(bounds, dpi); }
void EqualizerPage::SetTheme(const EqualizerTheme& theme) { impl_->SetTheme(theme); }
void EqualizerPage::SetBetaFeaturesEnabled(bool enabled) { impl_->SetBetaFeaturesEnabled(enabled); }
void EqualizerPage::SetBackgroundPainter(std::function<void(HDC, const RECT&)> painter) { impl_->SetBackgroundPainter(std::move(painter)); }
HWND EqualizerPage::WindowHandle() const { return impl_->WindowHandle(); }
bool EqualizerPage::IsInitialized() const { return impl_->IsInitialized(); }
bool EqualizerPage::IsEnabled() const { return impl_->IsEnabled(); }
bool EqualizerPage::TrayControlsEnabled() const { return impl_->TrayControlsEnabled(); }
bool EqualizerPage::RememberPerDevice() const { return impl_->RememberPerDevice(); }
bool EqualizerPage::AutomaticallyApplyDeviceProfile() const { return impl_->AutomaticallyApplyDeviceProfile(); }
bool EqualizerPage::EnableOnStartup() const { return impl_->EnableOnStartup(); }
bool EqualizerPage::PreventClipping() const { return impl_->PreventClipping(); }
bool EqualizerPage::ShowTechnicalControls() const { return impl_->ShowTechnicalControls(); }
std::wstring EqualizerPage::CurrentPresetId() const { return impl_->CurrentPresetId(); }
std::wstring EqualizerPage::CurrentOutputName() const { return impl_->CurrentOutputName(); }
std::wstring EqualizerPage::CurrentHeadphoneName() const { return impl_->CurrentHeadphoneName(); }
std::wstring EqualizerPage::HeadphoneDatabaseVersion() const { return impl_->HeadphoneDatabaseVersion(); }
void EqualizerPage::ToggleEnabled() { impl_->ToggleEnabled(); }
void EqualizerPage::SelectPreset(const std::wstring& id) { impl_->SelectPreset(id); }
void EqualizerPage::SetRememberPerDevice(bool value) { impl_->SetRememberPerDevice(value); }
void EqualizerPage::SetAutomaticallyApplyDeviceProfile(bool value) { impl_->SetAutomaticallyApplyDeviceProfile(value); }
void EqualizerPage::SetEnableOnStartup(bool value) { impl_->SetEnableOnStartup(value); }
void EqualizerPage::SetPreventClipping(bool value) { impl_->SetPreventClipping(value); }
void EqualizerPage::SetTrayControlsEnabled(bool value) { impl_->SetTrayControlsEnabled(value); }
void EqualizerPage::SetShowTechnicalControls(bool value) { impl_->SetShowTechnicalControls(value); }
void EqualizerPage::OpenAdvanced(bool diagnostics) { impl_->OpenAdvanced(diagnostics); }
void EqualizerPage::BeginBackendSetup() { impl_->BeginBackendSetup(); }
void EqualizerPage::BeginProfileUpdate() { impl_->BeginProfileUpdate(); }
LRESULT CALLBACK EqualizerPage::Impl::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    Impl* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<Impl*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->window_ = hwnd;
    }
    return self ? self->HandleMessage(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK EqualizerPage::Impl::SearchEditProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* self = static_cast<Impl*>(GetPropW(hwnd, L"RexEqualizerSearch"));
    if (self && ((message == WM_KEYDOWN && wParam == L'A' &&
                  (GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
                  (GetKeyState(VK_MENU) & 0x8000) == 0) ||
                 (message == WM_CHAR && wParam == 1)))
    {
        SendMessageW(hwnd, EM_SETSEL, 0, -1);
        return 0;
    }
    if (self && message == WM_MOUSEWHEEL)
    {
        SendMessageW(self->window_, message, wParam, lParam);
        return 0;
    }
    if (self && message == WM_KEYDOWN && wParam == VK_ESCAPE)
    {
        self->searchOpen_ = false;
        self->outputOpen_ = false;
        SetWindowTextW(hwnd, L"");
        SetFocus(self->window_);
        InvalidateRect(self->window_, nullptr, FALSE);
        return 0;
    }
    if (message == WM_SETCURSOR)
    {
        SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
        return TRUE;
    }
    const WNDPROC original = self ? self->searchOriginalProc_ : nullptr;
    if (message == WM_NCDESTROY)
    {
        RemovePropW(hwnd, L"RexEqualizerSearch");
    }
    return original
        ? CallWindowProcW(original, hwnd, message, wParam, lParam)
        : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK EqualizerPage::Impl::NumericEditProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* self = static_cast<Impl*>(GetPropW(hwnd, L"RexEqualizerPage"));
    if (self && message == WM_KEYDOWN)
    {
        if (wParam == VK_RETURN)
        {
            self->CommitNumericEdit();
            return 0;
        }
        if (wParam == VK_ESCAPE)
        {
            ShowWindow(hwnd, SW_HIDE);
            self->numericAction_ = Action::None;
            self->numericIndex_ = -1;
            SetFocus(self->window_);
            return 0;
        }
    }
    if (self && message == WM_KILLFOCUS)
    {
        self->CommitNumericEdit();
        return 0;
    }
    return self && self->numericOriginalProc_
        ? CallWindowProcW(self->numericOriginalProc_, hwnd, message, wParam, lParam)
        : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT EqualizerPage::Impl::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT paint {};
        HDC hdc = BeginPaint(window_, &paint);
        Paint(hdc);
        EndPaint(window_, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        Layout();
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    case WM_COMMAND:
        if (reinterpret_cast<HWND>(lParam) == headphoneEdit_ && HIWORD(wParam) == EN_CHANGE)
        {
            OnEditChanged();
            return 0;
        }
        return 0;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkColor(hdc, theme_.inputBackground);
        SetTextColor(hdc, theme_.textPrimary);
        SetBkMode(hdc, OPAQUE);
        return reinterpret_cast<LRESULT>(editBrush_);
    }
    case WM_LBUTTONDOWN:
    {
        SetCapture(window_);
        const POINT point { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        const HitTarget hit = HitAt(point);
        pressedHit_ = -1;
        for (size_t index = 0; index < hits_.size(); ++index)
        {
            if (hits_[index].action == hit.action && hits_[index].index == hit.index &&
                EqualRect(&hits_[index].rect, &hit.rect))
            {
                pressedHit_ = static_cast<int>(index);
                break;
            }
        }
        OnClick(point);
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONDBLCLK:
    {
        const POINT point { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (ResetSliderAt(point)) return 0;
        break;
    }
    case WM_MOUSEMOVE:
        OnMouseMove({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }, wParam);
        return 0;
    case WM_MOUSELEAVE:
        if (hoveredHit_ != -1)
        {
            hoveredHit_ = -1;
            InvalidateRect(window_, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
    {
        const bool editManualPreamp = draggingManualPreamp_ && !manualPreampDragMoved_ &&
            initialized_ && !busy_ && !service_.CurrentDeviceSettings().preventClipping;
        const RECT editRect = manualPreampDragRect_;
        const double editValue = initialized_
            ? service_.CurrentDeviceSettings().manualPreampDb
            : 0.0;
        const bool wasDragging = draggingPreference_ >= 0 || draggingManualPreamp_ ||
            draggingGraphicBand_ >= 0;
        draggingPreference_ = -1;
        draggingManualPreamp_ = false;
        manualPreampDragMoved_ = false;
        draggingGraphicBand_ = -1;
        pressedHit_ = -1;
        if (wasDragging) FinishLivePreview();
        if (GetCapture() == window_) ReleaseCapture();
        InvalidateRect(window_, nullptr, FALSE);
        if (editManualPreamp)
        {
            BeginNumericEdit(Action::ManualPreamp, -1, editRect, editValue);
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (draggingPreference_ >= 0 || draggingManualPreamp_ ||
            draggingGraphicBand_ >= 0 || livePreviewDirty_)
        {
            draggingPreference_ = -1;
            draggingManualPreamp_ = false;
            manualPreampDragMoved_ = false;
            draggingGraphicBand_ = -1;
            pressedHit_ = -1;
            FinishLivePreview();
            InvalidateRect(window_, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEWHEEL:
    {
        POINT point { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(window_, &point);
        OnWheel(GET_WHEEL_DELTA_WPARAM(wParam), point);
        return 0;
    }
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            outputOpen_ = false;
            searchOpen_ = false;
            SetFocus(window_);
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_SPACE && !busy_)
        {
            ToggleEnabled();
            return 0;
        }
        break;
    case WM_TIMER:
        if (wParam == kDeviceTimerId && initialized_)
        {
            const std::uint64_t generation = service_.DeviceChangeGeneration();
            if (generation != deviceGeneration_)
            {
                deviceGeneration_ = generation;
                std::wstring errorMessage;
                service_.RefreshDevices(errorMessage);
                SyncHeadphoneEditFromSelection();
                if (service_.Settings().automaticallyApplyDeviceProfile)
                {
                    ApplyNow();
                }
                else if (!errorMessage.empty())
                {
                    ShowMessage(errorMessage, true);
                }
                Layout();
                InvalidateRect(window_, nullptr, FALSE);
            }
            return 0;
        }
        if (wParam == kApplyTimerId)
        {
            KillTimer(window_, kApplyTimerId);
            ApplyNow();
            return 0;
        }
        if (wParam == kLivePreviewTimerId)
        {
            KillTimer(window_, kLivePreviewTimerId);
            livePreviewTimerScheduled_ = false;
            if (livePreviewPending_)
            {
                livePreviewPending_ = false;
                ApplyNow(false);
            }
            return 0;
        }
        if (wParam == kSwitchTimerId)
        {
            const bool enabledAnimating = rex::ui::StepSwitchAnimation(enabledSwitch_);
            const bool preampAnimating = rex::ui::StepSwitchAnimation(preampSwitch_);
            if (!enabledAnimating && !preampAnimating) KillTimer(window_, kSwitchTimerId);
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        if (wParam == kSmoothScrollTimerId)
        {
            if (!StepSmoothScroll()) KillTimer(window_, kSmoothScrollTimerId);
            return 0;
        }
        break;
    case kWorkerCompleteMessage:
        CompleteWorker();
        return 0;
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}
void EqualizerPage::Impl::PaintDropdowns(HDC hdc)
{
    if (outputOpen_)
    {
        rex::ui::PaintDropdownMenuBackground(hdc, outputDropdownRect_, dpi_, Palette());
        int top = outputDropdownRect_.top + ScaleDip(dpi_, 6);
        const int height = ScaleDip(dpi_, 40);
        RECT defaultRect {
            outputDropdownRect_.left + ScaleDip(dpi_, 6), top,
            outputDropdownRect_.right - ScaleDip(dpi_, 6), top + height
        };
        AddHit(defaultRect, Action::Output, 0);
        rex::ui::DropdownItemOptions defaultOptions;
        defaultOptions.selected = service_.Settings().followWindowsDefault;
        defaultOptions.hovered = hoveredHit_ == static_cast<int>(hits_.size()) - 1;
        rex::ui::PaintDropdownItem(
            hdc, defaultRect, L"Follow Windows default", bodyFont_, smallFont_, dpi_, Palette(), defaultOptions);
        top += ScaleDip(dpi_, 44);

        const int visibleRows = VisibleOutputDeviceRows();
        const int deviceCount = static_cast<int>(service_.OutputDevices().size());
        const int end = std::min(deviceCount, outputFirstIndex_ + visibleRows);
        for (int index = outputFirstIndex_; index < end; ++index)
        {
            const auto& device = service_.OutputDevices()[static_cast<size_t>(index)];
            RECT row { defaultRect.left, top, defaultRect.right, top + height };
            AddHit(row, Action::Output, index + 1);
            rex::ui::DropdownItemOptions options;
            options.selected = !service_.Settings().followWindowsDefault &&
                service_.Settings().selectedOutputId == device.id;
            options.hovered = hoveredHit_ == static_cast<int>(hits_.size()) - 1;
            options.enabled = device.connected;
            options.badge = device.isDefault ? L"Default" : (device.connected ? L"" : L"Disconnected");
            rex::ui::PaintDropdownItem(
                hdc, row, device.name, bodyFont_, smallFont_, dpi_, Palette(), options);
            top += ScaleDip(dpi_, 44);
        }

        if (deviceCount > visibleRows)
        {
            RECT footer {
                outputDropdownRect_.left + ScaleDip(dpi_, 14), top,
                outputDropdownRect_.right - ScaleDip(dpi_, 14), outputDropdownRect_.bottom - ScaleDip(dpi_, 4)
            };
            std::wostringstream range;
            range << L"Scroll for more outputs  |  "
                  << (outputFirstIndex_ + 1) << L"-" << end << L" of " << deviceCount;
            Text(hdc, range.str(), footer, smallFont_, theme_.textSecondary,
                DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        }
    }

    if (searchOpen_)
    {
        const int resultCount = static_cast<int>(searchResults_.size());
        const int rows = std::min(kHeadphoneVisibleRows, resultCount);
        headphoneFirstIndex_ = std::clamp(
            headphoneFirstIndex_, 0, std::max(0, resultCount - rows));
        if (rows > 0)
        {
            rex::ui::PaintDropdownMenuBackground(hdc, headphoneResultsRect_, dpi_, Palette());
            int top = headphoneResultsRect_.top + ScaleDip(dpi_, 6);
            const int end = std::min(resultCount, headphoneFirstIndex_ + rows);
            for (int index = headphoneFirstIndex_; index < end; ++index)
            {
                const auto& result = searchResults_[static_cast<size_t>(index)];
                RECT row {
                    headphoneResultsRect_.left + ScaleDip(dpi_, 6), top,
                    headphoneResultsRect_.right - ScaleDip(dpi_, 6), top + ScaleDip(dpi_, 50)
                };
                AddHit(row, Action::Headphone, index);
                const bool hovered = hoveredHit_ == static_cast<int>(hits_.size()) - 1;
                if (hovered) FillRounded(hdc, row, ScaleDip(dpi_, 7), Palette().dropdownHover);
                RECT name {
                    row.left + ScaleDip(dpi_, 12), row.top + ScaleDip(dpi_, 5),
                    row.right - ScaleDip(dpi_, 120), row.top + ScaleDip(dpi_, 27)
                };
                Text(hdc, result.DisplayName(), name, bodyFont_, theme_.textPrimary,
                    DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                RECT source {
                    name.left, name.bottom - ScaleDip(dpi_, 2),
                    name.right, row.bottom - ScaleDip(dpi_, 4)
                };
                const std::wstring sourceText = result.measurementSource.empty()
                    ? L"Recommended EQ available"
                    : L"Source: " + result.measurementSource;
                Text(hdc, sourceText, source, smallFont_, theme_.textSecondary,
                    DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                RECT badge {
                    row.right - ScaleDip(dpi_, 108), row.top + ScaleDip(dpi_, 10),
                    row.right - ScaleDip(dpi_, 10), row.bottom - ScaleDip(dpi_, 10)
                };
                Text(hdc, result.cached ? L"Ready" : L"Download", badge, smallFont_,
                    result.cached ? theme_.accent : theme_.textSecondary,
                    DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
                top += ScaleDip(dpi_, 56);
            }
            if (resultCount > rows)
            {
                RECT footer {
                    headphoneResultsRect_.left + ScaleDip(dpi_, 14), top,
                    headphoneResultsRect_.right - ScaleDip(dpi_, 14),
                    headphoneResultsRect_.bottom - ScaleDip(dpi_, 4)
                };
                std::wostringstream range;
                range << L"Scroll for more headphones  |  "
                      << (headphoneFirstIndex_ + 1) << L"-" << end
                      << L" of " << resultCount << L" shown";
                Text(hdc, range.str(), footer, smallFont_, theme_.textSecondary,
                    DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            }
        }
        else
        {
            rex::ui::PaintDropdownMenuBackground(hdc, headphoneResultsRect_, dpi_, Palette());
            Text(hdc,
                service_.HeadphoneDatabaseVersion() == L"Not downloaded"
                    ? L"Download the headphone directory in Advanced to search AutoEq profiles."
                    : L"No matching headphone profiles found.",
                Inset(headphoneResultsRect_, ScaleDip(dpi_, 14), ScaleDip(dpi_, 8)),
                smallFont_, theme_.textSecondary,
                DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        }
    }
}
void EqualizerPage::Impl::PaintScrollbar(HDC hdc)
{
    RECT client {};
    GetClientRect(window_, &client);
    if (contentHeight_ <= client.bottom) return;
    scrollTrack_ = {
        client.right - ScaleDip(dpi_, 10), ScaleDip(dpi_, 8),
        client.right - ScaleDip(dpi_, 4), client.bottom - ScaleDip(dpi_, 8)
    };
    FillRounded(hdc, scrollTrack_, ScaleDip(dpi_, 3), Blend(theme_.border, theme_.pageBackground, 32));
    const int trackHeight = scrollTrack_.bottom - scrollTrack_.top;
    const int thumbHeight = std::max(ScaleDip(dpi_, 44), trackHeight * static_cast<int>(client.bottom) / contentHeight_);
    const int range = std::max(1, contentHeight_ - static_cast<int>(client.bottom));
    const int position = (trackHeight - thumbHeight) * scrollOffset_ / range;
    scrollThumb_ = {
        scrollTrack_.left, scrollTrack_.top + position,
        scrollTrack_.right, scrollTrack_.top + position + thumbHeight
    };
    FillRounded(hdc, scrollThumb_, ScaleDip(dpi_, 3), Blend(theme_.textSecondary, theme_.pageBackground, 42));
}

void EqualizerPage::Impl::OnEditChanged()
{
    if (busy_ || suppressSearchChange_) return;
    wchar_t text[256] {};
    GetWindowTextW(headphoneEdit_, text, static_cast<int>(std::size(text)));
    const std::wstring query = text;
    searchResults_ = query.empty() ? std::vector<rex::equalizer::HeadphoneProfileSummary> {}
                                   : service_.SearchHeadphones(query, kHeadphoneSearchResultLimit);
    headphoneFirstIndex_ = 0;
    searchOpen_ = !query.empty();
    outputOpen_ = false;
    Layout();
    InvalidateRect(window_, nullptr, FALSE);
}

void EqualizerPage::Impl::SyncHeadphoneEditFromSelection()
{
    if (!initialized_ || !headphoneEdit_) return;
    const auto& settings = service_.CurrentDeviceSettings();
    const std::wstring selectedName = settings.headphoneProfileId.empty()
        ? L""
        : settings.headphoneDisplayName;
    wchar_t current[256] {};
    GetWindowTextW(headphoneEdit_, current, static_cast<int>(std::size(current)));
    if (selectedName != current)
    {
        suppressSearchChange_ = true;
        SetWindowTextW(headphoneEdit_, selectedName.c_str());
        suppressSearchChange_ = false;
    }
    SendMessageW(headphoneEdit_, EM_SETSEL, 0, 0);
    SendMessageW(headphoneEdit_, EM_SCROLLCARET, 0, 0);
    searchResults_.clear();
    headphoneFirstIndex_ = 0;
    searchOpen_ = false;
}

void EqualizerPage::Impl::OnClick(POINT point)
{
    const HitTarget hit = HitAt(point);
    if (hit.action == Action::None)
    {
        outputOpen_ = false;
        if (!Contains(headphoneRect_, point))
        {
            searchOpen_ = false;
            SetFocus(window_);
        }
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }

    const auto presetIds = rex::equalizer::EqualizerService::PresetIds();
    switch (hit.action)
    {
    case Action::Toggle:
        ToggleEnabled();
        break;
    case Action::Output:
        if (hit.index < 0)
        {
            outputOpen_ = !outputOpen_;
            searchOpen_ = false;
            if (outputOpen_)
            {
                EnsureSelectedOutputVisible();
            }
            Layout();
        }
        else
        {
            if (hit.index == 0)
            {
                service_.SelectOutput(rex::equalizer::kFollowDefaultOutputId);
            }
            else
            {
                const size_t index = static_cast<size_t>(hit.index - 1);
                if (index < service_.OutputDevices().size())
                {
                    service_.SelectOutput(service_.OutputDevices()[index].id);
                }
            }
            outputOpen_ = false;
            SyncHeadphoneEditFromSelection();
            ApplyNow();
        }
        break;
    case Action::Headphone:
        if (hit.index >= 0 && static_cast<size_t>(hit.index) < searchResults_.size())
        {
            BeginWorkerProfile(searchResults_[static_cast<size_t>(hit.index)]);
        }
        break;
    case Action::Setup:
        BeginBackendSetup();
        break;
    case Action::Advanced:
        if (!betaFeaturesEnabled_) break;
        service_.SetAdvancedVisible(!service_.Settings().advancedVisible);
        if (!service_.Settings().advancedVisible)
        {
            diagnosticsOpen_ = false;
            scrollOffset_ = 0;
        }
        else
        {
            std::wstring ignored;
            service_.Save(ignored);
        }
        Layout();
        InvalidateRect(window_, nullptr, FALSE);
        break;
    case Action::Preset:
        if (hit.index >= 0 && static_cast<size_t>(hit.index) < presetIds.size())
        {
            SelectPreset(presetIds[static_cast<size_t>(hit.index)]);
        }
        break;
    case Action::Preference:
        draggingPreference_ = hit.index;
        OnMouseMove(point, MK_LBUTTON);
        break;
    case Action::PreferenceValue:
        if (!busy_ && hit.index >= 0 && hit.index < 4)
        {
            const auto& settings = service_.CurrentDeviceSettings();
            const std::array<double, 4> values {
                settings.bassDb, settings.warmthDb, settings.presenceDb, settings.trebleDb
            };
            BeginNumericEdit(
                Action::PreferenceValue, hit.index, hit.rect,
                values[static_cast<size_t>(hit.index)]);
        }
        break;
    case Action::Mode:
        if (hit.index >= 0 && hit.index <= 2)
        {
            service_.SetEditorMode(static_cast<rex::equalizer::EditorMode>(hit.index));
            QueueApply();
            Layout();
            InvalidateRect(window_, nullptr, FALSE);
        }
        break;
    case Action::GraphicBand:
        draggingGraphicBand_ = hit.index;
        OnMouseMove(point, MK_LBUTTON);
        break;
    case Action::AutomaticPreamp:
        SetPreventClipping(!service_.CurrentDeviceSettings().preventClipping);
        break;
    case Action::ManualPreamp:
        if (!busy_ && !service_.CurrentDeviceSettings().preventClipping)
        {
            draggingManualPreamp_ = true;
            manualPreampDragMoved_ = false;
            manualPreampDragStartX_ = point.x;
            manualPreampDragStartDb_ = service_.CurrentDeviceSettings().manualPreampDb;
            manualPreampDragRect_ = hit.rect;
        }
        break;
    case Action::FilterEnabled:
    {
        auto& settings = service_.CurrentDeviceSettings();
        if (!settings.parametricOverrideActive) service_.ConvertCurrentProfileToCustom();
        if (static_cast<size_t>(hit.index) < service_.CurrentDeviceSettings().customFilters.size())
        {
            auto filter = service_.CurrentDeviceSettings().customFilters[static_cast<size_t>(hit.index)];
            filter.enabled = !filter.enabled;
            service_.SetCustomFilter(static_cast<size_t>(hit.index), filter);
            QueueApply();
        }
        break;
    }
    case Action::FilterType:
    {
        if (!service_.CurrentDeviceSettings().parametricOverrideActive) service_.ConvertCurrentProfileToCustom();
        if (static_cast<size_t>(hit.index) < service_.CurrentDeviceSettings().customFilters.size())
        {
            auto filter = service_.CurrentDeviceSettings().customFilters[static_cast<size_t>(hit.index)];
            filter.type = NextFilterType(filter.type);
            service_.SetCustomFilter(static_cast<size_t>(hit.index), filter);
            QueueApply();
        }
        break;
    }
    case Action::FilterFrequency:
    case Action::FilterGain:
    case Action::FilterQ:
    {
        if (!service_.CurrentDeviceSettings().parametricOverrideActive) service_.ConvertCurrentProfileToCustom();
        if (static_cast<size_t>(hit.index) < service_.CurrentDeviceSettings().customFilters.size())
        {
            const auto& filter = service_.CurrentDeviceSettings().customFilters[static_cast<size_t>(hit.index)];
            const double value = hit.action == Action::FilterFrequency ? filter.frequencyHz
                : hit.action == Action::FilterGain ? filter.gainDb : filter.q;
            BeginNumericEdit(hit.action, hit.index, hit.rect, value);
        }
        break;
    }
    case Action::FilterRemove:
        if (!service_.CurrentDeviceSettings().parametricOverrideActive && !service_.CurrentProfile().filters.empty())
        {
            service_.ConvertCurrentProfileToCustom();
        }
        service_.RemoveCustomFilter(static_cast<size_t>(hit.index));
        QueueApply();
        Layout();
        break;
    case Action::FilterAdd:
        if (!service_.CurrentDeviceSettings().parametricOverrideActive && !service_.CurrentProfile().filters.empty())
        {
            service_.ConvertCurrentProfileToCustom();
        }
        service_.AddCustomFilter();
        QueueApply();
        Layout();
        break;
    case Action::ImportProfile:
        ImportProfile();
        break;
    case Action::ExportProfile:
        ExportProfile();
        break;
    case Action::ImportMeasurement:
        ImportMeasurement();
        break;
    case Action::ResetRecommended:
    case Action::ResetCustom:
        service_.ResetCustomEq();
        ApplyNow();
        Layout();
        break;
    case Action::ResetDevice:
        if (MessageBoxW(window_, L"Reset the Equalizer settings assigned to this output?", L"Reset device EQ", MB_ICONQUESTION | MB_YESNO) == IDYES)
        {
            service_.ResetDeviceProfile();
            SyncHeadphoneEditFromSelection();
            ApplyNow();
            Layout();
        }
        break;
    case Action::ProfileUpdate:
        BeginProfileUpdate();
        break;
    case Action::Reinitialize:
    {
        std::wstring errorMessage;
        service_.ReinitializeBackend(errorMessage);
        if (!errorMessage.empty()) ShowMessage(errorMessage, true);
        else ApplyNow();
        break;
    }
    case Action::OpenConfigurator:
    {
        const auto path = service_.BackendConfiguratorPath();
        if (!path.empty()) ShellExecuteW(window_, L"open", path.c_str(), nullptr, path.parent_path().c_str(), SW_SHOWNORMAL);
        else ShowMessage(L"Equalizer APO Device Selector is not available.", true);
        break;
    }
    case Action::OpenDataFolder:
        ShellExecuteW(window_, L"open", service_.DataDirectory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case Action::CopyDiagnostics:
    {
        const auto status = service_.CurrentBackendStatus();
        const auto& profile = service_.CurrentProfile();
        std::wostringstream value;
        value << L"Backend: " << status.backendName << L" " << status.backendVersion << L"\r\n"
              << L"Output: " << status.endpointName << L"\r\n"
              << L"State: " << rex::equalizer::EqualizerService::BackendAvailabilityText(status.availability) << L"\r\n"
              << L"Sample rate: " << status.sampleRate << L"\r\nChannels: " << status.channelCount
              << L"\r\nProfile: " << (profile.name.empty() ? L"Custom EQ" : profile.name)
              << L"\r\nProfile version: " << (profile.profileVersion.empty() ? L"Local" : profile.profileVersion)
              << L"\r\nSource: " << (profile.source.empty() ? L"Rex's Toolkit" : profile.source)
              << L"\r\nSource URL: " << profile.sourceUrl
              << L"\r\nTarget: " << profile.targetCurveId
              << L"\r\nFilters: " << profile.filters.size()
              << L"\r\nPreamp: " << FormatDb(profile.preampDb)
              << L"\r\nAudio path: Windows shared-mode system effects; ASIO and exclusive-mode audio may bypass EQ."
              << L"\r\nLast error: " << status.lastError;
        PutClipboardText(window_, value.str());
        ShowMessage(L"Equalizer diagnostics copied.");
        break;
    }
    default:
        break;
    }
    InvalidateRect(window_, nullptr, FALSE);
}

bool EqualizerPage::Impl::ResetSliderAt(POINT point)
{
    const HitTarget hit = HitAt(point);
    static const std::array<std::wstring, 4> ids { L"bass", L"warmth", L"presence", L"treble" };
    if ((hit.action == Action::Preference || hit.action == Action::PreferenceValue) &&
        hit.index >= 0 &&
        static_cast<size_t>(hit.index) < ids.size())
    {
        service_.SetPreferenceValue(ids[static_cast<size_t>(hit.index)], 0.0);
    }
    else if (hit.action == Action::ManualPreamp &&
        !service_.CurrentDeviceSettings().preventClipping)
    {
        service_.SetManualPreamp(0.0);
    }
    else if (hit.action == Action::GraphicBand && hit.index >= 0 && hit.index < 10)
    {
        service_.SetGraphicBand(static_cast<size_t>(hit.index), 0.0);
    }
    else
    {
        return false;
    }

    draggingPreference_ = -1;
    draggingManualPreamp_ = false;
    manualPreampDragMoved_ = false;
    draggingGraphicBand_ = -1;
    ShowWindow(numericEdit_, SW_HIDE);
    numericAction_ = Action::None;
    numericIndex_ = -1;
    QueueApply();
    InvalidateRect(window_, nullptr, FALSE);
    return true;
}

void EqualizerPage::Impl::OnMouseMove(POINT point, WPARAM keys)
{
    if ((keys & MK_LBUTTON) && draggingManualPreamp_)
    {
        constexpr double snapDistanceDb = 0.3;
        const int pixelsPerDb = std::max(1, ScaleDip(dpi_, 18));
        const int deltaPixels = point.x - manualPreampDragStartX_;
        if (std::abs(deltaPixels) >= ScaleDip(dpi_, 2)) manualPreampDragMoved_ = true;
        const double deltaDb = static_cast<double>(deltaPixels) /
            static_cast<double>(pixelsPerDb);
        double value = std::clamp(manualPreampDragStartDb_ + deltaDb, -30.0, 6.0);
        value = std::round(value * 10.0) / 10.0;
        if (std::abs(value) <= snapDistanceDb) value = 0.0;
        if (std::abs(service_.CurrentDeviceSettings().manualPreampDb - value) >= 0.05)
        {
            service_.SetManualPreamp(value);
            QueueLivePreview();
            InvalidateRect(window_, nullptr, FALSE);
        }
        return;
    }
    if ((keys & MK_LBUTTON) && draggingPreference_ >= 0 &&
        static_cast<size_t>(draggingPreference_) < preferenceTracks_.size())
    {
        const RECT track = preferenceTracks_[static_cast<size_t>(draggingPreference_)];
        const double rawValue = rex::ui::SliderValueFromPoint(track, point.x, -6.0, 6.0);
        double value = std::round(rawValue * 10.0) / 10.0;
        if (std::abs(value) <= 0.3) value = 0.0;
        static const std::array<std::wstring, 4> ids { L"bass", L"warmth", L"presence", L"treble" };
        const auto& settings = service_.CurrentDeviceSettings();
        const std::array<double, 4> currentValues {
            settings.bassDb, settings.warmthDb, settings.presenceDb, settings.trebleDb
        };
        if (std::abs(currentValues[static_cast<size_t>(draggingPreference_)] - value) >= 0.05)
        {
            service_.SetPreferenceValue(
                ids[static_cast<size_t>(draggingPreference_)], value);
            QueueLivePreview();
            InvalidateRect(window_, nullptr, FALSE);
        }
        return;
    }
    if ((keys & MK_LBUTTON) && draggingGraphicBand_ >= 0 && draggingGraphicBand_ < 10)
    {
        const RECT track = graphicTracks_[static_cast<size_t>(draggingGraphicBand_)];
        const double normalized = std::clamp(
            static_cast<double>(point.y - track.top) / std::max(1L, track.bottom - track.top), 0.0, 1.0);
        double value = std::round((12.0 - normalized * 24.0) * 10.0) / 10.0;
        if (std::abs(value) <= 0.5) value = 0.0;
        if (std::abs(service_.CurrentDeviceSettings().graphicGains[
                static_cast<size_t>(draggingGraphicBand_)] - value) >= 0.05)
        {
            service_.SetGraphicBand(static_cast<size_t>(draggingGraphicBand_), value);
            QueueLivePreview();
            InvalidateRect(window_, nullptr, FALSE);
        }
        return;
    }

    TRACKMOUSEEVENT tracking {};
    tracking.cbSize = sizeof(tracking);
    tracking.dwFlags = TME_LEAVE;
    tracking.hwndTrack = window_;
    TrackMouseEvent(&tracking);

    const HitTarget hit = HitAt(point);
    int newHover = -1;
    for (size_t index = 0; index < hits_.size(); ++index)
    {
        if (hits_[index].action == hit.action && hits_[index].index == hit.index &&
            EqualRect(&hits_[index].rect, &hit.rect))
        {
            newHover = static_cast<int>(index);
            break;
        }
    }
    if (newHover != hoveredHit_)
    {
        hoveredHit_ = newHover;
        InvalidateRect(window_, nullptr, FALSE);
    }
    const LPCWSTR cursor = hit.action == Action::PreferenceValue
        ? IDC_IBEAM
        : (hit.action == Action::ManualPreamp
            ? IDC_SIZEWE
            : (hit.action == Action::None ? IDC_ARROW : IDC_HAND));
    SetCursor(LoadCursorW(nullptr, cursor));
}

void EqualizerPage::Impl::OnWheel(int delta, POINT point)
{
    if (searchOpen_ && (Contains(headphoneResultsRect_, point) || Contains(headphoneRect_, point)))
    {
        const int resultCount = static_cast<int>(searchResults_.size());
        const int rows = std::min(kHeadphoneVisibleRows, resultCount);
        const int maximumFirst = std::max(0, resultCount - rows);
        const int steps = std::max(1, std::abs(delta) / WHEEL_DELTA);
        headphoneFirstIndex_ = std::clamp(
            headphoneFirstIndex_ + (delta < 0 ? steps : -steps), 0, maximumFirst);
        Layout();
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }
    if (outputOpen_ && (Contains(outputDropdownRect_, point) || Contains(outputRect_, point)))
    {
        const int visibleRows = VisibleOutputDeviceRows();
        const int maximumFirst = std::max(0, static_cast<int>(service_.OutputDevices().size()) - visibleRows);
        const int steps = std::max(1, std::abs(delta) / WHEEL_DELTA);
        outputFirstIndex_ = std::clamp(
            outputFirstIndex_ + (delta < 0 ? steps : -steps), 0, maximumFirst);
        Layout();
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }
    if (outputOpen_ || searchOpen_)
    {
        outputOpen_ = false;
        searchOpen_ = false;
    }
    if (numericEdit_ && IsWindowVisible(numericEdit_))
    {
        CommitNumericEdit();
    }

    const int scrollStep = ScaleDip(dpi_, 72);
    const long long accumulatedDelta = static_cast<long long>(scrollWheelDeltaRemainder_) +
        static_cast<long long>(delta) * scrollStep;
    const int pixelDelta = static_cast<int>(accumulatedDelta / WHEEL_DELTA);
    scrollWheelDeltaRemainder_ = static_cast<int>(accumulatedDelta % WHEEL_DELTA);
    if (pixelDelta == 0)
    {
        return;
    }

    const int targetBase = smoothScrollActive_ ? smoothScrollTargetOffset_ : scrollOffset_;
    ScrollToOffset(targetBase - pixelDelta, true);
}

void EqualizerPage::Impl::BeginNumericEdit(Action action, int index, const RECT& rect, double value)
{
    numericAction_ = action;
    numericIndex_ = index;
    numericRect_ = rect;
    wchar_t text[64] {};
    swprintf_s(text, L"%.3f", value);
    SetWindowTextW(numericEdit_, text);
    SetWindowPos(numericEdit_, HWND_TOP, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, SWP_SHOWWINDOW);
    SetFocus(numericEdit_);
    SendMessageW(numericEdit_, EM_SETSEL, 0, -1);
}

void EqualizerPage::Impl::CommitNumericEdit()
{
    if (!IsWindowVisible(numericEdit_)) return;
    wchar_t text[64] {};
    GetWindowTextW(numericEdit_, text, static_cast<int>(std::size(text)));
    wchar_t* end = nullptr;
    const double value = wcstod(text, &end);
    bool valid = end && *end == L'\0' && std::isfinite(value);
    auto& filters = service_.CurrentDeviceSettings().customFilters;
    if (valid && numericAction_ == Action::PreferenceValue &&
        numericIndex_ >= 0 && numericIndex_ < 4)
    {
        if (value < -6.0 || value > 6.0)
        {
            ShowMessage(L"Fine tune values must be between -6 dB and +6 dB.", true);
        }
        else
        {
            static const std::array<std::wstring, 4> ids {
                L"bass", L"warmth", L"presence", L"treble"
            };
            service_.SetPreferenceValue(
                ids[static_cast<size_t>(numericIndex_)], std::round(value * 10.0) / 10.0);
            QueueApply();
        }
    }
    else if (valid && numericAction_ == Action::ManualPreamp)
    {
        if (value < -30.0 || value > 6.0)
        {
            ShowMessage(L"Manual preamp must be between -30 dB and +6 dB.", true);
        }
        else
        {
            const auto outputDevice = service_.CurrentOutputDevice();
            const double sampleRate = outputDevice && outputDevice->sampleRate > 0
                ? static_cast<double>(outputDevice->sampleRate)
                : 48000.0;
            const double safePreamp = rex::equalizer::AutoEqService::CalculateAutomaticPreamp(
                service_.CurrentProfile().filters, sampleRate);
            service_.SetManualPreamp(value);
            QueueApply();
            if (value > safePreamp + 0.05)
            {
                ShowMessage(
                    L"This manual preamp may clip. Lower it or turn Prevent clipping back on.",
                    true);
            }
        }
    }
    else if (valid && numericIndex_ >= 0 && static_cast<size_t>(numericIndex_) < filters.size())
    {
        auto filter = filters[static_cast<size_t>(numericIndex_)];
        if (numericAction_ == Action::FilterFrequency) filter.frequencyHz = value;
        else if (numericAction_ == Action::FilterGain) filter.gainDb = value;
        else if (numericAction_ == Action::FilterQ) filter.q = value;
        std::wstring errorMessage;
        if (rex::equalizer::AutoEqService::ValidateFilter(filter, errorMessage))
        {
            service_.SetCustomFilter(static_cast<size_t>(numericIndex_), filter);
            QueueApply();
        }
        else
        {
            ShowMessage(errorMessage, true);
        }
    }
    else if (!valid)
    {
        ShowMessage(L"Enter a valid numeric EQ value.", true);
    }
    ShowWindow(numericEdit_, SW_HIDE);
    numericAction_ = Action::None;
    numericIndex_ = -1;
    SetFocus(window_);
    InvalidateRect(window_, nullptr, FALSE);
}

void EqualizerPage::Impl::ImportProfile()
{
    const auto path = OpenPath(window_, L"Rex Equalizer profile (*.rexeq)\0*.rexeq\0All files (*.*)\0*.*\0\0");
    if (!path) return;
    std::wstring errorMessage;
    if (!service_.ImportProfile(*path, errorMessage)) ShowMessage(errorMessage, true);
    else ApplyNow();
}

void EqualizerPage::Impl::ExportProfile()
{
    const auto path = SavePath(window_);
    if (!path) return;
    std::wstring errorMessage;
    if (!service_.ExportCurrentProfile(*path, errorMessage)) ShowMessage(errorMessage, true);
    else ShowMessage(L"Equalizer profile exported.");
}

void EqualizerPage::Impl::ImportMeasurement()
{
    const auto path = OpenPath(window_, L"Frequency response data (*.csv;*.txt)\0*.csv;*.txt\0All files (*.*)\0*.*\0\0");
    if (!path) return;
    const std::wstring name = path->stem().wstring();
    std::wstring errorMessage;
    if (!service_.ImportMeasurement(*path, name, errorMessage)) ShowMessage(errorMessage, true);
    else ApplyNow();
}
void EqualizerPage::Impl::PaintGraph(HDC hdc, const RECT& rect)
{
    const auto& settings = service_.CurrentDeviceSettings();
    PaintPanel(hdc, rect);

    const int graphWidth = static_cast<int>(rect.right - rect.left);
    const bool compactHeader = graphWidth < ScaleDip(dpi_, 760);
    const int right = rect.right - ScaleDip(dpi_, 18);
    RECT clippingSwitch {
        right - ScaleDip(dpi_, 58), rect.top + ScaleDip(dpi_, 17),
        right, rect.top + ScaleDip(dpi_, 51)
    };
    RECT clippingLabel {
        clippingSwitch.left - ScaleDip(dpi_, compactHeader ? 76 : 116),
        rect.top + ScaleDip(dpi_, 12),
        clippingSwitch.left - ScaleDip(dpi_, 10),
        rect.top + ScaleDip(dpi_, 56)
    };
    const int separatorX = clippingLabel.left - ScaleDip(dpi_, 14);
    RECT preampValue {
        separatorX - ScaleDip(dpi_, compactHeader ? 92 : 102),
        rect.top + ScaleDip(dpi_, 14),
        separatorX - ScaleDip(dpi_, 12),
        rect.top + ScaleDip(dpi_, 54)
    };
    RECT preampLabel {
        preampValue.left - ScaleDip(dpi_, compactHeader ? 0 : 70),
        rect.top + ScaleDip(dpi_, 12),
        preampValue.left - ScaleDip(dpi_, compactHeader ? 0 : 10),
        rect.top + ScaleDip(dpi_, 56)
    };
    const int controlsLeft = compactHeader ? preampValue.left : preampLabel.left;

    RECT title {
        rect.left + ScaleDip(dpi_, 18), rect.top + ScaleDip(dpi_, 12),
        controlsLeft - ScaleDip(dpi_, 16), rect.top + ScaleDip(dpi_, 38)
    };
    Text(hdc, L"Modeled final EQ response", title, sectionFont_, theme_.textPrimary,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    RECT note { title.left, title.bottom, title.right, title.bottom + ScaleDip(dpi_, 20) };
    Text(hdc, L"Based on the active filters. This is not a live microphone measurement.", note, smallFont_, theme_.textSecondary,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    if (!compactHeader)
    {
        Text(hdc, L"Preamp", preampLabel, smallFont_, theme_.textSecondary,
            DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    }
    if (!settings.preventClipping)
    {
        AddHit(preampValue, Action::ManualPreamp);
        const int hitIndex = static_cast<int>(hits_.size()) - 1;
        FillRounded(hdc, preampValue, ScaleDip(dpi_, 7), theme_.inputBackground);
        StrokeRounded(
            hdc, preampValue, ScaleDip(dpi_, 7),
            hoveredHit_ == hitIndex ? Blend(theme_.border, theme_.accent, 64) : theme_.border);
    }
    Text(hdc, FormatDb(service_.CurrentProfile().preampDb), preampValue, monoFont_,
        settings.preventClipping ? theme_.accent : theme_.textPrimary,
        DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    HPEN separatorPen = CreatePen(PS_SOLID, 1, theme_.border);
    HGDIOBJ oldSeparatorPen = SelectObject(hdc, separatorPen);
    MoveToEx(hdc, separatorX, rect.top + ScaleDip(dpi_, 16), nullptr);
    LineTo(hdc, separatorX, rect.top + ScaleDip(dpi_, 52));
    SelectObject(hdc, oldSeparatorPen);
    DeleteObject(separatorPen);

    Text(hdc, compactHeader ? L"Clipping" : L"Prevent clipping", clippingLabel,
        smallFont_, theme_.textSecondary, DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    preampSwitch_.bounds = clippingSwitch;
    AddHit(clippingSwitch, Action::AutomaticPreamp);
    const int switchHitIndex = static_cast<int>(hits_.size()) - 1;
    rex::ui::PaintSwitch(
        hdc, clippingSwitch,
        preampSwitch_.initialized ? preampSwitch_.position : (settings.preventClipping ? 1.0f : 0.0f),
        dpi_, Palette(), hoveredHit_ == switchHitIndex, pressedHit_ == switchHitIndex);

    RECT plot {
        rect.left + ScaleDip(dpi_, 54), rect.top + ScaleDip(dpi_, 68),
        rect.right - ScaleDip(dpi_, 20), rect.bottom - ScaleDip(dpi_, 34)
    };
    FillRounded(hdc, plot, ScaleDip(dpi_, 6), Blend(theme_.inputBackground, theme_.pageBackground, 18));

    const auto outputDevice = service_.CurrentOutputDevice();
    const double sampleRate = outputDevice && outputDevice->sampleRate > 0
        ? static_cast<double>(outputDevice->sampleRate)
        : 48000.0;
    const int plotWidth = static_cast<int>(std::max(1L, plot.right - plot.left));
    std::vector<std::pair<int, double>> responsePoints;
    responsePoints.reserve(static_cast<size_t>(plotWidth / 2 + 2));
    double minimumDb = 0.0;
    double maximumDb = 0.0;
    for (int pixel = 0; pixel <= plotWidth; pixel += 2)
    {
        const double normalized = static_cast<double>(pixel) / static_cast<double>(plotWidth);
        const double frequency = 20.0 * std::pow(1000.0, normalized);
        const double db = rex::equalizer::AutoEqService::ProfileResponseDb(
            service_.CurrentProfile(), frequency, sampleRate);
        if (!std::isfinite(db)) continue;
        responsePoints.emplace_back(pixel, db);
        minimumDb = std::min(minimumDb, db);
        maximumDb = std::max(maximumDb, db);
    }

    // Keep the familiar scale for ordinary profiles, then expand in readable
    // 6 dB increments instead of flattening out-of-range response values.
    const double desiredMinimum = std::min(-12.0, minimumDb);
    const double desiredMaximum = std::max(12.0, maximumDb);
    double gridStepDb = 6.0;
    while ((desiredMaximum - desiredMinimum) / gridStepDb > 8.0)
    {
        gridStepDb += 6.0;
    }
    const double lowerPadding = minimumDb < -12.0 ? 0.5 : 0.0;
    const double upperPadding = maximumDb > 12.0 ? 0.5 : 0.0;
    const double plotMinimumDb =
        std::floor((desiredMinimum - lowerPadding) / gridStepDb) * gridStepDb;
    const double plotMaximumDb =
        std::ceil((desiredMaximum + upperPadding) / gridStepDb) * gridStepDb;
    const double plotSpanDb = std::max(gridStepDb, plotMaximumDb - plotMinimumDb);

    for (double db = plotMinimumDb; db <= plotMaximumDb + 0.01; db += gridStepDb)
    {
        const double normalized = (plotMaximumDb - db) / plotSpanDb;
        const int y = plot.top + static_cast<int>(normalized * (plot.bottom - plot.top));
        const bool zeroLine = std::abs(db) < 0.01;
        HPEN pen = CreatePen(PS_SOLID, zeroLine ? 2 : 1,
            zeroLine ? Blend(theme_.border, theme_.textSecondary, 26) : theme_.border);
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        MoveToEx(hdc, plot.left, y, nullptr);
        LineTo(hdc, plot.right, y);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
        RECT label { rect.left + ScaleDip(dpi_, 8), y - ScaleDip(dpi_, 10), plot.left - ScaleDip(dpi_, 8), y + ScaleDip(dpi_, 10) };
        const int roundedDb = static_cast<int>(std::lround(db));
        Text(hdc, (roundedDb > 0 ? L"+" : L"") + std::to_wstring(roundedDb), label, smallFont_, theme_.textSecondary,
            DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    }

    const std::array<double, 4> frequencies { 20.0, 200.0, 2000.0, 20000.0 };
    const std::array<const wchar_t*, 4> labels { L"20 Hz", L"200 Hz", L"2 kHz", L"20 kHz" };
    for (size_t index = 0; index < frequencies.size(); ++index)
    {
        const double xNorm = std::log10(frequencies[index] / 20.0) / 3.0;
        const int x = plot.left + static_cast<int>(xNorm * (plot.right - plot.left));
        HPEN pen = CreatePen(PS_SOLID, 1, theme_.border);
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        MoveToEx(hdc, x, plot.top, nullptr);
        LineTo(hdc, x, plot.bottom);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
        RECT label { x - ScaleDip(dpi_, 38), plot.bottom + ScaleDip(dpi_, 5), x + ScaleDip(dpi_, 38), rect.bottom - ScaleDip(dpi_, 4) };
        Text(hdc, labels[index], label, smallFont_, theme_.textSecondary,
            DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }

    HPEN responsePen = CreatePen(PS_SOLID, ScaleDip(dpi_, 2), theme_.accent);
    HGDIOBJ oldPen = SelectObject(hdc, responsePen);
    bool started = false;
    for (const auto& [pixel, responseDb] : responsePoints)
    {
        const double db = std::clamp(
            responseDb, plotMinimumDb, plotMaximumDb);
        const int x = plot.left + pixel;
        const int y = plot.top + static_cast<int>(
            (plotMaximumDb - db) / plotSpanDb * (plot.bottom - plot.top));
        if (!started)
        {
            MoveToEx(hdc, x, y, nullptr);
            started = true;
        }
        else
        {
            LineTo(hdc, x, y);
        }
    }
    SelectObject(hdc, oldPen);
    DeleteObject(responsePen);
}

void EqualizerPage::Impl::PaintAdvanced(HDC hdc, int& y)
{
    RECT client {};
    GetClientRect(window_, &client);
    const int margin = ScaleDip(dpi_, 24);
    const int width = std::min(ScaleDip(dpi_, 1160), std::max(1, static_cast<int>(client.right) - margin * 2));
    const int left = std::max(margin, (static_cast<int>(client.right) - width) / 2);
    const int right = left + width;
    const auto& settings = service_.CurrentDeviceSettings();

    RECT heading { left, y, right, y + ScaleDip(dpi_, 34) };
    Text(hdc, L"Advanced equalizer", heading, headingFont_, theme_.textPrimary,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    y += ScaleDip(dpi_, 44);

    const std::array<std::pair<rex::equalizer::EditorMode, const wchar_t*>, 3> modes {{
        { rex::equalizer::EditorMode::Simple, L"Simple" },
        { rex::equalizer::EditorMode::Graphic, L"Graphic EQ" },
        { rex::equalizer::EditorMode::Parametric, L"Parametric EQ" }
    }};
    const int tabWidth = ScaleDip(dpi_, 150);
    for (size_t index = 0; index < modes.size(); ++index)
    {
        RECT tab {
            left + static_cast<int>(index) * (tabWidth + ScaleDip(dpi_, 8)),
            y,
            left + static_cast<int>(index) * (tabWidth + ScaleDip(dpi_, 8)) + tabWidth,
            y + ScaleDip(dpi_, 40)
        };
        HitTarget hit { tab, Action::Mode, static_cast<int>(index) };
        AddHit(tab, Action::Mode, static_cast<int>(index));
        PaintButton(hdc, hit, modes[index].second, false, settings.editorMode == modes[index].first, !busy_);
    }
    y += ScaleDip(dpi_, 54);

    if (settings.editorMode == rex::equalizer::EditorMode::Simple)
    {
        RECT presetPanel { left, y, right, y + ScaleDip(dpi_, 140) };
        PaintPanel(hdc, presetPanel);
        RECT presetTitle { presetPanel.left + ScaleDip(dpi_, 18), presetPanel.top + ScaleDip(dpi_, 10), presetPanel.right, presetPanel.top + ScaleDip(dpi_, 34) };
        Text(hdc, L"More sound preferences", presetTitle, sectionFont_, theme_.textPrimary,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        const std::array<int, 7> presetIndexes { 2, 5, 6, 8, 9, 10, 11 };
        const auto presetIds = rex::equalizer::EqualizerService::PresetIds();
        const int gap = ScaleDip(dpi_, 8);
        const int buttonWidth = (width - ScaleDip(dpi_, 36) - gap * 3) / 4;
        for (size_t item = 0; item < presetIndexes.size(); ++item)
        {
            const int column = static_cast<int>(item % 4);
            const int row = static_cast<int>(item / 4);
            const int presetIndex = presetIndexes[item];
            RECT button {
                presetPanel.left + ScaleDip(dpi_, 18) + column * (buttonWidth + gap),
                presetPanel.top + ScaleDip(dpi_, 46) + row * ScaleDip(dpi_, 42),
                presetPanel.left + ScaleDip(dpi_, 18) + column * (buttonWidth + gap) + buttonWidth,
                presetPanel.top + ScaleDip(dpi_, 82) + row * ScaleDip(dpi_, 42)
            };
            HitTarget hit { button, Action::Preset, presetIndex };
            AddHit(button, Action::Preset, presetIndex);
            PaintButton(hdc, hit,
                rex::equalizer::EqualizerService::PresetDisplayName(presetIds[static_cast<size_t>(presetIndex)]),
                false, settings.soundPreset == presetIds[static_cast<size_t>(presetIndex)], !busy_);
        }
        y = presetPanel.bottom + ScaleDip(dpi_, 16);
    }
    else if (settings.editorMode == rex::equalizer::EditorMode::Graphic)
    {
        RECT graphicPanel { left, y, right, y + ScaleDip(dpi_, 286) };
        PaintPanel(hdc, graphicPanel);
        const std::array<const wchar_t*, 10> labels {
            L"31", L"62", L"125", L"250", L"500", L"1k", L"2k", L"4k", L"8k", L"16k"
        };
        const int innerLeft = graphicPanel.left + ScaleDip(dpi_, 42);
        const int innerRight = graphicPanel.right - ScaleDip(dpi_, 42);
        const int stride = (innerRight - innerLeft) / 10;
        for (size_t index = 0; index < settings.graphicGains.size(); ++index)
        {
            const int center = innerLeft + stride / 2 + static_cast<int>(index) * stride;
            RECT valueRect { center - stride / 2, graphicPanel.top + ScaleDip(dpi_, 14), center + stride / 2, graphicPanel.top + ScaleDip(dpi_, 38) };
            Text(hdc, FormatDb(settings.graphicGains[index]), valueRect, smallFont_, theme_.textSecondary,
                DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            RECT track { center - ScaleDip(dpi_, 3), graphicPanel.top + ScaleDip(dpi_, 50), center + ScaleDip(dpi_, 3), graphicPanel.bottom - ScaleDip(dpi_, 52) };
            const double normalized = (12.0 - settings.graphicGains[index]) / 24.0;
            const int thumbY = track.top + static_cast<int>(normalized * (track.bottom - track.top));
            RECT thumb { center - ScaleDip(dpi_, 9), thumbY - ScaleDip(dpi_, 9), center + ScaleDip(dpi_, 9), thumbY + ScaleDip(dpi_, 9) };
            rex::ui::PaintSlider(hdc, track, thumb, dpi_, Palette(), !busy_);
            graphicTracks_[index] = track;
            AddHit({ center - ScaleDip(dpi_, 18), track.top - ScaleDip(dpi_, 10), center + ScaleDip(dpi_, 18), track.bottom + ScaleDip(dpi_, 10) },
                Action::GraphicBand, static_cast<int>(index));
            RECT labelRect { center - stride / 2, graphicPanel.bottom - ScaleDip(dpi_, 40), center + stride / 2, graphicPanel.bottom - ScaleDip(dpi_, 12) };
            Text(hdc, labels[index], labelRect, smallFont_, theme_.textPrimary,
                DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        }
        y = graphicPanel.bottom + ScaleDip(dpi_, 16);
    }
    else
    {
        const auto& filters = settings.parametricOverrideActive ? settings.customFilters : service_.CurrentProfile().filters;
        const int panelHeight = ScaleDip(dpi_, 76 + static_cast<int>(std::max<size_t>(1, filters.size())) * 48);
        RECT filterPanel { left, y, right, y + panelHeight };
        PaintPanel(hdc, filterPanel);
        RECT filterTitle { filterPanel.left + ScaleDip(dpi_, 18), filterPanel.top + ScaleDip(dpi_, 10), filterPanel.right - ScaleDip(dpi_, 150), filterPanel.top + ScaleDip(dpi_, 38) };
        Text(hdc, L"Parametric filters", filterTitle, sectionFont_, theme_.textPrimary,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        RECT addButton { filterPanel.right - ScaleDip(dpi_, 132), filterPanel.top + ScaleDip(dpi_, 9), filterPanel.right - ScaleDip(dpi_, 18), filterPanel.top + ScaleDip(dpi_, 41) };
        HitTarget addHit { addButton, Action::FilterAdd, 0 };
        AddHit(addButton, Action::FilterAdd);
        PaintButton(hdc, addHit, L"+ Add filter", false, false,
            filters.size() < static_cast<size_t>(service_.Settings().maximumPeqFilters));

        if (filters.empty())
        {
            RECT empty { filterPanel.left + ScaleDip(dpi_, 20), filterPanel.top + ScaleDip(dpi_, 48), filterPanel.right - ScaleDip(dpi_, 20), filterPanel.bottom - ScaleDip(dpi_, 12) };
            Text(hdc, L"No filters yet. Add a filter to begin building a custom EQ.", empty, smallFont_, theme_.textSecondary,
                DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        }
        for (size_t index = 0; index < filters.size(); ++index)
        {
            const auto& filter = filters[index];
            const int top = filterPanel.top + ScaleDip(dpi_, 50) + static_cast<int>(index) * ScaleDip(dpi_, 48);
            RECT row { filterPanel.left + ScaleDip(dpi_, 12), top, filterPanel.right - ScaleDip(dpi_, 12), top + ScaleDip(dpi_, 40) };
            FillRounded(hdc, row, ScaleDip(dpi_, 7), theme_.inputBackground);
            int x = row.left + ScaleDip(dpi_, 8);
            RECT enabledRect { x, row.top + ScaleDip(dpi_, 7), x + ScaleDip(dpi_, 26), row.bottom - ScaleDip(dpi_, 7) };
            AddHit(enabledRect, Action::FilterEnabled, static_cast<int>(index));
            FillRounded(hdc, enabledRect, ScaleDip(dpi_, 5), filter.enabled ? theme_.accentSoft : theme_.buttonBackground);
            Text(hdc, filter.enabled ? L"\u2713" : L"", enabledRect, bodyFont_, theme_.textPrimary, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            x = enabledRect.right + ScaleDip(dpi_, 8);
            const int typeWidth = ScaleDip(dpi_, 124);
            RECT typeRect { x, row.top + ScaleDip(dpi_, 4), x + typeWidth, row.bottom - ScaleDip(dpi_, 4) };
            AddHit(typeRect, Action::FilterType, static_cast<int>(index));
            PaintLabel(hdc, FilterTypeName(filter.type), Inset(typeRect, ScaleDip(dpi_, 8), 0), true);
            StrokeRounded(hdc, typeRect, ScaleDip(dpi_, 6), theme_.border);
            x = typeRect.right + ScaleDip(dpi_, 8);
            const int valueWidth = std::max(ScaleDip(dpi_, 84), (static_cast<int>(row.right) - x - ScaleDip(dpi_, 54)) / 3);
            const std::array<Action, 3> actions { Action::FilterFrequency, Action::FilterGain, Action::FilterQ };
            const std::array<std::wstring, 3> values {
                std::to_wstring(static_cast<int>(std::round(filter.frequencyHz))) + L" Hz",
                FormatDb(filter.gainDb, true),
                L"Q " + FormatDb(filter.q).substr(0, FormatDb(filter.q).find(L" dB"))
            };
            for (int field = 0; field < 3; ++field)
            {
                RECT value { x + field * (valueWidth + ScaleDip(dpi_, 8)), row.top + ScaleDip(dpi_, 4), x + field * (valueWidth + ScaleDip(dpi_, 8)) + valueWidth, row.bottom - ScaleDip(dpi_, 4) };
                AddHit(value, actions[static_cast<size_t>(field)], static_cast<int>(index));
                StrokeRounded(hdc, value, ScaleDip(dpi_, 6), theme_.border);
                Text(hdc, values[static_cast<size_t>(field)], Inset(value, ScaleDip(dpi_, 6), 0), monoFont_, theme_.textPrimary,
                    DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            }
            RECT remove { row.right - ScaleDip(dpi_, 36), row.top + ScaleDip(dpi_, 5), row.right - ScaleDip(dpi_, 4), row.bottom - ScaleDip(dpi_, 5) };
            AddHit(remove, Action::FilterRemove, static_cast<int>(index));
            Text(hdc, L"\u00d7", remove, sectionFont_, theme_.textSecondary, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        }
        y = filterPanel.bottom + ScaleDip(dpi_, 16);
    }

    RECT actions { left, y, right, y + ScaleDip(dpi_, 120) };
    PaintPanel(hdc, actions);
    const std::array<std::pair<Action, const wchar_t*>, 6> buttons {{
        { Action::ImportProfile, L"Import profile" },
        { Action::ExportProfile, L"Export profile" },
        { Action::ImportMeasurement, L"Import measurement" },
        { Action::ResetRecommended, L"Recommended EQ" },
        { Action::ResetCustom, L"Reset custom" },
        { Action::ResetDevice, L"Reset device" }
    }};
    const int actionGap = ScaleDip(dpi_, 8);
    const int actionWidth = (width - ScaleDip(dpi_, 36) - actionGap * 2) / 3;
    for (size_t index = 0; index < buttons.size(); ++index)
    {
        const int column = static_cast<int>(index % 3);
        const int row = static_cast<int>(index / 3);
        RECT button {
            actions.left + ScaleDip(dpi_, 18) + column * (actionWidth + actionGap),
            actions.top + ScaleDip(dpi_, 14) + row * ScaleDip(dpi_, 46),
            actions.left + ScaleDip(dpi_, 18) + column * (actionWidth + actionGap) + actionWidth,
            actions.top + ScaleDip(dpi_, 52) + row * ScaleDip(dpi_, 46)
        };
        HitTarget hit { button, buttons[index].first, 0 };
        AddHit(button, buttons[index].first);
        PaintButton(hdc, hit, buttons[index].second, false, false, !busy_);
    }
    y = actions.bottom + ScaleDip(dpi_, 16);

    if (service_.Settings().showTechnicalControls || diagnosticsOpen_)
    {
    RECT diagnostics { left, y, right, y + ScaleDip(dpi_, 258) };
    PaintPanel(hdc, diagnostics, diagnosticsOpen_);
    const auto backend = service_.CurrentBackendStatus();
    const auto& profile = service_.CurrentProfile();
    RECT diagTitle { diagnostics.left + ScaleDip(dpi_, 18), diagnostics.top + ScaleDip(dpi_, 12), diagnostics.right - ScaleDip(dpi_, 18), diagnostics.top + ScaleDip(dpi_, 38) };
    Text(hdc, L"Equalizer diagnostics", diagTitle, sectionFont_, theme_.textPrimary,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    std::wostringstream details;
    details << L"Backend: " << backend.backendName << L" " << backend.backendVersion << L"\r\n"
            << L"Output: " << (backend.endpointName.empty() ? L"None" : backend.endpointName) << L"\r\n"
            << L"State: " << rex::equalizer::EqualizerService::BackendAvailabilityText(backend.availability) << L"\r\n"
            << L"Format: " << backend.sampleRate << L" Hz / " << backend.channelCount << L" channels"
            << L"   Filters: " << backend.activeFilterCount << L"   Preamp: " << FormatDb(backend.preampDb) << L"\r\n"
            << L"Profile: " << (profile.name.empty() ? L"Custom EQ" : profile.name)
            << L"   Version: " << (profile.profileVersion.empty() ? L"Local" : profile.profileVersion) << L"\r\n"
            << L"Source: " << (profile.source.empty() ? L"Rex's Toolkit" : profile.source) << L"\r\n"
            << L"Latency: " << backend.latency << L"\r\n"
            << L"Shared-mode Windows audio is supported. ASIO and exclusive mode may bypass system effects.";
    if (!backend.lastError.empty()) details << L"\r\nLast error: " << backend.lastError;
    RECT detailRect { diagTitle.left, diagTitle.bottom + ScaleDip(dpi_, 4), diagnostics.right - ScaleDip(dpi_, 18), diagnostics.bottom - ScaleDip(dpi_, 58) };
    Text(hdc, details.str(), detailRect, monoFont_, theme_.textSecondary,
        DT_LEFT | DT_WORDBREAK | DT_TOP);

    const std::array<std::pair<Action, const wchar_t*>, 5> diagButtons {{
        { Action::Reinitialize, L"Reinitialize" },
        { Action::OpenConfigurator, L"Device selector" },
        { Action::OpenDataFolder, L"Data folder" },
        { Action::CopyDiagnostics, L"Copy details" },
        { Action::ProfileUpdate, L"Update profiles" }
    }};
    const int diagWidth = (width - ScaleDip(dpi_, 36) - ScaleDip(dpi_, 8) * 4) / 5;
    for (size_t index = 0; index < diagButtons.size(); ++index)
    {
        RECT button {
            diagnostics.left + ScaleDip(dpi_, 18) + static_cast<int>(index) * (diagWidth + ScaleDip(dpi_, 8)),
            diagnostics.bottom - ScaleDip(dpi_, 48),
            diagnostics.left + ScaleDip(dpi_, 18) + static_cast<int>(index) * (diagWidth + ScaleDip(dpi_, 8)) + diagWidth,
            diagnostics.bottom - ScaleDip(dpi_, 12)
        };
        HitTarget hit { button, diagButtons[index].first, 0 };
        AddHit(button, diagButtons[index].first);
        PaintButton(hdc, hit, diagButtons[index].second, false, false, !busy_);
    }
    y = diagnostics.bottom;
    }
    contentHeight_ = y + scrollOffset_ + ScaleDip(dpi_, 28);
}
void EqualizerPage::Impl::AddHit(const RECT& rect, Action action, int index)
{
    if (HasArea(rect)) hits_.push_back({ rect, action, index });
}

EqualizerPage::Impl::HitTarget EqualizerPage::Impl::HitAt(POINT point) const
{
    for (auto iterator = hits_.rbegin(); iterator != hits_.rend(); ++iterator)
    {
        if (Contains(iterator->rect, point)) return *iterator;
    }
    return {};
}

void EqualizerPage::Impl::PaintPanel(HDC hdc, const RECT& rect, bool accent)
{
    FillRounded(hdc, rect, ScaleDip(dpi_, 10), theme_.panelBackground);
    StrokeRounded(
        hdc, rect, ScaleDip(dpi_, 10),
        accent ? Blend(theme_.border, theme_.accent, 54) : theme_.border);
}

void EqualizerPage::Impl::PaintButton(
    HDC hdc,
    const HitTarget& target,
    const std::wstring& label,
    bool primary,
    bool active,
    bool enabled)
{
    int hitIndex = -1;
    for (size_t index = 0; index < hits_.size(); ++index)
    {
        if (hits_[index].action == target.action && hits_[index].index == target.index &&
            EqualRect(&hits_[index].rect, &target.rect))
        {
            hitIndex = static_cast<int>(index);
            break;
        }
    }
    rex::ui::ControlState state;
    state.hovered = hitIndex == hoveredHit_;
    state.pressed = hitIndex == pressedHit_;
    state.active = active;
    state.enabled = enabled;
    rex::ui::ButtonOptions options;
    options.cornerRadiusDip = 8;
    options.role = primary ? rex::ui::ButtonRole::Primary : rex::ui::ButtonRole::Neutral;
    rex::ui::PaintButton(hdc, target.rect, label, bodyFont_, dpi_, Palette(), state, options);
}

void EqualizerPage::Impl::PaintLabel(HDC hdc, const std::wstring& label, const RECT& rect, bool selected)
{
    Text(hdc, label, rect, selected ? sectionFont_ : bodyFont_,
        selected ? theme_.textPrimary : theme_.textSecondary,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

void EqualizerPage::Impl::Paint(HDC target)
{
    RECT client {};
    GetClientRect(window_, &client);
    const int width = client.right;
    const int height = client.bottom;
    if (width <= 0 || height <= 0) return;

    if (!bufferDc_ || bufferSize_.cx != width || bufferSize_.cy != height)
    {
        if (bufferDc_)
        {
            SelectObject(bufferDc_, oldBufferBitmap_);
            DeleteObject(bufferBitmap_);
            DeleteDC(bufferDc_);
        }
        bufferDc_ = CreateCompatibleDC(target);
        bufferBitmap_ = CreateCompatibleBitmap(target, width, height);
        oldBufferBitmap_ = static_cast<HBITMAP>(SelectObject(bufferDc_, bufferBitmap_));
        bufferSize_ = { width, height };
    }
    if (backgroundPainter_)
    {
        backgroundPainter_(bufferDc_, parentBounds_);
    }
    else
    {
        FillRectColor(bufferDc_, client, theme_.pageBackground);
    }
    PaintContent(bufferDc_);
    BitBlt(target, 0, 0, width, height, bufferDc_, 0, 0, SRCCOPY);
}

void EqualizerPage::Impl::PaintContent(HDC hdc)
{
    hits_.clear();
    preferenceTracks_.clear();
    graphicTracks_.fill({});
    Layout();

    RECT client {};
    GetClientRect(window_, &client);
    const int margin = ScaleDip(dpi_, 24);
    const int width = std::min(ScaleDip(dpi_, 1160), std::max(1, static_cast<int>(client.right) - margin * 2));
    const int left = std::max(margin, (static_cast<int>(client.right) - width) / 2);
    const int y = ScaleDip(dpi_, 16) - scrollOffset_;
    const int right = left + width;
    const int setupHeight = SetupPanelHeight(width);

    RECT setupPanel { left, y, right, y + setupHeight };
    PaintPanel(hdc, setupPanel, initialized_ && service_.CurrentDeviceSettings().enabled);

    RECT titleRect {
        setupPanel.left + ScaleDip(dpi_, 20), setupPanel.top + ScaleDip(dpi_, 14),
        setupPanel.right - ScaleDip(dpi_, 150), setupPanel.top + ScaleDip(dpi_, 45)
    };
    Text(hdc, L"System equalizer", titleRect, headingFont_, theme_.textPrimary,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    RECT subtitleRect {
        titleRect.left, titleRect.bottom - ScaleDip(dpi_, 2),
        setupPanel.right - ScaleDip(dpi_, 150), titleRect.bottom + ScaleDip(dpi_, 24)
    };
    Text(hdc, L"Applies system-wide to supported Windows audio output.", subtitleRect, smallFont_, theme_.textSecondary,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    RECT switchRect {
        setupPanel.right - ScaleDip(dpi_, 94), setupPanel.top + ScaleDip(dpi_, 22),
        setupPanel.right - ScaleDip(dpi_, 24), setupPanel.top + ScaleDip(dpi_, 54)
    };
    enabledSwitch_.bounds = switchRect;
    AddHit(switchRect, Action::Toggle);
    rex::ui::PaintSwitch(
        hdc, switchRect, enabledSwitch_.initialized ? enabledSwitch_.position : (IsEnabled() ? 1.0f : 0.0f),
        dpi_, Palette(), hoveredHit_ == static_cast<int>(hits_.size()) - 1,
        pressedHit_ == static_cast<int>(hits_.size()) - 1);
    RECT onLabel { switchRect.left - ScaleDip(dpi_, 44), switchRect.top, switchRect.left - ScaleDip(dpi_, 8), switchRect.bottom };
    Text(hdc, IsEnabled() ? L"ON" : L"OFF", onLabel, sectionFont_,
        IsEnabled() ? theme_.accent : theme_.textSecondary,
        DT_RIGHT | DT_SINGLELINE | DT_VCENTER);

    RECT outputLabel { outputRect_.left, outputRect_.top - ScaleDip(dpi_, 25), outputRect_.right, outputRect_.top - ScaleDip(dpi_, 4) };
    RECT headphoneLabel { headphoneRect_.left, headphoneRect_.top - ScaleDip(dpi_, 25), headphoneRect_.right, headphoneRect_.top - ScaleDip(dpi_, 4) };
    PaintLabel(hdc, L"Output", outputLabel, true);
    PaintLabel(hdc, L"Headphones", headphoneLabel, true);

    AddHit(outputRect_, Action::Output, -1);
    rex::ui::ControlState outputState;
    outputState.hovered = hoveredHit_ == static_cast<int>(hits_.size()) - 1;
    outputState.pressed = pressedHit_ == static_cast<int>(hits_.size()) - 1;
    outputState.enabled = initialized_ && !busy_;
    rex::ui::PaintDropdownField(
        hdc, outputRect_, CurrentOutputName(), bodyFont_, dpi_, Palette(), outputState, outputOpen_);

    FillRounded(hdc, headphoneRect_, ScaleDip(dpi_, 9), theme_.inputBackground);
    StrokeRounded(hdc, headphoneRect_, ScaleDip(dpi_, 9),
        searchOpen_ ? Blend(theme_.border, theme_.accent, 58) : theme_.border);
    RECT searchIcon {
        headphoneRect_.left + ScaleDip(dpi_, 12),
        headphoneRect_.top + (headphoneRect_.bottom - headphoneRect_.top - ScaleDip(dpi_, 18)) / 2,
        headphoneRect_.left + ScaleDip(dpi_, 30),
        headphoneRect_.top + (headphoneRect_.bottom - headphoneRect_.top + ScaleDip(dpi_, 18)) / 2
    };
    HPEN searchPen = CreatePen(PS_SOLID, std::max(1, ScaleDip(dpi_, 2)), theme_.textSecondary);
    HGDIOBJ previousPen = SelectObject(hdc, searchPen);
    HGDIOBJ previousBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    Ellipse(hdc, searchIcon.left, searchIcon.top, searchIcon.right - ScaleDip(dpi_, 5), searchIcon.bottom - ScaleDip(dpi_, 5));
    MoveToEx(hdc, searchIcon.right - ScaleDip(dpi_, 7), searchIcon.bottom - ScaleDip(dpi_, 7), nullptr);
    LineTo(hdc, searchIcon.right, searchIcon.bottom);
    SelectObject(hdc, previousBrush);
    SelectObject(hdc, previousPen);
    DeleteObject(searchPen);

    std::wstring backendMessage = statusMessage_;
    bool backendReady = false;
    if (initialized_)
    {
        const auto backend = service_.CurrentBackendStatus();
        backendReady = backend.availability == rex::equalizer::BackendAvailability::Ready;
        backendMessage = backendReady
            ? L"Ready for supported shared-mode Windows audio."
            : rex::equalizer::EqualizerService::BackendAvailabilityText(backend.availability);
    }
    if (backendMessage.empty()) backendMessage = initialized_ ? L"Ready." : L"Equalizer could not be initialized.";

    const bool showSetupButton = initialized_ && !backendReady;
    RECT backendStatus {
        setupPanel.left + ScaleDip(dpi_, 20), setupPanel.bottom - ScaleDip(dpi_, 34),
        setupPanel.right - ScaleDip(dpi_, showSetupButton ? 164 : 20), setupPanel.bottom - ScaleDip(dpi_, 8)
    };
    Text(hdc, (backendReady ? L"\u2713  " : L"!  ") + backendMessage, backendStatus, smallFont_,
        backendReady ? Blend(theme_.textSecondary, theme_.accent, 28) : (statusError_ ? theme_.danger : theme_.warning),
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    if (showSetupButton)
    {
        const auto backend = service_.CurrentBackendStatus();
        RECT setupButton {
            setupPanel.right - ScaleDip(dpi_, 144), setupPanel.bottom - ScaleDip(dpi_, 42),
            setupPanel.right - ScaleDip(dpi_, 20), setupPanel.bottom - ScaleDip(dpi_, 8)
        };
        HitTarget hit { setupButton, Action::Setup, 0 };
        AddHit(hit.rect, hit.action);
        std::wstring setupLabel = L"Set up";
        if (backend.availability == rex::equalizer::BackendAvailability::NotInstalled) setupLabel = L"Install engine";
        else if (backend.availability == rex::equalizer::BackendAvailability::DeviceNotConfigured) setupLabel = L"Select output";
        else if (backend.availability == rex::equalizer::BackendAvailability::ToolkitIncludeMissing) setupLabel = L"Finish setup";
        PaintButton(hdc, hit, setupLabel, true, false, !busy_);
    }

    int simpleBottom = setupPanel.bottom;
    if (initialized_) simpleBottom = PaintSimple(hdc);

    if (initialized_ && betaFeaturesEnabled_ && service_.Settings().advancedVisible)
    {
        int advancedY = simpleBottom + ScaleDip(dpi_, 24);
        PaintAdvanced(hdc, advancedY);
    }
    else
    {
        contentHeight_ = simpleBottom + scrollOffset_ + ScaleDip(dpi_, 28);
    }

    PaintDropdowns(hdc);
    PaintScrollbar(hdc);
}

int EqualizerPage::Impl::PaintSimple(HDC hdc)
{
    if (!initialized_) return ScaleDip(dpi_, 16);
    RECT client {};
    GetClientRect(window_, &client);
    const int margin = ScaleDip(dpi_, 24);
    const int width = std::min(ScaleDip(dpi_, 1160), std::max(1, static_cast<int>(client.right) - margin * 2));
    const int left = std::max(margin, (static_cast<int>(client.right) - width) / 2);
    const int right = left + width;
    const int y = ScaleDip(dpi_, 16) - scrollOffset_;
    const auto& settings = service_.CurrentDeviceSettings();
    int cursor = y + SetupPanelHeight(width) + ScaleDip(dpi_, 14);

    RECT correction { left, cursor, right, cursor + ScaleDip(dpi_, 72) };
    PaintPanel(hdc, correction, !settings.headphoneProfileId.empty());
    RECT check {
        correction.left + ScaleDip(dpi_, 18), correction.top + ScaleDip(dpi_, 17),
        correction.left + ScaleDip(dpi_, 56), correction.bottom - ScaleDip(dpi_, 17)
    };
    FillRounded(hdc, check, ScaleDip(dpi_, 8),
        settings.headphoneProfileId.empty() ? theme_.inputBackground : Blend(theme_.panelBackground, theme_.accentSoft, 52));
    Text(hdc, settings.headphoneProfileId.empty() ? L"-" : L"\u2713", check, sectionFont_,
        settings.headphoneProfileId.empty() ? theme_.textSecondary : theme_.accent,
        DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    RECT correctionTitle {
        check.right + ScaleDip(dpi_, 12), correction.top + ScaleDip(dpi_, 10),
        correction.right - ScaleDip(dpi_, 18), correction.top + ScaleDip(dpi_, 36)
    };
    Text(hdc,
        settings.headphoneProfileId.empty()
            ? L"No headphone correction selected"
            : L"Recommended headphone optimization enabled",
        correctionTitle, sectionFont_, theme_.textPrimary,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    RECT correctionName {
        correctionTitle.left, correctionTitle.bottom - ScaleDip(dpi_, 1),
        correction.right - ScaleDip(dpi_, 18), correction.bottom - ScaleDip(dpi_, 9)
    };
    const std::wstring correctionDetail = settings.headphoneProfileId.empty()
        ? L"Search above to download and apply a headphone profile."
        : settings.headphoneDisplayName;
    Text(hdc, correctionDetail, correctionName, smallFont_, theme_.textSecondary,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    cursor = correction.bottom + ScaleDip(dpi_, 16);

    RECT soundLabel { left, cursor, right, cursor + ScaleDip(dpi_, 26) };
    Text(hdc, L"Sound", soundLabel, sectionFont_, theme_.textPrimary,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    cursor = soundLabel.bottom + ScaleDip(dpi_, 4);
    const auto presetIds = rex::equalizer::EqualizerService::PresetIds();
    const int gap = ScaleDip(dpi_, 10);
    const int columns = PresetColumnCount(width);
    const int rows = (static_cast<int>(kSimplePresetIds.size()) + columns - 1) / columns;
    const int presetWidth = (width - gap * (columns - 1)) / columns;
    for (size_t index = 0; index < kSimplePresetIds.size(); ++index)
    {
        const auto preset = std::find(presetIds.begin(), presetIds.end(), kSimplePresetIds[index]);
        if (preset == presetIds.end()) continue;
        const int presetIndex = static_cast<int>(std::distance(presetIds.begin(), preset));
        const int column = static_cast<int>(index) % columns;
        const int row = static_cast<int>(index) / columns;
        RECT rect {
            left + column * (presetWidth + gap),
            cursor + row * ScaleDip(dpi_, 50),
            left + column * (presetWidth + gap) + presetWidth,
            cursor + row * ScaleDip(dpi_, 50) + ScaleDip(dpi_, 42)
        };
        HitTarget hit { rect, Action::Preset, presetIndex };
        AddHit(rect, Action::Preset, presetIndex);
        PaintButton(
            hdc, hit,
            rex::equalizer::EqualizerService::PresetDisplayName(kSimplePresetIds[index]),
            false, settings.soundPreset == kSimplePresetIds[index], !busy_);
    }
    cursor += rows * ScaleDip(dpi_, 42) + std::max(0, rows - 1) * ScaleDip(dpi_, 8) + ScaleDip(dpi_, 16);

    const bool stackPreferences = width < ScaleDip(dpi_, 680);
    const int preferenceHeight = ScaleDip(dpi_, stackPreferences ? 286 : 174);
    RECT preferencePanel { left, cursor, right, cursor + preferenceHeight };
    PaintPanel(hdc, preferencePanel);
    RECT preferenceTitle {
        preferencePanel.left + ScaleDip(dpi_, 20), preferencePanel.top + ScaleDip(dpi_, 10),
        preferencePanel.right - ScaleDip(dpi_, 20), preferencePanel.top + ScaleDip(dpi_, 36)
    };
    Text(hdc, L"Fine tune", preferenceTitle, sectionFont_, theme_.textPrimary,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    const std::array<const wchar_t*, 4> labels { L"Bass", L"Warmth", L"Presence", L"Treble" };
    const std::array<double, 4> values {
        settings.bassDb, settings.warmthDb, settings.presenceDb, settings.trebleDb
    };
    const int preferenceColumns = stackPreferences ? 1 : 2;
    const int columnGap = ScaleDip(dpi_, 20);
    const int columnWidth = (width - ScaleDip(dpi_, 40) - columnGap * (preferenceColumns - 1)) / preferenceColumns;
    const int rowStride = ScaleDip(dpi_, stackPreferences ? 58 : 62);
    for (int index = 0; index < 4; ++index)
    {
        const int column = index % preferenceColumns;
        const int row = index / preferenceColumns;
        const int x = preferencePanel.left + ScaleDip(dpi_, 20) + column * (columnWidth + columnGap);
        const int top = preferencePanel.top + ScaleDip(dpi_, 44) + row * rowStride;
        RECT valueRect {
            x + columnWidth - ScaleDip(dpi_, 74), top - ScaleDip(dpi_, 2),
            x + columnWidth, top + ScaleDip(dpi_, 26)
        };
        RECT labelRect { x, top, valueRect.left - ScaleDip(dpi_, 8), top + ScaleDip(dpi_, 22) };
        Text(hdc, labels[index], labelRect, bodyFont_, theme_.textPrimary,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        AddHit(valueRect, Action::PreferenceValue, index);
        const bool valueHovered = hoveredHit_ == static_cast<int>(hits_.size()) - 1;
        const bool valueEditing = numericAction_ == Action::PreferenceValue && numericIndex_ == index;
        FillRounded(hdc, valueRect, ScaleDip(dpi_, 7),
            valueHovered || valueEditing ? theme_.panelHover : theme_.inputBackground);
        StrokeRounded(hdc, valueRect, ScaleDip(dpi_, 7),
            valueEditing ? Blend(theme_.border, theme_.accent, 62) : theme_.border);
        Text(hdc, FormatDb(values[index], true), valueRect, smallFont_, theme_.textSecondary,
            DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        RECT track { x, top + ScaleDip(dpi_, 32), x + columnWidth, top + ScaleDip(dpi_, 38) };
        RECT thumb = rex::ui::SliderThumbRectForValue(track, (values[index] + 6.0) / 12.0, dpi_);
        rex::ui::PaintSlider(hdc, track, thumb, dpi_, Palette(), !busy_);
        preferenceTracks_.push_back(track);
        AddHit({ track.left, track.top - ScaleDip(dpi_, 6), track.right, track.bottom + ScaleDip(dpi_, 12) },
            Action::Preference, index);
    }
    cursor = preferencePanel.bottom + ScaleDip(dpi_, 16);

    RECT graph { left, cursor, right, cursor + ScaleDip(dpi_, 286) };
    PaintGraph(hdc, graph);
    cursor = graph.bottom + ScaleDip(dpi_, 16);

    const bool showAdvancedToggle = betaFeaturesEnabled_;
    const bool stackFooter = showAdvancedToggle && width < ScaleDip(dpi_, 620);
    RECT footer { left, cursor, right, cursor + ScaleDip(dpi_, stackFooter ? 108 : 72) };
    PaintPanel(hdc, footer);

    int statusRight = footer.right - ScaleDip(dpi_, 18);
    int statusBottom = footer.bottom - ScaleDip(dpi_, 10);
    if (showAdvancedToggle)
    {
        RECT customize {
            stackFooter ? footer.left + ScaleDip(dpi_, 18) : footer.right - ScaleDip(dpi_, 210),
            stackFooter ? footer.bottom - ScaleDip(dpi_, 52) : footer.top + ScaleDip(dpi_, 14),
            footer.right - ScaleDip(dpi_, 18),
            footer.bottom - ScaleDip(dpi_, 14)
        };
        HitTarget customizeHit { customize, Action::Advanced, 0 };
        AddHit(customize, Action::Advanced);
        PaintButton(hdc, customizeHit,
            service_.Settings().advancedVisible ? L"Hide advanced" : L"Customize EQ",
            false, service_.Settings().advancedVisible, !busy_);
        statusRight = stackFooter ? footer.right - ScaleDip(dpi_, 18) : customize.left - ScaleDip(dpi_, 18);
        statusBottom = stackFooter ? footer.top + ScaleDip(dpi_, 42) : footer.bottom - ScaleDip(dpi_, 10);
    }

    RECT status {
        footer.left + ScaleDip(dpi_, 18), footer.top + ScaleDip(dpi_, 10),
        statusRight,
        statusBottom
    };
    RECT statusDot {
        status.left, status.top + (status.bottom - status.top - ScaleDip(dpi_, 7)) / 2,
        status.left + ScaleDip(dpi_, 7), status.top + (status.bottom - status.top + ScaleDip(dpi_, 7)) / 2
    };
    FillRounded(hdc, statusDot, ScaleDip(dpi_, 4), statusError_ ? theme_.danger : (busy_ ? theme_.warning : theme_.accent));
    status.left = statusDot.right + ScaleDip(dpi_, 10);
    Text(hdc, statusMessage_.empty() ? L"Ready." : statusMessage_, status, smallFont_,
        statusError_ ? theme_.danger : theme_.textSecondary,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    return footer.bottom;
}
void EqualizerPage::Impl::BeginBackendSetup()
{
    if (!initialized_ || busy_) return;
    const auto status = service_.CurrentBackendStatus();
    if (status.availability == rex::equalizer::BackendAvailability::NotInstalled)
    {
        const auto package = rex::equalizer::EqualizerApoSetup::InspectBundledPackage();
        if (!package.verified)
        {
            ShowMessage(package.message, true);
            return;
        }

        const auto output = service_.CurrentOutputDevice();
        if (!output || output->endpointGuid.empty())
        {
            ShowMessage(
                L"Connect and select a Windows playback device before installing the audio engine.",
                true);
            return;
        }

        const std::wstring prompt =
            L"Rex's Toolkit includes the official Equalizer APO " + package.version +
            L" system audio engine.\n\n"
            L"Set up on:\n" + output->name + L"\n\n"
            L"Rex will:\n"
            L"- ask Windows for administrator approval\n"
            L"- silently install the verified bundled engine\n"
            L"- enable it for this output without changing outputs already configured\n"
            L"- run Equalizer APO's compatibility check\n"
            L"- add one isolated Rex configuration with a one-time backup\n\n"
            L"If Rex cannot match this output exactly, the official Device Selector "
            L"will stay open for manual confirmation. Windows Audio may restart, or "
            L"Windows may request a computer restart. Audio and device data stay on "
            L"this PC. Continue?";
        const int answer = MessageBoxW(
            window_, prompt.c_str(), L"Install System Audio Engine",
            MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
        if (answer != IDYES) return;

        HANDLE process = nullptr;
        std::wstring errorMessage;
        if (!rex::equalizer::EqualizerApoSetup::LaunchElevatedManagedSetup(
                window_, output->endpointGuid, process, errorMessage))
        {
            ShowMessage(errorMessage, true);
            return;
        }
        BeginSetupWorker(process, L"Installing and verifying the system audio engine...");
        return;
    }
    if (status.availability == rex::equalizer::BackendAvailability::DeviceNotConfigured)
    {
        const auto output = service_.CurrentOutputDevice();
        if (!output || output->endpointGuid.empty())
        {
            ShowMessage(
                L"Connect and select a Windows playback device before finishing audio setup.",
                true);
            return;
        }

        const std::wstring prompt =
            L"Rex's Toolkit will enable the system audio engine for:\n\n" +
            output->name +
            L"\n\nRex will select this output automatically and preserve any outputs "
            L"already configured. If the device cannot be matched exactly, Equalizer "
            L"APO's official Device Selector will stay open for manual confirmation. "
            L"Continue?";
        const int answer = MessageBoxW(
            window_, prompt.c_str(), L"Enable Selected Audio Output",
            MB_ICONINFORMATION | MB_OKCANCEL);
        if (answer != IDOK) return;

        HANDLE process = nullptr;
        std::wstring errorMessage;
        if (!rex::equalizer::EqualizerApoSetup::LaunchElevatedOutputSelection(
                window_, output->endpointGuid, process, errorMessage))
        {
            ShowMessage(errorMessage, true);
            return;
        }
        BeginSetupWorker(
            process, L"Enabling the output and restarting Windows Audio...");
        return;
    }

    if (status.availability == rex::equalizer::BackendAvailability::ToolkitIncludeMissing)
    {
        const int answer = MessageBoxW(
            window_,
            L"Rex's Toolkit needs to add one managed Include line to Equalizer APO's config.txt.\n\n"
            L"Your existing configuration remains in place. A one-time config.txt.rex-backup is created, "
            L"and Rex's settings are written to a separate managed file. Continue?",
            L"Finish Equalizer Integration",
            MB_ICONINFORMATION | MB_YESNO | MB_DEFBUTTON2);
        if (answer != IDYES) return;

        std::wstring errorMessage;
        if (!service_.InstallManagedInclude(errorMessage))
        {
            HANDLE process = nullptr;
            if (!rex::equalizer::EqualizerApoSetup::LaunchElevatedIntegration(
                    window_, process, errorMessage))
            {
                ShowMessage(errorMessage.empty() ? L"Equalizer setup was not completed." : errorMessage, true);
                return;
            }
            BeginSetupWorker(process, L"Finishing the isolated Rex Equalizer configuration...");
            return;
        }

        std::wstring ignored;
        rex::equalizer::EqualizerApoSetup::SetSetupPending(false, ignored);
        ApplyNow();
        ShowMessage(L"Equalizer integration is ready.");
        return;
    }

    if (status.availability == rex::equalizer::BackendAvailability::Ready)
    {
        std::wstring ignored;
        rex::equalizer::EqualizerApoSetup::SetSetupPending(false, ignored);
        ShowMessage(L"The system audio engine is already ready.");
        return;
    }
    ShowMessage(rex::equalizer::EqualizerService::BackendAvailabilityText(status.availability), true);
}

void EqualizerPage::Impl::PublishWorkerResult(WorkerResult result) noexcept
{
    try
    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        workerResult_ = std::move(result);
    }
    catch (...)
    {
        if (!shuttingDown_.load() && window_)
        {
            PostMessageW(window_, kWorkerCompleteMessage, 0, 0);
        }
        return;
    }
    if (!shuttingDown_.load() && window_)
    {
        PostMessageW(window_, kWorkerCompleteMessage, 0, 0);
    }
}

void EqualizerPage::Impl::PublishWorkerFailure(
    WorkerKind kind,
    const wchar_t* message) noexcept
{
    try
    {
        WorkerResult result;
        result.kind = kind;
        result.message = message;
        PublishWorkerResult(std::move(result));
    }
    catch (...)
    {
        if (!shuttingDown_.load() && window_)
        {
            PostMessageW(window_, kWorkerCompleteMessage, 0, 0);
        }
    }
}

void EqualizerPage::Impl::BeginProfileUpdate()
{
    if (!initialized_ || busy_) return;
    if (worker_.joinable()) worker_.join();
    busy_ = true;
    statusError_ = false;
    statusMessage_ = L"Downloading and validating the AutoEq headphone directory...";
    InvalidateRect(window_, nullptr, FALSE);
    try
    {
        std::filesystem::path dataDirectory = service_.DataDirectory();
        worker_ = std::thread([this, dataDirectory = std::move(dataDirectory)]() noexcept {
            try
            {
                WorkerResult result;
                result.kind = WorkerKind::Index;
                rex::equalizer::HeadphoneProfileService updater;
                if (!updater.Initialize(dataDirectory, result.message))
                {
                    result.success = false;
                }
                else
                {
                    result.success = updater.UpdateAutoEqIndex(result.message);
                }
                if (result.success) result.message = L"Headphone directory updated.";
                PublishWorkerResult(std::move(result));
            }
            catch (...)
            {
                PublishWorkerFailure(
                    WorkerKind::Index,
                    L"The headphone directory update stopped unexpectedly. Try again.");
            }
        });
    }
    catch (...)
    {
        PublishWorkerFailure(
            WorkerKind::Index,
            L"The headphone directory update could not be started. Try again.");
    }
}

void EqualizerPage::Impl::BeginWorkerProfile(const rex::equalizer::HeadphoneProfileSummary& summary)
{
    if (busy_) return;
    if (worker_.joinable()) worker_.join();
    busy_ = true;
    searchOpen_ = false;
    statusError_ = false;
    statusMessage_ = summary.cached
        ? L"Loading the recommended headphone profile..."
        : L"Downloading and validating the recommended AutoEq profile...";
    InvalidateRect(window_, nullptr, FALSE);
    try
    {
        rex::equalizer::HeadphoneProfileSummary workerSummary = summary;
        std::filesystem::path dataDirectory = service_.DataDirectory();
        worker_ = std::thread(
            [this,
             summary = std::move(workerSummary),
             dataDirectory = std::move(dataDirectory)]() noexcept {
            try
            {
                WorkerResult result;
                result.kind = WorkerKind::Profile;
                result.profileId = summary.id;
                result.displayName = summary.DisplayName();
                rex::equalizer::HeadphoneProfileService loader;
                std::wstring initializeError;
                if (!loader.Initialize(dataDirectory, initializeError))
                {
                    result.message = initializeError;
                }
                else
                {
                    result.success = loader.ResolveProfile(summary.id, result.profile, result.message);
                    if (result.success) result.message = L"Recommended headphone optimization loaded.";
                }
                PublishWorkerResult(std::move(result));
            }
            catch (...)
            {
                PublishWorkerFailure(
                    WorkerKind::Profile,
                    L"Rex's Toolkit could not finish loading this headphone profile. Try again.");
            }
        });
    }
    catch (...)
    {
        PublishWorkerFailure(
            WorkerKind::Profile,
            L"Rex's Toolkit could not start loading this headphone profile. Try again.");
    }
}

void EqualizerPage::Impl::BeginSetupWorker(HANDLE process, const std::wstring& statusMessage)
{
    if (!process) return;
    if (busy_)
    {
        CloseHandle(process);
        return;
    }
    if (worker_.joinable()) worker_.join();
    busy_ = true;
    statusError_ = false;
    statusMessage_ = statusMessage;
    Layout();
    InvalidateRect(window_, nullptr, FALSE);

    try
    {
        worker_ = std::thread([this, process]() noexcept {
            try
            {
                WorkerResult result;
                result.kind = WorkerKind::BackendSetup;
                bool cancelled = false;
                for (;;)
                {
                    const DWORD waitResult = WaitForSingleObject(process, 200);
                    if (waitResult == WAIT_OBJECT_0)
                    {
                        if (!GetExitCodeProcess(process, &result.exitCode))
                        {
                            result.message = L"Rex's Toolkit could not read the setup result.";
                            result.exitCode = ERROR_GEN_FAILURE;
                        }
                        result.success = result.exitCode == 0;
                        break;
                    }
                    if (waitResult == WAIT_FAILED)
                    {
                        result.exitCode = GetLastError();
                        result.message = L"Rex's Toolkit lost contact with the setup process.";
                        break;
                    }
                    if (shuttingDown_.load())
                    {
                        cancelled = true;
                        break;
                    }
                }
                if (!result.success && result.message.empty())
                {
                    result.message = result.exitCode == 31
                        ? L"Equalizer setup is complete, but Windows Audio could not be restarted. "
                          L"Restart Windows once to activate audio processing."
                        : L"Equalizer setup did not complete. No active audio configuration was changed by Rex's Toolkit.";
                }
                CloseHandle(process);
                if (!cancelled) PublishWorkerResult(std::move(result));
            }
            catch (...)
            {
                CloseHandle(process);
                PublishWorkerFailure(
                    WorkerKind::BackendSetup,
                    L"Equalizer setup stopped unexpectedly. No active audio configuration was changed by Rex's Toolkit.");
            }
        });
    }
    catch (...)
    {
        CloseHandle(process);
        PublishWorkerFailure(
            WorkerKind::BackendSetup,
            L"Equalizer setup could not be started. No active audio configuration was changed by Rex's Toolkit.");
    }
}

void EqualizerPage::Impl::CompleteWorker()
{
    if (worker_.joinable()) worker_.join();
    WorkerResult result;
    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        result = std::move(workerResult_);
        workerResult_ = {};
    }
    busy_ = false;
    if (result.kind == WorkerKind::BackendSetup)
    {
        std::wstring backendError;
        service_.ReinitializeBackend(backendError);
        deviceGeneration_ = service_.DeviceChangeGeneration();
        const auto status = service_.CurrentBackendStatus();
        if (!result.success)
        {
            std::wstring message = result.message;
            if (message.empty()) message = backendError;
            if (message.empty()) message = L"Equalizer setup did not complete.";
            ShowMessage(message, true);
        }
        else if (status.availability == rex::equalizer::BackendAvailability::Ready)
        {
            std::wstring ignored;
            rex::equalizer::EqualizerApoSetup::SetSetupPending(false, ignored);
            ApplyNow();
            ShowMessage(L"Equalizer setup is complete and ready for supported Windows audio.");
        }
        else if (status.availability == rex::equalizer::BackendAvailability::DeviceNotConfigured)
        {
            ShowMessage(
                L"Rex could not confirm automatic output selection. Choose Select output "
                L"again; the official selector will remain open if manual confirmation is needed.", true);
        }
        else if (status.availability == rex::equalizer::BackendAvailability::ToolkitIncludeMissing)
        {
            ShowMessage(L"The audio engine is installed. Choose Finish setup to enable Rex's isolated configuration.");
        }
        else if (status.availability == rex::equalizer::BackendAvailability::NoOutput)
        {
            ShowMessage(L"The audio engine is installed, but Windows has no compatible connected output right now.", true);
        }
        else
        {
            if (status.availability == rex::equalizer::BackendAvailability::NotInstalled)
            {
                std::wstring ignored;
                rex::equalizer::EqualizerApoSetup::SetSetupPending(false, ignored);
            }
            std::wstring message = result.message;
            if (message.empty()) message = backendError;
            if (message.empty()) message = rex::equalizer::EqualizerService::BackendAvailabilityText(status.availability);
            ShowMessage(message, true);
        }
    }
    else if (result.success && result.kind == WorkerKind::Profile)
    {
        service_.SelectResolvedHeadphone(result.profileId, result.displayName, std::move(result.profile));
        SyncHeadphoneEditFromSelection();
        ApplyNow();
    }
    else if (result.success && result.kind == WorkerKind::Index)
    {
        std::wstring ignored;
        service_.ReloadHeadphoneIndex(ignored);
        ShowMessage(result.message);
    }
    else
    {
        ShowMessage(result.message.empty() ? L"The Equalizer operation failed." : result.message, true);
    }
    Layout();
    InvalidateRect(window_, nullptr, FALSE);
}

void EqualizerPage::Impl::ApplyNow(bool persistSettings)
{
    if (!initialized_) return;
    KillTimer(window_, kApplyTimerId);
    const auto result = service_.Apply(persistSettings);
    const auto& settings = service_.CurrentDeviceSettings();
    const double preampDb = service_.CurrentProfile().preampDb;
    if (result.success && settings.enabled && settings.preventClipping && preampDb < -0.25)
    {
        statusMessage_ = result.message + L" Safe headroom lowers output by " +
            FormatDb(std::abs(preampDb)) + L"; raise Windows volume if needed.";
    }
    else
    {
        statusMessage_ = result.message;
    }
    statusError_ = !result.success;
    InvalidateRect(window_, nullptr, FALSE);
}

void EqualizerPage::Impl::QueueApply()
{
    if (!initialized_) return;
    KillTimer(window_, kApplyTimerId);
    SetTimer(window_, kApplyTimerId, kApplyDelayMs, nullptr);
}

void EqualizerPage::Impl::QueueLivePreview()
{
    if (!initialized_) return;
    KillTimer(window_, kApplyTimerId);
    livePreviewDirty_ = true;
    livePreviewPending_ = true;
    if (livePreviewTimerScheduled_) return;

    livePreviewTimerScheduled_ =
        SetTimer(window_, kLivePreviewTimerId, kLivePreviewIntervalMs, nullptr) != 0;
    if (!livePreviewTimerScheduled_)
    {
        livePreviewPending_ = false;
        ApplyNow(false);
    }
}

void EqualizerPage::Impl::FinishLivePreview()
{
    if (window_) KillTimer(window_, kLivePreviewTimerId);
    livePreviewTimerScheduled_ = false;
    livePreviewPending_ = false;
    const bool shouldApply = initialized_ && livePreviewDirty_;
    livePreviewDirty_ = false;
    if (shouldApply) ApplyNow();
}

void EqualizerPage::Impl::ShowMessage(const std::wstring& message, bool error)
{
    statusMessage_ = message;
    statusError_ = error;
    InvalidateRect(window_, nullptr, FALSE);
}

rex::ui::Palette EqualizerPage::Impl::Palette() const
{
    rex::ui::Palette palette;
    palette.pageBackground = theme_.pageBackground;
    palette.inputBackground = theme_.inputBackground;
    palette.buttonBackground = theme_.buttonBackground;
    palette.buttonHover = theme_.panelHover;
    palette.buttonPressed = Blend(theme_.panelHover, theme_.accent, 18);
    palette.disabledBackground = Blend(theme_.buttonBackground, theme_.pageBackground, 55);
    palette.disabledText = Blend(theme_.textSecondary, theme_.pageBackground, 35);
    palette.dropdownBackground = Blend(theme_.panelBackground, theme_.inputBackground, 28);
    palette.dropdownHover = theme_.panelHover;
    palette.dropdownSelected = Blend(theme_.panelBackground, theme_.accentSoft, 36);
    palette.border = theme_.border;
    palette.textPrimary = theme_.textPrimary;
    palette.textSecondary = theme_.textSecondary;
    palette.accent = theme_.accent;
    palette.accentSoft = theme_.accentSoft;
    palette.danger = theme_.danger;
    palette.dangerAccent = theme_.danger;
    palette.surfaceStyle = theme_.surfaceStyle;
    palette.surfaceOpacity = theme_.surfaceOpacity;
    palette.light = theme_.light;
    return palette;
}

int EqualizerPage::Impl::SetupPanelHeight(int contentWidth) const
{
    return ScaleDip(dpi_, contentWidth < ScaleDip(dpi_, 720) ? 306 : 218);
}

int EqualizerPage::Impl::PresetColumnCount(int contentWidth) const
{
    const int gap = ScaleDip(dpi_, 10);
    const int preferredWidth = ScaleDip(dpi_, 132);
    return std::clamp((contentWidth + gap) / std::max(1, preferredWidth + gap), 1, 5);
}

int EqualizerPage::Impl::SimpleContentBottom(int contentWidth) const
{
    int cursor = ScaleDip(dpi_, 16) + SetupPanelHeight(contentWidth) + ScaleDip(dpi_, 14);
    cursor += ScaleDip(dpi_, 72 + 16);
    cursor += ScaleDip(dpi_, 26 + 4);
    const int columns = PresetColumnCount(contentWidth);
    const int rows = (5 + columns - 1) / columns;
    cursor += rows * ScaleDip(dpi_, 42) + std::max(0, rows - 1) * ScaleDip(dpi_, 8) + ScaleDip(dpi_, 16);
    cursor += ScaleDip(dpi_, contentWidth < ScaleDip(dpi_, 680) ? 286 : 174) + ScaleDip(dpi_, 16);
    cursor += ScaleDip(dpi_, 286 + 16);
    cursor += ScaleDip(dpi_, betaFeaturesEnabled_ && contentWidth < ScaleDip(dpi_, 620) ? 108 : 72);
    return cursor;
}

int EqualizerPage::Impl::EstimatedContentHeight(int contentWidth) const
{
    if (!initialized_)
    {
        return ScaleDip(dpi_, 16) + SetupPanelHeight(contentWidth) + ScaleDip(dpi_, 28);
    }

    int cursor = SimpleContentBottom(contentWidth);
    if (!betaFeaturesEnabled_ || !service_.Settings().advancedVisible) return cursor + ScaleDip(dpi_, 28);

    cursor += ScaleDip(dpi_, 24);
    cursor += ScaleDip(dpi_, 44);
    cursor += ScaleDip(dpi_, 54);

    const auto& settings = service_.CurrentDeviceSettings();
    if (settings.editorMode == rex::equalizer::EditorMode::Simple)
    {
        cursor += ScaleDip(dpi_, 140 + 16);
    }
    else if (settings.editorMode == rex::equalizer::EditorMode::Graphic)
    {
        cursor += ScaleDip(dpi_, 286 + 16);
    }
    else
    {
        const auto& filters = settings.parametricOverrideActive ? settings.customFilters : service_.CurrentProfile().filters;
        cursor += ScaleDip(dpi_, 76 + static_cast<int>(std::max<size_t>(1, filters.size())) * 48 + 16);
    }

    cursor += ScaleDip(dpi_, 120 + 16);
    if (service_.Settings().showTechnicalControls || diagnosticsOpen_) cursor += ScaleDip(dpi_, 258);
    return cursor + ScaleDip(dpi_, 28);
}

int EqualizerPage::Impl::VisibleOutputDeviceRows() const
{
    if (!window_ || service_.OutputDevices().empty()) return 0;
    RECT client {};
    GetClientRect(window_, &client);
    const int availableBelow = std::max(0L, client.bottom - outputRect_.bottom - ScaleDip(dpi_, 8));
    const int availableAbove = std::max(0L, outputRect_.top - ScaleDip(dpi_, 8));
    const int available = std::max(availableBelow, availableAbove);
    const int fixedHeight = ScaleDip(dpi_, 12 + 44 + 26);
    const int rows = std::max(1, (available - fixedHeight) / std::max(1, ScaleDip(dpi_, 44)));
    return std::clamp(rows, 1, static_cast<int>(std::min<size_t>(7, service_.OutputDevices().size())));
}

void EqualizerPage::Impl::EnsureSelectedOutputVisible()
{
    const int visibleRows = VisibleOutputDeviceRows();
    const int count = static_cast<int>(service_.OutputDevices().size());
    if (visibleRows <= 0 || count <= visibleRows)
    {
        outputFirstIndex_ = 0;
        return;
    }

    int selected = 0;
    if (!service_.Settings().followWindowsDefault)
    {
        for (int index = 0; index < count; ++index)
        {
            if (service_.OutputDevices()[static_cast<size_t>(index)].id == service_.Settings().selectedOutputId)
            {
                selected = index;
                break;
            }
        }
    }
    if (selected < outputFirstIndex_) outputFirstIndex_ = selected;
    if (selected >= outputFirstIndex_ + visibleRows) outputFirstIndex_ = selected - visibleRows + 1;
    outputFirstIndex_ = std::clamp(outputFirstIndex_, 0, std::max(0, count - visibleRows));
}

void EqualizerPage::Impl::Layout()
{
    if (!window_) return;
    RECT client {};
    GetClientRect(window_, &client);
    const int margin = ScaleDip(dpi_, 24);
    const int width = std::min(ScaleDip(dpi_, 1160), std::max(1, static_cast<int>(client.right) - margin * 2));
    contentHeight_ = EstimatedContentHeight(width);
    ClampScroll();

    const int left = std::max(margin, (static_cast<int>(client.right) - width) / 2);
    const int y = ScaleDip(dpi_, 16) - scrollOffset_;
    const bool stackedFields = width < ScaleDip(dpi_, 720);

    if (stackedFields)
    {
        outputRect_ = {
            left + ScaleDip(dpi_, 20), y + ScaleDip(dpi_, 100),
            left + width - ScaleDip(dpi_, 20), y + ScaleDip(dpi_, 148)
        };
        headphoneRect_ = {
            outputRect_.left, y + ScaleDip(dpi_, 190),
            outputRect_.right, y + ScaleDip(dpi_, 238)
        };
    }
    else
    {
        outputRect_ = {
            left + ScaleDip(dpi_, 20), y + ScaleDip(dpi_, 105),
            left + width / 2 - ScaleDip(dpi_, 8), y + ScaleDip(dpi_, 153)
        };
        headphoneRect_ = {
            left + width / 2 + ScaleDip(dpi_, 8), y + ScaleDip(dpi_, 105),
            left + width - ScaleDip(dpi_, 20), y + ScaleDip(dpi_, 153)
        };
    }

    const int outputRows = VisibleOutputDeviceRows();
    const int deviceCount = static_cast<int>(service_.OutputDevices().size());
    outputFirstIndex_ = std::clamp(outputFirstIndex_, 0, std::max(0, deviceCount - outputRows));
    const bool outputOverflow = deviceCount > outputRows;
    const int outputMenuHeight = ScaleDip(dpi_, 12 + 44 * (1 + outputRows) + (outputOverflow ? 26 : 0));
    int outputTop = outputRect_.bottom + ScaleDip(dpi_, 6);
    if (outputTop + outputMenuHeight > client.bottom - ScaleDip(dpi_, 8) &&
        outputRect_.top - ScaleDip(dpi_, 6) - outputMenuHeight >= ScaleDip(dpi_, 8))
    {
        outputTop = outputRect_.top - ScaleDip(dpi_, 6) - outputMenuHeight;
    }
    outputDropdownRect_ = { outputRect_.left, outputTop, outputRect_.right, outputTop + outputMenuHeight };

    const int searchResultCount = static_cast<int>(searchResults_.size());
    const int searchRows = std::min(kHeadphoneVisibleRows, searchResultCount);
    headphoneFirstIndex_ = std::clamp(
        headphoneFirstIndex_, 0, std::max(0, searchResultCount - searchRows));
    const bool searchOverflow = searchResultCount > searchRows;
    const int searchMenuHeight = searchRows > 0
        ? ScaleDip(dpi_, 12 + 56 * searchRows + (searchOverflow ? 26 : 0))
        : ScaleDip(dpi_, 60);
    int searchTop = headphoneRect_.bottom + ScaleDip(dpi_, 6);
    if (searchTop + searchMenuHeight > client.bottom - ScaleDip(dpi_, 8) &&
        headphoneRect_.top - ScaleDip(dpi_, 6) - searchMenuHeight >= ScaleDip(dpi_, 8))
    {
        searchTop = headphoneRect_.top - ScaleDip(dpi_, 6) - searchMenuHeight;
    }
    headphoneResultsRect_ = {
        headphoneRect_.left, searchTop,
        headphoneRect_.right, searchTop + searchMenuHeight
    };
    UpdateChildBounds();
}

void EqualizerPage::Impl::ClampScroll()
{
    if (!window_) return;
    RECT client {};
    GetClientRect(window_, &client);
    const int maximumOffset = std::max(0, contentHeight_ - static_cast<int>(client.bottom - client.top));
    scrollOffset_ = std::clamp(scrollOffset_, 0, maximumOffset);
    smoothScrollTargetOffset_ = std::clamp(smoothScrollTargetOffset_, 0, maximumOffset);
}

void EqualizerPage::Impl::ScrollToOffset(int offset, bool animate)
{
    if (!window_) return;
    RECT client {};
    GetClientRect(window_, &client);
    const int maximumOffset = std::max(0, contentHeight_ - static_cast<int>(client.bottom - client.top));
    const int target = std::clamp(offset, 0, maximumOffset);

    if (!animate || !theme_.smoothScrollingEnabled || std::abs(target - scrollOffset_) <= ScaleDip(dpi_, 2))
    {
        StopSmoothScroll();
        if (scrollOffset_ == target) return;
        scrollOffset_ = target;
        smoothScrollTargetOffset_ = target;
        Layout();
        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE | RDW_NOCHILDREN);
        return;
    }

    smoothScrollTargetOffset_ = target;
    smoothScrollActive_ = true;
    const int remainingDistance = target - scrollOffset_;
    int immediateStep = static_cast<int>(std::lround(static_cast<double>(remainingDistance) * 0.65));
    if (immediateStep == 0)
    {
        immediateStep = remainingDistance > 0 ? 1 : -1;
    }
    scrollOffset_ = std::clamp(scrollOffset_ + immediateStep, 0, maximumOffset);
    Layout();
    RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE | RDW_NOCHILDREN);
    SetTimer(window_, kSmoothScrollTimerId, kSmoothScrollTickMs, nullptr);
}

void EqualizerPage::Impl::StopSmoothScroll()
{
    smoothScrollActive_ = false;
    smoothScrollTargetOffset_ = scrollOffset_;
    scrollWheelDeltaRemainder_ = 0;
    if (window_)
    {
        KillTimer(window_, kSmoothScrollTimerId);
    }
}

bool EqualizerPage::Impl::StepSmoothScroll()
{
    if (!smoothScrollActive_ || !window_)
    {
        return false;
    }

    RECT client {};
    GetClientRect(window_, &client);
    const int maximumOffset = std::max(0, contentHeight_ - static_cast<int>(client.bottom - client.top));
    smoothScrollTargetOffset_ = std::clamp(smoothScrollTargetOffset_, 0, maximumOffset);
    const int remainingDistance = smoothScrollTargetOffset_ - scrollOffset_;
    const int finishDistance = std::max(1, ScaleDip(dpi_, 2));
    if (std::abs(remainingDistance) <= finishDistance)
    {
        scrollOffset_ = smoothScrollTargetOffset_;
        smoothScrollActive_ = false;
        Layout();
        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE | RDW_NOCHILDREN);
        return false;
    }

    int step = static_cast<int>(std::lround(static_cast<double>(remainingDistance) * 0.65));
    const int minimumStep = std::max(1, ScaleDip(dpi_, 2));
    if (std::abs(step) < minimumStep)
    {
        step = remainingDistance > 0 ? minimumStep : -minimumStep;
    }
    if (std::abs(step) > std::abs(remainingDistance))
    {
        step = remainingDistance;
    }
    scrollOffset_ = std::clamp(scrollOffset_ + step, 0, maximumOffset);
    Layout();
    RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE | RDW_NOCHILDREN);
    return true;
}

void EqualizerPage::Impl::UpdateChildBounds()
{
    if (!window_ || !headphoneEdit_) return;
    const int fieldHeight = std::max(1L, headphoneRect_.bottom - headphoneRect_.top);
    const int editHeight = std::min(fieldHeight - ScaleDip(dpi_, 6), ScaleDip(dpi_, 24));
    const int editLeft = headphoneRect_.left + ScaleDip(dpi_, 38);
    const int editRight = headphoneRect_.right - ScaleDip(dpi_, 12);
    const int editTop = headphoneRect_.top + (fieldHeight - editHeight) / 2 + ScaleDip(dpi_, 2);
    RECT overlap {};
    const bool coveredByOutputMenu = outputOpen_ &&
        IntersectRect(&overlap, &outputDropdownRect_, &headphoneRect_) != FALSE;

    RECT desiredEdit { editLeft, editTop, editRight, editTop + editHeight };
    RECT currentEdit {};
    GetWindowRect(headphoneEdit_, &currentEdit);
    MapWindowPoints(nullptr, window_, reinterpret_cast<POINT*>(&currentEdit), 2);
    if (!EqualRect(&desiredEdit, &currentEdit))
    {
        SetWindowPos(
            headphoneEdit_, nullptr, desiredEdit.left, desiredEdit.top,
            std::max(1L, desiredEdit.right - desiredEdit.left),
            std::max(1L, desiredEdit.bottom - desiredEdit.top),
            SWP_NOACTIVATE | SWP_NOZORDER);
    }
    const bool editEnabled = initialized_ && !busy_;
    if ((IsWindowEnabled(headphoneEdit_) != FALSE) != editEnabled)
    {
        EnableWindow(headphoneEdit_, editEnabled);
    }
    const bool editVisible = visible_ && !coveredByOutputMenu;
    if ((IsWindowVisible(headphoneEdit_) != FALSE) != editVisible)
    {
        ShowWindow(headphoneEdit_, editVisible ? SW_SHOW : SW_HIDE);
    }

    if (numericEdit_ && IsWindowVisible(numericEdit_))
    {
        SetWindowPos(
            numericEdit_, nullptr, numericRect_.left, numericRect_.top,
            std::max(1L, numericRect_.right - numericRect_.left),
            std::max(1L, numericRect_.bottom - numericRect_.top),
            SWP_NOACTIVATE | SWP_NOZORDER);
    }
}
bool EqualizerPage::Impl::Create(HINSTANCE instance, HWND parent)
{
    if (window_) return true;
    shuttingDown_ = false;
    instance_ = instance;
    parent_ = parent;

    WNDCLASSEXW windowClass {};
    windowClass.cbSize = sizeof(windowClass);
    if (!GetClassInfoExW(instance_, kPageClassName, &windowClass))
    {
        windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = instance_;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = nullptr;
        windowClass.lpszClassName = kPageClassName;
        if (!RegisterClassExW(&windowClass)) return false;
    }

    window_ = CreateWindowExW(
        0, kPageClassName, L"",
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 1, 1, parent_, nullptr, instance_, this);
    if (!window_) return false;

    headphoneEdit_ = CreateWindowExW(
        0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 1, 1, window_, reinterpret_cast<HMENU>(1001), instance_, nullptr);
    numericEdit_ = CreateWindowExW(
        0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL | ES_CENTER,
        0, 0, 1, 1, window_, reinterpret_cast<HMENU>(1002), instance_, nullptr);
    if (!headphoneEdit_ || !numericEdit_)
    {
        Destroy();
        return false;
    }

    SendMessageW(headphoneEdit_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search headphones..."));
    SendMessageW(headphoneEdit_, EM_SETLIMITTEXT, 160, 0);
    SendMessageW(headphoneEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
        MAKELPARAM(ScaleDip(dpi_, 2), ScaleDip(dpi_, 2)));
    searchOriginalProc_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        headphoneEdit_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(SearchEditProc)));
    SetPropW(headphoneEdit_, L"RexEqualizerSearch", this);
    SendMessageW(numericEdit_, EM_SETLIMITTEXT, 18, 0);
    numericOriginalProc_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        numericEdit_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(NumericEditProc)));
    SetPropW(numericEdit_, L"RexEqualizerPage", this);
    rex::ui::SetThemedEditContextMenu(headphoneEdit_, Palette());
    rex::ui::SetThemedEditContextMenu(numericEdit_, Palette());

    RecreateFonts();
    std::wstring errorMessage;
    initialized_ = service_.Initialize(errorMessage);
    statusMessage_ = initialized_ ? L"Ready." : errorMessage;
    statusError_ = !initialized_;
    if (initialized_)
    {
        SyncHeadphoneEditFromSelection();
        deviceGeneration_ = service_.DeviceChangeGeneration();
        if (service_.Settings().enableOnStartup)
        {
            service_.SetEnabled(true);
        }
        enabledSwitch_ = rex::ui::MakeSwitchAnimationState(service_.CurrentDeviceSettings().enabled);
        preampSwitch_ = rex::ui::MakeSwitchAnimationState(service_.CurrentDeviceSettings().preventClipping);
        ApplyNow();

        if (rex::equalizer::EqualizerApoSetup::IsSetupPending())
        {
            const auto backend = service_.CurrentBackendStatus();
            statusError_ = false;
            switch (backend.availability)
            {
            case rex::equalizer::BackendAvailability::Ready:
            {
                std::wstring ignored;
                rex::equalizer::EqualizerApoSetup::SetSetupPending(false, ignored);
                statusMessage_ = L"Equalizer setup resumed successfully and is ready.";
                break;
            }
            case rex::equalizer::BackendAvailability::DeviceNotConfigured:
                statusMessage_ = L"Finish Equalizer setup by choosing Select output.";
                break;
            case rex::equalizer::BackendAvailability::ToolkitIncludeMissing:
                statusMessage_ = L"Finish Equalizer setup by enabling Rex's isolated configuration.";
                break;
            case rex::equalizer::BackendAvailability::NoOutput:
                statusMessage_ = L"Equalizer is installed. Connect a Windows audio output to finish setup.";
                break;
            case rex::equalizer::BackendAvailability::NotInstalled:
                statusMessage_ = L"Equalizer setup was interrupted. Choose Install engine to try again.";
                statusError_ = true;
                break;
            case rex::equalizer::BackendAvailability::Error:
                statusMessage_ = L"Equalizer setup needs attention. Open diagnostics for details.";
                statusError_ = true;
                break;
            }
        }
    }
    UpdateDeviceTimer();
    return true;
}

void EqualizerPage::Impl::Shutdown()
{
    if (shuttingDown_.exchange(true)) return;
    if (window_)
    {
        KillTimer(window_, kDeviceTimerId);
        KillTimer(window_, kApplyTimerId);
        KillTimer(window_, kLivePreviewTimerId);
        KillTimer(window_, kSwitchTimerId);
        KillTimer(window_, kSmoothScrollTimerId);
    }
    if (worker_.joinable()) worker_.join();
    service_.Shutdown();
    initialized_ = false;
}

void EqualizerPage::Impl::Destroy()
{
    Shutdown();
    if (numericEdit_)
    {
        RemovePropW(numericEdit_, L"RexEqualizerPage");
        numericEdit_ = nullptr;
    }
    headphoneEdit_ = nullptr;
    if (window_)
    {
        HWND window = window_;
        window_ = nullptr;
        DestroyWindow(window);
    }
    if (bufferDc_)
    {
        SelectObject(bufferDc_, oldBufferBitmap_);
        DeleteObject(bufferBitmap_);
        DeleteDC(bufferDc_);
        bufferDc_ = nullptr;
        bufferBitmap_ = nullptr;
        oldBufferBitmap_ = nullptr;
    }
    for (HFONT* font : { &headingFont_, &sectionFont_, &bodyFont_, &smallFont_, &monoFont_ })
    {
        if (*font) DeleteObject(*font);
        *font = nullptr;
    }
    if (editBrush_) DeleteObject(editBrush_);
    editBrush_ = nullptr;
}

void EqualizerPage::Impl::SetVisible(bool visible)
{
    visible_ = visible;
    if (!window_) return;
    if (visible && initialized_) SyncHeadphoneEditFromSelection();
    ShowWindow(window_, visible ? SW_SHOW : SW_HIDE);
    if (!visible)
    {
        FinishLivePreview();
        StopSmoothScroll();
        searchOpen_ = false;
        outputOpen_ = false;
        ShowWindow(numericEdit_, SW_HIDE);
    }
    UpdateDeviceTimer();
}

void EqualizerPage::Impl::UpdateDeviceTimer()
{
    if (!window_) return;
    const bool needsDeviceEvents = visible_ ||
        (initialized_ && service_.CurrentDeviceSettings().enabled &&
         service_.Settings().automaticallyApplyDeviceProfile);
    if (needsDeviceEvents)
    {
        SetTimer(window_, kDeviceTimerId, kDeviceTimerMs, nullptr);
    }
    else
    {
        KillTimer(window_, kDeviceTimerId);
    }
}

void EqualizerPage::Impl::SetBounds(const RECT& bounds, UINT dpi)
{
    if (!window_) return;
    const bool dpiChanged = dpi_ != dpi;
    const bool boundsChanged = EqualRect(&parentBounds_, &bounds) == FALSE;
    if (!dpiChanged && !boundsChanged)
    {
        return;
    }
    parentBounds_ = bounds;
    dpi_ = dpi;
    if (dpiChanged) RecreateFonts();
    if (boundsChanged)
    {
        SetWindowPos(
            window_, HWND_TOP, bounds.left, bounds.top,
            std::max(1L, bounds.right - bounds.left),
            std::max(1L, bounds.bottom - bounds.top),
            SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
    Layout();
    InvalidateRect(window_, nullptr, FALSE);
}

void EqualizerPage::Impl::SetTheme(const EqualizerTheme& theme)
{
    const bool unchanged =
        theme_.pageBackground == theme.pageBackground &&
        theme_.panelBackground == theme.panelBackground &&
        theme_.panelHover == theme.panelHover &&
        theme_.inputBackground == theme.inputBackground &&
        theme_.buttonBackground == theme.buttonBackground &&
        theme_.border == theme.border &&
        theme_.textPrimary == theme.textPrimary &&
        theme_.textSecondary == theme.textSecondary &&
        theme_.accent == theme.accent &&
        theme_.accentSoft == theme.accentSoft &&
        theme_.warning == theme.warning &&
        theme_.danger == theme.danger &&
        theme_.surfaceStyle == theme.surfaceStyle &&
        theme_.surfaceOpacity == theme.surfaceOpacity &&
        theme_.light == theme.light &&
        theme_.smoothScrollingEnabled == theme.smoothScrollingEnabled;
    if (unchanged)
    {
        return;
    }

    const bool inputBackgroundChanged = theme_.inputBackground != theme.inputBackground;
    theme_ = theme;
    if (!theme_.smoothScrollingEnabled)
    {
        StopSmoothScroll();
    }
    if (inputBackgroundChanged || !editBrush_)
    {
        if (editBrush_) DeleteObject(editBrush_);
        editBrush_ = CreateSolidBrush(theme_.inputBackground);
    }
    rex::ui::SetThemedEditContextMenu(headphoneEdit_, Palette());
    rex::ui::SetThemedEditContextMenu(numericEdit_, Palette());
    if (window_) InvalidateRect(window_, nullptr, FALSE);
}

void EqualizerPage::Impl::SetBetaFeaturesEnabled(bool enabled)
{
    const bool advancedWasVisible = initialized_ && service_.Settings().advancedVisible;
    const bool mustCloseAdvanced = !enabled && (advancedWasVisible || diagnosticsOpen_);
    if (betaFeaturesEnabled_ == enabled && !mustCloseAdvanced) return;

    betaFeaturesEnabled_ = enabled;
    if (mustCloseAdvanced)
    {
        if (advancedWasVisible)
        {
            service_.SetAdvancedVisible(false);
            std::wstring ignored;
            service_.Save(ignored);
        }
        diagnosticsOpen_ = false;
        scrollOffset_ = 0;
        smoothScrollTargetOffset_ = 0;
        StopSmoothScroll();
    }

    if (window_)
    {
        Layout();
        ClampScroll();
        UpdateChildBounds();
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void EqualizerPage::Impl::SetBackgroundPainter(std::function<void(HDC, const RECT&)> painter)
{
    backgroundPainter_ = std::move(painter);
    if (window_) InvalidateRect(window_, nullptr, FALSE);
}

void EqualizerPage::Impl::RecreateFonts()
{
    for (HFONT* font : { &headingFont_, &sectionFont_, &bodyFont_, &smallFont_, &monoFont_ })
    {
        if (*font) DeleteObject(*font);
    }
    headingFont_ = MakeFont(dpi_, 18, FW_SEMIBOLD, L"Bahnschrift SemiBold");
    sectionFont_ = MakeFont(dpi_, 12, FW_SEMIBOLD, L"Bahnschrift SemiBold");
    bodyFont_ = MakeFont(dpi_, 11, FW_NORMAL);
    smallFont_ = MakeFont(dpi_, 9, FW_NORMAL);
    monoFont_ = MakeFont(dpi_, 9, FW_NORMAL, L"Cascadia Mono");
    if (headphoneEdit_)
    {
        SendMessageW(headphoneEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont_), TRUE);
        SendMessageW(headphoneEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
            MAKELPARAM(ScaleDip(dpi_, 2), ScaleDip(dpi_, 2)));
    }
    if (numericEdit_) SendMessageW(numericEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont_), FALSE);
    if (editBrush_) DeleteObject(editBrush_);
    editBrush_ = CreateSolidBrush(theme_.inputBackground);
}

std::wstring EqualizerPage::Impl::CurrentPresetId() const
{
    return initialized_ ? service_.CurrentDeviceSettings().soundPreset : L"balanced";
}

std::wstring EqualizerPage::Impl::CurrentOutputName() const
{
    if (!initialized_) return L"No output";
    const auto device = service_.CurrentOutputDevice();
    return device ? device->name : L"No output";
}

std::wstring EqualizerPage::Impl::CurrentHeadphoneName() const
{
    return initialized_ ? service_.CurrentDeviceSettings().headphoneDisplayName : L"No headphone correction";
}

std::wstring EqualizerPage::Impl::HeadphoneDatabaseVersion() const
{
    return initialized_ ? service_.HeadphoneDatabaseVersion() : L"Unavailable";
}

void EqualizerPage::Impl::ToggleEnabled()
{
    if (!initialized_) return;
    service_.SetEnabled(!service_.CurrentDeviceSettings().enabled);
    rex::ui::SetSwitchTarget(enabledSwitch_, service_.CurrentDeviceSettings().enabled, enabledSwitch_.bounds);
    SetTimer(window_, kSwitchTimerId, 16, nullptr);
    ApplyNow();
    UpdateDeviceTimer();
}

void EqualizerPage::Impl::SelectPreset(const std::wstring& id)
{
    if (!initialized_ || busy_) return;
    service_.SetSoundPreset(id);
    ApplyNow();
}

void EqualizerPage::Impl::SetRememberPerDevice(bool value)
{
    if (!initialized_) return;
    service_.SetRememberPerDevice(value);
    SyncHeadphoneEditFromSelection();
    ApplyNow();
}

void EqualizerPage::Impl::SetAutomaticallyApplyDeviceProfile(bool value)
{
    if (!initialized_) return;
    service_.SetAutomaticallyApplyDeviceProfile(value);
    std::wstring ignored;
    service_.Save(ignored);
    UpdateDeviceTimer();
    InvalidateRect(window_, nullptr, FALSE);
}

void EqualizerPage::Impl::SetEnableOnStartup(bool value)
{
    if (!initialized_) return;
    service_.SetEnableOnStartup(value);
    std::wstring ignored;
    service_.Save(ignored);
    InvalidateRect(window_, nullptr, FALSE);
}

void EqualizerPage::Impl::SetPreventClipping(bool value)
{
    if (!initialized_) return;
    service_.SetPreventClipping(value);
    rex::ui::SetSwitchTarget(preampSwitch_, value, preampSwitch_.bounds);
    SetTimer(window_, kSwitchTimerId, 16, nullptr);
    ApplyNow();
}

void EqualizerPage::Impl::SetTrayControlsEnabled(bool value)
{
    if (!initialized_) return;
    service_.SetTrayControlsEnabled(value);
    std::wstring ignored;
    service_.Save(ignored);
    InvalidateRect(window_, nullptr, FALSE);
}

void EqualizerPage::Impl::SetShowTechnicalControls(bool value)
{
    if (!initialized_) return;
    service_.SetShowTechnicalControls(value);
    if (!value) diagnosticsOpen_ = false;
    std::wstring ignored;
    service_.Save(ignored);
    Layout();
    ClampScroll();
    UpdateChildBounds();
    InvalidateRect(window_, nullptr, FALSE);
}

void EqualizerPage::Impl::OpenAdvanced(bool diagnostics)
{
    if (!initialized_ || !betaFeaturesEnabled_) return;
    service_.SetAdvancedVisible(true);
    diagnosticsOpen_ = diagnostics;
    scrollOffset_ = diagnostics ? 10000 : 0;
    Layout();
    ClampScroll();
    UpdateChildBounds();
    InvalidateRect(window_, nullptr, FALSE);
}
