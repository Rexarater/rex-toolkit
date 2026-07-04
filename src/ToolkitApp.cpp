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
#include <winhttp.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <ctime>
#include <cwchar>
#include <cwctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <numeric>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

namespace
{
constexpr wchar_t kWindowClassName[] = L"RexToolkitWindowClass";
constexpr wchar_t kMacroOverlayClassName[] = L"RexToolkitMacroOverlayClass";
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
constexpr UINT kAnimeImportFinishedMessage = WM_APP + 110;
constexpr UINT kTrayIconMessage = WM_APP + 130;
constexpr UINT kExternalLaunchRestoreMessage = WM_APP + 131;
constexpr UINT kSmartTransferConnectFinishedMessage = WM_APP + 132;
constexpr UINT kSmartTransferDownloadProgressMessage = WM_APP + 133;
constexpr UINT kSmartTransferDownloadFinishedMessage = WM_APP + 134;
constexpr UINT kSmartTransferPairingFinishedMessage = WM_APP + 135;
constexpr UINT kSmartTransferResponseFinishedMessage = WM_APP + 136;
constexpr UINT kSmartTransferApplyResponseFinishedMessage = WM_APP + 137;
constexpr UINT kVideoCompressionAnalysisFinishedMessage = WM_APP + 138;
constexpr UINT kVideoCompressionProgressMessage = WM_APP + 139;
constexpr UINT kVideoCompressionFinishedMessage = WM_APP + 140;
constexpr UINT kTrayIconId = 1;
constexpr UINT kTrayMenuOpenCommand = 45001;
constexpr UINT kTrayMenuExitCommand = 45002;
constexpr wchar_t kSingleInstanceMutexName[] = L"Local\\RexToolkitSingleInstance";
constexpr wchar_t kStartupRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kStartupRunValueName[] = L"RexToolkit";
constexpr int kMinClicksPerSecond = 1;
constexpr int kMaxClicksPerSecond = 100;
constexpr UINT_PTR kClockTimerId = 1002;
constexpr UINT_PTR kAnimeNotesAutosaveTimerId = 1003;
constexpr UINT kAnimeNotesAutosaveDelayMs = 850;
constexpr UINT_PTR kResizeTimerId = 1004;
constexpr UINT kResizeRedrawMs = 66;
constexpr UINT_PTR kReminderTimerId = 1005;
constexpr UINT kReminderCheckMs = 30000;
constexpr UINT_PTR kSmartTransferTimerId = 1006;
constexpr UINT kSmartTransferRefreshMs = 500;
constexpr UINT_PTR kMacroRecorderTimerId = 1007;
constexpr UINT kMacroRecorderRefreshMs = 250;
constexpr int kMacroRecordHotkeyId = 2101;
constexpr int kMacroPlayHotkeyId = 2102;
constexpr int kMacroStopHotkeyId = 2103;
constexpr int kBackBufferGrowthStep = 256;
constexpr ULONG_PTR kAutoClickExtraInfo = 0x5254584B;
constexpr int kAnimeSearchResultCardWidthDip = 216;
constexpr int kAnimeSearchResultCardHeightDip = 452;
constexpr int kAnimeSearchResultGridGapDip = 22;
constexpr int kAnimeSearchResultMaxColumns = 10;
constexpr int kToolHeaderTopDip = 16;
constexpr int kToolBodyTopDip = 104;
constexpr unsigned long long kMaximumVideoCompressionTargetBytes = 100000ULL * 1024ULL * 1024ULL * 1024ULL;

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

    if (theme == AppTheme::Midnight)
    {
        return {
            RGB(9, 15, 30),
            RGB(13, 22, 42),
            RGB(19, 31, 56),
            RGB(27, 43, 77),
            RGB(90, 166, 255),
            RGB(29, 75, 132),
            RGB(245, 190, 84),
            RGB(237, 245, 255),
            RGB(158, 178, 208),
            RGB(45, 65, 102),
            RGB(12, 22, 42),
            RGB(25, 39, 68),
            RGB(36, 58, 98),
            RGB(31, 50, 86),
            RGB(22, 32, 54),
            RGB(97, 112, 139),
            RGB(16, 28, 52),
            RGB(34, 54, 91),
            RGB(28, 58, 102)
        };
    }

    if (theme == AppTheme::Forest)
    {
        return {
            RGB(13, 22, 18),
            RGB(18, 32, 26),
            RGB(26, 43, 35),
            RGB(34, 55, 45),
            RGB(75, 184, 127),
            RGB(38, 103, 72),
            RGB(224, 176, 82),
            RGB(238, 247, 241),
            RGB(166, 190, 176),
            RGB(52, 76, 63),
            RGB(18, 31, 26),
            RGB(33, 50, 42),
            RGB(43, 69, 55),
            RGB(42, 62, 52),
            RGB(30, 42, 36),
            RGB(111, 133, 119),
            RGB(22, 37, 31),
            RGB(39, 63, 51),
            RGB(34, 75, 54)
        };
    }

    if (theme == AppTheme::Rose)
    {
        return {
            RGB(30, 18, 28),
            RGB(41, 24, 38),
            RGB(53, 32, 49),
            RGB(69, 42, 62),
            RGB(232, 105, 153),
            RGB(112, 48, 80),
            RGB(246, 186, 90),
            RGB(252, 241, 247),
            RGB(205, 164, 187),
            RGB(88, 56, 77),
            RGB(36, 22, 34),
            RGB(58, 35, 53),
            RGB(78, 47, 70),
            RGB(69, 43, 62),
            RGB(43, 29, 41),
            RGB(137, 101, 122),
            RGB(44, 27, 42),
            RGB(73, 43, 66),
            RGB(82, 42, 67)
        };
    }

