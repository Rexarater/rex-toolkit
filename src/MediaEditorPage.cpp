#include "CacheManager.h"
#include "MediaEditorPage.h"
#include "UiComponents.h"
#include "resource.h"

#include <commdlg.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace
{
constexpr wchar_t kMediaEditorPageClass[] = L"RexToolkit.MediaEditorPage";
constexpr wchar_t kMediaEditorPreviewClass[] = L"RexToolkit.MediaEditorPreview";
constexpr wchar_t kMediaEditorTextPopupClass[] = L"RexToolkit.MediaEditorTextPopup";
constexpr UINT kImportFinishedMessage = WM_APP + 0x431;
constexpr UINT kExportProgressMessage = WM_APP + 0x432;
constexpr UINT kExportFinishedMessage = WM_APP + 0x433;
constexpr UINT kPreviewReadyMessage = WM_APP + 0x434;
constexpr UINT kPreviewErrorMessage = WM_APP + 0x435;
constexpr UINT kPreviewProxyFinishedMessage = WM_APP + 0x436;
constexpr UINT_PTR kClipboardTimerId = 1;
constexpr UINT_PTR kPreviewTimerId = 2;
constexpr UINT_PTR kControlAnimationTimerId = 3;
constexpr UINT kClipboardTimerMs = 650;
constexpr UINT kPreviewTimerMs = 33;
constexpr UINT kControlAnimationTimerMs = 16;
constexpr ULONGLONG kNativePreviewTimeoutMs = 3000;
constexpr COLORREF kTextEditColorKey = RGB(1, 2, 3);

enum class EditorView
{
    Import,
    Video,
    Image
};

enum class PointerAction
{
    None,
    TimelineSeek,
    TimelineTrimLeft,
    TimelineTrimRight,
    TimelineReorder,
    TimelineScroll,
    ImageDraw,
    ImageTextBox,
    ImageCrop,
    ImagePan,
    ImageOpacity,
    ImageThickness,
    VideoVolume
};

enum class ImageToolbarPopup
{
    None,
    Color,
    Thickness,
    Zoom,
    Save
};

enum class CropHandle
{
    None,
    Left,
    Top,
    Right,
    Bottom,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
    Move
};

struct MediaImportThreadResult
{
    unsigned long long generation = 0;
    bool appendVideo = false;
    std::unique_ptr<ImageEditingSession> imageSession;
    std::vector<VideoAnalysis> videos;
    std::wstring errorMessage;
};

std::unique_ptr<Gdiplus::Bitmap> LoadPngResource(HINSTANCE instance, int resourceId)
{
    HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!resource) return nullptr;

    const DWORD resourceSize = SizeofResource(instance, resource);
    HGLOBAL loadedResource = LoadResource(instance, resource);
    const void* resourceData = LockResource(loadedResource);
    if (!resourceData || resourceSize == 0) return nullptr;

    HGLOBAL streamMemory = GlobalAlloc(GMEM_MOVEABLE, resourceSize);
    if (!streamMemory) return nullptr;

    void* streamData = GlobalLock(streamMemory);
    if (!streamData)
    {
        GlobalFree(streamMemory);
        return nullptr;
    }
    std::memcpy(streamData, resourceData, resourceSize);
    GlobalUnlock(streamMemory);

    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(streamMemory, TRUE, &stream)))
    {
        GlobalFree(streamMemory);
        return nullptr;
    }

    std::unique_ptr<Gdiplus::Bitmap> bitmap(Gdiplus::Bitmap::FromStream(stream));
    stream->Release();
    if (bitmap && bitmap->GetLastStatus() == Gdiplus::Ok) return bitmap;
    return nullptr;
}

std::unique_ptr<Gdiplus::Bitmap> CreateTintedBitmap(
    Gdiplus::Bitmap* bitmap,
    COLORREF tint)
{
    if (!bitmap || bitmap->GetWidth() == 0 || bitmap->GetHeight() == 0)
    {
        return nullptr;
    }
    auto tinted = std::make_unique<Gdiplus::Bitmap>(
        bitmap->GetWidth(),
        bitmap->GetHeight(),
        PixelFormat32bppARGB);
    const float red = static_cast<float>(GetRValue(tint)) / 255.0f;
    const float green = static_cast<float>(GetGValue(tint)) / 255.0f;
    const float blue = static_cast<float>(GetBValue(tint)) / 255.0f;
    Gdiplus::ColorMatrix matrix = {
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
        red, green, blue, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 0.0f, 1.0f
    };
    Gdiplus::ImageAttributes attributes;
    attributes.SetColorMatrix(
        &matrix,
        Gdiplus::ColorMatrixFlagsDefault,
        Gdiplus::ColorAdjustTypeBitmap);
    Gdiplus::Graphics graphics(tinted.get());
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
    Gdiplus::Rect destination(
        0,
        0,
        static_cast<INT>(bitmap->GetWidth()),
        static_cast<INT>(bitmap->GetHeight()));
    graphics.DrawImage(
        bitmap,
        destination,
        0,
        0,
        static_cast<INT>(bitmap->GetWidth()),
        static_cast<INT>(bitmap->GetHeight()),
        Gdiplus::UnitPixel,
        &attributes);
    return tinted;
}

struct PreviewProxyThreadResult
{
    unsigned long long generation = 0;
    unsigned long long clipId = 0;
    std::wstring sourceKey;
    std::filesystem::path proxyPath;
    std::wstring errorMessage;
};

COLORREF Blend(COLORREF first, COLORREF second, int secondPercent)
{
    const int percent = std::clamp(secondPercent, 0, 100);
    const int inverse = 100 - percent;
    return RGB(
        (GetRValue(first) * inverse + GetRValue(second) * percent) / 100,
        (GetGValue(first) * inverse + GetGValue(second) * percent) / 100,
        (GetBValue(first) * inverse + GetBValue(second) * percent) / 100);
}

rex::ui::Palette EditorComponentPalette(const MediaEditorTheme& theme)
{
    rex::ui::Palette palette;
    palette.pageBackground = theme.pageBackground;
    palette.inputBackground = theme.inputBackground;
    palette.buttonBackground = theme.buttonBackground;
    palette.buttonHover = Blend(theme.buttonBackground, theme.textPrimary, 7);
    palette.buttonPressed = Blend(theme.buttonBackground, theme.pageBackground, 18);
    palette.disabledBackground = Blend(theme.buttonBackground, theme.pageBackground, 52);
    palette.disabledText = Blend(theme.textSecondary, theme.pageBackground, 45);
    palette.dropdownBackground = theme.panelBackground;
    palette.dropdownHover = theme.panelHover;
    palette.dropdownSelected = Blend(theme.accentSoft, theme.accent, 14);
    palette.border = theme.border;
    palette.textPrimary = theme.textPrimary;
    palette.textSecondary = theme.textSecondary;
    palette.accent = theme.accent;
    palette.accentSoft = theme.accentSoft;
    palette.danger = theme.danger;
    palette.dangerAccent = Blend(theme.danger, theme.textPrimary, 18);
    palette.light =
        (GetRValue(theme.pageBackground) * 299 +
         GetGValue(theme.pageBackground) * 587 +
         GetBValue(theme.pageBackground) * 114) / 1000 >= 145;
    return palette;
}

bool Contains(const RECT& rect, POINT point)
{
    return point.x >= rect.left && point.x < rect.right &&
        point.y >= rect.top && point.y < rect.bottom;
}

bool HasArea(const RECT& rect)
{
    return rect.right > rect.left && rect.bottom > rect.top;
}

bool SameTheme(const MediaEditorTheme& first, const MediaEditorTheme& second)
{
    return first.pageBackground == second.pageBackground &&
        first.panelBackground == second.panelBackground &&
        first.panelHover == second.panelHover &&
        first.inputBackground == second.inputBackground &&
        first.buttonBackground == second.buttonBackground &&
        first.border == second.border &&
        first.textPrimary == second.textPrimary &&
        first.textSecondary == second.textSecondary &&
        first.accent == second.accent &&
        first.accentSoft == second.accentSoft &&
        first.warning == second.warning &&
        first.danger == second.danger;
}

RECT Inset(RECT rect, int horizontal, int vertical)
{
    rect.left += horizontal;
    rect.right -= horizontal;
    rect.top += vertical;
    rect.bottom -= vertical;
    return rect;
}

std::wstring WindowText(HWND window)
{
    if (!window)
    {
        return {};
    }
    const int length = GetWindowTextLengthW(window);
    std::wstring text(static_cast<size_t>(std::max(0, length)) + 1, L'\0');
    GetWindowTextW(window, text.data(), static_cast<int>(text.size()));
    text.resize(static_cast<size_t>(std::max(0, length)));
    return text;
}

std::wstring Lower(std::wstring value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](wchar_t character)
        {
            return static_cast<wchar_t>(std::towlower(character));
        });
    return value;
}

std::wstring PreviewCacheKey(const std::filesystem::path& path)
{
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(path, error);
    const std::wstring normalized = Lower((error ? path : absolute).lexically_normal().wstring());

    std::wostringstream key;
    key << L"preview-v2|" << normalized;
    error.clear();
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (!error)
    {
        key << L"|size=" << size;
    }
    else
    {
        key << L"|size=unknown";
    }

    error.clear();
    const std::filesystem::file_time_type modified = std::filesystem::last_write_time(path, error);
    if (!error)
    {
        key << L"|modified=" << modified.time_since_epoch().count();
    }
    return key.str();
}

std::filesystem::path CreatePreviewProxyDirectory()
{
    const std::filesystem::path root = CacheManager::MediaEditorTemporaryRoot();
    if (root.empty()) return {};
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (error)
    {
        return {};
    }
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        const std::filesystem::path candidate = root /
            (L"preview-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt));
        error.clear();
        if (std::filesystem::create_directory(candidate, error))
        {
            return candidate;
        }
    }
    return {};
}

void RemovePreviewPath(const std::filesystem::path& path, bool recursive = false)
{
    if (path.empty()) return;
    std::error_code error;
    if (recursive) std::filesystem::remove_all(path, error);
    else std::filesystem::remove(path, error);
}

bool CanUseNativeWindowsPreview(const VideoEditorClipModel& clip)
{
    const std::wstring extension = Lower(clip.sourcePath.extension().wstring());
    if (extension != L".mp4" && extension != L".m4v" && extension != L".mov")
    {
        return false;
    }
    if (Lower(clip.analysis.videoCodec) != L"h264")
    {
        return false;
    }
    const std::wstring pixelFormat = Lower(clip.analysis.pixelFormat);
    if (!pixelFormat.empty() &&
        pixelFormat != L"yuv420p" &&
        pixelFormat != L"yuvj420p" &&
        pixelFormat != L"nv12")
    {
        return false;
    }
    if (!clip.analysis.hasAudio)
    {
        return true;
    }
    const std::wstring audioCodec = Lower(clip.analysis.audioCodec);
    return audioCodec == L"aac" || audioCodec == L"mp3";
}

std::wstring FormatTime(double seconds)
{
    const int total = std::max(0, static_cast<int>(std::round(seconds)));
    const int hours = total / 3600;
    const int minutes = (total % 3600) / 60;
    const int remainder = total % 60;
    wchar_t buffer[32] {};
    if (hours > 0) swprintf_s(buffer, L"%d:%02d:%02d", hours, minutes, remainder);
    else swprintf_s(buffer, L"%d:%02d", minutes, remainder);
    return buffer;
}

HFONT CreateFontForDpi(UINT dpi, int pointSize, int weight, const wchar_t* face = L"Segoe UI")
{
    LOGFONTW font {};
    font.lfHeight = -MulDiv(pointSize, static_cast<int>(dpi), 72);
    font.lfWeight = weight;
    font.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(font.lfFaceName, face);
    return CreateFontIndirectW(&font);
}

void BuildRoundedPath(Gdiplus::GraphicsPath& path, const RECT& rect, int radius)
{
    const Gdiplus::REAL left = static_cast<Gdiplus::REAL>(rect.left);
    const Gdiplus::REAL top = static_cast<Gdiplus::REAL>(rect.top);
    const Gdiplus::REAL right = static_cast<Gdiplus::REAL>(rect.right);
    const Gdiplus::REAL bottom = static_cast<Gdiplus::REAL>(rect.bottom);
    const Gdiplus::REAL diameter = static_cast<Gdiplus::REAL>(std::max(0, radius * 2));
    path.StartFigure();
    path.AddArc(left, top, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(right - diameter, top, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(right - diameter, bottom - diameter, diameter, diameter, 0.0f, 90.0f);
    path.AddArc(left, bottom - diameter, diameter, diameter, 90.0f, 90.0f);
    path.CloseFigure();
}

void FillRounded(HDC hdc, const RECT& rect, int radius, COLORREF color, BYTE alpha = 255)
{
    if (!HasArea(rect)) return;
    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath path;
    BuildRoundedPath(path, rect, radius);
    Gdiplus::SolidBrush brush(Gdiplus::Color(
        alpha,
        GetRValue(color),
        GetGValue(color),
        GetBValue(color)));
    graphics.FillPath(&brush, &path);
}

void StrokeRounded(HDC hdc, const RECT& rect, int radius, COLORREF color, float width = 1.0f)
{
    if (!HasArea(rect)) return;
    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath path;
    BuildRoundedPath(path, rect, radius);
    Gdiplus::Pen pen(Gdiplus::Color(
        255,
        GetRValue(color),
        GetGValue(color),
        GetBValue(color)), width);
    graphics.DrawPath(&pen, &path);
}

void FillRectColor(HDC hdc, const RECT& rect, COLORREF color)
{
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(hdc, &rect, brush);
    DeleteObject(brush);
}

void DrawLabel(
    HDC hdc,
    const std::wstring& text,
    const RECT& rect,
    HFONT font,
    COLORREF color,
    UINT format)
{
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, color);
    HGDIOBJ oldFont = SelectObject(hdc, font);
    RECT copy = rect;
    DrawTextW(hdc, text.c_str(), static_cast<int>(text.size()), &copy, format | DT_NOPREFIX);
    SelectObject(hdc, oldFont);
}

Gdiplus::REAL FittedTextSize(
    Gdiplus::Graphics& graphics,
    const std::wstring& text,
    Gdiplus::REAL width,
    Gdiplus::REAL height)
{
    if (text.empty() || width <= 1.0f || height <= 1.0f)
    {
        return 2.0f;
    }

    Gdiplus::FontFamily preferredFamily(L"Segoe UI");
    const Gdiplus::FontFamily* family =
        preferredFamily.GetLastStatus() == Gdiplus::Ok
            ? &preferredFamily
            : Gdiplus::FontFamily::GenericSansSerif();
    Gdiplus::StringFormat format;
    format.SetFormatFlags(
        Gdiplus::StringFormatFlagsMeasureTrailingSpaces |
        Gdiplus::StringFormatFlagsLineLimit |
        Gdiplus::StringFormatFlagsNoClip);
    format.SetTrimming(Gdiplus::StringTrimmingNone);

    Gdiplus::REAL low = 2.0f;
    Gdiplus::REAL high = std::max(
        low,
        std::min<Gdiplus::REAL>(2048.0f, height));
    for (int pass = 0; pass < 15; ++pass)
    {
        const Gdiplus::REAL candidate = (low + high) * 0.5f;
        Gdiplus::Font font(
            family,
            candidate,
            Gdiplus::FontStyleRegular,
            Gdiplus::UnitPixel);
        Gdiplus::RectF measured;
        INT fitted = 0;
        INT lines = 0;
        const Gdiplus::REAL measurementHeight = std::max(
            height * 2.0f,
            height + candidate * 2.0f + 4.0f);
        const Gdiplus::Status measuredStatus = graphics.MeasureString(
            text.c_str(),
            static_cast<INT>(text.size()),
            &font,
            Gdiplus::RectF(0.0f, 0.0f, width, measurementHeight),
            &format,
            &measured,
            &fitted,
            &lines);
        const bool fits =
            measuredStatus == Gdiplus::Ok &&
            fitted >= static_cast<INT>(text.size()) &&
            measured.Width <= width + 0.75f &&
            measured.Height <= height + 0.75f;
        if (fits) low = candidate;
        else high = candidate;
    }
    return low;
}

void DrawTextBox(
    Gdiplus::Graphics& graphics,
    const MediaEditorTextBox& textBox)
{
    if (textBox.text.empty() ||
        textBox.bounds.Width() <= 1.0f ||
        textBox.bounds.Height() <= 1.0f)
    {
        return;
    }

    const int rotation =
        ((textBox.rotationQuarterTurns % 4) + 4) % 4;
    const bool sideways = (rotation % 2) != 0;
    const Gdiplus::REAL physicalWidth = textBox.bounds.Width();
    const Gdiplus::REAL physicalHeight = textBox.bounds.Height();
    const Gdiplus::REAL logicalWidth =
        sideways ? physicalHeight : physicalWidth;
    const Gdiplus::REAL logicalHeight =
        sideways ? physicalWidth : physicalHeight;
    const Gdiplus::REAL padding = std::clamp(
        std::min(logicalWidth, logicalHeight) * 0.035f,
        1.5f,
        18.0f);
    const Gdiplus::REAL contentWidth =
        std::max(1.0f, logicalWidth - padding * 2.0f);
    const Gdiplus::REAL contentHeight =
        std::max(1.0f, logicalHeight - padding * 2.0f);

    const Gdiplus::GraphicsState state = graphics.Save();
    graphics.SetClip(
        Gdiplus::RectF(
            textBox.bounds.left,
            textBox.bounds.top,
            physicalWidth,
            physicalHeight),
        Gdiplus::CombineModeIntersect);
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    graphics.TranslateTransform(
        (textBox.bounds.left + textBox.bounds.right) * 0.5f,
        (textBox.bounds.top + textBox.bounds.bottom) * 0.5f);
    graphics.RotateTransform(static_cast<Gdiplus::REAL>(rotation * 90));

    Gdiplus::FontFamily preferredFamily(L"Segoe UI");
    const Gdiplus::FontFamily* family =
        preferredFamily.GetLastStatus() == Gdiplus::Ok
            ? &preferredFamily
            : Gdiplus::FontFamily::GenericSansSerif();
    const Gdiplus::REAL fontSize =
        FittedTextSize(graphics, textBox.text, contentWidth, contentHeight);
    Gdiplus::Font font(
        family,
        fontSize,
        Gdiplus::FontStyleRegular,
        Gdiplus::UnitPixel);
    Gdiplus::StringFormat format;
    format.SetFormatFlags(
        Gdiplus::StringFormatFlagsMeasureTrailingSpaces |
        Gdiplus::StringFormatFlagsLineLimit);
    format.SetTrimming(Gdiplus::StringTrimmingNone);
    const Gdiplus::RectF layoutRect(
        -logicalWidth * 0.5f + padding,
        -logicalHeight * 0.5f + padding,
        contentWidth,
        contentHeight);
    Gdiplus::SolidBrush brush(Gdiplus::Color(
        static_cast<BYTE>(std::clamp(textBox.opacity, 0.0f, 1.0f) * 255.0f),
        GetRValue(textBox.color),
        GetGValue(textBox.color),
        GetBValue(textBox.color)));
    graphics.DrawString(
        textBox.text.c_str(),
        static_cast<INT>(textBox.text.size()),
        &font,
        layoutRect,
        &format,
        &brush);
    graphics.Restore(state);
}

bool HasVisibleText(const std::wstring& text)
{
    return std::any_of(
        text.begin(),
        text.end(),
        [](wchar_t character) { return std::iswspace(character) == 0; });
}

std::vector<std::filesystem::path> ParseOpenFileBuffer(const wchar_t* buffer)
{
    std::vector<std::filesystem::path> paths;
    if (!buffer || !*buffer)
    {
        return paths;
    }
    const std::filesystem::path first(buffer);
    const wchar_t* next = buffer + wcslen(buffer) + 1;
    if (!*next)
    {
        paths.push_back(first);
        return paths;
    }
    while (*next)
    {
        paths.push_back(first / next);
        next += wcslen(next) + 1;
    }
    return paths;
}
}

class MediaEditorPage::Impl
{
public:
    Impl() = default;
    ~Impl() { Shutdown(); }

    bool Create(HINSTANCE instance, HWND parent);
    void Destroy();
    void Shutdown();
    void SetVisible(bool visible);
    void SetBounds(const RECT& bounds, UINT dpi);
    void SetTheme(const MediaEditorTheme& theme);
    void SetDefaultOutputFolder(const std::filesystem::path& folder);
    void OpenFiles(const std::vector<std::filesystem::path>& paths);
    void PasteFromClipboard();
    void ResetToImport();
    bool IsBusy() const;
    HWND WindowHandle() const { return hwnd_; }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK PreviewWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK TextPopupWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK TextEditWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

private:
    int Dips(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }
    void RecreateFonts();
    void Layout();
    void UpdateChildWindows();
    void Paint(HDC hdc);
    void PaintBuffered(HDC target, const RECT& paintRect);
    void ReleasePaintBuffer();
    void PaintImport(HDC hdc);
    void PaintVideo(HDC hdc);
    void PaintImage(HDC hdc);
    void PaintExportOverlay(HDC hdc);
    void PaintKeybindsOverlay(HDC hdc);
    void PaintButton(HDC hdc, const RECT& rect, const std::wstring& label, bool enabled = true, bool primary = false, bool selected = false);
    void PaintIconButton(HDC hdc, const RECT& rect, int icon, const std::wstring& label, bool enabled = true, bool selected = false);
    void PaintProgress(HDC hdc, const RECT& rect, double value);
    void PaintImageCanvas(HDC hdc);
    void SetExportGpuEnabled(bool enabled);
    bool StepExportGpuToggleAnimation();

    void ChooseFiles(bool addToTimeline);
    void BeginImport(
        std::vector<std::filesystem::path> paths,
        std::optional<MediaEditorImageBuffer> clipboardImage,
        bool appendVideo);
    void FinishImport(std::unique_ptr<MediaImportThreadResult> result);
    void CancelImport();
    void UpdateClipboardAvailability();
    void LoadSelectedVideoPreview(bool seekToPlayhead = true);
    void BeginCompatibilityPreview(
        const VideoEditorClipModel& clip,
        double sourceSeconds);
    void FinishCompatibilityPreview(std::unique_ptr<PreviewProxyThreadResult> result);
    void CancelCompatibilityPreview();
    void ClearCompatibilityPreviews();
    bool OpenPreviewPath(
        const VideoEditorClipModel& clip,
        const std::filesystem::path& path,
        double sourceSeconds,
        bool compatibilityProxy);
    void SeekTimeline(double timelineSeconds);
    void ToggleTimelinePlayback();
    void StepTimelineFrame(int direction);
    bool AdvanceTimelinePlayback();
    void BeginVideoExport();
    void StartVideoExport(const std::filesystem::path& outputPath);
    void CancelVideoExport();
    void FinishExport(std::unique_ptr<VideoEditorExportResult> result);
    void OpenExportFile();
    void OpenExportFolder();
    std::optional<std::filesystem::path> ChooseVideoOutputPath();
    std::optional<std::pair<std::filesystem::path, ImageFormat>> ChooseImageOutputPath();
    void SaveImage(bool forceSaveAs);
    void CopyImage();

    void OnLeftButtonDown(POINT point, WPARAM keys);
    void OnMouseMove(POINT point, WPARAM keys);
    void OnLeftButtonUp(POINT point, WPARAM keys);
    void OnMouseWheel(POINT point, int delta, WPARAM keys);
    void OnKeyDown(WPARAM key, LPARAM flags);
    void OnDropFiles(HDROP drop);
    void AppendDrawingSamples(POINT point);
    void AppendDrawingPoint(MediaEditorPoint point);
    RECT HitButton(POINT point) const;
    void UpdateHover(POINT point);
    int TimelineClipAt(POINT point) const;
    double TimelineSecondsAtX(int x) const;
    int TimelineXAtSeconds(double seconds) const;
    double TimelineVisibleDuration() const;
    void ClampTimelineView();
    void SetTimelineZoom(double zoom, int anchorX);
    double SnappedTimelineStart(double desiredStart, int movingIndex) const;
    CropHandle CropHandleAt(POINT point) const;
    std::optional<MediaEditorPoint> CanvasToImage(POINT point) const;
    POINT ImageToCanvas(MediaEditorPoint point) const;
    void RebuildImagePreviewCache();
    void FitImageToCanvas();
    void ClampImagePan();
    void BeginTextEditing(MediaEditorCropRect bounds);
    void CommitTextEditing();
    void CancelTextEditing();
    void HandleTextEditNotification(WORD notification);
    void UpdateTextEditBounds();
    void UpdateTextEditFont();
    void SetCursorForPoint(POINT point);

    HINSTANCE instance_ = nullptr;
    HWND parent_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND previewHost_ = nullptr;
    HWND sizeLimitEdit_ = nullptr;
    HWND textPopup_ = nullptr;
    HWND textEdit_ = nullptr;
    WNDPROC originalTextEditProc_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    RECT textPopupScreenRect_ {};
    HBRUSH textEditBrush_ = nullptr;
    HFONT textEditFont_ = nullptr;
    bool finishingTextEdit_ = false;
    UINT dpi_ = 96;
    MediaEditorTheme theme_;
    RECT pageBounds_ {};
    bool hasPageBounds_ = false;
    HDC paintBufferDc_ = nullptr;
    HBITMAP paintBufferBitmap_ = nullptr;
    HBITMAP paintBufferPreviousBitmap_ = nullptr;
    SIZE paintBufferSize_ {};
    std::filesystem::path defaultOutputFolder_;

    HFONT headingFont_ = nullptr;
    HFONT sectionFont_ = nullptr;
    HFONT bodyFont_ = nullptr;
    HFONT smallFont_ = nullptr;
    HFONT monoFont_ = nullptr;

    EditorView view_ = EditorView::Import;
    bool visible_ = false;
    bool importBusy_ = false;
    bool exportOpen_ = false;
    bool exportBusy_ = false;
    bool exportComplete_ = false;
    bool keybindsOpen_ = false;
    bool contextPasteOpen_ = false;
    bool mouseTracking_ = false;
    bool spaceHeld_ = false;
    bool timelinePlaybackRequested_ = false;
    double timelineZoom_ = 1.0;
    double timelineViewStartSeconds_ = 0.0;
    std::wstring statusText_;
    ClipboardMediaAvailability clipboardAvailability_;

    std::atomic_bool importCancel_ { false };
    std::atomic_ullong importGeneration_ { 0 };
    std::thread importThread_;
    std::atomic_bool previewProxyCancel_ { false };
    std::atomic_ullong previewProxyGeneration_ { 0 };
    std::thread previewProxyThread_;
    bool previewProxyBusy_ = false;
    bool previewUsingProxy_ = false;
    unsigned long long previewProxyClipId_ = 0;
    double previewProxySeekSeconds_ = 0.0;
    ULONGLONG nativePreviewStartedAt_ = 0;
    unsigned long long nativePreviewClipId_ = 0;
    std::filesystem::path previewProxyDirectory_;
    std::map<std::wstring, std::filesystem::path> previewProxyCache_;
    std::atomic_bool exportCancel_ { false };
    std::thread exportThread_;
    VideoEditorExportProgress exportProgress_;
    VideoEditorExportResult exportResult_;
    ULONGLONG exportStartedAt_ = 0;
    ULONGLONG exportLastClockPaintAt_ = 0;
    bool exportGpuEnabled_ = true;
    rex::ui::SwitchAnimationState exportGpuToggleAnimation_ = rex::ui::MakeSwitchAnimationState(true);

    ClipboardMediaService clipboardService_;
    ImageEditingSession imageSession_;
    VideoTimelineModel timeline_;
    VideoPreviewService preview_;
    VideoEditorExportService exportService_;

    VideoEditorExportMode exportMode_ = VideoEditorExportMode::KeepOriginalQuality;
    bool exportSizeUnitGb_ = false;
    float videoVolume_ = 0.8f;
    std::filesystem::path lastImageSavePath_;
    ImageFormat lastImageSaveFormat_ = ImageFormat::Png;

    COLORREF drawingColor_ = RGB(255, 76, 96);
    float drawingThickness_ = 6.0f;
    float drawingOpacity_ = 1.0f;
    bool eraserEnabled_ = false;
    bool textToolEnabled_ = false;
    bool textEditing_ = false;
    ImageToolbarPopup imageToolbarPopup_ = ImageToolbarPopup::None;
    float imageScale_ = 1.0f;
    bool imageFitMode_ = true;
    Gdiplus::PointF imagePan_ { 0.0f, 0.0f };
    RECT imageDisplayRect_ {};
    MediaEditorDrawingStroke currentStroke_;
    MediaEditorTextBox activeTextBox_;
    MediaEditorCropRect pendingTextBounds_ {};
    MediaEditorPoint textDragStart_ {};
    bool imageEraseChanged_ = false;
    std::unique_ptr<Gdiplus::Bitmap> imagePreviewCache_;
    std::unique_ptr<Gdiplus::Bitmap> eraserIcon_;
    std::unique_ptr<Gdiplus::Bitmap> textIcon_;
    std::unique_ptr<Gdiplus::Bitmap> textIconTinted_;
    std::unique_ptr<Gdiplus::Bitmap> galleryIcon_;
    std::unique_ptr<Gdiplus::Bitmap> galleryIconTinted_;

    PointerAction pointerAction_ = PointerAction::None;
    CropHandle activeCropHandle_ = CropHandle::None;
    POINT pointerStart_ {};
    Gdiplus::PointF panStart_ {};
    MediaEditorCropRect cropStart_ {};
    DWORD drawingStartTime_ = 0;
    DWORD lastDrawingSampleTime_ = 0;
    POINT lastDrawingScreenPoint_ {};
    double trimStartIn_ = 0.0;
    double trimStartOut_ = 0.0;
    double trimPixelsPerSecond_ = 1.0;
    int timelineScrollStartX_ = 0;
    double timelineScrollStartSeconds_ = 0.0;
    int dragClipIndex_ = -1;
    double dragStartTimelineSeconds_ = 0.0;
    double dragGrabOffsetSeconds_ = 0.0;
    bool dragMoved_ = false;
    unsigned long long dragInsertTargetId_ = 0;
    bool dragInsertAfter_ = false;
    RECT hoveredRect_ {};
    RECT pressedRect_ {};

    RECT clientRect_ {};
    RECT importDropRect_ {};
    RECT chooseFileRect_ {};
    RECT pasteRect_ {};
    RECT contextPasteRect_ {};
    RECT newEditRect_ {};
    RECT keybindsRect_ {};
    RECT undoRect_ {};
    RECT redoRect_ {};
    RECT previewPanelRect_ {};
    RECT previewVideoRect_ {};
    RECT playRect_ {};
    RECT muteRect_ {};
    RECT volumeRect_ {};
    RECT timelinePanelRect_ {};
    RECT timelineTrackRect_ {};
    RECT timelineScrollbarRect_ {};
    RECT timelineScrollbarThumbRect_ {};
    std::vector<RECT> timelineClipRects_;
    std::vector<RECT> timelineLeftHandleRects_;
    std::vector<RECT> timelineRightHandleRects_;
    RECT addClipRect_ {};
    RECT splitRect_ {};
    RECT deleteRect_ {};
    RECT moveLeftRect_ {};
    RECT moveRightRect_ {};
    RECT exportRect_ {};

    RECT imageToolbarRect_ {};
    RECT imageCanvasRect_ {};
    RECT drawRect_ {};
    RECT colorRect_ {};
    RECT colorPopupRect_ {};
    RECT colorCustomRect_ {};
    std::array<RECT, 5> colorSwatchRects_ {};
    RECT thicknessRect_ {};
    RECT thicknessPopupRect_ {};
    RECT thicknessSliderRect_ {};
    std::array<RECT, 3> thicknessRects_ {};
    RECT opacityRect_ {};
    RECT eraserRect_ {};
    RECT textToolRect_ {};
    RECT rotateRect_ {};
    RECT resetCropRect_ {};
    RECT fitRect_ {};
    RECT actualSizeRect_ {};
    RECT zoomPopupRect_ {};
    std::array<RECT, 6> zoomOptionRects_ {};
    RECT copyRect_ {};
    RECT saveRect_ {};
    RECT saveMenuRect_ {};
    RECT savePopupRect_ {};
    RECT saveAsRect_ {};
    std::array<LONG, 4> imageToolbarSeparatorXs_ {};
    std::array<RECT, 8> cropHandleRects_ {};
    RECT cropMoveRect_ {};

    RECT exportOverlayRect_ {};
    std::array<RECT, 3> exportModeRects_ {};
    RECT exportSizeEditRect_ {};
    RECT exportSizeUnitRect_ {};
    RECT exportGpuRowRect_ {};
    RECT exportGpuToggleRect_ {};
    RECT exportCancelRect_ {};
    RECT exportStartRect_ {};
    RECT exportOpenFileRect_ {};
    RECT exportOpenFolderRect_ {};
    RECT exportDoneRect_ {};
    RECT keybindsOverlayRect_ {};
    RECT keybindsCloseRect_ {};
};

MediaEditorPage::MediaEditorPage()
    : impl_(std::make_unique<Impl>())
{
}

MediaEditorPage::~MediaEditorPage() = default;

