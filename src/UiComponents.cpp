#include "UiComponents.h"

#include <gdiplus.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <new>

namespace rex::ui
{
namespace
{
constexpr wchar_t kEditContextProperty[] = L"RexToolkit.ThemedEditContext";
constexpr UINT kEditMenuUndo = 1;
constexpr UINT kEditMenuCut = 2;
constexpr UINT kEditMenuCopy = 3;
constexpr UINT kEditMenuPaste = 4;
constexpr UINT kEditMenuDelete = 5;
constexpr UINT kEditMenuSelectAll = 6;

struct ThemedEditState
{
    WNDPROC originalProc = nullptr;
    Palette palette;
    UINT menuDpi = 96;
};

struct EditMenuItem
{
    const wchar_t* label = L"";
    bool separator = false;
};

void FillMenuRect(HDC hdc, const RECT& rect, COLORREF color)
{
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(hdc, &rect, brush);
    DeleteObject(brush);
}

enum class SurfaceRole
{
    None,
    Control,
    Input,
    Dropdown
};

int Dips(int value, UINT dpi)
{
    return MulDiv(value, static_cast<int>(dpi), 96);
}

bool HasArea(const RECT& rect)
{
    return rect.right > rect.left && rect.bottom > rect.top;
}

Gdiplus::Color AlphaColor(COLORREF color, BYTE alpha = 255)
{
    return Gdiplus::Color(
        alpha,
        GetRValue(color),
        GetGValue(color),
        GetBValue(color));
}

void BuildRoundedPath(Gdiplus::GraphicsPath& path, const RECT& rect, int radius)
{
    const int width = static_cast<int>(rect.right - rect.left);
    const int height = static_cast<int>(rect.bottom - rect.top);
    const int safeRadius = std::max(1, std::min(
        radius,
        std::max(1, std::min(width, height) / 2)));
    const int diameter = safeRadius * 2;
    path.AddArc(rect.left, rect.top, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(rect.right - diameter, rect.top, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(rect.right - diameter, rect.bottom - diameter, diameter, diameter, 0.0f, 90.0f);
    path.AddArc(rect.left, rect.bottom - diameter, diameter, diameter, 90.0f, 90.0f);
    path.CloseFigure();
}

SurfaceRole RoleForColor(COLORREF color, const Palette& palette)
{
    if (color == palette.inputBackground)
    {
        return SurfaceRole::Input;
    }
    if (color == palette.buttonBackground ||
        color == palette.buttonHover ||
        color == palette.buttonPressed ||
        color == palette.disabledBackground)
    {
        return SurfaceRole::Control;
    }
    if (color == palette.dropdownBackground ||
        color == palette.dropdownHover ||
        color == palette.dropdownSelected)
    {
        return SurfaceRole::Dropdown;
    }
    return SurfaceRole::None;
}

int SurfaceOpacity(const Palette& palette, SurfaceRole role)
{
    if (palette.surfaceStyle == SurfaceStyle::Solid || role == SurfaceRole::None)
    {
        return 100;
    }

    const int strength = std::clamp(palette.surfaceOpacity, 35, 100);
    if (palette.surfaceStyle == SurfaceStyle::Translucent)
    {
        switch (role)
        {
        case SurfaceRole::Control:
            return std::clamp(56 + strength * 38 / 100, 69, 94);
        case SurfaceRole::Input:
            return std::clamp(82 + strength * 16 / 100, 87, 98);
        case SurfaceRole::Dropdown:
            return std::clamp(84 + strength * 14 / 100, 88, 98);
        default:
            return 100;
        }
    }

    switch (role)
    {
    case SurfaceRole::Control:
        return std::clamp(48 + strength * 34 / 100, 60, 82);
    case SurfaceRole::Input:
        return std::clamp(78 + strength * 18 / 100, 84, 96);
    case SurfaceRole::Dropdown:
        return std::clamp(80 + strength * 18 / 100, 86, 98);
    default:
        return 100;
    }
}

void FillRounded(
    HDC hdc,
    const RECT& rect,
    int radius,
    COLORREF color,
    const Palette& palette,
    SurfaceRole role)
{
    if (!HasArea(rect))
    {
        return;
    }

    const int opacity = SurfaceOpacity(palette, role);
    if (opacity < 100)
    {
        Gdiplus::Graphics graphics(hdc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::GraphicsPath path;
        BuildRoundedPath(path, rect, radius);
        const COLORREF fillColor = palette.surfaceStyle == SurfaceStyle::Glass
            ? BlendColor(color, palette.frostHighlight, 4)
            : color;
        Gdiplus::SolidBrush brush(AlphaColor(
            fillColor,
            static_cast<BYTE>(opacity * 255 / 100)));
        graphics.FillPath(&brush, &path);
        return;
    }

    HBRUSH brush = CreateSolidBrush(color);
    HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(hdc, brush));
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, pen));
    const int diameter = std::max(2, std::min(
        radius * 2,
        std::min(
            static_cast<int>(rect.right - rect.left),
            static_cast<int>(rect.bottom - rect.top))));
    RoundRect(hdc, rect.left, rect.top, rect.right, rect.bottom, diameter, diameter);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void FillRounded(
    HDC hdc,
    const RECT& rect,
    int radius,
    COLORREF color,
    const Palette& palette)
{
    FillRounded(hdc, rect, radius, color, palette, RoleForColor(color, palette));
}

void StrokeRounded(
    HDC hdc,
    const RECT& rect,
    int radius,
    COLORREF color,
    const Palette& palette)
{
    if (!HasArea(rect))
    {
        return;
    }

    if (palette.surfaceStyle != SurfaceStyle::Solid && color == palette.border)
    {
        Gdiplus::Graphics graphics(hdc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::GraphicsPath path;
        BuildRoundedPath(path, rect, radius);
        const BYTE alpha = palette.surfaceStyle == SurfaceStyle::Glass ? 118 : 182;
        const COLORREF borderColor = palette.surfaceStyle == SurfaceStyle::Glass
            ? BlendColor(palette.border, palette.textPrimary, 16)
            : palette.border;
        Gdiplus::Pen pen(AlphaColor(borderColor, alpha), 1.0f);
        graphics.DrawPath(&pen, &path);
        return;
    }

    HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(hdc, GetStockObject(NULL_BRUSH)));
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, pen));
    const int diameter = std::max(2, std::min(
        radius * 2,
        std::min(
            static_cast<int>(rect.right - rect.left),
            static_cast<int>(rect.bottom - rect.top))));
    RoundRect(hdc, rect.left, rect.top, rect.right, rect.bottom, diameter, diameter);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
}

void DrawTextLine(
    HDC hdc,
    std::wstring_view text,
    const RECT& bounds,
    HFONT font,
    COLORREF color,
    UINT format)
{
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, color);
    HGDIOBJ previousFont = font ? SelectObject(hdc, font) : nullptr;
    RECT copy = bounds;
    DrawTextW(
        hdc,
        text.data(),
        static_cast<int>(text.size()),
        &copy,
        format | DT_NOPREFIX);
    if (previousFont)
    {
        SelectObject(hdc, previousFont);
    }
}

