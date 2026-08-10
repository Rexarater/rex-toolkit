#include "UiComponents.h"

#include <cmath>
#include <iostream>

namespace
{
bool Require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
    }
    return condition;
}
}

int main()
{
    bool ok = true;

    rex::ui::SwitchAnimationState animation =
        rex::ui::MakeSwitchAnimationState(false);
    const RECT switchBounds { 10, 20, 70, 52 };
    ok &= Require(
        rex::ui::SetSwitchTarget(animation, true, switchBounds),
        "Enabling a switch should start an animation.");
    ok &= Require(animation.animating, "Switch should report an active animation.");

    float previous = animation.position;
    int frames = 0;
    while (rex::ui::StepSwitchAnimation(animation) && frames < 100)
    {
        ok &= Require(
            animation.position >= previous,
            "Switch position should move monotonically toward on.");
        previous = animation.position;
        ++frames;
    }
    ok &= Require(frames < 100, "Switch animation should settle promptly.");
    ok &= Require(
        std::fabs(animation.position - 1.0f) < 0.0001f,
        "Switch animation should finish exactly at its target.");
    ok &= Require(!animation.animating, "Settled switch should stop animating.");
    ok &= Require(
        !rex::ui::SetSwitchTarget(animation, true, switchBounds),
        "Setting the current switch value should not restart animation.");

    ok &= Require(
        rex::ui::SetSwitchTarget(animation, false, switchBounds),
        "Disabling a switch should start the reverse animation.");
    while (rex::ui::StepSwitchAnimation(animation))
    {
    }
    ok &= Require(
        std::fabs(animation.position) < 0.0001f,
        "Reverse animation should finish exactly at zero.");

    const RECT track { 10, 20, 110, 30 };
    const RECT thumb = rex::ui::SliderThumbRectForValue(track, 0.25, 96);
    ok &= Require(
        (thumb.left + thumb.right) / 2 == 35,
        "Slider thumb should be placed from the normalized value.");
    ok &= Require(
        (thumb.top + thumb.bottom) / 2 == 25,
        "Slider thumb should be vertically centered on its track.");

    const rex::ui::SliderGeometry geometry =
        rex::ui::CalculateSliderGeometry(track, thumb, 96);
    ok &= Require(
        geometry.activeTrack.right == 35,
        "Slider active track should end at the thumb center.");
    ok &= Require(
        std::fabs(rex::ui::SliderValueFromPoint(track, 60, 0.0, 100.0) - 50.0) < 0.001,
        "Slider point conversion should preserve the requested range.");
    ok &= Require(
        rex::ui::SliderValueFromPoint(track, -50, 10.0, 20.0) == 10.0,
        "Slider values should clamp below the track.");
    ok &= Require(
        rex::ui::SliderValueFromPoint(track, 500, 10.0, 20.0) == 20.0,
        "Slider values should clamp above the track.");

    const COLORREF midpoint = rex::ui::BlendColor(
        RGB(0, 0, 0),
        RGB(255, 255, 255),
        50);
    ok &= Require(
        GetRValue(midpoint) == 128 &&
        GetGValue(midpoint) == 128 &&
        GetBValue(midpoint) == 128,
        "Color blending should round channel midpoints consistently.");

    return ok ? 0 : 1;
}
