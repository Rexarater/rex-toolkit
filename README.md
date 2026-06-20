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
- An AniList-powered `Anime Tracker`
- A local `Reminders` tool
- A `Smart File Transfer` tool with LAN, Direct Host, and manual WebRTC P2P fallback
- A sectioned Settings page with startup, output folder, clock, theme, update, and about options
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
- `Essentia` for optional BPM and musical key analysis

`yt-dlp.exe`, `ffmpeg.exe`, and the Essentia music extractor are bundled with the app and copied to the build output under the `tools` folder. To update a helper later, replace the matching file in `tools`.

The app first looks for bundled media helpers beside the app, then falls back to tools available on `PATH`. If a dependency is missing, it shows a setup message instead of crashing.

The downloader remembers its last selected format and quality in `%APPDATA%\RexToolkit\media_downloader.txt`. The default output folder is managed in Settings.

## Settings

Settings are saved in `%APPDATA%\RexToolkit\settings.txt`. The Settings page includes:

- Default output folder
- Startup page: Favorites or All Tools
- Date/time format for the top-right clock
- Theme selection, including light mode
- Manual update checks
- About details, project links, licenses, and bundled third-party tools

### Anime Tracker

The Anime Tracker lets you search AniList, add anime to a local watchlist, track watched episodes, change watch status, save local notes, refresh metadata, and see upcoming episodes or sequel relations when AniList provides them.

It does not require a login, does not scrape streaming sites, and does not include piracy or playback features. Watchlist data is saved locally in `%APPDATA%\RexsToolkit\anime_tracker.json`.

### Reminders

The Reminders tool lets you create, edit, complete, snooze, delete, filter, and sort local reminders. Reminders support title, due date/time, all-day mode, notes, category, priority, recurrence, alert timing, and birthday-style yearly reminders.

When reminders are due soon, due now, or overdue, Rex's Toolkit can show a thin alert banner above the main content with Snooze, Complete, View, and close actions. Reminder data is saved locally in `%APPDATA%\RexToolkit\reminders.json`.

### Smart File Transfer

Smart File Transfer sends selected files directly between Rex's Toolkit users. It tries LAN first, then Direct Host if the sender enables temporary UPnP port mapping. If both direct HTTP routes fail, the app can guide both users through manual WebRTC P2P pairing with sender and receiver response codes.

Manual P2P uses WebRTC data channels through bundled `libdatachannel`, with public STUN servers only. There is no cloud storage, no paid signaling server, and no TURN relay in this version. Files are streamed in chunks and receivers write `.part` files before finalizing completed downloads.

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

## Bundled Native Dependencies

- `yt-dlp`, `FFmpeg`, and `Essentia` power the Media Downloader.
- `libdatachannel` powers Smart File Transfer's manual WebRTC P2P fallback.
- Static OpenSSL libraries are bundled under `third_party/openssl-x64-windows-static` for the libdatachannel build.

`libdatachannel` is MPL-2.0 licensed. OpenSSL license files are included in the bundled vcpkg package folders under `third_party/openssl-x64-windows-static/share/openssl`.

## Adding Tools Later

Future tools are registered in `src/ToolkitApp.cpp` inside `CreateToolRegistry()`.

Add new `ToolDefinition` entries there, then wire each tool to its own view or command handler as the toolkit grows.
