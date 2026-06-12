# Rex's Toolkit

Rex's Toolkit is a native Windows C++ desktop app for a future all-in-one utility toolkit.

The current version provides:

- A standalone Windows desktop window titled `Rex's Toolkit`
- A dark, modern layout
- Top navigation between `Favorites` and `All Tools`
- A 3-wide icon card grid for tools
- Favorite stars for adding or removing tools from Favorites
- A first tool: `Auto Clicker`
- A real image-focused `File Converter`
- A media-focused `YouTube & SoundCloud Downloader`
- A Settings page with manual update checks
- Empty states where no favorites or tools are available
- A clear tool registration section for future expansion

Favorites are saved between launches in `%APPDATA%\RexToolkit\favorites.txt`.

## Current Tools

### Auto Clicker

The Auto Clicker includes:

- Speed slider, 1-100 clicks/sec
- Hold-to-test button
- Bindable activation input, including keyboard keys and side mouse buttons
- Bindable output mouse button
- Running/stopped status text

Hold the activation input to spam the selected output button. Release it to stop.

### File Converter

The File Converter currently focuses on images. It supports queueing multiple files, drag-and-drop, file browsing, output format selection, filename conflict handling, quality settings for JPG/WEBP, progress feedback, and per-file errors.

When you click `Convert`, the app asks where to save the result. Single-file conversions use a Windows `Save As` dialog, while multi-file conversions ask for a destination folder.

Image conversion is implemented through Windows Imaging Component. PNG, JPG/JPEG, and BMP are supported on standard Windows installs. WEBP decode/encode support depends on the installed Windows WIC codecs; the app detects available encoders and does not fake unsupported output formats.

### YouTube & SoundCloud Downloader

The Media Downloader supports authorized YouTube and SoundCloud links and can save media as MP4, MP3, or WAV. It uses external tools rather than custom download logic:

- `yt-dlp` for metadata and media retrieval
- `FFmpeg` for merging, audio extraction, and conversion

`yt-dlp.exe` and `ffmpeg.exe` are bundled with the app and copied to the build output under the `tools` folder. To update either helper later, replace the matching file in `tools`.

The app first looks for bundled media helpers beside the app, then falls back to tools available on `PATH`. If a dependency is missing, it shows a setup message instead of crashing.

The downloader remembers its last selected format, quality, and output folder in `%APPDATA%\RexToolkit\media_downloader.txt`.

## Manual Update Checks

Rex's Toolkit checks this file when you click `Check for Updates` on the Settings page:

```text
https://raw.githubusercontent.com/Rexarater/rex-toolkit/main/latest.json
```

The app compares its local `APP_VERSION` value with the `latestVersion` field in that JSON file. If a newer version exists, it shows the release notes and downloads the update package when you click `Download Update`.

After the package downloads, Rex's Toolkit closes, copies the updated files into the current app folder, and relaunches. If the app is installed in a protected folder, Windows permissions may prevent the copy from completing.

To publish a new update:

1. Build a new version of the app.
2. Change `APP_VERSION` in `src/AppConstants.h`.
3. Zip the app.
4. Create a new GitHub Release.
5. Upload the zip.
6. Update `latest.json` with the new `latestVersion`, `downloadUrl`, and release notes.

## Build Requirements

- Windows 10 or newer
- CMake 3.20+
- A C++17 compiler, such as Visual Studio 2026 or Visual Studio Build Tools

## Build

From a Developer PowerShell or Developer Command Prompt:

```powershell
cmake --preset windows-release
cmake --build --preset windows-release
```

## Run

```powershell
.\build\windows-release\RexToolkit.exe
```

## Visual Studio

Open this folder in Visual Studio, then select the `windows-debug` or `windows-release` CMake preset.

If Visual Studio previously configured the project with the wrong generator, delete the old generated `build` folder and reconfigure with one of the presets above.

## Adding Tools Later

Future tools are registered in `src/ToolkitApp.cpp` inside `CreateToolRegistry()`.

Add new `ToolDefinition` entries there, then wire each tool to its own view or command handler as the toolkit grows.
