#pragma once

#include "EqualizerService.h"
#include "UiComponents.h"

#include <windows.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

struct EqualizerTheme
{
    COLORREF pageBackground = RGB(18, 20, 24);
    COLORREF panelBackground = RGB(31, 35, 43);
    COLORREF panelHover = RGB(42, 48, 59);
    COLORREF inputBackground = RGB(24, 28, 35);
    COLORREF buttonBackground = RGB(37, 42, 51);
    COLORREF border = RGB(49, 56, 68);
    COLORREF textPrimary = RGB(245, 247, 250);
    COLORREF textSecondary = RGB(166, 174, 186);
    COLORREF accent = RGB(83, 147, 245);
    COLORREF accentSoft = RGB(44, 86, 153);
    COLORREF warning = RGB(231, 174, 62);
    COLORREF danger = RGB(224, 88, 102);
    rex::ui::SurfaceStyle surfaceStyle = rex::ui::SurfaceStyle::Solid;
    int surfaceOpacity = 100;
    bool light = false;
    bool smoothScrollingEnabled = true;
};

class EqualizerPage
{
public:
    EqualizerPage();
    ~EqualizerPage();

    EqualizerPage(const EqualizerPage&) = delete;
    EqualizerPage& operator=(const EqualizerPage&) = delete;

    bool Create(HINSTANCE instance, HWND parent);
    void Destroy();
    void Shutdown();
    void SetVisible(bool visible);
    void SetBounds(const RECT& bounds, UINT dpi);
    void SetTheme(const EqualizerTheme& theme);
    void SetBetaFeaturesEnabled(bool enabled);
    void SetBackgroundPainter(std::function<void(HDC, const RECT&)> painter);
    HWND WindowHandle() const;

    bool IsInitialized() const;
    bool IsEnabled() const;
    bool TrayControlsEnabled() const;
    bool RememberPerDevice() const;
    bool AutomaticallyApplyDeviceProfile() const;
    bool EnableOnStartup() const;
    bool PreventClipping() const;
    bool ShowTechnicalControls() const;
    std::wstring CurrentPresetId() const;
    std::wstring CurrentOutputName() const;
    std::wstring CurrentHeadphoneName() const;
    std::wstring HeadphoneDatabaseVersion() const;

    void ToggleEnabled();
    void SelectPreset(const std::wstring& presetId);
    void SetRememberPerDevice(bool enabled);
    void SetAutomaticallyApplyDeviceProfile(bool enabled);
    void SetEnableOnStartup(bool enabled);
    void SetPreventClipping(bool enabled);
    void SetTrayControlsEnabled(bool enabled);
    void SetShowTechnicalControls(bool enabled);
    void OpenAdvanced(bool diagnostics = false);
    void BeginBackendSetup();
    void BeginProfileUpdate();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