void PaintCheckmark(HDC hdc, const RECT& bounds, COLORREF color, UINT dpi)
{
    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    Gdiplus::Pen pen(
        AlphaColor(color),
        static_cast<Gdiplus::REAL>(std::max(1, Dips(2, dpi))));
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);

    const Gdiplus::REAL left = static_cast<Gdiplus::REAL>(bounds.left);
    const Gdiplus::REAL top = static_cast<Gdiplus::REAL>(bounds.top);
    const Gdiplus::REAL width = static_cast<Gdiplus::REAL>(bounds.right - bounds.left);
    const Gdiplus::REAL height = static_cast<Gdiplus::REAL>(bounds.bottom - bounds.top);
    Gdiplus::PointF points[3] {
        { left + width * 0.22f, top + height * 0.52f },
        { left + width * 0.43f, top + height * 0.73f },
        { left + width * 0.80f, top + height * 0.29f }
    };
    graphics.DrawLines(&pen, points, static_cast<INT>(std::size(points)));
}

bool PaintEditMenuItem(
    const ThemedEditState& state,
    const DRAWITEMSTRUCT& item)
{
    if (item.CtlType != ODT_MENU || item.itemData == 0)
    {
        return false;
    }

    const auto* menuItem = reinterpret_cast<const EditMenuItem*>(item.itemData);
    const Palette& palette = state.palette;
    FillMenuRect(item.hDC, item.rcItem, palette.dropdownBackground);
    if (menuItem->separator)
    {
        RECT line = item.rcItem;
        const int inset = Dips(12, state.menuDpi);
        line.left += inset;
        line.right -= inset;
        line.top = (line.top + line.bottom) / 2;
        line.bottom = line.top + 1;
        FillMenuRect(item.hDC, line, palette.border);
        return true;
    }

    const bool disabled = (item.itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;
    const bool selected = !disabled && (item.itemState & ODS_SELECTED) != 0;
    if (selected)
    {
        RECT highlight = item.rcItem;
        InflateRect(
            &highlight,
            -Dips(4, state.menuDpi),
            -Dips(2, state.menuDpi));
        FillRounded(
            item.hDC,
            highlight,
            Dips(6, state.menuDpi),
            palette.dropdownHover,
            palette,
            SurfaceRole::Dropdown);
    }

    RECT label = item.rcItem;
    label.left += Dips(14, state.menuDpi);
    label.right -= Dips(14, state.menuDpi);
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(
        item.hDC,
        disabled ? palette.disabledText : palette.textPrimary);
    HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    const HGDIOBJ previousFont = SelectObject(item.hDC, font);
    DrawTextW(
        item.hDC,
        menuItem->label,
        -1,
        &label,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(item.hDC, previousFont);
    return true;
}

void AddEditMenuItem(
    HMENU menu,
    UINT position,
    UINT command,
    bool enabled,
    const EditMenuItem& item)
{
    MENUITEMINFOW info {};
    info.cbSize = sizeof(info);
    info.fMask = MIIM_FTYPE | MIIM_ID | MIIM_STATE | MIIM_DATA;
    info.fType = MFT_OWNERDRAW;
    info.wID = command;
    info.fState = enabled ? MFS_ENABLED : (MFS_DISABLED | MFS_GRAYED);
    info.dwItemData = reinterpret_cast<ULONG_PTR>(&item);
    InsertMenuItemW(menu, position, TRUE, &info);
}

void ShowEditContextMenu(HWND edit, ThemedEditState& state, POINT screenPoint)
{
    if (!IsWindowEnabled(edit))
    {
        return;
    }

    state.menuDpi = std::max<UINT>(96, GetDpiForWindow(edit));
    if (screenPoint.x == -1 && screenPoint.y == -1)
    {
        if (!GetCaretPos(&screenPoint))
        {
            screenPoint = { 8, 8 };
        }
        ClientToScreen(edit, &screenPoint);
        screenPoint.y += Dips(18, state.menuDpi);
    }

    DWORD selectionStart = 0;
    DWORD selectionEnd = 0;
    SendMessageW(
        edit,
        EM_GETSEL,
        reinterpret_cast<WPARAM>(&selectionStart),
        reinterpret_cast<LPARAM>(&selectionEnd));
    const bool hasSelection = selectionStart != selectionEnd;
    const bool readOnly =
        (GetWindowLongPtrW(edit, GWL_STYLE) & ES_READONLY) != 0;
    const bool canPaste = !readOnly &&
        (IsClipboardFormatAvailable(CF_UNICODETEXT) ||
         IsClipboardFormatAvailable(CF_TEXT));
    const bool canUndo =
        !readOnly && SendMessageW(edit, EM_CANUNDO, 0, 0) != 0;
    const bool hasText = GetWindowTextLengthW(edit) > 0;

    const std::array<EditMenuItem, 8> items {{
        { L"Undo", false },
        { L"", true },
        { L"Cut", false },
        { L"Copy", false },
        { L"Paste", false },
        { L"Delete", false },
        { L"", true },
        { L"Select all", false }
    }};

    HMENU menu = CreatePopupMenu();
    if (!menu)
    {
        return;
    }
    HBRUSH menuBrush = CreateSolidBrush(state.palette.dropdownBackground);
    MENUINFO menuInfo {};
    menuInfo.cbSize = sizeof(menuInfo);
    menuInfo.fMask = MIM_BACKGROUND | MIM_STYLE;
    menuInfo.hbrBack = menuBrush;
    menuInfo.dwStyle = MNS_NOCHECK;
    SetMenuInfo(menu, &menuInfo);

    AddEditMenuItem(menu, 0, kEditMenuUndo, canUndo, items[0]);
    AddEditMenuItem(menu, 1, 100, false, items[1]);
    AddEditMenuItem(
        menu,
        2,
        kEditMenuCut,
        !readOnly && hasSelection,
        items[2]);
    AddEditMenuItem(menu, 3, kEditMenuCopy, hasSelection, items[3]);
    AddEditMenuItem(menu, 4, kEditMenuPaste, canPaste, items[4]);
    AddEditMenuItem(
        menu,
        5,
        kEditMenuDelete,
        !readOnly && hasSelection,
        items[5]);
    AddEditMenuItem(menu, 6, 101, false, items[6]);
    AddEditMenuItem(menu, 7, kEditMenuSelectAll, hasText, items[7]);

    HWND root = GetAncestor(edit, GA_ROOT);
    if (root)
    {
        SetForegroundWindow(root);
    }
    const UINT command = TrackPopupMenuEx(
        menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
        screenPoint.x,
        screenPoint.y,
        edit,
        nullptr);
    DestroyMenu(menu);
    DeleteObject(menuBrush);
    if (root)
    {
        PostMessageW(root, WM_NULL, 0, 0);
    }

    switch (command)
    {
    case kEditMenuUndo:
        SendMessageW(edit, EM_UNDO, 0, 0);
        break;
    case kEditMenuCut:
        SendMessageW(edit, WM_CUT, 0, 0);
        break;
    case kEditMenuCopy:
        SendMessageW(edit, WM_COPY, 0, 0);
        break;
    case kEditMenuPaste:
        SendMessageW(edit, WM_PASTE, 0, 0);
        break;
    case kEditMenuDelete:
        SendMessageW(edit, WM_CLEAR, 0, 0);
        break;
    case kEditMenuSelectAll:
        SendMessageW(edit, EM_SETSEL, 0, -1);
        break;
    default:
        break;
    }
}

LRESULT CALLBACK ThemedEditWindowProc(
    HWND edit,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    auto* state = static_cast<ThemedEditState*>(
        GetPropW(edit, kEditContextProperty));
    if (!state)
    {
        return DefWindowProcW(edit, message, wParam, lParam);
    }

    if (message == WM_CONTEXTMENU)
    {
        ShowEditContextMenu(
            edit,
            *state,
            { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
        return 0;
    }
    if (message == WM_MEASUREITEM)
    {
        auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (measure && measure->CtlType == ODT_MENU &&
            measure->itemData != 0)
        {
            const auto* item =
                reinterpret_cast<const EditMenuItem*>(measure->itemData);

            measure->itemWidth = Dips(174, state->menuDpi);
            measure->itemHeight =
                Dips(item->separator ? 9 : 34, state->menuDpi);
            return TRUE;
        }
    }
    if (message == WM_DRAWITEM)
    {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (item && PaintEditMenuItem(*state, *item))
        {
            return TRUE;
        }
    }

    const WNDPROC original = state->originalProc;
    if (message == WM_NCDESTROY)
    {
        RemovePropW(edit, kEditContextProperty);
        SetWindowLongPtrW(
            edit,
            GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(original));
        delete state;
    }
    return original
        ? CallWindowProcW(original, edit, message, wParam, lParam)
        : DefWindowProcW(edit, message, wParam, lParam);
}
}

void SetThemedEditContextMenu(HWND edit, const Palette& palette)
{
    if (!edit)
    {
        return;
    }
    if (auto* existing = static_cast<ThemedEditState*>(
            GetPropW(edit, kEditContextProperty)))
    {
        existing->palette = palette;
        return;
    }

    auto* state = new (std::nothrow) ThemedEditState;
    if (!state)
    {
        return;
    }
    state->palette = palette;
    if (!SetPropW(edit, kEditContextProperty, state))
    {
        delete state;
        return;
    }
    state->originalProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(
            edit,
            GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(ThemedEditWindowProc)));
    if (!state->originalProc)
    {
        RemovePropW(edit, kEditContextProperty);
        delete state;
    }
}

COLORREF BlendColor(COLORREF base, COLORREF tint, int tintPercent)
{
    const int amount = std::clamp(tintPercent, 0, 100);
    const auto channel = [amount](BYTE from, BYTE to)
    {
        return static_cast<BYTE>((static_cast<int>(from) * (100 - amount) +
            static_cast<int>(to) * amount + 50) / 100);
    };
    return RGB(
        channel(GetRValue(base), GetRValue(tint)),
        channel(GetGValue(base), GetGValue(tint)),
        channel(GetBValue(base), GetBValue(tint)));
}

void PaintButton(
    HDC hdc,
    const RECT& bounds,
    std::wstring_view label,
    HFONT font,
    UINT dpi,
    const Palette& palette,
    const ControlState& state,
    const ButtonOptions& options)
{
    if (!HasArea(bounds))
    {
        return;
    }

    const bool danger = options.role == ButtonRole::Danger;
    const bool primary = options.role == ButtonRole::Primary || danger;
    const COLORREF actionBase = danger ? palette.danger : palette.accentSoft;
    const COLORREF actionAccent = danger ? palette.dangerAccent : palette.accent;
    COLORREF background = palette.buttonBackground;
    if (!state.enabled)
    {
        background = palette.disabledBackground;
    }
    else if (primary)
    {
        background = state.pressed
            ? BlendColor(actionBase, palette.pageBackground, 24)
            : state.hovered
                ? BlendColor(actionBase, actionAccent, 44)
                : actionBase;
    }
    else if (state.active)
    {
        background = state.pressed
            ? BlendColor(palette.accentSoft, palette.pageBackground, 22)
            : state.hovered
                ? BlendColor(palette.accentSoft, palette.accent, 36)
                : palette.accentSoft;
    }
    else if (state.pressed)
    {
        background = palette.buttonPressed;
    }
    else if (state.hovered)
    {
        background = palette.buttonHover;
    }

    const COLORREF border = !state.enabled
        ? BlendColor(palette.border, palette.disabledBackground, 28)
        : primary
            ? (state.pressed
                ? BlendColor(actionAccent, palette.pageBackground, 20)
                : state.hovered
                    ? actionAccent
                    : BlendColor(palette.border, actionAccent, 68))
            : state.pressed
                ? BlendColor(palette.border, palette.accent, 58)
                : state.hovered || state.active
                    ? BlendColor(palette.border, palette.accent, 44)
                    : palette.border;

    RECT paintBounds = bounds;
    if (state.enabled && state.pressed && options.insetWhenPressed &&
        bounds.right - bounds.left > Dips(4, dpi) &&
        bounds.bottom - bounds.top > Dips(4, dpi))
    {
        InflateRect(&paintBounds, -Dips(1, dpi), -Dips(1, dpi));
    }

    const int cornerRadius = Dips(options.cornerRadiusDip, dpi);
    FillRounded(hdc, paintBounds, cornerRadius, background, palette);
    StrokeRounded(hdc, paintBounds, cornerRadius, border, palette);

    RECT textBounds = paintBounds;
    if (textBounds.right - textBounds.left > Dips(24, dpi))
    {
        InflateRect(&textBounds, -Dips(options.horizontalPaddingDip, dpi), 0);
    }
    DrawTextLine(
        hdc,
        label,
        textBounds,
        font,
        state.enabled ? palette.textPrimary : palette.disabledText,
        DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

SliderGeometry CalculateSliderGeometry(
    const RECT& track,
    const RECT& thumb,
    UINT dpi)
{
    SliderGeometry geometry;
    if (!HasArea(track) || !HasArea(thumb))
    {
        return geometry;
    }

    const int centerY = (track.top + track.bottom) / 2;
    const int trackHeight = std::max(
        Dips(4, dpi),
        static_cast<int>(track.bottom - track.top) / 2);
    geometry.track = {
        track.left,
        centerY - trackHeight / 2,
        track.right,
        centerY + (trackHeight + 1) / 2
    };
    geometry.thumbCenter = {
        (thumb.left + thumb.right) / 2,
        (thumb.top + thumb.bottom) / 2
    };
    geometry.activeTrack = geometry.track;
    geometry.activeTrack.right = std::clamp(
        geometry.thumbCenter.x,
        geometry.track.left,
        geometry.track.right);
    geometry.outerRadius = Dips(10, dpi);
    geometry.innerRadius = Dips(6, dpi);
    return geometry;
}

RECT SliderThumbRectForValue(
    const RECT& track,
    double normalizedValue,
    UINT dpi,
    int radiusDip)
{
    const double amount = std::clamp(normalizedValue, 0.0, 1.0);
    const int centerX = track.left + static_cast<int>(std::lround(
        static_cast<double>(track.right - track.left) * amount));
    const int centerY = (track.top + track.bottom) / 2;
    const int radius = std::max(1, Dips(radiusDip, dpi));
    return {
        centerX - radius,
        centerY - radius,
        centerX + radius,
        centerY + radius
    };
}

double SliderValueFromPoint(
    const RECT& track,
    int x,
    double minimum,
    double maximum)
{
    if (track.right <= track.left || maximum <= minimum)
    {
        return minimum;
    }
    const double amount = std::clamp(
        static_cast<double>(x - track.left) /
            static_cast<double>(track.right - track.left),
        0.0,
        1.0);
    return minimum + (maximum - minimum) * amount;
}

void PaintSlider(
    HDC hdc,
    const RECT& track,
    const RECT& thumb,
    UINT dpi,
    const Palette& palette,
    bool enabled)
{
    const SliderGeometry geometry = CalculateSliderGeometry(track, thumb, dpi);
    if (!HasArea(geometry.track))
    {
        return;
    }

    const int radius = std::max(
        1,
        static_cast<int>(geometry.track.bottom - geometry.track.top));
    FillRounded(
        hdc,
        geometry.track,
        radius,
        enabled ? palette.border : palette.disabledBackground,
        palette,
        SurfaceRole::None);
    if (geometry.activeTrack.right > geometry.activeTrack.left)
    {
        FillRounded(
            hdc,
            geometry.activeTrack,
            radius,
            enabled ? palette.accent : palette.disabledText,
            palette,
            SurfaceRole::None);
    }

    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const COLORREF thumbColor = enabled ? palette.accent : palette.disabledText;
    Gdiplus::SolidBrush halo(AlphaColor(thumbColor, enabled ? 64 : 28));
    graphics.FillEllipse(
        &halo,
        geometry.thumbCenter.x - geometry.outerRadius,
        geometry.thumbCenter.y - geometry.outerRadius,
        geometry.outerRadius * 2,
        geometry.outerRadius * 2);
    Gdiplus::SolidBrush face(AlphaColor(
        enabled ? palette.textPrimary : palette.disabledText));
    graphics.FillEllipse(
        &face,
        geometry.thumbCenter.x - geometry.innerRadius,
        geometry.thumbCenter.y - geometry.innerRadius,
        geometry.innerRadius * 2,
        geometry.innerRadius * 2);
    Gdiplus::Pen ring(
        AlphaColor(thumbColor),
        static_cast<Gdiplus::REAL>(std::max(1, Dips(2, dpi))));
    graphics.DrawEllipse(
        &ring,
        geometry.thumbCenter.x - geometry.innerRadius,
        geometry.thumbCenter.y - geometry.innerRadius,
        geometry.innerRadius * 2,
        geometry.innerRadius * 2);
}

SwitchAnimationState MakeSwitchAnimationState(bool enabled)
{
    SwitchAnimationState state;
    state.position = enabled ? 1.0f : 0.0f;
    state.target = state.position;
    state.initialized = true;
    return state;
}

bool SetSwitchTarget(
    SwitchAnimationState& state,
    bool enabled,
    const RECT& bounds)
{
    const float target = enabled ? 1.0f : 0.0f;
    bool started = false;
    if (!state.initialized)
    {
        state.position = target;
        state.target = target;
        state.initialized = true;
    }
    else if (std::fabs(state.target - target) > 0.001f)
    {
        state.target = target;
        state.animating = true;
        started = true;
    }
    state.bounds = bounds;
    return started;
}

bool StepSwitchAnimation(
    SwitchAnimationState& state,
    float easing,
    float completionThreshold)
{
    if (!state.initialized || !state.animating)
    {
        return false;
    }

    const float safeEasing = std::clamp(easing, 0.01f, 1.0f);
    const float safeThreshold = std::max(0.0001f, completionThreshold);
    const float delta = state.target - state.position;
    if (std::fabs(delta) <= safeThreshold)
    {
        state.position = state.target;
        state.animating = false;
        return false;
    }

    state.position += delta * safeEasing;
    if (std::fabs(state.target - state.position) <= safeThreshold)
    {
        state.position = state.target;
        state.animating = false;
    }
    return state.animating;
}

void PaintSwitch(
    HDC hdc,
    const RECT& bounds,
    float position,
    UINT dpi,
    const Palette& palette,
    bool hovered,
    bool pressed)
{
    if (!HasArea(bounds))
    {
        return;
    }

    const int width = static_cast<int>(bounds.right - bounds.left);
    const int height = static_cast<int>(bounds.bottom - bounds.top);
    const float clampedPosition = std::clamp(position, 0.0f, 1.0f);
    const int activePercent = static_cast<int>(std::lround(clampedPosition * 100.0f));
    const COLORREF offTrack = BlendColor(
        palette.buttonBackground,
        palette.inputBackground,
        24);
    const COLORREF onTrack = BlendColor(palette.accentSoft, palette.accent, 64);
    COLORREF trackColor = BlendColor(offTrack, onTrack, activePercent);
    if (hovered)
    {
        trackColor = BlendColor(trackColor, palette.accent, 10);
    }
    if (pressed)
    {
        trackColor = BlendColor(trackColor, palette.pageBackground, 9);
    }

    const COLORREF trackTop = BlendColor(
        trackColor,
        RGB(255, 255, 255),
        palette.light ? 5 : 10);
    const COLORREF trackBottom = BlendColor(
        trackColor,
        palette.pageBackground,
        palette.light ? 3 : 12);
    const COLORREF trackBorder = BlendColor(
        palette.border,
        palette.accent,
        std::clamp(activePercent * 3 / 5 + (hovered ? 20 : 0), 0, 82));

    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

    Gdiplus::GraphicsPath trackPath;
    BuildRoundedPath(trackPath, bounds, height / 2);
    if (hovered || clampedPosition > 0.01f)
    {
        const BYTE glowAlpha = static_cast<BYTE>(std::clamp(
            static_cast<int>(34.0f + clampedPosition * 42.0f +
                (hovered ? 28.0f : 0.0f)),
            0,
            112));
        Gdiplus::Pen glowPen(
            AlphaColor(palette.accent, glowAlpha),
            static_cast<Gdiplus::REAL>(std::max(1, Dips(2, dpi))));
        graphics.DrawPath(&glowPen, &trackPath);
    }

    Gdiplus::RectF trackRect(
        static_cast<Gdiplus::REAL>(bounds.left),
        static_cast<Gdiplus::REAL>(bounds.top),
        static_cast<Gdiplus::REAL>(width),
        static_cast<Gdiplus::REAL>(height));
    Gdiplus::LinearGradientBrush trackBrush(
        trackRect,
        AlphaColor(trackTop),
        AlphaColor(trackBottom),
        Gdiplus::LinearGradientModeVertical);
    graphics.FillPath(&trackBrush, &trackPath);
    Gdiplus::Pen borderPen(
        AlphaColor(trackBorder),
        static_cast<Gdiplus::REAL>(std::max(1, Dips(1, dpi))));
    graphics.DrawPath(&borderPen, &trackPath);

    const Gdiplus::REAL inset = static_cast<Gdiplus::REAL>(Dips(3, dpi));
    const Gdiplus::REAL baseThumbSize = std::max<Gdiplus::REAL>(
        1.0f,
        static_cast<Gdiplus::REAL>(height) - inset * 2.0f);
    const Gdiplus::REAL pressInset = pressed
        ? static_cast<Gdiplus::REAL>(Dips(1, dpi))
        : 0.0f;
    const Gdiplus::REAL travel = std::max<Gdiplus::REAL>(
        0.0f,
        static_cast<Gdiplus::REAL>(width) - inset * 2.0f - baseThumbSize);
    const Gdiplus::REAL thumbSize = std::max<Gdiplus::REAL>(
        1.0f,
        baseThumbSize - pressInset * 2.0f);
    const Gdiplus::REAL thumbLeft =
        static_cast<Gdiplus::REAL>(bounds.left) +
        inset + travel * clampedPosition + pressInset;
    const Gdiplus::REAL thumbTop =
        static_cast<Gdiplus::REAL>(bounds.top) + inset + pressInset;

    Gdiplus::RectF shadowRect(
        thumbLeft,
        thumbTop + static_cast<Gdiplus::REAL>(Dips(1, dpi)),
        thumbSize,
        thumbSize);
    Gdiplus::SolidBrush shadowBrush(Gdiplus::Color(62, 0, 0, 0));
    graphics.FillEllipse(&shadowBrush, shadowRect);

    Gdiplus::RectF thumbRect(thumbLeft, thumbTop, thumbSize, thumbSize);
    const COLORREF thumbBottom = palette.light
        ? RGB(235, 239, 246)
        : RGB(226, 232, 242);
    Gdiplus::LinearGradientBrush thumbBrush(
        thumbRect,
        AlphaColor(RGB(255, 255, 255)),
        AlphaColor(thumbBottom),
        Gdiplus::LinearGradientModeVertical);
    graphics.FillEllipse(&thumbBrush, thumbRect);
    Gdiplus::Pen thumbBorder(
        AlphaColor(palette.border, 72),
        static_cast<Gdiplus::REAL>(std::max(1, Dips(1, dpi))));
    graphics.DrawEllipse(&thumbBorder, thumbRect);
}

void PaintDropdownChevron(
    HDC hdc,
    const RECT& bounds,
    bool expanded,
    COLORREF color,
    UINT dpi)
{
    const Gdiplus::REAL centerX =
        static_cast<Gdiplus::REAL>(bounds.left + bounds.right) * 0.5f;
    const Gdiplus::REAL centerY =
        static_cast<Gdiplus::REAL>(bounds.top + bounds.bottom) * 0.5f;
    const Gdiplus::REAL halfWidth = static_cast<Gdiplus::REAL>(Dips(5, dpi));
    const Gdiplus::REAL halfHeight = static_cast<Gdiplus::REAL>(Dips(3, dpi));
    Gdiplus::PointF points[3] {
        { centerX - halfWidth, centerY + (expanded ? halfHeight : -halfHeight) },
        { centerX, centerY + (expanded ? -halfHeight : halfHeight) },
        { centerX + halfWidth, centerY + (expanded ? halfHeight : -halfHeight) }
    };

    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    Gdiplus::Pen pen(
        AlphaColor(color),
        static_cast<Gdiplus::REAL>(std::max(1, Dips(2, dpi))));
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    graphics.DrawLines(&pen, points, static_cast<INT>(std::size(points)));
}

void PaintDropdownField(
    HDC hdc,
    const RECT& bounds,
    std::wstring_view label,
    HFONT font,
    UINT dpi,
    const Palette& palette,
    const ControlState& state,
    bool expanded,
    bool opensDown)
{
    if (!HasArea(bounds))
    {
        return;
    }

    const COLORREF background = !state.enabled
        ? palette.disabledBackground
        : expanded
            ? palette.dropdownSelected
            : state.pressed
                ? palette.buttonPressed
                : state.hovered
                    ? palette.dropdownHover
                    : palette.inputBackground;
    const COLORREF border = !state.enabled
        ? palette.border
        : expanded
            ? BlendColor(palette.border, palette.accent, 52)
            : state.pressed
                ? BlendColor(palette.border, palette.accent, 42)
                : state.hovered
                    ? BlendColor(palette.border, palette.accent, 36)
                    : palette.border;

    RECT paintBounds = bounds;
    if (state.enabled && state.pressed)
    {
        OffsetRect(&paintBounds, 0, Dips(1, dpi));
    }
    FillRounded(hdc, paintBounds, Dips(10, dpi), background, palette);
    StrokeRounded(hdc, paintBounds, Dips(10, dpi), border, palette);

    RECT labelRect {
        paintBounds.left + Dips(16, dpi),
        paintBounds.top,
        paintBounds.right - Dips(43, dpi),
        paintBounds.bottom
    };
    DrawTextLine(
        hdc,
        label,
        labelRect,
        font,
        state.enabled ? palette.textPrimary : palette.disabledText,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    if (!state.enabled)
    {
        return;
    }

    RECT chevronRect {
        paintBounds.right - Dips(32, dpi),
        paintBounds.top,
        paintBounds.right - Dips(10, dpi),
        paintBounds.bottom
    };
    PaintDropdownChevron(
        hdc,
        chevronRect,
        expanded && opensDown,
        expanded || state.hovered || state.pressed
            ? palette.textPrimary
            : palette.textSecondary,
        dpi);
}

void PaintDropdownMenuBackground(
    HDC hdc,
    const RECT& bounds,
    UINT dpi,
    const Palette& palette)
{
    if (!HasArea(bounds))
    {
        return;
    }

    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

    RECT outerShadow = bounds;
    InflateRect(&outerShadow, Dips(5, dpi), Dips(4, dpi));
    OffsetRect(&outerShadow, 0, Dips(5, dpi));
    Gdiplus::GraphicsPath outerPath;
    BuildRoundedPath(outerPath, outerShadow, Dips(15, dpi));
    Gdiplus::SolidBrush outerBrush(Gdiplus::Color(18, 0, 0, 0));
    graphics.FillPath(&outerBrush, &outerPath);

    RECT innerShadow = bounds;
    InflateRect(&innerShadow, Dips(2, dpi), Dips(2, dpi));
    OffsetRect(&innerShadow, 0, Dips(3, dpi));
    Gdiplus::GraphicsPath innerPath;
    BuildRoundedPath(innerPath, innerShadow, Dips(13, dpi));
    Gdiplus::SolidBrush innerBrush(Gdiplus::Color(42, 0, 0, 0));
    graphics.FillPath(&innerBrush, &innerPath);

    FillRounded(
        hdc,
        bounds,
        Dips(12, dpi),
        palette.dropdownBackground,
        palette,
        SurfaceRole::None);
    StrokeRounded(
        hdc,
        bounds,
        Dips(12, dpi),
        BlendColor(palette.border, palette.accent, 18),
        palette);
}

void PaintDropdownItem(
    HDC hdc,
    const RECT& bounds,
    std::wstring_view label,
    HFONT font,
    HFONT badgeFont,
    UINT dpi,
    const Palette& palette,
    const DropdownItemOptions& options)
{
    if (!HasArea(bounds))
    {
        return;
    }

    if (options.selected || options.hovered)
    {
        const COLORREF background = options.selected && options.hovered
            ? BlendColor(palette.dropdownSelected, palette.accent, 16)
            : options.selected
                ? palette.dropdownSelected
                : palette.dropdownHover;
        FillRounded(hdc, bounds, Dips(8, dpi), background, palette);
        StrokeRounded(
            hdc,
            bounds,
            Dips(8, dpi),
            options.selected
                ? BlendColor(palette.border, palette.accent, 38)
                : BlendColor(palette.border, palette.accent, 20),
            palette);
    }

    RECT markerColumn {
        bounds.left + Dips(9, dpi),
        bounds.top,
        bounds.left + Dips(31, dpi),
        bounds.bottom
    };
    if (options.selectionStyle == DropdownSelectionStyle::Checkbox)
    {
        RECT box {
            markerColumn.left + Dips(4, dpi),
            markerColumn.top +
                ((markerColumn.bottom - markerColumn.top) - Dips(16, dpi)) / 2,
            markerColumn.left + Dips(20, dpi),
            markerColumn.top +
                ((markerColumn.bottom - markerColumn.top) + Dips(16, dpi)) / 2
        };
        FillRounded(
            hdc,
            box,
            Dips(4, dpi),
            options.selected ? palette.accent : palette.inputBackground,
            palette);
        StrokeRounded(
            hdc,
            box,
            Dips(4, dpi),
            options.selected ? palette.accent : palette.border,
            palette);
        if (options.selected)
        {
            PaintCheckmark(hdc, box, RGB(255, 255, 255), dpi);
        }
    }
    else if (options.selected)
    {
        const int markerSize = Dips(19, dpi);
        const int markerTop = markerColumn.top +
            ((markerColumn.bottom - markerColumn.top) - markerSize) / 2;
        RECT markerRect {
            markerColumn.left + Dips(1, dpi),
            markerTop,
            markerColumn.left + Dips(1, dpi) + markerSize,
            markerTop + markerSize
        };
        Gdiplus::Graphics graphics(hdc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::SolidBrush indicator(AlphaColor(palette.accent, 34));
        graphics.FillEllipse(
            &indicator,
            static_cast<INT>(markerRect.left),
            static_cast<INT>(markerRect.top),
            static_cast<INT>(markerRect.right - markerRect.left),
            static_cast<INT>(markerRect.bottom - markerRect.top));
        PaintCheckmark(hdc, markerRect, palette.accent, dpi);
    }

    RECT textRect = bounds;
    textRect.left += Dips(38, dpi);
    textRect.right -= Dips(13, dpi);
    RECT badgeRect {};
    if (!options.badge.empty())
    {
        badgeRect = {
            bounds.right - Dips(126, dpi),
            bounds.top + Dips(6, dpi),
            bounds.right - Dips(10, dpi),
            bounds.bottom - Dips(6, dpi)
        };
        textRect.right = badgeRect.left - Dips(10, dpi);
    }

    const COLORREF textColor = options.enabled
        ? (options.selected || options.hovered
            ? palette.textPrimary
            : palette.textSecondary)
        : palette.disabledText;
    DrawTextLine(
        hdc,
        label,
        textRect,
        font,
        textColor,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    if (!options.badge.empty())
    {
        FillRounded(
            hdc,
            badgeRect,
            Dips(10, dpi),
            options.enabled ? palette.accentSoft : palette.disabledBackground,
            palette);
        StrokeRounded(
            hdc,
            badgeRect,
            Dips(10, dpi),
            options.enabled ? palette.accent : palette.border,
            palette);
        DrawTextLine(
            hdc,
            options.badge,
            badgeRect,
            badgeFont ? badgeFont : font,
            options.enabled ? palette.textPrimary : palette.disabledText,
            DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }
}
}