bool MediaEditorPage::Create(HINSTANCE instance, HWND parent) { return impl_->Create(instance, parent); }
void MediaEditorPage::Destroy() { impl_->Destroy(); }
void MediaEditorPage::Shutdown() { impl_->Shutdown(); }
void MediaEditorPage::SetVisible(bool visible) { impl_->SetVisible(visible); }
void MediaEditorPage::SetBounds(const RECT& bounds, UINT dpi) { impl_->SetBounds(bounds, dpi); }
void MediaEditorPage::SetTheme(const MediaEditorTheme& theme) { impl_->SetTheme(theme); }
void MediaEditorPage::SetDefaultOutputFolder(const std::filesystem::path& folder) { impl_->SetDefaultOutputFolder(folder); }
void MediaEditorPage::OpenFiles(const std::vector<std::filesystem::path>& paths) { impl_->OpenFiles(paths); }
void MediaEditorPage::PasteFromClipboard() { impl_->PasteFromClipboard(); }
void MediaEditorPage::ResetToImport() { impl_->ResetToImport(); }
HWND MediaEditorPage::WindowHandle() const { return impl_->WindowHandle(); }
bool MediaEditorPage::IsBusy() const { return impl_->IsBusy(); }

bool MediaEditorPage::Impl::Create(HINSTANCE instance, HWND parent)
{
    if (hwnd_)
    {
        return true;
    }
    instance_ = instance;
    eraserIcon_ = LoadPngResource(instance_, IDR_MEDIA_EDITOR_ERASER_ICON);
    textIcon_ = LoadPngResource(instance_, IDR_MEDIA_EDITOR_TEXT_ICON);
    textIconTinted_ = CreateTintedBitmap(textIcon_.get(), theme_.textPrimary);
    galleryIcon_ = LoadPngResource(instance_, IDR_GALLERY_ICON);
    galleryIconTinted_ = CreateTintedBitmap(galleryIcon_.get(), theme_.textPrimary);
    parent_ = parent;

    WNDCLASSEXW windowClass {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kMediaEditorPageClass;
    if (!GetClassInfoExW(instance, kMediaEditorPageClass, &windowClass))
    {
        if (!RegisterClassExW(&windowClass))
        {
            return false;
        }
    }

    WNDCLASSEXW previewClass {};
    previewClass.cbSize = sizeof(previewClass);
    previewClass.lpfnWndProc = PreviewWindowProc;
    previewClass.hInstance = instance;
    previewClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    previewClass.lpszClassName = kMediaEditorPreviewClass;
    if (!GetClassInfoExW(instance, kMediaEditorPreviewClass, &previewClass) &&
        !RegisterClassExW(&previewClass))
    {
        return false;
    }

    WNDCLASSEXW textPopupClass {};
    textPopupClass.cbSize = sizeof(textPopupClass);
    textPopupClass.lpfnWndProc = TextPopupWindowProc;
    textPopupClass.hInstance = instance;
    textPopupClass.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    textPopupClass.lpszClassName = kMediaEditorTextPopupClass;
    if (!GetClassInfoExW(instance, kMediaEditorTextPopupClass, &textPopupClass) &&
        !RegisterClassExW(&textPopupClass))
    {
        return false;
    }

    hwnd_ = CreateWindowExW(
        WS_EX_CONTROLPARENT,
        kMediaEditorPageClass,
        L"",
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_TABSTOP,
        0,
        0,
        1,
        1,
        parent,
        nullptr,
        instance,
        this);
    return hwnd_ != nullptr;
}

void MediaEditorPage::Impl::Destroy()
{
    Shutdown();
    if (hwnd_)
    {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void MediaEditorPage::Impl::Shutdown()
{
    importCancel_ = true;
    exportCancel_ = true;
    ClearCompatibilityPreviews();
    preview_.Shutdown();
    if (importThread_.joinable()) importThread_.join();
    if (exportThread_.joinable()) exportThread_.join();
    imagePreviewCache_.reset();
    eraserIcon_.reset();
    textIcon_.reset();
    textIconTinted_.reset();
    galleryIcon_.reset();
    galleryIconTinted_.reset();
    importBusy_ = false;
    exportBusy_ = false;
}

void MediaEditorPage::Impl::SetVisible(bool visible)
{
    if (!hwnd_) return;
    if (visible_ == visible)
    {
        return;
    }
    if (!visible && textEditing_)
    {
        CommitTextEditing();
    }
    visible_ = visible;
    ShowWindow(hwnd_, visible ? SW_SHOW : SW_HIDE);
    if (!visible)
    {
        timelinePlaybackRequested_ = false;
        keybindsOpen_ = false;
        preview_.Pause();
        ShowWindow(previewHost_, SW_HIDE);
        ShowWindow(sizeLimitEdit_, SW_HIDE);
        ShowWindow(textPopup_, SW_HIDE);
    }
    else
    {
        UpdateClipboardAvailability();
        UpdateChildWindows();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void MediaEditorPage::Impl::SetBounds(const RECT& bounds, UINT dpi)
{
    if (!hwnd_) return;
    const UINT newDpi = std::max<UINT>(96, dpi);
    const bool dpiChanged = dpi_ != newDpi;
    const bool boundsChanged = !hasPageBounds_ || !EqualRect(&pageBounds_, &bounds);
    if (!dpiChanged && !boundsChanged)
    {
        return;
    }
    pageBounds_ = bounds;
    hasPageBounds_ = true;
    if (dpiChanged)
    {
        dpi_ = newDpi;
        RecreateFonts();
    }
    SetWindowPos(
        hwnd_,
        nullptr,
        bounds.left,
        bounds.top,
        std::max(1L, bounds.right - bounds.left),
        std::max(1L, bounds.bottom - bounds.top),
        SWP_NOZORDER | SWP_NOACTIVATE);
    GetClientRect(hwnd_, &clientRect_);
    Layout();
    UpdateChildWindows();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MediaEditorPage::Impl::SetTheme(const MediaEditorTheme& theme)
{
    if (SameTheme(theme_, theme))
    {
        return;
    }
    theme_ = theme;
    if (editBrush_) DeleteObject(editBrush_);
    editBrush_ = CreateSolidBrush(theme_.inputBackground);
    textIconTinted_ = CreateTintedBitmap(textIcon_.get(), theme_.textPrimary);
    galleryIconTinted_ = CreateTintedBitmap(galleryIcon_.get(), theme_.textPrimary);
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    if (textEdit_) InvalidateRect(textEdit_, nullptr, TRUE);
}

void MediaEditorPage::Impl::SetDefaultOutputFolder(const std::filesystem::path& folder)
{
    if (defaultOutputFolder_ == folder)
    {
        return;
    }
    defaultOutputFolder_ = folder;
}

void MediaEditorPage::Impl::SetExportGpuEnabled(bool enabled)
{
    exportGpuEnabled_ = enabled;
    const bool started = rex::ui::SetSwitchTarget(
        exportGpuToggleAnimation_,
        enabled,
        exportGpuToggleRect_);
    if (hwnd_)
    {
        if (started || exportGpuToggleAnimation_.animating)
        {
            SetTimer(
                hwnd_,
                kControlAnimationTimerId,
                kControlAnimationTimerMs,
                nullptr);
        }
        else
        {
            KillTimer(hwnd_, kControlAnimationTimerId);
        }

        RECT dirty = exportGpuRowRect_;
        InflateRect(&dirty, Dips(3), Dips(3));
        InvalidateRect(hwnd_, &dirty, FALSE);
    }
}

bool MediaEditorPage::Impl::StepExportGpuToggleAnimation()
{
    const bool keepAnimating = rex::ui::StepSwitchAnimation(
        exportGpuToggleAnimation_);
    RECT dirty = exportGpuRowRect_;
    InflateRect(&dirty, Dips(3), Dips(3));
    InvalidateRect(hwnd_, &dirty, FALSE);
    return keepAnimating;
}

bool MediaEditorPage::Impl::IsBusy() const
{
    return importBusy_ || exportBusy_ || previewProxyBusy_;
}

void MediaEditorPage::Impl::RecreateFonts()
{
    for (HFONT font : { headingFont_, sectionFont_, bodyFont_, smallFont_, monoFont_ })
    {
        if (font) DeleteObject(font);
    }
    headingFont_ = CreateFontForDpi(dpi_, 18, FW_SEMIBOLD, L"Bahnschrift SemiBold");
    sectionFont_ = CreateFontForDpi(dpi_, 12, FW_SEMIBOLD, L"Bahnschrift SemiBold");
    bodyFont_ = CreateFontForDpi(dpi_, 11, FW_NORMAL);
    smallFont_ = CreateFontForDpi(dpi_, 9, FW_NORMAL);
    monoFont_ = CreateFontForDpi(dpi_, 10, FW_NORMAL, L"Cascadia Mono");
    if (sizeLimitEdit_)
    {
        SendMessageW(sizeLimitEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont_), TRUE);
    }
    if (textEdit_ && textEditing_)
    {
        UpdateTextEditFont();
    }
}

LRESULT CALLBACK MediaEditorPage::Impl::WindowProc(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    Impl* page = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        page = static_cast<Impl*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(page));
        if (page) page->hwnd_ = window;
    }
    if (page)
    {
        return page->HandleMessage(message, wParam, lParam);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK MediaEditorPage::Impl::PreviewWindowProc(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    Impl* page = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        page = static_cast<Impl*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(page));
    }
    switch (message)
    {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT paint {};
        HDC hdc = BeginPaint(window, &paint);
        if (!page || !page->preview_.IsReady())
        {
            RECT client {};
            GetClientRect(window, &client);
            FillRectColor(hdc, client, RGB(8, 10, 14));
        }
        EndPaint(window, &paint);
        if (page) page->preview_.UpdateVideo();
        return 0;
    }
    case WM_SIZE:
        if (page) page->preview_.UpdateVideo();
        return 0;
    case WM_LBUTTONDOWN:
        if (page &&
            page->visible_ &&
            page->view_ == EditorView::Video &&
            !page->exportOpen_ &&
            !page->exportBusy_ &&
            !page->keybindsOpen_)
        {
            SetFocus(page->hwnd_);
            page->ToggleTimelinePlayback();
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT)
        {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        if (page) page->preview_.UpdateVideo();
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK MediaEditorPage::Impl::TextEditWindowProc(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    Impl* page = reinterpret_cast<Impl*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (page)
    {
        if (message == WM_ERASEBKGND && page->textEditBrush_)
        {
            RECT client {};
            GetClientRect(window, &client);
            FillRect(reinterpret_cast<HDC>(wParam), &client, page->textEditBrush_);
            return 1;
        }
        if (message == WM_PAINT && page->textEditBrush_)
        {
            PAINTSTRUCT paint {};
            HDC dc = BeginPaint(window, &paint);
            RECT client {};
            GetClientRect(window, &client);
            FillRect(dc, &client, page->textEditBrush_);
            EndPaint(window, &paint);
            return 0;
        }
        if (message == WM_PRINTCLIENT && page->textEditBrush_)
        {
            RECT client {};
            GetClientRect(window, &client);
            FillRect(reinterpret_cast<HDC>(wParam), &client, page->textEditBrush_);
            return 0;
        }
        if (message == WM_CHAR &&
            wParam == L'\r' &&
            (GetKeyState(VK_SHIFT) & 0x8000) == 0)
        {
            return 0;
        }
        if (message == WM_KEYDOWN &&
            wParam == 'A' &&
            (GetKeyState(VK_CONTROL) & 0x8000) != 0)
        {
            SendMessageW(window, EM_SETSEL, 0, -1);
            return 0;
        }
        if (message == WM_KEYDOWN && wParam == VK_ESCAPE)
        {
            page->CancelTextEditing();
            SetFocus(page->hwnd_);
            return 0;
        }
        if (message == WM_KEYDOWN &&
            wParam == VK_RETURN &&
            (GetKeyState(VK_SHIFT) & 0x8000) == 0)
        {
            page->CommitTextEditing();
            SetFocus(page->hwnd_);
            return 0;
        }
        if (message == WM_GETDLGCODE)
        {
            const LRESULT original = page->originalTextEditProc_
                ? CallWindowProcW(
                    page->originalTextEditProc_,
                    window,
                    message,
                    wParam,
                    lParam)
                : 0;
            return original | DLGC_WANTALLKEYS | DLGC_WANTCHARS;
        }
        if (page->originalTextEditProc_)
        {
            return CallWindowProcW(
                page->originalTextEditProc_,
                window,
                message,
                wParam,
                lParam);
        }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK MediaEditorPage::Impl::TextPopupWindowProc(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    Impl* page = reinterpret_cast<Impl*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        page = static_cast<Impl*>(create->lpCreateParams);
        SetWindowLongPtrW(
            window,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(page));
    }

    switch (message)
    {
    case WM_ERASEBKGND:
        if (page && page->textEditBrush_)
        {
            RECT client {};
            GetClientRect(window, &client);
            FillRect(reinterpret_cast<HDC>(wParam), &client, page->textEditBrush_);
            return 1;
        }
        break;

    case WM_PAINT:
        if (page && page->textEditBrush_)
        {
            PAINTSTRUCT paint {};
            HDC dc = BeginPaint(window, &paint);
            RECT client {};
            GetClientRect(window, &client);
            FillRect(dc, &client, page->textEditBrush_);
            EndPaint(window, &paint);
            return 0;
        }
        break;

    case WM_SIZE:
        if (page && page->textEdit_)
        {
            MoveWindow(
                page->textEdit_,
                0,
                0,
                std::max(1, static_cast<int>(LOWORD(lParam))),
                std::max(1, static_cast<int>(HIWORD(lParam))),
                TRUE);
        }
        return 0;

    case WM_COMMAND:
        if (page && reinterpret_cast<HWND>(lParam) == page->textEdit_)
        {
            page->HandleTextEditNotification(HIWORD(wParam));
            return 0;
        }
        break;

    case WM_CTLCOLOREDIT:
        if (page && reinterpret_cast<HWND>(lParam) == page->textEdit_)
        {
            HDC editDc = reinterpret_cast<HDC>(wParam);
            // The canvas renders the exact fitted text used by copy and save.
            // Keep native glyphs color-keyed while retaining edit input and its caret.
            SetTextColor(editDc, kTextEditColorKey);
            SetBkColor(editDc, kTextEditColorKey);
            SetBkMode(editDc, OPAQUE);
            return reinterpret_cast<LRESULT>(page->textEditBrush_);
        }
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT MediaEditorPage::Impl::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
        dpi_ = std::max<UINT>(96, GetDpiForWindow(hwnd_));
        RecreateFonts();
        editBrush_ = CreateSolidBrush(theme_.inputBackground);
        previewHost_ = CreateWindowExW(
            0,
            kMediaEditorPreviewClass,
            L"",
            WS_CHILD | WS_CLIPSIBLINGS,
            0,
            0,
            1,
            1,
            hwnd_,
            nullptr,
            instance_,
            this);
        sizeLimitEdit_ = CreateWindowExW(
            0,
            L"EDIT",
            L"300",
            WS_CHILD | WS_TABSTOP | ES_LEFT | ES_NUMBER | ES_AUTOHSCROLL,
            0,
            0,
            1,
            1,
            hwnd_,
            nullptr,
            instance_,
            nullptr);
        SendMessageW(sizeLimitEdit_, EM_SETLIMITTEXT, 7, 0);
        SendMessageW(sizeLimitEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont_), TRUE);
        SendMessageW(sizeLimitEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(Dips(10), Dips(8)));
        textEditBrush_ = CreateSolidBrush(kTextEditColorKey);
        textPopup_ = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW,
            kMediaEditorTextPopupClass,
            L"",
            WS_POPUP,
            0,
            0,
            1,
            1,
            GetAncestor(hwnd_, GA_ROOT),
            nullptr,
            instance_,
            this);
        if (textPopup_)
        {
            textEdit_ = CreateWindowExW(
                0,
                L"EDIT",
                L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_LEFT | ES_MULTILINE |
                    ES_WANTRETURN | ES_NOHIDESEL | ES_AUTOVSCROLL,
                0,
                0,
                1,
                1,
                textPopup_,
                nullptr,
                instance_,
                nullptr);
        }
        if (textEdit_)
        {
            SetWindowLongPtrW(
                textEdit_,
                GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(this));
            originalTextEditProc_ = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(
                    textEdit_,
                    GWLP_WNDPROC,
                    reinterpret_cast<LONG_PTR>(TextEditWindowProc)));
            SendMessageW(textEdit_, EM_SETLIMITTEXT, 8192, 0);
            SendMessageW(textEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(Dips(3), Dips(3)));
        }
        if (textPopup_)
        {
            SetLayeredWindowAttributes(textPopup_, kTextEditColorKey, 255, LWA_COLORKEY);
        }
        preview_.Initialize(previewHost_, hwnd_, kPreviewReadyMessage, kPreviewErrorMessage);
        DragAcceptFiles(hwnd_, TRUE);
        SetTimer(hwnd_, kClipboardTimerId, kClipboardTimerMs, nullptr);
        SetTimer(hwnd_, kPreviewTimerId, kPreviewTimerMs, nullptr);
        statusText_ = L"Drop a video or image to begin.";
        return 0;

    case WM_SIZE:
        GetClientRect(hwnd_, &clientRect_);
        ReleasePaintBuffer();
        Layout();
        UpdateChildWindows();
        return 0;

    case WM_DPICHANGED:
        dpi_ = HIWORD(wParam);
        RecreateFonts();
        Layout();
        UpdateChildWindows();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;

    case WM_PAINT:
    {
        PAINTSTRUCT paint {};
        HDC hdc = BeginPaint(hwnd_, &paint);
        PaintBuffered(hdc, paint.rcPaint);
        EndPaint(hwnd_, &paint);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_TIMER:
        if (wParam == kClipboardTimerId)
        {
            if (visible_ && view_ == EditorView::Import) UpdateClipboardAvailability();
            return 0;
        }
        if (wParam == kPreviewTimerId)
        {
            if (visible_ && view_ == EditorView::Video)
            {
                if (!preview_.IsReady() &&
                    !previewProxyBusy_ &&
                    !previewUsingProxy_ &&
                    nativePreviewStartedAt_ != 0 &&
                    GetTickCount64() - nativePreviewStartedAt_ >= kNativePreviewTimeoutMs)
                {
                    const int selected = timeline_.SelectedIndex();
                    if (selected >= 0 &&
                        selected < static_cast<int>(timeline_.Clips().size()) &&
                        timeline_.Clips()[selected].id == nativePreviewClipId_)
                    {
                        const VideoEditorClipModel& clip = timeline_.Clips()[selected];
                        nativePreviewStartedAt_ = 0;
                        BeginCompatibilityPreview(clip, previewProxySeekSeconds_);
                    }
                }
                const bool reachedClipEnd = preview_.Tick();
                if (reachedClipEnd && timelinePlaybackRequested_)
                {
                    AdvanceTimelinePlayback();
                }
                else if (timelinePlaybackRequested_ && timeline_.SelectedIndex() >= 0 &&
                    !preview_.IsNavigationPending())
                {
                    const int index = timeline_.SelectedIndex();
                    const auto& clip = timeline_.Clips()[index];
                    timeline_.SetPlayhead(
                        timeline_.ClipStartTime(index) +
                        std::clamp(preview_.PositionSeconds() - clip.trimInSeconds, 0.0, clip.Duration()));
                    RECT timelineDirty = timelineTrackRect_;
                    InflateRect(&timelineDirty, Dips(4), Dips(4));
                    InvalidateRect(hwnd_, &timelineDirty, FALSE);
                    RECT previewControls = previewPanelRect_;
                    previewControls.top = previewVideoRect_.bottom;
                    InvalidateRect(hwnd_, &previewControls, FALSE);
                }
            }
            if (visible_ && textEditing_)
            {
                UpdateTextEditBounds();
            }
            if (visible_ && exportBusy_ && exportStartedAt_ != 0)
            {
                const ULONGLONG now = GetTickCount64();
                exportProgress_.elapsedSeconds = std::max(
                    exportProgress_.elapsedSeconds,
                    static_cast<double>(now - exportStartedAt_) / 1000.0);
                if (now - exportLastClockPaintAt_ >= 200)
                {
                    exportLastClockPaintAt_ = now;
                    InvalidateRect(hwnd_, &exportOverlayRect_, FALSE);
                }
            }
            return 0;
        }
        if (wParam == kControlAnimationTimerId)
        {
            if (!StepExportGpuToggleAnimation())
            {
                KillTimer(hwnd_, kControlAnimationTimerId);
            }
            return 0;
        }
        break;

    case WM_DROPFILES:
        OnDropFiles(reinterpret_cast<HDROP>(wParam));
        return 0;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    {
        SetFocus(hwnd_);
        POINT point { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        OnLeftButtonDown(point, wParam);
        return 0;
    }

    case WM_MOUSEMOVE:
    {
        POINT point { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        OnMouseMove(point, wParam);
        if (!mouseTracking_)
        {
            TRACKMOUSEEVENT track { sizeof(track), TME_LEAVE, hwnd_, 0 };
            TrackMouseEvent(&track);
            mouseTracking_ = true;
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        mouseTracking_ = false;
        {
            const RECT oldHover = hoveredRect_;
            hoveredRect_ = {};
            if (HasArea(oldHover)) InvalidateRect(hwnd_, &oldHover, FALSE);
        }
        return 0;

    case WM_LBUTTONUP:
    {
        POINT point { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        OnLeftButtonUp(point, wParam);
        return 0;
    }

    case WM_CAPTURECHANGED:
        if (pointerAction_ != PointerAction::None)
        {
            if (pointerAction_ == PointerAction::ImageCrop) imageSession_.CancelEdit();
            if (pointerAction_ == PointerAction::ImageTextBox)
            {
                pendingTextBounds_ = {};
            }
            if (pointerAction_ == PointerAction::ImageDraw)
            {
                if (currentStroke_.eraser) imageSession_.CancelEdit();
                currentStroke_ = {};
                imageEraseChanged_ = false;
            }
            if (pointerAction_ == PointerAction::ImageDraw) drawingStartTime_ = lastDrawingSampleTime_ = 0;
            if (pointerAction_ == PointerAction::TimelineTrimLeft || pointerAction_ == PointerAction::TimelineTrimRight)
            {
                timeline_.CancelEdit();
            }
            if (pointerAction_ == PointerAction::TimelineReorder)
            {
                timeline_.CancelEdit();
                dragClipIndex_ = -1;
                dragMoved_ = false;
                dragInsertTargetId_ = 0;
                dragInsertAfter_ = false;
                Layout();
            }
            pointerAction_ = PointerAction::None;
            pressedRect_ = {};
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;

    case WM_MOUSEWHEEL:
    {
        POINT point { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd_, &point);
        OnMouseWheel(point, GET_WHEEL_DELTA_WPARAM(wParam), LOWORD(wParam));
        return 0;
    }

    case WM_KEYDOWN:
        OnKeyDown(wParam, lParam);
        return 0;

    case WM_KEYUP:
        if (wParam == VK_SPACE) spaceHeld_ = false;
        return 0;

    case WM_CONTEXTMENU:
    {
        if (view_ == EditorView::Import && clipboardAvailability_.HasCompatibleMedia())
        {
            POINT point { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (point.x == -1 && point.y == -1)
            {
                point = { importDropRect_.left + Dips(24), importDropRect_.top + Dips(24) };
            }
            else
            {
                ScreenToClient(hwnd_, &point);
            }
            contextPasteRect_ = { point.x, point.y, point.x + Dips(150), point.y + Dips(42) };
            contextPasteOpen_ = true;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    }

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT)
        {
            POINT point {};
            GetCursorPos(&point);
            ScreenToClient(hwnd_, &point);
            SetCursorForPoint(point);
            return TRUE;
        }
        break;

    case WM_COMMAND:
        if (reinterpret_cast<HWND>(lParam) == textEdit_)
        {
            HandleTextEditNotification(HIWORD(wParam));
            return 0;
        }
        break;

    case WM_CTLCOLOREDIT:
    {
        HDC editDc = reinterpret_cast<HDC>(wParam);
        if (reinterpret_cast<HWND>(lParam) == textEdit_)
        {
            SetTextColor(editDc, activeTextBox_.color);
            SetBkColor(editDc, kTextEditColorKey);
            SetBkMode(editDc, OPAQUE);
            return reinterpret_cast<LRESULT>(textEditBrush_);
        }
        SetTextColor(editDc, theme_.textPrimary);
        SetBkColor(editDc, theme_.inputBackground);
        return reinterpret_cast<LRESULT>(editBrush_);
    }

    case kImportFinishedMessage:
        FinishImport(std::unique_ptr<MediaImportThreadResult>(
            reinterpret_cast<MediaImportThreadResult*>(lParam)));
        return 0;

    case kExportProgressMessage:
    {
        std::unique_ptr<VideoEditorExportProgress> progress(
            reinterpret_cast<VideoEditorExportProgress*>(lParam));
        if (progress) exportProgress_ = *progress;
        InvalidateRect(hwnd_, &exportOverlayRect_, FALSE);
        return 0;
    }

    case kExportFinishedMessage:
        FinishExport(std::unique_ptr<VideoEditorExportResult>(
            reinterpret_cast<VideoEditorExportResult*>(lParam)));
        return 0;

    case kPreviewReadyMessage:
        nativePreviewStartedAt_ = 0;
        statusText_.clear();
        if (view_ == EditorView::Video && preview_.IsReady())
        {
            const int selected = timeline_.SelectedIndex();
            const auto& clips = timeline_.Clips();
            if (selected >= 0 && selected < static_cast<int>(clips.size()))
            {
                const auto& clip = clips[selected];
                const double clipOffset = std::clamp(
                    preview_.PositionSeconds() - clip.trimInSeconds,
                    0.0,
                    clip.Duration());
                timeline_.SetPlayhead(clip.TimelineStart() + clipOffset);
                InvalidateRect(hwnd_, &timelinePanelRect_, FALSE);
            }
        }
        if (timelinePlaybackRequested_ && preview_.IsReady() && !preview_.IsPlaying())
        {
            preview_.Play();
        }
        UpdateChildWindows();
        InvalidateRect(previewHost_, nullptr, FALSE);
        InvalidateRect(hwnd_, &previewPanelRect_, FALSE);
        return 0;

    case kPreviewErrorMessage:
        if (previewProxyBusy_)
        {
            return 0;
        }
        if (!previewUsingProxy_ && view_ == EditorView::Video && timeline_.SelectedIndex() >= 0)
        {
            const VideoEditorClipModel& clip = timeline_.Clips()[timeline_.SelectedIndex()];
            BeginCompatibilityPreview(
                clip,
                std::clamp(preview_.PositionSeconds(), clip.trimInSeconds, clip.trimOutSeconds));
            return 0;
        }
        timelinePlaybackRequested_ = false;
        InvalidateRect(hwnd_, &playRect_, FALSE);
        statusText_ = preview_.ErrorMessage();
        UpdateChildWindows();
        InvalidateRect(hwnd_, &previewPanelRect_, FALSE);
        return 0;

    case kPreviewProxyFinishedMessage:
        FinishCompatibilityPreview(std::unique_ptr<PreviewProxyThreadResult>(
            reinterpret_cast<PreviewProxyThreadResult*>(lParam)));
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd_, kClipboardTimerId);
        KillTimer(hwnd_, kPreviewTimerId);
        KillTimer(hwnd_, kControlAnimationTimerId);
        ClearCompatibilityPreviews();
        preview_.Shutdown();
        if (textPopup_ && IsWindow(textPopup_))
        {
            DestroyWindow(textPopup_);
        }
        textPopup_ = nullptr;
        textEdit_ = nullptr;
        originalTextEditProc_ = nullptr;
        ReleasePaintBuffer();
        if (editBrush_) { DeleteObject(editBrush_); editBrush_ = nullptr; }
        if (textEditBrush_) { DeleteObject(textEditBrush_); textEditBrush_ = nullptr; }
        if (textEditFont_) { DeleteObject(textEditFont_); textEditFont_ = nullptr; }
        if (headingFont_) { DeleteObject(headingFont_); headingFont_ = nullptr; }
        if (sectionFont_) { DeleteObject(sectionFont_); sectionFont_ = nullptr; }
        if (bodyFont_) { DeleteObject(bodyFont_); bodyFont_ = nullptr; }
        if (smallFont_) { DeleteObject(smallFont_); smallFont_ = nullptr; }
        if (monoFont_) { DeleteObject(monoFont_); monoFont_ = nullptr; }
        return 0;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}
void MediaEditorPage::Impl::Layout()
{
    if (!hwnd_) return;
    GetClientRect(hwnd_, &clientRect_);
    const int width = std::max(1L, clientRect_.right - clientRect_.left);
    const int height = std::max(1L, clientRect_.bottom - clientRect_.top);
    const int margin = std::clamp(Dips(22), Dips(14), std::max(Dips(14), width / 22));
    const int gap = Dips(12);

    importDropRect_ = {
        margin,
        Dips(18),
        width - margin,
        std::max(Dips(250), height - Dips(28))
    };
    const int importButtonWidth = Dips(156);
    const int importButtonGap = Dips(12);
    const int buttonTop = importDropRect_.bottom - Dips(78);
    chooseFileRect_ = {
        width / 2 - importButtonWidth - importButtonGap / 2,
        buttonTop,
        width / 2 - importButtonGap / 2,
        buttonTop + Dips(44)
    };
    pasteRect_ = clipboardAvailability_.HasCompatibleMedia()
        ? RECT {
            width / 2 + importButtonGap / 2,
            buttonTop,
            width / 2 + importButtonGap / 2 + importButtonWidth,
            buttonTop + Dips(44)
        }
        : RECT {
            width / 2 - importButtonWidth / 2,
            buttonTop,
            width / 2 + importButtonWidth / 2,
            buttonTop + Dips(44)
        };
    if (!clipboardAvailability_.HasCompatibleMedia())
    {
        chooseFileRect_ = pasteRect_;
        pasteRect_ = {};
    }

    const int videoToolbarHeight = Dips(58);
    const int videoToolbarButtonHeight = Dips(40);
    const int videoToolbarTop = (videoToolbarHeight - videoToolbarButtonHeight) / 2;

    const int videoTop = videoToolbarHeight + Dips(10);
    const int actionHeight = Dips(42);
    const int timelineHeight = Dips(106);
    const int fixedHeightAfterPreview = gap + timelineHeight + gap + actionHeight + margin;
    const int maximumPreviewHeight = std::max(
        Dips(220),
        height - videoTop - fixedHeightAfterPreview);
    const int previewInnerWidth = std::max(1, width - margin * 2 - Dips(28));
    const int preferredVideoHeight = std::clamp(
        MulDiv(previewInnerWidth, 9, 16),
        Dips(260),
        Dips(720));
    const int previewHeight = std::clamp(
        preferredVideoHeight + Dips(76),
        Dips(220),
        maximumPreviewHeight);
    previewPanelRect_ = {
        margin,
        videoTop,
        width - margin,
        videoTop + previewHeight
    };
    previewVideoRect_ = {
        previewPanelRect_.left + Dips(14),
        previewPanelRect_.top + Dips(14),
        previewPanelRect_.right - Dips(14),
        previewPanelRect_.bottom - Dips(62)
    };
    playRect_ = {
        previewPanelRect_.left + Dips(16),
        previewPanelRect_.bottom - Dips(50),
        previewPanelRect_.left + Dips(56),
        previewPanelRect_.bottom - Dips(12)
    };
    muteRect_ = {
        previewPanelRect_.right - Dips(210),
        playRect_.top,
        previewPanelRect_.right - Dips(170),
        playRect_.bottom
    };
    volumeRect_ = {
        muteRect_.right + Dips(12),
        playRect_.top + Dips(16),
        previewPanelRect_.right - Dips(18),
        playRect_.bottom - Dips(16)
    };

    timelinePanelRect_ = {
        margin,
        previewPanelRect_.bottom + gap,
        width - margin,
        previewPanelRect_.bottom + gap + timelineHeight
    };
    timelineTrackRect_ = {
        timelinePanelRect_.left + Dips(14),
        timelinePanelRect_.top + Dips(35),
        timelinePanelRect_.right - Dips(14),
        timelinePanelRect_.bottom - Dips(30)
    };
    timelineScrollbarRect_ = {
        timelinePanelRect_.left + Dips(16),
        timelinePanelRect_.bottom - Dips(18),
        timelinePanelRect_.right - Dips(16),
        timelinePanelRect_.bottom - Dips(10)
    };
    ClampTimelineView();

    timelineClipRects_.clear();
    timelineLeftHandleRects_.clear();
    timelineRightHandleRects_.clear();
    const auto& clips = timeline_.Clips();
    if (!clips.empty() && HasArea(timelineTrackRect_))
    {
        const double visibleDuration = std::max(0.001, TimelineVisibleDuration());
        const double viewEnd = timelineViewStartSeconds_ + visibleDuration;
        timelineClipRects_.reserve(clips.size());
        timelineLeftHandleRects_.reserve(clips.size());
        timelineRightHandleRects_.reserve(clips.size());
        for (size_t index = 0; index < clips.size(); ++index)
        {
            if (clips[index].TimelineEnd() < timelineViewStartSeconds_ ||
                clips[index].TimelineStart() > viewEnd)
            {
                timelineClipRects_.push_back({});
                timelineLeftHandleRects_.push_back({});
                timelineRightHandleRects_.push_back({});
                continue;
            }
            const int logicalLeft = TimelineXAtSeconds(clips[index].TimelineStart());
            const int logicalRight = TimelineXAtSeconds(clips[index].TimelineEnd());
            const int trackLeft = static_cast<int>(timelineTrackRect_.left);
            const int trackRight = static_cast<int>(timelineTrackRect_.right);
            const int left = std::clamp(logicalLeft, trackLeft, trackRight);
            const int right = std::clamp(logicalRight, left, trackRight);
            if (right <= left)
            {
                timelineClipRects_.push_back({});
                timelineLeftHandleRects_.push_back({});
                timelineRightHandleRects_.push_back({});
                continue;
            }
            RECT clipRect { left, timelineTrackRect_.top, right, timelineTrackRect_.bottom };
            timelineClipRects_.push_back(clipRect);
            const int clipWidth = std::max(1L, clipRect.right - clipRect.left);
            const int handleWidth = std::min(Dips(8), std::max(3, clipWidth / 3));
            timelineLeftHandleRects_.push_back(
                logicalLeft >= trackLeft && logicalLeft <= trackRight
                    ? RECT { clipRect.left, clipRect.top, clipRect.left + handleWidth, clipRect.bottom }
                    : RECT {});
            timelineRightHandleRects_.push_back(
                logicalRight >= trackLeft && logicalRight <= trackRight
                    ? RECT { clipRect.right - handleWidth, clipRect.top, clipRect.right, clipRect.bottom }
                    : RECT {});
        }
    }
    else
    {
        timelineClipRects_.resize(clips.size());
        timelineLeftHandleRects_.resize(clips.size());
        timelineRightHandleRects_.resize(clips.size());
    }

    timelineScrollbarThumbRect_ = {};
    if (HasArea(timelineScrollbarRect_) && timeline_.DurationSeconds() > 0.0)
    {
        const int scrollbarWidth = timelineScrollbarRect_.right - timelineScrollbarRect_.left;
        const double totalDuration = timeline_.DurationSeconds();
        const double visibleDuration = TimelineVisibleDuration();
        const int thumbWidth = std::clamp(
            static_cast<int>(std::llround(scrollbarWidth * visibleDuration / totalDuration)),
            std::min(Dips(36), scrollbarWidth),
            scrollbarWidth);
        const int travel = std::max(0, scrollbarWidth - thumbWidth);
        const double maximumStart = std::max(0.0, totalDuration - visibleDuration);
        const int thumbLeft = timelineScrollbarRect_.left +
            (maximumStart > 0.0
                ? static_cast<int>(std::llround(travel * timelineViewStartSeconds_ / maximumStart))
                : 0);
        timelineScrollbarThumbRect_ = {
            thumbLeft, timelineScrollbarRect_.top,
            thumbLeft + thumbWidth, timelineScrollbarRect_.bottom };
    }

    const int actionTop = timelinePanelRect_.bottom + gap;
    int actionLeft = margin;
    addClipRect_ = { actionLeft, actionTop, actionLeft + Dips(122), actionTop + actionHeight };
    splitRect_ = { addClipRect_.right + gap, actionTop, addClipRect_.right + gap + Dips(106), actionTop + actionHeight };
    deleteRect_ = { splitRect_.right + gap, actionTop, splitRect_.right + gap + Dips(106), actionTop + actionHeight };
    moveLeftRect_ = { deleteRect_.right + gap, actionTop, deleteRect_.right + gap + Dips(46), actionTop + actionHeight };
    moveRightRect_ = { moveLeftRect_.right + Dips(8), actionTop, moveLeftRect_.right + Dips(54), actionTop + actionHeight };
    exportRect_ = { width - margin - Dips(144), actionTop, width - margin, actionTop + actionHeight };

    imageToolbarRect_ = {
        margin, Dips(10), width - margin, Dips(74) };
    const int toolbarTop = imageToolbarRect_.top + Dips(10);
    const int toolbarBottom = toolbarTop + Dips(44);
    const int toolbarInnerWidth =
        imageToolbarRect_.right - imageToolbarRect_.left - Dips(20);
    const bool compactImageToolbar = toolbarInnerWidth < Dips(1320);
    const bool tightImageToolbar = toolbarInnerWidth < Dips(980);
    const int itemGap = Dips(tightImageToolbar ? 4 : 6);
    const int groupGap = Dips(tightImageToolbar ? 10 : compactImageToolbar ? 14 : 18);
    const int iconWidth = Dips(tightImageToolbar ? 40 : 44);

    int right = imageToolbarRect_.right - Dips(10);
    const int newWidth = Dips(tightImageToolbar ? 44 : compactImageToolbar ? 72 : 86);
    newEditRect_ = { right - newWidth, toolbarTop, right, toolbarBottom };
    right = newEditRect_.left - itemGap;

    const int saveArrowWidth = Dips(tightImageToolbar ? 30 : 34);
    const int saveMainWidth = Dips(tightImageToolbar ? 56 : compactImageToolbar ? 72 : 92);
    saveMenuRect_ = { right - saveArrowWidth, toolbarTop, right, toolbarBottom };
    saveRect_ = {
        saveMenuRect_.left - saveMainWidth,
        toolbarTop,
        saveMenuRect_.left,
        toolbarBottom
    };
    right = saveRect_.left - itemGap;

    const int copyWidth = Dips(tightImageToolbar ? 44 : compactImageToolbar ? 72 : 86);
    copyRect_ = { right - copyWidth, toolbarTop, right, toolbarBottom };
    imageToolbarSeparatorXs_[3] = copyRect_.left - groupGap / 2;
    right = copyRect_.left - groupGap;

    const int zoomWidth = Dips(tightImageToolbar ? 64 : compactImageToolbar ? 74 : 86);
    actualSizeRect_ = { right - zoomWidth, toolbarTop, right, toolbarBottom };
    right = actualSizeRect_.left - itemGap;
    const int fitWidth = Dips(tightImageToolbar ? 44 : compactImageToolbar ? 64 : 74);
    fitRect_ = { right - fitWidth, toolbarTop, right, toolbarBottom };
    right = fitRect_.left - itemGap;
    rotateRect_ = { right - iconWidth, toolbarTop, right, toolbarBottom };
    imageToolbarSeparatorXs_[2] = rotateRect_.left - groupGap / 2;
    const int propertyLimit = rotateRect_.left - groupGap;

    int left = imageToolbarRect_.left + Dips(10);
    undoRect_ = { left, toolbarTop, left + iconWidth, toolbarBottom };
    left = undoRect_.right + itemGap;
    redoRect_ = { left, toolbarTop, left + iconWidth, toolbarBottom };
    imageToolbarSeparatorXs_[0] = redoRect_.right + groupGap / 2;
    left = redoRect_.right + groupGap;

    const int drawWidth = Dips(tightImageToolbar ? 44 : compactImageToolbar ? 70 : 86);
    drawRect_ = { left, toolbarTop, left + drawWidth, toolbarBottom };
    left = drawRect_.right + itemGap;
    const int eraserWidth = Dips(tightImageToolbar ? 44 : compactImageToolbar ? 78 : 96);
    eraserRect_ = { left, toolbarTop, left + eraserWidth, toolbarBottom };
    left = eraserRect_.right + itemGap;
    const int textWidth = Dips(tightImageToolbar ? 44 : compactImageToolbar ? 68 : 82);
    textToolRect_ = { left, toolbarTop, left + textWidth, toolbarBottom };
    imageToolbarSeparatorXs_[1] = textToolRect_.right + groupGap / 2;
    left = textToolRect_.right + groupGap;

    colorRect_ = {};
    thicknessRect_ = {};
    opacityRect_ = {};
    const bool drawTool = !eraserEnabled_ && !textToolEnabled_;
    if (!eraserEnabled_)
    {
        const int colorWidth = Dips(tightImageToolbar ? 48 : compactImageToolbar ? 84 : 98);
        colorRect_ = { left, toolbarTop, left + colorWidth, toolbarBottom };
        left = colorRect_.right + itemGap;
    }
    if (!textToolEnabled_)
    {
        const int thicknessWidth = Dips(tightImageToolbar ? 64 : compactImageToolbar ? 84 : 96);
        thicknessRect_ = { left, toolbarTop, left + thicknessWidth, toolbarBottom };
        left = thicknessRect_.right + itemGap;
    }
    if (!eraserEnabled_)
    {
        const int opacityWidth = Dips(tightImageToolbar ? 100 : compactImageToolbar ? 132 : 168);
        const int available = propertyLimit - left;
        if (available >= Dips(72))
        {
            opacityRect_ = {
                left,
                toolbarTop + Dips(17),
                left + std::min(opacityWidth, available),
                toolbarTop + Dips(29)
            };
        }
    }
    if (!drawTool && imageToolbarPopup_ == ImageToolbarPopup::Thickness &&
        !HasArea(thicknessRect_))
    {
        imageToolbarPopup_ = ImageToolbarPopup::None;
    }
    if (imageToolbarPopup_ == ImageToolbarPopup::Color && !HasArea(colorRect_))
    {
        imageToolbarPopup_ = ImageToolbarPopup::None;
    }

    resetCropRect_ = {};
    colorPopupRect_ = {};
    colorCustomRect_ = {};
    thicknessPopupRect_ = {};
    thicknessSliderRect_ = {};
    zoomPopupRect_ = {};
    savePopupRect_ = {};
    saveAsRect_ = {};
    for (RECT& rect : colorSwatchRects_) rect = {};
    for (RECT& rect : thicknessRects_) rect = {};
    for (RECT& rect : zoomOptionRects_) rect = {};

    const int popupTop = imageToolbarRect_.bottom + Dips(6);
    auto popupLeft = [&](int desiredLeft, int popupWidth)
    {
        return std::clamp(
            desiredLeft,
            static_cast<int>(imageToolbarRect_.left),
            std::max(
                static_cast<int>(imageToolbarRect_.left),
                static_cast<int>(imageToolbarRect_.right) - popupWidth));
    };
    if (imageToolbarPopup_ == ImageToolbarPopup::Color && HasArea(colorRect_))
    {
        const int popupWidth = Dips(228);
        const int popupX = popupLeft(colorRect_.left, popupWidth);
        colorPopupRect_ = {
            popupX, popupTop, popupX + popupWidth, popupTop + Dips(108) };
        int swatchLeft = colorPopupRect_.left + Dips(14);
        for (RECT& swatch : colorSwatchRects_)
        {
            swatch = {
                swatchLeft, colorPopupRect_.top + Dips(14),
                swatchLeft + Dips(30), colorPopupRect_.top + Dips(44) };
            swatchLeft += Dips(40);
        }
        colorCustomRect_ = {
            colorPopupRect_.left + Dips(12), colorPopupRect_.top + Dips(56),
            colorPopupRect_.right - Dips(12), colorPopupRect_.bottom - Dips(12) };
    }
    else if (imageToolbarPopup_ == ImageToolbarPopup::Thickness &&
        HasArea(thicknessRect_))
    {
        const int popupWidth = Dips(212);
        const int popupX = popupLeft(thicknessRect_.left, popupWidth);
        thicknessPopupRect_ = {
            popupX, popupTop, popupX + popupWidth, popupTop + Dips(180) };
        int rowTop = thicknessPopupRect_.top + Dips(12);
        for (RECT& option : thicknessRects_)
        {
            option = {
                thicknessPopupRect_.left + Dips(12), rowTop,
                thicknessPopupRect_.right - Dips(12), rowTop + Dips(36) };
            rowTop += Dips(40);
        }
        thicknessSliderRect_ = {
            thicknessPopupRect_.left + Dips(18), thicknessPopupRect_.bottom - Dips(24),
            thicknessPopupRect_.right - Dips(18), thicknessPopupRect_.bottom - Dips(14) };
    }
    else if (imageToolbarPopup_ == ImageToolbarPopup::Zoom)
    {
        const int popupWidth = Dips(144);
        const int popupX = popupLeft(actualSizeRect_.right - popupWidth, popupWidth);
        zoomPopupRect_ = {
            popupX, popupTop, popupX + popupWidth, popupTop + Dips(224) };
        int rowTop = zoomPopupRect_.top + Dips(10);
        for (RECT& option : zoomOptionRects_)
        {
            option = {
                zoomPopupRect_.left + Dips(10), rowTop,
                zoomPopupRect_.right - Dips(10), rowTop + Dips(30) };
            rowTop += Dips(34);
        }
    }
    else if (imageToolbarPopup_ == ImageToolbarPopup::Save)
    {
        const int popupWidth = Dips(156);
        const int popupX = popupLeft(saveMenuRect_.right - popupWidth, popupWidth);
        savePopupRect_ = {
            popupX, popupTop, popupX + popupWidth, popupTop + Dips(54) };
        saveAsRect_ = {
            savePopupRect_.left + Dips(8), savePopupRect_.top + Dips(8),
            savePopupRect_.right - Dips(8), savePopupRect_.bottom - Dips(8) };
    }

    imageCanvasRect_ = {
        margin,
        imageToolbarRect_.bottom + gap,
        width - margin,
        height - margin
    };
    if (view_ == EditorView::Image && imageSession_.IsLoaded())
    {
        if (imageFitMode_) FitImageToCanvas();
        else ClampImagePan();
    }
    if (view_ == EditorView::Video)
    {
        newEditRect_ = {
            width - margin - Dips(126), videoToolbarTop,
            width - margin, videoToolbarTop + videoToolbarButtonHeight };
        keybindsRect_ = {
            newEditRect_.left - Dips(136), videoToolbarTop,
            newEditRect_.left - Dips(10), videoToolbarTop + videoToolbarButtonHeight };
        undoRect_ = {
            margin, videoToolbarTop, margin + Dips(42), videoToolbarTop + videoToolbarButtonHeight };
        redoRect_ = {
            undoRect_.right + Dips(8), undoRect_.top,
            undoRect_.right + Dips(50), undoRect_.bottom };
    }

    const int overlayWidth = std::min(Dips(720), width - Dips(36));
    const int overlayHeight = std::min(Dips(500), height - Dips(30));
    exportOverlayRect_ = {
        (width - overlayWidth) / 2,
        (height - overlayHeight) / 2,
        (width + overlayWidth) / 2,
        (height + overlayHeight) / 2
    };
    int modeTop = exportOverlayRect_.top + Dips(86);
    for (RECT& modeRect : exportModeRects_)
    {
        modeRect = {
            exportOverlayRect_.left + Dips(26),
            modeTop,
            exportOverlayRect_.right - Dips(26),
            modeTop + Dips(66)
        };
        modeTop += Dips(76);
    }
    exportSizeEditRect_ = {
        exportModeRects_[2].right - Dips(208),
        exportModeRects_[2].top + Dips(14),
        exportModeRects_[2].right - Dips(92),
        exportModeRects_[2].bottom - Dips(14)
    };
    exportSizeUnitRect_ = {
        exportSizeEditRect_.right + Dips(8),
        exportSizeEditRect_.top,
        exportModeRects_[2].right - Dips(12),
        exportSizeEditRect_.bottom
    };
    exportGpuRowRect_ = {
        exportOverlayRect_.left + Dips(26),
        exportModeRects_[2].bottom + Dips(14),
        exportOverlayRect_.right - Dips(26),
        exportModeRects_[2].bottom + Dips(76)
    };
    exportGpuToggleRect_ = {
        exportGpuRowRect_.right - Dips(78),
        exportGpuRowRect_.top + Dips(15),
        exportGpuRowRect_.right - Dips(18),
        exportGpuRowRect_.bottom - Dips(15)
    };
    exportCancelRect_ = {
        exportOverlayRect_.right - Dips(276),
        exportOverlayRect_.bottom - Dips(62),
        exportOverlayRect_.right - Dips(154),
        exportOverlayRect_.bottom - Dips(20)
    };
    exportStartRect_ = {
        exportOverlayRect_.right - Dips(144),
        exportCancelRect_.top,
        exportOverlayRect_.right - Dips(24),
        exportCancelRect_.bottom
    };
    exportOpenFileRect_ = {
        exportOverlayRect_.left + Dips(26),
        exportOverlayRect_.bottom - Dips(64),
        exportOverlayRect_.left + Dips(146),
        exportOverlayRect_.bottom - Dips(22)
    };
    exportOpenFolderRect_ = {
        exportOpenFileRect_.right + Dips(10),
        exportOpenFileRect_.top,
        exportOpenFileRect_.right + Dips(150),
        exportOpenFileRect_.bottom
    };
    exportDoneRect_ = {
        exportOverlayRect_.right - Dips(136),
        exportOpenFileRect_.top,
        exportOverlayRect_.right - Dips(24),
        exportOpenFileRect_.bottom
    };

    const int keybindsWidth = std::min(Dips(820), width - Dips(36));
    const int keybindsHeight = std::min(Dips(470), height - Dips(30));
    keybindsOverlayRect_ = {
        (width - keybindsWidth) / 2,
        (height - keybindsHeight) / 2,
        (width + keybindsWidth) / 2,
        (height + keybindsHeight) / 2
    };
    keybindsCloseRect_ = {
        keybindsOverlayRect_.right - Dips(126),
        keybindsOverlayRect_.bottom - Dips(60),
        keybindsOverlayRect_.right - Dips(24),
        keybindsOverlayRect_.bottom - Dips(20)
    };
}

void MediaEditorPage::Impl::UpdateChildWindows()
{
    if (!hwnd_) return;
    const bool showPreview =
        visible_ &&
        view_ == EditorView::Video &&
        timeline_.SelectedIndex() >= 0 &&
        !exportOpen_ &&
        !keybindsOpen_ &&
        preview_.IsReady();
    ShowWindow(previewHost_, showPreview ? SW_SHOW : SW_HIDE);
    if (showPreview)
    {
        SetWindowPos(
            previewHost_,
            HWND_BOTTOM,
            previewVideoRect_.left,
            previewVideoRect_.top,
            std::max(1L, previewVideoRect_.right - previewVideoRect_.left),
            std::max(1L, previewVideoRect_.bottom - previewVideoRect_.top),
            SWP_NOACTIVATE);
    }

    const bool showSizeEdit =
        visible_ &&
        view_ == EditorView::Video &&
        exportOpen_ &&
        !exportBusy_ &&
        !exportComplete_ &&
        exportMode_ == VideoEditorExportMode::FitUnderSizeLimit;
    ShowWindow(sizeLimitEdit_, showSizeEdit ? SW_SHOW : SW_HIDE);
    if (showSizeEdit)
    {
        SetWindowPos(
            sizeLimitEdit_,
            HWND_TOP,
            exportSizeEditRect_.left,
            exportSizeEditRect_.top,
            std::max(1L, exportSizeEditRect_.right - exportSizeEditRect_.left),
            std::max(1L, exportSizeEditRect_.bottom - exportSizeEditRect_.top),
            SWP_NOACTIVATE);
    }

    const bool showTextEdit =
        visible_ &&
        view_ == EditorView::Image &&
        textEditing_ &&
        !exportOpen_ &&
        !keybindsOpen_;
    if (showTextEdit)
    {
        UpdateTextEditBounds();
    }
    else
    {
        ShowWindow(textPopup_, SW_HIDE);
    }
}

void MediaEditorPage::Impl::HandleTextEditNotification(WORD notification)
{
    if (notification == EN_CHANGE && textEditing_ && !finishingTextEdit_)
    {
        const int length = GetWindowTextLengthW(textEdit_);
        std::wstring text(static_cast<size_t>(length) + 1, L'\0');
        GetWindowTextW(textEdit_, text.data(), length + 1);
        text.resize(static_cast<size_t>(length));
        activeTextBox_.text = std::move(text);
        UpdateTextEditFont();
        InvalidateRect(hwnd_, &imageCanvasRect_, FALSE);
    }
    else if (notification == EN_KILLFOCUS &&
        textEditing_ &&
        !finishingTextEdit_)
    {
        CommitTextEditing();
    }
}

void MediaEditorPage::Impl::BeginTextEditing(MediaEditorCropRect bounds)
{
    if (!imageSession_.IsLoaded() || !textPopup_ || !textEdit_)
    {
        return;
    }
    if (textEditing_)
    {
        CommitTextEditing();
    }

    activeTextBox_ = {};
    activeTextBox_.bounds = bounds;
    activeTextBox_.color = drawingColor_;
    activeTextBox_.opacity = drawingOpacity_;
    activeTextBox_.rotationQuarterTurns = 0;
    pendingTextBounds_ = {};
    textEditing_ = true;
    finishingTextEdit_ = false;
    SetWindowTextW(textEdit_, L"");
    UpdateTextEditBounds();
    UpdateTextEditFont();
    SetFocus(textEdit_);
    SendMessageW(textEdit_, EM_SETSEL, 0, 0);
    statusText_ = L"Type in the box. Press Enter to finish, Shift+Enter for a new line, or Esc to cancel.";
    InvalidateRect(hwnd_, &imageCanvasRect_, FALSE);
}

void MediaEditorPage::Impl::CommitTextEditing()
{
    if (!textEditing_ || finishingTextEdit_)
    {
        return;
    }
    finishingTextEdit_ = true;
    const int length = GetWindowTextLengthW(textEdit_);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(textEdit_, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    activeTextBox_.text = std::move(text);
    ShowWindow(textPopup_, SW_HIDE);
    textEditing_ = false;

    if (HasVisibleText(activeTextBox_.text))
    {
        if (!imageSession_.AddTextBox(std::move(activeTextBox_)))
        {
            statusText_ = L"There is not enough memory to add this text.";
        }
        else
        {
            statusText_.clear();
        }
    }
    else
    {
        statusText_.clear();
    }
    activeTextBox_ = {};
    pendingTextBounds_ = {};
    finishingTextEdit_ = false;
    InvalidateRect(hwnd_, &undoRect_, FALSE);
    InvalidateRect(hwnd_, &redoRect_, FALSE);
    InvalidateRect(hwnd_, &imageCanvasRect_, FALSE);
}

void MediaEditorPage::Impl::CancelTextEditing()
{
    if (!textEditing_ || finishingTextEdit_)
    {
        return;
    }
    finishingTextEdit_ = true;
    ShowWindow(textPopup_, SW_HIDE);
    textEditing_ = false;
    activeTextBox_ = {};
    pendingTextBounds_ = {};
    statusText_.clear();
    finishingTextEdit_ = false;
    InvalidateRect(hwnd_, &imageCanvasRect_, FALSE);
}

void MediaEditorPage::Impl::UpdateTextEditBounds()
{
    if (!textPopup_ || !textEdit_ || !textEditing_ || !imageSession_.IsLoaded())
    {
        return;
    }
    const POINT topLeft = ImageToCanvas({
        activeTextBox_.bounds.left,
        activeTextBox_.bounds.top
    });
    const POINT bottomRight = ImageToCanvas({
        activeTextBox_.bounds.right,
        activeTextBox_.bounds.bottom
    });
    RECT bounds {
        topLeft.x + Dips(3),
        topLeft.y + Dips(3),
        bottomRight.x - Dips(3),
        bottomRight.y - Dips(3)
    };
    if (!HasArea(bounds))
    {
        ShowWindow(textPopup_, SW_HIDE);
        return;
    }

    POINT screenTopLeft { bounds.left, bounds.top };
    ClientToScreen(hwnd_, &screenTopLeft);
    const int width = std::max<int>(1, bounds.right - bounds.left);
    const int height = std::max<int>(1, bounds.bottom - bounds.top);
    const RECT screenBounds {
        screenTopLeft.x,
        screenTopLeft.y,
        screenTopLeft.x + width,
        screenTopLeft.y + height
    };
    const bool boundsChanged = !EqualRect(&screenBounds, &textPopupScreenRect_);
    if (boundsChanged)
    {
        textPopupScreenRect_ = screenBounds;
        SetWindowPos(
            textPopup_,
            HWND_TOP,
            screenBounds.left,
            screenBounds.top,
            width,
            height,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SetWindowPos(
            textEdit_,
            nullptr,
            0,
            0,
            width,
            height,
            SWP_NOACTIVATE | SWP_NOZORDER);
        UpdateTextEditFont();
    }
    else if (!IsWindowVisible(textPopup_))
    {
        ShowWindow(textPopup_, SW_SHOWNOACTIVATE);
    }
    SetLayeredWindowAttributes(textPopup_, kTextEditColorKey, 255, LWA_COLORKEY);
}

void MediaEditorPage::Impl::UpdateTextEditFont()
{
    if (!textEdit_ || !textEditing_)
    {
        return;
    }
    RECT client {};
    GetClientRect(textEdit_, &client);
    const int width = std::max(1L, client.right - client.left - Dips(6));
    const int height = std::max(1L, client.bottom - client.top - Dips(6));
    const std::wstring sample =
        activeTextBox_.text.empty() ? L"M" : activeTextBox_.text;
    HDC editDc = GetDC(textEdit_);
    Gdiplus::REAL size = static_cast<Gdiplus::REAL>(Dips(12));
    if (editDc)
    {
        Gdiplus::Graphics graphics(editDc);
        size = FittedTextSize(
            graphics,
            sample,
            static_cast<Gdiplus::REAL>(width),
            static_cast<Gdiplus::REAL>(height));
        ReleaseDC(textEdit_, editDc);
    }

    LOGFONTW font {};
    font.lfHeight = -std::max(2, static_cast<int>(std::floor(size)));
    font.lfWeight = FW_NORMAL;
    font.lfQuality = ANTIALIASED_QUALITY;
    wcscpy_s(font.lfFaceName, L"Segoe UI");
    HFONT nextFont = CreateFontIndirectW(&font);
    if (!nextFont)
    {
        return;
    }
    SendMessageW(
        textEdit_,
        WM_SETFONT,
        reinterpret_cast<WPARAM>(nextFont),
        TRUE);
    if (textEditFont_)
    {
        DeleteObject(textEditFont_);
    }
    textEditFont_ = nextFont;
}

void MediaEditorPage::Impl::PaintButton(
    HDC hdc,
    const RECT& rect,
    const std::wstring& label,
    bool enabled,
    bool primary,
    bool selected)
{
    if (!HasArea(rect))
    {
        return;
    }

    rex::ui::ControlState state;
    state.hovered = enabled && EqualRect(&rect, &hoveredRect_);
    state.pressed = enabled && EqualRect(&rect, &pressedRect_);
    state.active = selected;
    state.enabled = enabled;

    rex::ui::ButtonOptions options;
    options.role = primary
        ? rex::ui::ButtonRole::Primary
        : rex::ui::ButtonRole::Neutral;
    rex::ui::PaintButton(
        hdc,
        rect,
        label,
        bodyFont_,
        dpi_,
        EditorComponentPalette(theme_),
        state,
        options);
}

void MediaEditorPage::Impl::PaintIconButton(
    HDC hdc,
    const RECT& rect,
    int icon,
    const std::wstring& label,
    bool enabled,
    bool selected)
{
    PaintButton(hdc, rect, L"", enabled, false, selected);
    const COLORREF color = enabled ? theme_.textPrimary : theme_.textSecondary;
    RECT glyph = rect;
    if (label.empty())
    {
        const int center = (rect.left + rect.right) / 2;
        glyph.left = center - Dips(9);
        glyph.right = center + Dips(9);
    }
    else
    {
        glyph.left += Dips(10);
        glyph.right = glyph.left + Dips(18);
    }
    glyph.top += (rect.bottom - rect.top - Dips(18)) / 2;
    glyph.bottom = glyph.top + Dips(18);

    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen pen(
        Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)),
        static_cast<Gdiplus::REAL>(Dips(2)));
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    const float left = static_cast<float>(glyph.left);
    const float top = static_cast<float>(glyph.top);
    const float right = static_cast<float>(glyph.right);
    const float bottom = static_cast<float>(glyph.bottom);
    const float centerX = (left + right) * 0.5f;
    const float centerY = (top + bottom) * 0.5f;

    if (icon == 22)
    {
        Gdiplus::Pen toolPen(
            Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)),
            static_cast<Gdiplus::REAL>(Dips(4)));
        toolPen.SetStartCap(Gdiplus::LineCapRound);
        toolPen.SetEndCap(Gdiplus::LineCapRound);
        graphics.DrawLine(&toolPen, left + Dips(4), bottom - Dips(4), right - Dips(4), top + Dips(4));
        graphics.DrawLine(&pen, left + Dips(2), bottom - Dips(2), left + Dips(6), bottom - Dips(6));
    }
    else if (icon == 23)
    {
        const float arm = static_cast<float>(Dips(5));
        graphics.DrawLine(&pen, left + Dips(2), top + arm, left + Dips(2), top + Dips(2));
        graphics.DrawLine(&pen, left + Dips(2), top + Dips(2), left + arm, top + Dips(2));
        graphics.DrawLine(&pen, right - arm, top + Dips(2), right - Dips(2), top + Dips(2));
        graphics.DrawLine(&pen, right - Dips(2), top + Dips(2), right - Dips(2), top + arm);
        graphics.DrawLine(&pen, left + Dips(2), bottom - arm, left + Dips(2), bottom - Dips(2));
        graphics.DrawLine(&pen, left + Dips(2), bottom - Dips(2), left + arm, bottom - Dips(2));
        graphics.DrawLine(&pen, right - arm, bottom - Dips(2), right - Dips(2), bottom - Dips(2));
        graphics.DrawLine(&pen, right - Dips(2), bottom - arm, right - Dips(2), bottom - Dips(2));
    }
    else if (icon == 24)
    {
        graphics.DrawEllipse(
            &pen,
            left + Dips(2),
            top + Dips(2),
            static_cast<Gdiplus::REAL>(Dips(11)),
            static_cast<Gdiplus::REAL>(Dips(11)));
        graphics.DrawLine(&pen, left + Dips(12), top + Dips(12), right - Dips(1), bottom - Dips(1));
    }
    else if (icon == 25)
    {
        graphics.DrawRectangle(
            &pen,
            left + Dips(2),
            top + Dips(5),
            static_cast<Gdiplus::REAL>(Dips(11)),
            static_cast<Gdiplus::REAL>(Dips(11)));
        graphics.DrawRectangle(
            &pen,
            left + Dips(6),
            top + Dips(2),
            static_cast<Gdiplus::REAL>(Dips(11)),
            static_cast<Gdiplus::REAL>(Dips(11)));
    }
    else if (icon == 21 && textIconTinted_)
    {
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        const Gdiplus::Rect destination(
            glyph.left,
            glyph.top,
            glyph.right - glyph.left,
            glyph.bottom - glyph.top);
        graphics.DrawImage(
            textIconTinted_.get(),
            destination,
            0,
            0,
            static_cast<INT>(textIconTinted_->GetWidth()),
            static_cast<INT>(textIconTinted_->GetHeight()),
            Gdiplus::UnitPixel);
    }
    else if (icon == 21)
    {
        graphics.DrawLine(&pen, left + Dips(2), top + Dips(3), right - Dips(2), top + Dips(3));
        graphics.DrawLine(&pen, centerX, top + Dips(3), centerX, bottom - Dips(2));
        graphics.DrawLine(&pen, centerX - Dips(4), bottom - Dips(2), centerX + Dips(4), bottom - Dips(2));
    }
    else if (icon == 17)
    {
        Gdiplus::GraphicsPath path;
        path.AddLine(left + Dips(5), top + Dips(3), right - Dips(3), centerY);
        path.AddLine(right - Dips(3), centerY, left + Dips(5), bottom - Dips(3));
        path.CloseFigure();
        Gdiplus::SolidBrush brush(Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)));
        graphics.FillPath(&brush, &path);
    }
    else if (icon == 18)
    {
        Gdiplus::SolidBrush brush(Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)));
        graphics.FillRectangle(
            &brush,
            left + Dips(4),
            top + Dips(3),
            static_cast<Gdiplus::REAL>(Dips(4)),
            bottom - top - Dips(6));
        graphics.FillRectangle(
            &brush,
            right - Dips(8),
            top + Dips(3),
            static_cast<Gdiplus::REAL>(Dips(4)),
            bottom - top - Dips(6));
    }
    else if (icon == 19 || icon == 20)
    {
        constexpr float sourceSize = 75.0f;
        const float scale = std::min((right - left) / sourceSize, (bottom - top) / sourceSize);
        const float offsetX = left + ((right - left) - sourceSize * scale) * 0.5f;
        const float offsetY = top + ((bottom - top) - sourceSize * scale) * 0.5f;
        Gdiplus::Matrix transform(scale, 0.0f, 0.0f, scale, offsetX, offsetY);

        const Gdiplus::PointF speakerPoints[] = {
            { 39.0f, 14.0f }, { 22.0f, 29.0f }, { 6.0f, 29.0f },
            { 6.0f, 48.0f }, { 22.0f, 48.0f }, { 39.0f, 63.0f }
        };
        Gdiplus::GraphicsPath speaker;
        speaker.AddPolygon(speakerPoints, static_cast<INT>(std::size(speakerPoints)));
        speaker.Transform(&transform);
        Gdiplus::SolidBrush brush(Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)));
        graphics.FillPath(&brush, &speaker);

        Gdiplus::Pen audioPen(
            Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)),
            std::max(1.0f, 5.0f * scale));
        audioPen.SetStartCap(Gdiplus::LineCapRound);
        audioPen.SetEndCap(Gdiplus::LineCapRound);
        if (icon == 19)
        {
            Gdiplus::GraphicsPath waves;
            waves.AddBezier(48.0f, 27.6f, 56.0f, 33.0f, 56.0f, 43.6f, 48.0f, 49.0f);
            waves.StartFigure();
            waves.AddBezier(55.1f, 20.5f, 67.0f, 29.2f, 67.0f, 47.4f, 55.1f, 56.1f);
            waves.StartFigure();
            waves.AddBezier(61.6f, 14.0f, 75.0f, 25.5f, 75.0f, 51.1f, 61.6f, 62.6f);
            waves.Transform(&transform);
            graphics.DrawPath(&audioPen, &waves);
        }
        else
        {
            Gdiplus::GraphicsPath mute;
            mute.AddLine(49.0f, 26.0f, 69.0f, 50.0f);
            mute.StartFigure();
            mute.AddLine(69.0f, 26.0f, 49.0f, 50.0f);
            mute.Transform(&transform);
            graphics.DrawPath(&audioPen, &mute);
        }
    }
    else if (icon == 3)
    {
        Gdiplus::GraphicsPath path;
        path.AddLine(left + 3, top + 2, right - 2, centerY);
        path.AddLine(right - 2, centerY, left + 3, bottom - 2);
        path.CloseFigure();
        Gdiplus::SolidBrush brush(Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)));
        graphics.FillPath(&brush, &path);
    }
    else if (icon == 6)
    {
        graphics.DrawLine(&pen, left + 4, top + 6, right - 4, top + 6);
        graphics.DrawLine(&pen, left + 7, top + 3, right - 7, top + 3);
        graphics.DrawRectangle(&pen, left + 6, top + 7, right - left - 12, bottom - top - 10);
    }
    else if (icon == 7 || icon == 8)
    {
        const float direction = icon == 7 ? -1.0f : 1.0f;
        graphics.DrawLine(&pen, centerX - 6 * direction, top + 3, centerX + 3 * direction, centerY);
        graphics.DrawLine(&pen, centerX + 3 * direction, centerY, centerX - 6 * direction, bottom - 3);
    }
    else if (icon == 4)
    {
        graphics.DrawLine(&pen, centerX, top + 3, centerX, bottom - 3);
        graphics.DrawLine(&pen, left + 3, centerY, right - 3, centerY);
    }
    else if (icon == 5)
    {
        graphics.DrawLine(&pen, centerX, top + 1, centerX, bottom - 1);
        graphics.DrawLine(&pen, left + 2, centerY, centerX - 3, centerY);
        graphics.DrawLine(&pen, centerX + 3, centerY, right - 2, centerY);
    }
    else if (icon == 9)
    {
        if (eraserIcon_)
        {
            const float red = static_cast<float>(GetRValue(color)) / 255.0f;
            const float green = static_cast<float>(GetGValue(color)) / 255.0f;
            const float blue = static_cast<float>(GetBValue(color)) / 255.0f;
            Gdiplus::ColorMatrix tint = {
                0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                red, green, blue, 1.0f, 0.0f,
                0.0f, 0.0f, 0.0f, 0.0f, 1.0f
            };
            Gdiplus::ImageAttributes attributes;
            attributes.SetColorMatrix(&tint);
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
            const Gdiplus::Rect destination(glyph.left, glyph.top, glyph.right - glyph.left, glyph.bottom - glyph.top);
            graphics.DrawImage(eraserIcon_.get(), destination, 0, 0,
                eraserIcon_->GetWidth(), eraserIcon_->GetHeight(), Gdiplus::UnitPixel, &attributes);
        }
    }
    else if (icon == 1 || icon == 2 || icon == 10)
    {
        const bool redo = icon == 2 || icon == 10;
        Gdiplus::GraphicsPath path;
        path.StartFigure();
        path.AddBezier(9.533f, 15.250f, 12.275f, 15.250f, 14.510f, 12.883f, 14.510f, 10.125f);
        path.AddBezier(14.510f, 10.125f, 14.510f, 7.369f, 12.276f, 5.062f, 9.533f, 5.062f);
        path.AddBezier(9.533f, 5.062f, 8.605f, 5.062f, 7.543f, 5.683f, 6.730f, 6.175f);
        path.AddLine(6.730f, 6.175f, 8.479f, 8.500f);
        path.AddLine(8.479f, 8.500f, 2.500f, 8.500f);
        path.AddLine(2.500f, 8.500f, 3.496f, 2.250f);
        path.AddLine(3.496f, 2.250f, 5.445f, 4.473f);
        path.AddBezier(5.445f, 4.473f, 6.594f, 3.629f, 8.004f, 3.063f, 9.533f, 3.063f);
        path.AddBezier(9.533f, 3.063f, 13.375f, 3.063f, 16.500f, 6.172f, 16.500f, 10.032f);
        path.AddBezier(16.500f, 10.032f, 16.500f, 13.892f, 13.375f, 17.266f, 9.533f, 17.266f);
        path.AddBezier(9.533f, 17.266f, 6.761f, 17.266f, 4.369f, 15.500f, 3.247f, 13.500f);
        path.AddLine(3.247f, 13.500f, 5.576f, 13.500f);
        path.AddBezier(5.576f, 13.500f, 6.486f, 14.500f, 7.914f, 15.250f, 9.533f, 15.250f);
        path.CloseFigure();

        constexpr float sourceSize = 19.0f;
        const float scale = std::min((right - left) / sourceSize, (bottom - top) / sourceSize);
        const float offsetX = left + ((right - left) - sourceSize * scale) * 0.5f;
        const float offsetY = top + ((bottom - top) - sourceSize * scale) * 0.5f;
        Gdiplus::Matrix transform(
            redo ? -scale : scale, 0.0f, 0.0f, scale,
            redo ? offsetX + sourceSize * scale : offsetX, offsetY);
        path.Transform(&transform);
        Gdiplus::SolidBrush brush(Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)));
        graphics.FillPath(&brush, &path);
    }
    else
    {
        graphics.DrawRectangle(&pen, left + 3, top + 3, right - left - 6, bottom - top - 6);
    }

    RECT textRect = rect;
    textRect.left = glyph.right + Dips(6);
    textRect.right -= Dips(8);
    DrawLabel(hdc, label, textRect, smallFont_, color, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

void MediaEditorPage::Impl::PaintProgress(HDC hdc, const RECT& rect, double value)
{
    FillRounded(hdc, rect, Dips(6), theme_.inputBackground);
    RECT fill = rect;
    fill.right = fill.left + static_cast<int>((fill.right - fill.left) * std::clamp(value, 0.0, 1.0));
    if (fill.right > fill.left) FillRounded(hdc, fill, Dips(6), theme_.accent);
}

void MediaEditorPage::Impl::Paint(HDC hdc)
{
    FillRectColor(hdc, clientRect_, theme_.pageBackground);
    if (view_ == EditorView::Import) PaintImport(hdc);
    else if (view_ == EditorView::Video) PaintVideo(hdc);
    else PaintImage(hdc);

    if (exportOpen_) PaintExportOverlay(hdc);
    else if (keybindsOpen_) PaintKeybindsOverlay(hdc);
    if (contextPasteOpen_ && HasArea(contextPasteRect_))
    {
        FillRounded(hdc, contextPasteRect_, Dips(10), theme_.panelBackground);
        StrokeRounded(hdc, contextPasteRect_, Dips(10), theme_.border);
        DrawLabel(hdc, L"Paste", Inset(contextPasteRect_, Dips(14), 0), bodyFont_, theme_.textPrimary, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    }
}

void MediaEditorPage::Impl::ReleasePaintBuffer()
{
    if (paintBufferDc_ && paintBufferPreviousBitmap_)
    {
        SelectObject(paintBufferDc_, paintBufferPreviousBitmap_);
    }
    paintBufferPreviousBitmap_ = nullptr;
    if (paintBufferBitmap_) DeleteObject(paintBufferBitmap_);
    if (paintBufferDc_) DeleteDC(paintBufferDc_);
    paintBufferBitmap_ = nullptr;
    paintBufferDc_ = nullptr;
    paintBufferSize_ = {};
}

void MediaEditorPage::Impl::PaintBuffered(HDC target, const RECT& paintRect)
{
    const int width = std::max(1L, clientRect_.right - clientRect_.left);
    const int height = std::max(1L, clientRect_.bottom - clientRect_.top);
    if (!paintBufferDc_ || !paintBufferBitmap_ ||
        paintBufferSize_.cx != width || paintBufferSize_.cy != height)
    {
        ReleasePaintBuffer();
        paintBufferDc_ = CreateCompatibleDC(target);
        paintBufferBitmap_ = CreateCompatibleBitmap(target, width, height);
        if (paintBufferDc_ && paintBufferBitmap_)
        {
            paintBufferPreviousBitmap_ = static_cast<HBITMAP>(
                SelectObject(paintBufferDc_, paintBufferBitmap_));
            paintBufferSize_ = { width, height };
        }
        else
        {
            ReleasePaintBuffer();
            Paint(target);
            return;
        }
    }

    const int saved = SaveDC(paintBufferDc_);
    IntersectClipRect(
        paintBufferDc_,
        paintRect.left,
        paintRect.top,
        paintRect.right,
        paintRect.bottom);
    Paint(paintBufferDc_);
    RestoreDC(paintBufferDc_, saved);
    BitBlt(
        target,
        paintRect.left,
        paintRect.top,
        paintRect.right - paintRect.left,
        paintRect.bottom - paintRect.top,
        paintBufferDc_,
        paintRect.left,
        paintRect.top,
        SRCCOPY);
}
void MediaEditorPage::Impl::PaintImport(HDC hdc)
{
    const bool hovered = EqualRect(&hoveredRect_, &importDropRect_) && !importBusy_;
    FillRounded(
        hdc,
        importDropRect_,
        Dips(18),
        hovered ? Blend(theme_.inputBackground, theme_.accent, 7) : theme_.inputBackground);
    StrokeRounded(
        hdc,
        importDropRect_,
        Dips(18),
        hovered ? Blend(theme_.border, theme_.accent, 70) : Blend(theme_.border, theme_.accent, 22),
        hovered ? static_cast<float>(Dips(2)) : 1.0f);

    const int centerX = (importDropRect_.left + importDropRect_.right) / 2;
    const int centerY = (importDropRect_.top + importDropRect_.bottom) / 2 - Dips(28);
    RECT iconRect { centerX - Dips(34), centerY - Dips(84), centerX + Dips(34), centerY - Dips(16) };
    FillRounded(hdc, iconRect, Dips(16), Blend(theme_.panelBackground, theme_.accentSoft, 15));
    StrokeRounded(hdc, iconRect, Dips(16), Blend(theme_.border, theme_.accent, 42));

    if (galleryIconTinted_)
    {
        Gdiplus::Graphics graphics(hdc);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        const Gdiplus::Rect destination(
            iconRect.left + Dips(13),
            iconRect.top + Dips(13),
            iconRect.right - iconRect.left - Dips(26),
            iconRect.bottom - iconRect.top - Dips(26));
        graphics.DrawImage(
            galleryIconTinted_.get(),
            destination,
            0,
            0,
            static_cast<INT>(galleryIconTinted_->GetWidth()),
            static_cast<INT>(galleryIconTinted_->GetHeight()),
            Gdiplus::UnitPixel);
    }

    RECT titleRect {
        importDropRect_.left + Dips(24),
        centerY - Dips(2),
        importDropRect_.right - Dips(24),
        centerY + Dips(32)
    };
    DrawLabel(
        hdc,
        importBusy_ ? L"Opening media..." : L"Drop a video or image here",
        titleRect,
        headingFont_,
        theme_.textPrimary,
        DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    RECT subtitleRect {
        titleRect.left,
        titleRect.bottom + Dips(2),
        titleRect.right,
        titleRect.bottom + Dips(30)
    };
    DrawLabel(
        hdc,
        importBusy_
            ? L"Checking the file and preparing the editor."
            : L"MP4, MOV, MKV, WEBM, AVI, M4V, PNG, JPG, WEBP, and BMP",
        subtitleRect,
        bodyFont_,
        theme_.textSecondary,
        DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    if (clipboardAvailability_.HasCompatibleMedia() && !importBusy_)
    {
        RECT clipboardRect {
            importDropRect_.left + Dips(28),
            subtitleRect.bottom + Dips(14),
            importDropRect_.right - Dips(28),
            subtitleRect.bottom + Dips(42)
        };
        DrawLabel(
            hdc,
            clipboardAvailability_.Summary(),
            clipboardRect,
            bodyFont_,
            theme_.accent,
            DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }

    PaintButton(hdc, chooseFileRect_, importBusy_ ? L"Opening..." : L"Choose file", !importBusy_, true);
    if (HasArea(pasteRect_))
    {
        PaintButton(hdc, pasteRect_, L"Paste", !importBusy_);
    }

    RECT statusRect {
        importDropRect_.left + Dips(22),
        importDropRect_.bottom - Dips(30),
        importDropRect_.right - Dips(22),
        importDropRect_.bottom - Dips(8)
    };
    DrawLabel(
        hdc,
        statusText_,
        statusRect,
        smallFont_,
        statusText_.find(L"not") != std::wstring::npos || statusText_.find(L"could") != std::wstring::npos
            ? theme_.warning
            : theme_.textSecondary,
        DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

void MediaEditorPage::Impl::PaintVideo(HDC hdc)
{
    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    PaintIconButton(hdc, undoRect_, 1, L"", timeline_.CanUndo() && !exportBusy_);
    PaintIconButton(hdc, redoRect_, 2, L"", timeline_.CanRedo() && !exportBusy_);

    RECT modeTitle {
        redoRect_.right + Dips(16),
        undoRect_.top,
        keybindsRect_.left - Dips(14),
        undoRect_.bottom
    };
    DrawLabel(
        hdc,
        L"Video editor",
        modeTitle,
        sectionFont_,
        theme_.textPrimary,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    PaintButton(hdc, newEditRect_, L"New edit", !exportBusy_);
    PaintButton(hdc, keybindsRect_, L"Keybinds", !exportBusy_);

    FillRounded(hdc, previewPanelRect_, Dips(16), theme_.panelBackground);
    StrokeRounded(hdc, previewPanelRect_, Dips(16), theme_.border);
    FillRounded(hdc, previewVideoRect_, Dips(12), RGB(8, 10, 14));
    StrokeRounded(hdc, previewVideoRect_, Dips(12), Blend(theme_.border, RGB(0, 0, 0), 25));

    if (!preview_.IsReady())
    {
        RECT previewMessage = Inset(previewVideoRect_, Dips(22), Dips(20));
        const std::wstring message = previewProxyBusy_
            ? L"Preparing a compatible preview with FFmpeg...\nThe original file will still be used for export."
            : previewUsingProxy_ && !statusText_.empty()
                ? statusText_
                : preview_.ErrorMessage().empty()
                    ? L"Preparing selected clip preview..."
                    : preview_.ErrorMessage();
        DrawLabel(
            hdc,
            message,
            previewMessage,
            bodyFont_,
            theme_.textSecondary,
            DT_CENTER | DT_WORDBREAK | DT_VCENTER | DT_END_ELLIPSIS);
    }

    PaintIconButton(
        hdc,
        playRect_,
        timelinePlaybackRequested_ ? 18 : 17,
        L"",
        (preview_.IsReady() || timelinePlaybackRequested_) && !exportBusy_,
        timelinePlaybackRequested_);

    double selectedPosition = 0.0;
    double selectedDuration = 0.0;
    if (timeline_.SelectedIndex() >= 0)
    {
        const auto& clip = timeline_.Clips()[timeline_.SelectedIndex()];
        selectedPosition = preview_.IsReady()
            ? std::clamp(preview_.PositionSeconds() - clip.trimInSeconds, 0.0, clip.Duration())
            : std::clamp(
                timeline_.PlayheadSeconds() - timeline_.ClipStartTime(timeline_.SelectedIndex()),
                0.0,
                clip.Duration());
        selectedDuration = clip.Duration();
    }
    RECT timeRect {
        playRect_.right + Dips(12),
        playRect_.top,
        previewPanelRect_.left + (previewPanelRect_.right - previewPanelRect_.left) / 2,
        playRect_.bottom
    };
    DrawLabel(
        hdc,
        FormatTime(selectedPosition) + L"  /  " + FormatTime(selectedDuration),
        timeRect,
        monoFont_,
        theme_.textSecondary,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    const bool volumeEnabled = preview_.IsReady() && !preview_.IsMuted();
    PaintIconButton(
        hdc,
        muteRect_,
        preview_.IsMuted() ? 20 : 19,
        L"",
        preview_.IsReady(),
        preview_.IsMuted());
    const COLORREF volumeTrackColor = volumeEnabled
        ? theme_.inputBackground
        : Blend(theme_.inputBackground, theme_.pageBackground, 48);
    const COLORREF volumeFillColor = volumeEnabled
        ? theme_.accent
        : Blend(theme_.textSecondary, theme_.pageBackground, 58);
    FillRounded(hdc, volumeRect_, Dips(4), volumeTrackColor);
    RECT volumeFill = volumeRect_;
    volumeFill.right = volumeFill.left + static_cast<int>((volumeFill.right - volumeFill.left) * videoVolume_);
    FillRounded(hdc, volumeFill, Dips(4), volumeFillColor);
    const int volumeX = volumeRect_.left + static_cast<int>((volumeRect_.right - volumeRect_.left) * videoVolume_);
    Gdiplus::SolidBrush volumeThumb(
        Gdiplus::Color(255, GetRValue(volumeEnabled ? theme_.textPrimary : theme_.textSecondary), GetGValue(volumeEnabled ? theme_.textPrimary : theme_.textSecondary), GetBValue(volumeEnabled ? theme_.textPrimary : theme_.textSecondary)));
    graphics.FillEllipse(
        &volumeThumb,
        static_cast<Gdiplus::REAL>(volumeX - Dips(6)),
        static_cast<Gdiplus::REAL>((volumeRect_.top + volumeRect_.bottom) / 2 - Dips(6)),
        static_cast<Gdiplus::REAL>(Dips(12)),
        static_cast<Gdiplus::REAL>(Dips(12)));

    FillRounded(hdc, timelinePanelRect_, Dips(16), theme_.panelBackground);
    StrokeRounded(hdc, timelinePanelRect_, Dips(16), theme_.border);
    RECT timelineTitle {
        timelinePanelRect_.left + Dips(14),
        timelinePanelRect_.top + Dips(4),
        timelinePanelRect_.right - Dips(14),
        timelinePanelRect_.top + Dips(31)
    };
    const std::wstring timelineLabel = L"Timeline  |  " +
        std::to_wstring(timeline_.Clips().size()) +
        (timeline_.Clips().size() == 1 ? L" clip" : L" clips") +
        L"  |  " + FormatTime(timeline_.DurationSeconds());
    RECT timelineLabelRect = timelineTitle;
    timelineLabelRect.right = timelineTitle.left + (timelineTitle.right - timelineTitle.left) * 45 / 100;
    DrawLabel(hdc, timelineLabel, timelineLabelRect, sectionFont_, theme_.textPrimary, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    RECT timelineHintRect = timelineTitle;
    timelineHintRect.left = timelineLabelRect.right + Dips(12);
    DrawLabel(
        hdc,
        L"Wheel to zoom  |  Drag the lower bar to move  |  " +
            std::to_wstring(static_cast<int>(std::lround(timelineZoom_ * 100.0))) +
            L"%  |  Export: full timeline",
        timelineHintRect,
        smallFont_,
        theme_.textSecondary,
        DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    FillRounded(hdc, timelineTrackRect_, Dips(10), theme_.inputBackground);
    Gdiplus::Pen gridPen(
        Gdiplus::Color(90, GetRValue(theme_.border), GetGValue(theme_.border), GetBValue(theme_.border)),
        1.0f);
    for (int division = 1; division < 10; ++division)
    {
        const float x = static_cast<float>(timelineTrackRect_.left) +
            static_cast<float>(timelineTrackRect_.right - timelineTrackRect_.left) * division / 10.0f;
        graphics.DrawLine(
            &gridPen,
            x,
            static_cast<float>(timelineTrackRect_.top + Dips(5)),
            x,
            static_cast<float>(timelineTrackRect_.bottom - Dips(5)));
    }
    const auto& clips = timeline_.Clips();
    for (size_t index = 0; index < timelineClipRects_.size() && index < clips.size(); ++index)
    {
        const RECT& clipRect = timelineClipRects_[index];
        if (!HasArea(clipRect))
        {
            continue;
        }
        const bool selected = static_cast<int>(index) == timeline_.SelectedIndex();
        const COLORREF clipColor = selected
            ? Blend(theme_.accentSoft, theme_.accent, 32)
            : Blend(theme_.buttonBackground, theme_.accentSoft, 10);
        FillRounded(hdc, clipRect, Dips(8), clipColor);
        StrokeRounded(
            hdc,
            clipRect,
            Dips(8),
            selected ? theme_.accent : theme_.border,
            selected ? static_cast<float>(Dips(2)) : 1.0f);
        FillRounded(hdc, timelineLeftHandleRects_[index], Dips(6), selected ? theme_.accent : Blend(theme_.border, theme_.textSecondary, 18));
        FillRounded(hdc, timelineRightHandleRects_[index], Dips(6), selected ? theme_.accent : Blend(theme_.border, theme_.textSecondary, 18));

        RECT nameRect = Inset(clipRect, Dips(14), 0);
        RECT durationRect = nameRect;
        if (clipRect.right - clipRect.left >= Dips(140))
        {
            durationRect.left = std::max(nameRect.left, durationRect.right - Dips(72));
            nameRect.right = durationRect.left - Dips(8);
        }
        DrawLabel(
            hdc,
            clips[index].sourcePath.filename().wstring(),
            nameRect,
            smallFont_,
            theme_.textPrimary,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        if (clipRect.right - clipRect.left >= Dips(140))
        {
            DrawLabel(
                hdc,
                FormatTime(clips[index].Duration()),
                durationRect,
                monoFont_,
                theme_.textSecondary,
                DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        }
    }

    if (dragInsertTargetId_ != 0)
    {
        for (size_t index = 0; index < clips.size() && index < timelineClipRects_.size(); ++index)
        {
            if (clips[index].id != dragInsertTargetId_ || !HasArea(timelineClipRects_[index]))
                continue;
            const int markerX = dragInsertAfter_
                ? timelineClipRects_[index].right
                : timelineClipRects_[index].left;
            Gdiplus::Pen insertPen(
                Gdiplus::Color(255, GetRValue(theme_.accent), GetGValue(theme_.accent), GetBValue(theme_.accent)),
                static_cast<Gdiplus::REAL>(Dips(3)));
            insertPen.SetStartCap(Gdiplus::LineCapRound);
            insertPen.SetEndCap(Gdiplus::LineCapRound);
            graphics.DrawLine(
                &insertPen,
                static_cast<Gdiplus::REAL>(markerX),
                static_cast<Gdiplus::REAL>(timelineTrackRect_.top - Dips(4)),
                static_cast<Gdiplus::REAL>(markerX),
                static_cast<Gdiplus::REAL>(timelineTrackRect_.bottom + Dips(4)));
            Gdiplus::PointF marker[] {
                { static_cast<Gdiplus::REAL>(markerX - Dips(5)), static_cast<Gdiplus::REAL>(timelineTrackRect_.top - Dips(5)) },
                { static_cast<Gdiplus::REAL>(markerX + Dips(5)), static_cast<Gdiplus::REAL>(timelineTrackRect_.top - Dips(5)) },
                { static_cast<Gdiplus::REAL>(markerX), static_cast<Gdiplus::REAL>(timelineTrackRect_.top + Dips(3)) }
            };
            Gdiplus::SolidBrush insertBrush(
                Gdiplus::Color(255, GetRValue(theme_.accent), GetGValue(theme_.accent), GetBValue(theme_.accent)));
            graphics.FillPolygon(&insertBrush, marker, static_cast<INT>(std::size(marker)));
            break;
        }
    }

    if (timeline_.DurationSeconds() > 0.0 && HasArea(timelineTrackRect_))
    {
        const int playheadX = TimelineXAtSeconds(timeline_.PlayheadSeconds());
        if (playheadX >= timelineTrackRect_.left && playheadX <= timelineTrackRect_.right)
        {
            Gdiplus::Pen playheadPen(
                Gdiplus::Color(255, GetRValue(theme_.warning), GetGValue(theme_.warning), GetBValue(theme_.warning)),
                static_cast<Gdiplus::REAL>(Dips(2)));
            graphics.DrawLine(
                &playheadPen,
                static_cast<Gdiplus::REAL>(playheadX),
                static_cast<Gdiplus::REAL>(timelineTrackRect_.top - Dips(3)),
                static_cast<Gdiplus::REAL>(playheadX),
                static_cast<Gdiplus::REAL>(timelineTrackRect_.bottom + Dips(3)));
        }
    }

    FillRounded(hdc, timelineScrollbarRect_, Dips(4), Blend(theme_.inputBackground, theme_.border, 18));
    if (HasArea(timelineScrollbarThumbRect_))
    {
        const COLORREF thumbColor = timelineZoom_ > 1.001
            ? Blend(theme_.accentSoft, theme_.accent, 24)
            : Blend(theme_.border, theme_.textSecondary, 14);
        FillRounded(hdc, timelineScrollbarThumbRect_, Dips(4), thumbColor);
        StrokeRounded(hdc, timelineScrollbarThumbRect_, Dips(4), timelineZoom_ > 1.001 ? theme_.accent : theme_.border);
    }

    const bool hasSelection = timeline_.SelectedIndex() >= 0;
    PaintIconButton(hdc, addClipRect_, 4, L"Add clip", !exportBusy_);
    PaintIconButton(hdc, splitRect_, 5, L"Split", hasSelection && !exportBusy_);
    PaintIconButton(hdc, deleteRect_, 6, L"Delete", hasSelection && !exportBusy_);
    PaintIconButton(hdc, moveLeftRect_, 7, L"", hasSelection && timeline_.SelectedIndex() > 0 && !exportBusy_);
    PaintIconButton(
        hdc,
        moveRightRect_,
        8,
        L"",
        hasSelection && timeline_.SelectedIndex() + 1 < static_cast<int>(timeline_.Clips().size()) && !exportBusy_);
    PaintButton(hdc, exportRect_, L"Export video", !timeline_.Clips().empty() && !exportBusy_, true);

}
void MediaEditorPage::Impl::PaintImage(HDC hdc)
{
    FillRounded(hdc, imageToolbarRect_, Dips(10), theme_.panelBackground);
    StrokeRounded(
        hdc,
        imageToolbarRect_,
        Dips(10),
        Blend(theme_.border, theme_.panelBackground, 28));

    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    auto gdiColor = [](COLORREF color, BYTE alpha = 255)
    {
        return Gdiplus::Color(
            alpha,
            GetRValue(color),
            GetGValue(color),
            GetBValue(color));
    };
    auto drawChevron = [&](const RECT& rect, COLORREF color)
    {
        Gdiplus::Pen chevronPen(gdiColor(color), static_cast<Gdiplus::REAL>(Dips(2)));
        chevronPen.SetStartCap(Gdiplus::LineCapRound);
        chevronPen.SetEndCap(Gdiplus::LineCapRound);
        const float centerX = static_cast<float>((rect.left + rect.right) / 2);
        const float centerY = static_cast<float>((rect.top + rect.bottom) / 2);
        graphics.DrawLine(
            &chevronPen,
            centerX - Dips(4),
            centerY - Dips(2),
            centerX,
            centerY + Dips(2));
        graphics.DrawLine(
            &chevronPen,
            centerX,
            centerY + Dips(2),
            centerX + Dips(4),
            centerY - Dips(2));
    };

    for (LONG separatorX : imageToolbarSeparatorXs_)
    {
        if (separatorX <= imageToolbarRect_.left || separatorX >= imageToolbarRect_.right)
        {
            continue;
        }
        RECT separator {
            separatorX,
            imageToolbarRect_.top + Dips(16),
            separatorX + std::max(1, Dips(1)),
            imageToolbarRect_.bottom - Dips(16)
        };
        FillRectColor(hdc, separator, Blend(theme_.border, theme_.panelBackground, 18));
    }

    const bool drawTool = !eraserEnabled_ && !textToolEnabled_;
    PaintIconButton(hdc, undoRect_, 1, L"", imageSession_.CanUndo());
    PaintIconButton(hdc, redoRect_, 2, L"", imageSession_.CanRedo());
    PaintIconButton(
        hdc,
        drawRect_,
        22,
        drawRect_.right - drawRect_.left >= Dips(68) ? L"Draw" : L"",
        true,
        drawTool);
    PaintIconButton(
        hdc,
        eraserRect_,
        9,
        eraserRect_.right - eraserRect_.left >= Dips(72) ? L"Eraser" : L"",
        true,
        eraserEnabled_);
    PaintIconButton(
        hdc,
        textToolRect_,
        21,
        textToolRect_.right - textToolRect_.left >= Dips(66) ? L"Text" : L"",
        true,
        textToolEnabled_);

    if (HasArea(colorRect_))
    {
        PaintButton(hdc, colorRect_, L"", true);
        const int swatchSize = Dips(20);
        const int swatchLeft = colorRect_.left + Dips(10);
        const int swatchTop = colorRect_.top + (colorRect_.bottom - colorRect_.top - swatchSize) / 2;
        Gdiplus::SolidBrush currentColor(gdiColor(drawingColor_));
        graphics.FillEllipse(
            &currentColor,
            static_cast<Gdiplus::REAL>(swatchLeft),
            static_cast<Gdiplus::REAL>(swatchTop),
            static_cast<Gdiplus::REAL>(swatchSize),
            static_cast<Gdiplus::REAL>(swatchSize));
        Gdiplus::Pen swatchOutline(
            gdiColor(Blend(theme_.textPrimary, drawingColor_, 32)),
            static_cast<Gdiplus::REAL>(std::max(1, Dips(1))));
        graphics.DrawEllipse(
            &swatchOutline,
            static_cast<Gdiplus::REAL>(swatchLeft),
            static_cast<Gdiplus::REAL>(swatchTop),
            static_cast<Gdiplus::REAL>(swatchSize),
            static_cast<Gdiplus::REAL>(swatchSize));
        if (colorRect_.right - colorRect_.left >= Dips(72))
        {
            RECT labelRect {
                swatchLeft + swatchSize + Dips(8),
                colorRect_.top,
                colorRect_.right - Dips(22),
                colorRect_.bottom
            };
            DrawLabel(
                hdc,
                L"Color",
                labelRect,
                smallFont_,
                theme_.textPrimary,
                DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            RECT chevronRect {
                colorRect_.right - Dips(22),
                colorRect_.top,
                colorRect_.right - Dips(6),
                colorRect_.bottom
            };
            drawChevron(chevronRect, theme_.textSecondary);
        }
        else
        {
            RECT chevronRect {
                colorRect_.right - Dips(18),
                colorRect_.top,
                colorRect_.right - Dips(3),
                colorRect_.bottom
            };
            drawChevron(chevronRect, theme_.textSecondary);
        }
    }

    if (HasArea(thicknessRect_))
    {
        PaintButton(hdc, thicknessRect_, L"", true);
        const bool showThicknessLine =
            thicknessRect_.right - thicknessRect_.left >= Dips(78);
        Gdiplus::Pen widthPen(
            gdiColor(theme_.textPrimary),
            static_cast<Gdiplus::REAL>(std::clamp(drawingThickness_ * 0.32f, 1.0f, 4.5f)));
        widthPen.SetStartCap(Gdiplus::LineCapRound);
        widthPen.SetEndCap(Gdiplus::LineCapRound);
        const int centerY = (thicknessRect_.top + thicknessRect_.bottom) / 2;
        if (showThicknessLine)
        {
            graphics.DrawLine(
                &widthPen,
                static_cast<Gdiplus::REAL>(thicknessRect_.left + Dips(10)),
                static_cast<Gdiplus::REAL>(centerY),
                static_cast<Gdiplus::REAL>(thicknessRect_.left + Dips(28)),
                static_cast<Gdiplus::REAL>(centerY));
        }
        RECT valueRect {
            thicknessRect_.left + Dips(showThicknessLine ? 34 : 8),
            thicknessRect_.top,
            thicknessRect_.right - Dips(20),
            thicknessRect_.bottom
        };
        DrawLabel(
            hdc,
            std::to_wstring(static_cast<int>(std::round(drawingThickness_))) + L" px",
            valueRect,
            smallFont_,
            theme_.textPrimary,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        RECT chevronRect {
            thicknessRect_.right - Dips(20),
            thicknessRect_.top,
            thicknessRect_.right - Dips(5),
            thicknessRect_.bottom
        };
        drawChevron(chevronRect, theme_.textSecondary);
    }

    if (HasArea(opacityRect_))
    {
        RECT opacityLabel = opacityRect_;
        opacityLabel.top -= Dips(17);
        opacityLabel.bottom = opacityRect_.top - Dips(1);
        DrawLabel(
            hdc,
            L"Opacity " + std::to_wstring(static_cast<int>(std::round(drawingOpacity_ * 100.0f))) + L"%",
            opacityLabel,
            smallFont_,
            theme_.textSecondary,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        FillRounded(hdc, opacityRect_, Dips(6), theme_.inputBackground);
        RECT opacityFill = opacityRect_;
        opacityFill.right = opacityFill.left +
            static_cast<int>((opacityFill.right - opacityFill.left) * drawingOpacity_);
        FillRounded(hdc, opacityFill, Dips(6), theme_.accent);
        const int opacityX = opacityRect_.left +
            static_cast<int>((opacityRect_.right - opacityRect_.left) * drawingOpacity_);
        Gdiplus::SolidBrush opacityThumb(gdiColor(theme_.textPrimary));
        graphics.FillEllipse(
            &opacityThumb,
            static_cast<Gdiplus::REAL>(opacityX - Dips(6)),
            static_cast<Gdiplus::REAL>((opacityRect_.top + opacityRect_.bottom) / 2 - Dips(6)),
            static_cast<Gdiplus::REAL>(Dips(12)),
            static_cast<Gdiplus::REAL>(Dips(12)));
    }

    PaintIconButton(hdc, rotateRect_, 10, L"", true);
    PaintIconButton(
        hdc,
        fitRect_,
        23,
        fitRect_.right - fitRect_.left >= Dips(60) ? L"Fit" : L"",
        true,
        imageFitMode_);
    const bool showZoomIcon =
        actualSizeRect_.right - actualSizeRect_.left >= Dips(82);
    if (showZoomIcon)
        PaintIconButton(hdc, actualSizeRect_, 24, L" ", true);
    else
        PaintButton(hdc, actualSizeRect_, L"", true);
    RECT zoomLabelRect {
        actualSizeRect_.left + Dips(showZoomIcon ? 32 : 8),
        actualSizeRect_.top,
        actualSizeRect_.right - Dips(19),
        actualSizeRect_.bottom
    };
    DrawLabel(
        hdc,
        std::to_wstring(static_cast<int>(std::round(imageScale_ * 100.0f))) + L"%",
        zoomLabelRect,
        smallFont_,
        theme_.textPrimary,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    RECT zoomChevronRect {
        actualSizeRect_.right - Dips(19),
        actualSizeRect_.top,
        actualSizeRect_.right - Dips(4),
        actualSizeRect_.bottom
    };
    drawChevron(zoomChevronRect, theme_.textSecondary);

    PaintIconButton(
        hdc,
        copyRect_,
        25,
        copyRect_.right - copyRect_.left >= Dips(68) ? L"Copy" : L"",
        imageSession_.IsLoaded());

    RECT saveUnion {
        saveRect_.left,
        saveRect_.top,
        saveMenuRect_.right,
        saveMenuRect_.bottom
    };
    const bool saveEnabled = imageSession_.IsLoaded();
    const bool saveHovered =
        saveEnabled &&
        (EqualRect(&saveRect_, &hoveredRect_) || EqualRect(&saveMenuRect_, &hoveredRect_));
    const bool savePressed =
        saveEnabled &&
        (EqualRect(&saveRect_, &pressedRect_) || EqualRect(&saveMenuRect_, &pressedRect_));
    COLORREF saveBackground = saveEnabled
        ? theme_.accent
        : Blend(theme_.buttonBackground, theme_.pageBackground, 52);
    if (saveHovered) saveBackground = Blend(saveBackground, theme_.textPrimary, 10);
    if (savePressed) saveBackground = Blend(saveBackground, theme_.pageBackground, 18);
    FillRounded(hdc, saveUnion, Dips(8), saveBackground);
    StrokeRounded(
        hdc,
        saveUnion,
        Dips(8),
        saveEnabled ? Blend(theme_.accent, theme_.textPrimary, 18) : theme_.border);
    RECT saveLabel = saveRect_;
    DrawLabel(
        hdc,
        L"Save",
        saveLabel,
        bodyFont_,
        saveEnabled ? theme_.textPrimary : theme_.textSecondary,
        DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    RECT saveDivider {
        saveMenuRect_.left,
        saveMenuRect_.top + Dips(8),
        saveMenuRect_.left + std::max(1, Dips(1)),
        saveMenuRect_.bottom - Dips(8)
    };
    FillRectColor(
        hdc,
        saveDivider,
        saveEnabled
            ? Blend(theme_.accent, theme_.textPrimary, 25)
            : Blend(theme_.border, theme_.buttonBackground, 20));
    drawChevron(saveMenuRect_, saveEnabled ? theme_.textPrimary : theme_.textSecondary);

    PaintIconButton(
        hdc,
        newEditRect_,
        4,
        newEditRect_.right - newEditRect_.left >= Dips(68) ? L"New" : L"",
        true);

    PaintImageCanvas(hdc);

    auto paintPopupFrame = [&](const RECT& rect)
    {
        RECT shadow = rect;
        OffsetRect(&shadow, Dips(2), Dips(3));
        FillRounded(hdc, shadow, Dips(9), Blend(theme_.pageBackground, RGB(0, 0, 0), 28), 170);
        FillRounded(hdc, rect, Dips(9), theme_.panelBackground);
        StrokeRounded(hdc, rect, Dips(9), Blend(theme_.border, theme_.textPrimary, 8));
    };

    if (imageToolbarPopup_ == ImageToolbarPopup::Color && HasArea(colorPopupRect_))
    {
        paintPopupFrame(colorPopupRect_);
        const std::array<COLORREF, 5> swatches {
            RGB(255, 255, 255),
            RGB(255, 76, 96),
            RGB(255, 205, 64),
            RGB(82, 157, 255),
            RGB(18, 20, 24)
        };
        for (size_t index = 0; index < colorSwatchRects_.size(); ++index)
        {
            const RECT& swatchRect = colorSwatchRects_[index];
            const COLORREF swatchColor = swatches[index];
            Gdiplus::SolidBrush swatchBrush(gdiColor(swatchColor));
            graphics.FillEllipse(
                &swatchBrush,
                static_cast<Gdiplus::REAL>(swatchRect.left),
                static_cast<Gdiplus::REAL>(swatchRect.top),
                static_cast<Gdiplus::REAL>(swatchRect.right - swatchRect.left),
                static_cast<Gdiplus::REAL>(swatchRect.bottom - swatchRect.top));
            Gdiplus::Pen swatchBorder(
                gdiColor(Blend(theme_.border, theme_.textPrimary, 12)),
                static_cast<Gdiplus::REAL>(std::max(1, Dips(1))));
            graphics.DrawEllipse(
                &swatchBorder,
                static_cast<Gdiplus::REAL>(swatchRect.left),
                static_cast<Gdiplus::REAL>(swatchRect.top),
                static_cast<Gdiplus::REAL>(swatchRect.right - swatchRect.left),
                static_cast<Gdiplus::REAL>(swatchRect.bottom - swatchRect.top));
            if (drawingColor_ == swatchColor)
            {
                Gdiplus::Pen selectedRing(
                    gdiColor(theme_.textPrimary),
                    static_cast<Gdiplus::REAL>(Dips(2)));
                graphics.DrawEllipse(
                    &selectedRing,
                    static_cast<Gdiplus::REAL>(swatchRect.left - Dips(3)),
                    static_cast<Gdiplus::REAL>(swatchRect.top - Dips(3)),
                    static_cast<Gdiplus::REAL>(swatchRect.right - swatchRect.left + Dips(6)),
                    static_cast<Gdiplus::REAL>(swatchRect.bottom - swatchRect.top + Dips(6)));
            }
        }
        PaintButton(hdc, colorCustomRect_, L"Custom color...");
    }
    else if (imageToolbarPopup_ == ImageToolbarPopup::Thickness &&
        HasArea(thicknessPopupRect_))
    {
        paintPopupFrame(thicknessPopupRect_);
        const std::array<float, 3> thicknesses { 2.0f, 6.0f, 14.0f };
        const std::array<const wchar_t*, 3> thicknessLabels {
            L"Thin", L"Medium", L"Thick"
        };
        for (size_t index = 0; index < thicknessRects_.size(); ++index)
        {
            const bool selected = std::abs(drawingThickness_ - thicknesses[index]) < 0.1f;
            PaintButton(hdc, thicknessRects_[index], L"", true, false, selected);
            const int centerY = (thicknessRects_[index].top + thicknessRects_[index].bottom) / 2;
            Gdiplus::Pen optionPen(
                gdiColor(theme_.textPrimary),
                static_cast<Gdiplus::REAL>(std::clamp(thicknesses[index] * 0.32f, 1.0f, 4.5f)));
            optionPen.SetStartCap(Gdiplus::LineCapRound);
            optionPen.SetEndCap(Gdiplus::LineCapRound);
            graphics.DrawLine(
                &optionPen,
                static_cast<Gdiplus::REAL>(thicknessRects_[index].left + Dips(12)),
                static_cast<Gdiplus::REAL>(centerY),
                static_cast<Gdiplus::REAL>(thicknessRects_[index].left + Dips(42)),
                static_cast<Gdiplus::REAL>(centerY));
            RECT label {
                thicknessRects_[index].left + Dips(54),
                thicknessRects_[index].top,
                thicknessRects_[index].right - Dips(12),
                thicknessRects_[index].bottom
            };
            DrawLabel(
                hdc,
                std::wstring(thicknessLabels[index]) + L"  " +
                    std::to_wstring(static_cast<int>(thicknesses[index])) + L" px",
                label,
                smallFont_,
                theme_.textPrimary,
                DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        }
        RECT customLabel = thicknessSliderRect_;
        customLabel.top -= Dips(18);
        customLabel.bottom = thicknessSliderRect_.top - Dips(2);
        DrawLabel(
            hdc,
            L"Custom  " + std::to_wstring(static_cast<int>(std::round(drawingThickness_))) + L" px",
            customLabel,
            smallFont_,
            theme_.textSecondary,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        const float thicknessAmount = std::clamp(
            (drawingThickness_ - 1.0f) / 29.0f,
            0.0f,
            1.0f);
        const RECT thicknessThumb = rex::ui::SliderThumbRectForValue(
            thicknessSliderRect_,
            thicknessAmount,
            dpi_);
        rex::ui::PaintSlider(
            hdc,
            thicknessSliderRect_,
            thicknessThumb,
            dpi_,
            EditorComponentPalette(theme_));
    }
    else if (imageToolbarPopup_ == ImageToolbarPopup::Zoom && HasArea(zoomPopupRect_))
    {
        paintPopupFrame(zoomPopupRect_);
        const std::array<int, 6> zoomValues { 25, 50, 75, 100, 150, 200 };
        for (size_t index = 0; index < zoomOptionRects_.size(); ++index)
        {
            const bool selected =
                !imageFitMode_ &&
                std::abs(imageScale_ * 100.0f - static_cast<float>(zoomValues[index])) < 0.5f;
            PaintButton(
                hdc,
                zoomOptionRects_[index],
                std::to_wstring(zoomValues[index]) + L"%",
                true,
                false,
                selected);
        }
    }
    else if (imageToolbarPopup_ == ImageToolbarPopup::Save && HasArea(savePopupRect_))
    {
        paintPopupFrame(savePopupRect_);
        PaintButton(hdc, saveAsRect_, L"Save as...");
    }
}

void MediaEditorPage::Impl::PaintImageCanvas(HDC hdc)
{
    FillRounded(hdc, imageCanvasRect_, Dips(16), Blend(theme_.inputBackground, RGB(0, 0, 0), 18));
    StrokeRounded(hdc, imageCanvasRect_, Dips(16), theme_.border);
    if (!imageSession_.IsLoaded() || !HasArea(imageDisplayRect_))
    {
        DrawLabel(
            hdc,
            L"No image loaded",
            imageCanvasRect_,
            bodyFont_,
            theme_.textSecondary,
            DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        return;
    }

    const MediaEditorImageBuffer& image = imageSession_.Image();
    Gdiplus::Bitmap fullResolutionBitmap(
        static_cast<INT>(image.width),
        static_cast<INT>(image.height),
        static_cast<INT>(image.stride),
        PixelFormat32bppPARGB,
        const_cast<BYTE*>(image.pixels.data()));
    Gdiplus::Bitmap* displayBitmap = &fullResolutionBitmap;
    UINT displayBitmapWidth = image.width;
    UINT displayBitmapHeight = image.height;
    if (imagePreviewCache_ && imagePreviewCache_->GetLastStatus() == Gdiplus::Ok)
    {
        displayBitmap = imagePreviewCache_.get();
        displayBitmapWidth = imagePreviewCache_->GetWidth();
        displayBitmapHeight = imagePreviewCache_->GetHeight();
    }
    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetInterpolationMode(
        imageScale_ < 1.0f
            ? Gdiplus::InterpolationModeHighQualityBicubic
            : Gdiplus::InterpolationModeNearestNeighbor);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

    Gdiplus::Region oldClip;
    graphics.GetClip(&oldClip);
    graphics.SetClip(Gdiplus::Rect(
        imageCanvasRect_.left + Dips(2),
        imageCanvasRect_.top + Dips(2),
        imageCanvasRect_.right - imageCanvasRect_.left - Dips(4),
        imageCanvasRect_.bottom - imageCanvasRect_.top - Dips(4)),
        Gdiplus::CombineModeIntersect);
    graphics.DrawImage(
        displayBitmap,
        Gdiplus::Rect(
            imageDisplayRect_.left,
            imageDisplayRect_.top,
            imageDisplayRect_.right - imageDisplayRect_.left,
            imageDisplayRect_.bottom - imageDisplayRect_.top),
        0,
        0,
        displayBitmapWidth,
        displayBitmapHeight,
        Gdiplus::UnitPixel);

    auto drawStroke = [&](const MediaEditorDrawingStroke& stroke, bool current)
    {
        if (stroke.points.empty()) return;
        const BYTE alpha = static_cast<BYTE>(
            std::clamp(stroke.opacity, 0.05f, 1.0f) * (current && stroke.eraser ? 100.0f : 255.0f));
        const COLORREF strokeColor = stroke.eraser ? theme_.textPrimary : stroke.color;
        const Gdiplus::Color gdiStrokeColor(
            alpha,
            GetRValue(strokeColor),
            GetGValue(strokeColor),
            GetBValue(strokeColor));
        Gdiplus::Pen pen(
            gdiStrokeColor,
            std::max(1.0f, stroke.thickness * imageScale_));
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        std::vector<Gdiplus::PointF> points;
        points.reserve(stroke.points.size());
        for (const MediaEditorPoint& point : stroke.points)
        {
            const POINT canvas = ImageToCanvas(point);
            points.emplace_back(static_cast<float>(canvas.x), static_cast<float>(canvas.y));
        }
        if (points.size() == 1)
        {
            const float radius = std::max(1.0f, stroke.thickness * imageScale_ * 0.5f);
            Gdiplus::SolidBrush dot(gdiStrokeColor);
            graphics.FillEllipse(&dot, points[0].X - radius, points[0].Y - radius, radius * 2, radius * 2);
        }
        else
        {
            graphics.DrawLines(&pen, points.data(), static_cast<INT>(points.size()));
        }
    };

    for (const MediaEditorDrawingStroke& stroke : imageSession_.Strokes())
    {
        drawStroke(stroke, false);
    }
    if (!currentStroke_.points.empty() && !currentStroke_.eraser)
    {
        drawStroke(currentStroke_, true);
    }

    for (const MediaEditorTextBox& textBox : imageSession_.TextBoxes())
    {
        MediaEditorTextBox displayText = textBox;
        const POINT topLeft = ImageToCanvas({
            textBox.bounds.left,
            textBox.bounds.top
        });
        const POINT bottomRight = ImageToCanvas({
            textBox.bounds.right,
            textBox.bounds.bottom
        });
        displayText.bounds = {
            static_cast<float>(topLeft.x),
            static_cast<float>(topLeft.y),
            static_cast<float>(bottomRight.x),
            static_cast<float>(bottomRight.y)
        };
        DrawTextBox(graphics, displayText);
    }

    if (textEditing_ && HasVisibleText(activeTextBox_.text))
    {
        MediaEditorTextBox displayText = activeTextBox_;
        const POINT topLeft = ImageToCanvas({
            activeTextBox_.bounds.left,
            activeTextBox_.bounds.top
        });
        const POINT bottomRight = ImageToCanvas({
            activeTextBox_.bounds.right,
            activeTextBox_.bounds.bottom
        });
        displayText.bounds = {
            static_cast<float>(topLeft.x),
            static_cast<float>(topLeft.y),
            static_cast<float>(bottomRight.x),
            static_cast<float>(bottomRight.y)
        };
        DrawTextBox(graphics, displayText);
    }

    auto drawTextSelection = [&](const MediaEditorCropRect& bounds, bool draft)
    {
        if (bounds.Width() <= 0.0f || bounds.Height() <= 0.0f)
        {
            return;
        }
        const POINT topLeft = ImageToCanvas({ bounds.left, bounds.top });
        const POINT bottomRight = ImageToCanvas({ bounds.right, bounds.bottom });
        const Gdiplus::RectF selection(
            static_cast<Gdiplus::REAL>(topLeft.x),
            static_cast<Gdiplus::REAL>(topLeft.y),
            static_cast<Gdiplus::REAL>(bottomRight.x - topLeft.x),
            static_cast<Gdiplus::REAL>(bottomRight.y - topLeft.y));
        if (draft)
        {
            Gdiplus::SolidBrush fill(Gdiplus::Color(
                24,
                GetRValue(theme_.accent),
                GetGValue(theme_.accent),
                GetBValue(theme_.accent)));
            graphics.FillRectangle(&fill, selection);
        }
        const Gdiplus::REAL outlineOffset =
            static_cast<Gdiplus::REAL>(Dips(3));
        const Gdiplus::RectF outlineRect(
            selection.X - outlineOffset,
            selection.Y - outlineOffset,
            selection.Width + outlineOffset * 2.0f,
            selection.Height + outlineOffset * 2.0f);
        Gdiplus::Pen outline(
            Gdiplus::Color(
                255,
                GetRValue(theme_.accent),
                GetGValue(theme_.accent),
                GetBValue(theme_.accent)),
            static_cast<Gdiplus::REAL>(Dips(2)));
        outline.SetDashStyle(Gdiplus::DashStyleDash);
        graphics.DrawRectangle(&outline, outlineRect);
    };
    if (pointerAction_ == PointerAction::ImageTextBox)
    {
        drawTextSelection(pendingTextBounds_, true);
    }
    else if (textEditing_)
    {
        drawTextSelection(activeTextBox_.bounds, false);
    }

    const MediaEditorCropRect crop = imageSession_.Crop();
    const POINT cropTopLeft = ImageToCanvas({ crop.left, crop.top });
    const POINT cropBottomRight = ImageToCanvas({ crop.right, crop.bottom });
    RECT cropRect {
        cropTopLeft.x,
        cropTopLeft.y,
        cropBottomRight.x,
        cropBottomRight.y
    };

    Gdiplus::SolidBrush dimBrush(Gdiplus::Color(125, 0, 0, 0));
    const std::array<Gdiplus::Rect, 4> dimRects {
        Gdiplus::Rect(imageDisplayRect_.left, imageDisplayRect_.top, imageDisplayRect_.right - imageDisplayRect_.left, std::max<LONG>(0, cropRect.top - imageDisplayRect_.top)),
        Gdiplus::Rect(imageDisplayRect_.left, cropRect.bottom, imageDisplayRect_.right - imageDisplayRect_.left, std::max<LONG>(0, imageDisplayRect_.bottom - cropRect.bottom)),
        Gdiplus::Rect(imageDisplayRect_.left, cropRect.top, std::max<LONG>(0, cropRect.left - imageDisplayRect_.left), std::max<LONG>(0, cropRect.bottom - cropRect.top)),
        Gdiplus::Rect(cropRect.right, cropRect.top, std::max<LONG>(0, imageDisplayRect_.right - cropRect.right), std::max<LONG>(0, cropRect.bottom - cropRect.top))
    };
    for (const Gdiplus::Rect& rect : dimRects)
    {
        if (rect.Width > 0 && rect.Height > 0) graphics.FillRectangle(&dimBrush, rect);
    }

    Gdiplus::Pen cropPen(
        Gdiplus::Color(255, GetRValue(theme_.accent), GetGValue(theme_.accent), GetBValue(theme_.accent)),
        static_cast<Gdiplus::REAL>(Dips(2)));
    graphics.DrawRectangle(
        &cropPen,
        static_cast<Gdiplus::REAL>(cropRect.left),
        static_cast<Gdiplus::REAL>(cropRect.top),
        static_cast<Gdiplus::REAL>(cropRect.right - cropRect.left),
        static_cast<Gdiplus::REAL>(cropRect.bottom - cropRect.top));

    const std::array<POINT, 8> handleCenters {
        POINT { cropRect.left, cropRect.top },
        POINT { (cropRect.left + cropRect.right) / 2, cropRect.top },
        POINT { cropRect.right, cropRect.top },
        POINT { cropRect.left, (cropRect.top + cropRect.bottom) / 2 },
        POINT { cropRect.right, (cropRect.top + cropRect.bottom) / 2 },
        POINT { cropRect.left, cropRect.bottom },
        POINT { (cropRect.left + cropRect.right) / 2, cropRect.bottom },
        POINT { cropRect.right, cropRect.bottom }
    };
    const int handleRadius = Dips(6);
    Gdiplus::SolidBrush handleBrush(
        Gdiplus::Color(255, GetRValue(theme_.textPrimary), GetGValue(theme_.textPrimary), GetBValue(theme_.textPrimary)));
    for (size_t index = 0; index < handleCenters.size(); ++index)
    {
        cropHandleRects_[index] = {
            handleCenters[index].x - handleRadius,
            handleCenters[index].y - handleRadius,
            handleCenters[index].x + handleRadius,
            handleCenters[index].y + handleRadius
        };
        graphics.FillEllipse(
            &handleBrush,
            static_cast<Gdiplus::REAL>(cropHandleRects_[index].left),
            static_cast<Gdiplus::REAL>(cropHandleRects_[index].top),
            static_cast<Gdiplus::REAL>(handleRadius * 2),
            static_cast<Gdiplus::REAL>(handleRadius * 2));
        graphics.DrawEllipse(
            &cropPen,
            static_cast<Gdiplus::REAL>(cropHandleRects_[index].left),
            static_cast<Gdiplus::REAL>(cropHandleRects_[index].top),
            static_cast<Gdiplus::REAL>(handleRadius * 2),
            static_cast<Gdiplus::REAL>(handleRadius * 2));
    }

    const int moveHandleSize = Dips(28);
    const int cropCenterX = (cropRect.left + cropRect.right) / 2;
    const int cropCenterY = (cropRect.top + cropRect.bottom) / 2;
    cropMoveRect_ = {
        cropCenterX - moveHandleSize / 2,
        cropCenterY - moveHandleSize / 2,
        cropCenterX + moveHandleSize / 2,
        cropCenterY + moveHandleSize / 2
    };
    FillRounded(hdc, cropMoveRect_, Dips(8), theme_.accent, 235);
    StrokeRounded(hdc, cropMoveRect_, Dips(8), Blend(theme_.accent, theme_.textPrimary, 35));
    Gdiplus::Pen movePen(
        Gdiplus::Color(255, GetRValue(theme_.textPrimary), GetGValue(theme_.textPrimary), GetBValue(theme_.textPrimary)),
        static_cast<Gdiplus::REAL>(Dips(2)));
    movePen.SetStartCap(Gdiplus::LineCapRound);
    movePen.SetEndCap(Gdiplus::LineCapRound);
    const float moveX = static_cast<float>(cropCenterX);
    const float moveY = static_cast<float>(cropCenterY);
    const float arm = static_cast<float>(Dips(7));
    const float arrow = static_cast<float>(Dips(3));
    graphics.DrawLine(&movePen, moveX - arm, moveY, moveX + arm, moveY);
    graphics.DrawLine(&movePen, moveX, moveY - arm, moveX, moveY + arm);
    graphics.DrawLine(&movePen, moveX - arm, moveY, moveX - arm + arrow, moveY - arrow);
    graphics.DrawLine(&movePen, moveX - arm, moveY, moveX - arm + arrow, moveY + arrow);
    graphics.DrawLine(&movePen, moveX + arm, moveY, moveX + arm - arrow, moveY - arrow);
    graphics.DrawLine(&movePen, moveX + arm, moveY, moveX + arm - arrow, moveY + arrow);
    graphics.DrawLine(&movePen, moveX, moveY - arm, moveX - arrow, moveY - arm + arrow);
    graphics.DrawLine(&movePen, moveX, moveY - arm, moveX + arrow, moveY - arm + arrow);
    graphics.DrawLine(&movePen, moveX, moveY + arm, moveX - arrow, moveY + arm - arrow);
    graphics.DrawLine(&movePen, moveX, moveY + arm, moveX + arrow, moveY + arm - arrow);
    graphics.SetClip(&oldClip);

    const bool cropAdjusted =
        std::abs(crop.left) > 0.5f ||
        std::abs(crop.top) > 0.5f ||
        std::abs(crop.right - static_cast<float>(imageSession_.Width())) > 0.5f ||
        std::abs(crop.bottom - static_cast<float>(imageSession_.Height())) > 0.5f;
    if (cropAdjusted)
    {
        resetCropRect_ = {
            imageCanvasRect_.right - Dips(122),
            imageCanvasRect_.top + Dips(16),
            imageCanvasRect_.right - Dips(16),
            imageCanvasRect_.top + Dips(52)
        };
        PaintButton(hdc, resetCropRect_, L"Reset crop");
    }
    else
    {
        resetCropRect_ = {};
    }
    RECT statusRect {
        imageCanvasRect_.left + Dips(16),
        imageCanvasRect_.bottom - Dips(34),
        imageCanvasRect_.right - Dips(16),
        imageCanvasRect_.bottom - Dips(10)
    };
    const std::wstring canvasStatus =
        statusText_.empty() && textToolEnabled_
        ? L"Drag on the image to create a transparent text box."
        : statusText_.empty()
        ? L"Drag the center handle to move the crop  |  " +
            std::to_wstring(static_cast<int>(std::round(imageScale_ * 100.0f))) + L"%  |  " +
            std::to_wstring(static_cast<int>(std::round(crop.Width()))) + L" x " +
            std::to_wstring(static_cast<int>(std::round(crop.Height())))
        : statusText_;
    FillRounded(hdc, statusRect, Dips(8), theme_.panelBackground, 220);
    DrawLabel(
        hdc,
        canvasStatus,
        Inset(statusRect, Dips(10), 0),
        smallFont_,
        theme_.textSecondary,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

void MediaEditorPage::Impl::PaintExportOverlay(HDC hdc)
{
    Gdiplus::Graphics graphics(hdc);
    Gdiplus::SolidBrush dim(Gdiplus::Color(150, 0, 0, 0));
    const Gdiplus::Rect dimRect(
        clientRect_.left,
        clientRect_.top,
        clientRect_.right - clientRect_.left,
        clientRect_.bottom - clientRect_.top);
    graphics.FillRectangle(&dim, dimRect);

    FillRounded(hdc, exportOverlayRect_, Dips(18), theme_.panelBackground);
    StrokeRounded(hdc, exportOverlayRect_, Dips(18), Blend(theme_.border, theme_.accent, 25));
    RECT titleRect {
        exportOverlayRect_.left + Dips(26),
        exportOverlayRect_.top + Dips(20),
        exportOverlayRect_.right - Dips(26),
        exportOverlayRect_.top + Dips(54)
    };
    DrawLabel(
        hdc,
        exportBusy_ ? L"Exporting video" : exportComplete_ ? (exportResult_.success ? L"Video ready" : L"Export failed") : L"Export video",
        titleRect,
        headingFont_,
        theme_.textPrimary,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    if (exportBusy_)
    {
        RECT phaseRect {
            exportOverlayRect_.left + Dips(28),
            exportOverlayRect_.top + Dips(112),
            exportOverlayRect_.right - Dips(28),
            exportOverlayRect_.top + Dips(152)
        };
        DrawLabel(
            hdc,
            exportProgress_.message.empty()
                ? VideoEditorExportService::PhaseLabel(exportProgress_.phase)
                : exportProgress_.message,
            phaseRect,
            sectionFont_,
            theme_.textPrimary,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        RECT progressRect {
            phaseRect.left,
            phaseRect.bottom + Dips(18),
            phaseRect.right,
            phaseRect.bottom + Dips(30)
        };
        PaintProgress(hdc, progressRect, exportProgress_.progress);
        RECT detailRect {
            phaseRect.left,
            progressRect.bottom + Dips(14),
            phaseRect.right,
            progressRect.bottom + Dips(44)
        };
        std::wstring details = FormatTime(exportProgress_.elapsedSeconds) + L" elapsed";
        if (exportProgress_.estimatedRemainingSeconds > 0.5)
        {
            details += L"  |  About " + FormatTime(exportProgress_.estimatedRemainingSeconds) + L" remaining";
        }
        else if (exportProgress_.phase != VideoEditorExportPhase::Verifying)
        {
            details += L"  |  Estimating time remaining...";
        }
        DrawLabel(hdc, details, detailRect, monoFont_, theme_.textSecondary, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        PaintButton(hdc, exportCancelRect_, L"Cancel");
        return;
    }

    if (exportComplete_)
    {
        RECT messageRect {
            exportOverlayRect_.left + Dips(28),
            exportOverlayRect_.top + Dips(92),
            exportOverlayRect_.right - Dips(28),
            exportOverlayRect_.bottom - Dips(94)
        };
        std::wstring message = exportResult_.success
            ? exportResult_.outputPath.filename().wstring() + L"\n\n" +
                VideoCompressionService::FormatBytes(exportResult_.outputSizeBytes) + L"\n" +
                exportResult_.details
            : exportResult_.errorMessage;
        DrawLabel(
            hdc,
            message,
            messageRect,
            bodyFont_,
            exportResult_.success ? theme_.textPrimary : theme_.danger,
            DT_LEFT | DT_WORDBREAK | DT_TOP | DT_END_ELLIPSIS);
        if (exportResult_.success)
        {
            PaintButton(hdc, exportOpenFileRect_, L"Open file", true, true);
            PaintButton(hdc, exportOpenFolderRect_, L"Open folder");
        }
        PaintButton(hdc, exportDoneRect_, L"Done");
        return;
    }

    const std::array<std::wstring, 3> titles {
        L"Keep original quality",
        L"Make file smaller",
        L"Fit under a size limit"
    };
    const std::array<std::wstring, 3> descriptions {
        L"Preserves the original quality when possible.",
        L"Chooses sensible compression automatically.",
        L"Targets a maximum file size and verifies the result."
    };
    for (size_t index = 0; index < exportModeRects_.size(); ++index)
    {
        const bool selected = static_cast<int>(exportMode_) == static_cast<int>(index);
        const RECT& modeRect = exportModeRects_[index];
        FillRounded(
            hdc,
            modeRect,
            Dips(12),
            selected ? Blend(theme_.inputBackground, theme_.accentSoft, 28) : theme_.inputBackground);
        StrokeRounded(
            hdc,
            modeRect,
            Dips(12),
            selected ? theme_.accent : theme_.border,
            selected ? static_cast<float>(Dips(2)) : 1.0f);

        const int centerY = (modeRect.top + modeRect.bottom) / 2;
        Gdiplus::SolidBrush dotBrush(
            Gdiplus::Color(
                255,
                GetRValue(selected ? theme_.accent : theme_.border),
                GetGValue(selected ? theme_.accent : theme_.border),
                GetBValue(selected ? theme_.accent : theme_.border)));
        graphics.FillEllipse(
            &dotBrush,
            static_cast<Gdiplus::REAL>(modeRect.left + Dips(16)),
            static_cast<Gdiplus::REAL>(centerY - Dips(6)),
            static_cast<Gdiplus::REAL>(Dips(12)),
            static_cast<Gdiplus::REAL>(Dips(12)));

        RECT modeTitle {
            modeRect.left + Dips(42),
            modeRect.top + Dips(8),
            index == 2 ? exportSizeEditRect_.left - Dips(12) : modeRect.right - Dips(16),
            modeRect.top + Dips(32)
        };
        DrawLabel(hdc, titles[index], modeTitle, sectionFont_, theme_.textPrimary, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        RECT descriptionRect = modeTitle;
        descriptionRect.top = modeTitle.bottom;
        descriptionRect.bottom = modeRect.bottom - Dips(6);
        DrawLabel(hdc, descriptions[index], descriptionRect, smallFont_, theme_.textSecondary, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }
    if (exportMode_ == VideoEditorExportMode::FitUnderSizeLimit)
    {
        FillRounded(hdc, exportSizeEditRect_, Dips(9), theme_.inputBackground);
        StrokeRounded(hdc, exportSizeEditRect_, Dips(9), theme_.border);
        PaintButton(hdc, exportSizeUnitRect_, exportSizeUnitGb_ ? L"GB" : L"MB");
    }

    const bool gpuHovered = EqualRect(&hoveredRect_, &exportGpuRowRect_) ||
        EqualRect(&hoveredRect_, &exportGpuToggleRect_);
    FillRounded(
        hdc,
        exportGpuRowRect_,
        Dips(12),
        gpuHovered
            ? Blend(theme_.inputBackground, theme_.accentSoft, 12)
            : theme_.inputBackground);
    StrokeRounded(
        hdc,
        exportGpuRowRect_,
        Dips(12),
        exportGpuEnabled_
            ? Blend(theme_.border, theme_.accent, 42)
            : theme_.border);
    RECT gpuTitle {
        exportGpuRowRect_.left + Dips(16),
        exportGpuRowRect_.top + Dips(7),
        exportGpuToggleRect_.left - Dips(16),
        exportGpuRowRect_.top + Dips(31)
    };
    DrawLabel(
        hdc,
        L"GPU acceleration",
        gpuTitle,
        sectionFont_,
        theme_.textPrimary,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    RECT gpuDescription = gpuTitle;
    gpuDescription.top = gpuTitle.bottom;
    gpuDescription.bottom = exportGpuRowRect_.bottom - Dips(5);
    DrawLabel(
        hdc,
        exportGpuEnabled_
            ? L"Automatic NVIDIA, Intel, or AMD detection with CPU fallback."
            : L"Use CPU encoding for maximum compatibility.",
        gpuDescription,
        smallFont_,
        theme_.textSecondary,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    const bool gpuPressed = EqualRect(&pressedRect_, &exportGpuRowRect_) ||
        EqualRect(&pressedRect_, &exportGpuToggleRect_);
    rex::ui::SetSwitchTarget(
        exportGpuToggleAnimation_,
        exportGpuEnabled_,
        exportGpuToggleRect_);
    rex::ui::PaintSwitch(
        hdc,
        exportGpuToggleRect_,
        exportGpuToggleAnimation_.position,
        dpi_,
        EditorComponentPalette(theme_),
        gpuHovered,
        gpuPressed);
    PaintButton(hdc, exportCancelRect_, L"Back");
    PaintButton(hdc, exportStartRect_, L"Export", true, true);
}

void MediaEditorPage::Impl::PaintKeybindsOverlay(HDC hdc)
{
    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush dim(Gdiplus::Color(160, 0, 0, 0));
    graphics.FillRectangle(
        &dim,
        Gdiplus::Rect(
            clientRect_.left,
            clientRect_.top,
            clientRect_.right - clientRect_.left,
            clientRect_.bottom - clientRect_.top));

    FillRounded(hdc, keybindsOverlayRect_, Dips(18), theme_.panelBackground);
    StrokeRounded(
        hdc,
        keybindsOverlayRect_,
        Dips(18),
        Blend(theme_.border, theme_.accent, 30));

    RECT titleRect {
        keybindsOverlayRect_.left + Dips(26),
        keybindsOverlayRect_.top + Dips(18),
        keybindsOverlayRect_.right - Dips(26),
        keybindsOverlayRect_.top + Dips(52)
    };
    DrawLabel(
        hdc,
        L"Keyboard shortcuts",
        titleRect,
        headingFont_,
        theme_.textPrimary,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    RECT subtitleRect = titleRect;
    subtitleRect.top = titleRect.bottom;
    subtitleRect.bottom = subtitleRect.top + Dips(26);
    DrawLabel(
        hdc,
        L"Fast playback and editing controls for the video timeline.",
        subtitleRect,
        smallFont_,
        theme_.textSecondary,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    struct ShortcutRow
    {
        const wchar_t* key;
        const wchar_t* action;
    };
    const std::array<ShortcutRow, 7> playbackRows {{
        { L"Click video", L"Play or pause" },
        { L"Space", L"Play or pause" },
        { L",", L"Previous frame" },
        { L".", L"Next frame" },
        { L"Left / Right", L"Previous or next frame" },
        { L"M", L"Mute or unmute" },
        { L"Esc", L"Close the active panel" }
    }};
    const std::array<ShortcutRow, 7> editingRows {{
        { L"S", L"Split at the playhead" },
        { L"A", L"Add clips" },
        { L"Delete", L"Remove selected clip" },
        { L"Ctrl + O", L"Import media" },
        { L"Ctrl + Z", L"Undo" },
        { L"Ctrl + Y", L"Redo" },
        { L"E", L"Export video" }
    }};

    const int contentLeft = keybindsOverlayRect_.left + Dips(26);
    const int contentRight = keybindsOverlayRect_.right - Dips(26);
    const int columnGap = Dips(28);
    const int columnWidth = (contentRight - contentLeft - columnGap) / 2;
    const int headingTop = keybindsOverlayRect_.top + Dips(92);
    const int rowsTop = headingTop + Dips(34);
    const int availableRowsHeight = std::max(
        Dips(210), static_cast<int>(keybindsCloseRect_.top) - Dips(14) - rowsTop);
    const int rowHeight = std::clamp(availableRowsHeight / 7, Dips(30), Dips(42));

    auto drawColumn = [&](int left, const wchar_t* heading, const auto& rows)
    {
        RECT headingRect { left, headingTop, left + columnWidth, rowsTop - Dips(4) };
        DrawLabel(
            hdc,
            heading,
            headingRect,
            sectionFont_,
            theme_.textPrimary,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        for (size_t index = 0; index < rows.size(); ++index)
        {
            const int top = rowsTop + static_cast<int>(index) * rowHeight;
            RECT keyRect { left, top + Dips(3), left + Dips(112), top + rowHeight - Dips(3) };
            FillRounded(hdc, keyRect, Dips(8), theme_.inputBackground);
            StrokeRounded(hdc, keyRect, Dips(8), theme_.border);
            DrawLabel(hdc, rows[index].key, keyRect, monoFont_, theme_.textPrimary,
                DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            RECT actionRect {
                keyRect.right + Dips(12), top,
                left + columnWidth, top + rowHeight };
            DrawLabel(hdc, rows[index].action, actionRect, smallFont_, theme_.textSecondary,
                DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        }
    };

    drawColumn(contentLeft, L"Playback", playbackRows);
    drawColumn(contentLeft + columnWidth + columnGap, L"Editing", editingRows);
    PaintButton(hdc, keybindsCloseRect_, L"Close");
}

void MediaEditorPage::Impl::ChooseFiles(bool addToTimeline)
{
    if (importBusy_ || exportBusy_) return;
    std::vector<wchar_t> buffer(32768, L'\0');
    const wchar_t videoFilter[] =
        L"Video files\0*.mp4;*.mov;*.mkv;*.webm;*.avi;*.m4v;*.wmv;*.flv;*.mpeg;*.mpg;*.ts;*.m2ts;*.mts;*.3gp;*.3g2;*.ogv;*.vob;*.mxf;*.asf;*.f4v\0"
        L"All files\0*.*\0\0";
    const wchar_t allFilter[] =
        L"Videos and images\0*.mp4;*.mov;*.mkv;*.webm;*.avi;*.m4v;*.wmv;*.flv;*.mpeg;*.mpg;*.ts;*.m2ts;*.mts;*.3gp;*.3g2;*.ogv;*.vob;*.mxf;*.asf;*.f4v;*.png;*.jpg;*.jpeg;*.webp;*.bmp\0"
        L"Video files\0*.mp4;*.mov;*.mkv;*.webm;*.avi;*.m4v;*.wmv;*.flv;*.mpeg;*.mpg;*.ts;*.m2ts;*.mts;*.3gp;*.3g2;*.ogv;*.vob;*.mxf;*.asf;*.f4v\0"
        L"Image files\0*.png;*.jpg;*.jpeg;*.webp;*.bmp\0All files\0*.*\0\0";

    OPENFILENAMEW dialog {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFilter = addToTimeline ? videoFilter : allFilter;
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
        OFN_ALLOWMULTISELECT | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&dialog))
    {
        std::vector<std::filesystem::path> paths = ParseOpenFileBuffer(buffer.data());
        BeginImport(std::move(paths), std::nullopt, addToTimeline);
    }
}

void MediaEditorPage::Impl::OpenFiles(const std::vector<std::filesystem::path>& paths)
{
    if (paths.empty())
    {
        return;
    }
    const bool allVideos = std::all_of(
        paths.begin(),
        paths.end(),
        [](const std::filesystem::path& path)
        {
            return MediaTypeDetector::IsSupportedVideo(path);
        });
    const bool append = view_ == EditorView::Video && allVideos;
    BeginImport(paths, std::nullopt, append);
}

void MediaEditorPage::Impl::PasteFromClipboard()
{
    if (keybindsOpen_ || exportOpen_ || importBusy_ || exportBusy_)
    {
        return;
    }
    std::wstring errorMessage;
    std::optional<ClipboardMediaPayload> payload =
        clipboardService_.ReadCompatibleMedia(errorMessage);
    if (!payload)
    {
        statusText_ = errorMessage.empty()
            ? L"The clipboard does not contain a supported image or video."
            : errorMessage;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (payload->image)
    {
        BeginImport({}, std::move(payload->image), false);
    }
    else
    {
        OpenFiles(payload->files);
    }
}

void MediaEditorPage::Impl::BeginImport(
    std::vector<std::filesystem::path> paths,
    std::optional<MediaEditorImageBuffer> clipboardImage,
    bool appendVideo)
{
    if (exportBusy_)
    {
        statusText_ = L"Finish or cancel the current export first.";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (textEditing_)
    {
        CancelTextEditing();
    }
    CancelImport();

    if (!clipboardImage && paths.empty())
    {
        statusText_ = L"Choose a supported image or video.";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    bool imageImport = clipboardImage.has_value();
    if (!clipboardImage)
    {
        const bool oneImage =
            paths.size() == 1 &&
            MediaTypeDetector::IsSupportedImage(paths.front());
        const bool videos =
            !paths.empty() &&
            std::all_of(
                paths.begin(),
                paths.end(),
                [](const std::filesystem::path& path)
                {
                    return MediaTypeDetector::IsSupportedVideo(path);
                });
        if (!oneImage && !videos)
        {
            statusText_ = L"Choose one image or one or more supported video files.";
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        imageImport = oneImage;
    }

    const unsigned long long generation = ++importGeneration_;
    importCancel_ = false;
    importBusy_ = true;
    statusText_ = imageImport ? L"Opening image..." : L"Analyzing video clips...";
    InvalidateRect(hwnd_, nullptr, FALSE);
    const HWND resultWindow = hwnd_;

    importThread_ = std::thread(
        [this,
         resultWindow,
         paths = std::move(paths),
         clipboardImage = std::move(clipboardImage),
         appendVideo,
         imageImport,
         generation]() mutable
        {
            auto result = std::make_unique<MediaImportThreadResult>();
            result->generation = generation;
            result->appendVideo = appendVideo;
            if (imageImport)
            {
                result->imageSession = std::make_unique<ImageEditingSession>();
                bool loaded = false;
                if (clipboardImage)
                {
                    loaded = result->imageSession->LoadFromBuffer(
                        std::move(*clipboardImage),
                        {},
                        result->errorMessage);
                }
                else
                {
                    loaded = result->imageSession->LoadFromFile(
                        paths.front(),
                        result->errorMessage);
                }
                if (!loaded)
                {
                    result->imageSession.reset();
                }
            }
            else
            {
                VideoCompressionService analyzer;
                const ExternalToolStatus tools = analyzer.CheckExternalTools();
                if (!tools.ffprobeFound)
                {
                    result->errorMessage =
                        L"FFprobe is missing. Restore the bundled tools folder or add FFprobe to PATH.";
                }
                else
                {
                    for (const std::filesystem::path& path : paths)
                    {
                        if (importCancel_.load())
                        {
                            result->errorMessage = L"Import cancelled.";
                            break;
                        }
                        std::wstring analysisError;
                        VideoAnalysis analysis = analyzer.Analyze(
                            path,
                            importCancel_,
                            analysisError);
                        if (analysis.durationSeconds <= 0.0)
                        {
                            result->errorMessage = analysisError.empty()
                                ? L"\"" + path.filename().wstring() + L"\" is not a readable video."
                                : analysisError;
                            result->videos.clear();
                            break;
                        }
                        analysis.frameTimestamps = analyzer.BuildFrameTimestampIndex(
                            analysis,
                            importCancel_);
                        result->videos.push_back(std::move(analysis));
                    }
                }
            }

            MediaImportThreadResult* raw = result.release();
            if (!PostMessageW(resultWindow, kImportFinishedMessage, 0, reinterpret_cast<LPARAM>(raw)))
            {
                delete raw;
            }
        });
}

void MediaEditorPage::Impl::CancelImport()
{
    ++importGeneration_;
    importCancel_ = true;
    if (importThread_.joinable()) importThread_.join();
    importBusy_ = false;
}

void MediaEditorPage::Impl::FinishImport(
    std::unique_ptr<MediaImportThreadResult> result)
{
    if (result && result->generation != importGeneration_.load())
    {
        return;
    }
    if (importThread_.joinable()) importThread_.join();
    importBusy_ = false;
    if (!result)
    {
        statusText_ = L"The media could not be opened.";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (!result->errorMessage.empty())
    {
        if (result->errorMessage != L"Import cancelled.")
        {
            statusText_ = result->errorMessage;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    exportOpen_ = false;
    keybindsOpen_ = false;
    exportComplete_ = false;
    exportResult_ = {};
    if (result->imageSession)
    {
        ClearCompatibilityPreviews();
        imageSession_ = std::move(*result->imageSession);
        imageSession_.ClearHistory();
        timeline_.Reset();
        timelineZoom_ = 1.0;
        timelineViewStartSeconds_ = 0.0;
        view_ = EditorView::Image;
        imageFitMode_ = true;
        imagePan_ = { 0.0f, 0.0f };
        currentStroke_ = {};
        imageEraseChanged_ = false;
        lastImageSavePath_.clear();
        statusText_.clear();
        RebuildImagePreviewCache();
        Layout();
        FitImageToCanvas();
    }
    else if (!result->videos.empty())
    {
        if (!result->appendVideo)
        {
            ClearCompatibilityPreviews();
            timeline_.Reset();
            timelineZoom_ = 1.0;
            timelineViewStartSeconds_ = 0.0;
        }
        for (VideoAnalysis& analysis : result->videos)
        {
            timeline_.AddClip(std::move(analysis));
        }
        if (!result->appendVideo)
        {
            timeline_.ClearHistory();
        }
        imageSession_.Reset();
        imagePreviewCache_.reset();
        view_ = EditorView::Video;
        statusText_ = result->videos.size() == 1
            ? L"Clip ready."
            : std::to_wstring(result->videos.size()) + L" clips added in order.";
        Layout();
        LoadSelectedVideoPreview();
    }
    UpdateChildWindows();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MediaEditorPage::Impl::UpdateClipboardAvailability()
{
    const ClipboardMediaAvailability latest = clipboardService_.DetectCompatibleMedia();
    const bool changed =
        latest.hasImagePixels != clipboardAvailability_.hasImagePixels ||
        latest.hasSupportedFiles != clipboardAvailability_.hasSupportedFiles ||
        latest.hasImageFile != clipboardAvailability_.hasImageFile ||
        latest.hasVideoFiles != clipboardAvailability_.hasVideoFiles ||
        latest.videoFileCount != clipboardAvailability_.videoFileCount;
    if (!changed)
    {
        return;
    }
    clipboardAvailability_ = latest;
    Layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MediaEditorPage::Impl::ResetToImport()
{
    if (exportBusy_)
    {
        CancelVideoExport();
        return;
    }
    if (textEditing_)
    {
        CancelTextEditing();
    }
    CancelImport();
    ClearCompatibilityPreviews();
    timeline_.Reset();
    timelineZoom_ = 1.0;
    timelineViewStartSeconds_ = 0.0;
    imageSession_.Reset();
    imagePreviewCache_.reset();
    currentStroke_ = {};
    imageEraseChanged_ = false;
    timelinePlaybackRequested_ = false;
    view_ = EditorView::Import;
    exportOpen_ = false;
    keybindsOpen_ = false;
    exportComplete_ = false;
    contextPasteOpen_ = false;
    lastImageSavePath_.clear();
    statusText_ = L"Drop a video or image to begin.";
    UpdateClipboardAvailability();
    Layout();
    UpdateChildWindows();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MediaEditorPage::Impl::LoadSelectedVideoPreview(bool seekToPlayhead)
{
    CancelCompatibilityPreview();
    preview_.Pause();
    const int selected = timeline_.SelectedIndex();
    if (selected < 0 || selected >= static_cast<int>(timeline_.Clips().size()))
    {
        preview_.Close();
        UpdateChildWindows();
        return;
    }
    const VideoEditorClipModel& clip = timeline_.Clips()[selected];
    const double local = seekToPlayhead
        ? std::clamp(
            timeline_.PlayheadSeconds() - timeline_.ClipStartTime(selected),
            0.0,
            clip.Duration())
        : 0.0;
    const double sourceSeconds = clip.trimInSeconds + local;
    const std::wstring sourceKey = PreviewCacheKey(clip.sourcePath);
    auto cached = previewProxyCache_.find(sourceKey);
    if (cached != previewProxyCache_.end())
    {
        std::error_code cacheError;
        if (std::filesystem::is_regular_file(cached->second, cacheError) &&
            OpenPreviewPath(clip, cached->second, sourceSeconds, true))
        {
            return;
        }
        RemovePreviewPath(cached->second);
        previewProxyCache_.erase(cached);
    }

    if (!CanUseNativeWindowsPreview(clip))
    {
        BeginCompatibilityPreview(clip, sourceSeconds);
        return;
    }
    if (!OpenPreviewPath(clip, clip.sourcePath, sourceSeconds, false))
    {
        BeginCompatibilityPreview(clip, sourceSeconds);
    }
}

bool MediaEditorPage::Impl::OpenPreviewPath(
    const VideoEditorClipModel& clip,
    const std::filesystem::path& path,
    double sourceSeconds,
    bool compatibilityProxy)
{
    const double clampedSourceSeconds = std::clamp(
        sourceSeconds,
        clip.trimInSeconds,
        clip.trimOutSeconds);
    previewProxySeekSeconds_ = clampedSourceSeconds;
    previewUsingProxy_ = compatibilityProxy;
    nativePreviewStartedAt_ = 0;
    nativePreviewClipId_ = 0;
    statusText_ = compatibilityProxy
        ? L"Opening compatibility preview..."
        : L"Preparing selected clip preview...";
    if (!preview_.Open(path, clip.trimInSeconds, clip.trimOutSeconds))
    {
        return false;
    }
    if (!compatibilityProxy)
    {
        nativePreviewStartedAt_ = GetTickCount64();
        nativePreviewClipId_ = clip.id;
    }
    preview_.Seek(clampedSourceSeconds);
    UpdateChildWindows();
    InvalidateRect(hwnd_, &previewPanelRect_, FALSE);
    return true;
}

void MediaEditorPage::Impl::BeginCompatibilityPreview(
    const VideoEditorClipModel& clip,
    double sourceSeconds)
{
    CancelCompatibilityPreview();
    if (previewProxyDirectory_.empty())
    {
        previewProxyDirectory_ = CreatePreviewProxyDirectory();
    }
    if (previewProxyDirectory_.empty())
    {
        previewUsingProxy_ = true;
        statusText_ = L"A temporary folder for the compatibility preview could not be created.";
        InvalidateRect(hwnd_, &previewPanelRect_, FALSE);
        return;
    }

    const unsigned long long generation = ++previewProxyGeneration_;
    const std::wstring sourceKey = PreviewCacheKey(clip.sourcePath);
    const std::filesystem::path outputPath = previewProxyDirectory_ /
        (L"clip-" + std::to_wstring(clip.id) + L"-" +
            std::to_wstring(generation) + L".mp4");
    preview_.Close();
    nativePreviewStartedAt_ = 0;
    nativePreviewClipId_ = 0;
    previewProxyCancel_ = false;
    previewProxyBusy_ = true;
    previewUsingProxy_ = false;
    previewProxyClipId_ = clip.id;
    previewProxySeekSeconds_ = std::clamp(
        sourceSeconds,
        clip.trimInSeconds,
        clip.trimOutSeconds);
    statusText_ = L"Preparing a compatible preview with FFmpeg...";
    UpdateChildWindows();
    InvalidateRect(hwnd_, &previewPanelRect_, FALSE);

    const HWND resultWindow = hwnd_;
    const std::filesystem::path sourcePath = clip.sourcePath;
    const int videoStreamIndex = std::max(0, clip.analysis.videoStreamIndex);
    previewProxyThread_ = std::thread(
        [this, resultWindow, generation, clipId = clip.id, sourceKey, sourcePath, outputPath, videoStreamIndex]()
        {
            auto result = std::make_unique<PreviewProxyThreadResult>();
            result->generation = generation;
            result->clipId = clipId;
            result->sourceKey = sourceKey;
            result->proxyPath = outputPath;

            ExternalToolService toolService;
            const ExternalToolStatus tools = toolService.CheckTools();
            if (!tools.ffmpegFound)
            {
                result->errorMessage =
                    L"This format needs the bundled FFmpeg compatibility preview, but FFmpeg is missing.";
            }
            else
            {
                ProcessRunner runner;
                const std::vector<std::wstring> arguments {
                    L"-hide_banner",
                    L"-loglevel", L"error",
                    L"-nostdin",
                    L"-y",
                    L"-fflags", L"+genpts",
                    L"-i", sourcePath.wstring(),
                    L"-map", L"0:" + std::to_wstring(videoStreamIndex),
                    L"-map", L"0:a:0?",
                    L"-vf", L"scale=w='max(2,trunc(min(1280,iw)/2)*2)':h=-2:flags=fast_bilinear,format=yuv420p",
                    L"-c:v", L"libx264",
                    L"-preset", L"ultrafast",
                    L"-crf", L"26",
                    L"-c:a", L"aac",
                    L"-b:a", L"128k",
                    L"-ac", L"2",
                    L"-sn",
                    L"-map_metadata", L"-1",
                    L"-avoid_negative_ts", L"make_zero",
                    L"-max_muxing_queue_size", L"2048",
                    L"-movflags", L"+faststart",
                    outputPath.wstring()
                };
                const ProcessResult process = runner.Run(
                    tools.ffmpegPath,
                    arguments,
                    previewProxyCancel_,
                    {});
                std::error_code outputError;
                const bool validOutput =
                    process.exitCode == 0 &&
                    std::filesystem::is_regular_file(outputPath, outputError) &&
                    std::filesystem::file_size(outputPath, outputError) > 0;
                if (process.cancelled || previewProxyCancel_.load())
                {
                    result->errorMessage = L"Compatibility preview cancelled.";
                }
                else if (!validOutput)
                {
                    result->errorMessage =
                        L"This video could not be converted into a playable preview. Export may still work.";
                }
            }

            if (!result->errorMessage.empty())
            {
                RemovePreviewPath(outputPath);
                result->proxyPath.clear();
            }
            PreviewProxyThreadResult* raw = result.release();
            if (!PostMessageW(
                resultWindow,
                kPreviewProxyFinishedMessage,
                0,
                reinterpret_cast<LPARAM>(raw)))
            {
                if (raw)
                {
                    RemovePreviewPath(raw->proxyPath);
                    delete raw;
                }
            }
        });
}

void MediaEditorPage::Impl::FinishCompatibilityPreview(
    std::unique_ptr<PreviewProxyThreadResult> result)
{
    if (previewProxyThread_.joinable()) previewProxyThread_.join();
    if (!result)
    {
        previewProxyBusy_ = false;
        return;
    }
    const bool current =
        result->generation == previewProxyGeneration_.load() &&
        result->clipId == previewProxyClipId_;
    if (!current)
    {
        RemovePreviewPath(result->proxyPath);
        return;
    }
    previewProxyBusy_ = false;
    if (!result->errorMessage.empty())
    {
        if (result->errorMessage != L"Compatibility preview cancelled.")
        {
            previewUsingProxy_ = true;
            statusText_ = result->errorMessage;
            InvalidateRect(hwnd_, &previewPanelRect_, FALSE);
        }
        return;
    }

    const int selected = timeline_.SelectedIndex();
    if (selected < 0 || selected >= static_cast<int>(timeline_.Clips().size()) ||
        timeline_.Clips()[selected].id != result->clipId)
    {
        RemovePreviewPath(result->proxyPath);
        return;
    }
    previewProxyCache_[result->sourceKey] = result->proxyPath;
    const VideoEditorClipModel& clip = timeline_.Clips()[selected];
    if (!OpenPreviewPath(
        clip,
        result->proxyPath,
        previewProxySeekSeconds_,
        true))
    {
        statusText_ = L"Windows could not open the generated compatibility preview.";
    }
}

void MediaEditorPage::Impl::CancelCompatibilityPreview()
{
    ++previewProxyGeneration_;
    previewProxyCancel_ = true;
    if (previewProxyThread_.joinable()) previewProxyThread_.join();
    previewProxyCancel_ = false;
    previewProxyBusy_ = false;
    previewProxyClipId_ = 0;
}

void MediaEditorPage::Impl::ClearCompatibilityPreviews()
{
    CancelCompatibilityPreview();
    preview_.Close();
    previewUsingProxy_ = false;
    nativePreviewStartedAt_ = 0;
    nativePreviewClipId_ = 0;
    previewProxyCache_.clear();
    RemovePreviewPath(previewProxyDirectory_, true);
    previewProxyDirectory_.clear();
}

void MediaEditorPage::Impl::SeekTimeline(double timelineSeconds)
{
    timeline_.SetPlayhead(timelineSeconds);
    const std::optional<VideoTimelineLocation> location =
        timeline_.Locate(timeline_.PlayheadSeconds());
    if (!location)
    {
        timelinePlaybackRequested_ = false;
        InvalidateRect(hwnd_, &playRect_, FALSE);
        preview_.Pause();
        InvalidateRect(hwnd_, &timelinePanelRect_, FALSE);
        InvalidateRect(hwnd_, &previewPanelRect_, FALSE);
        return;
    }
    const bool changedClip = timeline_.SelectedIndex() != location->clipIndex;
    timeline_.SelectClip(location->clipIndex);
    if (changedClip)
    {
        LoadSelectedVideoPreview(false);
    }
    previewProxySeekSeconds_ = location->sourceSeconds;
    preview_.Seek(location->sourceSeconds);
    InvalidateRect(hwnd_, &timelinePanelRect_, FALSE);
    InvalidateRect(hwnd_, &previewPanelRect_, FALSE);
}

void MediaEditorPage::Impl::ToggleTimelinePlayback()
{
    if (timelinePlaybackRequested_)
    {
        timelinePlaybackRequested_ = false;
        preview_.Pause();
    }
    else if (preview_.IsReady() && timeline_.SelectedIndex() >= 0)
    {
        timelinePlaybackRequested_ = true;
        preview_.Play();
    }
    InvalidateRect(hwnd_, &playRect_, FALSE);
}

void MediaEditorPage::Impl::StepTimelineFrame(int direction)
{
    const int selected = timeline_.SelectedIndex();
    if (direction == 0 ||
        selected < 0 ||
        selected >= static_cast<int>(timeline_.Clips().size()))
    {
        return;
    }

    const VideoEditorClipModel& clip = timeline_.Clips()[selected];
    double fps = clip.analysis.fps;
    if (!std::isfinite(fps) || fps <= 1.0)
    {
        fps = 30.0;
    }
    const double frameSeconds = 1.0 / fps;
    const double finalFrame = std::max(0.0, timeline_.DurationSeconds() - frameSeconds);
    double target = std::clamp(
        timeline_.PlayheadSeconds() + direction * frameSeconds,
        0.0,
        finalFrame);

    const std::shared_ptr<const std::vector<double>>& frameTimestamps =
        clip.analysis.frameTimestamps;
    if (frameTimestamps && !frameTimestamps->empty())
    {
        const double clipOffset = std::clamp(
            timeline_.PlayheadSeconds() - clip.TimelineStart(),
            0.0,
            clip.Duration());
        const double sourcePosition = clip.trimInSeconds + clipOffset;
        const double tolerance = std::max(0.000001, frameSeconds * 0.1);
        auto frame = frameTimestamps->end();
        if (direction < 0 && sourcePosition > clip.trimInSeconds + tolerance)
        {
            frame = std::lower_bound(
                frameTimestamps->begin(),
                frameTimestamps->end(),
                sourcePosition - tolerance);
            if (frame != frameTimestamps->begin()) --frame;
        }
        else if (direction > 0)
        {
            frame = std::upper_bound(
                frameTimestamps->begin(),
                frameTimestamps->end(),
                sourcePosition + tolerance);
        }

        if (frame != frameTimestamps->end() &&
            *frame >= clip.trimInSeconds - tolerance &&
            *frame < clip.trimOutSeconds - tolerance)
        {
            const double targetSource = std::clamp(
                *frame,
                clip.trimInSeconds,
                clip.trimOutSeconds);
            target = std::clamp(
                clip.TimelineStart() + targetSource - clip.trimInSeconds,
                0.0,
                finalFrame);
        }
    }

    timelinePlaybackRequested_ = false;
    if (direction < 0)
    {
        preview_.Pause();
        SeekTimeline(target);
        InvalidateRect(hwnd_, &playRect_, FALSE);
        return;
    }
    if (preview_.IsReady() && preview_.StepFrame(direction))
    {
        InvalidateRect(hwnd_, &playRect_, FALSE);
        return;
    }
    preview_.Pause();
    SeekTimeline(target);
    InvalidateRect(hwnd_, &playRect_, FALSE);
}

bool MediaEditorPage::Impl::AdvanceTimelinePlayback()
{
    const int currentIndex = timeline_.SelectedIndex();
    if (currentIndex < 0 || currentIndex >= static_cast<int>(timeline_.Clips().size()))
    {
        timelinePlaybackRequested_ = false;
        preview_.Pause();
        return false;
    }

    const VideoEditorClipModel current = timeline_.Clips()[currentIndex];
    const double currentEnd = current.TimelineEnd();
    int nextIndex = -1;
    double nextStart = std::numeric_limits<double>::infinity();
    for (int index = 0; index < static_cast<int>(timeline_.Clips().size()); ++index)
    {
        if (index == currentIndex) continue;
        const double candidateStart = timeline_.Clips()[index].TimelineStart();
        if (candidateStart + 0.001 < currentEnd) continue;
        if (candidateStart < nextStart)
        {
            nextStart = candidateStart;
            nextIndex = index;
        }
    }

    constexpr double maximumContinuousGapSeconds = 0.05;
    if (nextIndex < 0 || nextStart - currentEnd > maximumContinuousGapSeconds)
    {
        timelinePlaybackRequested_ = false;
        timeline_.SetPlayhead(currentEnd);
        preview_.Pause();
        preview_.Seek(current.trimInSeconds);
        InvalidateRect(hwnd_, &playRect_, FALSE);
        InvalidateRect(hwnd_, &timelinePanelRect_, FALSE);
        return false;
    }

    const VideoEditorClipModel next = timeline_.Clips()[nextIndex];
    const bool sameSource = PreviewCacheKey(current.sourcePath) == PreviewCacheKey(next.sourcePath);
    const bool contiguousSource =
        sameSource && std::abs(current.trimOutSeconds - next.trimInSeconds) <= maximumContinuousGapSeconds;
    const bool previewWasPlaying = preview_.IsPlaying();

    timeline_.SelectClip(nextIndex);
    timeline_.SetPlayhead(next.TimelineStart());
    if (sameSource && preview_.IsReady())
    {
        preview_.SetPlaybackRange(next.trimInSeconds, next.trimOutSeconds);
        if (!contiguousSource || !previewWasPlaying)
        {
            preview_.Seek(next.trimInSeconds);
            preview_.Play();
        }
    }
    else
    {
        LoadSelectedVideoPreview(false);
    }

    InvalidateRect(hwnd_, &timelinePanelRect_, FALSE);
    InvalidateRect(hwnd_, &previewPanelRect_, FALSE);
    return true;
}

void MediaEditorPage::Impl::BeginVideoExport()
{
    if (timeline_.Clips().empty() || importBusy_)
    {
        return;
    }
    timelinePlaybackRequested_ = false;
    preview_.Pause();
    keybindsOpen_ = false;
    exportOpen_ = true;
    exportBusy_ = false;
    exportComplete_ = false;
    exportResult_ = {};
    exportProgress_ = {};
    Layout();
    UpdateChildWindows();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

std::optional<std::filesystem::path> MediaEditorPage::Impl::ChooseVideoOutputPath()
{
    std::filesystem::path folder = defaultOutputFolder_;
    if (folder.empty() || !std::filesystem::exists(folder))
    {
        folder = ExternalToolService::DefaultDownloadsFolder();
    }

    unsigned long long targetBytes = 300ULL * 1024ULL * 1024ULL;
    try
    {
        const unsigned long long value = std::stoull(WindowText(sizeLimitEdit_));
        const unsigned long long multiplier = exportSizeUnitGb_
            ? 1024ULL * 1024ULL * 1024ULL
            : 1024ULL * 1024ULL;
        if (value > 0 && value <= std::numeric_limits<unsigned long long>::max() / multiplier)
        {
            targetBytes = value * multiplier;
        }
    }
    catch (...)
    {
    }

    const std::filesystem::path suggested = VideoEditorExportService::SuggestedOutputPath(
        timeline_.Clips(),
        exportMode_,
        targetBytes,
        folder);
    std::vector<wchar_t> fileBuffer(32768, L'\0');
    wcsncpy_s(
        fileBuffer.data(),
        fileBuffer.size(),
        suggested.wstring().c_str(),
        _TRUNCATE);
    const wchar_t filter[] = L"MP4 video\0*.mp4\0\0";
    OPENFILENAMEW dialog {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = fileBuffer.data();
    dialog.nMaxFile = static_cast<DWORD>(fileBuffer.size());
    dialog.lpstrDefExt = L"mp4";
    dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&dialog))
    {
        return std::nullopt;
    }
    std::filesystem::path path(fileBuffer.data());
    if (Lower(path.extension().wstring()) != L".mp4")
    {
        path.replace_extension(L".mp4");
    }
    return path;
}

void MediaEditorPage::Impl::StartVideoExport(const std::filesystem::path& outputPath)
{
    if (exportBusy_ || outputPath.empty())
    {
        return;
    }

    unsigned long long targetValue = 0;
    try
    {
        targetValue = std::stoull(WindowText(sizeLimitEdit_));
    }
    catch (...)
    {
        targetValue = 0;
    }
    if (exportMode_ == VideoEditorExportMode::FitUnderSizeLimit && targetValue == 0)
    {
        statusText_ = L"Enter a size limit of at least 1 MB.";
        InvalidateRect(hwnd_, &exportOverlayRect_, FALSE);
        return;
    }
    const unsigned long long multiplier = exportSizeUnitGb_
        ? 1024ULL * 1024ULL * 1024ULL
        : 1024ULL * 1024ULL;
    if (exportMode_ == VideoEditorExportMode::FitUnderSizeLimit &&
        targetValue > std::numeric_limits<unsigned long long>::max() / multiplier)
    {
        statusText_ = L"That size limit is too large.";
        InvalidateRect(hwnd_, &exportOverlayRect_, FALSE);
        return;
    }

    if (exportThread_.joinable()) exportThread_.join();
    VideoEditorExportOptions options;
    options.mode = exportMode_;
    options.outputPath = outputPath;
    options.encoderMode = exportGpuEnabled_ ? VideoEncoderMode::AutomaticGpu : VideoEncoderMode::Cpu;
    options.targetSizeBytes = exportMode_ == VideoEditorExportMode::FitUnderSizeLimit
        ? targetValue * multiplier
        : 300ULL * 1024ULL * 1024ULL;

    std::vector<VideoEditorClipModel> clips = timeline_.Clips();
    exportCancel_ = false;
    exportBusy_ = true;
    exportComplete_ = false;
    exportProgress_.phase = VideoEditorExportPhase::Preparing;
    exportProgress_.message = L"Preparing the timeline...";
    exportProgress_.progress = 0.0;
    exportProgress_.elapsedSeconds = 0.0;
    exportProgress_.estimatedRemainingSeconds = 0.0;
    exportStartedAt_ = GetTickCount64();
    exportLastClockPaintAt_ = exportStartedAt_;
    UpdateChildWindows();
    InvalidateRect(hwnd_, nullptr, FALSE);
    const HWND resultWindow = hwnd_;

    exportThread_ = std::thread(
        [this, resultWindow, clips = std::move(clips), options]() mutable
        {
            VideoEditorExportResult result = exportService_.Export(
                clips,
                options,
                exportCancel_,
                [resultWindow](const VideoEditorExportProgress& progress)
                {
                    auto* update = new VideoEditorExportProgress(progress);
                    if (!PostMessageW(
                        resultWindow,
                        kExportProgressMessage,
                        0,
                        reinterpret_cast<LPARAM>(update)))
                    {
                        delete update;
                    }
                });
            auto* finished = new VideoEditorExportResult(std::move(result));
            if (!PostMessageW(
                resultWindow,
                kExportFinishedMessage,
                0,
                reinterpret_cast<LPARAM>(finished)))
            {
                delete finished;
            }
        });
}

void MediaEditorPage::Impl::CancelVideoExport()
{
    if (exportBusy_)
    {
        exportCancel_ = true;
        exportProgress_.message = L"Cancelling export...";
        InvalidateRect(hwnd_, &exportOverlayRect_, FALSE);
        return;
    }
    exportOpen_ = false;
    exportComplete_ = false;
    UpdateChildWindows();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MediaEditorPage::Impl::FinishExport(
    std::unique_ptr<VideoEditorExportResult> result)
{
    if (exportThread_.joinable()) exportThread_.join();
    exportBusy_ = false;
    exportStartedAt_ = 0;
    exportComplete_ = true;
    exportResult_ = result ? *result : VideoEditorExportResult {};
    if (!result)
    {
        exportResult_.errorMessage = L"The export ended without a result.";
    }
    statusText_ = exportResult_.success
        ? L"Export complete."
        : exportResult_.cancelled
            ? L"Export cancelled."
            : exportResult_.errorMessage;
    UpdateChildWindows();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MediaEditorPage::Impl::OpenExportFile()
{
    if (!exportResult_.success || exportResult_.outputPath.empty()) return;
    ShellExecuteW(
        hwnd_,
        L"open",
        exportResult_.outputPath.c_str(),
        nullptr,
        exportResult_.outputPath.parent_path().c_str(),
        SW_SHOWNORMAL);
}

void MediaEditorPage::Impl::OpenExportFolder()
{
    if (!exportResult_.success || exportResult_.outputPath.empty()) return;
    const std::wstring parameters =
        L"/select,\"" + exportResult_.outputPath.wstring() + L"\"";
    ShellExecuteW(hwnd_, L"open", L"explorer.exe", parameters.c_str(), nullptr, SW_SHOWNORMAL);
}

std::optional<std::pair<std::filesystem::path, ImageFormat>>
MediaEditorPage::Impl::ChooseImageOutputPath()
{
    std::filesystem::path folder = defaultOutputFolder_;
    if (folder.empty() || !std::filesystem::exists(folder))
    {
        folder = ExternalToolService::DefaultDownloadsFolder();
    }

    std::wstring stem = imageSession_.SourcePath().empty()
        ? L"image_edited"
        : imageSession_.SourcePath().stem().wstring() + L"_edited";
    ImageFormat initialFormat = ImageFormat::Png;
    if (!imageSession_.SourcePath().empty())
    {
        initialFormat = SupportedFormatRegistry::ImageFormatFromExtension(
            imageSession_.SourcePath()).value_or(ImageFormat::Png);
    }
    const std::wstring extension = SupportedFormatRegistry::ExtensionFor(initialFormat);
    const std::filesystem::path suggested = folder / (stem + extension);
    std::vector<wchar_t> fileBuffer(32768, L'\0');
    wcsncpy_s(
        fileBuffer.data(),
        fileBuffer.size(),
        suggested.wstring().c_str(),
        _TRUNCATE);
    const wchar_t filter[] =
        L"PNG image\0*.png\0JPEG image\0*.jpg;*.jpeg\0WEBP image\0*.webp\0BMP image\0*.bmp\0\0";

    OPENFILENAMEW dialog {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = fileBuffer.data();
    dialog.nMaxFile = static_cast<DWORD>(fileBuffer.size());
    dialog.nFilterIndex =
        initialFormat == ImageFormat::Jpg ? 2 :
        initialFormat == ImageFormat::Webp ? 3 :
        initialFormat == ImageFormat::Bmp ? 4 : 1;
    dialog.lpstrDefExt = L"png";
    dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&dialog))
    {
        return std::nullopt;
    }

    const ImageFormat format =
        dialog.nFilterIndex == 2 ? ImageFormat::Jpg :
        dialog.nFilterIndex == 3 ? ImageFormat::Webp :
        dialog.nFilterIndex == 4 ? ImageFormat::Bmp :
        ImageFormat::Png;
    std::filesystem::path path(fileBuffer.data());
    const std::optional<ImageFormat> typedFormat =
        SupportedFormatRegistry::ImageFormatFromExtension(path);
    if (!typedFormat)
    {
        path.replace_extension(SupportedFormatRegistry::ExtensionFor(format));
    }
    return std::make_pair(path, typedFormat.value_or(format));
}

void MediaEditorPage::Impl::SaveImage(bool forceSaveAs)
{
    if (!imageSession_.IsLoaded())
    {
        return;
    }
    std::filesystem::path outputPath = lastImageSavePath_;
    ImageFormat outputFormat = lastImageSaveFormat_;
    if (forceSaveAs || outputPath.empty())
    {
        const auto selection = ChooseImageOutputPath();
        if (!selection)
        {
            return;
        }
        outputPath = selection->first;
        outputFormat = selection->second;
    }

    std::wstring errorMessage;
    if (!imageSession_.SaveAs(outputPath, outputFormat, errorMessage))
    {
        statusText_ = errorMessage;
    }
    else
    {
        lastImageSavePath_ = outputPath;
        lastImageSaveFormat_ = outputFormat;
        statusText_ = L"Saved " + outputPath.filename().wstring() + L".";
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MediaEditorPage::Impl::CopyImage()
{
    if (!imageSession_.IsLoaded())
    {
        return;
    }
    MediaEditorImageBuffer flattened;
    std::wstring errorMessage;
    if (!imageSession_.Flatten(flattened, errorMessage) ||
        !clipboardService_.CopyImage(flattened, errorMessage))
    {
        statusText_ = errorMessage;
    }
    else
    {
        statusText_ = L"Finished image copied to the clipboard.";
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}
RECT MediaEditorPage::Impl::HitButton(POINT point) const
{
    auto hit = [&](const RECT& rect) -> RECT
    {
        return HasArea(rect) && Contains(rect, point) ? rect : RECT {};
    };

    if (keybindsOpen_)
    {
        return hit(keybindsCloseRect_);
    }
    if (exportOpen_)
    {
        if (exportBusy_) return hit(exportCancelRect_);
        if (exportComplete_)
        {
            for (const RECT& rect : { exportOpenFileRect_, exportOpenFolderRect_, exportDoneRect_ })
            {
                if (Contains(rect, point)) return rect;
            }
            return {};
        }
        if (exportMode_ == VideoEditorExportMode::FitUnderSizeLimit &&
            Contains(exportSizeUnitRect_, point))
        {
            return exportSizeUnitRect_;
        }
        for (const RECT& rect : exportModeRects_)
        {
            if (Contains(rect, point)) return rect;
        }
        for (const RECT& rect : { exportSizeUnitRect_, exportGpuRowRect_, exportGpuToggleRect_, exportCancelRect_, exportStartRect_ })
        {
            if (Contains(rect, point)) return rect;
        }
        return {};
    }
    if (contextPasteOpen_ && Contains(contextPasteRect_, point))
    {
        return contextPasteRect_;
    }
    if (view_ == EditorView::Import)
    {
        for (const RECT& rect : { chooseFileRect_, pasteRect_, importDropRect_ })
        {
            if (Contains(rect, point)) return rect;
        }
        return {};
    }
    if (view_ == EditorView::Video)
    {
        if (!exportBusy_ && timeline_.CanUndo() && Contains(undoRect_, point))
            return undoRect_;
        if (!exportBusy_ && timeline_.CanRedo() && Contains(redoRect_, point))
            return redoRect_;
        for (const RECT& rect : {
            keybindsRect_, newEditRect_, playRect_, muteRect_, addClipRect_,
            splitRect_, deleteRect_, moveLeftRect_, moveRightRect_, exportRect_ })
        {
            if (Contains(rect, point)) return rect;
        }
        return {};
    }

    if (imageSession_.CanUndo() && Contains(undoRect_, point))
        return undoRect_;
    if (imageSession_.CanRedo() && Contains(redoRect_, point))
        return redoRect_;
    for (const RECT& rect : {
        newEditRect_, drawRect_, colorRect_, thicknessRect_, eraserRect_,
        textToolRect_, rotateRect_, resetCropRect_, fitRect_, actualSizeRect_,
        copyRect_, saveRect_, saveMenuRect_, saveAsRect_, colorCustomRect_,
        thicknessSliderRect_ })
    {
        if (HasArea(rect) && Contains(rect, point)) return rect;
    }
    for (const RECT& rect : colorSwatchRects_)
    {
        if (HasArea(rect) && Contains(rect, point)) return rect;
    }
    for (const RECT& rect : thicknessRects_)
    {
        if (HasArea(rect) && Contains(rect, point)) return rect;
    }
    for (const RECT& rect : zoomOptionRects_)
    {
        if (HasArea(rect) && Contains(rect, point)) return rect;
    }
    return {};
}

void MediaEditorPage::Impl::UpdateHover(POINT point)
{
    const RECT latest = HitButton(point);
    if (!EqualRect(&latest, &hoveredRect_))
    {
        const RECT old = hoveredRect_;
        hoveredRect_ = latest;
        if (HasArea(old)) InvalidateRect(hwnd_, &old, FALSE);
        if (HasArea(latest)) InvalidateRect(hwnd_, &latest, FALSE);
    }
}

int MediaEditorPage::Impl::TimelineClipAt(POINT point) const
{
    for (size_t index = 0; index < timelineClipRects_.size(); ++index)
    {
        if (Contains(timelineClipRects_[index], point))
        {
            return static_cast<int>(index);
        }
    }
    return -1;
}

double MediaEditorPage::Impl::TimelineSecondsAtX(int x) const
{
    if (!HasArea(timelineTrackRect_) || timeline_.DurationSeconds() <= 0.0)
    {
        return 0.0;
    }
    const double ratio = std::clamp(
        static_cast<double>(x - timelineTrackRect_.left) /
            static_cast<double>(std::max(1L, timelineTrackRect_.right - timelineTrackRect_.left)),
        0.0,
        1.0);
    return timelineViewStartSeconds_ + ratio * TimelineVisibleDuration();
}

int MediaEditorPage::Impl::TimelineXAtSeconds(double seconds) const
{
    if (!HasArea(timelineTrackRect_) || timeline_.DurationSeconds() <= 0.0)
    {
        return timelineTrackRect_.left;
    }
    const double visibleDuration = std::max(0.001, TimelineVisibleDuration());
    const double ratio = (seconds - timelineViewStartSeconds_) / visibleDuration;
    return timelineTrackRect_.left + static_cast<int>(std::llround(
        ratio * (timelineTrackRect_.right - timelineTrackRect_.left)));
}

double MediaEditorPage::Impl::TimelineVisibleDuration() const
{
    const double totalDuration = timeline_.DurationSeconds();
    if (totalDuration <= 0.0)
    {
        return 0.0;
    }
    return totalDuration / std::clamp(timelineZoom_, 1.0, 32.0);
}

void MediaEditorPage::Impl::ClampTimelineView()
{
    timelineZoom_ = std::clamp(timelineZoom_, 1.0, 32.0);
    const double totalDuration = timeline_.DurationSeconds();
    if (totalDuration <= 0.0)
    {
        timelineViewStartSeconds_ = 0.0;
        return;
    }
    const double maximumStart = std::max(0.0, totalDuration - TimelineVisibleDuration());
    timelineViewStartSeconds_ = std::clamp(timelineViewStartSeconds_, 0.0, maximumStart);
}

void MediaEditorPage::Impl::SetTimelineZoom(double zoom, int anchorX)
{
    if (!HasArea(timelineTrackRect_) || timeline_.DurationSeconds() <= 0.0)
    {
        return;
    }
    const double oldVisibleDuration = TimelineVisibleDuration();
    const double anchorRatio = std::clamp(
        static_cast<double>(anchorX - timelineTrackRect_.left) /
            static_cast<double>(std::max(1L, timelineTrackRect_.right - timelineTrackRect_.left)),
        0.0,
        1.0);
    const double anchorSeconds = timelineViewStartSeconds_ + anchorRatio * oldVisibleDuration;
    timelineZoom_ = std::clamp(zoom, 1.0, 32.0);
    timelineViewStartSeconds_ = anchorSeconds - anchorRatio * TimelineVisibleDuration();
    ClampTimelineView();
}

double MediaEditorPage::Impl::SnappedTimelineStart(double desiredStart, int movingIndex) const
{
    if (movingIndex < 0 || movingIndex >= static_cast<int>(timeline_.Clips().size()))
    {
        return std::max(0.0, desiredStart);
    }
    const auto& clips = timeline_.Clips();
    const double clipDuration = clips[movingIndex].Duration();
    const double secondsPerPixel = TimelineVisibleDuration() /
        static_cast<double>(std::max(1L, timelineTrackRect_.right - timelineTrackRect_.left));
    const double snapDistance = std::max(0.02, secondsPerPixel * Dips(9));
    double snapped = std::max(0.0, desiredStart);
    double nearestDistance = snapDistance;
    auto consider = [&](double candidate)
    {
        candidate = std::max(0.0, candidate);
        const double distance = std::abs(candidate - desiredStart);
        if (distance <= nearestDistance)
        {
            snapped = candidate;
            nearestDistance = distance;
        }
    };
    consider(0.0);
    consider(std::max(0.0, timeline_.DurationSeconds() - clipDuration));
    for (int index = 0; index < static_cast<int>(clips.size()); ++index)
    {
        if (index == movingIndex) continue;
        consider(clips[index].TimelineEnd());
        consider(clips[index].TimelineStart() - clipDuration);
    }
    return snapped;
}

CropHandle MediaEditorPage::Impl::CropHandleAt(POINT point) const
{
    const std::array<CropHandle, 8> handles {
        CropHandle::TopLeft,
        CropHandle::Top,
        CropHandle::TopRight,
        CropHandle::Left,
        CropHandle::Right,
        CropHandle::BottomLeft,
        CropHandle::Bottom,
        CropHandle::BottomRight
    };
    for (size_t index = 0; index < cropHandleRects_.size(); ++index)
    {
        RECT generous = cropHandleRects_[index];
        InflateRect(&generous, Dips(3), Dips(3));
        if (Contains(generous, point))
        {
            return handles[index];
        }
    }
    if (Contains(cropMoveRect_, point))
    {
        return CropHandle::Move;
    }
    return CropHandle::None;
}

std::optional<MediaEditorPoint> MediaEditorPage::Impl::CanvasToImage(POINT point) const
{
    if (!imageSession_.IsLoaded() || imageScale_ <= 0.0f)
    {
        return std::nullopt;
    }
    const float x = (point.x - imageDisplayRect_.left) / imageScale_;
    const float y = (point.y - imageDisplayRect_.top) / imageScale_;
    if (x < 0.0f || y < 0.0f ||
        x > static_cast<float>(imageSession_.Width()) ||
        y > static_cast<float>(imageSession_.Height()))
    {
        return std::nullopt;
    }
    return MediaEditorPoint { x, y };
}

POINT MediaEditorPage::Impl::ImageToCanvas(MediaEditorPoint point) const
{
    return {
        imageDisplayRect_.left + static_cast<LONG>(std::lround(point.x * imageScale_)),
        imageDisplayRect_.top + static_cast<LONG>(std::lround(point.y * imageScale_))
    };
}

void MediaEditorPage::Impl::RebuildImagePreviewCache()
{
    imagePreviewCache_.reset();
    if (!imageSession_.IsLoaded())
    {
        return;
    }
    const MediaEditorImageBuffer& image = imageSession_.Image();
    constexpr UINT maximumPreviewDimension = 3072;
    const UINT largestDimension = std::max(image.width, image.height);
    if (largestDimension <= maximumPreviewDimension)
    {
        return;
    }

    const double scale = static_cast<double>(maximumPreviewDimension) /
        static_cast<double>(largestDimension);
    const UINT previewWidth = std::max<UINT>(1, static_cast<UINT>(std::lround(image.width * scale)));
    const UINT previewHeight = std::max<UINT>(1, static_cast<UINT>(std::lround(image.height * scale)));
    auto preview = std::make_unique<Gdiplus::Bitmap>(
        static_cast<INT>(previewWidth),
        static_cast<INT>(previewHeight),
        PixelFormat32bppPARGB);
    if (preview->GetLastStatus() != Gdiplus::Ok)
    {
        return;
    }

    Gdiplus::Bitmap source(
        static_cast<INT>(image.width),
        static_cast<INT>(image.height),
        static_cast<INT>(image.stride),
        PixelFormat32bppPARGB,
        const_cast<BYTE*>(image.pixels.data()));
    Gdiplus::Graphics graphics(preview.get());
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    if (graphics.DrawImage(&source, 0, 0, previewWidth, previewHeight) == Gdiplus::Ok)
    {
        imagePreviewCache_ = std::move(preview);
    }
}

void MediaEditorPage::Impl::FitImageToCanvas()
{
    if (!imageSession_.IsLoaded() || !HasArea(imageCanvasRect_))
    {
        imageDisplayRect_ = {};
        return;
    }
    const float availableWidth = static_cast<float>(
        std::max(1L, imageCanvasRect_.right - imageCanvasRect_.left - Dips(36)));
    const float availableHeight = static_cast<float>(
        std::max(1L, imageCanvasRect_.bottom - imageCanvasRect_.top - Dips(54)));
    imageScale_ = std::max(
        0.01f,
        std::min(
            availableWidth / static_cast<float>(imageSession_.Width()),
            availableHeight / static_cast<float>(imageSession_.Height())));
    imagePan_ = { 0.0f, 0.0f };
    imageFitMode_ = true;
    ClampImagePan();
}

void MediaEditorPage::Impl::ClampImagePan()
{
    if (!imageSession_.IsLoaded() || !HasArea(imageCanvasRect_))
    {
        imageDisplayRect_ = {};
        return;
    }
    const float displayWidth = imageSession_.Width() * imageScale_;
    const float displayHeight = imageSession_.Height() * imageScale_;
    const float canvasWidth = static_cast<float>(imageCanvasRect_.right - imageCanvasRect_.left);
    const float canvasHeight = static_cast<float>(imageCanvasRect_.bottom - imageCanvasRect_.top);
    const float maxPanX = std::max(0.0f, (displayWidth - canvasWidth) * 0.5f + Dips(60));
    const float maxPanY = std::max(0.0f, (displayHeight - canvasHeight) * 0.5f + Dips(60));
    imagePan_.X = std::clamp(imagePan_.X, -maxPanX, maxPanX);
    imagePan_.Y = std::clamp(imagePan_.Y, -maxPanY, maxPanY);

    const float centerX = (imageCanvasRect_.left + imageCanvasRect_.right) * 0.5f + imagePan_.X;
    const float centerY = (imageCanvasRect_.top + imageCanvasRect_.bottom) * 0.5f + imagePan_.Y;
    imageDisplayRect_ = {
        static_cast<LONG>(std::lround(centerX - displayWidth * 0.5f)),
        static_cast<LONG>(std::lround(centerY - displayHeight * 0.5f)),
        static_cast<LONG>(std::lround(centerX + displayWidth * 0.5f)),
        static_cast<LONG>(std::lround(centerY + displayHeight * 0.5f))
    };
}

void MediaEditorPage::Impl::SetCursorForPoint(POINT point)
{
    if (keybindsOpen_ || exportOpen_)
    {
        SetCursor(LoadCursorW(nullptr, HasArea(HitButton(point)) ? IDC_HAND : IDC_ARROW));
        return;
    }
    if (view_ == EditorView::Image && Contains(imageCanvasRect_, point))
    {
        if (textToolEnabled_ && !spaceHeld_ && CanvasToImage(point))
        {
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            return;
        }
        switch (CropHandleAt(point))
        {
        case CropHandle::Left:
        case CropHandle::Right:
            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
            return;
        case CropHandle::Top:
        case CropHandle::Bottom:
            SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
            return;
        case CropHandle::TopLeft:
        case CropHandle::BottomRight:
            SetCursor(LoadCursorW(nullptr, IDC_SIZENWSE));
            return;
        case CropHandle::TopRight:
        case CropHandle::BottomLeft:
            SetCursor(LoadCursorW(nullptr, IDC_SIZENESW));
            return;
        case CropHandle::Move:
            SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            return;
        case CropHandle::None:
            break;
        }
        if (CanvasToImage(point))
        {
            SetCursor(LoadCursorW(nullptr, spaceHeld_ ? IDC_SIZEALL : IDC_CROSS));
            return;
        }
    }
    if (view_ == EditorView::Video)
    {
        for (const RECT& rect : timelineLeftHandleRects_)
        {
            if (Contains(rect, point))
            {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                return;
            }
        }
        for (const RECT& rect : timelineRightHandleRects_)
        {
            if (Contains(rect, point))
            {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                return;
            }
        }
        if (Contains(volumeRect_, point) && !preview_.IsMuted())
        {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return;
        }
        if (Contains(timelineScrollbarRect_, point) && timelineZoom_ > 1.001)
        {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return;
        }
        if (TimelineClipAt(point) >= 0)
        {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            return;
        }
        if (Contains(timelineTrackRect_, point))
        {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return;
        }
    }
    SetCursor(LoadCursorW(nullptr, HasArea(HitButton(point)) ? IDC_HAND : IDC_ARROW));
}

void MediaEditorPage::Impl::OnLeftButtonDown(POINT point, WPARAM)
{
    contextPasteOpen_ = contextPasteOpen_ && Contains(contextPasteRect_, point);

    if (keybindsOpen_ || exportOpen_)
    {
        pressedRect_ = HitButton(point);
        if (HasArea(pressedRect_))
        {
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, &pressedRect_, FALSE);
        }
        return;
    }

    if (view_ == EditorView::Video && !exportBusy_)
    {
        if (Contains(timelineScrollbarRect_, point) && timelineZoom_ > 1.001)
        {
            if (!Contains(timelineScrollbarThumbRect_, point))
            {
                const int scrollbarWidth = timelineScrollbarRect_.right - timelineScrollbarRect_.left;
                const int thumbWidth = timelineScrollbarThumbRect_.right - timelineScrollbarThumbRect_.left;
                const int travel = std::max(0, scrollbarWidth - thumbWidth);
                const int thumbLeft = std::clamp(
                    static_cast<int>(point.x) - thumbWidth / 2,
                    static_cast<int>(timelineScrollbarRect_.left),
                    static_cast<int>(timelineScrollbarRect_.left + travel));
                const double maximumStart = std::max(
                    0.0,
                    timeline_.DurationSeconds() - TimelineVisibleDuration());
                timelineViewStartSeconds_ = travel > 0
                    ? maximumStart * (thumbLeft - timelineScrollbarRect_.left) / travel
                    : 0.0;
                ClampTimelineView();
                Layout();
            }
            timelineScrollStartX_ = point.x;
            timelineScrollStartSeconds_ = timelineViewStartSeconds_;
            pointerAction_ = PointerAction::TimelineScroll;
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, &timelinePanelRect_, FALSE);
            return;
        }

        if (Contains(volumeRect_, point) && !preview_.IsMuted())
        {
            pointerAction_ = PointerAction::VideoVolume;
            pointerStart_ = point;
            SetCapture(hwnd_);
            OnMouseMove(point, MK_LBUTTON);
            return;
        }

        for (size_t index = 0; index < timelineClipRects_.size(); ++index)
        {
            const bool leftHandle = Contains(timelineLeftHandleRects_[index], point);
            const bool rightHandle = Contains(timelineRightHandleRects_[index], point);
            if (leftHandle || rightHandle)
            {
                if (timeline_.SelectedIndex() != static_cast<int>(index))
                {
                    timeline_.SelectClip(static_cast<int>(index));
                    LoadSelectedVideoPreview();
                }
                const VideoEditorClipModel& clip = timeline_.Clips()[index];
                timeline_.BeginEdit();
                trimStartIn_ = clip.trimInSeconds;
                trimStartOut_ = clip.trimOutSeconds;
                trimPixelsPerSecond_ = std::max(
                    0.001,
                    static_cast<double>(timelineTrackRect_.right - timelineTrackRect_.left) /
                        std::max(0.001, TimelineVisibleDuration()));
                pointerStart_ = point;
                pointerAction_ = leftHandle
                    ? PointerAction::TimelineTrimLeft
                    : PointerAction::TimelineTrimRight;
                SetCapture(hwnd_);
                InvalidateRect(hwnd_, &timelinePanelRect_, FALSE);
                return;
            }
        }

        const int clipIndex = TimelineClipAt(point);
        if (clipIndex >= 0)
        {
            pointerStart_ = point;
            if (timeline_.SelectedIndex() != clipIndex)
            {
                timeline_.SelectClip(clipIndex);
                LoadSelectedVideoPreview();
            }
            dragClipIndex_ = timeline_.SelectedIndex();
            timeline_.BeginEdit();
            dragStartTimelineSeconds_ = timeline_.ClipStartTime(dragClipIndex_);
            dragGrabOffsetSeconds_ = std::clamp(
                TimelineSecondsAtX(point.x) - dragStartTimelineSeconds_,
                0.0,
                timeline_.Clips()[dragClipIndex_].Duration());
            dragMoved_ = false;
            dragInsertTargetId_ = 0;
            dragInsertAfter_ = false;
            pointerAction_ = PointerAction::TimelineReorder;
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, &timelinePanelRect_, FALSE);
            return;
        }
        if (Contains(timelineTrackRect_, point) && timeline_.DurationSeconds() > 0.0)
        {
            pointerAction_ = PointerAction::TimelineSeek;
            SetCapture(hwnd_);
            OnMouseMove(point, MK_LBUTTON);
            return;
        }
    }

    if (view_ == EditorView::Image && imageSession_.IsLoaded())
    {
        if (imageToolbarPopup_ != ImageToolbarPopup::None)
        {
            RECT popupRect {};
            switch (imageToolbarPopup_)
            {
            case ImageToolbarPopup::Color: popupRect = colorPopupRect_; break;
            case ImageToolbarPopup::Thickness: popupRect = thicknessPopupRect_; break;
            case ImageToolbarPopup::Zoom: popupRect = zoomPopupRect_; break;
            case ImageToolbarPopup::Save: popupRect = savePopupRect_; break;
            case ImageToolbarPopup::None: break;
            }

            if (imageToolbarPopup_ == ImageToolbarPopup::Thickness &&
                Contains(thicknessSliderRect_, point))
            {
                pointerAction_ = PointerAction::ImageThickness;
                SetCapture(hwnd_);
                OnMouseMove(point, MK_LBUTTON);
                return;
            }

            const bool overPopupAnchor =
                Contains(colorRect_, point) ||
                Contains(thicknessRect_, point) ||
                Contains(actualSizeRect_, point) ||
                Contains(saveMenuRect_, point);
            if (HasArea(popupRect) && Contains(popupRect, point))
            {
                pressedRect_ = HitButton(point);
                if (HasArea(pressedRect_))
                {
                    SetCapture(hwnd_);
                    InvalidateRect(hwnd_, &pressedRect_, FALSE);
                }
                return;
            }
            if (!overPopupAnchor)
            {
                imageToolbarPopup_ = ImageToolbarPopup::None;
                hoveredRect_ = {};
                Layout();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
        if (Contains(opacityRect_, point))
        {
            pointerAction_ = PointerAction::ImageOpacity;
            SetCapture(hwnd_);
            OnMouseMove(point, MK_LBUTTON);
            return;
        }

        if (textToolEnabled_ &&
            !spaceHeld_ &&
            Contains(imageCanvasRect_, point))
        {
            if (const auto imagePoint = CanvasToImage(point))
            {
                textDragStart_ = *imagePoint;
                pendingTextBounds_ = {
                    imagePoint->x,
                    imagePoint->y,
                    imagePoint->x,
                    imagePoint->y
                };
                pointerAction_ = PointerAction::ImageTextBox;
                SetCapture(hwnd_);
                InvalidateRect(hwnd_, &imageCanvasRect_, FALSE);
                return;
            }
        }

        const CropHandle cropHandle = CropHandleAt(point);
        if (cropHandle != CropHandle::None)
        {
            activeCropHandle_ = cropHandle;
            pointerStart_ = point;
            cropStart_ = imageSession_.Crop();
            imageSession_.BeginEdit();
            pointerAction_ = PointerAction::ImageCrop;
            SetCapture(hwnd_);
            return;
        }

        if (Contains(imageCanvasRect_, point))
        {
            const std::optional<MediaEditorPoint> imagePoint = CanvasToImage(point);
            if (spaceHeld_)
            {
                pointerStart_ = point;
                panStart_ = imagePan_;
                pointerAction_ = PointerAction::ImagePan;
                imageFitMode_ = false;
                SetCapture(hwnd_);
                return;
            }
            if (imagePoint)
            {
                currentStroke_ = {};
                currentStroke_.color = drawingColor_;
                currentStroke_.thickness = eraserEnabled_
                    ? std::max(10.0f, drawingThickness_ * 2.5f)
                    : drawingThickness_;
                currentStroke_.opacity = drawingOpacity_;
                currentStroke_.eraser = eraserEnabled_;
                currentStroke_.points.push_back(*imagePoint);
                imageEraseChanged_ = false;
                if (currentStroke_.eraser)
                {
                    imageSession_.BeginEdit();
                    imageEraseChanged_ = imageSession_.AddStroke(currentStroke_);
                }
                drawingStartTime_ = static_cast<DWORD>(GetMessageTime());
                lastDrawingSampleTime_ = drawingStartTime_;
                lastDrawingScreenPoint_ = point;
                ClientToScreen(hwnd_, &lastDrawingScreenPoint_);
                pointerAction_ = PointerAction::ImageDraw;
                SetCapture(hwnd_);
                InvalidateRect(hwnd_, &imageCanvasRect_, FALSE);
                return;
            }
        }
    }

    pressedRect_ = HitButton(point);
    if (HasArea(pressedRect_))
    {
        SetCapture(hwnd_);
        InvalidateRect(hwnd_, &pressedRect_, FALSE);
    }
    else
    {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void MediaEditorPage::Impl::OnMouseMove(POINT point, WPARAM)
{
    if (pointerAction_ == PointerAction::None)
    {
        UpdateHover(point);
        return;
    }

    switch (pointerAction_)
    {
    case PointerAction::VideoVolume:
        videoVolume_ = std::clamp(
            static_cast<float>(point.x - volumeRect_.left) /
                static_cast<float>(std::max(1L, volumeRect_.right - volumeRect_.left)),
            0.0f,
            1.0f);
        preview_.SetVolume(videoVolume_);
        InvalidateRect(hwnd_, &previewPanelRect_, FALSE);
        break;

    case PointerAction::TimelineSeek:
        if (timeline_.DurationSeconds() > 0.0)
        {
            SeekTimeline(TimelineSecondsAtX(point.x));
        }
        break;

    case PointerAction::TimelineTrimLeft:
    case PointerAction::TimelineTrimRight:
    {
        const int selected = timeline_.SelectedIndex();
        if (selected >= 0)
        {
            const double delta = (point.x - pointerStart_.x) / trimPixelsPerSecond_;
            const double trimIn = pointerAction_ == PointerAction::TimelineTrimLeft
                ? trimStartIn_ + delta
                : trimStartIn_;
            const double trimOut = pointerAction_ == PointerAction::TimelineTrimRight
                ? trimStartOut_ + delta
                : trimStartOut_;
            timeline_.SetTrim(selected, trimIn, trimOut, false);
            Layout();
            InvalidateRect(hwnd_, &timelinePanelRect_, FALSE);
        }
        break;
    }

    case PointerAction::ImageDraw:
        AppendDrawingSamples(point);
        break;

    case PointerAction::ImageTextBox:
    {
        const float x = std::clamp(
            static_cast<float>(point.x - imageDisplayRect_.left) /
                std::max(0.01f, imageScale_),
            0.0f,
            static_cast<float>(imageSession_.Width()));
        const float y = std::clamp(
            static_cast<float>(point.y - imageDisplayRect_.top) /
                std::max(0.01f, imageScale_),
            0.0f,
            static_cast<float>(imageSession_.Height()));
        pendingTextBounds_ = {
            std::min(textDragStart_.x, x),
            std::min(textDragStart_.y, y),
            std::max(textDragStart_.x, x),
            std::max(textDragStart_.y, y)
        };
        InvalidateRect(hwnd_, &imageCanvasRect_, FALSE);
        break;
    }

    case PointerAction::ImageCrop:
    {
        const float dx = (point.x - pointerStart_.x) / std::max(0.01f, imageScale_);
        const float dy = (point.y - pointerStart_.y) / std::max(0.01f, imageScale_);
        MediaEditorCropRect crop = cropStart_;
        switch (activeCropHandle_)
        {
        case CropHandle::Left: crop.left += dx; break;
        case CropHandle::Top: crop.top += dy; break;
        case CropHandle::Right: crop.right += dx; break;
        case CropHandle::Bottom: crop.bottom += dy; break;
        case CropHandle::TopLeft: crop.left += dx; crop.top += dy; break;
        case CropHandle::TopRight: crop.right += dx; crop.top += dy; break;
        case CropHandle::BottomLeft: crop.left += dx; crop.bottom += dy; break;
        case CropHandle::BottomRight: crop.right += dx; crop.bottom += dy; break;
        case CropHandle::Move:
        {
            const float moveX = std::clamp(
                dx,
                -cropStart_.left,
                static_cast<float>(imageSession_.Width()) - cropStart_.right);
            const float moveY = std::clamp(
                dy,
                -cropStart_.top,
                static_cast<float>(imageSession_.Height()) - cropStart_.bottom);
            crop = { cropStart_.left + moveX, cropStart_.top + moveY, cropStart_.right + moveX, cropStart_.bottom + moveY };
            break;
        }
        case CropHandle::None: break;
        }
        imageSession_.SetCrop(crop);
        InvalidateRect(hwnd_, &imageCanvasRect_, FALSE);
        break;
    }

    case PointerAction::ImagePan:
        imagePan_.X = panStart_.X + static_cast<float>(point.x - pointerStart_.x);
        imagePan_.Y = panStart_.Y + static_cast<float>(point.y - pointerStart_.y);
        ClampImagePan();
        InvalidateRect(hwnd_, &imageCanvasRect_, FALSE);
        break;

    case PointerAction::ImageOpacity:
        drawingOpacity_ = std::clamp(
            static_cast<float>(point.x - opacityRect_.left) /
                static_cast<float>(std::max(1L, opacityRect_.right - opacityRect_.left)),
            0.05f,
            1.0f);
        InvalidateRect(hwnd_, &imageToolbarRect_, FALSE);
        break;
    case PointerAction::ImageThickness:
    {
        const float amount = std::clamp(
            static_cast<float>(point.x - thicknessSliderRect_.left) /
                static_cast<float>(std::max(1L, thicknessSliderRect_.right - thicknessSliderRect_.left)),
            0.0f,
            1.0f);
        drawingThickness_ = 1.0f + amount * 29.0f;
        InvalidateRect(hwnd_, &imageToolbarRect_, FALSE);
        InvalidateRect(hwnd_, &thicknessPopupRect_, FALSE);
        break;
    }

    case PointerAction::TimelineScroll:
    {
        const int scrollbarWidth = timelineScrollbarRect_.right - timelineScrollbarRect_.left;
        const int thumbWidth = timelineScrollbarThumbRect_.right - timelineScrollbarThumbRect_.left;
        const int travel = std::max(0, scrollbarWidth - thumbWidth);
        const double maximumStart = std::max(
            0.0,
            timeline_.DurationSeconds() - TimelineVisibleDuration());
        if (travel > 0 && maximumStart > 0.0)
        {
            timelineViewStartSeconds_ = timelineScrollStartSeconds_ +
                static_cast<double>(point.x - timelineScrollStartX_) * maximumStart / travel;
            ClampTimelineView();
            Layout();
            InvalidateRect(hwnd_, &timelinePanelRect_, FALSE);
        }
        break;
    }

    case PointerAction::TimelineReorder:
        if (timeline_.SelectedIndex() >= 0 &&
            (dragMoved_ || std::abs(point.x - pointerStart_.x) >= Dips(4)))
        {
            const int movingIndex = timeline_.SelectedIndex();
            const int targetIndex = TimelineClipAt(point);
            if (targetIndex >= 0 && targetIndex != movingIndex)
            {
                const RECT& targetRect = timelineClipRects_[targetIndex];
                dragInsertTargetId_ = timeline_.Clips()[targetIndex].id;
                dragInsertAfter_ = point.x >= (targetRect.left + targetRect.right) / 2;
                dragMoved_ = true;
                InvalidateRect(hwnd_, &timelinePanelRect_, FALSE);
                break;
            }
            if (dragInsertTargetId_ != 0)
            {
                dragInsertTargetId_ = 0;
                dragInsertAfter_ = false;
                InvalidateRect(hwnd_, &timelinePanelRect_, FALSE);
            }
            const double desiredStart = TimelineSecondsAtX(point.x) - dragGrabOffsetSeconds_;
            const double snappedStart = SnappedTimelineStart(desiredStart, movingIndex);
            if (timeline_.MoveSelectedTo(snappedStart, false))
            {
                dragMoved_ = true;
                dragClipIndex_ = timeline_.SelectedIndex();
                Layout();
                InvalidateRect(hwnd_, &timelinePanelRect_, FALSE);
            }
        }
        break;

    case PointerAction::None:
        break;
    }
}

void MediaEditorPage::Impl::OnLeftButtonUp(POINT point, WPARAM)
{
    const PointerAction completedAction = pointerAction_;
    if (completedAction == PointerAction::ImageDraw)
    {
        AppendDrawingSamples(point);
    }
    else if (completedAction == PointerAction::ImageTextBox)
    {
        OnMouseMove(point, MK_LBUTTON);
    }
    pointerAction_ = PointerAction::None;

    if (completedAction == PointerAction::TimelineTrimLeft ||
        completedAction == PointerAction::TimelineTrimRight)
    {
        timeline_.CommitEdit();
        Layout();
        LoadSelectedVideoPreview();
    }
    else if (completedAction == PointerAction::TimelineReorder)
    {
        if (dragInsertTargetId_ != 0)
        {
            const unsigned long long targetId = dragInsertTargetId_;
            const bool insertAfter = dragInsertAfter_;
            timeline_.CancelEdit();
            const int selected = timeline_.SelectedIndex();
            int targetRemainingIndex = -1;
            int remainingIndex = 0;
            for (int index = 0; index < static_cast<int>(timeline_.Clips().size()); ++index)
            {
                if (index == selected) continue;
                if (timeline_.Clips()[index].id == targetId)
                {
                    targetRemainingIndex = remainingIndex;
                    break;
                }
                ++remainingIndex;
            }
            bool inserted = false;
            if (selected >= 0 && targetRemainingIndex >= 0)
            {
                const int destination = std::clamp(
                    targetRemainingIndex + (insertAfter ? 1 : 0),
                    0,
                    static_cast<int>(timeline_.Clips().size()) - 1);
                inserted = timeline_.MoveSelected(destination);
            }
            if (inserted && timeline_.SelectedIndex() >= 0)
            {
                const VideoEditorClipModel& clip = timeline_.Clips()[timeline_.SelectedIndex()];
                preview_.Pause();
                preview_.Seek(clip.trimInSeconds);
                timeline_.SetPlayhead(clip.TimelineStart());
            }
            dragMoved_ = inserted;
            Layout();
            InvalidateRect(hwnd_, &previewPanelRect_, FALSE);
        }
        else if (dragMoved_)
        {
            timeline_.CommitEdit();
            const int selected = timeline_.SelectedIndex();
            if (selected >= 0)
            {
                const VideoEditorClipModel& clip = timeline_.Clips()[selected];
                timeline_.SetPlayhead(clip.TimelineStart());
                preview_.Pause();
                preview_.Seek(clip.trimInSeconds);
                preview_.UpdateVideo();
            }
            Layout();
            InvalidateRect(hwnd_, &previewPanelRect_, FALSE);
        }
        else
        {
            timeline_.CancelEdit();
            Layout();
            if (Contains(timelineTrackRect_, point) && timeline_.DurationSeconds() > 0.0)
            {
                SeekTimeline(TimelineSecondsAtX(point.x));
            }
        }
        dragClipIndex_ = -1;
        dragMoved_ = false;
        dragInsertTargetId_ = 0;
        dragInsertAfter_ = false;
        InvalidateRect(hwnd_, &timelinePanelRect_, FALSE);
    }
    else if (completedAction == PointerAction::ImageCrop)
    {
        imageSession_.CommitEdit();
        activeCropHandle_ = CropHandle::None;
        InvalidateRect(hwnd_, &undoRect_, FALSE);
        InvalidateRect(hwnd_, &redoRect_, FALSE);
        InvalidateRect(hwnd_, &imageCanvasRect_, FALSE);
    }
    else if (completedAction == PointerAction::ImageTextBox)
    {
        const MediaEditorCropRect bounds = pendingTextBounds_;
        pendingTextBounds_ = {};
        const float screenWidth = bounds.Width() * imageScale_;
        const float screenHeight = bounds.Height() * imageScale_;
        if (screenWidth >= static_cast<float>(Dips(48)) &&
            screenHeight >= static_cast<float>(Dips(30)))
        {
            BeginTextEditing(bounds);
        }
        else
        {
            statusText_ = L"Drag a larger box for the text.";
        }
        InvalidateRect(hwnd_, &imageCanvasRect_, FALSE);
    }
    else if (completedAction == PointerAction::ImageDraw)
    {
        const bool erased = currentStroke_.eraser;
        if (erased)
        {
            if (imageEraseChanged_) imageSession_.CommitEdit();
            else imageSession_.CancelEdit();
        }
        else
        {
            imageSession_.AddStroke(std::move(currentStroke_));
        }
        currentStroke_ = {};
        imageEraseChanged_ = false;
        InvalidateRect(hwnd_, &undoRect_, FALSE);
        InvalidateRect(hwnd_, &redoRect_, FALSE);
        if (erased) InvalidateRect(hwnd_, &imageCanvasRect_, FALSE);
        drawingStartTime_ = lastDrawingSampleTime_ = 0;
    }

    if (completedAction != PointerAction::None)
    {
        pressedRect_ = {};
        if (GetCapture() == hwnd_) ReleaseCapture();
        return;
    }

    RECT clicked = pressedRect_;
    pressedRect_ = {};
    if (GetCapture() == hwnd_) ReleaseCapture();
    if (!HasArea(clicked) || !Contains(clicked, point))
    {
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    auto same = [&](const RECT& rect)
    {
        return EqualRect(&clicked, &rect) != FALSE;
    };

    if (keybindsOpen_)
    {
        if (same(keybindsCloseRect_))
        {
            keybindsOpen_ = false;
            UpdateChildWindows();
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    if (exportOpen_)
    {
        if (exportBusy_ && same(exportCancelRect_)) CancelVideoExport();
        else if (exportComplete_)
        {
            if (same(exportOpenFileRect_)) OpenExportFile();
            else if (same(exportOpenFolderRect_)) OpenExportFolder();
            else if (same(exportDoneRect_)) CancelVideoExport();
        }
        else
        {
            for (size_t index = 0; index < exportModeRects_.size(); ++index)
            {
                if (same(exportModeRects_[index]))
                {
                    exportMode_ = static_cast<VideoEditorExportMode>(index);
                    Layout();
                    UpdateChildWindows();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return;
                }
            }
            if (same(exportSizeUnitRect_)) exportSizeUnitGb_ = !exportSizeUnitGb_;
            else if (same(exportGpuRowRect_) || same(exportGpuToggleRect_))
                SetExportGpuEnabled(!exportGpuEnabled_);
            else if (same(exportCancelRect_)) CancelVideoExport();
            else if (same(exportStartRect_))
            {
                const auto output = ChooseVideoOutputPath();
                if (output) StartVideoExport(*output);
            }
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    if (contextPasteOpen_ && same(contextPasteRect_))
    {
        contextPasteOpen_ = false;
        PasteFromClipboard();
        return;
    }
    contextPasteOpen_ = false;

    if (view_ == EditorView::Import)
    {
        if (same(pasteRect_)) PasteFromClipboard();
        else if (same(chooseFileRect_) || same(importDropRect_)) ChooseFiles(false);
        return;
    }

    if (view_ == EditorView::Video)
    {
        if (same(undoRect_)) { if (timeline_.Undo()) { Layout(); LoadSelectedVideoPreview(); } }
        else if (same(redoRect_)) { if (timeline_.Redo()) { Layout(); LoadSelectedVideoPreview(); } }
        else if (same(newEditRect_)) ResetToImport();
        else if (same(keybindsRect_) && !exportBusy_)
        {
            timelinePlaybackRequested_ = false;
            preview_.Pause();
            keybindsOpen_ = true;
            UpdateChildWindows();
        }
        else if (same(playRect_)) ToggleTimelinePlayback();
        else if (same(muteRect_)) preview_.SetMuted(!preview_.IsMuted());
        else if (same(addClipRect_)) ChooseFiles(true);
        else if (same(splitRect_))
        {
            if (timeline_.SplitSelectedAtPlayhead()) { Layout(); LoadSelectedVideoPreview(); }
        }
        else if (same(deleteRect_))
        {
            if (timeline_.DeleteSelected())
            {
                if (timeline_.Clips().empty()) ResetToImport();
                else { Layout(); LoadSelectedVideoPreview(); }
            }
        }
        else if (same(moveLeftRect_))
        {
            if (timeline_.MoveSelected(timeline_.SelectedIndex() - 1)) { Layout(); LoadSelectedVideoPreview(); }
        }
        else if (same(moveRightRect_))
        {
            if (timeline_.MoveSelected(timeline_.SelectedIndex() + 1)) { Layout(); LoadSelectedVideoPreview(); }
        }
        else if (same(exportRect_)) BeginVideoExport();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    if (same(undoRect_))
    {
        imageToolbarPopup_ = ImageToolbarPopup::None;
        if (imageSession_.Undo())
        {
            RebuildImagePreviewCache();
            if (imageFitMode_) FitImageToCanvas(); else ClampImagePan();
        }
    }
    else if (same(redoRect_))
    {
        imageToolbarPopup_ = ImageToolbarPopup::None;
        if (imageSession_.Redo())
        {
            RebuildImagePreviewCache();
            if (imageFitMode_) FitImageToCanvas(); else ClampImagePan();
        }
    }
    else if (same(newEditRect_))
    {
        imageToolbarPopup_ = ImageToolbarPopup::None;
        ResetToImport();
    }
    else if (same(drawRect_))
    {
        eraserEnabled_ = false;
        textToolEnabled_ = false;
        imageToolbarPopup_ = ImageToolbarPopup::None;
        statusText_.clear();
    }
    else if (same(eraserRect_))
    {
        eraserEnabled_ = true;
        textToolEnabled_ = false;
        imageToolbarPopup_ = ImageToolbarPopup::None;
        statusText_.clear();
    }
    else if (same(textToolRect_))
    {
        textToolEnabled_ = true;
        eraserEnabled_ = false;
        imageToolbarPopup_ = ImageToolbarPopup::None;
        statusText_.clear();
    }
    else if (same(colorRect_))
    {
        imageToolbarPopup_ =
            imageToolbarPopup_ == ImageToolbarPopup::Color
                ? ImageToolbarPopup::None
                : ImageToolbarPopup::Color;
    }
    else if (same(thicknessRect_))
    {
        imageToolbarPopup_ =
            imageToolbarPopup_ == ImageToolbarPopup::Thickness
                ? ImageToolbarPopup::None
                : ImageToolbarPopup::Thickness;
    }
    else if (same(colorCustomRect_))
    {
        static COLORREF customColors[16] {};
        CHOOSECOLORW picker {};
        picker.lStructSize = sizeof(picker);
        picker.hwndOwner = hwnd_;
        picker.rgbResult = drawingColor_;
        picker.lpCustColors = customColors;
        picker.Flags = CC_FULLOPEN | CC_RGBINIT;
        if (ChooseColorW(&picker)) drawingColor_ = picker.rgbResult;
        imageToolbarPopup_ = ImageToolbarPopup::None;
    }
    else if (same(rotateRect_))
    {
        imageToolbarPopup_ = ImageToolbarPopup::None;
        if (imageSession_.RotateClockwise())
        {
            RebuildImagePreviewCache();
            imageFitMode_ = true;
            FitImageToCanvas();
        }
        else
        {
            statusText_ = L"There is not enough memory to rotate this image.";
        }
    }
    else if (same(resetCropRect_))
    {
        imageToolbarPopup_ = ImageToolbarPopup::None;
        imageSession_.ResetCrop();
        resetCropRect_ = {};
    }
    else if (same(fitRect_))
    {
        imageToolbarPopup_ = ImageToolbarPopup::None;
        imageFitMode_ = true;
        FitImageToCanvas();
    }
    else if (same(actualSizeRect_))
    {
        imageToolbarPopup_ =
            imageToolbarPopup_ == ImageToolbarPopup::Zoom
                ? ImageToolbarPopup::None
                : ImageToolbarPopup::Zoom;
    }
    else if (same(copyRect_))
    {
        imageToolbarPopup_ = ImageToolbarPopup::None;
        CopyImage();
    }
    else if (same(saveRect_))
    {
        imageToolbarPopup_ = ImageToolbarPopup::None;
        SaveImage(false);
    }
    else if (same(saveMenuRect_))
    {
        imageToolbarPopup_ =
            imageToolbarPopup_ == ImageToolbarPopup::Save
                ? ImageToolbarPopup::None
                : ImageToolbarPopup::Save;
    }
    else if (same(saveAsRect_))
    {
        imageToolbarPopup_ = ImageToolbarPopup::None;
        SaveImage(true);
    }
    else
    {
        const std::array<COLORREF, 5> swatches {
            RGB(255, 255, 255), RGB(255, 76, 96), RGB(255, 205, 64),
            RGB(82, 157, 255), RGB(18, 20, 24)
        };
        const std::array<float, 3> thicknesses { 2.0f, 6.0f, 14.0f };
        const std::array<int, 6> zoomValues { 25, 50, 75, 100, 150, 200 };
        for (size_t index = 0; index < colorSwatchRects_.size(); ++index)
        {
            if (same(colorSwatchRects_[index]))
            {
                drawingColor_ = swatches[index];
                imageToolbarPopup_ = ImageToolbarPopup::None;
                break;
            }
        }
        for (size_t index = 0; index < thicknessRects_.size(); ++index)
        {
            if (same(thicknessRects_[index]))
            {
                drawingThickness_ = thicknesses[index];
                imageToolbarPopup_ = ImageToolbarPopup::None;
                break;
            }
        }
        for (size_t index = 0; index < zoomOptionRects_.size(); ++index)
        {
            if (same(zoomOptionRects_[index]))
            {
                imageFitMode_ = false;
                imageScale_ = static_cast<float>(zoomValues[index]) / 100.0f;
                imagePan_ = { 0.0f, 0.0f };
                ClampImagePan();
                imageToolbarPopup_ = ImageToolbarPopup::None;
                break;
            }
        }
    }
    Layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MediaEditorPage::Impl::AppendDrawingPoint(MediaEditorPoint point)
{
    if (currentStroke_.points.empty())
    {
        currentStroke_.points.push_back(point);
        return;
    }
    if (currentStroke_.points.size() >= 250000)
    {
        return;
    }

    const MediaEditorPoint start = currentStroke_.points.back();
    const float dx = point.x - start.x;
    const float dy = point.y - start.y;
    const float distance = std::sqrt(dx * dx + dy * dy);
    if (distance < 0.05f)
    {
        return;
    }

    const float maximumStep = std::max(0.35f, 2.0f / std::max(0.01f, imageScale_));
    const int steps = std::clamp(static_cast<int>(std::ceil(distance / maximumStep)), 1, 96);
    for (int step = 1; step <= steps && currentStroke_.points.size() < 250000; ++step)
    {
        const float amount = static_cast<float>(step) / static_cast<float>(steps);
        currentStroke_.points.push_back({ start.x + dx * amount, start.y + dy * amount });
    }

    const POINT canvasStart = ImageToCanvas(start);
    const POINT canvasEnd = ImageToCanvas(point);
    const int radius = static_cast<int>(std::ceil(
        std::max(1.0f, currentStroke_.thickness * imageScale_ * 0.5f))) + Dips(3);
    RECT dirty {
        std::min(canvasStart.x, canvasEnd.x) - radius,
        std::min(canvasStart.y, canvasEnd.y) - radius,
        std::max(canvasStart.x, canvasEnd.x) + radius + 1,
        std::max(canvasStart.y, canvasEnd.y) + radius + 1
    };
    RECT clipped {};
    if (IntersectRect(&clipped, &dirty, &imageCanvasRect_))
    {
        InvalidateRect(hwnd_, &clipped, FALSE);
    }
}

void MediaEditorPage::Impl::AppendDrawingSamples(POINT point)
{
    const size_t previousPointCount = currentStroke_.points.size();
    POINT screenPoint = point;
    ClientToScreen(hwnd_, &screenPoint);
    MOUSEMOVEPOINT input {
        screenPoint.x & 0x0000FFFF,
        screenPoint.y & 0x0000FFFF,
        static_cast<DWORD>(GetMessageTime()),
        static_cast<ULONG_PTR>(GetMessageExtraInfo())
    };
    std::array<MOUSEMOVEPOINT, 64> history {};
    const int count = GetMouseMovePointsEx(
        sizeof(MOUSEMOVEPOINT),
        &input,
        history.data(),
        static_cast<int>(history.size()),
        GMMP_USE_DISPLAY_POINTS);

    if (count > 0)
    {
        const auto decodeDisplayCoordinate = [](int value)
        {
            return value > 32767 ? value - 65536 : value;
        };

        for (int index = 0; index < count; ++index)
        {
            history[static_cast<size_t>(index)].x =
                decodeDisplayCoordinate(history[static_cast<size_t>(index)].x);
            history[static_cast<size_t>(index)].y =
                decodeDisplayCoordinate(history[static_cast<size_t>(index)].y);
        }

        // GetMouseMovePointsEx returns newest points first. Find the last point
        // already consumed so older history is never inserted back into the stroke.
        int consumedPointIndex = -1;
        for (int index = 0; index < count; ++index)
        {
            const MOUSEMOVEPOINT& sample = history[static_cast<size_t>(index)];
            if (sample.time == lastDrawingSampleTime_ &&
                sample.x == lastDrawingScreenPoint_.x &&
                sample.y == lastDrawingScreenPoint_.y)
            {
                consumedPointIndex = index;
                break;
            }
        }

        const int candidateCount = consumedPointIndex >= 0 ? consumedPointIndex : count;
        for (int index = candidateCount - 1; index >= 0; --index)
        {
            const MOUSEMOVEPOINT& sample = history[static_cast<size_t>(index)];
            if (static_cast<LONG>(sample.time - drawingStartTime_) < 0)
            {
                continue;
            }
            if (consumedPointIndex < 0 &&
                static_cast<LONG>(sample.time - lastDrawingSampleTime_) <= 0)
            {
                continue;
            }
            POINT samplePoint { sample.x, sample.y };
            ScreenToClient(hwnd_, &samplePoint);
            if (const auto imagePoint = CanvasToImage(samplePoint))
            {
                AppendDrawingPoint(*imagePoint);
            }
            lastDrawingSampleTime_ = sample.time;
            lastDrawingScreenPoint_ = { sample.x, sample.y };
        }
    }

    if (const auto imagePoint = CanvasToImage(point))
    {
        AppendDrawingPoint(*imagePoint);
    }
    lastDrawingSampleTime_ = input.time;
    lastDrawingScreenPoint_ = screenPoint;

    if (currentStroke_.eraser && currentStroke_.points.size() > previousPointCount)
    {
        MediaEditorDrawingStroke eraseSegment;
        eraseSegment.color = currentStroke_.color;
        eraseSegment.thickness = currentStroke_.thickness;
        eraseSegment.opacity = currentStroke_.opacity;
        eraseSegment.eraser = true;
        const size_t firstPoint = previousPointCount > 0 ? previousPointCount - 1 : 0;
        eraseSegment.points.assign(
            currentStroke_.points.begin() + static_cast<std::ptrdiff_t>(firstPoint),
            currentStroke_.points.end());
        imageEraseChanged_ = imageSession_.AddStroke(std::move(eraseSegment)) ||
            imageEraseChanged_;
    }
}

void MediaEditorPage::Impl::OnMouseWheel(POINT point, int delta, WPARAM keys)
{
    if (view_ == EditorView::Video && !exportOpen_ && !keybindsOpen_ &&
        Contains(timelinePanelRect_, point) && timeline_.DurationSeconds() > 0.0 && delta != 0)
    {
        const double steps = static_cast<double>(delta) / static_cast<double>(WHEEL_DELTA);
        if ((keys & MK_SHIFT) != 0 && timelineZoom_ > 1.001)
        {
            timelineViewStartSeconds_ -= steps * TimelineVisibleDuration() * 0.12;
            ClampTimelineView();
        }
        else
        {
            SetTimelineZoom(timelineZoom_ * std::pow(1.22, steps), point.x);
        }
        Layout();
        InvalidateRect(hwnd_, &timelinePanelRect_, FALSE);
        return;
    }

    if (view_ != EditorView::Image || exportOpen_ || !imageSession_.IsLoaded() ||
        !Contains(imageCanvasRect_, point) || delta == 0)
    {
        return;
    }

    const float oldScale = imageScale_;
    const float anchorX = (point.x - imageDisplayRect_.left) / std::max(0.01f, oldScale);
    const float anchorY = (point.y - imageDisplayRect_.top) / std::max(0.01f, oldScale);
    const float steps = static_cast<float>(delta) / static_cast<float>(WHEEL_DELTA);
    const float zoomed = oldScale * static_cast<float>(std::pow(1.12f, steps));
    imageScale_ = std::clamp(zoomed, 0.02f, 16.0f);
    imageFitMode_ = false;

    const float displayLeft = point.x - anchorX * imageScale_;
    const float displayTop = point.y - anchorY * imageScale_;
    const float centerX = displayLeft + imageSession_.Width() * imageScale_ * 0.5f;
    const float centerY = displayTop + imageSession_.Height() * imageScale_ * 0.5f;
    imagePan_.X = centerX - (imageCanvasRect_.left + imageCanvasRect_.right) * 0.5f;
    imagePan_.Y = centerY - (imageCanvasRect_.top + imageCanvasRect_.bottom) * 0.5f;
    ClampImagePan();
    if (textEditing_) UpdateTextEditBounds();
    InvalidateRect(hwnd_, &imageCanvasRect_, FALSE);
    InvalidateRect(hwnd_, &actualSizeRect_, FALSE);
}

void MediaEditorPage::Impl::OnKeyDown(WPARAM key, LPARAM flags)
{
    const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    const bool repeated = (flags & (static_cast<LPARAM>(1) << 30)) != 0;

    if (key == VK_ESCAPE)
    {
        if (view_ == EditorView::Image &&
            imageToolbarPopup_ != ImageToolbarPopup::None)
        {
            imageToolbarPopup_ = ImageToolbarPopup::None;
            Layout();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        contextPasteOpen_ = false;
        if (keybindsOpen_)
        {
            keybindsOpen_ = false;
            UpdateChildWindows();
        }
        else if (exportOpen_) CancelVideoExport();
        else if (pointerAction_ == PointerAction::ImageCrop) imageSession_.CancelEdit();
        else if (pointerAction_ == PointerAction::TimelineTrimLeft || pointerAction_ == PointerAction::TimelineTrimRight) timeline_.CancelEdit();
        else if (pointerAction_ == PointerAction::TimelineReorder)
        {
            timeline_.CancelEdit();
            dragClipIndex_ = -1;
            dragMoved_ = false;
            dragInsertTargetId_ = 0;
            dragInsertAfter_ = false;
            Layout();
        }
        if (pointerAction_ == PointerAction::ImageDraw && currentStroke_.eraser)
        {
            imageSession_.CancelEdit();
        }
        currentStroke_ = {};
        pendingTextBounds_ = {};
        imageEraseChanged_ = false;
        pointerAction_ = PointerAction::None;
        if (GetCapture() == hwnd_) ReleaseCapture();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (keybindsOpen_ || exportOpen_)
    {
        return;
    }
    if (control && key == 'V')
    {
        PasteFromClipboard();
        return;
    }
    if (control && key == 'O')
    {
        ChooseFiles(view_ == EditorView::Video);
        return;
    }
    if (control && key == 'Z' && !shift)
    {
        if (view_ == EditorView::Video)
        {
            if (timeline_.Undo()) { Layout(); LoadSelectedVideoPreview(); }
        }
        else if (view_ == EditorView::Image && imageSession_.Undo())
        {
            RebuildImagePreviewCache();
            if (imageFitMode_) FitImageToCanvas(); else ClampImagePan();
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (control && (key == 'Y' || (shift && key == 'Z')))
    {
        if (view_ == EditorView::Video)
        {
            if (timeline_.Redo()) { Layout(); LoadSelectedVideoPreview(); }
        }
        else if (view_ == EditorView::Image && imageSession_.Redo())
        {
            RebuildImagePreviewCache();
            if (imageFitMode_) FitImageToCanvas(); else ClampImagePan();
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (view_ == EditorView::Image && control && key == 'S')
    {
        SaveImage(shift);
        return;
    }
    if (view_ == EditorView::Image &&
        !control &&
        !alt &&
        !repeated &&
        (key == 'D' || key == 'E' || key == 'T'))
    {
        if (key == 'D')
        {
            eraserEnabled_ = false;
            textToolEnabled_ = false;
        }
        else if (key == 'E')
        {
            eraserEnabled_ = true;
            textToolEnabled_ = false;
        }
        else
        {
            eraserEnabled_ = false;
            textToolEnabled_ = true;
        }
        imageToolbarPopup_ = ImageToolbarPopup::None;
        statusText_.clear();
        Layout();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (key == VK_SPACE)
    {
        if (!spaceHeld_ && view_ == EditorView::Video)
        {
            ToggleTimelinePlayback();
            InvalidateRect(hwnd_, &playRect_, FALSE);
        }
        spaceHeld_ = true;
        return;
    }
    if (view_ != EditorView::Video)
    {
        return;
    }
    if (!control && !alt &&
        (key == VK_OEM_COMMA || key == VK_LEFT ||
         key == VK_OEM_PERIOD || key == VK_RIGHT))
    {
        const int direction = key == VK_OEM_COMMA || key == VK_LEFT ? -1 : 1;
        StepTimelineFrame(direction);
        return;
    }
    if (!repeated && key == VK_DELETE)
    {
        if (timeline_.DeleteSelected())
        {
            if (timeline_.Clips().empty()) ResetToImport();
            else { Layout(); LoadSelectedVideoPreview(); }
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (!control && !alt && !repeated && key == 'S')
    {
        if (timeline_.SplitSelectedAtPlayhead())
        {
            Layout();
            LoadSelectedVideoPreview();
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (!control && !alt && !repeated && key == 'A')
    {
        ChooseFiles(true);
        return;
    }
    if (!control && !alt && !repeated && key == 'M')
    {
        preview_.SetMuted(!preview_.IsMuted());
        InvalidateRect(hwnd_, &muteRect_, FALSE);
        RECT audioControls = muteRect_;
        UnionRect(&audioControls, &audioControls, &volumeRect_);
        InvalidateRect(hwnd_, &audioControls, FALSE);
        return;
    }
    if (!control && !alt && !repeated && key == 'E')
    {
        BeginVideoExport();
        return;
    }
}

void MediaEditorPage::Impl::OnDropFiles(HDROP drop)
{
    std::vector<std::filesystem::path> paths;
    const UINT count = std::min<UINT>(DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0), 256);
    for (UINT index = 0; index < count; ++index)
    {
        const UINT length = DragQueryFileW(drop, index, nullptr, 0);
        std::wstring path(static_cast<size_t>(length) + 1, L'\0');
        DragQueryFileW(drop, index, path.data(), length + 1);
        path.resize(length);
        paths.emplace_back(std::move(path));
    }
    DragFinish(drop);
    OpenFiles(paths);
}
