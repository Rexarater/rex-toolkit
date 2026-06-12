#include "AppConstants.h"
#include "ToolkitApp.h"
#include "resource.h"

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cwchar>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <utility>

namespace
{
constexpr wchar_t kWindowClassName[] = L"RexToolkitWindowClass";
constexpr wchar_t kWindowTitle[] = L"Rex's Toolkit";

constexpr COLORREF kAppBackground = RGB(18, 20, 24);
constexpr COLORREF kSidebarBackground = RGB(24, 27, 33);
constexpr COLORREF kPanelBackground = RGB(31, 35, 43);
constexpr COLORREF kPanelHover = RGB(39, 44, 53);
constexpr COLORREF kAccent = RGB(83, 147, 245);
constexpr COLORREF kAccentSoft = RGB(44, 86, 153);
constexpr COLORREF kGold = RGB(245, 191, 79);
constexpr COLORREF kTextPrimary = RGB(245, 247, 250);
constexpr COLORREF kTextSecondary = RGB(166, 174, 186);
constexpr COLORREF kBorder = RGB(48, 54, 65);

constexpr int kMinWindowWidth = 920;
constexpr int kMinWindowHeight = 560;
constexpr UINT kConversionProgressMessage = WM_APP + 101;
constexpr UINT kConversionFinishedMessage = WM_APP + 102;
constexpr UINT kMediaJobUpdateMessage = WM_APP + 103;
constexpr UINT kMediaFinishedMessage = WM_APP + 104;
constexpr UINT kUpdateCheckFinishedMessage = WM_APP + 105;
constexpr int kMinClicksPerSecond = 1;
constexpr int kMaxClicksPerSecond = 100;
constexpr UINT_PTR kClockTimerId = 1002;
constexpr ULONG_PTR kAutoClickExtraInfo = 0x5254584B;

ToolkitApp* g_activeApp = nullptr;

std::vector<ToolDefinition> CreateToolRegistry()
{
    // Register future tools here.
    //
    // Example:
    // return {
    //     { L"json_formatter", L"JSON Formatter", L"Format and validate JSON.", false, ToolKind::JsonFormatter },
    //     { L"hash_generator", L"Hash Generator", L"Create file and text hashes.", true, ToolKind::HashGenerator },
    // };
    //
    // Keep this registry as the source of truth for navigation, favorites,
    // search, and tool launch metadata as Rex's Toolkit grows.
    return {
        {
            L"auto_clicker",
            L"Auto Clicker",
            L"Automate repeated mouse clicks with a bindable activation key.",
            false,
            ToolKind::AutoClicker
        },
        {
            L"file_converter",
            L"File Converter",
            L"Convert images between common formats such as WEBP, PNG, JPG, and BMP.",
            false,
            ToolKind::FileConverter
        },
        {
            L"media_downloader",
            L"YouTube & SoundCloud Downloader",
            L"Download authorized videos or audio as MP4, MP3, or WAV.",
            false,
            ToolKind::MediaDownloader
        }
    };
}

HFONT CreateUiFont(int dpi, int pointSize, int weight, const wchar_t* faceName = L"Segoe UI")
{
    const int height = -MulDiv(pointSize, dpi, 72);

    LOGFONTW logFont {};
    logFont.lfHeight = height;
    logFont.lfWeight = weight;
    logFont.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(logFont.lfFaceName, faceName);

    return CreateFontIndirectW(&logFont);
}

bool IsPointInRect(const RECT& rect, POINT point)
{
    return point.x >= rect.left && point.x < rect.right &&
           point.y >= rect.top && point.y < rect.bottom;
}

bool HasArea(const RECT& rect)
{
    return rect.right > rect.left && rect.bottom > rect.top;
}

RECT PaddedRect(RECT rect, int padding)
{
    InflateRect(&rect, padding, padding);
    return rect;
}

bool RectsOverlap(const RECT& first, const RECT& second)
{
    return first.left < second.right && first.right > second.left &&
           first.top < second.bottom && first.bottom > second.top;
}

void FillSolidRect(HDC hdc, const RECT& rect, COLORREF color)
{
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(hdc, &rect, brush);
    DeleteObject(brush);
}

void FillRoundRect(HDC hdc, const RECT& rect, int radius, COLORREF color)
{
    HBRUSH brush = CreateSolidBrush(color);
    HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(hdc, brush));
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, pen));

    RoundRect(hdc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void StrokeRoundRect(HDC hdc, const RECT& rect, int radius, COLORREF color)
{
    HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(hdc, GetStockObject(NULL_BRUSH)));
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, pen));

    RoundRect(hdc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
}

RECT ShrinkRect(RECT rect, int dx, int dy)
{
    rect.left += dx;
    rect.right -= dx;
    rect.top += dy;
    rect.bottom -= dy;
    return rect;
}

void DrawTextLine(HDC hdc, const wchar_t* text, RECT rect, HFONT font, COLORREF color, UINT format)
{
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, font));
    SetTextColor(hdc, color);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextW(hdc, text, -1, &rect, format | DT_NOPREFIX);
    SelectObject(hdc, oldFont);
}

std::wstring GetWindowTextString(HWND hwnd)
{
    if (!hwnd)
    {
        return {};
    }

    const int length = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    if (length > 0)
    {
        GetWindowTextW(hwnd, text.data(), length + 1);
    }
    text.resize(static_cast<size_t>(length));
    return text;
}

void SetWindowTextIfChanged(HWND hwnd, const std::wstring& text)
{
    if (hwnd && GetWindowTextString(hwnd) != text)
    {
        SetWindowTextW(hwnd, text.c_str());
    }
}

std::wstring KeyLabel(UINT virtualKey)
{
    switch (virtualKey)
    {
    case VK_SPACE:
        return L"Space";
    case VK_ESCAPE:
        return L"Esc";
    case VK_RETURN:
        return L"Enter";
    case VK_TAB:
        return L"Tab";
    case VK_BACK:
        return L"Backspace";
    case VK_SHIFT:
        return L"Shift";
    case VK_CONTROL:
        return L"Ctrl";
    case VK_MENU:
        return L"Alt";
    case VK_LEFT:
        return L"Left Arrow";
    case VK_RIGHT:
        return L"Right Arrow";
    case VK_UP:
        return L"Up Arrow";
    case VK_DOWN:
        return L"Down Arrow";
    default:
        break;
    }

    UINT scanCode = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC);
    if (scanCode == 0)
    {
        return L"Unassigned";
    }

    if (virtualKey == VK_LEFT || virtualKey == VK_UP || virtualKey == VK_RIGHT ||
        virtualKey == VK_DOWN || virtualKey == VK_INSERT || virtualKey == VK_DELETE ||
        virtualKey == VK_HOME || virtualKey == VK_END || virtualKey == VK_PRIOR ||
        virtualKey == VK_NEXT)
    {
        scanCode |= 0x100;
    }

    wchar_t keyName[64] {};
    if (GetKeyNameTextW(static_cast<LONG>(scanCode << 16), keyName, static_cast<int>(std::size(keyName))) > 0)
    {
        return keyName;
    }

    return L"Key " + std::to_wstring(virtualKey);
}

std::wstring MouseButtonLabel(OutputMouseButton button)
{
    switch (button)
    {
    case OutputMouseButton::Left:
        return L"Left Mouse";
    case OutputMouseButton::Right:
        return L"Right Mouse";
    case OutputMouseButton::Middle:
        return L"Middle Mouse";
    }

    return L"Left Mouse";
}

std::wstring FileSizeLabel(unsigned long long bytes)
{
    const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB" };
    double value = static_cast<double>(bytes);
    int unitIndex = 0;
    while (value >= 1024.0 && unitIndex < 3)
    {
        value /= 1024.0;
        ++unitIndex;
    }

    std::wostringstream output;
    if (unitIndex == 0)
    {
        output << static_cast<unsigned long long>(value) << L" " << units[unitIndex];
    }
    else
    {
        output.setf(std::ios::fixed);
        output.precision(1);
        output << value << L" " << units[unitIndex];
    }
    return output.str();
}

std::wstring ActivationMouseButtonLabel(ActivationMouseButton button)
{
    switch (button)
    {
    case ActivationMouseButton::Left:
        return L"Left Mouse";
    case ActivationMouseButton::Right:
        return L"Right Mouse";
    case ActivationMouseButton::Middle:
        return L"Middle Mouse";
    case ActivationMouseButton::X1:
        return L"Side Mouse 1";
    case ActivationMouseButton::X2:
        return L"Side Mouse 2";
    }

    return L"Side Mouse 1";
}

DWORD MouseDownFlag(OutputMouseButton button)
{
    switch (button)
    {
    case OutputMouseButton::Left:
        return MOUSEEVENTF_LEFTDOWN;
    case OutputMouseButton::Right:
        return MOUSEEVENTF_RIGHTDOWN;
    case OutputMouseButton::Middle:
        return MOUSEEVENTF_MIDDLEDOWN;
    }

    return MOUSEEVENTF_LEFTDOWN;
}

DWORD MouseUpFlag(OutputMouseButton button)
{
    switch (button)
    {
    case OutputMouseButton::Left:
        return MOUSEEVENTF_LEFTUP;
    case OutputMouseButton::Right:
        return MOUSEEVENTF_RIGHTUP;
    case OutputMouseButton::Middle:
        return MOUSEEVENTF_MIDDLEUP;
    }

    return MOUSEEVENTF_LEFTUP;
}
}

ToolkitApp::ToolkitApp(HINSTANCE instance)
    : instance_(instance), tools_(CreateToolRegistry())
{
    g_activeApp = this;
    supportedOutputFormats_ = fileConversionService_.SupportedOutputFormats();
    if (!supportedOutputFormats_.empty())
    {
        conversionOptions_.outputFormat = supportedOutputFormats_.front();
    }
    LoadFavorites();
    LoadWindowSettings();
    LoadAutoClickerSettings();
    autoClickerCps_.store(autoClicker_.clicksPerSecond);
    autoClickerOutputButton_.store(static_cast<int>(autoClicker_.outputButton));
    mediaExternalTools_ = mediaDownloadService_.CheckExternalTools();
    mediaDownloadOptions_.outputFolder = ExternalToolService::DefaultDownloadsFolder();
    LoadMediaDownloadSettings();
    mediaDownloadJob_.outputFolder = mediaDownloadOptions_.outputFolder;
    mediaStatusText_ = MediaSetupMessage().empty() ? L"Ready." : MediaSetupMessage();
}

int ToolkitApp::Run(int showCommand)
{
    if (!RegisterWindowClass() || !CreateMainWindow(showCommand))
    {
        return 1;
    }

    MSG message {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return static_cast<int>(message.wParam);
}

bool ToolkitApp::RegisterWindowClass()
{
    HICON appIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_REX_TOOLKIT_ICON));

    WNDCLASSEXW windowClass {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = ToolkitApp::WindowProc;
    windowClass.hInstance = instance_;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = appIcon ? appIcon : LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hIconSm = appIcon ? appIcon : LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.lpszClassName = kWindowClassName;

    return RegisterClassExW(&windowClass) != 0;
}

bool ToolkitApp::CreateMainWindow(int showCommand)
{
    hwnd_ = CreateWindowExW(
        0,
        kWindowClassName,
        kWindowTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        std::max(Dips(kMinWindowWidth), static_cast<int>(savedWindowSize_.cx)),
        std::max(Dips(kMinWindowHeight), static_cast<int>(savedWindowSize_.cy)),
        nullptr,
        nullptr,
        instance_,
        this);

    if (!hwnd_)
    {
        return false;
    }

    ShowWindow(hwnd_, savedWindowMaximized_ ? SW_MAXIMIZE : showCommand);
    SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(LoadIconW(instance_, MAKEINTRESOURCEW(IDI_REX_TOOLKIT_ICON))));
    SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(LoadIconW(instance_, MAKEINTRESOURCEW(IDI_REX_TOOLKIT_ICON))));
    UpdateWindow(hwnd_);
    return true;
}

LRESULT CALLBACK ToolkitApp::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    ToolkitApp* app = nullptr;

    if (message == WM_NCCREATE)
    {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = static_cast<ToolkitApp*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        app->hwnd_ = hwnd;
    }
    else
    {
        app = reinterpret_cast<ToolkitApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (app)
    {
        return app->HandleMessage(message, wParam, lParam);
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK ToolkitApp::KeyboardHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code >= 0 && g_activeApp)
    {
        const auto* keyboard = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        if (keyboard)
        {
            if (g_activeApp->HandleKeyboardHook(wParam, *keyboard))
            {
                return 1;
            }
        }
    }

    return CallNextHookEx(nullptr, code, wParam, lParam);
}

LRESULT CALLBACK ToolkitApp::MouseHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code >= 0 && g_activeApp)
    {
        const auto* mouse = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        if (mouse)
        {
            if (g_activeApp->HandleMouseHook(wParam, *mouse))
            {
                return 1;
            }
        }
    }

    return CallNextHookEx(nullptr, code, wParam, lParam);
}

