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
#include <urlmon.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cwchar>
#include <cwctype>
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

COLORREF kAppBackground = RGB(18, 20, 24);
COLORREF kSidebarBackground = RGB(24, 27, 33);
COLORREF kPanelBackground = RGB(31, 35, 43);
COLORREF kPanelHover = RGB(39, 44, 53);
COLORREF kAccent = RGB(83, 147, 245);
COLORREF kAccentSoft = RGB(44, 86, 153);
COLORREF kGold = RGB(245, 191, 79);
COLORREF kTextPrimary = RGB(245, 247, 250);
COLORREF kTextSecondary = RGB(166, 174, 186);
COLORREF kBorder = RGB(48, 54, 65);
COLORREF kInputBackground = RGB(25, 29, 36);
COLORREF kButtonBackground = RGB(37, 42, 51);
COLORREF kButtonHover = RGB(48, 56, 69);
COLORREF kButtonPressed = RGB(46, 58, 74);
COLORREF kDisabledBackground = RGB(34, 38, 46);
COLORREF kDisabledText = RGB(112, 120, 132);
COLORREF kDropdownBackground = RGB(29, 34, 43);
COLORREF kDropdownHover = RGB(43, 50, 62);
COLORREF kDropdownSelected = RGB(39, 54, 78);

constexpr int kMinWindowWidth = 920;
constexpr int kMinWindowHeight = 560;
constexpr UINT kConversionProgressMessage = WM_APP + 101;
constexpr UINT kConversionFinishedMessage = WM_APP + 102;
constexpr UINT kMediaJobUpdateMessage = WM_APP + 103;
constexpr UINT kMediaFinishedMessage = WM_APP + 104;
constexpr UINT kUpdateCheckFinishedMessage = WM_APP + 105;
constexpr UINT kUpdateInstallFinishedMessage = WM_APP + 106;
constexpr UINT kAnimeSearchFinishedMessage = WM_APP + 107;
constexpr UINT kAnimeRefreshFinishedMessage = WM_APP + 108;
constexpr UINT kAnimeRefreshAllFinishedMessage = WM_APP + 109;
constexpr int kMinClicksPerSecond = 1;
constexpr int kMaxClicksPerSecond = 100;
constexpr UINT_PTR kClockTimerId = 1002;
constexpr UINT_PTR kAnimeNotesAutosaveTimerId = 1003;
constexpr UINT kAnimeNotesAutosaveDelayMs = 850;
constexpr ULONG_PTR kAutoClickExtraInfo = 0x5254584B;
constexpr int kAnimeSearchResultCardWidthDip = 216;
constexpr int kAnimeSearchResultCardHeightDip = 452;
constexpr int kAnimeSearchResultGridGapDip = 22;
constexpr int kAnimeSearchResultMaxColumns = 6;

ToolkitApp* g_activeApp = nullptr;

struct ThemePalette
{
    COLORREF appBackground;
    COLORREF sidebarBackground;
    COLORREF panelBackground;
    COLORREF panelHover;
    COLORREF accent;
    COLORREF accentSoft;
    COLORREF gold;
    COLORREF textPrimary;
    COLORREF textSecondary;
    COLORREF border;
    COLORREF inputBackground;
    COLORREF buttonBackground;
    COLORREF buttonHover;
    COLORREF buttonPressed;
    COLORREF disabledBackground;
    COLORREF disabledText;
    COLORREF dropdownBackground;
    COLORREF dropdownHover;
    COLORREF dropdownSelected;
};

ThemePalette PaletteForTheme(AppTheme theme)
{
    if (theme == AppTheme::Light)
    {
        return {
            RGB(245, 247, 251),
            RGB(238, 242, 247),
            RGB(255, 255, 255),
            RGB(232, 238, 247),
            RGB(47, 111, 214),
            RGB(215, 229, 252),
            RGB(179, 120, 22),
            RGB(31, 41, 55),
            RGB(92, 105, 124),
            RGB(211, 219, 232),
            RGB(247, 249, 252),
            RGB(239, 243, 249),
            RGB(226, 234, 246),
            RGB(214, 226, 244),
            RGB(232, 236, 243),
            RGB(151, 160, 174),
            RGB(255, 255, 255),
            RGB(236, 242, 250),
            RGB(223, 235, 254)
        };
    }

    return {
        RGB(18, 20, 24),
        RGB(24, 27, 33),
        RGB(31, 35, 43),
        RGB(39, 44, 53),
        RGB(83, 147, 245),
        RGB(44, 86, 153),
        RGB(245, 191, 79),
        RGB(245, 247, 250),
        RGB(166, 174, 186),
        RGB(48, 54, 65),
        RGB(25, 29, 36),
        RGB(37, 42, 51),
        RGB(48, 56, 69),
        RGB(46, 58, 74),
        RGB(34, 38, 46),
        RGB(112, 120, 132),
        RGB(29, 34, 43),
        RGB(43, 50, 62),
        RGB(39, 54, 78)
    };
}

void ApplyPalette(AppTheme theme)
{
    const ThemePalette palette = PaletteForTheme(theme);
    kAppBackground = palette.appBackground;
    kSidebarBackground = palette.sidebarBackground;
    kPanelBackground = palette.panelBackground;
    kPanelHover = palette.panelHover;
    kAccent = palette.accent;
    kAccentSoft = palette.accentSoft;
    kGold = palette.gold;
    kTextPrimary = palette.textPrimary;
    kTextSecondary = palette.textSecondary;
    kBorder = palette.border;
    kInputBackground = palette.inputBackground;
    kButtonBackground = palette.buttonBackground;
    kButtonHover = palette.buttonHover;
    kButtonPressed = palette.buttonPressed;
    kDisabledBackground = palette.disabledBackground;
    kDisabledText = palette.disabledText;
    kDropdownBackground = palette.dropdownBackground;
    kDropdownHover = palette.dropdownHover;
    kDropdownSelected = palette.dropdownSelected;
}

struct UpdateInstallResult
{
    bool success = false;
    std::wstring message;
};

struct AnimeSearchThreadResult
{
    AnimeSearchResponse response;
    std::wstring message;
    std::vector<std::pair<std::wstring, std::wstring>> coverFiles;
    bool append = false;
};

struct AnimeRefreshThreadResult
{
    std::optional<AnimeSearchResult> result;
    std::wstring message;
    int listIndex = -1;
};

struct AnimeRefreshAllThreadResult
{
    std::vector<std::pair<int, AnimeSearchResult>> results;
    std::wstring message;
};

std::wstring LocalTimeZoneLabel()
{
    TIME_ZONE_INFORMATION timeZone {};
    const DWORD timeZoneState = GetTimeZoneInformation(&timeZone);
    const wchar_t* name = timeZone.StandardName;
    if (timeZoneState == TIME_ZONE_ID_DAYLIGHT && timeZone.DaylightName[0] != L'\0')
    {
        name = timeZone.DaylightName;
    }
    return name && name[0] != L'\0' ? std::wstring(name) : L"local time";
}

std::wstring FriendlyRefreshedLabel(const std::wstring& isoUtc)
{
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (swscanf_s(isoUtc.c_str(), L"%d-%d-%dT%d:%d:%dZ", &year, &month, &day, &hour, &minute, &second) != 6)
    {
        return L"refreshed recently";
    }

    SYSTEMTIME utc {};
    utc.wYear = static_cast<WORD>(year);
    utc.wMonth = static_cast<WORD>(month);
    utc.wDay = static_cast<WORD>(day);
    utc.wHour = static_cast<WORD>(hour);
    utc.wMinute = static_cast<WORD>(minute);
    utc.wSecond = static_cast<WORD>(second);

    FILETIME utcFile {};
    FILETIME localFile {};
    SYSTEMTIME local {};
    if (!SystemTimeToFileTime(&utc, &utcFile) ||
        !FileTimeToLocalFileTime(&utcFile, &localFile) ||
        !FileTimeToSystemTime(&localFile, &local))
    {
        return L"refreshed recently";
    }

    SYSTEMTIME now {};
    GetLocalTime(&now);
    const wchar_t* suffix = local.wHour >= 12 ? L"PM" : L"AM";
    WORD displayHour = local.wHour % 12;
    if (displayHour == 0)
    {
        displayHour = 12;
    }

    wchar_t buffer[64] {};
    if (local.wYear == now.wYear && local.wMonth == now.wMonth && local.wDay == now.wDay)
    {
        swprintf_s(buffer, L"refreshed today at %u:%02u %s", displayHour, local.wMinute, suffix);
        return buffer;
    }

    static constexpr wchar_t months[][4] {
        L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
        L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"
    };
    const wchar_t* monthName = local.wMonth >= 1 && local.wMonth <= 12
        ? months[local.wMonth - 1]
        : L"";
    swprintf_s(buffer, L"refreshed %s %u at %u:%02u %s", monthName, local.wDay, displayHour, local.wMinute, suffix);
    return buffer;
}

int Mp4QualityHeight(Mp4Quality quality)
{
    switch (quality)
    {
    case Mp4Quality::P4320:
        return 4320;
    case Mp4Quality::P2160:
        return 2160;
    case Mp4Quality::P1440:
        return 1440;
    case Mp4Quality::P1080:
        return 1080;
    case Mp4Quality::P720:
        return 720;
    case Mp4Quality::P480:
        return 480;
    case Mp4Quality::Best:
        return 0;
    }
    return 0;
}

Mp4Quality BestAvailableQualityCap(int maxVideoHeight)
{
    if (maxVideoHeight >= 4320) return Mp4Quality::P4320;
    if (maxVideoHeight >= 2160) return Mp4Quality::P2160;
    if (maxVideoHeight >= 1440) return Mp4Quality::P1440;
    if (maxVideoHeight >= 1080) return Mp4Quality::P1080;
    if (maxVideoHeight >= 720) return Mp4Quality::P720;
    if (maxVideoHeight >= 480) return Mp4Quality::P480;
    return Mp4Quality::Best;
}

std::wstring PowerShellQuote(const std::wstring& value)
{
    std::wstring quoted = L"'";
    for (wchar_t ch : value)
    {
        if (ch == L'\'')
        {
            quoted += L"''";
        }
        else
        {
            quoted.push_back(ch);
        }
    }
    quoted.push_back(L'\'');
    return quoted;
}