    if (theme == AppTheme::HighContrast)
    {
        return {
            RGB(0, 0, 0),
            RGB(8, 8, 8),
            RGB(16, 16, 16),
            RGB(30, 30, 30),
            RGB(0, 170, 255),
            RGB(0, 76, 120),
            RGB(255, 214, 74),
            RGB(255, 255, 255),
            RGB(210, 220, 230),
            RGB(92, 92, 92),
            RGB(5, 5, 5),
            RGB(24, 24, 24),
            RGB(42, 42, 42),
            RGB(34, 34, 34),
            RGB(18, 18, 18),
            RGB(150, 150, 150),
            RGB(10, 10, 10),
            RGB(36, 36, 36),
            RGB(0, 58, 92)
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

struct VideoCompressionAnalysisThreadResult
{
    VideoAnalysis analysis;
    std::wstring errorMessage;
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

struct AnimeImportThreadResult
{
    AnimeImportResult result;
    AnimeImportResult alternateResult;
    std::wstring userName;
    std::wstring message;
    std::wstring alternateMessage;
    std::vector<std::pair<std::wstring, std::wstring>> coverFiles;
    AnimeImportSource source = AnimeImportSource::AniList;
    AnimeImportSource alternateSource = AnimeImportSource::MyAnimeList;
    bool needsSourceChoice = false;
};

struct SmartTransferConnectThreadResult
{
    SmartTransferConnectResult result;
};

struct SmartTransferDownloadThreadResult
{
    bool success = false;
    std::wstring message;
};

struct SmartTransferWebRtcThreadResult
{
    bool success = false;
    std::wstring code;
    std::wstring message;
};

class WinHttpScopedHandle
{
public:
    explicit WinHttpScopedHandle(HINTERNET value = nullptr) : handle_(value) {}
    ~WinHttpScopedHandle()
    {
        if (handle_)
        {
            WinHttpCloseHandle(handle_);
        }
    }

    WinHttpScopedHandle(const WinHttpScopedHandle&) = delete;
    WinHttpScopedHandle& operator=(const WinHttpScopedHandle&) = delete;

    operator HINTERNET() const
    {
        return handle_;
    }

private:
    HINTERNET handle_ = nullptr;
};

struct ParsedHttpUrl
{
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    DWORD flags = WINHTTP_FLAG_SECURE;
};

std::optional<ParsedHttpUrl> ParseHttpUrl(const std::wstring& url)
{
    URL_COMPONENTSW components {};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &components))
    {
        return std::nullopt;
    }
    if (components.nScheme != INTERNET_SCHEME_HTTP && components.nScheme != INTERNET_SCHEME_HTTPS)
    {
        return std::nullopt;
    }

    ParsedHttpUrl parsed;
    parsed.host.assign(components.lpszHostName, components.dwHostNameLength);
    parsed.path.assign(components.lpszUrlPath, components.dwUrlPathLength);
    parsed.path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    if (parsed.path.empty())
    {
        parsed.path = L"/";
    }
    parsed.port = components.nPort;
    parsed.flags = components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    return parsed;
}

bool DownloadUrlWithWinHttp(const std::wstring& url, const std::filesystem::path& destination)
{
    const auto parsed = ParseHttpUrl(url);
    if (!parsed)
    {
        return false;
    }

    WinHttpScopedHandle session(WinHttpOpen(
        L"RexToolkitImageCache/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if (!session)
    {
        return false;
    }

    WinHttpSetTimeouts(session, 8000, 8000, 12000, 12000);

    WinHttpScopedHandle connection(WinHttpConnect(session, parsed->host.c_str(), parsed->port, 0));
    if (!connection)
    {
        return false;
    }

    WinHttpScopedHandle request(WinHttpOpenRequest(
        connection,
        L"GET",
        parsed->path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        parsed->flags));
    if (!request)
    {
        return false;
    }

    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    const wchar_t headers[] =
        L"Accept: image/jpeg,image/png,image/gif,*/*;q=0.8\r\n";
    if (!WinHttpSendRequest(
            request,
            headers,
            static_cast<DWORD>(-1),
            WINHTTP_NO_REQUEST_DATA,
            0,
            0,
            0) ||
        !WinHttpReceiveResponse(request, nullptr))
    {
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &statusCode,
            &statusCodeSize,
            WINHTTP_NO_HEADER_INDEX) ||
        statusCode < 200 ||
        statusCode >= 300)
    {
        return false;
    }

    std::ofstream output(destÛ6Ó«h‘éì¶»§q«^tBˆYˆ
Y\ÜØYÙHOHÓWÖ•UÓ‘ÕÓŠBˆÂˆÛÛœİÓÔ‘]ÛˆHUÓÔ‘
[İ\ÙK›[İ\ÙQ]JNÂˆÛ“[İ\ÙP]Û‘›ÜXİ]˜][Ûš[™[™Ê]ÛˆOH•UÓŒˆÈXİ]˜][Û“[İ\ÙP]Û–ˆˆXİ]˜][Û“[İ\ÙP]Û–JNÂˆ™]\›ˆYNÂˆBˆB‚ˆYˆ
]]ĞÛXÚÙ\—Ë˜Xİ]˜][Û’Ú[™OHXİ]˜][Û’[œ]Ú[™“[İ\ÙP]ÛŠBˆÂˆ™]\›ˆ˜[ÙNÂˆB‚ˆYˆ
\ĞXİ]˜][Û“[İ\ÙSY\ÜØYÙJY\ÜØYÙK[İ\ÙKYJJBˆÂˆYˆ
]]ĞÛXÚÙ\—ËÙÙÛS[ÙQ[˜X›Y
BˆÂˆYˆ
X]]ĞÛXÚÙ\Xİ]˜][Û’[ÊBˆÂˆ]]ĞÛXÚÙ\Xİ]˜][Û’[ÈHYNÂˆÙ]]]ĞÛXÚÙ\”[›š[™ÊX]]ĞÛXÚÙ\—Ëœ[›š[™ÊNÂˆBˆBˆ[ÙBˆÂˆÙ]]]ĞÛXÚÙ\”[›š[™ÊYJNÂˆBˆ™]\›ˆYNÂˆBˆ[ÙHYˆ
\ĞXİ]˜][Û“[İ\ÙSY\ÜØYÙJY\ÜØYÙK[İ\ÙK˜[ÙJJBˆÂˆ]]ĞÛXÚÙ\Xİ]˜][Û’[ÈH˜[ÙNÂˆYˆ
X]]ĞÛXÚÙ\—ËÙÙÛS[ÙQ[˜X›Y
BˆÂˆÙ]]]ĞÛXÚÙ\”[›š[™Ê˜[ÙJNÂˆBˆ™]\›ˆYNÂˆB‚ˆ™]\›ˆ˜[ÙNÂŸB‚˜›ÛÛÛÛÚ]\’\ĞXİ]˜][Û“[İ\ÙSY\ÜØYÙJÔTSHY\ÜØYÙKÛÛœİTÓÓÒÔÕ•PÕ	ˆ[İ\ÙK›ÛÛİÛŠHÛÛœİÂˆİÚ]Ú
]]ĞÛXÚÙ\—Ë˜Xİ]˜][Û“[İ\ÙP]ÛŠBˆÂˆØ\ÙHXİ]˜][Û“[İ\ÙP]Û“Y‚ˆ™]\›ˆY\ÜØYÙHOH
İÛˆÈÓWÓ•UÓ‘ÕÓˆˆÓWÓ•UÓ•T
NÂˆØ\ÙHXİ]˜][Û“[İ\ÙP]Û”šYÚ‚ˆ™]\›ˆY\ÜØYÙHOH
İÛˆÈÓWÔ•UÓ‘ÕÓˆˆÓWÔ•UÓ•T
NÂˆØ\ÙHXİ]˜][Û“[İ\ÙP]Û“ZYN‚ˆ™]\›ˆY\ÜØYÙHOH
İÛˆÈÓWÓP•UÓ‘ÕÓˆˆÓWÓP•UÓ•T
NÂˆØ\ÙHXİ]˜][Û“[İ\ÙP]Û–N‚ˆ™]\›ˆY\ÜØYÙHOH
İÛˆÈÓWÖ•UÓ‘ÕÓˆˆÓWÖ•UÓ•T
H	‰ˆUÓÔ‘
[İ\ÙK›[İ\ÙQ]JHOH•UÓŒNÂˆØ\ÙHXİ]˜][Û“[İ\ÙP]Û–‚ˆ™]\›ˆY\ÜØYÙHOH
İÛˆÈÓWÖ•UÓ‘ÕÓˆˆÓWÖ•UÓ•T
H	‰ˆUÓÔ‘
[İ\ÙK›[İ\ÙQ]JHOH•UÓŒÂˆB‚ˆ™]\›ˆ˜[ÙNÂŸB‚œİ™XİÜÛÛYš[š][ÛˆÛÛÚ]\•š\ÚX›UÛÛÑ›Üİ\œ™[YÙJ
HÛÛœİÂˆYˆ
İ\œ™[YÙWÈOHYÙN[ÛÛÊBˆÂˆÛÛœİİÜİš[™È]Y\HH[ÛÛÔÙX\˜ÚXÙZÛ\Xİ]™WÂˆÈˆ‚ˆˆİÙ\˜Ø\ÙU^
š[UÚ]\ÜXÙJÙ]Ú[™İÕ^İš[™Ê[ÛÛÔÙX\˜ÚY]ÊJJNÂˆYˆ
]Y\K™[\J
JBˆÂˆ™]\›ˆÛÛ×ÎÂˆB‚ˆİÚ\İš[™Üİ™X[H]Y\Tİ™X[J]Y\JNÂˆİ™XİÜİÜİš[™Ïˆ\›\ÎÂˆİÜİš[™È\›NÂˆÚ[H
]Y\Tİ™X[Hˆ\›JBˆÂˆ\›\Ëœ\ÚØ˜XÚÊ\›JNÂˆBˆYˆ
\›\Ë™[\J
JBˆÂˆ™]\›ˆÛÛ×ÎÂˆB‚ˆİ™XİÜÛÛYš[š][Ûˆš[\™YÛÛÎÂˆİ˜ÛÜWÚYŠˆÛÛ×Ë˜™YÚ[Š
KˆÛÛ×Ë™[™

Kˆİ˜˜XÚ×Ú[œÙ\\Šš[\™YÛÛÊKˆÉ\›\×JÛÛœİÛÛYš[š][Û‰ˆÛÛ
BˆÂˆÛÛœİİÜİš[™È^\İXÚÈHİÙ\˜Ø\ÙU^
ÛÛ›˜[YH
Èˆˆ
ÈÛÛ™\ØÜš\[Ûˆ
Èˆˆ
ÈÛÛšY
NÂˆ™]\›ˆİ˜[ÛÙŠˆ\›\Ë˜™YÚ[Š
Kˆ\›\Ë™[™

KˆÉš^\İXÚ×JÛÛœİİÜİš[™ÉˆÙX\˜Ú\›JBˆÂˆ™]\›ˆ^\İXÚË™š[™
ÙX\˜Ú\›JHOHİÜİš[™Î›œÜÎÂˆJNÂˆJNÂˆ™]\›ˆš[\™YÛÛÎÂˆB‚ˆİ™XİÜÛÛYš[š][Ûˆ˜]›Üš]\ÎÂˆİ˜ÛÜWÚYŠˆÛÛ×Ë˜™YÚ[Š
KˆÛÛ×Ë™[™

Kˆİ˜˜XÚ×Ú[œÙ\\Š˜]›Üš]\ÊKˆ×JÛÛœİÛÛYš[š][Û‰ˆÛÛ
BˆÂˆ™]\›ˆÛÛ™˜]›Üš]NÂˆJNÂˆ™]\›ˆ˜]›Üš]\ÎÂŸB‚˜ÛÛœİÛÛYš[š][ÛŠˆÛÛÚ]\‘š[™ÛÛ
ÛÛÚ[™ÛÛ
HÛÛœİÂˆÛÛœİ]]È›İ[™Hİ™š[™ÚYŠˆÛÛ×Ë˜™YÚ[Š
KˆÛÛ×Ë™[™

KˆİÛÛJÛÛœİÛÛYš[š][Û‰ˆYš[š][ÛŠBˆÂˆ™]\›ˆYš[š][Û‹šÚ[™OHÛÛÂˆJNÂ‚ˆYˆ
›İ[™OHÛÛ×Ë™[™

JBˆÂˆ™]\›ˆ[ÂˆB‚ˆ™]\›ˆ	Š
™›İ[™
NÂŸB‚”‘PÕÛÛÚ]\•ÛÛØ\™™Xİ
Ú^™Wİ[™^
HÛÛœİÂˆÛÛœİ[ÛÛ[[œÈHÎÂˆÛÛœİ[X\™Ú[ˆH\ÊŠNÂˆÛÛœİ[Ø\H\ÊN
NÂˆÛÛœİ[ÜHÛÛ[™XİËÜ
È
İ\œ™[YÙWÈOHYÙN[ÛÛÈÈ\ÊNN
Hˆ\ÊL
JHHØÜ›ÛÙ™œÙ]WÎÂˆÛÛœİ[]˜Z[X›UÚYHİ›X^
Kİ]X×ØØ\İ[ŠÛÛ[™XİËœšYÚHÛÛ[™XİË›Y
HH
X\™Ú[ˆ
ˆŠJNÂˆÛÛœİ[Ø\™Ú^™HHİ›X^
\ÊMŒ
K
]˜Z[X›UÚYH
Ø\
ˆ
ÛÛ[[œÈHJJJHÈÛÛ[[œÊNÂˆÛÛœİ[ÛÛ[[ˆHİ]X×ØØ\İ[Š[™^	HÛÛ[[œÊNÂˆÛÛœİ[›İÈHİ]X×ØØ\İ[Š[™^ÈÛÛ[[œÊNÂˆÛÛœİ[YHÛÛ[™XİË›Y
ÈX\™Ú[ˆ
È
ÛÛ[[ˆ
ˆ
Ø\™Ú^™H
ÈØ\
JNÂˆÛÛœİ[Ø\™ÜHÜ
È
›İÈ
ˆ
Ø\™Ú^™H
ÈØ\
JNÂ‚ˆ™]\›ˆÂˆYˆØ\™ÜˆY
ÈØ\™Ú^™KˆØ\™Ü
ÈØ\™Ú^™BˆNÂŸB‚”‘PÕÛÛÚ]\•ÛÛ˜]›Üš]T™Xİ
ÛÛœİ‘PÕ	ˆØ\™
HÛÛœİÂˆ™]\›ˆÂˆØ\™›Y
È\ÊMŠKˆØ\™Ü
È\ÊM
KˆØ\™›Y
È\Ê
KˆØ\™Ü
È\ÊŠBˆNÂŸB‚›ÚYÛÛÚ]\•ÙÙÛQ˜]›Üš]JÛÛÚ[™ÛÛ
BÂˆÛÛœİ]]È›İ[™Hİ™š[™ÚYŠˆÛÛ×Ë˜™YÚ[Š
KˆÛÛ×Ë™[™

KˆİÛÛJÛÛœİÛÛYš[š][Û‰ˆYš[š][ÛŠBˆÂˆ™]\›ˆYš[š][Û‹šÚ[™OHÛÛÂˆJNÂ‚ˆYˆ
›İ[™OHÛÛ×Ë™[™

JBˆÂˆ™]\›ÂˆB‚ˆ›İ[™O™˜]›Üš]HHY›İ[™O™˜]›Üš]NÂˆØ]™Q˜]›Üš]\Ê
NÂˆİ™\•ÛÛ[™^ÈHLNÂˆ[˜[Y]T™Xİ
Û™Ë[‹SÑJNÂŸB‚œİÜİš[™ÈÛÛÚ]\Xİ]˜][Û’Ù^SX™[

HÛÛœİÂˆYˆ
]]ĞÛXÚÙ\—Ë˜Xİ]˜][Û’Ú[™OHXİ]˜][Û’[œ]Ú[™“[İ\ÙP]ÛŠBˆÂˆ™]\›ˆXİ]˜][Û“[İ\ÙP]Û“X™[
]]ĞÛXÚÙ\—Ë˜Xİ]˜][Û“[İ\ÙP]ÛŠNÂˆB‚ˆ™]\›ˆÙ^SX™[
]]ĞÛXÚÙ\—Ë˜Xİ]˜][Û’Ù^JNÂŸB‚œİÜİš[™ÈÛÛÚ]\“İ]]]Û“X™[

HÛÛœİÂˆ™]\›ˆİ]]š[™[™ÓX™[
]]ĞÛXÚÙ\—Ë›İ]]Ú[™]]ĞÛXÚÙ\—Ë›İ]]Ù^K]]ĞÛXÚÙ\—Ë›İ]]]ÛŠNÂŸB‚œİÜİš[™ÈÛÛÚ]\”İ]\ÓX™[
ÛÛ™\œÚ[Û”İ]\Èİ]\ÊHÛÛœİÂˆİÚ]Ú
İ]\ÊBˆÂˆØ\ÙHÛÛ™\œÚ[Û”İ]\Î”[™[™Î‚ˆ™]\›ˆ”[™[™ÈÂˆØ\ÙHÛÛ™\œÚ[Û”İ]\ÎÛÛ™\[™Î‚ˆ™]\›ˆÛÛ™\[™ÈÂˆØ\ÙHÛÛ™\œÚ[Û”İ]\ÎÛÛ\]N‚ˆ™]\›ˆÛÛ\]HÂˆØ\ÙHÛÛ™\œÚ[Û”İ]\Î‘˜Z[Y‚ˆ™]\›ˆ‘˜Z[YÂˆØ\ÙHÛÛ™\œÚ[Û”İ]\Î”ÚÚ\Y‚ˆ™]\›ˆ”ÚÚ\YÂˆB‚ˆ™]\›ˆ”[™[™ÈÂŸB‚œİÜİš[™ÈÛÛÚ]\“İ]]›Ü›X]X™[
[XYÙQ›Ü›X]›Ü›X]
HÛÛœİÂˆ™]\›ˆİ\ÜY›Ü›X]™YÚ\İN“X™[›ÜŠ›Ü›X]
NÂŸB‚œİÜİš[™ÈÛÛÚ]\ÛÛ™›Xİ™Z]š[Ü“X™[

HÛÛœİÂˆİÚ]Ú
ÛÛ™\œÚ[Û“Ü[Ûœ×Ë˜ÛÛ™›Xİ™Z]š[ÜŠBˆÂˆØ\ÙHÛÛ™›Xİ™Z]š[Ü]]Ô™[˜[YN‚ˆ™]\›ˆ]]Ë\™[˜[YHÂˆØ\ÙHÛÛ™›Xİ™Z]š[Ü“İ™\Üš]N‚ˆ™]\›ˆ“İ™\Üš]HÂˆØ\ÙHÛÛ™›Xİ™Z]š[Ü”ÚÚ\‚ˆ™]\›ˆ”ÚÚ\ÂˆB‚ˆ™]\›ˆ]]Ë\™[˜[YHÂŸB‚™İX›HÛÛÚ]\ÛÛ™\\”›ÙÜ™\ÜÊ
HÛÛœİÂˆYˆ
ÛÛ™\œÚ[Û’›Øœ×Ë™[\J
JBˆÂˆ™]\›ˆŒÂˆB‚ˆ[š[š\ÚYHÂˆ›Üˆ
ÛÛœİÛÛ™\œÚ[Û’›Ø‰ˆ›ØˆˆÛÛ™\œÚ[Û’›Øœ×ÊBˆÂˆYˆ
›Ø‹œİ]\ÈOHÛÛ™\œÚ[Û”İ]\ÎÛÛ\]Hˆ›Ø‹œİ]\ÈOHÛÛ™\œÚ[Û”İ]\Î‘˜Z[Yˆ›Ø‹œİ]\ÈOHÛÛ™\œÚ[Û”İ]\Î”ÚÚ\Y
BˆÂˆ
ÊÙš[š\ÚYÂˆBˆB‚ˆ™]\›ˆİ]X×ØØ\İİX›OŠš[š\ÚY
HÈİ]X×ØØ\İİX›OŠÛÛ™\œÚ[Û’›Øœ×ËœÚ^™J
JNÂŸB‚˜›ÛÛÛÛÚ]\Ø[”İ\YYXQİÛ›ØY

HÛÛœİÂˆYˆ
YYXP[˜[^š[™×ÈYYXS]\ÚXĞ[˜[^š[™×ÈYYXQİÛ›ØY[™×ÊBˆÂˆ™]\›ˆ˜[ÙNÂˆB‚ˆYˆ
SYYXTÙ]\Y\ÜØYÙJ
K™[\J
JBˆÂˆ™]\›ˆ˜[ÙNÂˆB‚ˆYˆ
Tİ\ÜY]›Ü›T™YÚ\İN’\Ôİ\ÜY\›
Ù]Ú[™İÕ^İš[™ÊYYXU\›Y]ÊJJBˆÂˆ™]\›ˆ˜[ÙNÂˆB‚ˆYˆ
Tİ\ÜY]›Ü›T™YÚ\İN’\ÓZÙ[Q\™XİYYXU\›
Ù]Ú[™İÕ^İš[™ÊYYXU\›Y]ÊJJBˆÂˆ™]\›ˆ˜[ÙNÂˆB‚ˆYˆ
YYXQİÛ›ØY›Ø—Ëœİ]\ÈOHYYXQİÛ›ØYİ]\Î”™XYHˆYYXQİÛ›ØY›Ø—Ë\›OHÙ]Ú[™İÕ^İš[™ÊYYXU\›Y]ÊJBˆÂˆ™]\›ˆ˜[ÙNÂˆB‚ˆYˆ
YYXQİÛ›ØYÜ[Ûœ×Ë›İ]]›Û\‹™[\J
Hˆ\İ™š[\Ş\İ[N™^\İÊYYXQİÛ›ØYÜ[Ûœ×Ë›İ]]›Û\ŠJBˆÂˆ™]\›ˆ˜[ÙNÂˆB‚ˆYˆ
YYXQİÛ›ØYÜ[Ûœ×Ë›İ]]›Ü›X]OHYYXSİ]]›Ü›X]“\	‰‚ˆYYXQİÛ›ØY›Ø—Ëœ]›Ü›HOHYYXT]›Ü›N”Ûİ[™ÛİY
BˆÂˆ™]\›ˆ˜[ÙNÂˆB‚ˆ™]\›ˆYNÂŸB‚œİÜİš[™ÈÛÛÚ]\“YYXT]X[]SX™[

HÛÛœİÂˆYˆ
YYXQİÛ›ØYÜ[Ûœ×Ë›İ]]›Ü›X]OHYYXSİ]]›Ü›X]“\
BˆÂˆ™]\›ˆYYXQİÛ›ØYÙ\šXÙN“\]X[]SX™[
YYXQİÛ›ØYÜ[Ûœ×Ë›\]X[]JNÂˆBˆYˆ
YYXQİÛ›ØYÜ[Ûœ×Ë›İ]]›Ü›X]OHYYXSİ]]›Ü›X]“\ÊBˆÂˆ™]\›ˆYYXQİÛ›ØYÙ\šXÙN“\Ğš]˜]SX™[
YYXQİÛ›ØYÜ[Ûœ×Ë›\Ğš]˜]JNÂˆBˆ™]\›ˆ•ĞUˆÜÜÛ\ÜÈÈ[˜ÛÛ\™\ÜÙYÂŸB‚œİÜİš[™ÈÛÛÚ]\“YYXTÙ]\Y\ÜØYÙJ
HÛÛœİÂˆİÜİš[™ÈY\ÜØYÙNÂˆYˆ
[YYXQ^\›˜[ÛÛ×Ë]›İ[™
BˆÂˆY\ÜØYÙH
ÏH]Y\ÈZ\ÜÚ[™ÈÂˆBˆYˆ
[YYXQ^\›˜[ÛÛ×Ë™™›\YÑ›İ[™
BˆÂˆYˆ
[Y\ÜØYÙK™[\J
JBˆÂˆY\ÜØYÙH
ÏHÈÂˆBˆY\ÜØYÙH
ÏH‘‘›\YÈ\ÈZ\ÜÚ[™ÈÂˆB‚ˆYˆ
[Y\ÜØYÙK™[\J
JBˆÂˆY\ÜØYÙH
ÏH‹ˆ]Z\ÜÚ[™ÈÛÛÈ[ˆH\ÛÛÈ›Û\ˆÜˆXZÙH[H]˜Z[X›HÛˆUˆÂˆBˆ™]\›ˆY\ÜØYÙNÂŸB‚œİÜİš[™ÈÛÛÚ]\İ\œ™[]U[YSX™[

HÛÛœİÂˆÖTÕSUSQHØØ[[YHßNÂˆÙ]ØØ[[YJ	›ØØ[[YJNÂ‚ˆØÚ\—İY™™\–ÍHßNÂˆYˆ
\Ù][™Ü×Ë˜ÛØÚÑ›Ü›X]OHÛØÚÑ›Ü›X]“[Û^LLˆˆ\Ù][™Ü×Ë˜ÛØÚÑ›Ü›X]OHÛØÚÑ›Ü›X]‘œšY[™LLŠBˆÂˆÛÛœİØÚ\—İ
ˆİY™š^HØØ[[YKÒİ\ˆHLˆÈ”HˆˆSHÂˆÓÔ‘İ\ˆHØØ[[YKÒİ\ˆ	HLÂˆYˆ
İ\ˆOH
BˆÂˆİ\ˆHLÂˆB‚ˆYˆ
\Ù][™Ü×Ë˜ÛØÚÑ›Ü›X]OHÛØÚÑ›Ü›X]‘œšY[™LLŠBˆÂˆİ]XÈÛÛœİ^ˆØÚ\—İÙYZÙ^\Ö×VÌLHÂˆ”İ[™^H‹“[Û™^H‹•Y\Ù^H‹•ÙY™\Ù^H‹•\œÙ^H‹‘œšY^H‹”Ø]\™^H‚ˆNÂˆİÜš[—ÜÊˆY™™\‹ˆ‰\È	]N‰LN‰LH	\È‹ˆÙYZÙ^\ÖÛØØ[[YKÑ^SÙ•ÙYZ×Kˆİ\‹ˆØØ[[YKÓZ[]KˆØØ[[YKÔÙXÛÛ™ˆİY™š^
NÂˆBˆ[ÙBˆÂˆİÜš[—ÜÊˆY™™\‹ˆ‰LKÉLKÉLH	]N‰LN‰LH	\È‹ˆØØ[[YKÓ[ÛˆØØ[[YKÑ^KˆØØ[[YKÖYX\‹ˆİ\‹ˆØØ[[YKÓZ[]KˆØØ[[YKÔÙXÛÛ™ˆİY™š^
NÂˆBˆBˆ[ÙHYˆ
\Ù][™Ü×Ë˜ÛØÚÑ›Ü›X]OHÛØÚÑ›Ü›X]’\ÛÌ
BˆÂˆİÜš[—ÜÊˆY™™\‹ˆ‰LKILKILH	LN‰LN‰LH‹ˆØØ[[YKÖYX\‹ˆØØ[[YKÓ[ÛˆØØ[[YKÑ^KˆØØ[[YKÒİ\‹ˆØØ[[YKÓZ[]KˆØØ[[YKÔÙXÛÛ™
NÂˆBˆ[ÙBˆÂˆİÜš[—ÜÊˆY™™\‹ˆ‰LKÉLKÉLH	LN‰LN‰LH‹ˆØØ[[YKÓ[ÛˆØØ[[YKÑ^KˆØØ[[YKÖYX\‹ˆØØ[[YKÒİ\‹ˆØØ[[YKÓZ[]KˆØØ[[YKÔÙXÛÛ™
NÂˆBˆ™]\›ˆY™™\ÂŸB‚œİÜİš[™ÈÛÛÚ]\”İ\YÙSX™[

HÛÛœİÂˆ™]\›ˆ\Ù][™Ü×Ëœİ\YÙHOHY˜][İ\YÙN[ÛÛÈÈ[ÛÛÈˆˆ‘˜]›Üš]\ÈÂŸB‚œİÜİš[™ÈÛÛÚ]\ÛØÚÑ›Ü›X]X™[

HÛÛœİÂˆİÚ]Ú
\Ù][™Ü×Ë˜ÛØÚÑ›Ü›X]
BˆÂˆØ\ÙHÛØÚÑ›Ü›X]“[Û^LL‚ˆ™]\›ˆ“SKÑÖVVVHL‹Zİ\ˆÂˆØ\ÙHÛØÚÑ›Ü›X]’\ÛÌ‚ˆ™]\›ˆ–VVVKSSKQZİ\ˆÂˆØ\ÙHÛØÚÑ›Ü›X]‘œšY[™LL‚ˆ™]\›ˆ•ÙYZÙ^H
ÈL‹Zİ\ˆÂˆØ\ÙHÛØÚÑ›Ü›X]“[Û^L‚ˆ™]\›ˆ“SKÑÖVVVHZİ\ˆÂˆBˆ™]\›ˆ“SKÑÖVVVHZİ\ˆÂŸB‚œİÜİš[™ÈÛÛÚ]\•[YSX™[

HÛÛœİÂˆİÚ]Ú
\Ù][™Ü×Ë[YJBˆÂˆØ\ÙH\[YN“YÚ‚ˆ™]\›ˆ“YÚÂˆØ\ÙH\[YN“ZYšYÚ‚ˆ™]\›ˆ“ZYšYÚÂˆØ\ÙH\[YN‘›Ü™\İ‚ˆ™]\›ˆ‘›Ü™\İÂˆØ\ÙH\[YN”›ÜÙN‚ˆ™]\›ˆ”›ÜÙHÂˆØ\ÙH\[YN’YÚÛÛ˜\İ‚ˆ™]\›ˆ’YÚÛÛ˜\İÂˆØ\ÙH\[YN‘\šÎ‚ˆ™]\›ˆ‘\šÈÂˆBˆ™]\›ˆ‘\šÈÂŸB‚œİÜİš[™ÈÛÛÚ]\”ÛX\˜[œÙ™\‘^\˜][Û“X™[

HÛÛœİÂˆİÚ]Ú
ÛX\˜[œÙ™\“Ü[Ûœ×Ë™^\˜][ÛŠBˆÂˆØ\ÙHÛX\˜[œÙ™\‘^\˜][Û‘šYY[“Z[]\Î‚ˆ™]\›ˆŒMHZ[]\ÈÂˆØ\ÙHÛX\˜[œÙ™\‘^\˜][Û•\SZ[]\Î‚ˆ™]\›ˆŒÌZ[]\ÈÂˆØ\ÙHÛX\˜[œÙ™\‘^\˜][Û“Û™Rİ\‚ˆ™]\›ˆŒHİ\ˆÂˆØ\ÙHÛX\˜[œÙ™\‘^\˜][Û“X[X[‚ˆ™]\›ˆ•[[X[X[HİÜYÂˆBˆ™]\›ˆŒÌZ[]\ÈÂŸB‚œİÜİš[™ÈÛÛÚ]\”ÛX\˜[œÙ™\’Üİİ]\ÓX™[

HÛÛœİÂˆİÚ]Ú
ÛX\˜[œÙ™\’ÜİÛ˜\ÚİËœİ]\ÊBˆÂˆØ\ÙHÛX\˜[œÙ™\’Üİİ]\Î”™\\š[™Î‚ˆ™]\›ˆ”™\\š[™Èš[\Ë‹‹ˆÂˆØ\ÙHÛX\˜[œÙ™\’Üİİ]\Î’Üİ[™Î‚ˆ™]\›ˆ•ØZ][™È›Üˆ™XÙZ]™\‹ˆÂˆØ\ÙHÛX\˜[œÙ™\’Üİİ]\Î•ØZ][™Ñ›Ü\›İ˜[‚ˆ™]\›ˆ•ØZ][™È›ÜˆÙ[™\ˆ\›İ˜[ˆÂˆØ\ÙHÛX\˜[œÙ™\’Üİİ]\Î”Ù[™[™Î‚ˆ™]\›ˆÛX\˜[œÙ™\’ÜİÛ˜\ÚİË˜İ\œ™[š[K™[\J
BˆÈ”Ù[™[™Ë‹‹ˆ‚ˆˆ”Ù[™[™Èˆ
ÈÛX\˜[œÙ™\’ÜİÛ˜\ÚİË˜İ\œ™[š[H
È‹‹‹ˆÂˆØ\ÙHÛX\˜[œÙ™\’Üİİ]\ÎÛÛ\]N‚ˆ™]\›ˆ•˜[œÙ™\ˆÛÛ\]KˆÂˆØ\ÙHÛX\˜[œÙ™\’Üİİ]\Î‘˜Z[Y‚ˆ™]\›ˆ•˜[œÙ™\ˆ˜Z[YˆÂˆØ\ÙHÛX\˜[œÙ™\’Üİİ]\ÎØ[˜Ù[Y‚ˆ™]\›ˆ•˜[œÙ™\ˆØ[˜Ù[YˆÂˆØ\ÙHÛX\˜[œÙ™\’Üİİ]\Î’YN‚ˆ™]\›ˆ”™XYKˆÂˆBˆ™]\›ˆ”™XYKˆÂŸB‚œİÜİš[™ÈÛÛÚ]\”ÛX\˜[œÙ™\ÛY[İ]\ÓX™[

HÛÛœİÂˆİÚ]Ú
ÛX\˜[œÙ™\‘İÛ›ØY›ÙÜ™\Ü×Ëœİ]\ÊBˆÂˆØ\ÙHÛX\˜[œÙ™\ÛY[İ]\ÎÛÛ›™Xİ[™Î‚ˆ™]\›ˆÛÛ›™Xİ[™Ë‹‹ˆÂˆØ\ÙHÛX\˜[œÙ™\ÛY[İ]\Î•ØZ][™Ñ›Ü\›İ˜[‚ˆ™]\›ˆ•ØZ][™È›ÜˆÙ[™\ˆ\›İ˜[ˆÂˆØ\ÙHÛX\˜[œÙ™\ÛY[İ]\ÎÛÛ›™XİY‚ˆ™]\›ˆÛÛ›™XİY\Ú[™ÈØØ[™]ÛÜšËˆÂˆØ\ÙHÛX\˜[œÙ™\ÛY[İ]\Î‘İÛ›ØY[™Î‚ˆ™]\›ˆ‘İÛ›ØY[™Ë‹‹ˆÂˆØ\ÙHÛX\˜[œÙ™\ÛY[İ]\ÎÛÛ\]N‚ˆ™]\›ˆ‘İÛ›ØYÛÛ\]KˆÂˆØ\ÙHÛX\˜[œÙ™\ÛY[İ]\Î‘˜Z[Y‚ˆ™]\›ˆ‘İÛ›ØY˜Z[YˆÂˆØ\ÙHÛX\˜[œÙ™\ÛY[İ]\ÎØ[˜Ù[Y‚ˆ™]\›ˆ‘İÛ›ØYØ[˜Ù[YˆÂˆØ\ÙHÛX\˜[œÙ™\ÛY[İ]\Î’YN‚ˆ™]\›ˆ”™XYKˆÂˆBˆ™]\›ˆ”™XYKˆÂŸB‚›ÚYÛÛÚ]\\U[YJ
BÂˆ\T[]J\Ù][™Ü×Ë[YJNÂˆ™Yœ™\Ú[YXÛÛ”™\Ûİ\˜Ù\Ê
NÂˆYˆ
Y]˜XÚÙÜ›İ[™œ\ÚÊBˆÂˆ[]SØš™Xİ
Y]˜XÚÙÜ›İ[™œ\ÚÊNÂˆBˆY]˜XÚÙÜ›İ[™œ\ÚÈHÜ™X]TÛÛYœ\Ú
Ò[œ]˜XÚÙÜ›İ[™
NÂˆYˆ
Û™ÊBˆÂˆ\Q\šÕ]P˜\Š
NÂˆ[˜[Y]T™Xİ
Û™Ë[‹SÑJNÂˆBˆYˆ
XXÜ›Óİ™\›^UÚ[™İ×ÊBˆÂˆ\U]P˜\•[YJXXÜ›Óİ™\›^UÚ[™İ×ÊNÂˆ[˜[Y]SXXÜ›Óİ™\›^J
NÂˆBŸB‚š[ÛÛÚ]\‘\Ê[˜[YJHÛÛœİÂˆ™]\›ˆ][]Š˜[YKWËMŠNÂŸB‚š[ÛÛÚ]\“XXÜ›Óİ™\›^Q\Ê[˜[YJHÛÛœİÂˆ™]\›ˆ][]Š˜[YKXXÜ›Óİ™\›^QWÈˆÈXXÜ›Óİ™\›^QWÈˆWËMŠNÂŸB