LRESULT ToolkitApp::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
    {
        dpi_ = GetDpiForWindow(hwnd_);
        titleFont_ = CreateUiFont(dpi_, 17, FW_SEMIBOLD);
        navFont_ = CreateUiFont(dpi_, 12, FW_SEMIBOLD);
        headingFont_ = CreateUiFont(dpi_, 22, FW_SEMIBOLD, L"Bahnschrift SemiBold");
        bodyFont_ = CreateUiFont(dpi_, 11, FW_NORMAL);
        monospaceFont_ = CreateUiFont(dpi_, 10, FW_NORMAL, L"Cascadia Mono");
        if (mediaUrlEdit_)
        {
            SendMessageW(mediaUrlEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(monospaceFont_), TRUE);
        }
        if (mediaFileNameEdit_)
        {
            SendMessageW(mediaFileNameEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(monospaceFont_), TRUE);
        }
        editBackgroundBrush_ = CreateSolidBrush(RGB(25, 29, 36));
        Gdiplus::GdiplusStartupInput gdiplusStartupInput {};
        Gdiplus::GdiplusStartup(&gdiplusToken_, &gdiplusStartupInput, nullptr);
        LoadLogoResource();
        LoadToolIconResources();
        CreateMediaDownloaderControls();
        ApplyDarkTitleBar();
        DragAcceptFiles(hwnd_, TRUE);
        RecalculateLayout();
        SetTimer(hwnd_, kClockTimerId, 1000, nullptr);
        InstallInputHooks();
        return 0;
    }

    case WM_DPICHANGED:
    {
        dpi_ = HIWORD(wParam);

        DeleteObject(titleFont_);
        DeleteObject(navFont_);
        DeleteObject(headingFont_);
        DeleteObject(bodyFont_);
        DeleteObject(monospaceFont_);

        titleFont_ = CreateUiFont(dpi_, 17, FW_SEMIBOLD);
        navFont_ = CreateUiFont(dpi_, 12, FW_SEMIBOLD);
        headingFont_ = CreateUiFont(dpi_, 22, FW_SEMIBOLD, L"Bahnschrift SemiBold");
        bodyFont_ = CreateUiFont(dpi_, 11, FW_NORMAL);
        monospaceFont_ = CreateUiFont(dpi_, 10, FW_NORMAL, L"Cascadia Mono");

        for (HWND edit : { mediaUrlEdit_, mediaFileNameEdit_ })
        {
            if (!edit)
            {
                continue;
            }
            SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(monospaceFont_), TRUE);
            SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(Dips(4), Dips(4)));
        }

        const RECT* suggested = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(
            hwnd_,
            nullptr,
            suggested->left,
            suggested->top,
            suggested->right - suggested->left,
            suggested->bottom - suggested->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        RecalculateLayout();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }

    case WM_SIZE:
        RecalculateLayout();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;

    case WM_CTLCOLOREDIT:
    {
        HDC editDc = reinterpret_cast<HDC>(wParam);
        SetTextColor(editDc, kTextPrimary);
        SetBkColor(editDc, RGB(25, 29, 36));
        return reinterpret_cast<LRESULT>(editBackgroundBrush_);
    }

    case WM_GETMINMAXINFO:
    {
        auto* minMaxInfo = reinterpret_cast<MINMAXINFO*>(lParam);
        minMaxInfo->ptMinTrackSize.x = Dips(kMinWindowWidth);
        minMaxInfo->ptMinTrackSize.y = Dips(kMinWindowHeight);
        return 0;
    }

    case WM_MOUSEMOVE:
        OnMouseMove({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
        return 0;

    case WM_MOUSEWHEEL:
        OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam), { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
        return 0;

    case WM_MOUSELEAVE:
        mouseLeaveTracking_ = false;
        if (hoverNavIndex_ != -1 || hoverToolIndex_ != -1 || hasHoveredButton_)
        {
            const RECT previousButtonHover = hoveredButtonRect_;
            const bool hadPreviousButtonHover = hasHoveredButton_;
            const int previousNavHover = hoverNavIndex_;
            const int previousToolHover = hoverToolIndex_;
            hoverNavIndex_ = -1;
            hoverToolIndex_ = -1;
            hasHoveredButton_ = false;
            hoveredButtonRect_ = {};
            if (previousNavHover == 0)
            {
                RECT repaint = PaddedRect(favoritesNavRect_, Dips(3));
                InvalidateRect(hwnd_, &repaint, FALSE);
            }
            else if (previousNavHover == 1)
            {
                RECT repaint = PaddedRect(allToolsNavRect_, Dips(3));
                InvalidateRect(hwnd_, &repaint, FALSE);
            }
            else if (previousNavHover == 2)
            {
                RECT repaint = PaddedRect(settingsNavRect_, Dips(3));
                InvalidateRect(hwnd_, &repaint, FALSE);
            }
            if (hadPreviousButtonHover)
            {
                RECT repaint = PaddedRect(previousButtonHover, Dips(3));
                InvalidateRect(hwnd_, &repaint, FALSE);
            }
            if (previousToolHover >= 0)
            {
                RECT repaint = PaddedRect(ToolCardRect(static_cast<size_t>(previousToolHover)), Dips(4));
                InvalidateRect(hwnd_, &repaint, FALSE);
            }
        }
        return 0;

    case WM_LBUTTONDOWN:
        if (static_cast<ULONG_PTR>(GetMessageExtraInfo()) == kAutoClickExtraInfo)
        {
            return 0;
        }
        OnLeftButtonDown({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
        return 0;

    case WM_LBUTTONUP:
        if (static_cast<ULONG_PTR>(GetMessageExtraInfo()) == kAutoClickExtraInfo)
        {
            return 0;
        }
        OnLeftButtonUp({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
        return 0;

    case WM_RBUTTONDOWN:
        if (static_cast<ULONG_PTR>(GetMessageExtraInfo()) == kAutoClickExtraInfo)
        {
            return 0;
        }
        if (awaitingActivationKey_)
        {
            OnMouseButtonForActivationBinding(ActivationMouseButton::Right);
            return 0;
        }
        OnMouseButtonForBinding(OutputMouseButton::Right);
        return 0;

    case WM_MBUTTONDOWN:
        if (static_cast<ULONG_PTR>(GetMessageExtraInfo()) == kAutoClickExtraInfo)
        {
            return 0;
        }
        if (awaitingActivationKey_)
        {
            OnMouseButtonForActivationBinding(ActivationMouseButton::Middle);
            return 0;
        }
        OnMouseButtonForBinding(OutputMouseButton::Middle);
        return 0;

    case WM_XBUTTONDOWN:
        if (awaitingActivationKey_)
        {
            const WORD button = HIWORD(wParam);
            OnMouseButtonForActivationBinding(button == XBUTTON2 ? ActivationMouseButton::X2 : ActivationMouseButton::X1);
            return 0;
        }
        break;

    case WM_TIMER:
        if (wParam == kClockTimerId)
        {
            InvalidateRect(hwnd_, &headerRect_, FALSE);
            return 0;
        }
        break;

    case WM_KEYDOWN:
        if (awaitingActivationKey_)
        {
            awaitingActivationKey_ = false;
            if (wParam != VK_ESCAPE)
            {
                SetActivationKey(static_cast<UINT>(wParam));
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        if (wParam == '1')
        {
            SelectPage(Page::Favorites);
            return 0;
        }
        if (wParam == '2')
        {
            SelectPage(Page::AllTools);
            return 0;
        }
        if (wParam == '3')
        {
            SelectPage(Page::Settings);
            return 0;
        }
        break;

    case WM_DROPFILES:
    {
        HDROP drop = reinterpret_cast<HDROP>(wParam);
        POINT dropPoint {};
        DragQueryPoint(drop, &dropPoint);

        std::vector<std::filesystem::path> droppedPaths;
        const UINT fileCount = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT index = 0; index < fileCount; ++index)
        {
            wchar_t path[MAX_PATH] {};
            DragQueryFileW(drop, index, path, static_cast<UINT>(std::size(path)));
            droppedPaths.emplace_back(path);
        }
        DragFinish(drop);

        if (currentPage_ == Page::Tool && currentTool_ == ToolKind::FileConverter &&
            IsPointInRect(converterDropZoneRect_, dropPoint))
        {
            AddFilesToConverterQueue(droppedPaths);
        }
        else if (currentPage_ == Page::Tool && currentTool_ == ToolKind::MediaDownloader)
        {
            bool foundUrl = false;
            for (const std::filesystem::path& path : droppedPaths)
            {
                std::wifstream file(path);
                std::wstring line;
                while (std::getline(file, line))
                {
                    std::wistringstream words(line);
                    std::wstring word;
                    while (words >> word)
                    {
                        if (SupportedPlatformRegistry::IsSupportedUrl(word))
                        {
                            SetWindowTextW(mediaUrlEdit_, word.c_str());
                            mediaStatusText_ = L"URL loaded from dropped file.";
                            foundUrl = true;
                            break;
                        }
                    }
                    if (foundUrl)
                    {
                        break;
                    }
                }
                if (foundUrl)
                {
                    break;
                }
            }

            if (!foundUrl)
            {
                mediaStatusText_ = L"Drop a text file containing a YouTube or SoundCloud link.";
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    }

    case kConversionProgressMessage:
    {
        std::unique_ptr<ConversionResult> result(reinterpret_cast<ConversionResult*>(lParam));
        if (result)
        {
            ApplyConversionResult(*result);
        }
        return 0;
    }

    case kConversionFinishedMessage:
        fileConverterConverting_ = false;
        FinishConversionThread();
        UpdateFileConverterSummary();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;

    case kMediaJobUpdateMessage:
    {
        std::unique_ptr<MediaDownloadJob> job(reinterpret_cast<MediaDownloadJob*>(lParam));
        if (job)
        {
            ApplyMediaJobUpdate(*job);
        }
        return 0;
    }

    case kMediaFinishedMessage:
        mediaAnalyzing_ = false;
        mediaDownloading_ = false;
        FinishMediaThread();
        UpdateMediaDownloaderControls();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;

    case kUpdateCheckFinishedMessage:
    {
        std::unique_ptr<UpdateCheckResult> result(reinterpret_cast<UpdateCheckResult*>(lParam));
        if (result)
        {
            ApplyUpdateCheckResult(*result);
        }
        FinishUpdateThread();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }

    case WM_PAINT:
    {
        PAINTSTRUCT paint {};
        HDC hdc = BeginPaint(hwnd_, &paint);
        Paint(hdc, paint.rcPaint);
        EndPaint(hwnd_, &paint);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_DESTROY:
        conversionCancelRequested_ = true;
        mediaCancelRequested_ = true;
        FinishConversionThread();
        FinishMediaThread();
        FinishUpdateThread();
        SaveWindowSettings();
        SaveMediaDownloadSettings();
        SaveAutoClickerSettings();
        SetAutoClickerRunning(false);
        KillTimer(hwnd_, kClockTimerId);
        RemoveInputHooks();
    logo_.reset();
    autoClickerIcon_.reset();
    fileConverterIcon_.reset();
    mediaDownloaderIcon_.reset();
    allToolsIcon_.reset();
    settingsIcon_.reset();
        ReleaseBackBuffer();
        if (gdiplusToken_ != 0)
        {
            Gdiplus::GdiplusShutdown(gdiplusToken_);
            gdiplusToken_ = 0;
        }
        DeleteObject(titleFont_);
        DeleteObject(navFont_);
        DeleteObject(headingFont_);
        DeleteObject(bodyFont_);
        DeleteObject(monospaceFont_);
        DeleteObject(editBackgroundBrush_);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

void ToolkitApp::ApplyDarkTitleBar()
{
    SetWindowTheme(hwnd_, L"DarkMode_Explorer", nullptr);

    BOOL useDarkMode = TRUE;
    constexpr DWORD darkModeAttribute = 20;
    DwmSetWindowAttribute(
        hwnd_,
        darkModeAttribute,
        &useDarkMode,
        sizeof(useDarkMode));
}

void ToolkitApp::LoadLogoResource()
{
    logo_ = LoadPngResource(IDR_REX_TOOLKIT_LOGO);
}

std::unique_ptr<Gdiplus::Bitmap> ToolkitApp::LoadPngResource(int resourceId) const
{
    HRSRC resource = FindResourceW(instance_, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!resource)
    {
        return nullptr;
    }

    const DWORD resourceSize = SizeofResource(instance_, resource);
    HGLOBAL loadedResource = LoadResource(instance_, resource);
    const void* resourceData = LockResource(loadedResource);
    if (!resourceData || resourceSize == 0)
    {
        return nullptr;
    }

    HGLOBAL streamMemory = GlobalAlloc(GMEM_MOVEABLE, resourceSize);
    if (!streamMemory)
    {
        return nullptr;
    }

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

    if (bitmap && bitmap->GetLastStatus() == Gdiplus::Ok)
    {
        return bitmap;
    }

    return nullptr;
}

void ToolkitApp::LoadToolIconResources()
{
    autoClickerIcon_ = LoadPngResource(IDR_AUTO_CLICKER_ICON);
    fileConverterIcon_ = LoadPngResource(IDR_FILE_CONVERTER_ICON);
    mediaDownloaderIcon_ = LoadPngResource(IDR_MEDIA_DOWNLOADER_ICON);
    allToolsIcon_ = LoadPngResource(IDR_ALL_TOOLS_ICON);
    settingsIcon_ = LoadPngResource(IDR_SETTINGS_ICON);
}

std::wstring ToolkitApp::SettingsDirectory() const
{
    wchar_t appDataPath[MAX_PATH] {};
    const DWORD length = GetEnvironmentVariableW(L"APPDATA", appDataPath, static_cast<DWORD>(std::size(appDataPath)));
    if (length == 0 || length >= std::size(appDataPath))
    {
        return L".";
    }

    return std::wstring(appDataPath) + L"\\RexToolkit";
}

std::wstring ToolkitApp::FavoritesFilePath() const
{
    return SettingsDirectory() + L"\\favorites.txt";
}

std::wstring ToolkitApp::WindowSettingsFilePath() const
{
    return SettingsDirectory() + L"\\window.txt";
}

std::wstring ToolkitApp::MediaDownloadSettingsFilePath() const
{
    return SettingsDirectory() + L"\\media_downloader.txt";
}

std::wstring ToolkitApp::AutoClickerSettingsFilePath() const
{
    return SettingsDirectory() + L"\\auto_clicker.txt";
}

void ToolkitApp::LoadFavorites()
{
    std::wifstream file(FavoritesFilePath());
    if (!file)
    {
        return;
    }

    std::set<std::wstring> favoriteIds;
    std::wstring id;
    while (std::getline(file, id))
    {
        if (!id.empty())
        {
            favoriteIds.insert(id);
        }
    }

    for (ToolDefinition& tool : tools_)
    {
        tool.favorite = favoriteIds.find(tool.id) != favoriteIds.end();
    }
}

void ToolkitApp::SaveFavorites() const
{
    const std::wstring settingsDirectory = SettingsDirectory();
    CreateDirectoryW(settingsDirectory.c_str(), nullptr);

    std::wofstream file(FavoritesFilePath(), std::ios::trunc);
    if (!file)
    {
        return;
    }

    for (const ToolDefinition& tool : tools_)
    {
        if (tool.favorite)
        {
            file << tool.id << L'\n';
        }
    }
}

void ToolkitApp::LoadWindowSettings()
{
    std::wifstream file(WindowSettingsFilePath());
    if (!file)
    {
        return;
    }

    int width = 0;
    int height = 0;
    int maximized = 0;
    file >> width >> height >> maximized;

    if (width >= kMinWindowWidth && height >= kMinWindowHeight)
    {
        savedWindowSize_.cx = width;
        savedWindowSize_.cy = height;
    }

    savedWindowMaximized_ = maximized != 0;
}

void ToolkitApp::SaveWindowSettings() const
{
    if (!hwnd_)
    {
        return;
    }

    WINDOWPLACEMENT placement {};
    placement.length = sizeof(placement);
    if (!GetWindowPlacement(hwnd_, &placement))
    {
        return;
    }

    const int width = std::max(
        kMinWindowWidth,
        static_cast<int>(placement.rcNormalPosition.right - placement.rcNormalPosition.left));
    const int height = std::max(
        kMinWindowHeight,
        static_cast<int>(placement.rcNormalPosition.bottom - placement.rcNormalPosition.top));

    const std::wstring settingsDirectory = SettingsDirectory();
    CreateDirectoryW(settingsDirectory.c_str(), nullptr);

    std::wofstream file(WindowSettingsFilePath(), std::ios::trunc);
    if (!file)
    {
        return;
    }

    file << width << L' ' << height << L' ' << (placement.showCmd == SW_SHOWMAXIMIZED ? 1 : 0) << L'\n';
}

void ToolkitApp::LoadMediaDownloadSettings()
{
    std::wifstream file(MediaDownloadSettingsFilePath());
    if (!file)
    {
        return;
    }

    int outputFormat = 0;
    int mp4Quality = 0;
    int mp3Bitrate = 0;
    file >> outputFormat >> mp4Quality >> mp3Bitrate;
    file.ignore(std::numeric_limits<std::streamsize>::max(), L'\n');

    std::wstring outputFolder;
    std::getline(file, outputFolder);

    if (outputFormat >= 0 && outputFormat <= 2)
    {
        mediaDownloadOptions_.outputFormat = static_cast<MediaOutputFormat>(outputFormat);
    }
    if (mp4Quality >= 0 && mp4Quality <= 3)
    {
        mediaDownloadOptions_.mp4Quality = static_cast<Mp4Quality>(mp4Quality);
    }
    if (mp3Bitrate >= 0 && mp3Bitrate <= 3)
    {
        mediaDownloadOptions_.mp3Bitrate = static_cast<Mp3Bitrate>(mp3Bitrate);
    }
    if (!outputFolder.empty())
    {
        mediaDownloadOptions_.outputFolder = outputFolder;
    }
}

void ToolkitApp::SaveMediaDownloadSettings() const
{
    const std::wstring settingsDirectory = SettingsDirectory();
    CreateDirectoryW(settingsDirectory.c_str(), nullptr);

    std::wofstream file(MediaDownloadSettingsFilePath(), std::ios::trunc);
    if (!file)
    {
        return;
    }

    file << static_cast<int>(mediaDownloadOptions_.outputFormat) << L' '
        << static_cast<int>(mediaDownloadOptions_.mp4Quality) << L' '
        << static_cast<int>(mediaDownloadOptions_.mp3Bitrate) << L'\n'
        << mediaDownloadOptions_.outputFolder.wstring() << L'\n';
}

void ToolkitApp::LoadAutoClickerSettings()
{
    std::wifstream file(AutoClickerSettingsFilePath());
    if (!file)
    {
        return;
    }

    int clicksPerSecond = autoClicker_.clicksPerSecond;
    int activationKind = static_cast<int>(autoClicker_.activationKind);
    unsigned int activationKey = autoClicker_.activationKey;
    int activationMouseButton = static_cast<int>(autoClicker_.activationMouseButton);
    int outputButton = static_cast<int>(autoClicker_.outputButton);

    file >> clicksPerSecond >> activationKind >> activationKey >> activationMouseButton >> outputButton;
    autoClicker_.clicksPerSecond = std::clamp(clicksPerSecond, kMinClicksPerSecond, kMaxClicksPerSecond);

    if (activationKind >= static_cast<int>(ActivationInputKind::Keyboard) &&
        activationKind <= static_cast<int>(ActivationInputKind::MouseButton))
    {
        autoClicker_.activationKind = static_cast<ActivationInputKind>(activationKind);
    }

    if (activationKey > 0 && activationKey <= 0xFE)
    {
        autoClicker_.activationKey = activationKey;
    }

    if (activationMouseButton >= static_cast<int>(ActivationMouseButton::Left) &&
        activationMouseButton <= static_cast<int>(ActivationMouseButton::X2))
    {
        autoClicker_.activationMouseButton = static_cast<ActivationMouseButton>(activationMouseButton);
    }

    if (outputButton >= static_cast<int>(OutputMouseButton::Left) &&
        outputButton <= static_cast<int>(OutputMouseButton::Middle))
    {
        autoClicker_.outputButton = static_cast<OutputMouseButton>(outputButton);
    }
}

void ToolkitApp::SaveAutoClickerSettings() const
{
    const std::wstring settingsDirectory = SettingsDirectory();
    CreateDirectoryW(settingsDirectory.c_str(), nullptr);

    std::wofstream file(AutoClickerSettingsFilePath(), std::ios::trunc);
    if (!file)
    {
        return;
    }

    file << autoClicker_.clicksPerSecond << L' '
        << static_cast<int>(autoClicker_.activationKind) << L' '
        << autoClicker_.activationKey << L' '
        << static_cast<int>(autoClicker_.activationMouseButton) << L' '
        << static_cast<int>(autoClicker_.outputButton) << L'\n';
}

void ToolkitApp::RecalculateLayout()
{
    if (!hwnd_)
    {
        return;
    }

    GetClientRect(hwnd_, &clientRect_);

    const int headerHeight = Dips(72);
    const int navHeight = Dips(42);
    const int navWidth = Dips(152);
    const int navGap = Dips(10);

    headerRect_ = clientRect_;
    headerRect_.bottom = std::min(headerRect_.top + headerHeight, clientRect_.bottom);

    const int navGroupWidth = (navWidth * 3) + (navGap * 2);
    const int navLeft = clientRect_.left + ((clientRect_.right - clientRect_.left) - navGroupWidth) / 2;
    const int navTop = headerRect_.top + ((headerRect_.bottom - headerRect_.top) - navHeight) / 2;
    favoritesNavRect_ = {
        navLeft,
        navTop,
        navLeft + navWidth,
        navTop + navHeight
    };

    allToolsNavRect_ = {
        favoritesNavRect_.right + navGap,
        navTop,
        favoritesNavRect_.right + navGap + navWidth,
        navTop + navHeight
    };

    settingsNavRect_ = {
        allToolsNavRect_.right + navGap,
        navTop,
        allToolsNavRect_.right + navGap + navWidth,
        navTop + navHeight
    };

    contentRect_ = {
        clientRect_.left,
        headerRect_.bottom,
        clientRect_.right,
        clientRect_.bottom
    };

    const int contentTop = contentRect_.top - scrollOffsetY_;
    const int contentBottom = contentRect_.bottom - scrollOffsetY_;
    const int contentMargin = Dips(42);
    backButtonRect_ = {
        contentRect_.left + contentMargin,
        contentTop + Dips(18),
        contentRect_.left + contentMargin + Dips(128),
        contentTop + Dips(52)
    };

    const int panelLeft = contentRect_.left + contentMargin;
    const int panelTop = contentTop + Dips(148);
    const int panelRight = contentRect_.right - contentMargin;

    speedSliderTrackRect_ = {
        panelLeft + Dips(32),
        panelTop + Dips(102),
        panelRight - Dips(32),
        panelTop + Dips(108)
    };

    const int sliderWidth = std::max(1, static_cast<int>(speedSliderTrackRect_.right - speedSliderTrackRect_.left));
    const int sliderOffset = MulDiv(
        autoClicker_.clicksPerSecond - kMinClicksPerSecond,
        sliderWidth,
        kMaxClicksPerSecond - kMinClicksPerSecond);
    const int thumbCenterX = static_cast<int>(speedSliderTrackRect_.left) + sliderOffset;
    speedSliderThumbRect_ = {
        thumbCenterX - Dips(10),
        speedSliderTrackRect_.top - Dips(8),
        thumbCenterX + Dips(10),
        speedSliderTrackRect_.bottom + Dips(8)
    };

    const int controlLeft = panelLeft + Dips(32);
    const int controlRight = panelRight - Dips(32);
    const int controlGap = Dips(16);
    const int buttonWidth = std::max(1, (controlRight - controlLeft - controlGap) / 2);

    activationKeyButtonRect_ = {
        controlLeft,
        panelTop + Dips(178),
        std::min(controlLeft + buttonWidth, controlRight),
        panelTop + Dips(222)
    };

    outputButtonButtonRect_ = {
        std::min(static_cast<int>(activationKeyButtonRect_.right) + controlGap, controlRight),
        panelTop + Dips(178),
        controlRight,
        panelTop + Dips(222)
    };

    startStopButtonRect_ = {
        panelLeft + Dips(32),
        panelTop + Dips(268),
        panelLeft + Dips(200),
        panelTop + Dips(316)
    };

    const int converterLeft = contentRect_.left + contentMargin;
    const int converterRight = contentRect_.right - contentMargin;
    const int converterTop = contentTop + Dips(132);
    converterDropZoneRect_ = {
        converterLeft,
        converterTop,
        converterRight,
        converterTop + Dips(96)
    };

    const int converterBrowseHeight = Dips(34);
    converterBrowseButtonRect_ = {
        converterDropZoneRect_.right - Dips(164),
        converterDropZoneRect_.top + ((converterDropZoneRect_.bottom - converterDropZoneRect_.top) - converterBrowseHeight) / 2,
        converterDropZoneRect_.right - Dips(22),
        converterDropZoneRect_.top + ((converterDropZoneRect_.bottom - converterDropZoneRect_.top) + converterBrowseHeight) / 2
    };

    const int simpleTop = converterDropZoneRect_.bottom + Dips(18);
    const int settingsGap = Dips(14);
    converterFormatButtonRect_ = {
        converterLeft + Dips(22),
        simpleTop + Dips(30),
        converterLeft + Dips(250),
        simpleTop + Dips(70)
    };
    const int converterSimpleButtonHeight = Dips(40);
    converterConvertButtonRect_ = {
        converterRight - Dips(172),
        converterFormatButtonRect_.top + ((converterFormatButtonRect_.bottom - converterFormatButtonRect_.top) - converterSimpleButtonHeight) / 2,
        converterRight - Dips(22),
        converterFormatButtonRect_.top + ((converterFormatButtonRect_.bottom - converterFormatButtonRect_.top) + converterSimpleButtonHeight) / 2
    };
    converterAdvancedToggleRect_ = {
        converterLeft,
        simpleTop + Dips(96),
        converterRight,
        simpleTop + Dips(134)
    };

    const int advancedTop = converterAdvancedToggleRect_.bottom + Dips(18);
    const int advancedLeft = converterLeft + Dips(22);
    const int advancedRight = converterRight - Dips(22);
    const int advancedColumnWidth = std::max(Dips(160), (advancedRight - advancedLeft - settingsGap) / 2);

    converterConflictButtonRect_ = {
        advancedLeft,
        advancedTop + Dips(64),
        advancedLeft + advancedColumnWidth,
        advancedTop + Dips(104)
    };
    converterJpgBackgroundButtonRect_ = {
        converterConflictButtonRect_.right + settingsGap,
        converterConflictButtonRect_.top,
        advancedRight,
        converterConflictButtonRect_.bottom
    };

    converterQualityTrackRect_ = {
        converterJpgBackgroundButtonRect_.left,
        converterJpgBackgroundButtonRect_.bottom + Dips(22),
        converterJpgBackgroundButtonRect_.right,
        converterJpgBackgroundButtonRect_.bottom + Dips(28)
    };

    const int activeQuality = conversionOptions_.outputFormat == ImageFormat::Webp
        ? conversionOptions_.webpQuality
        : conversionOptions_.jpgQuality;
    const int qualityTrackWidth = std::max(1, static_cast<int>(converterQualityTrackRect_.right - converterQualityTrackRect_.left));
    const int qualityOffset = MulDiv(activeQuality - 1, qualityTrackWidth, 99);
    const int qualityCenterX = static_cast<int>(converterQualityTrackRect_.left) + qualityOffset;
    converterQualityThumbRect_ = {
        qualityCenterX - Dips(8),
        converterQualityTrackRect_.top - Dips(6),
        qualityCenterX + Dips(8),
        converterQualityTrackRect_.bottom + Dips(6)
    };

    converterWebpLosslessRect_ = {
        converterJpgBackgroundButtonRect_.left,
        converterQualityTrackRect_.bottom + Dips(10),
        converterJpgBackgroundButtonRect_.right,
        converterQualityTrackRect_.bottom + Dips(40)
    };

    const int actionTop = advancedTop + Dips(124);
    converterCancelButtonRect_ = {
        advancedLeft,
        actionTop,
        advancedLeft + Dips(120),
        actionTop + Dips(44)
    };
    converterClearButtonRect_ = {
        converterCancelButtonRect_.right + Dips(12),
        actionTop,
        converterCancelButtonRect_.right + Dips(132),
        actionTop + Dips(44)
    };
    converterRemoveFailedButtonRect_ = {
        converterClearButtonRect_.right + Dips(12),
        actionTop,
        converterClearButtonRect_.right + Dips(156),
        actionTop + Dips(44)
    };
    converterProgressRect_ = {
        advancedLeft,
        actionTop + Dips(58),
        advancedRight,
        actionTop + Dips(68)
    };

    converterQueueRect_ = {
        advancedLeft,
        converterProgressRect_.bottom + Dips(38),
        advancedRight,
        contentBottom - Dips(32)
    };

    const int mediaLeft = contentRect_.left + contentMargin;
    const int mediaRight = contentRect_.right - contentMargin;
    const int mediaTop = contentTop + Dips(86);

    mediaUrlEditRect_ = {
        mediaLeft + Dips(24),
        mediaTop + Dips(52),
        mediaRight - Dips(244),
        mediaTop + Dips(92)
    };
    const int mediaUrlButtonHeight = Dips(38);
    mediaAnalyzeButtonRect_ = {
        mediaUrlEditRect_.right + Dips(12),
        mediaUrlEditRect_.top + ((mediaUrlEditRect_.bottom - mediaUrlEditRect_.top) - mediaUrlButtonHeight) / 2,
        mediaUrlEditRect_.right + Dips(112),
        mediaUrlEditRect_.top + ((mediaUrlEditRect_.bottom - mediaUrlEditRect_.top) + mediaUrlButtonHeight) / 2
    };
    mediaClearButtonRect_ = {
        mediaAnalyzeButtonRect_.right + Dips(10),
        mediaAnalyzeButtonRect_.top,
        mediaRight - Dips(24),
        mediaAnalyzeButtonRect_.bottom
    };
    const int mediaColumnGap = Dips(18);
    const int mediaColumnWidth = std::max(Dips(260), (mediaRight - mediaLeft - mediaColumnGap) / 2);
    mediaMetadataRect_ = {
        mediaLeft,
        mediaTop + Dips(120),
        mediaLeft + mediaColumnWidth,
        mediaTop + Dips(500)
    };

    const int mediaSettingsLeft = mediaMetadataRect_.right + mediaColumnGap;
    const int mediaSettingsRight = mediaRight;
    const int mediaSettingsTop = mediaMetadataRect_.top;
    const int mediaGap = Dips(14);
    const int mediaSettingButtonWidth = std::max(Dips(140), (mediaSettingsRight - mediaSettingsLeft - Dips(48) - mediaGap) / 2);
    mediaFormatButtonRect_ = {
        mediaSettingsLeft + Dips(24),
        mediaSettingsTop + Dips(44),
        mediaSettingsLeft + Dips(24) + mediaSettingButtonWidth,
        mediaSettingsTop + Dips(82)
    };
    mediaQualityButtonRect_ = {
        mediaFormatButtonRect_.right + mediaGap,
        mediaFormatButtonRect_.top,
        mediaRight - Dips(24),
        mediaFormatButtonRect_.bottom
    };
    mediaOutputFolderRect_ = {
        mediaSettingsLeft + Dips(24),
        mediaFormatButtonRect_.bottom + Dips(42),
        mediaSettingsRight - Dips(144),
        mediaFormatButtonRect_.bottom + Dips(80)
    };
    mediaBrowseButtonRect_ = {
        mediaOutputFolderRect_.right + Dips(12),
        mediaOutputFolderRect_.top,
        mediaSettingsRight - Dips(24),
        mediaOutputFolderRect_.bottom
    };
    mediaFileNameEditRect_ = {
        mediaSettingsLeft + Dips(24),
        mediaOutputFolderRect_.bottom + Dips(42),
        mediaSettingsRight - Dips(24),
        mediaOutputFolderRect_.bottom + Dips(80)
    };
    mediaDownloadButtonRect_ = {
        mediaSettingsLeft + Dips(24),
        mediaFileNameEditRect_.bottom + Dips(18),
        mediaSettingsLeft + Dips(174),
        mediaFileNameEditRect_.bottom + Dips(62)
    };
    mediaCancelButtonRect_ = {
        mediaDownloadButtonRect_.right + Dips(12),
        mediaDownloadButtonRect_.top,
        mediaDownloadButtonRect_.right + Dips(132),
        mediaDownloadButtonRect_.bottom
    };
    mediaProgressRect_ = {
        mediaSettingsLeft + Dips(24),
        mediaDownloadButtonRect_.bottom + Dips(16),
        mediaSettingsRight - Dips(24),
        mediaDownloadButtonRect_.bottom + Dips(26)
    };
    mediaOpenFileButtonRect_ = {
        mediaProgressRect_.left,
        mediaProgressRect_.bottom + Dips(28),
        mediaProgressRect_.left + Dips(130),
        mediaProgressRect_.bottom + Dips(66)
    };
    mediaOpenFolderButtonRect_ = {
        mediaOpenFileButtonRect_.right + Dips(12),
        mediaOpenFileButtonRect_.top,
        mediaOpenFileButtonRect_.right + Dips(154),
        mediaOpenFileButtonRect_.bottom
    };
    mediaCopyPathButtonRect_ = {
        mediaOpenFolderButtonRect_.right + Dips(12),
        mediaOpenFolderButtonRect_.top,
        mediaOpenFolderButtonRect_.right + Dips(136),
        mediaOpenFolderButtonRect_.bottom
    };

    const int settingsLeft = contentRect_.left + contentMargin;
    const int settingsTop = contentTop + Dips(166);
    settingsCheckUpdatesButtonRect_ = {
        settingsLeft + Dips(28),
        settingsTop + Dips(74),
        settingsLeft + Dips(220),
        settingsTop + Dips(118)
    };
    settingsDownloadUpdateButtonRect_ = {
        settingsLeft + Dips(28),
        settingsTop + Dips(300),
        settingsLeft + Dips(210),
        settingsTop + Dips(344)
    };

    int desiredContentHeight = contentRect_.bottom - contentRect_.top;
    if (currentPage_ == Page::Tool && currentTool_ == ToolKind::MediaDownloader)
    {
        desiredContentHeight = Dips(720);
    }
    else if (currentPage_ == Page::Tool && currentTool_ == ToolKind::FileConverter)
    {
        desiredContentHeight = fileConverterAdvancedOpen_ ? Dips(820) : Dips(680);
    }
    else if (currentPage_ == Page::Tool && currentTool_ == ToolKind::AutoClicker)
    {
        desiredContentHeight = Dips(560);
    }
    else if (currentPage_ == Page::Settings)
    {
        desiredContentHeight = Dips(620);
    }
    else
    {
        const auto visibleTools = VisibleToolsForCurrentPage();
        if (!visibleTools.empty())
        {
            const size_t rowCount = (visibleTools.size() + 2) / 3;
            desiredContentHeight = Dips(124) + static_cast<int>(rowCount) * Dips(178) + Dips(64);
        }
    }

    const int viewportHeight = std::max(1, static_cast<int>(contentRect_.bottom - contentRect_.top));
    scrollContentHeight_ = std::max(desiredContentHeight, viewportHeight);
    maxScrollOffsetY_ = std::max(0, scrollContentHeight_ - viewportHeight);
    ClampScrollOffset();

    scrollBarTrackRect_ = {
        contentRect_.right - Dips(14),
        contentRect_.top + Dips(10),
        contentRect_.right - Dips(6),
        contentRect_.bottom - Dips(10)
    };

    if (IsScrollBarVisible())
    {
        const int trackHeight = std::max(1, static_cast<int>(scrollBarTrackRect_.bottom - scrollBarTrackRect_.top));
        const int thumbHeight = std::clamp(MulDiv(viewportHeight, trackHeight, scrollContentHeight_), Dips(36), trackHeight);
        const int travel = std::max(1, trackHeight - thumbHeight);
        const int thumbTop = scrollBarTrackRect_.top + MulDiv(scrollOffsetY_, travel, std::max(1, maxScrollOffsetY_));
        scrollBarThumbRect_ = {
            scrollBarTrackRect_.left,
            thumbTop,
            scrollBarTrackRect_.right,
            thumbTop + thumbHeight
        };
    }
    else
    {
        scrollBarThumbRect_ = {};
    }

    UpdateMediaDownloaderControls();
}

void ToolkitApp::ClampScrollOffset()
{
    scrollOffsetY_ = std::clamp(scrollOffsetY_, 0, std::max(0, maxScrollOffsetY_));
}

void ToolkitApp::SetScrollOffset(int offset)
{
    const int clampedOffset = std::clamp(offset, 0, std::max(0, maxScrollOffsetY_));
    if (clampedOffset == scrollOffsetY_)
    {
        return;
    }

    scrollOffsetY_ = clampedOffset;
    CloseDropdown();
    RecalculateLayout();
    InvalidateRect(hwnd_, &contentRect_, FALSE);
}

bool ToolkitApp::IsScrollBarVisible() const
{
    return maxScrollOffsetY_ > 0;
}

void ToolkitApp::PaintScrollBar(HDC hdc)
{
    if (!IsScrollBarVisible())
    {
        return;
    }

    FillRoundRect(hdc, scrollBarTrackRect_, Dips(8), RGB(24, 28, 35));
    FillRoundRect(hdc, scrollBarThumbRect_, Dips(8), RGB(71, 83, 102));
}

void ToolkitApp::SelectPage(Page page)
{
    if (currentPage_ == page)
    {
        return;
    }

    currentPage_ = page;
    currentTool_ = ToolKind::None;
    hoverToolIndex_ = -1;
    hasHoveredButton_ = false;
    hasPressedButton_ = false;
    activeDropdown_ = DropdownKind::None;
    awaitingActivationKey_ = false;
    awaitingOutputButton_ = false;
    scrollOffsetY_ = 0;
    scrollBarDragging_ = false;
    UpdateMediaDownloaderControls();
    RecalculateLayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ToolkitApp::OpenTool(ToolKind tool)
{
    currentPage_ = Page::Tool;
    currentTool_ = tool;
    hoverToolIndex_ = -1;
    hasHoveredButton_ = false;
    hasPressedButton_ = false;
    activeDropdown_ = DropdownKind::None;
    awaitingActivationKey_ = false;
    awaitingOutputButton_ = false;
    scrollOffsetY_ = 0;
    scrollBarDragging_ = false;
    UpdateMediaDownloaderControls();
    RecalculateLayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool ToolkitApp::EnsureBackBuffer(HDC hdc, int width, int height)
{
    if (width <= 0 || height <= 0)
    {
        return false;
    }

    if (backBufferDc_ && backBufferBitmap_ &&
        backBufferSize_.cx == width && backBufferSize_.cy == height)
    {
        return true;
    }

    ReleaseBackBuffer();

    backBufferDc_ = CreateCompatibleDC(hdc);
    if (!backBufferDc_)
    {
        return false;
    }

    backBufferBitmap_ = CreateCompatibleBitmap(hdc, width, height);
    if (!backBufferBitmap_)
    {
        ReleaseBackBuffer();
        return false;
    }

    backBufferPreviousBitmap_ = static_cast<HBITMAP>(SelectObject(backBufferDc_, backBufferBitmap_));
    backBufferSize_ = { width, height };
    return true;
}

void ToolkitApp::ReleaseBackBuffer()
{
    if (backBufferDc_ && backBufferPreviousBitmap_)
    {
        SelectObject(backBufferDc_, backBufferPreviousBitmap_);
        backBufferPreviousBitmap_ = nullptr;
    }

    if (backBufferBitmap_)
    {
        DeleteObject(backBufferBitmap_);
        backBufferBitmap_ = nullptr;
    }

    if (backBufferDc_)
    {
        DeleteDC(backBufferDc_);
        backBufferDc_ = nullptr;
    }

    backBufferSize_ = {};
}

void ToolkitApp::Paint(HDC hdc, const RECT& paintRect)
{
    const int width = clientRect_.right - clientRect_.left;
    const int height = clientRect_.bottom - clientRect_.top;

    if (!EnsureBackBuffer(hdc, width, height))
    {
        return;
    }

    RECT clippedPaint = paintRect;
    if (!HasArea(clippedPaint))
    {
        clippedPaint = clientRect_;
    }

    const int savedDc = SaveDC(backBufferDc_);
    IntersectClipRect(backBufferDc_, clippedPaint.left, clippedPaint.top, clippedPaint.right, clippedPaint.bottom);

    FillSolidRect(backBufferDc_, clippedPaint, kAppBackground);
    PaintHeader(backBufferDc_);
    {
        const int contentDc = SaveDC(backBufferDc_);
        IntersectClipRect(backBufferDc_, contentRect_.left, contentRect_.top, contentRect_.right, contentRect_.bottom);
        PaintContent(backBufferDc_);
        RestoreDC(backBufferDc_, contentDc);
    }
    PaintScrollBar(backBufferDc_);
    PaintVersionFooter(backBufferDc_);
    PaintDropdown(backBufferDc_);

    RestoreDC(backBufferDc_, savedDc);

    BitBlt(
        hdc,
        clippedPaint.left,
        clippedPaint.top,
        clippedPaint.right - clippedPaint.left,
        clippedPaint.bottom - clippedPaint.top,
        backBufferDc_,
        clippedPaint.left,
        clippedPaint.top,
        SRCCOPY);
}

void ToolkitApp::PaintHeader(HDC hdc)
{
    FillSolidRect(hdc, headerRect_, kSidebarBackground);

    RECT border = headerRect_;
    border.top = headerRect_.bottom - 1;
    FillSolidRect(hdc, border, kBorder);

    RECT logoRect {
        headerRect_.left + Dips(28),
        headerRect_.top + Dips(8),
        headerRect_.left + Dips(84),
        headerRect_.top + Dips(64)
    };
    PaintLogo(hdc, logoRect);

    PaintNavItem(hdc, favoritesNavRect_, L"Favorites", currentPage_ == Page::Favorites);
    PaintNavItem(hdc, allToolsNavRect_, L"All Tools", currentPage_ == Page::AllTools);
    PaintNavItem(hdc, settingsNavRect_, L"Settings", currentPage_ == Page::Settings);

    RECT dateTimeRect {
        std::max(settingsNavRect_.right + Dips(24), headerRect_.right - Dips(300)),
        headerRect_.top,
        headerRect_.right - Dips(28),
        headerRect_.bottom
    };
    DrawTextLine(
        hdc,
        CurrentDateTimeLabel().c_str(),
        dateTimeRect,
        bodyFont_,
        kTextSecondary,
        DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

void ToolkitApp::PaintVersionFooter(HDC hdc)
{
    RECT versionRect {
        clientRect_.right - Dips(190),
        clientRect_.bottom - Dips(28),
        clientRect_.right - Dips(18),
        clientRect_.bottom - Dips(8)
    };

    std::wstring versionText = L"Version ";
    versionText += APP_VERSION;
    DrawTextLine(
        hdc,
        versionText.c_str(),
        versionRect,
        bodyFont_,
        RGB(125, 134, 148),
        DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

void ToolkitApp::PaintLogo(HDC hdc, const RECT& bounds)
{
    PaintBitmap(hdc, logo_.get(), bounds);
}

void ToolkitApp::PaintBitmap(HDC hdc, Gdiplus::Bitmap* bitmap, const RECT& bounds)
{
    if (!bitmap)
    {
        return;
    }

    const int boundsWidth = bounds.right - bounds.left;
    const int boundsHeight = bounds.bottom - bounds.top;
    if (boundsWidth <= 0 || boundsHeight <= 0)
    {
        return;
    }

    const double imageWidth = static_cast<double>(bitmap->GetWidth());
    const double imageHeight = static_cast<double>(bitmap->GetHeight());
    if (imageWidth <= 0.0 || imageHeight <= 0.0)
    {
        return;
    }

    const double scale = std::min(
        static_cast<double>(boundsWidth) / imageWidth,
        static_cast<double>(boundsHeight) / imageHeight);
    const int drawWidth = std::max(1, static_cast<int>(imageWidth * scale));
    const int drawHeight = std::max(1, static_cast<int>(imageHeight * scale));
    const int drawLeft = bounds.left + (boundsWidth - drawWidth) / 2;
    const int drawTop = bounds.top + (boundsHeight - drawHeight) / 2;

    Gdiplus::Graphics graphics(hdc);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    Gdiplus::Rect destination(
        drawLeft,
        drawTop,
        drawWidth,
        drawHeight);
    graphics.DrawImage(bitmap, destination);
}

void ToolkitApp::PaintContent(HDC hdc)
{
    if (currentPage_ == Page::Tool && currentTool_ == ToolKind::FileConverter)
    {
        PaintFileConverter(hdc);
        return;
    }

    if (currentPage_ == Page::Tool && currentTool_ == ToolKind::AutoClicker)
    {
        PaintAutoClicker(hdc);
        return;
    }

    if (currentPage_ == Page::Tool && currentTool_ == ToolKind::MediaDownloader)
    {
        PaintMediaDownloader(hdc);
        return;
    }

    if (currentPage_ == Page::Settings)
    {
        PaintSettings(hdc);
        return;
    }

    const int margin = Dips(42);

    RECT pageTitleRect {
        contentRect_.left + margin,
        contentRect_.top + Dips(40),
        contentRect_.right - margin,
        contentRect_.top + Dips(80)
    };

    const wchar_t* pageTitle = currentPage_ == Page::Favorites ? L"Favorites" : L"All Tools";
    DrawTextLine(hdc, pageTitle, pageTitleRect, headingFont_, kTextPrimary, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    RECT rule {
        contentRect_.left + margin,
        pageTitleRect.bottom + Dips(20),
        contentRect_.right - margin,
        pageTitleRect.bottom + Dips(21)
    };
    FillSolidRect(hdc, rule, kBorder);

    auto visibleTools = VisibleToolsForCurrentPage();
    if (!visibleTools.empty())
    {
        PaintToolCards(hdc, visibleTools);
        return;
    }

    if (currentPage_ == Page::Favorites)
    {
        PaintEmptyState(
            hdc,
            L"No favorites yet.",
            L"Favorite tools will appear here once they are added.");
    }
    else
    {
        PaintEmptyState(
            hdc,
            L"No tools available yet.",
            L"Tools will be added here in future updates.");
    }
}

void ToolkitApp::PaintNavItem(HDC hdc, const RECT& bounds, const wchar_t* label, bool selected)
{
    int navIndex = 2;
    if (wcscmp(label, L"Favorites") == 0)
    {
        navIndex = 0;
    }
    else if (wcscmp(label, L"All Tools") == 0)
    {
        navIndex = 1;
    }
    const bool hovered = hoverNavIndex_ == navIndex;

    COLORREF background = selected ? kAccentSoft : hovered ? kPanelHover : kSidebarBackground;
    FillRoundRect(hdc, bounds, Dips(12), background);

    if (selected)
    {
        RECT accent {
            bounds.left + Dips(14),
            bounds.bottom - Dips(4),
            bounds.right - Dips(14),
            bounds.bottom - Dips(1)
        };
        FillRoundRect(hdc, accent, Dips(4), kAccent);
    }

    RECT iconRect {
        bounds.left + Dips(16),
        bounds.top + Dips(10),
        bounds.left + Dips(36),
        bounds.top + Dips(30)
    };

    if (navIndex == 0)
    {
        PaintFavoriteStar(hdc, iconRect, selected);
    }
    else if (navIndex == 1)
    {
        PaintBitmap(hdc, allToolsIcon_.get(), iconRect);
    }
    else
    {
        PaintBitmap(hdc, settingsIcon_.get(), iconRect);
    }

    RECT textRect = bounds;
    textRect.left += Dips(44);
    textRect.right -= Dips(12);
    DrawTextLine(hdc, label, textRect, navFont_, kTextPrimary, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
}

void ToolkitApp::PaintEmptyState(HDC hdc, const wchar_t* title, const wchar_t* subtitle)
{
    const int contentWidth = static_cast<int>(contentRect_.right - contentRect_.left);
    const int contentHeight = static_cast<int>(contentRect_.bottom - contentRect_.top);
    const int panelWidth = std::min(Dips(560), std::max(Dips(280), contentWidth - Dips(84)));
    const int panelHeight = Dips(190);
    const int panelLeft = static_cast<int>(contentRect_.left) + (contentWidth - panelWidth) / 2;
    const int panelTop = static_cast<int>(contentRect_.top) + (contentHeight - panelHeight) / 2 + Dips(20);

    RECT panel {
        panelLeft,
        panelTop,
        panelLeft + panelWidth,
        panelTop + panelHeight
    };

    FillRoundRect(hdc, panel, Dips(16), kPanelBackground);
    StrokeRoundRect(hdc, panel, Dips(16), kBorder);

    const int textBlockHeight = Dips(68);
    const int textBlockTop = panel.top + ((panelHeight - textBlockHeight) / 2);
    RECT titleRect {
        panel.left + Dips(28),
        textBlockTop,
        panel.right - Dips(28),
        textBlockTop + Dips(32)
    };
    DrawTextLine(hdc, title, titleRect, navFont_, kTextPrimary, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    RECT subtitleRect = titleRect;
    subtitleRect.top = titleRect.bottom + Dips(6);
    subtitleRect.bottom = subtitleRect.top + Dips(30);
    DrawTextLine(hdc, subtitle, subtitleRect, bodyFont_, kTextSecondary, DT_CENTER | DT_WORDBREAK | DT_VCENTER);
}

void ToolkitApp::PaintAutoClicker(HDC hdc)
{
    const int margin = Dips(42);
    const ToolDefinition* tool = FindTool(ToolKind::AutoClicker);

    PaintBackButton(hdc, backButtonRect_);

    RECT titleRect {
        contentRect_.left + margin,
        contentRect_.top + Dips(82),
        contentRect_.right - margin,
        contentRect_.top + Dips(122)
    };
    DrawTextLine(
        hdc,
        tool ? tool->name.c_str() : L"Auto Clicker",
        titleRect,
        headingFont_,
        kTextPrimary,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    RECT subtitleRect = titleRect;
    subtitleRect.top = titleRect.bottom + Dips(2);
    subtitleRect.bottom = subtitleRect.top + Dips(28);
    DrawTextLine(
        hdc,
        L"Hold the activation input to repeatedly press the selected output button.",
        subtitleRect,
        bodyFont_,
        kTextSecondary,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    RECT panel {
        contentRect_.left + margin,
        contentRect_.top + Dips(148),
        contentRect_.right - margin,
        contentRect_.top + Dips(500)
    };

    FillRoundRect(hdc, panel, Dips(16), kPanelBackground);
    StrokeRoundRect(hdc, panel, Dips(16), kBorder);

    RECT statusRect {
        panel.right - Dips(190),
        panel.top + Dips(28),
        panel.right - Dips(32),
        panel.top + Dips(62)
    };
    FillRoundRect(hdc, statusRect, Dips(17), autoClicker_.running ? RGB(35, 95, 64) : RGB(52, 57, 68));
    DrawTextLine(
        hdc,
        autoClicker_.running ? L"Status: running" : L"Status: stopped",
        statusRect,
        bodyFont_,
        kTextPrimary,
        DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    RECT speedLabelRect {
        panel.left + Dips(32),
        panel.top + Dips(30),
        statusRect.left - Dips(18),
        panel.top + Dips(60)
    };
    DrawTextLine(hdc, L"Speed", speedLabelRect, navFont_, kTextPrimary, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    std::wstring speedValue = std::to_wstring(autoClicker_.clicksPerSecond) + L" clicks/sec";
    RECT speedValueRect = speedLabelRect;
    speedValueRect.top = speedLabelRect.bottom + Dips(4);
    speedValueRect.bottom = speedValueRect.top + Dips(24);
    DrawTextLine(hdc, speedValue.c_str(), speedValueRect, bodyFont_, kTextSecondary, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    PaintSlider(hdc);

    RECT activationLabelRect {
        activationKeyButtonRect_.left,
        activationKeyButtonRect_.top - Dips(30),
        activationKeyButtonRect_.right,
        activationKeyButtonRect_.top - Dips(6)
    };
    DrawTextLine(hdc, L"Activation key", activationLabelRect, bodyFont_, kTextSecondary, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    std::wstring activationLabel = awaitingActivationKey_ ? L"Press a key..." : ActivationKeyLabel();
    PaintButton(hdc, activationKeyButtonRect_, activationLabel.c_str(), false, awaitingActivationKey_);

    RECT outputLabelRect {
        outputButtonButtonRect_.left,
        outputButtonButtonRect_.top - Dips(30),
        outputButtonButtonRect_.right,
        outputButtonButtonRect_.top - Dips(6)
    };
    DrawTextLine(hdc, L"Output button", outputLabelRect, bodyFont_, kTextSecondary, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    std::wstring outputLabel = awaitingOutputButton_ ? L"Click mouse button..." : OutputButtonLabel();
    PaintButton(hdc, outputButtonButtonRect_, outputLabel.c_str(), false, awaitingOutputButton_);

    PaintButton(
        hdc,
        startStopButtonRect_,
        L"Hold to test",
        true,
        autoClicker_.running);

    RECT hintRect {
        startStopButtonRect_.right + Dips(20),
        startStopButtonRect_.top,
        panel.right - Dips(32),
        startStopButtonRect_.bottom
    };

    std::wstring hint = L"Hold " + ActivationKeyLabel() + L" to run.";
    DrawTextLine(hdc, hint.c_str(), hintRect, bodyFont_, kTextSecondary, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

void ToolkitApp::PaintButton(HDC hdc, const RECT& bounds, const wchar_t* label, bool primary, bool active, bool enabled)
{
    const bool hovered = enabled && hasHoveredButton_ && EqualRect(&bounds, &hoveredButtonRect_);
    const bool pressed = enabled && hasPressedButton_ && EqualRect(&bounds, &pressedButtonRect_);
    COLORREF background = !enabled
        ? RGB(34, 38, 46)
        : primary
        ? (pressed ? RGB(34, 98, 198) : hovered ? RGB(62, 123, 222) : active ? RGB(178, 72, 72) : kAccentSoft)
        : (pressed ? RGB(46, 58, 74) : hovered ? RGB(48, 56, 69) : active ? RGB(45, 72, 112) : RGB(37, 42, 51));
    COLORREF border = !enabled ? RGB(42, 47, 57) : primary ? (active ? RGB(220, 96, 96) : kAccent) : hovered ? RGB(74, 86, 104) : kBorder;
    COLORREF text = enabled ? kTextPrimary : RGB(112, 120, 132);

    RECT paintBounds = bounds;
    if (pressed)
    {
        OffsetRect(&paintBounds, 0, Dips(1));
    }

    FillRoundRect(hdc, paintBounds, Dips(12), background);
    StrokeRoundRect(hdc, paintBounds, Dips(12), border);
    DrawTextLine(hdc, label, paintBounds, navFont_, text, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

void ToolkitApp::PaintChevron(HDC hdc, const RECT& bounds, bool down, COLORREF color)
{
    const int centerX = bounds.left + ((bounds.right - bounds.left) / 2);
    const int centerY = bounds.top + ((bounds.bottom - bounds.top) / 2);
    const int width = static_cast<int>(bounds.right - bounds.left);
    const int height = static_cast<int>(bounds.bottom - bounds.top);
    const int size = std::max(4, std::min(width, height) / 3);

    POINT points[3] {};
    if (down)
    {
        points[0] = { centerX - size, centerY - (size / 2) };
        points[1] = { centerX + size, centerY - (size / 2) };
        points[2] = { centerX, centerY + size };
    }
    else
    {
        points[0] = { centerX + (size / 2), centerY - size };
        points[1] = { centerX + (size / 2), centerY + size };
        points[2] = { centerX - size, centerY };
    }

    HBRUSH brush = CreateSolidBrush(color);
    HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(hdc, brush));
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, pen));
    Polygon(hdc, points, static_cast<int>(std::size(points)));
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void ToolkitApp::PaintBackButton(HDC hdc, const RECT& bounds)
{
    PaintButton(hdc, bounds, L"", false);
    const int groupWidth = Dips(104);
    const int groupLeft = bounds.left + ((bounds.right - bounds.left) - groupWidth) / 2;
    RECT chevronRect {
        groupLeft,
        bounds.top + Dips(8),
        groupLeft + Dips(18),
        bounds.bottom - Dips(8)
    };
    PaintChevron(hdc, chevronRect, false, kTextSecondary);

    RECT textRect {
        chevronRect.right + Dips(8),
        bounds.top,
        groupLeft + groupWidth,
        bounds.bottom
    };
    DrawTextLine(hdc, L"All Tools", textRect, navFont_, kTextPrimary, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

void ToolkitApp::PaintDropdownButton(HDC hdc, const RECT& bounds, const wchar_t* label, bool enabled, bool down)
{
    PaintButton(hdc, bounds, label, false, false, enabled);
    if (!enabled)
    {
        return;
    }

    RECT chevronRect {
        bounds.right - Dips(30),
        bounds.top + Dips(10),
        bounds.right - Dips(12),
        bounds.bottom - Dips(10)
    };
    PaintChevron(hdc, chevronRect, down, kTextSecondary);
}

void ToolkitApp::PaintSlider(HDC hdc)
{
    RECT inactiveTrack = speedSliderTrackRect_;
    FillRoundRect(hdc, inactiveTrack, Dips(6), RGB(48, 55, 67));

    RECT activeTrack = speedSliderTrackRect_;
    activeTrack.right = speedSliderThumbRect_.left + ((speedSliderThumbRect_.right - speedSliderThumbRect_.left) / 2);
    FillRoundRect(hdc, activeTrack, Dips(6), kAccent);

    FillRoundRect(hdc, speedSliderThumbRect_, Dips(18), kTextPrimary);
    StrokeRoundRect(hdc, speedSliderThumbRect_, Dips(18), kAccent);
}

void ToolkitApp::PaintFileConverter(HDC hdc)
{
    const int margin = Dips(42);
    const ToolDefinition* tool = FindTool(ToolKind::FileConverter);

    PaintBackButton(hdc, backButtonRect_);

    RECT titleRect {
        contentRect_.left + margin,
        contentRect_.top + Dips(62),
        contentRect_.right - margin,
        contentRect_.top + Dips(102)
    };
    DrawTextLine(
        hdc,
        tool ? tool->name.c_str() : L"File Converter",
        titleRect,
        headingFont_,
        kTextPrimary,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    RECT subtitleRect = titleRect;
    subtitleRect.top = titleRect.bottom + Dips(2);
    subtitleRect.bottom = subtitleRect.top + Dips(28);
    DrawTextLine(
        hdc,
        L"Convert images between common formats.",
        subtitleRect,
        bodyFont_,
        kTextSecondary,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    FillRoundRect(hdc, converterDropZoneRect_, Dips(18), RGB(30, 36, 47));
    StrokeRoundRect(hdc, converterDropZoneRect_, Dips(18), kAccentSoft);

    RECT dropTitleRect {
        converterDropZoneRect_.left + Dips(28),
        converterDropZoneRect_.top + Dips(16),
        converterBrowseButtonRect_.left - Dips(16),
        converterDropZoneRect_.top + Dips(46)
    };
    DrawTextLine(hdc, L"Drag and drop files here", dropTitleRect, navFont_, kTextPrimary, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    RECT dropSubtitleRect = dropTitleRect;
    dropSubtitleRect.top = dropTitleRect.bottom + Dips(4);
    dropSubtitleRect.bottom = dropSubtitleRect.top + Dips(26);
    std::wstring dropSubtitle = L"or click Browse Files. Supports WEBP, PNG, JPG, JPEG, and BMP.";
    if (!conversionJobs_.empty())
    {
        dropSubtitle = std::to_wstring(conversionJobs_.size()) +
            (conversionJobs_.size() == 1 ? L" file queued." : L" files queued.");
    }
    DrawTextLine(
        hdc,
        dropSubtitle.c_str(),
        dropSubtitleRect,
        bodyFont_,
        kTextSecondary,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    PaintButton(hdc, converterBrowseButtonRect_, L"Browse Files", false);

    RECT simplePanel {
        converterDropZoneRect_.left,
        converterDropZoneRect_.bottom + Dips(18),
        converterDropZoneRect_.right,
        converterConvertButtonRect_.bottom + Dips(16)
    };
    FillRoundRect(hdc, simplePanel, Dips(16), kPanelBackground);
    StrokeRoundRect(hdc, simplePanel, Dips(16), kBorder);

    auto paintSetting = [&](const wchar_t* label, const RECT& button, const std::wstring& value, bool enabled = true)
    {
        RECT labelRect {
            button.left,
            button.top - Dips(25),
            button.right,
            button.top - Dips(4)
        };
        DrawTextLine(hdc, label, labelRect, bodyFont_, kTextSecondary, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        PaintDropdownButton(hdc, button, value.c_str(), enabled);
    };

    paintSetting(L"Convert to", converterFormatButtonRect_, OutputFormatLabel(conversionOptions_.outputFormat));
    PaintButton(hdc, converterConvertButtonRect_, fileConverterConverting_ ? L"Converting..." : L"Convert", true, fileConverterConverting_);

    PaintDropdownButton(
        hdc,
        converterAdvancedToggleRect_,
        L"Advanced options",
        true,
        !fileConverterAdvancedOpen_);

    if (!fileConverterAdvancedOpen_)
    {
        return;
    }

    RECT advancedPanel {
        converterAdvancedToggleRect_.left,
        converterAdvancedToggleRect_.bottom + Dips(12),
        converterAdvancedToggleRect_.right,
        converterQueueRect_.bottom + Dips(16)
    };
    FillRoundRect(hdc, advancedPanel, Dips(16), kPanelBackground);
    StrokeRoundRect(hdc, advancedPanel, Dips(16), kBorder);

    RECT advancedTitleRect {
        advancedPanel.left + Dips(22),
        advancedPanel.top + Dips(14),
        advancedPanel.right - Dips(22),
        advancedPanel.top + Dips(40)
    };
    DrawTextLine(hdc, L"Advanced options", advancedTitleRect, navFont_, kTextPrimary, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    paintSetting(L"Conflict handling", converterConflictButtonRect_, ConflictBehaviorLabel());

    if (conversionOptions_.outputFormat == ImageFormat::Jpg)
    {
        paintSetting(
            L"JPG background",
            converterJpgBackgroundButtonRect_,
            conversionOptions_.jpgBackground == JpgBackground::White ? L"White" : L"Black");
    }
    else if (conversionOptions_.outputFormat == ImageFormat::Webp)
    {
        paintSetting(L"WEBP options", converterJpgBackgroundButtonRect_, L"Quality slider", false);
    }
    else
    {
        paintSetting(L"Format options", converterJpgBackgroundButtonRect_, L"Preserve alpha");
    }

    if (conversionOptions_.outputFormat == ImageFormat::Jpg ||
        conversionOptions_.outputFormat == ImageFormat::Webp)
    {
        const int qualityValue = conversionOptions_.outputFormat == ImageFormat::Webp
            ? conversionOptions_.webpQuality
            : conversionOptions_.jpgQuality;

        RECT qualityLabel {
            converterQualityTrackRect_.left,
            converterQualityTrackRect_.top - Dips(18),
            converterQualityTrackRect_.right,
            converterQualityTrackRect_.top
        };
        std::wstring qualityText = L"Quality " + std::to_wstring(qualityValue);
        DrawTextLine(hdc, qualityText.c_str(), qualityLabel, bodyFont_, kTextSecondary, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

        FillRoundRect(hdc, converterQualityTrackRect_, Dips(6), RGB(48, 55, 67));
        RECT activeQualityTrack = converterQualityTrackRect_;
        activeQualityTrack.right = converterQualityThumbRect_.left + ((converterQualityThumbRect_.right - converterQualityThumbRect_.left) / 2);
        FillRoundRect(hdc, activeQualityTrack, Dips(6), kAccent);
        FillRoundRect(hdc, converterQualityThumbRect_, Dips(14), kTextPrimary);
        StrokeRoundRect(hdc, converterQualityThumbRect_, Dips(14), kAccent);
    }

    if (conversionOptions_.outputFormat == ImageFormat::Webp)
    {
        RECT losslessBox {
            converterWebpLosslessRect_.left,
            converterWebpLosslessRect_.top + Dips(5),
            converterWebpLosslessRect_.left + Dips(18),
            converterWebpLosslessRect_.top + Dips(23)
        };
        StrokeRoundRect(hdc, losslessBox, Dips(4), kBorder);
        if (conversionOptions_.webpLossless)
        {
            FillRoundRect(hdc, ShrinkRect(losslessBox, Dips(4), Dips(4)), Dips(3), kAccent);
        }
        RECT losslessText = converterWebpLosslessRect_;
        losslessText.left = losslessBox.right + Dips(8);
        DrawTextLine(hdc, L"Lossless when supported by installed codec", losslessText, bodyFont_, kTextSecondary, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }

    PaintButton(hdc, converterCancelButtonRect_, L"Cancel", false);
    PaintButton(hdc, converterClearButtonRect_, L"Clear All", false);
    PaintButton(hdc, converterRemoveFailedButtonRect_, L"Remove Failed", false);

    PaintProgressBar(hdc, converterProgressRect_, ConverterProgress());

    RECT summaryRect {
        converterProgressRect_.left,
        converterProgressRect_.bottom + Dips(4),
        converterProgressRect_.right,
        converterProgressRect_.bottom + Dips(28)
    };
    DrawTextLine(hdc, fileConverterSummary_.empty() ? L"Ready." : fileConverterSummary_.c_str(), summaryRect, bodyFont_, kTextSecondary, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    FillRoundRect(hdc, converterQueueRect_, Dips(16), kPanelBackground);
    StrokeRoundRect(hdc, converterQueueRect_, Dips(16), kBorder);

    RECT queueTitleRect {
        converterQueueRect_.left + Dips(18),
        converterQueueRect_.top + Dips(12),
        converterQueueRect_.right - Dips(18),
        converterQueueRect_.top + Dips(40)
    };
    DrawTextLine(hdc, L"Queue", queueTitleRect, navFont_, kTextPrimary, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    if (conversionJobs_.empty())
    {
        const int emptyBlockHeight = Dips(64);
        const int emptyBlockTop = converterQueueRect_.top +
            ((converterQueueRect_.bottom - converterQueueRect_.top) - emptyBlockHeight) / 2;
        RECT emptyTitle {
            converterQueueRect_.left + Dips(24),
            emptyBlockTop,
            converterQueueRect_.right - Dips(24),
            emptyBlockTop + Dips(30)
        };
        DrawTextLine(hdc, L"No files queued.", emptyTitle, navFont_, kTextPrimary, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

        RECT emptySubtitle = emptyTitle;
        emptySubtitle.top = emptyTitle.bottom + Dips(4);
        emptySubtitle.bottom = emptySubtitle.top + Dips(28);
        DrawTextLine(hdc, L"Use the drop zone above to add files.", emptySubtitle, bodyFont_, kTextSecondary, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        return;
    }

    const int rowHeight = Dips(42);
    int rowTop = converterQueueRect_.top + Dips(46);
    const int availableQueueHeight = static_cast<int>(converterQueueRect_.bottom - rowTop - Dips(16));
    const int maxVisibleRows = std::max(1, availableQueueHeight / rowHeight);
    const int visibleRows = std::min<int>(static_cast<int>(conversionJobs_.size()), maxVisibleRows);

    for (int index = 0; index < visibleRows; ++index)
    {
        const ConversionJob& job = conversionJobs_[index];
        RECT row {
            converterQueueRect_.left + Dips(14),
            rowTop,
            converterQueueRect_.right - Dips(14),
            rowTop + rowHeight - Dips(6)
        };

        FillRoundRect(hdc, row, Dips(10), index == selectedConversionJob_ ? RGB(42, 52, 68) : RGB(35, 40, 49));

        RECT fileRect { row.left + Dips(14), row.top, row.left + Dips(260), row.bottom };
        DrawTextLine(hdc, job.fileName.c_str(), fileRect, monospaceFont_, kTextPrimary, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

        RECT formatRect { fileRect.right + Dips(12), row.top, fileRect.right + Dips(84), row.bottom };
        DrawTextLine(hdc, job.inputFormat.c_str(), formatRect, monospaceFont_, kTextSecondary, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

        RECT sizeRect { formatRect.right + Dips(12), row.top, formatRect.right + Dips(112), row.bottom };
        std::wstring sizeText = job.fileSize > 0 ? FileSizeLabel(job.fileSize) : L"--";
        DrawTextLine(hdc, sizeText.c_str(), sizeRect, monospaceFont_, kTextSecondary, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

        RECT statusRect { sizeRect.right + Dips(12), row.top, sizeRect.right + Dips(160), row.bottom };
        DrawTextLine(hdc, StatusLabel(job.status).c_str(), statusRect, monospaceFont_, job.status == ConversionStatus::Failed ? RGB(240, 120, 120) : kTextSecondary, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

        RECT removeRect { row.right - Dips(38), row.top + Dips(6), row.right - Dips(10), row.bottom - Dips(6) };
        DrawTextLine(hdc, L"X", removeRect, navFont_, kTextSecondary, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

        rowTop += rowHeight;
    }
}

void ToolkitApp::PaintMediaDownloader(HDC hdc)
{
    const int margin = Dips(42);
    const ToolDefinition* tool = FindTool(ToolKind::MediaDownloader);

    PaintBackButton(hdc, backButtonRect_);

    RECT titleRect {
        backButtonRect_.right + Dips(20),
        contentRect_.top + Dips(16),
        contentRect_.right - margin,
        contentRect_.top + Dips(48)
    };
    DrawTextLine(
        hdc,
        tool ? tool->name.c_str() : L"YouTube & SoundCloud Downloader",
        titleRect,
        headingFont_,
        kTextPrimary,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    RECT subtitleRect = titleRect;
    subtitleRect.top = titleRect.bottom + Dips(2);
    subtitleRect.bottom = subtitleRect.top + Dips(22);
    DrawTextLine(
        hdc,
        L"Download authorized videos or audio and save them as MP4, MP3, or WAV.",
        subtitleRect,
        bodyFont_,
        kTextSecondary,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    RECT noticeRect = subtitleRect;
    noticeRect.top = subtitleRect.bottom + Dips(2);
    noticeRect.bottom = noticeRect.top + Dips(20);
    DrawTextLine(
        hdc,
        L"Only download content you own or have permission to save.",
        noticeRect,
        bodyFont_,
        kGold,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    RECT urlPanel {
        contentRect_.left + margin,
        mediaUrlEditRect_.top - Dips(34),
        contentRect_.right - margin,
        mediaUrlEditRect_.bottom + Dips(24)
    };
    FillRoundRect(hdc, urlPanel, Dips(16), kPanelBackground);
    StrokeRoundRect(hdc, urlPanel, Dips(16), kBorder);

    RECT urlLabel {
        mediaUrlEditRect_.left,
        mediaUrlEditRect_.top - Dips(28),
        mediaUrlEditRect_.right,
        mediaUrlEditRect_.top - Dips(6)
    };
    DrawTextLine(hdc, L"Media URL", urlLabel, bodyFont_, kTextSecondary, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    FillRoundRect(hdc, mediaUrlEditRect_, Dips(10), RGB(25, 29, 36));
    StrokeRoundRect(hdc, mediaUrlEditRect_, Dips(10), kBorder);
    PaintButton(hdc, mediaAnalyzeButtonRect_, mediaAnalyzing_ ? L"Analyzing..." : L"Analyze", false, mediaAnalyzing_, !mediaDownloading_);
    PaintButton(hdc, mediaClearButtonRect_, L"Clear", false, false, !mediaAnalyzing_ && !mediaDownloading_);

    FillRoundRect(hdc, mediaMetadataRect_, Dips(16), kPanelBackground);
    StrokeRoundRect(hdc, mediaMetadataRect_, Dips(16), kBorder);

    RECT metadataTitle {
        mediaMetadataRect_.left + Dips(22),
        mediaMetadataRect_.top + Dips(14),
        mediaMetadataRect_.right - Dips(22),
        mediaMetadataRect_.top + Dips(42)
    };
    DrawTextLine(hdc, L"Metadata preview", metadataTitle, navFont_, kTextPrimary, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    RECT metadataBody {
        metadataTitle.left,
        metadataTitle.bottom + Dips(6),
        metadataTitle.right,
        mediaMetadataRect_.bottom - Dips(16)
    };

    if (mediaDownloadJob_.title.empty())
    {
        DrawTextLine(
            hdc,
            L"Paste a YouTube or SoundCloud link, then click Analyze.",
            metadataBody,
            bodyFont_,
            kTextSecondary,
            DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }
    else
    {
        std::vector<std::wstring> rows {
            L"Title: " + mediaDownloadJob_.title,
            L"Source: " + SupportedPlatformRegistry::PlatformLabel(mediaDownloadJob_.platform),
            L"Duration: " + (mediaDownloadJob_.duration.empty() ? L"Unknown" : mediaDownloadJob_.duration),
            L"Uploader: " + (mediaDownloadJob_.uploader.empty() ? L"Unknown" : mediaDownloadJob_.uploader),
            L"Available media: " + MediaDownloadService::MediaTypeLabel(mediaDownloadJob_.mediaType)
        };

        int rowTop = metadataBody.top;
        for (const std::wstring& row : rows)
        {
            RECT rowRect { metadataBody.left, rowTop, metadataBody.right, rowTop + Dips(22) };
            DrawTextLine(hdc, row.c_str(), rowRect, monospaceFont_, kTextSecondary, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            rowTop += Dips(23);
        }
    }

    RECT settingsPanel {
        mediaMetadataRect_.right + Dips(18),
        mediaMetadataRect_.top,
        contentRect_.right - margin,
        mediaMetadataRect_.bottom
    };
    FillRoundRect(hdc, settingsPanel, Dips(16), kPanelBackground);
    StrokeRoundRect(hdc, settingsPanel, Dips(16), kBorder);

    auto paintLabeledButton = [&](const wchar_t* label, const RECT& rect, const std::wstring& value, bool enabled = true)
    {
        RECT labelRect {
            rect.left,
            rect.top - Dips(26),
            rect.right,
            rect.top - Dips(5)
        };
        DrawTextLine(hdc, label, labelRect, bodyFont_, kTextSecondary, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        PaintDropdownButton(hdc, rect, value.c_str(), enabled);
    };

    paintLabeledButton(L"Save as", mediaFormatButtonRect_, MediaDownloadService::FormatLabel(mediaDownloadOptions_.outputFormat), !mediaDownloading_);
    paintLabeledButton(L"Quality", mediaQualityButtonRect_, MediaQualityLabel(), !mediaDownloading_ && mediaDownloadOptions_.outputFormat != MediaOutputFormat::Wav);

    RECT folderLabel {
        mediaOutputFolderRect_.left,
        mediaOutputFolderRect_.top - Dips(26),
        mediaOutputFolderRect_.right,
        mediaOutputFolderRect_.top - Dips(5)
    };
    DrawTextLine(hdc, L"Save location", folderLabel, bodyFont_, kTextSecondary, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    FillRoundRect(hdc, mediaOutputFolderRect_, Dips(10), RGB(25, 29, 36));
    StrokeRoundRect(hdc, mediaOutputFolderRect_, Dips(10), kBorder);
    RECT folderTextRect = ShrinkRect(mediaOutputFolderRect_, Dips(12), 0);
    DrawTextLine(
        hdc,
        mediaDownloadOptions_.outputFolder.empty() ? L"Choose a folder" : mediaDownloadOptions_.outputFolder.wstring().c_str(),
        folderTextRect,
        monospaceFont_,
        kTextPrimary,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    PaintButton(hdc, mediaBrowseButtonRect_, L"Browse", false, false, !mediaDownloading_);

    RECT fileNameLabel {
        mediaFileNameEditRect_.left,
        mediaFileNameEditRect_.top - Dips(26),
        mediaFileNameEditRect_.right,
        mediaFileNameEditRect_.top - Dips(5)
    };
    DrawTextLine(hdc, L"File name", fileNameLabel, bodyFont_, kTextSecondary, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    FillRoundRect(hdc, mediaFileNameEditRect_, Dips(10), RGB(25, 29, 36));
    StrokeRoundRect(hdc, mediaFileNameEditRect_, Dips(10), kBorder);

    PaintButton(
        hdc,
        mediaDownloadButtonRect_,
        mediaDownloading_ ? L"Downloading..." : L"Download",
        true,
        mediaDownloading_,
        CanStartMediaDownload());
    PaintButton(hdc, mediaCancelButtonRect_, L"Cancel", false, false, mediaDownloading_ || mediaAnalyzing_);

    PaintProgressBar(hdc, mediaProgressRect_, mediaDownloadJob_.progress);

    RECT statusRect {
        mediaProgressRect_.left,
        mediaProgressRect_.bottom + Dips(6),
        mediaProgressRect_.right,
        mediaProgressRect_.bottom + Dips(32)
    };

    std::wstring statusText = mediaStatusText_.empty()
        ? MediaDownloadService::StatusLabel(mediaDownloadJob_.status)
        : mediaStatusText_;
    if (!mediaDownloadJob_.speed.empty())
    {
        statusText += L"  " + mediaDownloadJob_.speed;
    }
    if (!mediaDownloadJob_.eta.empty())
    {
        statusText += L"  ETA " + mediaDownloadJob_.eta;
    }
    DrawTextLine(hdc, statusText.c_str(), statusRect, monospaceFont_, kTextSecondary, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    const std::wstring setupMessage = MediaSetupMessage();
    if (!setupMessage.empty())
    {
        RECT setupRect = statusRect;
        setupRect.top = statusRect.bottom + Dips(2);
        setupRect.bottom = setupRect.top + Dips(24);
        DrawTextLine(hdc, setupMessage.c_str(), setupRect, bodyFont_, RGB(240, 170, 100), DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }

    if (mediaDownloadJob_.status == MediaDownloadStatus::Complete && !mediaDownloadJob_.outputFilePath.empty())
    {
        PaintButton(hdc, mediaOpenFileButtonRect_, L"Open File", false);
        PaintButton(hdc, mediaOpenFolderButtonRect_, L"Open Folder", false);
        PaintButton(hdc, mediaCopyPathButtonRect_, L"Copy Path", false);

        RECT outputPathRect {
            mediaOpenFileButtonRect_.left,
            mediaOpenFileButtonRect_.bottom + Dips(8),
            settingsPanel.right - Dips(24),
            mediaOpenFileButtonRect_.bottom + Dips(34)
        };
        DrawTextLine(
            hdc,
            mediaDownloadJob_.outputFilePath.wstring().c_str(),
            outputPathRect,
            monospaceFont_,
            kTextSecondary,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }

    (void)tool;
}

void ToolkitApp::PaintSettings(HDC hdc)
{
    const int margin = Dips(42);
    const int contentTop = contentRect_.top - scrollOffsetY_;

    RECT titleRect {
        contentRect_.left + margin,
        contentTop + Dips(40),
        contentRect_.right - margin,
        contentTop + Dips(80)
    };
    DrawTextLine(hdc, L"Settings", titleRect, headingFont_, kTextPrimary, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    RECT subtitleRect = titleRect;
    subtitleRect.top = titleRect.bottom + Dips(2);
    subtitleRect.bottom = subtitleRect.top + Dips(28);
    DrawTextLine(
        hdc,
        L"About Rex's Toolkit and update checks.",
        subtitleRect,
        bodyFont_,
        kTextSecondary,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    RECT panel {
        contentRect_.left + margin,
        contentTop + Dips(132),
        contentRect_.right - margin,
        contentTop + Dips(560)
    };
    FillRoundRect(hdc, panel, Dips(16), kPanelBackground);
    StrokeRoundRect(hdc, panel, Dips(16), kBorder);

    RECT aboutTitle {
        panel.left + Dips(28),
        panel.top + Dips(24),
        panel.right - Dips(28),
        panel.top + Dips(54)
    };
    DrawTextLine(hdc, L"About", aboutTitle, navFont_, kTextPrimary, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    std::wstring currentVersion = L"Current version: ";
    currentVersion += APP_VERSION;
    RECT versionRect {
        panel.left + Dips(28),
        aboutTitle.bottom + Dips(4),
        panel.right - Dips(28),
        aboutTitle.bottom + Dips(32)
    };
    DrawTextLine(hdc, currentVersion.c_str(), versionRect, bodyFont_, kTextSecondary, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    PaintButton(
        hdc,
        settingsCheckUpdatesButtonRect_,
        updateChecking_ ? L"Checking..." : L"Check for Updates",
        true,
        updateChecking_,
        !updateChecking_);

    RECT resultPanel {
        panel.left + Dips(28),
        settingsCheckUpdatesButtonRect_.bottom + Dips(28),
        panel.right - Dips(28),
        panel.bottom - Dips(28)
    };
    FillRoundRect(hdc, resultPanel, Dips(14), RGB(27, 31, 39));
    StrokeRoundRect(hdc, resultPanel, Dips(14), kBorder);

    RECT resultTitle {
        resultPanel.left + Dips(20),
        resultPanel.top + Dips(16),
        resultPanel.right - Dips(20),
        resultPanel.top + Dips(46)
    };

    if (updateChecking_)
    {
        DrawTextLine(hdc, L"Checking for updates...", resultTitle, navFont_, kTextPrimary, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        return;
    }

    if (!hasUpdateResult_)
    {
        DrawTextLine(hdc, L"Ready to check for updates.", resultTitle, navFont_, kTextPrimary, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        RECT body = resultTitle;
        body.top = resultTitle.bottom + Dips(6);
        body.bottom = body.top + Dips(34);
        DrawTextLine(
            hdc,
            L"The app will compare this install with the latest release manifest.",
            body,
            bodyFont_,
            kTextSecondary,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        return;
    }

    if (updateResult_.status == UpdateCheckStatus::UpdateAvailable)
    {
        DrawTextLine(hdc, L"Update available", resultTitle, navFont_, kTextPrimary, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    }
    else if (updateResult_.status == UpdateCheckStatus::UpToDate)
    {
        DrawTextLine(hdc, L"Rex's Toolkit is up to date.", resultTitle, navFont_, kTextPrimary, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    }
    else
    {
        DrawTextLine(hdc, L"Could not check for updates.", resultTitle, navFont_, RGB(240, 170, 100), DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    }

    RECT detailRect {
        resultTitle.left,
        resultTitle.bottom + Dips(8),
        resultTitle.right,
        resultTitle.bottom + Dips(34)
    };
    std::wstring detail = L"Current version: ";
    detail += updateResult_.currentVersion.empty() ? APP_VERSION : updateResult_.currentVersion;
    if (!updateResult_.latestVersion.empty())
    {
        detail += L"    Latest version: ";
        detail += updateResult_.latestVersion;
    }
    DrawTextLine(hdc, detail.c_str(), detailRect, monospaceFont_, kTextSecondary, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    if (updateResult_.status == UpdateCheckStatus::UpdateAvailable)
    {
        RECT notesTitle {
            detailRect.left,
            detailRect.bottom + Dips(16),
            detailRect.right,
            detailRect.bottom + Dips(42)
        };
        DrawTextLine(hdc, L"Release notes", notesTitle, bodyFont_, kTextPrimary, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

        int noteTop = notesTitle.bottom + Dips(4);
        if (updateResult_.releaseNotes.empty())
        {
            RECT noteRect { notesTitle.left, noteTop, notesTitle.right, noteTop + Dips(24) };
            DrawTextLine(hdc, L"No release notes provided.", noteRect, bodyFont_, kTextSecondary, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        }
        else
        {
            const size_t noteCount = std::min<size_t>(updateResult_.releaseNotes.size(), 5);
            for (size_t index = 0; index < noteCount; ++index)
            {
                std::wstring note = L"- " + updateResult_.releaseNotes[index];
                RECT noteRect { notesTitle.left, noteTop, notesTitle.right, noteTop + Dips(24) };
                DrawTextLine(hdc, note.c_str(), noteRect, bodyFont_, kTextSecondary, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                noteTop += Dips(24);
            }
        }

        PaintButton(hdc, settingsDownloadUpdateButtonRect_, L"Download Update", true);
    }
    else if (updateResult_.status != UpdateCheckStatus::UpToDate)
    {
        RECT errorRect {
            detailRect.left,
            detailRect.bottom + Dips(12),
            detailRect.right,
            detailRect.bottom + Dips(60)
        };
        DrawTextLine(
            hdc,
            updateResult_.errorMessage.empty()
                ? L"Could not check for updates. Please check your internet connection and try again."
                : updateResult_.errorMessage.c_str(),
            errorRect,
            bodyFont_,
            kTextSecondary,
            DT_LEFT | DT_WORDBREAK | DT_VCENTER);
    }
}

void ToolkitApp::PaintProgressBar(HDC hdc, const RECT& bounds, double progress)
{
    FillRoundRect(hdc, bounds, Dips(8), RGB(47, 54, 66));

    RECT filled = bounds;
    const int width = bounds.right - bounds.left;
    filled.right = filled.left + static_cast<int>(width * std::clamp(progress, 0.0, 1.0));
    if (filled.right > filled.left)
    {
        FillRoundRect(hdc, filled, Dips(8), kAccent);
    }
}

void ToolkitApp::OpenDropdown(
    DropdownKind kind,
    const RECT& anchor,
    const std::vector<std::wstring>& labels,
    const std::vector<int>& values,
    const std::vector<bool>& enabled,
    int selectedValue)
{
    activeDropdown_ = kind;
    dropdownLabels_ = labels;
    dropdownValues_ = values;
    dropdownEnabled_ = enabled;
    dropdownSelectedValue_ = selectedValue;
    hoverDropdownIndex_ = -1;
    UpdateMediaDownloaderControls();

    const int itemHeight = Dips(38);
    const int height = static_cast<int>(dropdownLabels_.size()) * itemHeight + Dips(10);
    const int gap = Dips(6);
    dropdownRect_ = {
        anchor.left,
        anchor.bottom + gap,
        anchor.right,
        anchor.bottom + gap + height
    };

    const int bottomLimit = clientRect_.bottom - Dips(18);
    if (dropdownRect_.bottom > bottomLimit)
    {
        const int shift = dropdownRect_.bottom - bottomLimit;
        dropdownRect_.top -= shift;
        dropdownRect_.bottom -= shift;
    }

    if (dropdownRect_.top < contentRect_.top + Dips(8))
    {
        const int shift = contentRect_.top + Dips(8) - dropdownRect_.top;
        dropdownRect_.top += shift;
        dropdownRect_.bottom += shift;
    }

    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ToolkitApp::CloseDropdown()
{
    if (activeDropdown_ == DropdownKind::None)
    {
        return;
    }

    activeDropdown_ = DropdownKind::None;
    dropdownLabels_.clear();
    dropdownValues_.clear();
    dropdownEnabled_.clear();
    hoverDropdownIndex_ = -1;
    UpdateMediaDownloaderControls();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ToolkitApp::PaintDropdown(HDC hdc)
{
    if (activeDropdown_ == DropdownKind::None || dropdownLabels_.empty())
    {
        return;
    }

    FillRoundRect(hdc, dropdownRect_, Dips(12), RGB(29, 34, 43));
    StrokeRoundRect(hdc, dropdownRect_, Dips(12), kAccentSoft);

    const int itemHeight = Dips(38);
    int itemTop = dropdownRect_.top + Dips(5);
    for (size_t index = 0; index < dropdownLabels_.size(); ++index)
    {
        RECT itemRect {
            dropdownRect_.left + Dips(5),
            itemTop,
            dropdownRect_.right - Dips(5),
            itemTop + itemHeight
        };

        const bool enabled = index >= dropdownEnabled_.size() || dropdownEnabled_[index];
        const bool selected = index < dropdownValues_.size() && dropdownValues_[index] == dropdownSelectedValue_;
        const bool hovered = enabled && hoverDropdownIndex_ == static_cast<int>(index);

        if (selected || hovered)
        {
            FillRoundRect(hdc, itemRect, Dips(9), selected ? RGB(44, 86, 153) : RGB(43, 50, 62));
        }

        RECT checkRect {
            itemRect.left + Dips(10),
            itemRect.top,
            itemRect.left + Dips(34),
            itemRect.bottom
        };
        if (selected)
        {
            RECT markerRect = ShrinkRect(checkRect, Dips(8), Dips(12));
            FillRoundRect(hdc, markerRect, Dips(5), kTextPrimary);
        }

        RECT textRect = itemRect;
        textRect.left += Dips(38);
        textRect.right -= Dips(12);
        DrawTextLine(
            hdc,
            dropdownLabels_[index].c_str(),
            textRect,
            navFont_,
            enabled ? kTextPrimary : RGB(115, 123, 136),
            DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

        itemTop += itemHeight;
    }
}

bool ToolkitApp::HandleDropdownClick(POINT point)
{
    if (activeDropdown_ == DropdownKind::None)
    {
        return false;
    }

    if (!IsPointInRect(dropdownRect_, point))
    {
        CloseDropdown();
        return true;
    }

    const int itemHeight = Dips(38);
    const int index = (point.y - dropdownRect_.top - Dips(5)) / itemHeight;
    if (index < 0 || index >= static_cast<int>(dropdownLabels_.size()))
    {
        return true;
    }

    if (index < static_cast<int>(dropdownEnabled_.size()) && !dropdownEnabled_[index])
    {
        return true;
    }

    const int value = dropdownValues_[static_cast<size_t>(index)];
    const DropdownKind kind = activeDropdown_;
    CloseDropdown();

    if (kind == DropdownKind::FileOutputFormat)
    {
        conversionOptions_.outputFormat = static_cast<ImageFormat>(value);
        for (ConversionJob& job : conversionJobs_)
        {
            job.outputFormat = conversionOptions_.outputFormat;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }

    if (kind == DropdownKind::FileConflictBehavior)
    {
        conversionOptions_.conflictBehavior = static_cast<ConflictBehavior>(value);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }

    if (kind == DropdownKind::FileJpgBackground)
    {
        conversionOptions_.jpgBackground = static_cast<JpgBackground>(value);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }

    if (kind == DropdownKind::FileFormatOptions)
    {
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }

    if (kind == DropdownKind::MediaFormat)
    {
        mediaDownloadOptions_.outputFormat = static_cast<MediaOutputFormat>(value);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }

    if (kind == DropdownKind::MediaQuality)
    {
        if (mediaDownloadOptions_.outputFormat == MediaOutputFormat::Mp4)
        {
            mediaDownloadOptions_.mp4Quality = static_cast<Mp4Quality>(value);
        }
        else if (mediaDownloadOptions_.outputFormat == MediaOutputFormat::Mp3)
        {
            mediaDownloadOptions_.mp3Bitrate = static_cast<Mp3Bitrate>(value);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }

    return true;
}

RECT ToolkitApp::ButtonRectAtPoint(POINT point) const
{
    auto hit = [&](const RECT& rect)
    {
        return IsPointInRect(rect, point) ? rect : RECT {};
    };
    RECT rect {};

    if (currentPage_ != Page::Tool)
    {
        if (currentPage_ == Page::Settings)
        {
            rect = hit(settingsCheckUpdatesButtonRect_);
            if (rect.right > rect.left) return rect;
            if (hasUpdateResult_ && updateResult_.status == UpdateCheckStatus::UpdateAvailable)
            {
                rect = hit(settingsDownloadUpdateButtonRect_);
                if (rect.right > rect.left) return rect;
            }
        }
        return {};
    }

    rect = hit(backButtonRect_);
    if (rect.right > rect.left) return rect;

    if (currentTool_ == ToolKind::AutoClicker)
    {
        for (const RECT& candidate : { activationKeyButtonRect_, outputButtonButtonRect_, startStopButtonRect_ })
        {
            rect = hit(candidate);
            if (rect.right > rect.left) return rect;
        }
    }
    else if (currentTool_ == ToolKind::FileConverter)
    {
        std::vector<RECT> candidates {
            converterBrowseButtonRect_,
            converterFormatButtonRect_,
            converterConvertButtonRect_,
            converterAdvancedToggleRect_
        };
        if (fileConverterAdvancedOpen_)
        {
            candidates.push_back(converterConflictButtonRect_);
            candidates.push_back(converterJpgBackgroundButtonRect_);
            candidates.push_back(converterCancelButtonRect_);
            candidates.push_back(converterClearButtonRect_);
            candidates.push_back(converterRemoveFailedButtonRect_);
        }
        for (const RECT& candidate : candidates)
        {
            rect = hit(candidate);
            if (rect.right > rect.left) return rect;
        }
    }
    else if (currentTool_ == ToolKind::MediaDownloader)
    {
        std::vector<RECT> candidates {
            mediaAnalyzeButtonRect_,
            mediaClearButtonRect_,
            mediaFormatButtonRect_,
            mediaQualityButtonRect_,
            mediaBrowseButtonRect_,
            mediaDownloadButtonRect_,
            mediaCancelButtonRect_
        };
        if (mediaDownloadJob_.status == MediaDownloadStatus::Complete && !mediaDownloadJob_.outputFilePath.empty())
        {
            candidates.push_back(mediaOpenFileButtonRect_);
            candidates.push_back(mediaOpenFolderButtonRect_);
            candidates.push_back(mediaCopyPathButtonRect_);
        }
        for (const RECT& candidate : candidates)
        {
            rect = hit(candidate);
            if (rect.right > rect.left) return rect;
        }
    }

    return {};
}

void ToolkitApp::UpdateButtonHover(POINT point)
{
    RECT newHover = ButtonRectAtPoint(point);
    const bool hasNewHover = HasArea(newHover);

    if (hasNewHover != hasHoveredButton_ ||
        (hasNewHover && !EqualRect(&newHover, &hoveredButtonRect_)))
    {
        const RECT previousHover = hoveredButtonRect_;
        const bool hadPreviousHover = hasHoveredButton_;
        hoveredButtonRect_ = newHover;
        hasHoveredButton_ = hasNewHover;

        if (hadPreviousHover)
        {
            RECT repaint = PaddedRect(previousHover, Dips(3));
            InvalidateRect(hwnd_, &repaint, FALSE);
        }
        if (hasNewHover)
        {
            RECT repaint = PaddedRect(newHover, Dips(3));
            InvalidateRect(hwnd_, &repaint, FALSE);
        }
    }
}

void ToolkitApp::PaintToolCards(HDC hdc, const std::vector<ToolDefinition>& tools)
{
    RECT clipBox {};
    const int clipType = GetClipBox(hdc, &clipBox);
    if (clipType == NULLREGION)
    {
        return;
    }

    for (size_t index = 0; index < tools.size(); ++index)
    {
        const ToolDefinition& tool = tools[index];
        RECT card = ToolCardRect(index);
        if (clipType != ERROR && !RectsOverlap(card, clipBox))
        {
            continue;
        }

        const bool hovered = hoverToolIndex_ == static_cast<int>(index);
        FillRoundRect(hdc, card, Dips(18), hovered ? kPanelHover : kPanelBackground);
        StrokeRoundRect(hdc, card, Dips(18), hovered ? kAccentSoft : kBorder);

        RECT starRect = ToolFavoriteRect(card);
        PaintFavoriteStar(hdc, starRect, tool.favorite);

        const int iconSize = Dips(70);
        RECT iconRect {
            card.left + ((card.right - card.left) - iconSize) / 2,
            card.top + Dips(34),
            card.left + ((card.right - card.left) + iconSize) / 2,
            card.top + Dips(34) + iconSize
        };
        PaintToolIcon(hdc, tool.kind, iconRect);

        RECT nameRect {
            card.left + Dips(18),
            iconRect.bottom + Dips(20),
            card.right - Dips(18),
            iconRect.bottom + Dips(48)
        };
        DrawTextLine(hdc, tool.name.c_str(), nameRect, navFont_, kTextPrimary, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

        RECT descriptionRect {
            card.left + Dips(20),
            nameRect.bottom + Dips(6),
            card.right - Dips(20),
            card.bottom - Dips(54)
        };
        DrawTextLine(hdc, tool.description.c_str(), descriptionRect, bodyFont_, kTextSecondary, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS);

        RECT openHintRect {
            card.left + Dips(24),
            card.bottom - Dips(42),
            card.right - Dips(24),
            card.bottom - Dips(18)
        };
        DrawTextLine(hdc, L"Open", openHintRect, bodyFont_, hovered ? kTextPrimary : kTextSecondary, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
}

void ToolkitApp::PaintToolIcon(HDC hdc, ToolKind tool, const RECT& bounds)
{
    FillRoundRect(hdc, bounds, Dips(18), RGB(42, 50, 64));
    StrokeRoundRect(hdc, bounds, Dips(18), RGB(58, 70, 92));

    Gdiplus::Bitmap* icon = nullptr;
    if (tool == ToolKind::AutoClicker)
    {
        icon = autoClickerIcon_.get();
    }
    else if (tool == ToolKind::FileConverter)
    {
        icon = fileConverterIcon_.get();
    }
    else if (tool == ToolKind::MediaDownloader)
    {
        icon = mediaDownloaderIcon_.get();
    }

    if (icon)
    {
        RECT iconBounds = ShrinkRect(bounds, Dips(13), Dips(13));
        PaintBitmap(hdc, icon, iconBounds);
        return;
    }

    RECT fallback {
        bounds.left + Dips(22),
        bounds.top + Dips(22),
        bounds.right - Dips(22),
        bounds.bottom - Dips(22)
    };
    FillRoundRect(hdc, fallback, Dips(12), kAccent);
}

void ToolkitApp::PaintFavoriteStar(HDC hdc, const RECT& bounds, bool favorite)
{
    constexpr double pi = 3.14159265358979323846;
    const float centerX = (static_cast<float>(bounds.left) + static_cast<float>(bounds.right)) / 2.0f;
    const float centerY = (static_cast<float>(bounds.top) + static_cast<float>(bounds.bottom)) / 2.0f;
    const float outerRadius = static_cast<float>(std::min(bounds.right - bounds.left, bounds.bottom - bounds.top)) / 2.0f - 2.0f;
    const double innerRadius = outerRadius * 0.46;

    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

    std::array<Gdiplus::PointF, 10> points {};
    for (int i = 0; i < 10; ++i)
    {
        const double radius = (i % 2 == 0) ? outerRadius : innerRadius;
        const double angle = (-pi / 2.0) + (static_cast<double>(i) * pi / 5.0);
        points[static_cast<size_t>(i)] = {
            centerX + static_cast<float>(std::cos(angle) * radius),
            centerY + static_cast<float>(std::sin(angle) * radius)
        };
    }

    Gdiplus::GraphicsPath path;
    path.AddPolygon(points.data(), static_cast<INT>(points.size()));
    path.CloseFigure();

    auto gdiplusColor = [](COLORREF color)
    {
        return Gdiplus::Color(
            static_cast<BYTE>(0xFFu),
            static_cast<BYTE>(color & 0xFFu),
            static_cast<BYTE>((color >> 8) & 0xFFu),
            static_cast<BYTE>((color >> 16) & 0xFFu));
    };

    const COLORREF strokeColor = favorite ? kGold : kTextSecondary;
    if (favorite)
    {
        Gdiplus::SolidBrush fillBrush(gdiplusColor(kGold));
        graphics.FillPath(&fillBrush, &path);
    }

    Gdiplus::Pen strokePen(
        gdiplusColor(strokeColor),
        static_cast<Gdiplus::REAL>(std::max(1, Dips(2))));
    strokePen.SetLineJoin(Gdiplus::LineJoinMiter);
    graphics.DrawPath(&strokePen, &path);
}

void ToolkitApp::OnMouseMove(POINT point)
{
    if (!mouseLeaveTracking_)
    {
        TRACKMOUSEEVENT trackMouseEvent {};
        trackMouseEvent.cbSize = sizeof(trackMouseEvent);
        trackMouseEvent.dwFlags = TME_LEAVE;
        trackMouseEvent.hwndTrack = hwnd_;
        if (TrackMouseEvent(&trackMouseEvent))
        {
            mouseLeaveTracking_ = true;
        }
    }

    if (speedSliderDragging_)
    {
        UpdateAutoClickerSpeedFromPoint(point.x);
        return;
    }

    if (converterQualityDragging_)
    {
        UpdateConverterQualityFromPoint(point.x);
        return;
    }

    if (scrollBarDragging_)
    {
        const int trackHeight = std::max(1, static_cast<int>(scrollBarTrackRect_.bottom - scrollBarTrackRect_.top));
        const int thumbHeight = std::max(1, static_cast<int>(scrollBarThumbRect_.bottom - scrollBarThumbRect_.top));
        const int travel = std::max(1, trackHeight - thumbHeight);
        const int dragY = static_cast<int>(point.y - scrollBarTrackRect_.top) - scrollBarDragOffsetY_;
        const int relativeY = std::clamp(dragY, 0, travel);
        SetScrollOffset(MulDiv(relativeY, maxScrollOffsetY_, travel));
        return;
    }

    UpdateButtonHover(point);

    if (activeDropdown_ != DropdownKind::None)
    {
        int newHoverDropdownIndex = -1;
        if (IsPointInRect(dropdownRect_, point))
        {
            const int itemHeight = Dips(38);
            newHoverDropdownIndex = (point.y - dropdownRect_.top - Dips(5)) / itemHeight;
            if (newHoverDropdownIndex < 0 || newHoverDropdownIndex >= static_cast<int>(dropdownLabels_.size()))
            {
                newHoverDropdownIndex = -1;
            }
        }

        if (newHoverDropdownIndex != hoverDropdownIndex_)
        {
            hoverDropdownIndex_ = newHoverDropdownIndex;
            RECT repaint = PaddedRect(dropdownRect_, Dips(3));
            InvalidateRect(hwnd_, &repaint, FALSE);
        }
    }

    int newHoverIndex = -1;
    if (IsPointInRect(favoritesNavRect_, point))
    {
        newHoverIndex = 0;
    }
    else if (IsPointInRect(allToolsNavRect_, point))
    {
        newHoverIndex = 1;
    }
    else if (IsPointInRect(settingsNavRect_, point))
    {
        newHoverIndex = 2;
    }

    if (newHoverIndex != hoverNavIndex_)
    {
        const int previousHoverIndex = hoverNavIndex_;
        hoverNavIndex_ = newHoverIndex;

        if (previousHoverIndex == 0)
        {
            RECT repaint = PaddedRect(favoritesNavRect_, Dips(3));
            InvalidateRect(hwnd_, &repaint, FALSE);
        }
        else if (previousHoverIndex == 1)
        {
            RECT repaint = PaddedRect(allToolsNavRect_, Dips(3));
            InvalidateRect(hwnd_, &repaint, FALSE);
        }
        else if (previousHoverIndex == 2)
        {
            RECT repaint = PaddedRect(settingsNavRect_, Dips(3));
            InvalidateRect(hwnd_, &repaint, FALSE);
        }
        if (newHoverIndex == 0)
        {
            RECT repaint = PaddedRect(favoritesNavRect_, Dips(3));
            InvalidateRect(hwnd_, &repaint, FALSE);
        }
        else if (newHoverIndex == 1)
        {
            RECT repaint = PaddedRect(allToolsNavRect_, Dips(3));
            InvalidateRect(hwnd_, &repaint, FALSE);
        }
        else if (newHoverIndex == 2)
        {
            RECT repaint = PaddedRect(settingsNavRect_, Dips(3));
            InvalidateRect(hwnd_, &repaint, FALSE);
        }
    }

    int newHoverToolIndex = -1;
    if (currentPage_ == Page::AllTools || currentPage_ == Page::Favorites)
    {
        const auto visibleTools = VisibleToolsForCurrentPage();

        for (size_t index = 0; index < visibleTools.size(); ++index)
        {
            RECT card = ToolCardRect(index);

            if (IsPointInRect(card, point))
            {
                newHoverToolIndex = static_cast<int>(index);
                break;
            }
        }
    }

    if (newHoverToolIndex != hoverToolIndex_)
    {
        const int previousHoverToolIndex = hoverToolIndex_;
        hoverToolIndex_ = newHoverToolIndex;

        if (previousHoverToolIndex >= 0)
        {
            RECT repaint = PaddedRect(ToolCardRect(static_cast<size_t>(previousHoverToolIndex)), Dips(4));
            InvalidateRect(hwnd_, &repaint, FALSE);
        }
        if (newHoverToolIndex >= 0)
        {
            RECT repaint = PaddedRect(ToolCardRect(static_cast<size_t>(newHoverToolIndex)), Dips(4));
            InvalidateRect(hwnd_, &repaint, FALSE);
        }
    }
}

void ToolkitApp::OnLeftButtonDown(POINT point)
{
    if (HandleDropdownClick(point))
    {
        return;
    }

    if (IsScrollBarVisible() && IsPointInRect(scrollBarTrackRect_, point))
    {
        if (IsPointInRect(scrollBarThumbRect_, point))
        {
            scrollBarDragging_ = true;
            scrollBarDragOffsetY_ = point.y - scrollBarThumbRect_.top;
            SetCapture(hwnd_);
        }
        else
        {
            const int viewportHeight = std::max(1, static_cast<int>(contentRect_.bottom - contentRect_.top));
            SetScrollOffset(scrollOffsetY_ + (point.y < scrollBarThumbRect_.top ? -viewportHeight : viewportHeight));
        }
        return;
    }

    pressedButtonRect_ = ButtonRectAtPoint(point);
    hasPressedButton_ = pressedButtonRect_.right > pressedButtonRect_.left && pressedButtonRect_.bottom > pressedButtonRect_.top;
    if (hasPressedButton_)
    {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    if (awaitingActivationKey_)
    {
        OnMouseButtonForActivationBinding(ActivationMouseButton::Left);
        return;
    }

    if (awaitingOutputButton_)
    {
        OnMouseButtonForBinding(OutputMouseButton::Left);
        return;
    }

    if (IsPointInRect(favoritesNavRect_, point))
    {
        SelectPage(Page::Favorites);
        return;
    }
    else if (IsPointInRect(allToolsNavRect_, point))
    {
        SelectPage(Page::AllTools);
        return;
    }
    else if (IsPointInRect(settingsNavRect_, point))
    {
        SelectPage(Page::Settings);
        return;
    }

    if (currentPage_ == Page::Settings)
    {
        if (IsPointInRect(settingsCheckUpdatesButtonRect_, point) && !updateChecking_)
        {
            StartUpdateCheck();
            return;
        }
        if (IsPointInRect(settingsDownloadUpdateButtonRect_, point) &&
            hasUpdateResult_ &&
            updateResult_.status == UpdateCheckStatus::UpdateAvailable)
        {
            OpenUpdateDownloadUrl();
            return;
        }
    }

    if (currentPage_ == Page::Tool && currentTool_ == ToolKind::FileConverter)
    {
        if (IsPointInRect(backButtonRect_, point))
        {
            SelectPage(Page::AllTools);
            return;
        }
        if (IsPointInRect(converterDropZoneRect_, point) || IsPointInRect(converterBrowseButtonRect_, point))
        {
            BrowseConverterFiles();
            return;
        }
        if (IsPointInRect(converterFormatButtonRect_, point))
        {
            ShowOutputFormatDropdown();
            return;
        }
        if (IsPointInRect(converterConvertButtonRect_, point))
        {
            StartFileConversion();
            return;
        }
        if (IsPointInRect(converterAdvancedToggleRect_, point))
        {
            fileConverterAdvancedOpen_ = !fileConverterAdvancedOpen_;
            RecalculateLayout();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (!fileConverterAdvancedOpen_)
        {
            return;
        }
        if (IsPointInRect(converterConflictButtonRect_, point))
        {
            ShowConflictBehaviorDropdown();
            return;
        }
        if (IsPointInRect(converterJpgBackgroundButtonRect_, point))
        {
            if (conversionOptions_.outputFormat == ImageFormat::Jpg)
            {
                ShowJpgBackgroundDropdown();
            }
            else if (conversionOptions_.outputFormat != ImageFormat::Webp)
            {
                ShowFormatOptionsDropdown();
            }
            return;
        }
        if (conversionOptions_.outputFormat == ImageFormat::Jpg ||
            conversionOptions_.outputFormat == ImageFormat::Webp)
        {
            RECT qualityHitRect = converterQualityTrackRect_;
            qualityHitRect.top -= Dips(14);
            qualityHitRect.bottom += Dips(14);
            qualityHitRect.left -= Dips(8);
            qualityHitRect.right += Dips(8);
            if (IsPointInRect(qualityHitRect, point) || IsPointInRect(converterQualityThumbRect_, point))
            {
                converterQualityDragging_ = true;
                SetCapture(hwnd_);
                UpdateConverterQualityFromPoint(point.x);
                return;
            }
        }
        if (IsPointInRect(converterWebpLosslessRect_, point) &&
            conversionOptions_.outputFormat == ImageFormat::Webp)
        {
            ToggleWebpLossless();
            return;
        }
        if (IsPointInRect(converterCancelButtonRect_, point))
        {
            CancelFileConversion();
            return;
        }
        if (IsPointInRect(converterClearButtonRect_, point) && !fileConverterConverting_)
        {
            conversionJobs_.clear();
            selectedConversionJob_ = -1;
            UpdateFileConverterSummary();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (IsPointInRect(converterRemoveFailedButtonRect_, point) && !fileConverterConverting_)
        {
            conversionJobs_.erase(
                std::remove_if(
                    conversionJobs_.begin(),
                    conversionJobs_.end(),
                    [](const ConversionJob& job)
                    {
                        return job.status == ConversionStatus::Failed;
                    }),
                conversionJobs_.end());
            selectedConversionJob_ = -1;
            UpdateFileConverterSummary();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        const int rowHeight = Dips(42);
        int rowTop = converterQueueRect_.top + Dips(46);
        const int availableQueueHeight = static_cast<int>(converterQueueRect_.bottom - rowTop - Dips(16));
        const int maxVisibleRows = std::max(1, availableQueueHeight / rowHeight);
        const int visibleRows = std::min<int>(static_cast<int>(conversionJobs_.size()), maxVisibleRows);
        for (int index = 0; index < visibleRows; ++index)
        {
            RECT row {
                converterQueueRect_.left + Dips(14),
                rowTop,
                converterQueueRect_.right - Dips(14),
                rowTop + rowHeight - Dips(6)
            };
            RECT removeRect { row.right - Dips(38), row.top + Dips(6), row.right - Dips(10), row.bottom - Dips(6) };
            if (IsPointInRect(removeRect, point) && !fileConverterConverting_)
            {
                conversionJobs_.erase(conversionJobs_.begin() + index);
                selectedConversionJob_ = -1;
                UpdateFileConverterSummary();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            if (IsPointInRect(row, point))
            {
                selectedConversionJob_ = index;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            rowTop += rowHeight;
        }
        return;
    }

    if (currentPage_ == Page::Tool && currentTool_ == ToolKind::MediaDownloader)
    {
        if (IsPointInRect(backButtonRect_, point))
        {
            SelectPage(Page::AllTools);
            return;
        }
        if (IsPointInRect(mediaAnalyzeButtonRect_, point) && !mediaDownloading_)
        {
            AnalyzeMediaUrl();
            return;
        }
        if (IsPointInRect(mediaClearButtonRect_, point) && !mediaAnalyzing_ && !mediaDownloading_)
        {
            SetWindowTextW(mediaUrlEdit_, L"");
            SetWindowTextW(mediaFileNameEdit_, L"");
            mediaDownloadJob_ = {};
            mediaStatusText_ = MediaSetupMessage().empty() ? L"Ready." : MediaSetupMessage();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (IsPointInRect(mediaFormatButtonRect_, point))
        {
            ShowMediaFormatDropdown();
            return;
        }
        if (IsPointInRect(mediaQualityButtonRect_, point))
        {
            ShowMediaQualityDropdown();
            return;
        }
        if (IsPointInRect(mediaBrowseButtonRect_, point) && !mediaDownloading_)
        {
            BrowseMediaOutputFolder();
            return;
        }
        if (IsPointInRect(mediaDownloadButtonRect_, point))
        {
            StartMediaDownload();
            return;
        }
        if (IsPointInRect(mediaCancelButtonRect_, point))
        {
            CancelMediaDownload();
            return;
        }
        if (mediaDownloadJob_.status == MediaDownloadStatus::Complete && !mediaDownloadJob_.outputFilePath.empty())
        {
            if (IsPointInRect(mediaOpenFileButtonRect_, point))
            {
                ShellExecuteW(hwnd_, L"open", mediaDownloadJob_.outputFilePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return;
            }
            if (IsPointInRect(mediaOpenFolderButtonRect_, point))
            {
                const std::filesystem::path folder = mediaDownloadJob_.outputFilePath.parent_path();
                ShellExecuteW(hwnd_, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return;
            }
            if (IsPointInRect(mediaCopyPathButtonRect_, point))
            {
                const std::wstring path = mediaDownloadJob_.outputFilePath.wstring();
                if (OpenClipboard(hwnd_))
                {
                    EmptyClipboard();
                    const SIZE_T bytes = (path.size() + 1) * sizeof(wchar_t);
                    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
                    if (memory)
                    {
                        void* data = GlobalLock(memory);
                        if (data)
                        {
                            std::memcpy(data, path.c_str(), bytes);
                            GlobalUnlock(memory);
                            SetClipboardData(CF_UNICODETEXT, memory);
                        }
                        else
                        {
                            GlobalFree(memory);
                        }
                    }
                    CloseClipboard();
                }
                mediaStatusText_ = L"Path copied.";
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
        }
        return;
    }

    if (currentPage_ == Page::Tool && currentTool_ == ToolKind::AutoClicker)
    {
        if (IsPointInRect(backButtonRect_, point))
        {
            SelectPage(Page::AllTools);
            return;
        }

        RECT sliderHitRect = speedSliderTrackRect_;
        sliderHitRect.top -= Dips(14);
        sliderHitRect.bottom += Dips(14);
        sliderHitRect.left -= Dips(8);
        sliderHitRect.right += Dips(8);

        if (IsPointInRect(sliderHitRect, point) || IsPointInRect(speedSliderThumbRect_, point))
        {
            speedSliderDragging_ = true;
            SetCapture(hwnd_);
            UpdateAutoClickerSpeedFromPoint(point.x);
            return;
        }

        if (IsPointInRect(startStopButtonRect_, point))
        {
            testButtonHeld_ = true;
            SetCapture(hwnd_);
            SetAutoClickerRunning(true);
            return;
        }

        if (IsPointInRect(activationKeyButtonRect_, point))
        {
            awaitingActivationKey_ = true;
            awaitingOutputButton_ = false;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }

        if (IsPointInRect(outputButtonButtonRect_, point))
        {
            awaitingOutputButton_ = true;
            awaitingActivationKey_ = false;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
    }

    if (currentPage_ == Page::AllTools || currentPage_ == Page::Favorites)
    {
        const auto visibleTools = VisibleToolsForCurrentPage();

        for (size_t index = 0; index < visibleTools.size(); ++index)
        {
            const ToolDefinition& tool = visibleTools[index];
            RECT card = ToolCardRect(index);

            if (IsPointInRect(card, point))
            {
                if (IsPointInRect(ToolFavoriteRect(card), point))
                {
                    ToggleFavorite(tool.kind);
                    return;
                }

                OpenTool(tool.kind);
                return;
            }
        }
    }
}

void ToolkitApp::OnLeftButtonUp(POINT)
{
    if (scrollBarDragging_)
    {
        scrollBarDragging_ = false;
        ReleaseCapture();
        return;
    }

    if (hasPressedButton_)
    {
        hasPressedButton_ = false;
        pressedButtonRect_ = {};
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    if (speedSliderDragging_)
    {
        speedSliderDragging_ = false;
        ReleaseCapture();
    }

    if (converterQualityDragging_)
    {
        converterQualityDragging_ = false;
        ReleaseCapture();
    }

    if (testButtonHeld_)
    {
        testButtonHeld_ = false;
        ReleaseCapture();
        SetAutoClickerRunning(false);
    }
}

void ToolkitApp::OnMouseWheel(int delta, POINT screenPoint)
{
    POINT clientPoint = screenPoint;
    ScreenToClient(hwnd_, &clientPoint);
    if (!IsPointInRect(contentRect_, clientPoint) || !IsScrollBarVisible())
    {
        return;
    }

    const int scrollStep = Dips(72);
    SetScrollOffset(scrollOffsetY_ - MulDiv(delta, scrollStep, WHEEL_DELTA));
}

void ToolkitApp::AddFilesToConverterQueue(const std::vector<std::filesystem::path>& paths)
{
    if (fileConverterConverting_)
    {
        return;
    }

    int added = 0;
    int skipped = 0;
    int duplicates = 0;

    for (const std::filesystem::path& path : paths)
    {
        const std::filesystem::path absolutePath = std::filesystem::absolute(path);
        const bool exists = std::any_of(
            conversionJobs_.begin(),
            conversionJobs_.end(),
            [&absolutePath](const ConversionJob& job)
            {
                std::error_code errorCode;
                return std::filesystem::equivalent(job.inputPath, absolutePath, errorCode);
            });

        if (exists)
        {
            ++duplicates;
            continue;
        }

        std::wstring warning;
        std::optional<ConversionJob> job = fileConversionService_.CreateJob(absolutePath, warning);
        if (!job)
        {
            ++skipped;
            continue;
        }

        job->outputFormat = conversionOptions_.outputFormat;
        conversionJobs_.push_back(*job);
        ++added;
    }

    std::wostringstream summary;
    if (added > 0)
    {
        summary << L"Added " << added << L" file" << (added == 1 ? L"" : L"s") << L".";
    }
    if (skipped > 0)
    {
        if (summary.tellp() > 0)
        {
            summary << L" ";
        }
        summary << skipped << L" unsupported file" << (skipped == 1 ? L" was" : L"s were") << L" skipped.";
    }
    if (duplicates > 0)
    {
        if (summary.tellp() > 0)
        {
            summary << L" ";
        }
        summary << duplicates << L" duplicate" << (duplicates == 1 ? L" was" : L"s were") << L" ignored.";
    }

    fileConverterSummary_ = summary.str();
    if (fileConverterSummary_.empty())
    {
        UpdateFileConverterSummary();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ToolkitApp::BrowseConverterFiles()
{
    std::vector<wchar_t> buffer(32768, L'\0');

    OPENFILENAMEW openFileName {};
    openFileName.lStructSize = sizeof(openFileName);
    openFileName.hwndOwner = hwnd_;
    openFileName.lpstrFilter =
        L"Supported images (*.webp;*.png;*.jpg;*.jpeg;*.bmp)\0*.webp;*.png;*.jpg;*.jpeg;*.bmp\0"
        L"All files (*.*)\0*.*\0";
    openFileName.lpstrFile = buffer.data();
    openFileName.nMaxFile = static_cast<DWORD>(buffer.size());
    openFileName.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (!GetOpenFileNameW(&openFileName))
    {
        return;
    }

    std::vector<std::filesystem::path> paths;
    const wchar_t* cursor = buffer.data();
    std::filesystem::path first = cursor;
    cursor += first.wstring().size() + 1;

    if (*cursor == L'\0')
    {
        paths.push_back(first);
    }
    else
    {
        while (*cursor != L'\0')
        {
            std::filesystem::path fileName = cursor;
            paths.push_back(first / fileName);
            cursor += fileName.wstring().size() + 1;
        }
    }

    AddFilesToConverterQueue(paths);
}

void ToolkitApp::CreateMediaDownloaderControls()
{
    mediaUrlEdit_ = CreateWindowExW(
        0,
        L"EDIT",
        L"",
        WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
        0,
        0,
        0,
        0,
        hwnd_,
        nullptr,
        instance_,
        nullptr);

    mediaFileNameEdit_ = CreateWindowExW(
        0,
        L"EDIT",
        L"",
        WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
        0,
        0,
        0,
        0,
        hwnd_,
        nullptr,
        instance_,
        nullptr);

    for (HWND edit : { mediaUrlEdit_, mediaFileNameEdit_ })
    {
        if (!edit)
        {
            continue;
        }

        SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(monospaceFont_), TRUE);
        SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(Dips(4), Dips(4)));
    }

    if (mediaUrlEdit_)
    {
        SendMessageW(mediaUrlEdit_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Paste a YouTube or SoundCloud link"));
    }
    if (mediaFileNameEdit_)
    {
        SendMessageW(mediaFileNameEdit_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Optional custom file name"));
    }
}

void ToolkitApp::UpdateMediaDownloaderControls()
{
    const bool visible = currentPage_ == Page::Tool && currentTool_ == ToolKind::MediaDownloader;
    const bool hideForDropdown = activeDropdown_ == DropdownKind::MediaFormat ||
        activeDropdown_ == DropdownKind::MediaQuality;

    auto moveEdit = [&](HWND edit, const RECT& rect, bool enabled)
    {
        if (!edit)
        {
            return;
        }

        const bool inViewport = visible && !hideForDropdown &&
            rect.top >= contentRect_.top &&
            rect.bottom <= contentRect_.bottom &&
            RectsOverlap(rect, contentRect_);
        ShowWindow(edit, inViewport ? SW_SHOW : SW_HIDE);
        EnableWindow(edit, inViewport && enabled);
        if (!inViewport)
        {
            return;
        }

        const int editWidth = std::max(1, static_cast<int>(rect.right - rect.left) - Dips(20));
        const int editHeight = std::max(1, Dips(24));
        const int editTop = static_cast<int>(rect.top) + ((rect.bottom - rect.top) - editHeight) / 2;

        SetWindowPos(
            edit,
            nullptr,
            static_cast<int>(rect.left) + Dips(10),
            editTop,
            editWidth,
            editHeight,
            SWP_NOZORDER | SWP_NOACTIVATE);
    };

    moveEdit(mediaUrlEdit_, mediaUrlEditRect_, !mediaAnalyzing_ && !mediaDownloading_);
    moveEdit(mediaFileNameEdit_, mediaFileNameEditRect_, !mediaDownloading_);
}

std::optional<std::filesystem::path> ToolkitApp::PromptForSingleConverterOutputPath(const ConversionJob& job) const
{
    const std::wstring extension = SupportedFormatRegistry::ExtensionFor(conversionOptions_.outputFormat);
    const std::wstring defaultName = job.inputPath.stem().wstring() + extension;
    const std::filesystem::path defaultPath = job.inputPath.has_parent_path()
        ? job.inputPath.parent_path() / defaultName
        : std::filesystem::path(defaultName);

    std::array<wchar_t, 32768> buffer {};
    wcsncpy_s(buffer.data(), buffer.size(), defaultPath.wstring().c_str(), _TRUNCATE);

    const std::wstring formatLabel = SupportedFormatRegistry::LabelFor(conversionOptions_.outputFormat);
    std::wstring filter;
    filter.reserve(96);
    filter += formatLabel + L" image (*" + extension + L")";
    filter.push_back(L'\0');
    filter += L"*" + extension;
    filter.push_back(L'\0');
    filter += L"All files (*.*)";
    filter.push_back(L'\0');
    filter += L"*.*";
    filter.push_back(L'\0');
    filter.push_back(L'\0');

    const std::wstring defaultExtension = extension.size() > 1 ? extension.substr(1) : extension;

    OPENFILENAMEW saveFileName {};
    saveFileName.lStructSize = sizeof(saveFileName);
    saveFileName.hwndOwner = hwnd_;
    saveFileName.lpstrFile = buffer.data();
    saveFileName.nMaxFile = static_cast<DWORD>(buffer.size());
    saveFileName.lpstrFilter = filter.c_str();
    saveFileName.nFilterIndex = 1;
    saveFileName.lpstrDefExt = defaultExtension.c_str();
    saveFileName.lpstrTitle = L"Save converted file";
    saveFileName.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetSaveFileNameW(&saveFileName))
    {
        return std::nullopt;
    }

    std::filesystem::path outputPath = buffer.data();
    if (!outputPath.has_extension())
    {
        outputPath += extension;
    }

    return outputPath;
}

std::optional<std::filesystem::path> ToolkitApp::PromptForBatchConverterOutputFolder() const
{
    BROWSEINFOW browseInfo {};
    browseInfo.hwndOwner = hwnd_;
    browseInfo.lpszTitle = L"Choose where to save converted files";
    browseInfo.ulFlags = BIF_RETURNONLYFSDIRS | BIF_USENEWUI;

    PIDLIST_ABSOLUTE itemList = SHBrowseForFolderW(&browseInfo);
    if (!itemList)
    {
        return std::nullopt;
    }

    std::optional<std::filesystem::path> folder;
    wchar_t path[MAX_PATH] {};
    if (SHGetPathFromIDListW(itemList, path))
    {
        folder = std::filesystem::path(path);
    }

    CoTaskMemFree(itemList);
    return folder;
}

void ToolkitApp::StartFileConversion()
{
    if (fileConverterConverting_ || conversionJobs_.empty())
    {
        return;
    }

    FinishConversionThread();
    conversionCancelRequested_ = false;

    std::vector<ConversionJob> jobs = conversionJobs_;
    ConversionOptions options = conversionOptions_;

    for (ConversionJob& job : jobs)
    {
        if (job.status != ConversionStatus::Failed || job.errorMessage.empty())
        {
            job.status = ConversionStatus::Pending;
            job.errorMessage.clear();
            job.outputPath.clear();
        }
        job.outputFormat = conversionOptions_.outputFormat;
    }

    if (jobs.size() == 1)
    {
        const auto outputPath = PromptForSingleConverterOutputPath(jobs.front());
        if (!outputPath)
        {
            fileConverterSummary_ = L"Save cancelled.";
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }

        jobs.front().outputPath = *outputPath;
        options.conflictBehavior = ConflictBehavior::Overwrite;
    }
    else
    {
        const auto outputFolder = PromptForBatchConverterOutputFolder();
        if (!outputFolder)
        {
            fileConverterSummary_ = L"Save cancelled.";
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }

        options.outputDirectoryMode = OutputDirectoryMode::ChosenFolder;
        options.outputDirectory = *outputFolder;
    }

    conversionJobs_ = jobs;
    fileConverterConverting_ = true;

    HWND hwnd = hwnd_;

    conversionThread_ = std::thread(
        [this, hwnd, jobs, options]()
        {
            fileConversionService_.ConvertBatch(
                jobs,
                options,
                conversionCancelRequested_,
                [hwnd](const ConversionResult& result)
                {
                    auto* heapResult = new ConversionResult(result);
                    PostMessageW(hwnd, kConversionProgressMessage, 0, reinterpret_cast<LPARAM>(heapResult));
                });

            PostMessageW(hwnd, kConversionFinishedMessage, 0, 0);
        });

    fileConverterSummary_ = L"Converting...";
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ToolkitApp::CancelFileConversion()
{
    if (!fileConverterConverting_)
    {
        return;
    }

    conversionCancelRequested_ = true;
    fileConverterSummary_ = L"Cancelling...";
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ToolkitApp::FinishConversionThread()
{
    if (conversionThread_.joinable() && conversionThread_.get_id() != std::this_thread::get_id())
    {
        conversionThread_.join();
    }
}

void ToolkitApp::ApplyConversionResult(const ConversionResult& result)
{
    if (result.index >= conversionJobs_.size())
    {
        return;
    }

    conversionJobs_[result.index] = result.job;
    UpdateFileConverterSummary();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ToolkitApp::BrowseMediaOutputFolder()
{
    BROWSEINFOW browseInfo {};
    browseInfo.hwndOwner = hwnd_;
    browseInfo.lpszTitle = L"Choose where to save downloads";
    browseInfo.ulFlags = BIF_RETURNONLYFSDIRS | BIF_USENEWUI;

    PIDLIST_ABSOLUTE itemList = SHBrowseForFolderW(&browseInfo);
    if (!itemList)
    {
        return;
    }

    wchar_t path[MAX_PATH] {};
    if (SHGetPathFromIDListW(itemList, path))
    {
        mediaDownloadOptions_.outputFolder = path;
        mediaDownloadJob_.outputFolder = mediaDownloadOptions_.outputFolder;
        mediaStatusText_ = L"Save location selected.";
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    CoTaskMemFree(itemList);
}

void ToolkitApp::AnalyzeMediaUrl()
{
    if (mediaAnalyzing_ || mediaDownloading_)
    {
        return;
    }

    const std::wstring url = GetWindowTextString(mediaUrlEdit_);
    if (!SupportedPlatformRegistry::IsSupportedUrl(url))
    {
        mediaDownloadJob_.status = MediaDownloadStatus::Failed;
        mediaDownloadJob_.errorMessage = L"Unsupported URL. Please enter a YouTube or SoundCloud link.";
        mediaStatusText_ = mediaDownloadJob_.errorMessage;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    mediaExternalTools_ = mediaDownloadService_.CheckExternalTools();
    const std::wstring setupMessage = MediaSetupMessage();
    if (!setupMessage.empty())
    {
        mediaDownloadJob_.status = MediaDownloadStatus::Failed;
        mediaDownloadJob_.errorMessage = setupMessage;
        mediaStatusText_ = setupMessage;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    FinishMediaThread();
    mediaCancelRequested_ = false;
    mediaAnalyzing_ = true;
    mediaDownloadJob_ = {};
    mediaDownloadJob_.url = url;
    mediaDownloadJob_.platform = SupportedPlatformRegistry::DetectPlatform(url);
    mediaDownloadJob_.status = MediaDownloadStatus::Analyzing;
    mediaStatusText_ = L"Analyzing...";
    UpdateMediaDownloaderControls();
    InvalidateRect(hwnd_, nullptr, FALSE);

    HWND hwnd = hwnd_;
    mediaThread_ = std::thread(
        [this, hwnd, url]()
        {
            std::wstring errorMessage;
            auto job = mediaDownloadService_.Analyze(url, mediaCancelRequested_, errorMessage);
            MediaDownloadJob resultJob;
            if (job)
            {
                resultJob = *job;
            }
            else
            {
                resultJob.url = url;
                resultJob.platform = SupportedPlatformRegistry::DetectPlatform(url);
                resultJob.status = mediaCancelRequested_.load() ? MediaDownloadStatus::Cancelled : MediaDownloadStatus::Failed;
                resultJob.errorMessage = errorMessage.empty() ? L"Metadata unavailable." : errorMessage;
            }

            auto* heapJob = new MediaDownloadJob(resultJob);
            PostMessageW(hwnd, kMediaJobUpdateMessage, 0, reinterpret_cast<LPARAM>(heapJob));
            PostMessageW(hwnd, kMediaFinishedMessage, 0, 0);
        });
}

void ToolkitApp::StartMediaDownload()
{
    if (!CanStartMediaDownload())
    {
        if (mediaStatusText_.empty())
        {
            mediaStatusText_ = L"Enter a supported URL and choose a valid save location first.";
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }

    mediaExternalTools_ = mediaDownloadService_.CheckExternalTools();
    const std::wstring setupMessage = MediaSetupMessage();
    if (!setupMessage.empty())
    {
        mediaDownloadJob_.status = MediaDownloadStatus::Failed;
        mediaDownloadJob_.errorMessage = setupMessage;
        mediaStatusText_ = setupMessage;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    FinishMediaThread();
    mediaCancelRequested_ = false;
    mediaDownloading_ = true;

    MediaDownloadJob job = mediaDownloadJob_;
    job.url = GetWindowTextString(mediaUrlEdit_);
    if (job.title.empty())
    {
        job.title = L"media";
    }
    job.platform = SupportedPlatformRegistry::DetectPlatform(job.url);
    job.status = MediaDownloadStatus::Downloading;
    job.progress = 0.0;
    job.errorMessage.clear();

    MediaDownloadOptions options = mediaDownloadOptions_;
    options.customFileName = MediaDownloadService::SanitizeFileName(GetWindowTextString(mediaFileNameEdit_));
    if (options.customFileName == L"media" && GetWindowTextString(mediaFileNameEdit_).empty())
    {
        options.customFileName.clear();
    }

    mediaDownloadJob_ = job;
    mediaStatusText_ = L"Downloading...";
    UpdateMediaDownloaderControls();
    InvalidateRect(hwnd_, nullptr, FALSE);

    HWND hwnd = hwnd_;
    mediaThread_ = std::thread(
        [this, hwnd, job, options]()
        {
            MediaDownloadJob finalJob = mediaDownloadService_.Download(
                job,
                options,
                mediaCancelRequested_,
                [hwnd](const MediaDownloadJob& progressJob)
                {
                    auto* heapJob = new MediaDownloadJob(progressJob);
                    PostMessageW(hwnd, kMediaJobUpdateMessage, 0, reinterpret_cast<LPARAM>(heapJob));
                });

            auto* heapJob = new MediaDownloadJob(finalJob);
            PostMessageW(hwnd, kMediaJobUpdateMessage, 0, reinterpret_cast<LPARAM>(heapJob));
            PostMessageW(hwnd, kMediaFinishedMessage, 0, 0);
        });
}

void ToolkitApp::CancelMediaDownload()
{
    if (!mediaAnalyzing_ && !mediaDownloading_)
    {
        return;
    }

    mediaCancelRequested_ = true;
    mediaStatusText_ = L"Cancelling...";
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ToolkitApp::FinishMediaThread()
{
    if (mediaThread_.joinable() && mediaThread_.get_id() != std::this_thread::get_id())
    {
        mediaThread_.join();
    }
}

void ToolkitApp::ApplyMediaJobUpdate(const MediaDownloadJob& job)
{
    const bool wasAnalyzing = mediaAnalyzing_;
    mediaDownloadJob_ = job;

    if (wasAnalyzing && job.status == MediaDownloadStatus::Ready)
    {
        mediaDownloadOptions_.outputFormat =
            (job.platform == MediaPlatform::SoundCloud || job.mediaType == MediaType::Audio)
            ? MediaOutputFormat::Mp3
            : MediaOutputFormat::Mp4;
        SetWindowTextIfChanged(mediaFileNameEdit_, MediaDownloadService::SanitizeFileName(job.title));
        mediaStatusText_ = L"Analysis complete.";
    }
    else if (job.status == MediaDownloadStatus::Complete)
    {
        mediaStatusText_ = L"Download complete.";
    }
    else if (job.status == MediaDownloadStatus::Failed)
    {
        mediaStatusText_ = job.errorMessage.empty() ? L"Download failed." : job.errorMessage;
    }
    else if (job.status == MediaDownloadStatus::Cancelled)
    {
        mediaStatusText_ = L"Cancelled.";
    }
    else
    {
        mediaStatusText_ = MediaDownloadService::StatusLabel(job.status) + L"...";
    }

    UpdateMediaDownloaderControls();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ToolkitApp::StartUpdateCheck()
{
    if (updateChecking_)
    {
        return;
    }

    FinishUpdateThread();
    updateChecking_ = true;
    hasUpdateResult_ = false;
    updateResult_ = {};
    updateResult_.currentVersion = APP_VERSION;
    InvalidateRect(hwnd_, nullptr, FALSE);

    HWND hwnd = hwnd_;
    updateThread_ = std::thread(
        [this, hwnd]()
        {
            UpdateCheckResult result = updateChecker_.CheckForUpdates(APP_VERSION, UPDATE_MANIFEST_URL);
            auto* heapResult = new UpdateCheckResult(std::move(result));
            PostMessageW(hwnd, kUpdateCheckFinishedMessage, 0, reinterpret_cast<LPARAM>(heapResult));
        });
}

void ToolkitApp::FinishUpdateThread()
{
    if (updateThread_.joinable() && updateThread_.get_id() != std::this_thread::get_id())
    {
        updateThread_.join();
    }
}

void ToolkitApp::ApplyUpdateCheckResult(const UpdateCheckResult& result)
{
    updateResult_ = result;
    updateChecking_ = false;
    hasUpdateResult_ = true;
}

void ToolkitApp::OpenUpdateDownloadUrl()
{
    if (!hasUpdateResult_ ||
        updateResult_.status != UpdateCheckStatus::UpdateAvailable ||
        !UpdateChecker::IsSafeHttpUrl(updateResult_.downloadUrl))
    {
        return;
    }

    ShellExecuteW(hwnd_, L"open", updateResult_.downloadUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void ToolkitApp::ShowMediaFormatDropdown()
{
    if (mediaDownloading_)
    {
        return;
    }

    const bool audioOnly = mediaDownloadJob_.platform == MediaPlatform::SoundCloud ||
        (mediaDownloadJob_.platform != MediaPlatform::YouTube && mediaDownloadJob_.mediaType == MediaType::Audio);

    OpenDropdown(
        DropdownKind::MediaFormat,
        mediaFormatButtonRect_,
        { L"MP4", L"MP3", L"WAV" },
        { static_cast<int>(MediaOutputFormat::Mp4), static_cast<int>(MediaOutputFormat::Mp3), static_cast<int>(MediaOutputFormat::Wav) },
        { !audioOnly, true, true },
        static_cast<int>(mediaDownloadOptions_.outputFormat));
}

void ToolkitApp::ShowMediaQualityDropdown()
{
    if (mediaDownloading_ || mediaDownloadOptions_.outputFormat == MediaOutputFormat::Wav)
    {
        return;
    }

    if (mediaDownloadOptions_.outputFormat == MediaOutputFormat::Mp4)
    {
        std::array<Mp4Quality, 4> qualities { Mp4Quality::Best, Mp4Quality::P1080, Mp4Quality::P720, Mp4Quality::P480 };
        OpenDropdown(
            DropdownKind::MediaQuality,
            mediaQualityButtonRect_,
            {
                MediaDownloadService::Mp4QualityLabel(qualities[0]),
                MediaDownloadService::Mp4QualityLabel(qualities[1]),
                MediaDownloadService::Mp4QualityLabel(qualities[2]),
                MediaDownloadService::Mp4QualityLabel(qualities[3])
            },
            { static_cast<int>(qualities[0]), static_cast<int>(qualities[1]), static_cast<int>(qualities[2]), static_cast<int>(qualities[3]) },
            { true, true, true, true },
            static_cast<int>(mediaDownloadOptions_.mp4Quality));
    }
    else
    {
        std::array<Mp3Bitrate, 4> bitrates { Mp3Bitrate::K320, Mp3Bitrate::K256, Mp3Bitrate::K192, Mp3Bitrate::K128 };
        OpenDropdown(
            DropdownKind::MediaQuality,
            mediaQualityButtonRect_,
            {
                MediaDownloadService::Mp3BitrateLabel(bitrates[0]),
                MediaDownloadService::Mp3BitrateLabel(bitrates[1]),
                MediaDownloadService::Mp3BitrateLabel(bitrates[2]),
                MediaDownloadService::Mp3BitrateLabel(bitrates[3])
            },
            { static_cast<int>(bitrates[0]), static_cast<int>(bitrates[1]), static_cast<int>(bitrates[2]), static_cast<int>(bitrates[3]) },
            { true, true, true, true },
            static_cast<int>(mediaDownloadOptions_.mp3Bitrate));
    }
}

void ToolkitApp::UpdateFileConverterSummary()
{
    if (conversionJobs_.empty())
    {
        fileConverterSummary_ = L"Ready.";
        return;
    }

    int complete = 0;
    int failed = 0;
    int skipped = 0;
    int converting = 0;
    for (const ConversionJob& job : conversionJobs_)
    {
        if (job.status == ConversionStatus::Complete)
        {
            ++complete;
        }
        else if (job.status == ConversionStatus::Failed)
        {
            ++failed;
        }
        else if (job.status == ConversionStatus::Skipped)
        {
            ++skipped;
        }
        else if (job.status == ConversionStatus::Converting)
        {
            ++converting;
        }
    }

    std::wostringstream summary;
    if (fileConverterConverting_ || converting > 0)
    {
        summary << L"Converting... ";
    }
    summary << L"Converted " << complete << L" successfully.";
    if (failed > 0)
    {
        summary << L" " << failed << L" failed.";
    }
    if (skipped > 0)
    {
        summary << L" " << skipped << L" skipped.";
    }
    fileConverterSummary_ = summary.str();
}

void ToolkitApp::ShowOutputFormatDropdown()
{
    if (supportedOutputFormats_.empty() || fileConverterConverting_)
    {
        return;
    }

    std::vector<std::wstring> labels;
    std::vector<int> values;
    std::vector<bool> enabled;
    for (ImageFormat format : supportedOutputFormats_)
    {
        labels.push_back(OutputFormatLabel(format));
        values.push_back(static_cast<int>(format));
        enabled.push_back(true);
    }

    OpenDropdown(
        DropdownKind::FileOutputFormat,
        converterFormatButtonRect_,
        labels,
        values,
        enabled,
        static_cast<int>(conversionOptions_.outputFormat));
}

void ToolkitApp::ShowConflictBehaviorDropdown()
{
    if (fileConverterConverting_)
    {
        return;
    }

    OpenDropdown(
        DropdownKind::FileConflictBehavior,
        converterConflictButtonRect_,
        { L"Auto-rename", L"Overwrite", L"Skip" },
        {
            static_cast<int>(ConflictBehavior::AutoRename),
            static_cast<int>(ConflictBehavior::Overwrite),
            static_cast<int>(ConflictBehavior::Skip)
        },
        { true, true, true },
        static_cast<int>(conversionOptions_.conflictBehavior));
}

void ToolkitApp::ShowJpgBackgroundDropdown()
{
    if (fileConverterConverting_ || conversionOptions_.outputFormat != ImageFormat::Jpg)
    {
        return;
    }

    OpenDropdown(
        DropdownKind::FileJpgBackground,
        converterJpgBackgroundButtonRect_,
        { L"White", L"Black" },
        {
            static_cast<int>(JpgBackground::White),
            static_cast<int>(JpgBackground::Black)
        },
        { true, true },
        static_cast<int>(conversionOptions_.jpgBackground));
}

void ToolkitApp::ShowFormatOptionsDropdown()
{
    if (fileConverterConverting_ || conversionOptions_.outputFormat == ImageFormat::Jpg ||
        conversionOptions_.outputFormat == ImageFormat::Webp)
    {
        return;
    }

    OpenDropdown(
        DropdownKind::FileFormatOptions,
        converterJpgBackgroundButtonRect_,
        { L"Preserve alpha" },
        { 0 },
        { true },
        0);
}

void ToolkitApp::CycleConflictBehavior()
{
    if (fileConverterConverting_)
    {
        return;
    }

    if (conversionOptions_.conflictBehavior == ConflictBehavior::AutoRename)
    {
        conversionOptions_.conflictBehavior = ConflictBehavior::Overwrite;
    }
    else if (conversionOptions_.conflictBehavior == ConflictBehavior::Overwrite)
    {
        conversionOptions_.conflictBehavior = ConflictBehavior::Skip;
    }
    else
    {
        conversionOptions_.conflictBehavior = ConflictBehavior::AutoRename;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ToolkitApp::CycleJpgBackground()
{
    conversionOptions_.jpgBackground =
        conversionOptions_.jpgBackground == JpgBackground::White ? JpgBackground::Black : JpgBackground::White;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ToolkitApp::ToggleWebpLossless()
{
    conversionOptions_.webpLossless = !conversionOptions_.webpLossless;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ToolkitApp::UpdateConverterQualityFromPoint(int x)
{
    const int trackLeft = static_cast<int>(converterQualityTrackRect_.left);
    const int trackRight = static_cast<int>(converterQualityTrackRect_.right);
    const int trackWidth = std::max(1, trackRight - trackLeft);
    const int clampedX = std::clamp(x, trackLeft, trackRight);
    const int relativeX = clampedX - trackLeft;
    const int quality = 1 + MulDiv(relativeX, 99, trackWidth);

    if (conversionOptions_.outputFormat == ImageFormat::Webp)
    {
        conversionOptions_.webpQuality = std::clamp(quality, 1, 100);
    }
    else if (conversionOptions_.outputFormat == ImageFormat::Jpg)
    {
        conversionOptions_.jpgQuality = std::clamp(quality, 1, 100);
    }

    RecalculateLayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ToolkitApp::OnMouseButtonForBinding(OutputMouseButton button)
{
    if (!awaitingOutputButton_)
    {
        return;
    }

    autoClicker_.outputButton = button;
    autoClickerOutputButton_.store(static_cast<int>(button));
    awaitingOutputButton_ = false;
    SaveAutoClickerSettings();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ToolkitApp::OnMouseButtonForActivationBinding(ActivationMouseButton button)
{
    if (!awaitingActivationKey_)
    {
        return;
    }

    SetActivationMouseButton(button);
    awaitingActivationKey_ = false;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ToolkitApp::UpdateAutoClickerSpeedFromPoint(int x)
{
    const int trackLeft = static_cast<int>(speedSliderTrackRect_.left);
    const int trackRight = static_cast<int>(speedSliderTrackRect_.right);
    const int trackWidth = std::max(1, trackRight - trackLeft);
    const int clampedX = std::clamp(x, trackLeft, trackRight);
    const int relativeX = clampedX - trackLeft;
    const int range = kMaxClicksPerSecond - kMinClicksPerSecond;
    const int newSpeed = kMinClicksPerSecond + MulDiv(relativeX, range, trackWidth);

    if (newSpeed == autoClicker_.clicksPerSecond)
    {
        return;
    }

    autoClicker_.clicksPerSecond = std::clamp(newSpeed, kMinClicksPerSecond, kMaxClicksPerSecond);
    SaveAutoClickerSettings();
    RecalculateLayout();
    UpdateAutoClickerTimer();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ToolkitApp::SetAutoClickerRunning(bool running)
{
    if (autoClicker_.running == running)
    {
        return;
    }

    autoClicker_.running = running;
    UpdateAutoClickerTimer();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ToolkitApp::UpdateAutoClickerTimer()
{
    autoClickerCps_.store(autoClicker_.clicksPerSecond);
    autoClickerOutputButton_.store(static_cast<int>(autoClicker_.outputButton));

    if (!autoClicker_.running)
    {
        autoClickThreadStop_.store(true);
        autoClickCondition_.notify_all();
        if (autoClickThread_.joinable())
        {
            autoClickThread_.join();
        }
        return;
    }

    if (autoClickThread_.joinable())
    {
        autoClickCondition_.notify_all();
        return;
    }

    autoClickThreadStop_.store(false);
    autoClickThread_ = std::thread(&ToolkitApp::AutoClickerLoop, this);
}

void ToolkitApp::AutoClickerLoop()
{
    using Clock = std::chrono::steady_clock;

    const bool timerResolutionRaised = timeBeginPeriod(1) == TIMERR_NOERROR;
    auto nextClick = Clock::now();
    std::unique_lock<std::mutex> lock(autoClickMutex_);

    while (!autoClickThreadStop_.load())
    {
        const int clicksPerSecond = std::clamp(autoClickerCps_.load(), kMinClicksPerSecond, kMaxClicksPerSecond);
        const auto interval = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(1.0 / static_cast<double>(clicksPerSecond)));
        nextClick += std::max(interval, std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(1)));

        if (autoClickCondition_.wait_until(lock, nextClick, [this]
            {
                return autoClickThreadStop_.load();
            }))
        {
            break;
        }

        lock.unlock();
        PerformAutoClick();
        lock.lock();

        const auto now = Clock::now();
        if (nextClick + std::chrono::milliseconds(100) < now)
        {
            nextClick = now;
        }
    }

    if (timerResolutionRaised)
    {
        timeEndPeriod(1);
    }
}

void ToolkitApp::PerformAutoClick()
{
    if (autoClickThreadStop_.load())
    {
        return;
    }

    const auto outputButton = static_cast<OutputMouseButton>(autoClickerOutputButton_.load());
    INPUT inputs[2] {};
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dwFlags = MouseDownFlag(outputButton);
    inputs[0].mi.dwExtraInfo = kAutoClickExtraInfo;
    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dwFlags = MouseUpFlag(outputButton);
    inputs[1].mi.dwExtraInfo = kAutoClickExtraInfo;

    SendInput(static_cast<UINT>(std::size(inputs)), inputs, sizeof(INPUT));
}

void ToolkitApp::SetActivationKey(UINT virtualKey)
{
    autoClicker_.activationKind = ActivationInputKind::Keyboard;
    autoClicker_.activationKey = virtualKey;
    SaveAutoClickerSettings();
}

void ToolkitApp::SetActivationMouseButton(ActivationMouseButton button)
{
    autoClicker_.activationKind = ActivationInputKind::MouseButton;
    autoClicker_.activationMouseButton = button;
    SaveAutoClickerSettings();
}

void ToolkitApp::InstallInputHooks()
{
    RemoveInputHooks();

    g_activeApp = this;
    keyboardHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, ToolkitApp::KeyboardHookProc, instance_, 0);
    mouseHook_ = SetWindowsHookExW(WH_MOUSE_LL, ToolkitApp::MouseHookProc, instance_, 0);
}

void ToolkitApp::RemoveInputHooks()
{
    if (keyboardHook_)
    {
        UnhookWindowsHookEx(keyboardHook_);
        keyboardHook_ = nullptr;
    }

    if (mouseHook_)
    {
        UnhookWindowsHookEx(mouseHook_);
        mouseHook_ = nullptr;
    }
}

bool ToolkitApp::HandleKeyboardHook(WPARAM message, const KBDLLHOOKSTRUCT& keyboard)
{
    if (keyboard.flags & LLKHF_INJECTED)
    {
        return false;
    }

    if (awaitingActivationKey_ &&
        (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) &&
        keyboard.vkCode != VK_ESCAPE)
    {
        awaitingActivationKey_ = false;
        SetActivationKey(static_cast<UINT>(keyboard.vkCode));
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }

    if (autoClicker_.activationKind != ActivationInputKind::Keyboard ||
        keyboard.vkCode != autoClicker_.activationKey)
    {
        return false;
    }

    if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN)
    {
        SetAutoClickerRunning(true);
        return true;
    }
    else if (message == WM_KEYUP || message == WM_SYSKEYUP)
    {
        SetAutoClickerRunning(false);
        return true;
    }

    return false;
}

bool ToolkitApp::HandleMouseHook(WPARAM message, const MSLLHOOKSTRUCT& mouse)
{
    if (mouse.dwExtraInfo == kAutoClickExtraInfo)
    {
        return false;
    }

    if (awaitingActivationKey_)
    {
        if (message == WM_LBUTTONDOWN)
        {
            OnMouseButtonForActivationBinding(ActivationMouseButton::Left);
            return true;
        }
        if (message == WM_RBUTTONDOWN)
        {
            OnMouseButtonForActivationBinding(ActivationMouseButton::Right);
            return true;
        }
        if (message == WM_MBUTTONDOWN)
        {
            OnMouseButtonForActivationBinding(ActivationMouseButton::Middle);
            return true;
        }
        if (message == WM_XBUTTONDOWN)
        {
            const WORD button = HIWORD(mouse.mouseData);
            OnMouseButtonForActivationBinding(button == XBUTTON2 ? ActivationMouseButton::X2 : ActivationMouseButton::X1);
            return true;
        }
    }

    if (autoClicker_.activationKind != ActivationInputKind::MouseButton)
    {
        return false;
    }

    if (IsActivationMouseMessage(message, mouse, true))
    {
        SetAutoClickerRunning(true);
        return true;
    }
    else if (IsActivationMouseMessage(message, mouse, false))
    {
        SetAutoClickerRunning(false);
        return true;
    }

    return false;
}

bool ToolkitApp::IsActivationMouseMessage(WPARAM message, const MSLLHOOKSTRUCT& mouse, bool down) const
{
    switch (autoClicker_.activationMouseButton)
    {
    case ActivationMouseButton::Left:
        return message == (down ? WM_LBUTTONDOWN : WM_LBUTTONUP);
    case ActivationMouseButton::Right:
        return message == (down ? WM_RBUTTONDOWN : WM_RBUTTONUP);
    case ActivationMouseButton::Middle:
        return message == (down ? WM_MBUTTONDOWN : WM_MBUTTONUP);
    case ActivationMouseButton::X1:
        return message == (down ? WM_XBUTTONDOWN : WM_XBUTTONUP) && HIWORD(mouse.mouseData) == XBUTTON1;
    case ActivationMouseButton::X2:
        return message == (down ? WM_XBUTTONDOWN : WM_XBUTTONUP) && HIWORD(mouse.mouseData) == XBUTTON2;
    }

    return false;
}

std::vector<ToolDefinition> ToolkitApp::VisibleToolsForCurrentPage() const
{
    if (currentPage_ == Page::AllTools)
    {
        return tools_;
    }

    std::vector<ToolDefinition> favorites;
    std::copy_if(
        tools_.begin(),
        tools_.end(),
        std::back_inserter(favorites),
        [](const ToolDefinition& tool)
        {
            return tool.favorite;
        });
    return favorites;
}

const ToolDefinition* ToolkitApp::FindTool(ToolKind tool) const
{
    const auto found = std::find_if(
        tools_.begin(),
        tools_.end(),
        [tool](const ToolDefinition& definition)
        {
            return definition.kind == tool;
        });

    if (found == tools_.end())
    {
        return nullptr;
    }

    return &(*found);
}

RECT ToolkitApp::ToolCardRect(size_t index) const
{
    const int columns = 3;
    const int margin = Dips(42);
    const int gap = Dips(18);
    const int top = contentRect_.top + Dips(124) - scrollOffsetY_;
    const int availableWidth = std::max(1, static_cast<int>(contentRect_.right - contentRect_.left) - (margin * 2));
    const int cardSize = std::max(Dips(160), (availableWidth - (gap * (columns - 1))) / columns);
    const int column = static_cast<int>(index % columns);
    const int row = static_cast<int>(index / columns);
    const int left = contentRect_.left + margin + (column * (cardSize + gap));
    const int cardTop = top + (row * (cardSize + gap));

    return {
        left,
        cardTop,
        left + cardSize,
        cardTop + cardSize
    };
}

RECT ToolkitApp::ToolFavoriteRect(const RECT& card) const
{
    return {
        card.left + Dips(16),
        card.top + Dips(14),
        card.left + Dips(44),
        card.top + Dips(42)
    };
}

void ToolkitApp::ToggleFavorite(ToolKind tool)
{
    const auto found = std::find_if(
        tools_.begin(),
        tools_.end(),
        [tool](const ToolDefinition& definition)
        {
            return definition.kind == tool;
        });

    if (found == tools_.end())
    {
        return;
    }

    found->favorite = !found->favorite;
    SaveFavorites();
    hoverToolIndex_ = -1;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

std::wstring ToolkitApp::ActivationKeyLabel() const
{
    if (autoClicker_.activationKind == ActivationInputKind::MouseButton)
    {
        return ActivationMouseButtonLabel(autoClicker_.activationMouseButton);
    }

    return KeyLabel(autoClicker_.activationKey);
}

std::wstring ToolkitApp::OutputButtonLabel() const
{
    return MouseButtonLabel(autoClicker_.outputButton);
}

std::wstring ToolkitApp::StatusLabel(ConversionStatus status) const
{
    switch (status)
    {
    case ConversionStatus::Pending:
        return L"Pending";
    case ConversionStatus::Converting:
        return L"Converting";
    case ConversionStatus::Complete:
        return L"Complete";
    case ConversionStatus::Failed:
        return L"Failed";
    case ConversionStatus::Skipped:
        return L"Skipped";
    }

    return L"Pending";
}

std::wstring ToolkitApp::OutputFormatLabel(ImageFormat format) const
{
    return SupportedFormatRegistry::LabelFor(format);
}

std::wstring ToolkitApp::ConflictBehaviorLabel() const
{
    switch (conversionOptions_.conflictBehavior)
    {
    case ConflictBehavior::AutoRename:
        return L"Auto-rename";
    case ConflictBehavior::Overwrite:
        return L"Overwrite";
    case ConflictBehavior::Skip:
        return L"Skip";
    }

    return L"Auto-rename";
}

double ToolkitApp::ConverterProgress() const
{
    if (conversionJobs_.empty())
    {
        return 0.0;
    }

    int finished = 0;
    for (const ConversionJob& job : conversionJobs_)
    {
        if (job.status == ConversionStatus::Complete ||
            job.status == ConversionStatus::Failed ||
            job.status == ConversionStatus::Skipped)
        {
            ++finished;
        }
    }

    return static_cast<double>(finished) / static_cast<double>(conversionJobs_.size());
}

bool ToolkitApp::CanStartMediaDownload() const
{
    if (mediaAnalyzing_ || mediaDownloading_)
    {
        return false;
    }

    if (!MediaSetupMessage().empty())
    {
        return false;
    }

    if (!SupportedPlatformRegistry::IsSupportedUrl(GetWindowTextString(mediaUrlEdit_)))
    {
        return false;
    }

    if (mediaDownloadOptions_.outputFolder.empty() ||
        !std::filesystem::exists(mediaDownloadOptions_.outputFolder))
    {
        return false;
    }

    if (mediaDownloadOptions_.outputFormat == MediaOutputFormat::Mp4 &&
        mediaDownloadJob_.platform == MediaPlatform::SoundCloud)
    {
        return false;
    }

    return true;
}

std::wstring ToolkitApp::MediaQualityLabel() const
{
    if (mediaDownloadOptions_.outputFormat == MediaOutputFormat::Mp4)
    {
        return MediaDownloadService::Mp4QualityLabel(mediaDownloadOptions_.mp4Quality);
    }
    if (mediaDownloadOptions_.outputFormat == MediaOutputFormat::Mp3)
    {
        return MediaDownloadService::Mp3BitrateLabel(mediaDownloadOptions_.mp3Bitrate);
    }
    return L"WAV lossless / uncompressed";
}

std::wstring ToolkitApp::MediaSetupMessage() const
{
    std::wstring message;
    if (!mediaExternalTools_.ytDlpFound)
    {
        message += L"yt-dlp is missing";
    }
    if (!mediaExternalTools_.ffmpegFound)
    {
        if (!message.empty())
        {
            message += L"; ";
        }
        message += L"FFmpeg is missing";
    }

    if (!message.empty())
    {
        message += L". Put missing tools in the app tools folder or make them available on PATH.";
    }
    return message;
}

std::wstring ToolkitApp::CurrentDateTimeLabel() const
{
    SYSTEMTIME localTime {};
    GetLocalTime(&localTime);

    wchar_t buffer[64] {};
    swprintf_s(
        buffer,
        L"%02u/%02u/%04u %02u:%02u:%02u",
        localTime.wMonth,
        localTime.wDay,
        localTime.wYear,
        localTime.wHour,
        localTime.wMinute,
        localTime.wSecond);
    return buffer;
}

int ToolkitApp::Dips(int value) const
{
    return MulDiv(value, dpi_, 96);
}