std::wstring CommandLineQuote(const std::wstring& value)
{
    std::wstring quoted = L"\"";
    size_t slashCount = 0;
    for (wchar_t ch : value)
    {
        if (ch == L'\\')
        {
            ++slashCount;
            continue;
        }
        if (ch == L'"')
        {
            quoted.append((slashCount * 2) + 1, L'\\');
            quoted.push_back(ch);
            slashCount = 0;
            continue;
        }
        quoted.append(slashCount, L'\\');
        slashCount = 0;
        quoted.push_back(ch);
    }
    quoted.append(slashCount * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::wstring NormalizeAnimeSearchQuery(const std::wstring& value)
{
    std::wstring compact;
    compact.reserve(value.size());
    for (wchar_t ch : value)
    {
        if (iswalnum(ch) != 0)
        {
            compact.push_back(static_cast<wchar_t>(towlower(ch)));
        }
    }

    if (compact == L"rezero")
    {
        return L"Re:Zero";
    }
    return value;
}

unsigned long long StableHash(const std::wstring& value)
{
    unsigned long long hash = 1469598103934665603ull;
    for (wchar_t ch : value)
    {
        hash ^= static_cast<unsigned long long>(ch);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::optional<std::filesystem::path> DownloadAnimeCoverToCache(const std::wstring& url)
{
    if (url.empty())
    {
        return std::nullopt;
    }

    wchar_t appDataPath[MAX_PATH] {};
    const DWORD length = GetEnvironmentVariableW(L"APPDATA", appDataPath, static_cast<DWORD>(std::size(appDataPath)));
    if (length == 0 || length >= std::size(appDataPath))
    {
        return std::nullopt;
    }

    const std::filesystem::path cacheDirectory =
        std::filesystem::path(appDataPath) / L"RexsToolkit" / L"anime_covers";
    std::error_code fileError;
    std::filesystem::create_directories(cacheDirectory, fileError);
    if (fileError)
    {
        return std::nullopt;
    }

    std::wostringstream fileName;
    fileName << std::hex << StableHash(url) << L".img";
    const std::filesystem::path coverPath = cacheDirectory / fileName.str();
    if (std::filesystem::exists(coverPath, fileError) && !fileError)
    {
        return coverPath;
    }

    const HRESULT result = URLDownloadToFileW(nullptr, url.c_str(), coverPath.wstring().c_str(), 0, nullptr);
    if (FAILED(result))
    {
        std::filesystem::remove(coverPath, fileError);
        return std::nullopt;
    }

    return coverPath;
}

std::wstring HResultMessage(HRESULT result)
{
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        static_cast<DWORD>(result),
        0,
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);

    if (length == 0 || !buffer)
    {
        std::wostringstream stream;
        stream << L"Error 0x" << std::hex << static_cast<unsigned long>(result);
        return stream.str();
    }

    std::wstring message(buffer, length);
    LocalFree(buffer);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' '))
    {
        message.pop_back();
    }
    return message;
}

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
        },
        {
            L"anime_tracker",
            L"Anime Tracker",
            L"Track anime progress, upcoming episodes, and sequel releases.",
            false,
            ToolKind::AnimeTracker
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

int CALLBACK BrowseFolderCallback(HWND hwnd, UINT message, LPARAM, LPARAM data)
{
    if (message == BFFM_INITIALIZED && data)
    {
        SendMessageW(hwnd, BFFM_SETSELECTION, TRUE, data);
    }
    return 0;
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
    LoadAppSettings();
    ApplyPalette(appSettings_.theme);
    currentPage_ = appSettings_.startPage == DefaultStartPage::AllTools ? Page::AllTools : Page::Favorites;
    LoadAutoClickerSettings();
    autoClickerCps_.store(autoClicker_.clicksPerSecond);
    autoClickerOutputButton_.store(static_cast<int>(autoClicker_.outputButton));
    mediaExternalTools_ = mediaDownloadService_.CheckExternalTools();
    mediaDownloadOptions_.outputFolder = ExternalToolService::DefaultDownloadsFolder();
    LoadMediaDownloadSettings();
    if (!appSettings_.defaultOutputFolder.empty())
    {
        mediaDownloadOptions_.outputFolder = appSettings_.defaultOutputFolder;
    }
    else
    {
        appSettings_.defaultOutputFolder = mediaDownloadOptions_.outputFolder;
    }
    mediaDownloadJob_.outputFolder = mediaDownloadOptions_.outputFolder;
    mediaStatusText_ = MediaSetupMessage().empty() ? L"Ready." : MediaSetupMessage();
    LoadAnimeTrackerData();
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
        if (message.message == WM_KEYDOWN &&
            message.wParam == 'A' &&
            (GetKeyState(VK_CONTROL) & 0x8000) != 0)
        {
            for (HWND edit : { mediaUrlEdit_, mediaFileNameEdit_, animeSearchEdit_, animeNotesEdit_ })
            {
                if (message.hwnd == edit)
                {
                    SendMessageW(edit, EM_SETSEL, 0, -1);
                    goto handledMessage;
                }
            }
        }

    if (message.message == WM_KEYDOWN &&
        message.wParam == VK_RETURN &&
        message.hwnd == animeSearchEdit_ &&
        currentPage_ == Page::Tool &&
            currentTool_ == ToolKind::AnimeTracker &&
            animeTrackerTab_ == AnimeTrackerTab::Search)
        {
        StartAnimeSearch(false);
        continue;
    }

    if (message.message == WM_KEYDOWN &&
        message.wParam == VK_RETURN &&
        message.hwnd == mediaUrlEdit_ &&
        currentPage_ == Page::Tool &&
        currentTool_ == ToolKind::MediaDownloader &&
        !mediaAnalyzing_ &&
        !mediaDownloading_)
    {
        AnalyzeMediaUrl();
        continue;
    }

    TranslateMessage(&message);
        DispatchMessageW(&message);
    handledMessage:
        ;
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
        searchInputFont_ = CreateUiFont(dpi_, 12, FW_NORMAL);
        monospaceFont_ = CreateUiFont(dpi_, 10, FW_NORMAL, L"Cascadia Mono");
        if (mediaUrlEdit_)
        {
            SendMessageW(mediaUrlEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(searchInputFont_), TRUE);
            SendMessageW(mediaUrlEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(Dips(8), Dips(6)));
        }
        if (mediaFileNameEdit_)
        {
            SendMessageW(mediaFileNameEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(searchInputFont_), TRUE);
            SendMessageW(mediaFileNameEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(Dips(8), Dips(6)));
        }
        editBackgroundBrush_ = CreateSolidBrush(kInputBackground);
        Gdiplus::GdiplusStartupInput gdiplusStartupInput {};
        Gdiplus::GdiplusStartup(&gdiplusToken_, &gdiplusStartupInput, nullptr);
        LoadLogoResource();
        LoadToolIconResources();
        CreateMediaDownloaderControls();
        CreateAnimeTrackerControls();
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
        DeleteObject(searchInputFont_);
        DeleteObject(monospaceFont_);

        titleFont_ = CreateUiFont(dpi_, 17, FW_SEMIBOLD);
        navFont_ = CreateUiFont(dpi_, 12, FW_SEMIBOLD);
        headingFont_ = CreateUiFont(dpi_, 22, FW_SEMIBOLD, L"Bahnschrift SemiBold");
        bodyFont_ = CreateUiFont(dpi_, 11, FW_NORMAL);
        searchInputFont_ = CreateUiFont(dpi_, 12, FW_NORMAL);
        monospaceFont_ = CreateUiFont(dpi_, 10, FW_NORMAL, L"Cascadia Mono");

        for (HWND edit : { mediaUrlEdit_, mediaFileNameEdit_ })
        {
            if (!edit)
            {
                continue;
            }
            SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(searchInputFont_), TRUE);
            SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(Dips(8), Dips(6)));
        }
        if (animeSearchEdit_)
        {
            SendMessageW(animeSearchEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(searchInputFont_), TRUE);
            SendMessageW(animeSearchEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(Dips(8), Dips(6)));
        }
        if (animeNotesEdit_)
        {
            SendMessageW(animeNotesEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(monospaceFont_), TRUE);
            SendMessageW(animeNotesEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(Dips(4), Dips(4)));
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
    case WM_CTLCOLORSTATIC:
    {
        HDC editDc = reinterpret_cast<HDC>(wParam);
        SetTextColor(editDc, kTextPrimary);
        SetBkColor(editDc, kInputBackground);
        return reinterpret_cast<LRESULT>(editBackgroundBrush_);
    }

    case WM_COMMAND:
        if (reinterpret_cast<HWND>(lParam) == animeSearchEdit_ && HIWORD(wParam) == EN_CHANGE)
        {
            if (GetWindowTextString(animeSearchEdit_).empty() &&
                (!animeSearchResults_.empty() || animeSearchHasRun_ || animeCanLoadMore_))
            {
                animeSearchResults_.clear();
                animeSearchResponse_ = {};
                animeSearchHasRun_ = false;
                animeCanLoadMore_ = false;
                animeCurrentPage_ = 1;
                animeStatusMessage_ = L"Search cleared.";
                RecalculateLayout();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        if (reinterpret_cast<HWND>(lParam) == animeNotesEdit_ && HIWORD(wParam) == EN_CHANGE)
        {
            if (!suppressAnimeNotesChange_ &&
                selectedAnimeIndex_ >= 0 &&
                selectedAnimeIndex_ < static_cast<int>(animeWatchList_.anime.size()))
            {
                animeNotesStatusText_ = L"Autosaving...";
                KillTimer(hwnd_, kAnimeNotesAutosaveTimerId);
                SetTimer(hwnd_, kAnimeNotesAutosaveTimerId, kAnimeNotesAutosaveDelayMs, nullptr);
                RECT repaint = PaddedRect(animeNotesEditRect_, Dips(48));
                InvalidateRect(hwnd_, &repaint, FALSE);
            }
            return 0;
        }
        break;

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
            RECT repaint = PaddedRect(dateTimeRect_, Dips(4));
            InvalidateRect(hwnd_, &repaint, FALSE);
            return 0;
        }
        if (wParam == kAnimeNotesAutosaveTimerId)
        {
            KillTimer(hwnd_, kAnimeNotesAutosaveTimerId);
            SaveSelectedAnimeNotes();
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

    case kUpdateInstallFinishedMessage:
    {
        std::unique_ptr<UpdateInstallResult> result(reinterpret_cast<UpdateInstallResult*>(lParam));
        updateInstalling_ = false;
        if (result)
        {
            updateInstallStatus_ = result->message;
        }
        FinishUpdateThread();
        InvalidateRect(hwnd_, nullptr, FALSE);
        if (result && result->success)
        {
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        }
        return 0;
    }

    case kAnimeSearchFinishedMessage:
    {
        std::unique_ptr<AnimeSearchThreadResult> result(reinterpret_cast<AnimeSearchThreadResult*>(lParam));
        animeSearching_ = false;
        if (result)
        {
            for (const auto& cover : result->coverFiles)
            {
                if (cover.first.empty() || cover.second.empty() || animeCoverCache_.find(cover.first) != animeCoverCache_.end())
                {
                    continue;
                }

                auto bitmap = std::make_unique<Gdiplus::Bitmap>(cover.second.c_str());
                if (bitmap && bitmap->GetLastStatus() == Gdiplus::Ok)
                {
                    animeCoverCache_[cover.first] = std::move(bitmap);
                }
            }
            ApplyAnimeSearchResponse(result->response, result->message, result->append);
        }
        FinishAnimeThread();
        RecalculateLayout();
        UpdateAnimeTrackerControls();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }

    case kAnimeRefreshFinishedMessage:
    {
        std::unique_ptr<AnimeRefreshThreadResult> result(reinterpret_cast<AnimeRefreshThreadResult*>(lParam));
        animeRefreshing_ = false;
        if (result && result->result)
        {
            ApplyAnimeRefreshResult(*result->result, result->listIndex, result->message);
        }
        else if (result)
        {
            animeStatusMessage_ = result->message;
        }
        FinishAnimeThread();
        UpdateAnimeTrackerControls();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }

    case kAnimeRefreshAllFinishedMessage:
    {
        std::unique_ptr<AnimeRefreshAllThreadResult> result(reinterpret_cast<AnimeRefreshAllThreadResult*>(lParam));
        animeRefreshing_ = false;
        if (result)
        {
            for (const auto& refreshed : result->results)
            {
                ApplyAnimeRefreshResult(refreshed.second, refreshed.first, L"");
            }
            animeStatusMessage_ = result->message;
        }
        FinishAnimeThread();
        SaveAnimeTrackerData();
        UpdateAnimeTrackerControls();
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
        FinishAnimeThread();
        SaveWindowSettings();
        SaveAppSettings();
        SaveMediaDownloadSettings();
        SaveAutoClickerSettings();
        SaveAnimeTrackerData();
        SetAutoClickerRunning(false);
        KillTimer(hwnd_, kClockTimerId);
        KillTimer(hwnd_, kAnimeNotesAutosaveTimerId);
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
        DeleteObject(searchInputFont_);
        DeleteObject(monospaceFont_);
        DeleteObject(editBackgroundBrush_);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

void ToolkitApp::ApplyDarkTitleBar()
{
    SetWindowTheme(hwnd_, appSettings_.theme == AppTheme::Dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);

    BOOL useDarkMode = appSettings_.theme == AppTheme::Dark ? TRUE : FALSE;
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

std::wstring ToolkitApp::AppSettingsFilePath() const
{
    return SettingsDirectory() + L"\\settings.txt";
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

void ToolkitApp::LoadAppSettings()
{
    appSettings_.defaultOutputFolder.clear();
    appSettings_.startPage = DefaultStartPage::Favorites;
    appSettings_.clockFormat = ClockFormat::MonthDay24;
    appSettings_.theme = AppTheme::Dark;

    std::wifstream file(AppSettingsFilePath());
    if (!file)
    {
        return;
    }

    std::wstring line;
    while (std::getline(file, line))
    {
        const size_t separator = line.find(L'=');
        if (separator == std::wstring::npos)
        {
            continue;
        }

        const std::wstring key = line.substr(0, separator);
        const std::wstring value = line.substr(separator + 1);
        if (key == L"defaultOutputFolder")
        {
            appSettings_.defaultOutputFolder = value;
        }
        else if (key == L"startPage")
        {
            appSettings_.startPage = value == L"all_tools" ? DefaultStartPage::AllTools : DefaultStartPage::Favorites;
        }
        else if (key == L"clockFormat")
        {
            if (value == L"mdy12")
            {
                appSettings_.clockFormat = ClockFormat::MonthDay12;
            }
            else if (value == L"iso24")
            {
                appSettings_.clockFormat = ClockFormat::Iso24;
            }
            else if (value == L"friendly12")
            {
                appSettings_.clockFormat = ClockFormat::Friendly12;
            }
            else
            {
                appSettings_.clockFormat = ClockFormat::MonthDay24;
            }
        }
        else if (key == L"theme")
        {
            appSettings_.theme = value == L"light" ? AppTheme::Light : AppTheme::Dark;
        }
    }
}

void ToolkitApp::SaveAppSettings() const
{
    const std::wstring settingsDirectory = SettingsDirectory();
    CreateDirectoryW(settingsDirectory.c_str(), nullptr);

    std::wofstream file(AppSettingsFilePath(), std::ios::trunc);
    if (!file)
    {
        return;
    }

    std::wstring clock = L"mdy24";
    if (appSettings_.clockFormat == ClockFormat::MonthDay12)
    {
        clock = L"mdy12";
    }
    else if (appSettings_.clockFormat == ClockFormat::Iso24)
    {
        clock = L"iso24";
    }
    else if (appSettings_.clockFormat == ClockFormat::Friendly12)
    {
        clock = L"friendly12";
    }

    file << L"defaultOutputFolder=" << appSettings_.defaultOutputFolder.wstring() << L'\n'
        << L"startPage=" << (appSettings_.startPage == DefaultStartPage::AllTools ? L"all_tools" : L"favorites") << L'\n'
        << L"clockFormat=" << clock << L'\n'
        << L"theme=" << (appSettings_.theme == AppTheme::Light ? L"light" : L"dark") << L'\n';
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
    if (mp4Quality >= 0 && mp4Quality <= 6)
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
    const int centeredNavLeft = clientRect_.left + ((clientRect_.right - clientRect_.left) - navGroupWidth) / 2;
    const int navSafeLeft = clientRect_.left + Dips(112);
    const int navSafeRight = clientRect_.right - Dips(250);
    int navLeft = centeredNavLeft;
    if (navSafeRight - navSafeLeft >= navGroupWidth)
    {
        navLeft = std::clamp(centeredNavLeft, navSafeLeft, navSafeRight - navGroupWidth);
    }
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

    dateTimeRect_ = {
        std::max(settingsNavRect_.right + Dips(20), headerRect_.right - Dips(250)),
        headerRect_.top,
        headerRect_.right - Dips(28),
        headerRect_.bottom
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
    const int panelTop = contentTop + Dips(164);
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
    const int settingsTop = contentTop + Dips(132);
    const int settingsRailWidth = Dips(220);
    const int settingsRailItemHeight = Dips(44);
    const int settingsRailGap = Dips(10);
    settingsGeneralTabRect_ = {
        settingsLeft + Dips(14),
        settingsTop + Dips(18),
        settingsLeft + settingsRailWidth - Dips(14),
        settingsTop + Dips(18) + settingsRailItemHeight
    };
    settingsAppearanceTabRect_ = {
        settingsGeneralTabRect_.left,
        settingsGeneralTabRect_.bottom + settingsRailGap,
        settingsGeneralTabRect_.right,
        settingsGeneralTabRect_.bottom + settingsRailGap + settingsRailItemHeight
    };
    settingsUpdatesTabRect_ = {
        settingsGeneralTabRect_.left,
        settingsAppearanceTabRect_.bottom + settingsRailGap,
        settingsGeneralTabRect_.right,
        settingsAppearanceTabRect_.bottom + settingsRailGap + settingsRailItemHeight
    };
    settingsAboutTabRect_ = {
        settingsGeneralTabRect_.left,
        settingsUpdatesTabRect_.bottom + settingsRailGap,
        settingsGeneralTabRect_.right,
        settingsUpdatesTabRect_.bottom + settingsRailGap + settingsRailItemHeight
    };

    const int settingsContentLeft = settingsLeft + settingsRailWidth + Dips(18);
    const int settingsContentRight = contentRect_.right - contentMargin;
    const int settingsControlLeft = settingsContentLeft + Dips(28);
    const int settingsControlRight = settingsContentRight - Dips(28);
    settingsDefaultFolderRect_ = {
        settingsControlLeft,
        settingsTop + Dips(134),
        settingsControlRight - Dips(132),
        settingsTop + Dips(174)
    };
    settingsBrowseDefaultFolderButtonRect_ = {
        settingsDefaultFolderRect_.right + Dips(12),
        settingsDefaultFolderRect_.top,
        settingsControlRight,
        settingsDefaultFolderRect_.bottom
    };
    settingsStartPageButtonRect_ = {
        settingsControlLeft,
        settingsDefaultFolderRect_.bottom + Dips(72),
        settingsControlLeft + Dips(240),
        settingsDefaultFolderRect_.bottom + Dips(112)
    };
    settingsThemeButtonRect_ = {
        settingsControlLeft,
        settingsTop + Dips(134),
        settingsControlLeft + Dips(240),
        settingsTop + Dips(174)
    };
    settingsClockFormatButtonRect_ = {
        settingsControlLeft,
        settingsThemeButtonRect_.bottom + Dips(72),
        settingsControlLeft + Dips(280),
        settingsThemeButtonRect_.bottom + Dips(112)
    };
    settingsCheckUpdatesButtonRect_ = {
        settingsControlLeft,
        settingsTop + Dips(134),
        settingsControlLeft + Dips(192),
        settingsTop + Dips(178)
    };
    settingsDownloadUpdateButtonRect_ = {
        settingsControlLeft,
        settingsTop + Dips(504),
        settingsControlLeft + Dips(182),
        settingsTop + Dips(548)
    };
    settingsGithubButtonRect_ = {
        settingsControlLeft,
        settingsTop + Dips(344),
        settingsControlLeft + Dips(142),
        settingsTop + Dips(386)
    };
    settingsReportIssueButtonRect_ = {
        settingsGithubButtonRect_.right + Dips(12),
        settingsGithubButtonRect_.top,
        settingsGithubButtonRect_.right + Dips(154),
        settingsGithubButtonRect_.bottom
    };

    const int animeLeft = contentRect_.left + contentMargin;
    const int animeRight = contentRect_.right - contentMargin;
    const int animeTop = contentTop + Dips(86);
    const int animePanelGap = Dips(18);
    animeSearchTabRect_ = {
        animeLeft,
        animeTop + Dips(28),
        animeLeft + Dips(136),
        animeTop + Dips(68)
    };
    animeListTabRect_ = {
        animeSearchTabRect_.right + Dips(10),
        animeSearchTabRect_.top,
        animeSearchTabRect_.right + Dips(146),
        animeSearchTabRect_.bottom
    };
    const int animeBodyTop = animeSearchTabRect_.bottom + Dips(18);
    const bool showAnimeResultsPanel = animeSearching_ || animeSearchHasRun_;
    const int animeSearchResultCount = showAnimeResultsPanel
        ? std::max(1, static_cast<int>(animeSearchResults_.size()))
        : 0;
    const int animeSearchGridGap = Dips(kAnimeSearchResultGridGapDip);
    const int animeSearchGridWidth = std::max(1, animeRight - animeLeft - Dips(44));
    const int animeSearchGridColumns = std::max(1, std::min(kAnimeSearchResultMaxColumns, (animeSearchGridWidth + animeSearchGridGap) / (Dips(kAnimeSearchResultCardWidthDip) + animeSearchGridGap)));
    const int animeSearchGridRows = animeSearchResultCount > 0
        ? (animeSearchResultCount + animeSearchGridColumns - 1) / animeSearchGridColumns
        : 0;
    const int animeSearchGridHeight = animeSearchGridRows > 0
        ? animeSearchGridRows * Dips(kAnimeSearchResultCardHeightDip) + (animeSearchGridRows - 1) * animeSearchGridGap
        : 0;
    const int animeSearchPanelHeight = showAnimeResultsPanel
        ? Dips(228) + animeSearchGridHeight + (animeCanLoadMore_ ? Dips(66) : Dips(22))
        : Dips(174);

    animeResultsRect_ = {
        animeLeft,
        animeBodyTop,
        animeRight,
        animeBodyTop + animeSearchPanelHeight
    };
    animeSearchEditRect_ = {
        animeResultsRect_.left + Dips(24),
        animeResultsRect_.top + Dips(96),
        animeResultsRect_.right - Dips(160),
        animeResultsRect_.top + Dips(132)
    };
    animeSearchButtonRect_ = {
        animeSearchEditRect_.right + Dips(12),
        animeSearchEditRect_.top,
        animeResultsRect_.right - Dips(24),
        animeSearchEditRect_.bottom
    };
    animeLoadMoreButtonRect_ = {
        animeResultsRect_.left + ((animeResultsRect_.right - animeResultsRect_.left) - Dips(136)) / 2,
        animeResultsRect_.bottom - Dips(48),
        animeResultsRect_.left + ((animeResultsRect_.right - animeResultsRect_.left) + Dips(136)) / 2,
        animeResultsRect_.bottom - Dips(12)
    };
    int animeSectionTop = animeTrackerTab_ == AnimeTrackerTab::Search
        ? animeBodyTop
        : animeBodyTop;
    const int animeVisibleListRows = std::max(1, static_cast<int>(VisibleAnimeEntryIndexes().size()));
    const int animeListHeight = Dips(148) + animeVisibleListRows * Dips(68) + Dips(20);
    animeListRect_ = {
        animeLeft,
        animeSectionTop,
        animeRight,
        animeSectionTop + std::max(Dips(240), animeListHeight)
    };
    animeFilterButtonRect_ = {
        animeListRect_.left + Dips(18),
        animeListRect_.top + Dips(48),
        animeListRect_.left + Dips(206),
        animeListRect_.top + Dips(86)
    };
    animeRefreshAllButtonRect_ = {
        animeListRect_.right - Dips(154),
        animeFilterButtonRect_.top,
        animeListRect_.right - Dips(18),
        animeFilterButtonRect_.bottom
    };
    animeSectionTop = animeListRect_.bottom + animePanelGap;

    const int animeDetailTop = animeSectionTop;
    const int animeDetailLeft = animeLeft;
    const int animeDetailRight = animeRight;
    animeStatusButtonRect_ = {
        animeDetailLeft + Dips(22),
        animeDetailTop + Dips(126),
        animeDetailLeft + Dips(214),
        animeDetailTop + Dips(166)
    };
    animeEpisodeMinusButtonRect_ = {
        animeStatusButtonRect_.right + Dips(12),
        animeStatusButtonRect_.top,
        animeStatusButtonRect_.right + Dips(58),
        animeStatusButtonRect_.bottom
    };
    animeEpisodePlusButtonRect_ = {
        animeEpisodeMinusButtonRect_.right + Dips(8),
        animeStatusButtonRect_.top,
        animeEpisodeMinusButtonRect_.right + Dips(54),
        animeStatusButtonRect_.bottom
    };
    animeFavoriteButtonRect_ = {
        animeEpisodePlusButtonRect_.right + Dips(12),
        animeStatusButtonRect_.top,
        animeEpisodePlusButtonRect_.right + Dips(134),
        animeStatusButtonRect_.bottom
    };

    animeSectionTop = animeStatusButtonRect_.bottom + Dips(38);
    const int animeScheduleGap = Dips(18);
    const int animeScheduleWidth = std::max(Dips(260), ((animeRight - Dips(44)) - animeScheduleGap) / 2);

    animeUpcomingRect_ = {
        animeLeft + Dips(22),
        animeSectionTop,
        animeLeft + Dips(22) + animeScheduleWidth,
        animeSectionTop + Dips(190)
    };

    animeSequelsRect_ = {
        animeUpcomingRect_.right + animeScheduleGap,
        animeSectionTop,
        animeRight - Dips(22),
        animeSectionTop + Dips(190)
    };

    animeSectionTop = std::max(animeUpcomingRect_.bottom, animeSequelsRect_.bottom) + Dips(48);
    animeSaveNotesButtonRect_ = {
        animeDetailRight - Dips(142),
        animeSectionTop - Dips(8),
        animeDetailRight - Dips(22),
        animeSectionTop + Dips(32)
    };
    animeNotesEditRect_ = {
        animeDetailLeft + Dips(22),
        animeSectionTop + Dips(42),
        animeDetailRight - Dips(22),
        animeSectionTop + Dips(156)
    };

    int desiredContentHeight = contentRect_.bottom - contentRect_.top;
    if (currentPage_ == Page::Tool && currentTool_ == ToolKind::MediaDownloader)
    {
        desiredContentHeight = Dips(720);
    }
    else if (currentPage_ == Page::Tool && currentTool_ == ToolKind::AnimeTracker)
    {
        const int animePageBottom = animeTrackerTab_ == AnimeTrackerTab::Search
            ? static_cast<int>(animeResultsRect_.bottom)
            : static_cast<int>(animeNotesEditRect_.bottom);
        desiredContentHeight = std::max(Dips(620), animePageBottom + Dips(48) - contentTop);
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
        desiredContentHeight = Dips(760);
    }
    else
    {
        const auto visibleTools = VisibleToolsForCurrentPage();
        if (!visibleTools.empty())
        {
            const int columns = 3;
            const int gap = Dips(18);
            const int availableWidth = std::max(1, static_cast<int>(contentRect_.right - contentRect_.left) - (contentMargin * 2));
            const int cardSize = std::max(Dips(160), (availableWidth - (gap * (columns - 1))) / columns);
            const size_t rowCount = (visibleTools.size() + 2) / 3;
            desiredContentHeight = Dips(124) + static_cast<int>(rowCount) * (cardSize + gap) + Dips(46);
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
    UpdateAnimeTrackerControls();
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

    FillRoundRect(hdc, scrollBarTrackRect_, Dips(8), kSidebarBackground);
    FillRoundRect(hdc, scrollBarThumbRect_, Dips(8), kAccentSoft);
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
    UpdateAnimeTrackerControls();
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
    UpdateAnimeTrackerControls();
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
    if (RectsOverlap(clippedPaint, headerRect_))
    {
        PaintHeader(backBufferDc_);
    }
    if (RectsOverlap(clippedPaint, contentRect_))
    {
        const int contentDc = SaveDC(backBufferDc_);
        IntersectClipRect(backBufferDc_, contentRect_.left, contentRect_.top, contentRect_.right, contentRect_.bottom);
        PaintContent(backBufferDc_);
        RestoreDC(backBufferDc_, contentDc);
    }
    if (RectsOverlap(clippedPaint, scrollBarTrackRect_))
    {
        PaintScrollBar(backBufferDc_);
    }
    RECT versionRect {
        clientRect_.right - Dips(190),
        clientRect_.bottom - Dips(28),
        clientRect_.right - Dips(18),
        clientRect_.bottom - Dips(8)
    };
    if (RectsOverlap(clippedPaint, versionRect))
    {
        PaintVersionFooter(backBufferDc_);
    }
    if (RectsOverlap(clippedPaint, dropdownRect_))
    {
        PaintDropdown(backBufferDc_);
    }

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
    RECT clip {};
    if (GetClipBox(hdc, &clip) == NULLREGION)
    {
        return;
    }

    FillSolidRect(hdc, headerRect_, kSidebarBackground);

    RECT border = headerRect_;
    border.top = headerRect_.bottom - 1;
    if (RectsOverlap(clip, border))
    {
        FillSolidRect(hdc, border, kBorder);
    }

    RECT logoRect {
        headerRect_.left + Dips(28),
        headerRect_.top + Dips(8),
        headerRect_.left + Dips(84),
        headerRect_.top + Dips(64)
    };
    if (RectsOverlap(clip, logoRect))
    {
        PaintLogo(hdc, logoRect);
    }

    if (RectsOverlap(clip, favoritesNavRect_))
    {
        PaintNavItem(hdc, favoritesNavRect_, L"Favorites", currentPage_ == Page::Favorites);
    }
    if (RectsOverlap(clip, allToolsNavRect_))
    {
        PaintNavItem(hdc, allToolsNavRect_, L"All Tools", currentPage_ == Page::AllTools);
    }
    if (RectsOverlap(clip, settingsNavRect_))
    {
        PaintNavItem(hdc, settingsNavRect_, L"Settings", currentPage_ == Page::Settings);
    }

    if (RectsOverlap(clip, dateTimeRect_))
    {
        DrawTextLine(
            hdc,
            CurrentDateTimeLabel().c_str(),
            dateTimeRect_,
            bodyFont_,
            kTextSecondary,
            DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }
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
        kTextSecondary,
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

void ToolkitApp::PaintBitmapTinted(HDC hdc, Gdiplus::Bitmap* bitmap, const RECT& bounds, COLORREF tint)
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

    const float red = static_cast<float>(GetRValue(tint)) / 255.0f;
    const float green = static_cast<float>(GetGValue(tint)) / 255.0f;
    const float blue = static_cast<float>(GetBValue(tint)) / 255.0f;
    Gdiplus::ColorMatrix colorMatrix = {
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
        red, green, blue, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 0.0f, 1.0f
    };
    Gdiplus::ImageAttributes attributes;
    attributes.SetColorMatrix(&colorMatrix, Gdiplus::ColorMatrixFlagsDefault, Gdiplus::ColorAdjustTypeBitmap);

    Gdiplus::Graphics graphics(hdc);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    Gdiplus::Rect destination(drawLeft, drawTop, drawWidth, drawHeight);
    graphics.DrawImage(
        bitmap,
        destination,
        0,
        0,
        bitmap->GetWidth(),
        bitmap->GetHeight(),
        Gdiplus::UnitPixel,
        &attributes);
}

void ToolkitApp::PaintBitmapCover(HDC hdc, Gdiplus::Bitmap* bitmap, const RECT& bounds, int radius)
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

    const double scale = std::max(
        static_cast<double>(boundsWidth) / imageWidth,
        static_cast<double>(boundsHeight) / imageHeight);
    const double sourceWidth = static_cast<double>(boundsWidth) / scale;
    const double sourceHeight = static_cast<double>(boundsHeight) / scale;
    const double sourceX = (imageWidth - sourceWidth) / 2.0;
    const double sourceY = (imageHeight - sourceHeight) / 2.0;

    Gdiplus::Graphics graphics(hdc);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    Gdiplus::GraphicsPath clipPath;
    const int diameter = std::max(1, radius * 2);
    clipPath.AddArc(bounds.left, bounds.top, diameter, diameter, 180.0f, 90.0f);
    clipPath.AddArc(bounds.right - diameter, bounds.top, diameter, diameter, 270.0f, 90.0f);
    clipPath.AddArc(bounds.right - diameter, bounds.bottom - diameter, diameter, diameter, 0.0f, 90.0f);
    clipPath.AddArc(bounds.left, bounds.bottom - diameter, diameter, diameter, 90.0f, 90.0f);
    clipPath.CloseFigure();
    graphics.SetClip(&clipPath);

    const Gdiplus::RectF destination(
        static_cast<Gdiplus::REAL>(bounds.left),
        static_cast<Gdiplus::REAL>(bounds.top),
        static_cast<Gdiplus::REAL>(boundsWidth),
        static_cast<Gdiplus::REAL>(boundsHeight));
    graphics.DrawImage(
        bitmap,
        destination,
        static_cast<Gdiplus::REAL>(sourceX),
        static_cast<Gdiplus::REAL>(sourceY),
        static_cast<Gdiplus::REAL>(sourceWidth),
        static_cast<Gdiplus::REAL>(sourceHeight),
        Gdiplus::UnitPixel);
    graphics.ResetClip();
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

    if (currentPage_ == Page::Tool && currentTool_ == ToolKind::AnimeTracker)
    {
        PaintAnimeTracker(hdc);
        return;
    }

    if (currentPage_ == Page::Settings)
    {
        PaintSettings(hdc);
        return;
    }

    const int margin = Dips(42);
    const int contentTop = contentRect_.top - scrollOffsetY_;

    RECT pageTitleRect {
        contentRect_.left + margin,
        contentTop + Dips(40),
        contentRect_.right - margin,
        contentTop + Dips(80)
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

    SIZE textSize {};
    HGDIOBJ previousFont = SelectObject(hdc, navFont_);
    GetTextExtentPoint32W(hdc, label, static_cast<int>(wcslen(label)), &textSize);
    SelectObject(hdc, previousFont);

    const int iconSize = Dips(20);
    const int iconGap = Dips(8);
    const int contentWidth = iconSize + iconGap + textSize.cx;
    const int contentLeft = bounds.left + ((bounds.right - bounds.left) - contentWidth) / 2;
    const int iconTop = bounds.top + ((bounds.bottom - bounds.top) - iconSize) / 2;
    RECT iconRect {
        contentLeft,
        iconTop,
        contentLeft + iconSize,
        iconTop + iconSize
    };

    if (navIndex == 0)
    {
        PaintFavoriteStar(hdc, iconRect, selected);
    }
    else if (navIndex == 1)
    {
        PaintBitmapTinted(hdc, allToolsIcon_.get(), iconRect, kTextPrimary);
    }
    else
    {
        PaintBitmapTinted(hdc, settingsIcon_.get(), iconRect, kTextPrimary);
    }

    RECT textRect = bounds;
    textRect.left = iconRect.right + iconGap;
    textRect.right = bounds.right - Dips(10);
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
    const int contentTop = contentRect_.top - scrollOffsetY_;
    const ToolDefinition* tool = FindTool(ToolKind::AutoClicker);

    PaintBackButton(hdc, backButtonRect_);

    RECT titleRect {
        contentRect_.left + margin,
        contentTop + Dips(82),
        contentRect_.right - margin,
        contentTop + Dips(122)
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
    subtitleRect.bottom = subtitleRect.top + Dips(24);
    DrawTextLine(
        hdc,
        L"Hold the activation input to repeatedly press the selected output button.",
        subtitleRect,
        bodyFont_,
        kTextSecondary,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    RECT panel {
        contentRect_.left + margin,
        contentTop + Dips(164),
        contentRect_.right - margin,
        contentTop + Dips(516)
    };

    FillRoundRect(hdc, panel, Dips(16), kPanelBackground);
    StrokeRoundRect(hdc, panel, Dips(16), kBorder);

    RECT statusRect {
        panel.right - Dips(190),
        panel.top + Dips(28),
        panel.right - Dips(32),
        panel.top + Dips(62)
    };
    FillRoundRect(hdc, statusRect, Dips(17), autoClicker_.running ? kAccentSoft : kInputBackground);
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
        ? kDisabledBackground
        : primary
        ? (pressed ? kAccent : hovered ? kAccent : active ? RGB(178, 72, 72) : kAccentSoft)
        : (pressed ? kButtonPressed : hovered ? kButtonHover : active ? kAccentSoft : kButtonBackground);
    COLORREF border = !enabled ? kBorder : primary ? (active ? RGB(220, 96, 96) : kAccent) : hovered ? kAccentSoft : kBorder;
    COLORREF text = enabled ? kTextPrimary : kDisabledText;

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
    const bool hovered = enabled && hasHoveredButton_ && EqualRect(&bounds, &hoveredButtonRect_);
    const bool pressed = enabled && hasPressedButton_ && EqualRect(&bounds, &pressedButtonRect_);
    const COLORREF background = !enabled
        ? kDisabledBackground
        : pressed
        ? kButtonPressed
        : hovered
        ? kButtonHover
        : kButtonBackground;
    const COLORREF border = !enabled ? kBorder : hovered ? kAccentSoft : kBorder;
    const COLORREF text = enabled ? kTextPrimary : kDisabledText;

    RECT paintBounds = bounds;
    if (pressed)
    {
        OffsetRect(&paintBounds, 0, Dips(1));
    }

    FillRoundRect(hdc, paintBounds, Dips(12), background);
    StrokeRoundRect(hdc, paintBounds, Dips(12), border);

    RECT labelRect {
        paintBounds.left + Dips(16),
        paintBounds.top,
        paintBounds.right - Dips(36),
        paintBounds.bottom
    };
    DrawTextLine(hdc, label, labelRect, navFont_, text, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    if (!enabled)
    {
        return;
    }

    RECT chevronRect {
        paintBounds.right - Dips(30),
        paintBounds.top + Dips(10),
        paintBounds.right - Dips(12),
        paintBounds.bottom - Dips(10)
    };
    PaintChevron(hdc, chevronRect, down, kTextSecondary);
}

void ToolkitApp::PaintSlider(HDC hdc)
{
    RECT inactiveTrack = speedSliderTrackRect_;
    FillRoundRect(hdc, inactiveTrack, Dips(6), kBorder);

    RECT activeTrack = speedSliderTrackRect_;
    activeTrack.right = speedSliderThumbRect_.left + ((speedSliderThumbRect_.right - speedSliderThumbRect_.left) / 2);
    FillRoundRect(hdc, activeTrack, Dips(6), kAccent);

    FillRoundRect(hdc, speedSliderThumbRect_, Dips(18), kTextPrimary);
    StrokeRoundRect(hdc, speedSliderThumbRect_, Dips(18), kAccent);
}

void ToolkitApp::PaintFileConverter(HDC hdc)
{
    const int margin = Dips(42);
    const int contentTop = contentRect_.top - scrollOffsetY_;
    const ToolDefinition* tool = FindTool(ToolKind::FileConverter);

    PaintBackButton(hdc, backButtonRect_);

    RECT titleRect {
        contentRect_.left + margin,
        contentTop + Dips(62),
        contentRect_.right - margin,
        contentTop + Dips(102)
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

    FillRoundRect(hdc, converterDropZoneRect_, Dips(18), kInputBackground);
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

        FillRoundRect(hdc, converterQualityTrackRect_, Dips(6), kBorder);
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

        FillRoundRect(hdc, row, Dips(10), index == selectedConversionJob_ ? kAccentSoft : kButtonBackground);

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
    const int contentTop = contentRect_.top - scrollOffsetY_;
    const ToolDefinition* tool = FindTool(ToolKind::MediaDownloader);

    PaintBackButton(hdc, backButtonRect_);

    RECT titleRect {
        backButtonRect_.right + Dips(20),
        contentTop + Dips(16),
        contentRect_.right - margin,
        contentTop + Dips(48)
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

    FillRoundRect(hdc, mediaUrlEditRect_, Dips(10), kInputBackground);
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
    FillRoundRect(hdc, mediaOutputFolderRect_, Dips(10), kInputBackground);
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
    FillRoundRect(hdc, mediaFileNameEditRect_, Dips(10), kInputBackground);
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

void ToolkitApp::PaintAnimeTracker(HDC hdc)
{
    const int margin = Dips(42);
    const int contentTop = contentRect_.top - scrollOffsetY_;
    const ToolDefinition* tool = FindTool(ToolKind::AnimeTracker);

    PaintBackButton(hdc, backButtonRect_);

    RECT titleRect {
        backButtonRect_.right + Dips(20),
        contentTop + Dips(16),
        contentRect_.right - margin,
        contentTop + Dips(48)
    };
    DrawTextLine(
        hdc,
        tool ? tool->name.c_str() : L"Anime Tracker",
        titleRect,
        headingFont_,
        kTextPrimary,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    RECT subtitleRect = titleRect;
    subtitleRect.top = titleRect.bottom + Dips(2);
    subtitleRect.bottom = subtitleRect.top + Dips(24);
    DrawTextLine(
        hdc,
        L"Search AniList, save a local watchlist, and track episodes, airing dates, and sequels.",
        subtitleRect,
        bodyFont_,
        kTextSecondary,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    RECT animeTabBar {
        animeSearchTabRect_.left - Dips(4),
        animeSearchTabRect_.top - Dips(4),
        animeListTabRect_.right + Dips(4),
        animeSearchTabRect_.bottom + Dips(4)
    };
    FillRoundRect(hdc, animeTabBar, Dips(14), kInputBackground);
    StrokeRoundRect(hdc, animeTabBar, Dips(14), kBorder);

    auto paintAnimeTab = [&](const RECT& tab, const wchar_t* label, bool selected)
    {
        const bool hovered = hasHoveredButton_ && EqualRect(&tab, &hoveredButtonRect_);
        const bool pressed = hasPressedButton_ && EqualRect(&tab, &pressedButtonRect_);
        RECT paintRect = tab;
        if (pressed)
        {
            OffsetRect(&paintRect, 0, Dips(1));
        }

        const COLORREF fill = selected
            ? kAccentSoft
            : hovered
            ? kPanelHover
            : kInputBackground;
        const COLORREF border = selected ? kAccentSoft : (hovered ? kBorder : kInputBackground);
        FillRoundRect(hdc, paintRect, Dips(11), fill);
        StrokeRoundRect(hdc, paintRect, Dips(11), border);

        DrawTextLine(
            hdc,
            label,
            paintRect,
            navFont_,
            selected || hovered ? kTextPrimary : kTextSecondary,
            DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    };
    paintAnimeTab(animeSearchTabRect_, L"Search", animeTrackerTab_ == AnimeTrackerTab::Search);
    paintAnimeTab(animeListTabRect_, L"Anime", animeTrackerTab_ == AnimeTrackerTab::Anime);

    auto paintPanel = [&](const RECT& panel, const wchar_t* title, const std::wstring& meta = L"")
    {
        FillRoundRect(hdc, panel, Dips(16), kPanelBackground);
        StrokeRoundRect(hdc, panel, Dips(16), kBorder);

        RECT titleLine {
            panel.left + Dips(18),
            panel.top + Dips(12),
            panel.right - Dips(18),
            panel.top + Dips(40)
        };
        DrawTextLine(hdc, title, titleLine, navFont_, kTextPrimary, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        if (!meta.empty())
        {
            DrawTextLine(hdc, meta.c_str(), titleLine, bodyFont_, kTextSecondary, DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        }
    };

    auto paintEmpty = [&](const RECT& panel, const wchar_t* title, const wchar_t* subtitle)
    {
        const int blockHeight = Dips(58);
        const int blockTop = panel.top + ((panel.bottom - panel.top) - blockHeight) / 2 + Dips(14);
        RECT titleLine {
            panel.left + Dips(24),
            blockTop,
            panel.right - Dips(24),
            blockTop + Dips(28)
        };
        DrawTextLine(hdc, title, titleLine, navFont_, kTextPrimary, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        RECT subtitleLine = titleLine;
        subtitleLine.top = titleLine.bottom + Dips(4);
        subtitleLine.bottom = subtitleLine.top + Dips(26);
        DrawTextLine(hdc, subtitle, subtitleLine, bodyFont_, kTextSecondary, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    };

    if (animeTrackerTab_ == AnimeTrackerTab::Search)
    {
    RECT searchPanel = animeResultsRect_;
    FillRoundRect(hdc, searchPanel, Dips(16), kPanelBackground);
    StrokeRoundRect(hdc, searchPanel, Dips(16), kBorder);

    RECT searchTitle {
        searchPanel.left + Dips(22),
        searchPanel.top + Dips(14),
        searchPanel.right - Dips(22),
        searchPanel.top + Dips(42)
    };
    DrawTextLine(hdc, L"Search AniList", searchTitle, navFont_, kTextPrimary, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    RECT searchHint {
        searchPanel.left + Dips(22),
        searchTitle.bottom + Dips(2),
        searchPanel.right - Dips(22),
        searchTitle.bottom + Dips(28)
    };
    DrawTextLine(
        hdc,
        L"Find anime by title, then add results to your Anime tab.",
        searchHint,
        bodyFont_,
        kTextSecondary,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    RECT inputShell {
        animeSearchEditRect_.left - Dips(2),
        animeSearchEditRect_.top - Dips(1),
        animeSearchButtonRect_.right + Dips(2),
        animeSearchEditRect_.bottom + Dips(1)
    };
    FillRoundRect(hdc, inputShell, Dips(12), kInputBackground);
    StrokeRoundRect(hdc, inputShell, Dips(12), kBorder);

    RECT searchIcon {
        inputShell.left + Dips(14),
        inputShell.top + Dips(10),
        inputShell.left + Dips(30),
        inputShell.bottom - Dips(10)
    };
    Gdiplus::Graphics iconGraphics(hdc);
    iconGraphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    iconGraphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    Gdiplus::Color iconColor;
    iconColor.SetFromCOLORREF(kTextSecondary);
    Gdiplus::Pen iconPen(
        iconColor,
        static_cast<Gdiplus::REAL>(std::max(1, Dips(2))));
    const Gdiplus::REAL iconLeft = static_cast<Gdiplus::REAL>(searchIcon.left);
    const Gdiplus::REAL iconTop = static_cast<Gdiplus::REAL>(searchIcon.top);
    const Gdiplus::REAL iconSize = static_cast<Gdiplus::REAL>(std::min(searchIcon.right - searchIcon.left, searchIcon.bottom - searchIcon.top) - Dips(4));
    iconGraphics.DrawEllipse(&iconPen, iconLeft, iconTop, iconSize, iconSize);
    iconGraphics.DrawLine(
        &iconPen,
        iconLeft + iconSize - Dips(1),
        iconTop + iconSize - Dips(1),
        static_cast<Gdiplus::REAL>(searchIcon.right),
        static_cast<Gdiplus::REAL>(searchIcon.bottom));

    PaintButton(hdc, animeSearchButtonRect_, animeSearching_ ? L"Searching..." : L"Search", true, animeSearching_, !animeSearching_ && !animeRefreshing_);

    RECT statusRect {
        searchPanel.left + Dips(22),
        animeSearchEditRect_.bottom + Dips(10),
        searchPanel.right - Dips(22),
        animeSearchEditRect_.bottom + Dips(36)
    };
    DrawTextLine(
        hdc,
        animeStatusMessage_.empty() ? L"Type an anime title, then search." : animeStatusMessage_.c_str(),
        statusRect,
        bodyFont_,
        kTextSecondary,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    if (animeSearching_)
    {
        RECT resultsBody = animeResultsRect_;
        resultsBody.top = animeSearchEditRect_.bottom + Dips(48);
        paintEmpty(resultsBody, L"Searching AniList...", L"Results will appear here.");
    }
    else if (animeSearchHasRun_ && animeSearchResults_.empty())
    {
        RECT resultsBody = animeResultsRect_;
        resultsBody.top = animeSearchEditRect_.bottom + Dips(48);
        paintEmpty(resultsBody, L"No matches found.", L"Try a different title.");
    }
    else if (!animeSearchResults_.empty())
    {
        RECT resultsTitle {
            animeResultsRect_.left + Dips(22),
            animeSearchEditRect_.bottom + Dips(48),
            animeResultsRect_.right - Dips(22),
            animeSearchEditRect_.bottom + Dips(74)
        };
        DrawTextLine(hdc, L"Results", resultsTitle, bodyFont_, kTextSecondary, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

        const int visibleRows = static_cast<int>(animeSearchResults_.size());
        for (int index = 0; index < visibleRows; ++index)
        {
            const AnimeSearchResult& result = animeSearchResults_[static_cast<size_t>(index)];
            RECT card = AnimeSearchResultCardRect(static_cast<size_t>(index));
            FillRoundRect(hdc, card, Dips(14), kButtonBackground);

            RECT cover {
                card.left + Dips(14),
                card.top + Dips(14),
                card.right - Dips(14),
                card.top + Dips(318)
            };
            auto coverBitmap = animeCoverCache_.find(result.coverImageUrl);
            if (coverBitmap != animeCoverCache_.end() && coverBitmap->second)
            {
                FillRoundRect(hdc, cover, Dips(12), kInputBackground);
                PaintBitmapCover(hdc, coverBitmap->second.get(), cover, Dips(12));
            }
            else
            {
                FillRoundRect(hdc, cover, Dips(12), kInputBackground);
                DrawTextLine(
                    hdc,
                    result.format.empty() ? L"TV" : result.format.c_str(),
                    cover,
                    bodyFont_,
                    kAccent,
                    DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            }

            RECT addRect = AnimeSearchResultAddRect(static_cast<size_t>(index));
            RECT titleLine {
                card.left + Dips(16),
                cover.bottom + Dips(10),
                card.right - Dips(16),
                cover.bottom + Dips(56)
            };
            DrawTextLine(hdc, result.title.c_str(), titleLine, navFont_, kTextPrimary, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);

            std::wstring meta = result.status;
            if (result.seasonYear > 0)
            {
                meta += meta.empty() ? L"" : L" / ";
                meta += std::to_wstring(result.seasonYear);
            }
            if (result.episodes > 0)
            {
                meta += meta.empty() ? L"" : L" / ";
                meta += std::to_wstring(result.episodes) + L" eps";
            }
            RECT metaLine = titleLine;
            metaLine.top = titleLine.bottom + Dips(4);
            metaLine.bottom = addRect.top - Dips(8);
            DrawTextLine(hdc, meta.empty() ? L"Anime" : meta.c_str(), metaLine, bodyFont_, kTextSecondary, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

            PaintButton(
                hdc,
                addRect,
                AnimeTrackerService::ContainsAnime(animeWatchList_, result.anilistId) ? L"Added" : L"Add",
                false,
                false,
                !AnimeTrackerService::ContainsAnime(animeWatchList_, result.anilistId));
        }

        if (animeCanLoadMore_)
        {
            PaintButton(hdc, animeLoadMoreButtonRect_, L"Load more", false, false, !animeSearching_ && !animeRefreshing_);
        }
    }
        return;
    }

    const std::vector<size_t> visibleEntries = VisibleAnimeEntryIndexes();
    paintPanel(animeListRect_, L"Watchlist", std::to_wstring(animeWatchList_.anime.size()) + L" saved");
    PaintDropdownButton(
        hdc,
        animeFilterButtonRect_,
        animeFilterAll_ ? L"All statuses" : AnimeTrackerService::UserStatusLabel(animeFilter_).c_str(),
        true);
    PaintButton(hdc, animeRefreshAllButtonRect_, animeRefreshing_ ? L"Refreshing..." : L"Refresh All", false, animeRefreshing_, !animeRefreshing_ && !animeWatchList_.anime.empty());

    if (visibleEntries.empty())
    {
        paintEmpty(
            animeListRect_,
            animeWatchList_.anime.empty() ? L"No anime saved yet." : L"No matches for this filter.",
            animeWatchList_.anime.empty() ? L"Add something from search results." : L"Use the filter menu above.");
    }
    else
    {
        RECT selectHint {
            animeListRect_.left + Dips(22),
            animeFilterButtonRect_.bottom + Dips(8),
            animeRefreshAllButtonRect_.left - Dips(16),
            animeFilterButtonRect_.bottom + Dips(34)
        };
        DrawTextLine(
            hdc,
            L"Select an anime from your watchlist to edit progress, notes, and schedule details below.",
            selectHint,
            bodyFont_,
            kTextSecondary,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

        const int rowHeight = Dips(68);
        const int rowTop = static_cast<int>(animeListRect_.top) + Dips(128);
        const int maxRows = std::max(1, (static_cast<int>(animeListRect_.bottom) - rowTop - Dips(14)) / rowHeight);
        const int visibleRows = std::min<int>(static_cast<int>(visibleEntries.size()), maxRows);
        for (int visibleIndex = 0; visibleIndex < visibleRows; ++visibleIndex)
        {
            const size_t entryIndex = visibleEntries[static_cast<size_t>(visibleIndex)];
            const AnimeEntry& entry = animeWatchList_.anime[entryIndex];
            RECT row = AnimeListRowRect(static_cast<size_t>(visibleIndex));
            const bool selected = static_cast<int>(entryIndex) == selectedAnimeIndex_;
            FillRoundRect(hdc, row, Dips(12), selected ? kAccentSoft : kButtonBackground);

            RECT textTitle {
                row.left + Dips(12),
                row.top + Dips(5),
                row.right - Dips(250),
                row.top + Dips(29)
            };
            std::wstring title = entry.favorite ? L"* " + entry.title : entry.title;
            DrawTextLine(hdc, title.c_str(), textTitle, navFont_, kTextPrimary, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

            RECT textMeta = textTitle;
            textMeta.top = textTitle.bottom + Dips(1);
            textMeta.bottom = row.bottom - Dips(5);
            std::wstring meta = AnimeStatusText(entry) + L" / " + AnimeProgressText(entry);
            if (entry.nextAiringEpisode.hasValue)
            {
                meta += L" / Ep " + std::to_wstring(entry.nextAiringEpisode.episode) + L" " +
                    AnimeTrackerService::CountdownLabel(entry.nextAiringEpisode.airingAt);
            }
            DrawTextLine(hdc, meta.c_str(), textMeta, bodyFont_, kTextSecondary, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

            PaintButton(hdc, AnimeListActionRect(static_cast<size_t>(visibleIndex), 0), L"AniList", false);
            PaintButton(hdc, AnimeListActionRect(static_cast<size_t>(visibleIndex), 1), L"Refresh", false, animeRefreshing_, !animeRefreshing_);
            PaintButton(hdc, AnimeListActionRect(static_cast<size_t>(visibleIndex), 2), L"Remove", false, false, !animeRefreshing_);
        }
    }

    RECT detailPanel {
        contentRect_.left + margin,
        animeListRect_.bottom + Dips(18),
        contentRect_.right - margin,
        animeNotesEditRect_.bottom + Dips(22)
    };
    FillRoundRect(hdc, detailPanel, Dips(16), kPanelBackground);
    StrokeRoundRect(hdc, detailPanel, Dips(16), kBorder);

    RECT detailTitle {
        detailPanel.left + Dips(22),
        detailPanel.top + Dips(14),
        detailPanel.right - Dips(22),
        detailPanel.top + Dips(42)
    };
    DrawTextLine(hdc, L"Details & Schedule", detailTitle, navFont_, kTextPrimary, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    const bool hasSelection =
        selectedAnimeIndex_ >= 0 &&
        selectedAnimeIndex_ < static_cast<int>(animeWatchList_.anime.size());
    if (!hasSelection)
    {
        RECT emptyRect {
            detailPanel.left + Dips(22),
            detailTitle.bottom + Dips(8),
            detailPanel.right - Dips(22),
            animeStatusButtonRect_.bottom
        };
        emptyRect.top = detailTitle.bottom + Dips(18);
        paintEmpty(emptyRect, L"No anime selected.", L"Choose a row in your watchlist to edit notes and progress.");
    }
    else
    {
        const AnimeEntry& selectedEntry = animeWatchList_.anime[static_cast<size_t>(selectedAnimeIndex_)];
        RECT selectedTitle {
            detailPanel.left + Dips(22),
            detailTitle.bottom + Dips(16),
            detailPanel.right - Dips(22),
            detailTitle.bottom + Dips(44)
        };
        DrawTextLine(hdc, selectedEntry.title.c_str(), selectedTitle, navFont_, kTextPrimary, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

        RECT selectedMeta = selectedTitle;
        selectedMeta.top = selectedTitle.bottom + Dips(2);
        selectedMeta.bottom = selectedMeta.top + Dips(24);
        std::wstring selectedInfo = AnimeProgressText(selectedEntry) + L" / " + AnimeStatusText(selectedEntry);
        if (!selectedEntry.lastRefreshed.empty())
        {
            selectedInfo += L" / " + FriendlyRefreshedLabel(selectedEntry.lastRefreshed);
        }
        DrawTextLine(hdc, selectedInfo.c_str(), selectedMeta, bodyFont_, kTextSecondary, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

        PaintButton(hdc, animeStatusButtonRect_, AnimeTrackerService::UserStatusLabel(selectedEntry.userStatus).c_str(), false);
        PaintButton(hdc, animeEpisodeMinusButtonRect_, L"-", false, false, selectedEntry.currentEpisode > 0);
        PaintButton(hdc, animeEpisodePlusButtonRect_, L"+", false, false, selectedEntry.totalEpisodes == 0 || selectedEntry.currentEpisode < selectedEntry.totalEpisodes);
        PaintButton(hdc, animeFavoriteButtonRect_, selectedEntry.favorite ? L"Favorited" : L"Favorite", false, selectedEntry.favorite);
    }

    RECT upcomingTitle {
        animeUpcomingRect_.left,
        animeUpcomingRect_.top,
        animeUpcomingRect_.right,
        animeUpcomingRect_.top + Dips(28)
    };
    DrawTextLine(hdc, L"Upcoming episodes", upcomingTitle, navFont_, kTextPrimary, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    std::vector<const AnimeEntry*> upcomingEntries;
    for (const AnimeEntry& entry : animeWatchList_.anime)
    {
        if (entry.nextAiringEpisode.hasValue)
        {
            upcomingEntries.push_back(&entry);
        }
    }
    std::sort(upcomingEntries.begin(), upcomingEntries.end(), [](const AnimeEntry* left, const AnimeEntry* right)
    {
        return left->nextAiringEpisode.airingAt < right->nextAiringEpisode.airingAt;
    });

    if (upcomingEntries.empty())
    {
        RECT emptyUpcoming = animeUpcomingRect_;
        emptyUpcoming.top = upcomingTitle.bottom + Dips(4);
        paintEmpty(emptyUpcoming, L"No upcoming episodes.", L"Refresh entries to pull current airing info.");
    }
    else
    {
        int rowTop = upcomingTitle.bottom + Dips(8);
        const int rowHeight = Dips(50);
        const int visibleRows = std::min<int>(static_cast<int>(upcomingEntries.size()), 3);
        for (int index = 0; index < visibleRows; ++index)
        {
            const AnimeEntry& entry = *upcomingEntries[static_cast<size_t>(index)];
            RECT row {
                animeUpcomingRect_.left + Dips(18),
                rowTop,
                animeUpcomingRect_.right - Dips(18),
                rowTop + rowHeight
            };
            RECT titleLine {
                row.left,
                row.top,
                row.right,
                row.top + Dips(24)
            };
            std::wstring title = entry.title + L" / Ep " +
                std::to_wstring(entry.nextAiringEpisode.episode);
            DrawTextLine(hdc, title.c_str(), titleLine, bodyFont_, kTextPrimary, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

            RECT timeLine = titleLine;
            timeLine.top = titleLine.bottom + Dips(2);
            timeLine.bottom = timeLine.top + Dips(22);
            std::wstring time = AnimeTrackerService::AiringDateLabel(entry.nextAiringEpisode.airingAt) + L" " +
                LocalTimeZoneLabel() + L" / " +
                AnimeTrackerService::CountdownLabel(entry.nextAiringEpisode.airingAt);
            DrawTextLine(hdc, time.c_str(), timeLine, bodyFont_, kTextSecondary, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            rowTop += rowHeight;
        }
    }

    const std::vector<AnimeRelation> sequels = VisibleUpcomingSequels();
    RECT scheduleRule {
        animeSequelsRect_.left - Dips(10),
        animeSequelsRect_.top + Dips(4),
        animeSequelsRect_.left - Dips(9),
        animeSequelsRect_.bottom - Dips(8)
    };
    FillSolidRect(hdc, scheduleRule, kBorder);

    RECT sequelsTitle {
        animeSequelsRect_.left,
        animeSequelsRect_.top,
        animeSequelsRect_.right,
        animeSequelsRect_.top + Dips(28)
    };
    DrawTextLine(hdc, L"Seasons & sequels", sequelsTitle, navFont_, kTextPrimary, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    if (sequels.empty())
    {
        RECT emptySequels = animeSequelsRect_;
        emptySequels.top = sequelsTitle.bottom + Dips(4);
        paintEmpty(emptySequels, L"No upcoming sequels found.", L"Refresh your saved entries to update relations.");
    }
    else
    {
        int rowTop = sequelsTitle.bottom + Dips(8);
        const int rowHeight = Dips(46);
        const int visibleRows = std::min<int>(static_cast<int>(sequels.size()), 3);
        for (int index = 0; index < visibleRows; ++index)
        {
            const AnimeRelation& relation = sequels[static_cast<size_t>(index)];
            RECT row {
                animeSequelsRect_.left + Dips(14),
                rowTop,
                animeSequelsRect_.right - Dips(14),
                rowTop + rowHeight - Dips(6)
            };
            FillRoundRect(hdc, row, Dips(10), kButtonBackground);
            RECT addRect = AnimeSequelActionRect(static_cast<size_t>(index), 0);
            RECT openRect = AnimeSequelActionRect(static_cast<size_t>(index), 1);
            RECT textRect {
                row.left + Dips(12),
                row.top,
                addRect.left - Dips(10),
                row.bottom
            };
            std::wstring text = relation.title;
            if (relation.seasonYear > 0)
            {
                text += L" / " + std::to_wstring(relation.seasonYear);
            }
            DrawTextLine(hdc, text.c_str(), textRect, bodyFont_, kTextPrimary, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            PaintButton(hdc, addRect, L"Add", false);
            PaintButton(hdc, openRect, L"Open", false, false, !relation.siteUrl.empty());
            rowTop += rowHeight;
        }
    }

    if (hasSelection)
    {
        RECT notesLabel {
            animeNotesEditRect_.left,
            animeNotesEditRect_.top - Dips(34),
            animeNotesEditRect_.left + Dips(220),
            animeNotesEditRect_.top - Dips(8)
        };
        DrawTextLine(hdc, L"Notes", notesLabel, navFont_, kTextPrimary, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        RECT notesStatus {
            notesLabel.right + Dips(10),
            notesLabel.top,
            animeNotesEditRect_.right,
            notesLabel.bottom
        };
        const std::wstring status = animeNotesStatusText_.empty()
            ? L"Autosaves after you stop typing."
            : animeNotesStatusText_;
        DrawTextLine(hdc, status.c_str(), notesStatus, bodyFont_, kTextSecondary, DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        FillRoundRect(hdc, animeNotesEditRect_, Dips(10), kInputBackground);
        StrokeRoundRect(hdc, animeNotesEditRect_, Dips(10), kBorder);
    }
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
        L"Customize startup, downloads, appearance, updates, and app information.",
        subtitleRect,
        bodyFont_,
        kTextSecondary,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    RECT railPanel {
        contentRect_.left + margin,
        contentTop + Dips(132),
        contentRect_.left + margin + Dips(220),
        contentTop + Dips(660)
    };
    FillRoundRect(hdc, railPanel, Dips(16), kPanelBackground);
    StrokeRoundRect(hdc, railPanel, Dips(16), kBorder);

    PaintSettingsSectionTab(hdc, settingsGeneralTabRect_, L"General", SettingsSection::General);
    PaintSettingsSectionTab(hdc, settingsAppearanceTabRect_, L"Appearance", SettingsSection::Appearance);
    PaintSettingsSectionTab(hdc, settingsUpdatesTabRect_, L"Updates", SettingsSection::Updates);
    PaintSettingsSectionTab(hdc, settingsAboutTabRect_, L"About", SettingsSection::About);

    RECT panel {
        railPanel.right + Dips(18),
        railPanel.top,
        contentRect_.right - margin,
        railPanel.bottom
    };
    FillRoundRect(hdc, panel, Dips(16), kPanelBackground);
    StrokeRoundRect(hdc, panel, Dips(16), kBorder);

    auto drawSectionTitle = [&](const wchar_t* title, const wchar_t* subtitle)
    {
        RECT sectionTitle {
            panel.left + Dips(28),
            panel.top + Dips(24),
            panel.right - Dips(28),
            panel.top + Dips(56)
        };
        DrawTextLine(hdc, title, sectionTitle, navFont_, kTextPrimary, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        RECT sectionSubtitle = sectionTitle;
        sectionSubtitle.top = sectionTitle.bottom + Dips(2);
        sectionSubtitle.bottom = sectionSubtitle.top + Dips(26);
        DrawTextLine(hdc, subtitle, sectionSubtitle, bodyFont_, kTextSecondary, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    };

    auto drawLabel = [&](const wchar_t* label, const RECT& control)
    {
        RECT labelRect {
            control.left,
            control.top - Dips(28),
            control.right,
            control.top - Dips(6)
        };
        DrawTextLine(hdc, label, labelRect, bodyFont_, kTextSecondary, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    };

    if (settingsSection_ == SettingsSection::General)
    {
        drawSectionTitle(L"General", L"Choose where files save and which page opens first.");
        drawLabel(L"Default output folder", settingsDefaultFolderRect_);
        FillRoundRect(hdc, settingsDefaultFolderRect_, Dips(10), kInputBackground);
        StrokeRoundRect(hdc, settingsDefaultFolderRect_, Dips(10), kBorder);
        RECT folderText = ShrinkRect(settingsDefaultFolderRect_, Dips(12), 0);
        DrawTextLine(
            hdc,
            appSettings_.defaultOutputFolder.empty() ? L"Choose a folder" : appSettings_.defaultOutputFolder.wstring().c_str(),
            folderText,
            bodyFont_,
            kTextPrimary,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        PaintButton(hdc, settingsBrowseDefaultFolderButtonRect_, L"Browse", false);

        drawLabel(L"Open on startup", settingsStartPageButtonRect_);
        PaintDropdownButton(hdc, settingsStartPageButtonRect_, StartPageLabel().c_str(), true);
        return;
    }

    if (settingsSection_ == SettingsSection::Appearance)
    {
        drawSectionTitle(L"Appearance", L"Switch themes and control the clock shown in the top-right corner.");
        drawLabel(L"Theme", settingsThemeButtonRect_);
        PaintDropdownButton(hdc, settingsThemeButtonRect_, ThemeLabel().c_str(), true);

        drawLabel(L"Date and time format", settingsClockFormatButtonRect_);
        PaintDropdownButton(hdc, settingsClockFormatButtonRect_, ClockFormatLabel().c_str(), true);

        RECT preview {
            settingsClockFormatButtonRect_.left,
            settingsClockFormatButtonRect_.bottom + Dips(18),
            panel.right - Dips(28),
            settingsClockFormatButtonRect_.bottom + Dips(58)
        };
        FillRoundRect(hdc, preview, Dips(10), kInputBackground);
        StrokeRoundRect(hdc, preview, Dips(10), kBorder);
        RECT previewText = ShrinkRect(preview, Dips(12), 0);
        std::wstring previewValue = L"Preview: " + CurrentDateTimeLabel();
        DrawTextLine(hdc, previewValue.c_str(), previewText, bodyFont_, kTextSecondary, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        return;
    }

    if (settingsSection_ == SettingsSection::Updates)
    {
        drawSectionTitle(L"Updates", L"Check here for releases, then download and install available updates inside the app.");
        PaintButton(
            hdc,
            settingsCheckUpdatesButtonRect_,
            updateChecking_ ? L"Checking..." : L"Check for Updates",
            true,
            updateChecking_,
            !updateChecking_ && !updateInstalling_);

        RECT resultPanel {
            panel.left + Dips(28),
            settingsCheckUpdatesButtonRect_.bottom + Dips(28),
            panel.right - Dips(28),
            panel.bottom - Dips(28)
        };
        FillRoundRect(hdc, resultPanel, Dips(14), kInputBackground);
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
        DrawTextLine(hdc, detail.c_str(), detailRect, bodyFont_, kTextSecondary, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

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
            const size_t noteCount = std::min<size_t>(updateResult_.releaseNotes.size(), 5);
            for (size_t index = 0; index < noteCount; ++index)
            {
                std::wstring note = L"- " + updateResult_.releaseNotes[index];
                RECT noteRect { notesTitle.left, noteTop, notesTitle.right, noteTop + Dips(24) };
                DrawTextLine(hdc, note.c_str(), noteRect, bodyFont_, kTextSecondary, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                noteTop += Dips(24);
            }

            PaintButton(hdc, settingsDownloadUpdateButtonRect_, updateInstalling_ ? L"Working..." : L"Download Update", true, updateInstalling_, !updateInstalling_);
            if (!updateInstallStatus_.empty())
            {
                RECT installStatusRect {
                    settingsDownloadUpdateButtonRect_.right + Dips(16),
                    settingsDownloadUpdateButtonRect_.top,
                    resultPanel.right - Dips(20),
                    settingsDownloadUpdateButtonRect_.bottom
                };
                DrawTextLine(
                    hdc,
                    updateInstallStatus_.c_str(),
                    installStatusRect,
                    bodyFont_,
                    kTextSecondary,
                    DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            }
        }
        return;
    }

    drawSectionTitle(L"About", L"App details, project links, and bundled dependencies.");

    struct InfoRow
    {
        const wchar_t* label;
        std::wstring value;
    };

    const std::vector<InfoRow> rows {
        { L"App name", L"Rex's Toolkit" },
        { L"Version", APP_VERSION },
        { L"Build date", APP_BUILD_DATE },
        { L"Developer", L"Rexarater" },
        { L"Licenses", L"Rex's Toolkit app assets and code are project-owned unless noted." },
        { L"Third-party tools", L"yt-dlp, FFmpeg, AniList API, Windows Imaging Component" }
    };

    int rowTop = panel.top + Dips(94);
    for (const InfoRow& row : rows)
    {
        RECT labelRect {
            panel.left + Dips(28),
            rowTop,
            panel.left + Dips(168),
            rowTop + Dips(28)
        };
        RECT valueRect {
            labelRect.right + Dips(16),
            rowTop,
            panel.right - Dips(28),
            rowTop + Dips(28)
        };
        DrawTextLine(hdc, row.label, labelRect, bodyFont_, kTextSecondary, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        DrawTextLine(hdc, row.value.c_str(), valueRect, bodyFont_, kTextPrimary, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        rowTop += Dips(36);
    }

    RECT githubLabel {
        panel.left + Dips(28),
        rowTop + Dips(4),
        panel.right - Dips(28),
        rowTop + Dips(28)
    };
    DrawTextLine(hdc, L"GitHub link", githubLabel, bodyFont_, kTextSecondary, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    PaintButton(hdc, settingsGithubButtonRect_, L"GitHub", false);
    PaintButton(hdc, settingsReportIssueButtonRect_, L"Report Issue", false);
}

void ToolkitApp::PaintSettingsSectionTab(HDC hdc, const RECT& bounds, const wchar_t* label, SettingsSection section)
{
    const bool selected = settingsSection_ == section;
    const bool hovered = hasHoveredButton_ && EqualRect(&bounds, &hoveredButtonRect_);
    FillRoundRect(hdc, bounds, Dips(12), selected ? kAccentSoft : hovered ? kPanelHover : kPanelBackground);
    StrokeRoundRect(hdc, bounds, Dips(12), selected ? kAccent : kBorder);
    RECT textRect = ShrinkRect(bounds, Dips(14), 0);
    DrawTextLine(
        hdc,
        label,
        textRect,
        navFont_,
        selected || hovered ? kTextPrimary : kTextSecondary,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

void ToolkitApp::PaintProgressBar(HDC hdc, const RECT& bounds, double progress)
{
    FillRoundRect(hdc, bounds, Dips(8), kBorder);

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
    UpdateAnimeTrackerControls();

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
    UpdateAnimeTrackerControls();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ToolkitApp::PaintDropdown(HDC hdc)
{
    if (activeDropdown_ == DropdownKind::None || dropdownLabels_.empty())
    {
        return;
    }

    FillRoundRect(hdc, dropdownRect_, Dips(12), kDropdownBackground);
    StrokeRoundRect(hdc, dropdownRect_, Dips(12), kBorder);

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
            FillRoundRect(hdc, itemRect, Dips(9), selected ? kDropdownSelected : kDropdownHover);
        }

        RECT checkRect {
            itemRect.left + Dips(10),
            itemRect.top,
            itemRect.left + Dips(34),
            itemRect.bottom
        };
        if (selected)
        {
            const int markerSize = Dips(8);
            const int markerLeft = checkRect.left + ((checkRect.right - checkRect.left) - markerSize) / 2;
            const int markerTop = checkRect.top + ((checkRect.bottom - checkRect.top) - markerSize) / 2;
            Gdiplus::Graphics markerGraphics(hdc);
            markerGraphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            markerGraphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
            Gdiplus::Color markerColor;
            markerColor.SetFromCOLORREF(kAccent);
            Gdiplus::SolidBrush markerBrush(markerColor);
            markerGraphics.FillEllipse(
                &markerBrush,
                static_cast<Gdiplus::REAL>(markerLeft),
                static_cast<Gdiplus::REAL>(markerTop),
                static_cast<Gdiplus::REAL>(markerSize),
                static_cast<Gdiplus::REAL>(markerSize));
        }

        RECT textRect = itemRect;
        textRect.left += Dips(38);
        textRect.right -= Dips(12);
        DrawTextLine(
            hdc,
            dropdownLabels_[index].c_str(),
            textRect,
            navFont_,
            enabled ? kTextPrimary : kDisabledText,
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

    if (kind == DropdownKind::AnimeFilter)
    {
        animeFilterAll_ = value == 0;
        if (!animeFilterAll_)
        {
            animeFilter_ = static_cast<AnimeUserStatus>(value - 1);
        }
        selectedAnimeIndex_ = -1;
        SetWindowTextW(animeNotesEdit_, L"");
        UpdateAnimeTrackerControls();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }

    if (kind == DropdownKind::SettingsStartPage)
    {
        appSettings_.startPage = value == 1 ? DefaultStartPage::AllTools : DefaultStartPage::Favorites;
        SaveAppSettings();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }

    if (kind == DropdownKind::SettingsClockFormat)
    {
        appSettings_.clockFormat = static_cast<ClockFormat>(value);
        SaveAppSettings();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }

    if (kind == DropdownKind::SettingsTheme)
    {
        appSettings_.theme = value == 1 ? AppTheme::Light : AppTheme::Dark;
        ApplyTheme();
        SaveAppSettings();
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
            for (const RECT& candidate : {
                settingsGeneralTabRect_,
                settingsAppearanceTabRect_,
                settingsUpdatesTabRect_,
                settingsAboutTabRect_
            })
            {
                rect = hit(candidate);
                if (rect.right > rect.left) return rect;
            }

            if (settingsSection_ == SettingsSection::General)
            {
                for (const RECT& candidate : { settingsBrowseDefaultFolderButtonRect_, settingsStartPageButtonRect_ })
                {
                    rect = hit(candidate);
                    if (rect.right > rect.left) return rect;
                }
            }
            else if (settingsSection_ == SettingsSection::Appearance)
            {
                for (const RECT& candidate : { settingsThemeButtonRect_, settingsClockFormatButtonRect_ })
                {
                    rect = hit(candidate);
                    if (rect.right > rect.left) return rect;
                }
            }
            else if (settingsSection_ == SettingsSection::Updates)
            {
                rect = hit(settingsCheckUpdatesButtonRect_);
                if (rect.right > rect.left) return rect;
                if (hasUpdateResult_ && updateResult_.status == UpdateCheckStatus::UpdateAvailable && !updateInstalling_)
                {
                    rect = hit(settingsDownloadUpdateButtonRect_);
                    if (rect.right > rect.left) return rect;
                }
            }
            else if (settingsSection_ == SettingsSection::About)
            {
                for (const RECT& candidate : { settingsGithubButtonRect_, settingsReportIssueButtonRect_ })
                {
                    rect = hit(candidate);
                    if (rect.right > rect.left) return rect;
                }
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
    else if (currentTool_ == ToolKind::AnimeTracker)
    {
        std::vector<RECT> candidates {
            animeSearchTabRect_,
            animeListTabRect_,
        };

        if (animeTrackerTab_ == AnimeTrackerTab::Search)
        {
            candidates.push_back(animeSearchButtonRect_);
            if (animeCanLoadMore_ && animeSearchHasRun_ && !animeSearchResults_.empty())
            {
                candidates.push_back(animeLoadMoreButtonRect_);
            }

            const int resultRows = static_cast<int>(animeSearchResults_.size());
            for (int index = 0; index < resultRows; ++index)
            {
                candidates.push_back(AnimeSearchResultAddRect(static_cast<size_t>(index)));
            }
        }
        else
        {
            candidates.push_back(animeFilterButtonRect_);
            candidates.push_back(animeRefreshAllButtonRect_);
            if (selectedAnimeIndex_ >= 0 && selectedAnimeIndex_ < static_cast<int>(animeWatchList_.anime.size()))
            {
                candidates.push_back(animeStatusButtonRect_);
                candidates.push_back(animeEpisodeMinusButtonRect_);
                candidates.push_back(animeEpisodePlusButtonRect_);
                candidates.push_back(animeFavoriteButtonRect_);
            }

            const std::vector<size_t> visibleEntries = VisibleAnimeEntryIndexes();
            const int listRows = static_cast<int>(visibleEntries.size());
            for (int visibleIndex = 0; visibleIndex < listRows; ++visibleIndex)
            {
                for (int action = 0; action < 3; ++action)
                {
                    candidates.push_back(AnimeListActionRect(static_cast<size_t>(visibleIndex), action));
                }
            }

            const std::vector<AnimeRelation> sequels = VisibleUpcomingSequels();
            const int sequelRows = std::min<int>(static_cast<int>(sequels.size()), 3);
            for (int index = 0; index < sequelRows; ++index)
            {
                candidates.push_back(AnimeSequelActionRect(static_cast<size_t>(index), 0));
                candidates.push_back(AnimeSequelActionRect(static_cast<size_t>(index), 1));
            }
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
    FillRoundRect(hdc, bounds, Dips(18), kInputBackground);
    StrokeRoundRect(hdc, bounds, Dips(18), kBorder);

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
    else if (tool == ToolKind::AnimeTracker)
    {
        RECT iconBounds = ShrinkRect(bounds, Dips(16), Dips(16));
        PaintAniListIcon(hdc, iconBounds);
        return;
    }

    if (icon)
    {
        RECT iconBounds = ShrinkRect(bounds, Dips(13), Dips(13));
        PaintBitmapTinted(hdc, icon, iconBounds, kTextPrimary);
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

void ToolkitApp::PaintAniListIcon(HDC hdc, const RECT& bounds)
{
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0)
    {
        return;
    }

    const float size = static_cast<float>(std::min(width, height));
    const float offsetX = static_cast<float>(bounds.left) + (static_cast<float>(width) - size) / 2.0f;
    const float offsetY = static_cast<float>(bounds.top) + (static_cast<float>(height) - size) / 2.0f;
    const float scale = size / 24.0f;
    auto x = [&](float value) { return offsetX + value * scale; };
    auto y = [&](float value) { return offsetY + value * scale; };

    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

    Gdiplus::GraphicsPath path;
    path.StartFigure();
    path.AddLine(x(6.361f), y(2.943f), x(0.0f), y(21.056f));
    path.AddLine(x(0.0f), y(21.056f), x(4.942f), y(21.056f));
    path.AddLine(x(4.942f), y(21.056f), x(6.019f), y(17.923f));
    path.AddLine(x(6.019f), y(17.923f), x(11.4f), y(17.923f));
    path.AddLine(x(11.4f), y(17.923f), x(12.452f), y(21.056f));
    path.AddLine(x(12.452f), y(21.056f), x(22.9f), y(21.056f));
    path.AddBezier(x(22.9f), y(21.056f), x(23.61f), y(21.056f), x(24.0f), y(20.665f), x(24.0f), y(19.955f));
    path.AddLine(x(24.0f), y(19.955f), x(24.0f), y(17.53f));
    path.AddBezier(x(24.0f), y(17.53f), x(24.0f), y(16.82f), x(23.61f), y(16.429f), x(22.9f), y(16.429f));
    path.AddLine(x(22.9f), y(16.429f), x(16.417f), y(16.429f));
    path.AddLine(x(16.417f), y(16.429f), x(16.417f), y(4.045f));
    path.AddBezier(x(16.417f), y(4.045f), x(16.417f), y(3.335f), x(16.025f), y(2.943f), x(15.316f), y(2.943f));
    path.AddLine(x(15.316f), y(2.943f), x(12.894f), y(2.943f));
    path.AddBezier(x(12.894f), y(2.943f), x(12.184f), y(2.943f), x(11.793f), y(3.335f), x(11.793f), y(4.045f));
    path.AddLine(x(11.793f), y(4.045f), x(11.793f), y(5.109f));
    path.AddLine(x(11.793f), y(5.109f), x(11.035f), y(2.943f));
    path.CloseFigure();

    path.StartFigure();
    path.AddLine(x(8.685f), y(8.891f), x(10.373f), y(13.909f));
    path.AddLine(x(10.373f), y(13.909f), x(7.144f), y(13.909f));
    path.CloseFigure();

    Gdiplus::Color iconColor;
    iconColor.SetFromCOLORREF(kTextPrimary);
    Gdiplus::SolidBrush brush(iconColor);
    graphics.FillPath(&brush, &path);
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

    HWND focusedWindow = GetFocus();
    if (focusedWindow == animeSearchEdit_ && !IsPointInRect(animeSearchEditRect_, point))
    {
        SetFocus(hwnd_);
    }
    else if (focusedWindow == animeNotesEdit_ && !IsPointInRect(animeNotesEditRect_, point))
    {
        SetFocus(hwnd_);
    }
    else if (focusedWindow == mediaUrlEdit_ && !IsPointInRect(mediaUrlEditRect_, point))
    {
        SetFocus(hwnd_);
    }
    else if (focusedWindow == mediaFileNameEdit_ && !IsPointInRect(mediaFileNameEditRect_, point))
    {
        SetFocus(hwnd_);
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
        if (IsPointInRect(settingsGeneralTabRect_, point))
        {
            settingsSection_ = SettingsSection::General;
            CloseDropdown();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (IsPointInRect(settingsAppearanceTabRect_, point))
        {
            settingsSection_ = SettingsSection::Appearance;
            CloseDropdown();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (IsPointInRect(settingsUpdatesTabRect_, point))
        {
            settingsSection_ = SettingsSection::Updates;
            CloseDropdown();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (IsPointInRect(settingsAboutTabRect_, point))
        {
            settingsSection_ = SettingsSection::About;
            CloseDropdown();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }

        if (settingsSection_ == SettingsSection::General &&
            IsPointInRect(settingsBrowseDefaultFolderButtonRect_, point))
        {
            BrowseDefaultOutputFolder();
            return;
        }
        if (settingsSection_ == SettingsSection::General &&
            IsPointInRect(settingsStartPageButtonRect_, point))
        {
            ShowSettingsStartPageDropdown();
            return;
        }
        if (settingsSection_ == SettingsSection::Appearance &&
            IsPointInRect(settingsThemeButtonRect_, point))
        {
            ShowSettingsThemeDropdown();
            return;
        }
        if (settingsSection_ == SettingsSection::Appearance &&
            IsPointInRect(settingsClockFormatButtonRect_, point))
        {
            ShowSettingsClockFormatDropdown();
            return;
        }
        if (settingsSection_ == SettingsSection::Updates &&
            IsPointInRect(settingsCheckUpdatesButtonRect_, point) && !updateChecking_)
        {
            StartUpdateCheck();
            return;
        }
        if (settingsSection_ == SettingsSection::Updates &&
            IsPointInRect(settingsDownloadUpdateButtonRect_, point) &&
            hasUpdateResult_ &&
            updateResult_.status == UpdateCheckStatus::UpdateAvailable &&
            !updateInstalling_)
        {
            StartUpdateInstall();
            return;
        }
        if (settingsSection_ == SettingsSection::About &&
            IsPointInRect(settingsGithubButtonRect_, point))
        {
            ShellExecuteW(hwnd_, L"open", L"https://github.com/Rexarater/rex-toolkit", nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
        if (settingsSection_ == SettingsSection::About &&
            IsPointInRect(settingsReportIssueButtonRect_, point))
        {
            ShellExecuteW(hwnd_, L"open", L"https://github.com/Rexarater/rex-toolkit/issues", nullptr, nullptr, SW_SHOWNORMAL);
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

    if (currentPage_ == Page::Tool && currentTool_ == ToolKind::AnimeTracker)
    {
        if (IsPointInRect(backButtonRect_, point))
        {
            SelectPage(Page::AllTools);
            return;
        }
        if (IsPointInRect(animeSearchTabRect_, point))
        {
            animeTrackerTab_ = AnimeTrackerTab::Search;
            scrollOffsetY_ = 0;
            CloseDropdown();
            RecalculateLayout();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (IsPointInRect(animeListTabRect_, point))
        {
            animeTrackerTab_ = AnimeTrackerTab::Anime;
            scrollOffsetY_ = 0;
            CloseDropdown();
            RecalculateLayout();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }

        if (animeTrackerTab_ == AnimeTrackerTab::Search)
        {
            if (IsPointInRect(animeSearchButtonRect_, point) && !animeSearching_ && !animeRefreshing_)
            {
                StartAnimeSearch(false);
                return;
            }
            if (IsPointInRect(animeLoadMoreButtonRect_, point) && animeCanLoadMore_ && !animeSearching_ && !animeRefreshing_)
            {
                StartAnimeSearch(true);
                return;
            }

            const int resultRows = static_cast<int>(animeSearchResults_.size());
            for (int index = 0; index < resultRows; ++index)
            {
                if (IsPointInRect(AnimeSearchResultAddRect(static_cast<size_t>(index)), point))
                {
                    AddAnimeFromSearch(static_cast<size_t>(index));
                    return;
                }
            }
            return;
        }

        if (IsPointInRect(animeFilterButtonRect_, point))
        {
            ShowAnimeFilterDropdown();
            return;
        }
        if (IsPointInRect(animeRefreshAllButtonRect_, point) && !animeRefreshing_ && !animeWatchList_.anime.empty())
        {
            RefreshAllAnime();
            return;
        }

        const std::vector<size_t> visibleEntries = VisibleAnimeEntryIndexes();
        const int listRows = static_cast<int>(visibleEntries.size());
        for (int visibleIndex = 0; visibleIndex < listRows; ++visibleIndex)
        {
            const size_t entryIndex = visibleEntries[static_cast<size_t>(visibleIndex)];
            if (IsPointInRect(AnimeListActionRect(static_cast<size_t>(visibleIndex), 0), point))
            {
                if (entryIndex < animeWatchList_.anime.size() && !animeWatchList_.anime[entryIndex].siteUrl.empty())
                {
                    ShellExecuteW(hwnd_, L"open", animeWatchList_.anime[entryIndex].siteUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
                else
                {
                    SelectAnimeEntry(static_cast<int>(entryIndex));
                }
                return;
            }
            if (IsPointInRect(AnimeListActionRect(static_cast<size_t>(visibleIndex), 1), point) && !animeRefreshing_)
            {
                RefreshAnimeEntry(entryIndex);
                return;
            }
            if (IsPointInRect(AnimeListActionRect(static_cast<size_t>(visibleIndex), 2), point) && !animeRefreshing_)
            {
                RemoveAnimeEntry(entryIndex);
                return;
            }
            if (IsPointInRect(AnimeListRowRect(static_cast<size_t>(visibleIndex)), point))
            {
                SelectAnimeEntry(static_cast<int>(entryIndex));
                return;
            }
        }

        const std::vector<AnimeRelation> sequels = VisibleUpcomingSequels();
        const int sequelRows = std::min<int>(static_cast<int>(sequels.size()), 3);
        for (int index = 0; index < sequelRows; ++index)
        {
            if (IsPointInRect(AnimeSequelActionRect(static_cast<size_t>(index), 0), point))
            {
                AddAnimeFromRelation(static_cast<size_t>(index));
                return;
            }
            if (IsPointInRect(AnimeSequelActionRect(static_cast<size_t>(index), 1), point) && !sequels[static_cast<size_t>(index)].siteUrl.empty())
            {
                ShellExecuteW(hwnd_, L"open", sequels[static_cast<size_t>(index)].siteUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return;
            }
        }

        if (selectedAnimeIndex_ >= 0 && selectedAnimeIndex_ < static_cast<int>(animeWatchList_.anime.size()))
        {
            if (IsPointInRect(animeStatusButtonRect_, point))
            {
                CycleSelectedAnimeStatus();
                return;
            }
            if (IsPointInRect(animeEpisodeMinusButtonRect_, point))
            {
                DecrementSelectedAnimeEpisode();
                return;
            }
            if (IsPointInRect(animeEpisodePlusButtonRect_, point))
            {
                IncrementAnimeEpisode(static_cast<size_t>(selectedAnimeIndex_));
                return;
            }
            if (IsPointInRect(animeFavoriteButtonRect_, point))
            {
                ToggleSelectedAnimeFavorite();
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
        SaveAutoClickerSettings();
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

        SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(searchInputFont_), TRUE);
        SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(Dips(8), Dips(6)));
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

    auto moveEdit = [&](HWND edit, const RECT& rect, bool enabled)
    {
        if (!edit)
        {
            return;
        }

        const bool coveredByDropdown = activeDropdown_ != DropdownKind::None &&
            HasArea(dropdownRect_) &&
            RectsOverlap(rect, dropdownRect_);
        const bool inViewport = visible && !coveredByDropdown &&
            rect.top >= contentRect_.top &&
            rect.bottom <= contentRect_.bottom &&
            RectsOverlap(rect, contentRect_);
        ShowWindow(edit, inViewport ? SW_SHOW : SW_HIDE);
        EnableWindow(edit, inViewport && enabled);
        if (!inViewport)
        {
            return;
        }

        const int editLeftInset = Dips(10);
        const int editRightInset = Dips(10);
        const int editWidth = std::max(1, static_cast<int>(rect.right - rect.left) - editLeftInset - editRightInset);
        const int editHeight = std::max(1, Dips(24));
        const int editTop = static_cast<int>(rect.top) + ((rect.bottom - rect.top) - editHeight) / 2 + Dips(2);

        SetWindowPos(
            edit,
            nullptr,
            static_cast<int>(rect.left) + editLeftInset,
            editTop,
            editWidth,
            editHeight,
            SWP_NOZORDER | SWP_NOACTIVATE);
    };

    moveEdit(mediaUrlEdit_, mediaUrlEditRect_, !mediaAnalyzing_ && !mediaDownloading_);
    moveEdit(mediaFileNameEdit_, mediaFileNameEditRect_, !mediaDownloading_);
}

std::wstring ToolkitApp::AnimeTrackerFilePath() const
{
    wchar_t appDataPath[MAX_PATH] {};
    const DWORD length = GetEnvironmentVariableW(L"APPDATA", appDataPath, static_cast<DWORD>(std::size(appDataPath)));
    if (length == 0 || length >= std::size(appDataPath))
    {
        return L"anime_tracker.json";
    }

    return std::wstring(appDataPath) + L"\\RexsToolkit\\anime_tracker.json";
}

void ToolkitApp::LoadAnimeTrackerData()
{
    std::wstring warning;
    animeWatchList_ = animeTrackerService_.LoadWatchList(AnimeTrackerFilePath(), warning);
    animeStatusMessage_ = warning.empty() ? L"Ready." : warning;
    if (!animeWatchList_.anime.empty())
    {
        selectedAnimeIndex_ = 0;
    }
}

void ToolkitApp::SaveAnimeTrackerData()
{
    std::wstring errorMessage;
    if (!animeTrackerService_.SaveWatchList(AnimeTrackerFilePath(), animeWatchList_, errorMessage))
    {
        animeStatusMessage_ = errorMessage;
    }
}

void ToolkitApp::CreateAnimeTrackerControls()
{
    animeSearchEdit_ = CreateWindowExW(
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

    animeNotesEdit_ = CreateWindowExW(
        0,
        L"EDIT",
        L"",
        WS_CHILD | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
        0,
        0,
        0,
        0,
        hwnd_,
        nullptr,
        instance_,
        nullptr);

    if (animeSearchEdit_)
    {
        SendMessageW(animeSearchEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(searchInputFont_), TRUE);
        SendMessageW(animeSearchEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(Dips(8), Dips(6)));
    }
    if (animeNotesEdit_)
    {
        SendMessageW(animeNotesEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(monospaceFont_), TRUE);
        SendMessageW(animeNotesEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(Dips(4), Dips(4)));
    }

    if (animeSearchEdit_)
    {
        SendMessageW(animeSearchEdit_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search anime titles"));
    }
    if (animeNotesEdit_)
    {
        SendMessageW(animeNotesEdit_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Local notes for this anime"));
    }
}

void ToolkitApp::UpdateAnimeTrackerControls()
{
    const bool visible = currentPage_ == Page::Tool && currentTool_ == ToolKind::AnimeTracker;
    const bool hideForDropdown = activeDropdown_ == DropdownKind::AnimeFilter;

    auto moveEdit = [&](HWND edit, const RECT& rect, bool enabled, bool shouldShow = true)
    {
        if (!edit)
        {
            return;
        }

        const bool inViewport = visible && shouldShow && !hideForDropdown &&
            rect.top >= contentRect_.top &&
            rect.bottom <= contentRect_.bottom &&
            RectsOverlap(rect, contentRect_);
        ShowWindow(edit, inViewport ? SW_SHOW : SW_HIDE);
        EnableWindow(edit, inViewport && enabled);
        if (!inViewport)
        {
            return;
        }

        int editLeftInset = Dips(10);
        int editRightInset = Dips(10);
        if (edit == animeSearchEdit_)
        {
            editLeftInset = Dips(40);
            editRightInset = Dips(12);
        }
        const int editWidth = std::max(1, static_cast<int>(rect.right - rect.left) - editLeftInset - editRightInset);
        const int editHeight = edit == animeNotesEdit_
            ? std::max(1, static_cast<int>(rect.bottom - rect.top) - Dips(16))
            : edit == animeSearchEdit_
            ? std::max(1, Dips(24))
            : std::max(1, Dips(24));
        int editTop = static_cast<int>(rect.top) + ((rect.bottom - rect.top) - editHeight) / 2;
        if (edit == animeSearchEdit_)
        {
            editTop += Dips(2);
        }

        SetWindowPos(
            edit,
            nullptr,
            static_cast<int>(rect.left) + editLeftInset,
            editTop,
            editWidth,
            editHeight,
            SWP_NOZORDER | SWP_NOACTIVATE);
    };

    const bool hasSelection =
        selectedAnimeIndex_ >= 0 &&
        selectedAnimeIndex_ < static_cast<int>(animeWatchList_.anime.size());
    moveEdit(
        animeSearchEdit_,
        animeSearchEditRect_,
        !animeSearching_ && !animeRefreshing_,
        animeTrackerTab_ == AnimeTrackerTab::Search);
    moveEdit(
        animeNotesEdit_,
        animeNotesEditRect_,
        true,
        animeTrackerTab_ == AnimeTrackerTab::Anime && hasSelection);

    static int lastNotesIndex = -2;
    if (hasSelection)
    {
        if (lastNotesIndex != selectedAnimeIndex_ || GetFocus() != animeNotesEdit_)
        {
            suppressAnimeNotesChange_ = true;
            SetWindowTextIfChanged(animeNotesEdit_, animeWatchList_.anime[static_cast<size_t>(selectedAnimeIndex_)].notes);
            suppressAnimeNotesChange_ = false;
        }
        if (lastNotesIndex != selectedAnimeIndex_)
        {
            KillTimer(hwnd_, kAnimeNotesAutosaveTimerId);
            animeNotesStatusText_.clear();
        }
        lastNotesIndex = selectedAnimeIndex_;
    }
    else
    {
        suppressAnimeNotesChange_ = true;
        SetWindowTextIfChanged(animeNotesEdit_, L"");
        suppressAnimeNotesChange_ = false;
        KillTimer(hwnd_, kAnimeNotesAutosaveTimerId);
        animeNotesStatusText_.clear();
        lastNotesIndex = -1;
    }
}

std::wstring ToolkitApp::AnimeSearchText() const
{
    return GetWindowTextString(animeSearchEdit_);
}

void ToolkitApp::StartAnimeSearch(bool appendResults)
{
    if (animeSearching_ || animeRefreshing_)
    {
        return;
    }

    const std::wstring searchText = NormalizeAnimeSearchQuery(AnimeSearchText());
    if (searchText.empty())
    {
        animeStatusMessage_ = L"Enter a title to search AniList.";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    FinishAnimeThread();
    animeSearching_ = true;
    animeAppendSearch_ = appendResults;
    const int nextPage = appendResults ? animeCurrentPage_ + 1 : 1;
    animeStatusMessage_ = appendResults ? L"Loading more AniList results..." : L"Searching AniList...";
    RecalculateLayout();
    UpdateAnimeTrackerControls();
    InvalidateRect(hwnd_, nullptr, FALSE);

    HWND hwnd = hwnd_;
    animeThread_ = std::thread(
        [this, hwnd, searchText, nextPage, appendResults]()
        {
            auto* result = new AnimeSearchThreadResult();
            result->append = appendResults;
            std::wstring errorMessage;
            result->response = animeTrackerService_.SearchAnime(searchText, nextPage, 8, errorMessage);
            if (!errorMessage.empty())
            {
                result->message = errorMessage;
            }
            else if (result->response.results.empty())
            {
                result->message = L"No AniList matches found.";
            }
            else
            {
                result->message = L"Found " + std::to_wstring(result->response.results.size()) + L" result(s).";
            }

            for (const AnimeSearchResult& anime : result->response.results)
            {
                if (anime.coverImageUrl.empty())
                {
                    continue;
                }
                if (auto coverPath = DownloadAnimeCoverToCache(anime.coverImageUrl))
                {
                    result->coverFiles.emplace_back(anime.coverImageUrl, coverPath->wstring());
                }
            }

            PostMessageW(hwnd, kAnimeSearchFinishedMessage, 0, reinterpret_cast<LPARAM>(result));
        });
}

void ToolkitApp::FinishAnimeThread()
{
    if (animeThread_.joinable() && animeThread_.get_id() != std::this_thread::get_id())
    {
        animeThread_.join();
    }
}

void ToolkitApp::ApplyAnimeSearchResponse(const AnimeSearchResponse& response, const std::wstring& message, bool appendResults)
{
    animeSearchResponse_ = response;
    animeSearchHasRun_ = true;
    animeCanLoadMore_ = response.hasNextPage;
    animeCurrentPage_ = std::max(1, response.currentPage);
    if (appendResults)
    {
        animeSearchResults_.insert(animeSearchResults_.end(), response.results.begin(), response.results.end());
    }
    else
    {
        animeSearchResults_ = response.results;
    }
    animeStatusMessage_ = message.empty() ? L"Ready." : message;
}

void ToolkitApp::AddAnimeFromSearch(size_t index)
{
    if (index >= animeSearchResults_.size())
    {
        return;
    }

    const AnimeSearchResult& result = animeSearchResults_[index];
    if (AnimeTrackerService::ContainsAnime(animeWatchList_, result.anilistId))
    {
        animeStatusMessage_ = L"That anime is already in your watchlist.";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    animeWatchList_.anime.push_back(AnimeTrackerService::EntryFromSearchResult(result));
    selectedAnimeIndex_ = static_cast<int>(animeWatchList_.anime.size() - 1);
    SaveAnimeTrackerData();
    UpdateAnimeTrackerControls();
    animeStatusMessage_ = L"Added " + result.title + L" to the Anime tab.";
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ToolkitApp::AddAnimeFromRelation(size_t index)
{
    const std::vector<AnimeRelation> sequels = VisibleUpcomingSequels();
    if (index >= sequels.size())
    {
        return;
    }

    const AnimeRelation& relation = sequels[index];
    if (AnimeTrackerService::ContainsAnime(animeWatchList_, relation.anilistId))
    {
        animeStatusMessage_ = L"That sequel is already saved.";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    AnimeSearchResult result;
    result.anilistId = relation.anilistId;
    result.title = relation.title;
    result.coverImageUrl = relation.coverImageUrl;
    result.format = relation.format;
    result.status = relation.status;
    result.episodes = relation.episodes;
    result.season = relation.season;
    result.seasonYear = relation.seasonYear;
    result.startDate = relation.startDate;
    result.siteUrl = relation.siteUrl;
    result.nextAiringEpisode = relation.nextAiringEpisode;
    animeWatchList_.anime.push_back(AnimeTrackerService::EntryFromSearchResult(result));
    selectedAnimeIndex_ = static_cast<int>(animeWatchList_.anime.size() - 1);
    SaveAnimeTrackerData();
    UpdateAnimeTrackerControls();
    animeStatusMessage_ = L"Added " + relation.title + L".";
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ToolkitApp::RefreshAnimeEntry(size_t index)
{
    if (index >= animeWatchList_.anime.size() || animeSearching_ || animeRefreshing_)
    {
        return;
    }

    FinishAnimeThread();
    animeRefreshing_ = true;
    animeStatusMessage_ = L"Refreshing from AniList...";
    UpdateAnimeTrackerControls();
    InvalidateRect(hwnd_, nullptr, FALSE);

    const int anilistId = animeWatchList_.anime[index].anilistId;
    HWND hwnd = hwnd_;
    animeThread_ = std::thread(
        [this, hwnd, anilistId, index]()
        {
            auto* result = new AnimeRefreshThreadResult();
            result->listIndex = static_cast<int>(index);
            std::wstring errorMessage;
            result->result = animeTrackerService_.RefreshAnime(anilistId, errorMessage);
            if (result->result)
            {
                result->message = L"Refreshed metadata.";
            }
            else
            {
                result->message = errorMessage.empty() ? L"Could not refresh that anime." : errorMessage;
            }

            PostMessageW(hwnd, kAnimeRefreshFinishedMessage, 0, reinterpret_cast<LPARAM>(result));
        });
}

void ToolkitApp::RefreshAllAnime()
{
    if (animeWatchList_.anime.empty() || animeSearching_ || animeRefreshing_)
    {
        return;
    }

    FinishAnimeThread();
    animeRefreshing_ = true;
    animeStatusMessage_ = L"Refreshing saved anime from AniList...";
    UpdateAnimeTrackerControls();
    InvalidateRect(hwnd_, nullptr, FALSE);

    std::vector<std::pair<int, int>> ids;
    ids.reserve(animeWatchList_.anime.size());
    for (size_t index = 0; index < animeWatchList_.anime.size(); ++index)
    {
        ids.emplace_back(static_cast<int>(index), animeWatchList_.anime[index].anilistId);
    }

    HWND hwnd = hwnd_;
    animeThread_ = std::thread(
        [this, hwnd, ids]()
        {
            auto* result = new AnimeRefreshAllThreadResult();
            int failed = 0;
            for (const auto& item : ids)
            {
                std::wstring errorMessage;
                auto refreshed = animeTrackerService_.RefreshAnime(item.second, errorMessage);
                if (refreshed)
                {
                    result->results.emplace_back(item.first, *refreshed);
                }
                else
                {
                    ++failed;
                }
            }

            result->message = L"Refreshed " + std::to_wstring(result->results.size()) + L" item(s).";
            if (failed > 0)
            {
                result->message += L" " + std::to_wstring(failed) + L" failed.";
            }

            PostMessageW(hwnd, kAnimeRefreshAllFinishedMessage, 0, reinterpret_cast<LPARAM>(result));
        });
}

void ToolkitApp::ApplyAnimeRefreshResult(const AnimeSearchResult& result, int listIndex, const std::wstring& message)
{
    if (listIndex < 0 || listIndex >= static_cast<int>(animeWatchList_.anime.size()))
    {
        return;
    }

    AnimeTrackerService::ApplyMetadata(animeWatchList_.anime[static_cast<size_t>(listIndex)], result);
    if (!message.empty())
    {
        animeStatusMessage_ = message;
    }
    SaveAnimeTrackerData();
}

void ToolkitApp::SelectAnimeEntry(int index)
{
    if (index < 0 || index >= static_cast<int>(animeWatchList_.anime.size()))
    {
        selectedAnimeIndex_ = -1;
    }
    else
    {
        selectedAnimeIndex_ = index;
    }
    UpdateAnimeTrackerControls();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ToolkitApp::RemoveAnimeEntry(size_t index)
{
    if (index >= animeWatchList_.anime.size())
    {
        return;
    }

    const std::wstring title = animeWatchList_.anime[index].title;
    animeWatchList_.anime.erase(animeWatchList_.anime.begin() + static_cast<std::ptrdiff_t>(index));
    if (selectedAnimeIndex_ == static_cast<int>(index))
    {
        selectedAnimeIndex_ = -1;
    }
    else if (selectedAnimeIndex_ > static_cast<int>(index))
    {
        --selectedAnimeIndex_;
    }
    SaveAnimeTrackerData();
    UpdateAnimeTrackerControls();
    animeStatusMessage_ = L"Removed " + title + L".";
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ToolkitApp::IncrementAnimeEpisode(size_t index)
{
    if (index >= animeWatchList_.anime.size())
    {
        return;
    }

    AnimeEntry& entry = animeWatchList_.anime[index];
    if (entry.totalEpisodes == 0 || entry.currentEpisode < entry.totalEpisodes)
    {
        ++entry.currentEpisode;
        if (entry.totalEpisodes > 0 && entry.currentEpisode >= entry.totalEpisodes)
        {
            entry.userStatus = AnimeUserStatus::Completed;
        }
        SaveAnimeTrackerData();
        animeStatusMessage_ = L"Progress updated.";
        UpdateAnimeTrackerControls();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void ToolkitApp::DecrementSelectedAnimeEpisode()
{
    if (selectedAnimeIndex_ < 0 || selectedAnimeIndex_ >= static_cast<int>(animeWatchList_.anime.size()))
    {
        return;
    }

    AnimeEntry& entry = animeWatchList_.anime[static_cast<size_t>(selectedAnimeIndex_)];
    if (entry.currentEpisode > 0)
    {
        --entry.currentEpisode;
        SaveAnimeTrackerData();
        animeStatusMessage_ = L"Progress updated.";
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void ToolkitApp::CycleSelectedAnimeStatus()
{
    if (selectedAnimeIndex_ < 0 || selectedAnimeIndex_ >= static_cast<int>(animeWatchList_.anime.size()))
    {
        return;
    }

    AnimeEntry& entry = animeWatchList_.anime[static_cast<size_t>(selectedAnimeIndex_)];
    entry.userStatus = AnimeTrackerService::NextUserStatus(entry.userStatus);
    SaveAnimeTrackerData();
    animeStatusMessage_ = L"Status changed to " + AnimeTrackerService::UserStatusLabel(entry.userStatus) + L".";
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ToolkitApp::ToggleSelectedAnimeFavorite()
{
    if (selectedAnimeIndex_ < 0 || selectedAnimeIndex_ >= static_cast<int>(animeWatchList_.anime.size()))
    {
        return;
    }

    AnimeEntry& entry = animeWatchList_.anime[static_cast<size_t>(selectedAnimeIndex_)];
    entry.favorite = !entry.favorite;
    SaveAnimeTrackerData();
    animeStatusMessage_ = entry.favorite ? L"Marked as a favorite." : L"Removed from Anime Tracker favorites.";
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ToolkitApp::SaveSelectedAnimeNotes()
{
    if (selectedAnimeIndex_ < 0 || selectedAnimeIndex_ >= static_cast<int>(animeWatchList_.anime.size()))
    {
        return;
    }

    animeWatchList_.anime[static_cast<size_t>(selectedAnimeIndex_)].notes = GetWindowTextString(animeNotesEdit_);
    SaveAnimeTrackerData();
    animeNotesStatusText_ = L"Saved automatically.";
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ToolkitApp::ShowAnimeFilterDropdown()
{
    OpenDropdown(
        DropdownKind::AnimeFilter,
        animeFilterButtonRect_,
        { L"All statuses", L"Watching", L"Planned", L"Completed", L"On Hold", L"Dropped" },
        {
            0,
            1 + static_cast<int>(AnimeUserStatus::Watching),
            1 + static_cast<int>(AnimeUserStatus::Planned),
            1 + static_cast<int>(AnimeUserStatus::Completed),
            1 + static_cast<int>(AnimeUserStatus::OnHold),
            1 + static_cast<int>(AnimeUserStatus::Dropped)
        },
        { true, true, true, true, true, true },
        animeFilterAll_ ? 0 : 1 + static_cast<int>(animeFilter_));
}

std::vector<size_t> ToolkitApp::VisibleAnimeEntryIndexes() const
{
    std::vector<size_t> indexes;
    for (size_t index = 0; index < animeWatchList_.anime.size(); ++index)
    {
        if (animeFilterAll_ || animeWatchList_.anime[index].userStatus == animeFilter_)
        {
            indexes.push_back(index);
        }
    }
    return indexes;
}

std::vector<AnimeRelation> ToolkitApp::VisibleUpcomingSequels() const
{
    return AnimeRelationTracker::UpcomingSequels(animeWatchList_);
}

std::wstring ToolkitApp::AnimeStatusText(const AnimeEntry& entry) const
{
    return AnimeTrackerService::UserStatusLabel(entry.userStatus);
}

std::wstring ToolkitApp::AnimeProgressText(const AnimeEntry& entry) const
{
    std::wstring progress = std::to_wstring(entry.currentEpisode);
    progress += L" / ";
    progress += entry.totalEpisodes > 0 ? std::to_wstring(entry.totalEpisodes) : L"?";
    progress += L" episodes";
    return progress;
}

RECT ToolkitApp::AnimeSearchResultCardRect(size_t index) const
{
    const int gap = Dips(kAnimeSearchResultGridGapDip);
    const int gridLeft = animeResultsRect_.left + Dips(22);
    const int gridRight = animeResultsRect_.right - Dips(22);
    const int gridTop = animeSearchEditRect_.bottom + Dips(84);
    const int gridWidth = std::max(1, gridRight - gridLeft);
    const int preferredCardWidth = Dips(kAnimeSearchResultCardWidthDip);
    const int columns = std::max(1, std::min(kAnimeSearchResultMaxColumns, (gridWidth + gap) / (preferredCardWidth + gap)));
    const int cardWidth = columns == 1 ? std::min(preferredCardWidth, gridWidth) : preferredCardWidth;
    const int row = static_cast<int>(index) / columns;
    const int column = static_cast<int>(index) % columns;
    const int left = gridLeft + column * (cardWidth + gap);
    const int top = gridTop + row * (Dips(kAnimeSearchResultCardHeightDip) + gap);
    return {
        left,
        top,
        left + cardWidth,
        top + Dips(kAnimeSearchResultCardHeightDip)
    };
}

RECT ToolkitApp::AnimeSearchResultAddRect(size_t index) const
{
    const RECT card = AnimeSearchResultCardRect(index);
    return {
        card.left + Dips(14),
        card.bottom - Dips(48),
        card.right - Dips(14),
        card.bottom - Dips(12)
    };
}

RECT ToolkitApp::AnimeListRowRect(size_t visibleIndex) const
{
    const int rowHeight = Dips(68);
    const int rowTop = animeListRect_.top + Dips(128) + static_cast<int>(visibleIndex) * rowHeight;
    return {
        animeListRect_.left + Dips(14),
        rowTop,
        animeListRect_.right - Dips(14),
        rowTop + rowHeight - Dips(6)
    };
}

RECT ToolkitApp::AnimeListActionRect(size_t visibleIndex, int actionIndex) const
{
    const RECT row = AnimeListRowRect(visibleIndex);
    const int gap = Dips(6);
    const int widths[] = { Dips(74), Dips(78), Dips(74) };
    int right = row.right - Dips(10);
    for (int index = 2; index > actionIndex; --index)
    {
        right -= widths[index] + gap;
    }
    return {
        right - widths[actionIndex],
        row.top + Dips(14),
        right,
        row.bottom - Dips(14)
    };
}

RECT ToolkitApp::AnimeSequelActionRect(size_t index, int actionIndex) const
{
    const int rowHeight = Dips(46);
    const int rowTop = animeSequelsRect_.top + Dips(48) + static_cast<int>(index) * rowHeight;
    const RECT row {
        animeSequelsRect_.left + Dips(14),
        rowTop,
        animeSequelsRect_.right - Dips(14),
        rowTop + rowHeight - Dips(6)
    };

    const int width = actionIndex == 0 ? Dips(54) : Dips(62);
    const int right = actionIndex == 0 ? row.right - Dips(76) : row.right - Dips(8);
    return {
        right - width,
        row.top + Dips(7),
        right,
        row.bottom - Dips(7)
    };
}

std::optional<std::filesystem::path> ToolkitApp::PromptForSingleConverterOutputPath(const ConversionJob& job) const
{
    const std::wstring extension = SupportedFormatRegistry::ExtensionFor(conversionOptions_.outputFormat);
    const std::wstring defaultName = job.inputPath.stem().wstring() + extension;
    const std::filesystem::path defaultPath = !appSettings_.defaultOutputFolder.empty() && std::filesystem::exists(appSettings_.defaultOutputFolder)
        ? appSettings_.defaultOutputFolder / defaultName
        : job.inputPath.has_parent_path()
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
    const std::wstring initialFolder = appSettings_.defaultOutputFolder.wstring();
    if (!initialFolder.empty())
    {
        browseInfo.lpfn = BrowseFolderCallback;
        browseInfo.lParam = reinterpret_cast<LPARAM>(initialFolder.c_str());
    }

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
    const std::wstring initialFolder = appSettings_.defaultOutputFolder.wstring();
    if (!initialFolder.empty())
    {
        browseInfo.lpfn = BrowseFolderCallback;
        browseInfo.lParam = reinterpret_cast<LPARAM>(initialFolder.c_str());
    }

    PIDLIST_ABSOLUTE itemList = SHBrowseForFolderW(&browseInfo);
    if (!itemList)
    {
        return;
    }

    wchar_t path[MAX_PATH] {};
    if (SHGetPathFromIDListW(itemList, path))
    {
        mediaDownloadOptions_.outputFolder = path;
        appSettings_.defaultOutputFolder = path;
        mediaDownloadJob_.outputFolder = mediaDownloadOptions_.outputFolder;
        mediaStatusText_ = L"Save location selected.";
        SaveAppSettings();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    CoTaskMemFree(itemList);
}

void ToolkitApp::BrowseDefaultOutputFolder()
{
    BROWSEINFOW browseInfo {};
    browseInfo.hwndOwner = hwnd_;
    browseInfo.lpszTitle = L"Choose the default output folder";
    browseInfo.ulFlags = BIF_RETURNONLYFSDIRS | BIF_USENEWUI;
    const std::wstring initialFolder = appSettings_.defaultOutputFolder.wstring();
    if (!initialFolder.empty())
    {
        browseInfo.lpfn = BrowseFolderCallback;
        browseInfo.lParam = reinterpret_cast<LPARAM>(initialFolder.c_str());
    }

    PIDLIST_ABSOLUTE itemList = SHBrowseForFolderW(&browseInfo);
    if (!itemList)
    {
        return;
    }

    wchar_t path[MAX_PATH] {};
    if (SHGetPathFromIDListW(itemList, path))
    {
        appSettings_.defaultOutputFolder = path;
        mediaDownloadOptions_.outputFolder = appSettings_.defaultOutputFolder;
        mediaDownloadJob_.outputFolder = mediaDownloadOptions_.outputFolder;
        SaveAppSettings();
        SaveMediaDownloadSettings();
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
        if (mediaDownloadOptions_.outputFormat == MediaOutputFormat::Mp4 &&
            job.maxVideoHeight > 0 &&
            Mp4QualityHeight(mediaDownloadOptions_.mp4Quality) > job.maxVideoHeight)
        {
            mediaDownloadOptions_.mp4Quality = BestAvailableQualityCap(job.maxVideoHeight);
        }
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
    if (updateChecking_ || updateInstalling_)
    {
        return;
    }

    FinishUpdateThread();
    updateChecking_ = true;
    hasUpdateResult_ = false;
    updateResult_ = {};
    updateResult_.currentVersion = APP_VERSION;
    updateInstallStatus_.clear();
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
    updateInstallStatus_.clear();
    hasUpdateResult_ = true;
}

void ToolkitApp::StartUpdateInstall()
{
    if (updateInstalling_ ||
        !hasUpdateResult_ ||
        updateResult_.status != UpdateCheckStatus::UpdateAvailable)
    {
        return;
    }

    const std::wstring downloadUrl = ResolveUpdatePackageUrl();
    if (!UpdateChecker::IsSafeHttpUrl(downloadUrl))
    {
        updateInstallStatus_ = L"Could not download the update. The download URL is not valid.";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    FinishUpdateThread();
    updateInstalling_ = true;
    updateInstallStatus_ = L"Downloading update...";
    InvalidateRect(hwnd_, nullptr, FALSE);

    HWND hwnd = hwnd_;
    updateThread_ = std::thread(
        [this, hwnd, downloadUrl]()
        {
            auto* result = new UpdateInstallResult();
            std::filesystem::path packagePath;
            std::wstring errorMessage;

            if (!DownloadUpdatePackage(downloadUrl, packagePath, errorMessage))
            {
                result->success = false;
                result->message = errorMessage.empty()
                    ? L"Could not download the update."
                    : errorMessage;
            }
            else if (!CreateAndLaunchUpdateInstaller(packagePath, errorMessage))
            {
                result->success = false;
                result->message = errorMessage.empty()
                    ? L"Could not start the update installer."
                    : errorMessage;
            }
            else
            {
                result->success = true;
                result->message = L"Installing update. Rex's Toolkit will restart.";
            }

            PostMessageW(hwnd, kUpdateInstallFinishedMessage, 0, reinterpret_cast<LPARAM>(result));
        });
}

std::wstring ToolkitApp::ResolveUpdatePackageUrl() const
{
    std::wstring downloadUrl = updateResult_.downloadUrl;
    std::wstring lowerUrl = downloadUrl;
    std::transform(lowerUrl.begin(), lowerUrl.end(), lowerUrl.begin(), [](wchar_t ch)
    {
        return static_cast<wchar_t>(towlower(ch));
    });

    if (lowerUrl.find(L".zip") != std::wstring::npos)
    {
        return downloadUrl;
    }

    if (!updateResult_.latestVersion.empty())
    {
        return L"https://github.com/Rexarater/rex-toolkit/releases/download/v" +
            updateResult_.latestVersion +
            L"/RexsToolkit_v" +
            updateResult_.latestVersion +
            L".zip";
    }

    return downloadUrl;
}

bool ToolkitApp::DownloadUpdatePackage(
    const std::wstring& downloadUrl,
    std::filesystem::path& packagePath,
    std::wstring& errorMessage) const
{
    wchar_t tempPathBuffer[MAX_PATH] {};
    const DWORD tempPathLength = GetTempPathW(static_cast<DWORD>(std::size(tempPathBuffer)), tempPathBuffer);
    if (tempPathLength == 0 || tempPathLength >= std::size(tempPathBuffer))
    {
        errorMessage = L"Could not download the update. Windows did not provide a temporary folder.";
        return false;
    }

    const std::filesystem::path tempRoot =
        std::filesystem::path(tempPathBuffer) / (L"RexToolkitUpdate_" + std::to_wstring(GetCurrentProcessId()));

    std::error_code fileError;
    std::filesystem::remove_all(tempRoot, fileError);
    fileError.clear();
    std::filesystem::create_directories(tempRoot, fileError);
    if (fileError)
    {
        errorMessage = L"Could not create the update staging folder.";
        return false;
    }

    packagePath = tempRoot / L"RexsToolkitUpdate.zip";

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool shouldUninitializeCom = SUCCEEDED(comResult);
    const HRESULT downloadResult = URLDownloadToFileW(nullptr, downloadUrl.c_str(), packagePath.wstring().c_str(), 0, nullptr);
    if (shouldUninitializeCom)
    {
        CoUninitialize();
    }

    if (FAILED(downloadResult))
    {
        errorMessage = L"Could not download the update. " + HResultMessage(downloadResult);
        return false;
    }

    fileError.clear();
    const auto packageSize = std::filesystem::file_size(packagePath, fileError);
    if (fileError || packageSize == 0)
    {
        errorMessage = L"Could not download the update. The update package was empty.";
        return false;
    }

    return true;
}

bool ToolkitApp::CreateAndLaunchUpdateInstaller(
    const std::filesystem::path& packagePath,
    std::wstring& errorMessage) const
{
    wchar_t executablePathBuffer[MAX_PATH] {};
    const DWORD executablePathLength = GetModuleFileNameW(nullptr, executablePathBuffer, static_cast<DWORD>(std::size(executablePathBuffer)));
    if (executablePathLength == 0 || executablePathLength >= std::size(executablePathBuffer))
    {
        errorMessage = L"Could not install the update. The app path could not be resolved.";
        return false;
    }

    const std::filesystem::path executablePath(executablePathBuffer);
    const std::filesystem::path appDirectory = executablePath.parent_path();
    const std::filesystem::path scriptPath = packagePath.parent_path() / L"install_rex_toolkit_update.ps1";
    const std::filesystem::path logPath = packagePath.parent_path() / L"install_rex_toolkit_update.log";

    std::wofstream script(scriptPath, std::ios::trunc);
    if (!script)
    {
        errorMessage = L"Could not install the update. The installer script could not be created.";
        return false;
    }

    script
        << L"$ErrorActionPreference = 'Stop'\n"
        << L"$processIdToWait = " << GetCurrentProcessId() << L"\n"
        << L"$zipPath = " << PowerShellQuote(packagePath.wstring()) << L"\n"
        << L"$appDir = " << PowerShellQuote(appDirectory.wstring()) << L"\n"
        << L"$exePath = " << PowerShellQuote(executablePath.wstring()) << L"\n"
        << L"$logPath = " << PowerShellQuote(logPath.wstring()) << L"\n"
        << L"$extractDir = Join-Path ([IO.Path]::GetTempPath()) ('RexToolkitUpdateExtract_' + [guid]::NewGuid().ToString('N'))\n"
        << L"try {\n"
        << L"    Wait-Process -Id $processIdToWait -ErrorAction SilentlyContinue\n"
        << L"    if (Test-Path -LiteralPath $extractDir) { Remove-Item -LiteralPath $extractDir -Recurse -Force }\n"
        << L"    New-Item -ItemType Directory -Force -Path $extractDir | Out-Null\n"
        << L"    Expand-Archive -LiteralPath $zipPath -DestinationPath $extractDir -Force\n"
        << L"    $exe = Get-ChildItem -LiteralPath $extractDir -Recurse -Filter 'RexToolkit.exe' -File | Select-Object -First 1\n"
        << L"    if (-not $exe) { throw 'The update package did not contain RexToolkit.exe.' }\n"
        << L"    $sourceDir = Split-Path -Parent $exe.FullName\n"
        << L"    Get-ChildItem -LiteralPath $sourceDir -Force | Copy-Item -Destination $appDir -Recurse -Force\n"
        << L"    Start-Process -FilePath $exePath\n"
        << L"    Remove-Item -LiteralPath $extractDir -Recurse -Force -ErrorAction SilentlyContinue\n"
        << L"    Remove-Item -LiteralPath $zipPath -Force -ErrorAction SilentlyContinue\n"
        << L"} catch {\n"
        << L"    ('Update failed: ' + $_.Exception.Message) | Out-File -FilePath $logPath -Encoding UTF8 -Append\n"
        << L"    Start-Process -FilePath $exePath\n"
        << L"}\n";
    script.close();

    const std::wstring parameters =
        L"-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File " +
        CommandLineQuote(scriptPath.wstring());
    HINSTANCE launchResult = ShellExecuteW(
        hwnd_,
        L"open",
        L"powershell.exe",
        parameters.c_str(),
        nullptr,
        SW_HIDE);

    if (reinterpret_cast<INT_PTR>(launchResult) <= 32)
    {
        errorMessage = L"Could not install the update. PowerShell could not be started.";
        return false;
    }

    return true;
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
        const std::array<Mp4Quality, 7> allQualities {
            Mp4Quality::Best,
            Mp4Quality::P4320,
            Mp4Quality::P2160,
            Mp4Quality::P1440,
            Mp4Quality::P1080,
            Mp4Quality::P720,
            Mp4Quality::P480
        };

        std::vector<Mp4Quality> qualities;
        qualities.reserve(allQualities.size());
        for (Mp4Quality quality : allQualities)
        {
            const int qualityHeight = Mp4QualityHeight(quality);
            if (quality == Mp4Quality::Best ||
                mediaDownloadJob_.maxVideoHeight <= 0 ||
                qualityHeight <= mediaDownloadJob_.maxVideoHeight)
            {
                qualities.push_back(quality);
            }
        }

        std::vector<std::wstring> labels;
        std::vector<int> values;
        std::vector<bool> enabled;
        for (Mp4Quality quality : qualities)
        {
            labels.push_back(MediaDownloadService::Mp4QualityLabel(quality));
            values.push_back(static_cast<int>(quality));
            enabled.push_back(true);
        }

        OpenDropdown(
            DropdownKind::MediaQuality,
            mediaQualityButtonRect_,
            labels,
            values,
            enabled,
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

void ToolkitApp::ShowSettingsStartPageDropdown()
{
    OpenDropdown(
        DropdownKind::SettingsStartPage,
        settingsStartPageButtonRect_,
        { L"Favorites", L"All Tools" },
        { static_cast<int>(DefaultStartPage::Favorites), static_cast<int>(DefaultStartPage::AllTools) },
        { true, true },
        static_cast<int>(appSettings_.startPage));
}

void ToolkitApp::ShowSettingsClockFormatDropdown()
{
    OpenDropdown(
        DropdownKind::SettingsClockFormat,
        settingsClockFormatButtonRect_,
        { L"MM/DD/YYYY 24-hour", L"MM/DD/YYYY 12-hour", L"YYYY-MM-DD 24-hour", L"Weekday + 12-hour" },
        {
            static_cast<int>(ClockFormat::MonthDay24),
            static_cast<int>(ClockFormat::MonthDay12),
            static_cast<int>(ClockFormat::Iso24),
            static_cast<int>(ClockFormat::Friendly12)
        },
        { true, true, true, true },
        static_cast<int>(appSettings_.clockFormat));
}

void ToolkitApp::ShowSettingsThemeDropdown()
{
    OpenDropdown(
        DropdownKind::SettingsTheme,
        settingsThemeButtonRect_,
        { L"Dark", L"Light" },
        { static_cast<int>(AppTheme::Dark), static_cast<int>(AppTheme::Light) },
        { true, true },
        static_cast<int>(appSettings_.theme));
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

    RECT repaint = speedSliderTrackRect_;
    UnionRect(&repaint, &repaint, &speedSliderThumbRect_);

    autoClicker_.clicksPerSecond = std::clamp(newSpeed, kMinClicksPerSecond, kMaxClicksPerSecond);
    const int sliderOffset = MulDiv(
        autoClicker_.clicksPerSecond - kMinClicksPerSecond,
        trackWidth,
        kMaxClicksPerSecond - kMinClicksPerSecond);
    const int thumbCenterX = trackLeft + sliderOffset;
    speedSliderThumbRect_ = {
        thumbCenterX - Dips(10),
        speedSliderTrackRect_.top - Dips(8),
        thumbCenterX + Dips(10),
        speedSliderTrackRect_.bottom + Dips(8)
    };
    UnionRect(&repaint, &repaint, &speedSliderThumbRect_);
    UpdateAutoClickerTimer();
    repaint.left -= Dips(26);
    repaint.right += Dips(26);
    repaint.top -= Dips(44);
    repaint.bottom += Dips(26);
    InvalidateRect(hwnd_, &repaint, FALSE);
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
    if (appSettings_.clockFormat == ClockFormat::MonthDay12 ||
        appSettings_.clockFormat == ClockFormat::Friendly12)
    {
        const wchar_t* suffix = localTime.wHour >= 12 ? L"PM" : L"AM";
        WORD hour = localTime.wHour % 12;
        if (hour == 0)
        {
            hour = 12;
        }

        if (appSettings_.clockFormat == ClockFormat::Friendly12)
        {
            static constexpr wchar_t weekdays[][10] {
                L"Sunday", L"Monday", L"Tuesday", L"Wednesday", L"Thursday", L"Friday", L"Saturday"
            };
            swprintf_s(
                buffer,
                L"%s %u:%02u:%02u %s",
                weekdays[localTime.wDayOfWeek],
                hour,
                localTime.wMinute,
                localTime.wSecond,
                suffix);
        }
        else
        {
            swprintf_s(
                buffer,
                L"%02u/%02u/%04u %u:%02u:%02u %s",
                localTime.wMonth,
                localTime.wDay,
                localTime.wYear,
                hour,
                localTime.wMinute,
                localTime.wSecond,
                suffix);
        }
    }
    else if (appSettings_.clockFormat == ClockFormat::Iso24)
    {
        swprintf_s(
            buffer,
            L"%04u-%02u-%02u %02u:%02u:%02u",
            localTime.wYear,
            localTime.wMonth,
            localTime.wDay,
            localTime.wHour,
            localTime.wMinute,
            localTime.wSecond);
    }
    else
    {
        swprintf_s(
            buffer,
            L"%02u/%02u/%04u %02u:%02u:%02u",
            localTime.wMonth,
            localTime.wDay,
            localTime.wYear,
            localTime.wHour,
            localTime.wMinute,
            localTime.wSecond);
    }
    return buffer;
}

std::wstring ToolkitApp::StartPageLabel() const
{
    return appSettings_.startPage == DefaultStartPage::AllTools ? L"All Tools" : L"Favorites";
}

std::wstring ToolkitApp::ClockFormatLabel() const
{
    switch (appSettings_.clockFormat)
    {
    case ClockFormat::MonthDay12:
        return L"MM/DD/YYYY 12-hour";
    case ClockFormat::Iso24:
        return L"YYYY-MM-DD 24-hour";
    case ClockFormat::Friendly12:
        return L"Weekday + 12-hour";
    case ClockFormat::MonthDay24:
        return L"MM/DD/YYYY 24-hour";
    }
    return L"MM/DD/YYYY 24-hour";
}

std::wstring ToolkitApp::ThemeLabel() const
{
    return appSettings_.theme == AppTheme::Light ? L"Light" : L"Dark";
}

void ToolkitApp::ApplyTheme()
{
    ApplyPalette(appSettings_.theme);
    if (editBackgroundBrush_)
    {
        DeleteObject(editBackgroundBrush_);
    }
    editBackgroundBrush_ = CreateSolidBrush(kInputBackground);
    if (hwnd_)
    {
        ApplyDarkTitleBar();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

int ToolkitApp::Dips(int value) const
{
    return MulDiv(value, dpi_, 96);
}
