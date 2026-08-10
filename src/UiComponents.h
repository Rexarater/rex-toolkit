#pragma once

#include <windows.h>

#include <string_view>

namespace rex::ui
{
// Reusable, stateless painters for Rex's Toolkit controls. Callers retain
// hit-testing and business state; only animation progress and reusable control
// geometry live here so the same components work in parent and child windows.

enum class SurfaceStyle
{
    Solid,
    Translucent,
    Glass
};

struct Palette
{
    COLORREF pageBackground = RGB(18, 20, 24);
    COLORREF inputBackground = RGB(25, 29, 36);
    COLORREF buttonBackground = RGB(37, 42, 51);
    COLORREF buttonHover = RGB(48, 56, 69);
    COLORREF buttonPressed = RGB(46, 58, 74);
    COLORREF disabledBackground = RGB(34, 38, 46);
    COLORREF disabledText = RGB(112, 120, 132);
    COLORREF dropdownBackground = RGB(29, 34, 43);
    COLORREF dropdownHover = RGB(43, 50, 62);
    COLORREF dropdownSelected = RGB(39, 54, 78);
    COLORREF border = RGB(48, 54, 65);
    COLORREF textPrimary = RGB(245, 247, 250);
    COLORREF textSecondary = RGB(166, 174, 186);
    COLORREF accent = RGB(83, 147, 245);
    COLORREF accentSoft = RGB(44, 86, 153);
    COLORREF danger = RGB(178, 72, 72);
    COLORREF dangerAccent = RGB(220, 96, 96);
    COLORREF frostHighlight = RGB(255, 255, 255);
    SurfaceStyle surfaceStyle = SurfaceStyle::Solid;
    int surfaceOpacity = 100;
    bool light = false;
};

struct ControlState
{
    bool hovered = false;
    bool pressed = false;
    bool active = false;
    bool enabled = true;
};

enum class ButtonRole
{
    Neutral,
    Primary,
    Danger
};

struct ButtonOptions
{
    ButtonRole role = ButtonRole::Neutral;
    int cornerRadiusDip = 10;
    int horizontalPaddingDip = 8;
    bool insetWhenPressed = true;
};

void PaintButton(
    HDC hdc,
    const RECT& bounds,
    std::wstring_view label,
    HFONT font,
    UINT dpi,
    const Palette& palette,
    const ControlState& state,
    const ButtonOptions& options = {});

struct SliderGeometry
{
    RECT track {};
    RECT activeTrack {};
    POINT thumbCenter {};
    int outerRadius = 0;
    int innerRadius = 0;
};

SliderGeometry CalculateSliderGeometry(
    const RECT& track,
    const RECT& thumb,
    UINT dpi);

RECT SliderThumbRectForValue(
    const RECT& track,
    double normalizedValue,
    UINT dpi,
    int radiusDip = 9);

double SliderValueFromPoint(
    const RECT& track,
    int x,
    double minimum,
    double maximum);

void PaintSlider(
    HDC hdc,
    const RECT& track,
    const RECT& thumb,
    UINT dpi,
    const Palette& palette,
    bool enabled = true);

struct SwitchAnimationState
{
    float position = 0.0f;
    float target = 0.0f;
    RECT bounds {};
    bool initialized = false;
    bool animating = false;
};

SwitchAnimationState MakeSwitchAnimationState(bool enabled);

// Returns true only when a new transition starts.
bool SetSwitchTarget(
    SwitchAnimationState& state,
    bool enabled,
    const RECT& bounds);

// Advances one frame and returns whether another frame is required.
bool StepSwitchAnimation(
    SwitchAnimationState& state,
    float easing = 0.42f,
    float completionThreshold = 0.012f);

void PaintSwitch(
    HDC hdc,
    const RECT& bounds,
    float position,
    UINT dpi,
    const Palette& palette,
    bool hovered = false,
    bool pressed = false);

void PaintDropdownChevron(
    HDC hdc,
    const RECT& bounds,
    bool expanded,
    COLORREF color,
    UINT dpi);

void PaintDropdownField(
    HDC hdc,
    const RECT& bounds,
    std::wstring_view label,
    HFONT font,
    UINT dpi,
    const Palette& palette,
    const ControlState& state,
    bool expanded,
    bool opensDown = true);

enum class DropdownSelectionStyle
{
    Checkmark,
    Checkbox
};

struct DropdownItemOptions
{
    bool enabled = true;
    bool selected = false;
    bool hovered = false;
    DropdownSelectionStyle selectionStyle = DropdownSelectionStyle::Checkmark;
    std::wstring_view badge;
};

void PaintDropdownMenuBackground(
    HDC hdc,
    const RECT& bounds,
    UINT dpi,
    const Palette& palette);

void PaintDropdownItem(
    HDC hdc,
    const RECT& bounds,
    std::wstring_view label,
    HFONT font,
    HFONT badgeFont,
    UINT dpi,
    const Palette& palette,
    const DropdownItemOptions& options = {});

COLORREF BlendColor(COLORREF base, COLORREF tint, int tintPercent);
}
