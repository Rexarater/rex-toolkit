#pragma once

#include "MediaEditorService.h"

#include <windows.h>

#include <filesystem>
#include <memory>
#include <vector>

struct MediaEditorTheme
{
    COLORREF pageBackground = RGB(17, 20, 25);
    COLORREF panelBackground = RGB(31, 35, 43);
    COLORREF panelHover = RGB(39, 45, 56);
    COLORREF inputBackground = RGB(24, 28, 35);
    COLORREF buttonBackground = RGB(37, 43, 53);
    COLORREF border = RGB(50, 59, 72);
    COLORREF textPrimary = RGB(238, 242, 248);
    COLORREF textSecondary = RGB(163, 176, 194);
    COLORREF accent = RGB(78, 143, 245);
    COLORREF accentSoft = RGB(43, 85, 145);
    COLORREF warning = RGB(240, 185, 73);
    COLORREF danger = RGB(226, 92, 103);
};

class MediaEditorPage
{
public:
    MediaEditorPage();
    ~MediaEditorPage();

    MediaEditorPage(const MediaEditorPage&) = delete;
    MediaEditorPage& operator=(const MediaEditorPage&) = delete;

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

    HWND WindowHandle() const;
    bool IsBusy() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
