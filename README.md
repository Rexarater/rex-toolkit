<p align="center">
  <img src="assets/rex_toolkit_logo.png" alt="Rex's Toolkit logo" width="180">
</p>

<h1 align="center">Rex's Toolkit</h1>

<p align="center">
  A free, all-in-one Windows utility app built to keep useful everyday tools in one polished place.
</p>

<p align="center">
  <a href="https://github.com/Rexarater/rex-toolkit/releases/latest"><strong>Download the latest release</strong></a>
  &nbsp;|&nbsp;
  <a href="https://github.com/Rexarater/rex-toolkit/issues/new">Report an issue</a>
</p>

Rex's Toolkit is a native Windows desktop app with a customizable interface, saved favorites, multiple themes, built-in update support, and a growing collection of practical tools. It is free to download and use.

## Getting Started

1. Open the [latest release](https://github.com/Rexarater/rex-toolkit/releases/latest).
2. Download the `RexsToolkit_v*.zip` file under **Assets**.
3. Extract the entire ZIP to a folder of your choice.
4. Run `RexToolkit.exe`.

Keep `RexToolkit.exe` and the included `tools` folder together. The release already contains the helper programs needed by features such as media downloading, music analysis, file transfer, and driver-enhanced macro playback.

Rex's Toolkit currently supports Windows 10 and Windows 11. Windows may show a SmartScreen prompt for a newly downloaded build; review the publisher and file before choosing to run it.

## Included Tools

| Tool | What it does |
| --- | --- |
| **Auto Clicker** | Repeats a keyboard or mouse output while an activation bind is held, or toggles clicking on and off when Toggle Mode is enabled. Supports keyboard keys, mouse buttons, alternating outputs, adjustable speed, and remembered binds. The activation bind only runs while the Auto Clicker page is open. |
| **Macro Recorder** | Records keyboard and mouse actions with timing, lets you test a recording before saving it, and replays it with configurable looping and emergency-stop controls. A compact always-on-top control strip is included for games and other full-screen workflows. |
| **File Converter** | Converts one or more images between WEBP, PNG, JPG/JPEG, and BMP with drag-and-drop, save-location prompts, conflict handling, and format-specific options. Available WEBP support depends on the Windows codecs installed on the computer. |
| **Video Compressor** | Compresses a video to fit under a chosen file size using FFmpeg. Accurate mode uses two-pass H.264 encoding; Fast mode uses a quicker one-pass encode. The tool verifies output size and can retry automatically at a lower bitrate. |
| **Video & Image Editor** | Trims, splits, combines, and reorders video clips on a simple timeline, or crops, rotates, and draws on images. Supports drag-and-drop, clipboard paste, undo/redo, quality presets, and optional per-user File Explorer integration. |
| **YouTube & SoundCloud Downloader** | Analyzes direct YouTube video and SoundCloud track links, then saves authorized media as MP4, editor-compatible MOV, MP3, or WAV. Quality selection, metadata preview, and a beta Essentia-powered BPM and key finder are included. |
| **Equalizer** | Applies headphone-specific recommended EQ and simple sound preferences to supported Windows audio outputs. Includes per-output profiles, automatic clipping protection, graphic and parametric editing, local measurement import, and tray shortcuts. The complete Rex bundle includes an optional, guided Equalizer APO setup for real system-wide processing. |
| **Anime Tracker** | Searches AniList, displays anime details, characters, and voice actors, and keeps a local watchlist with episode progress, statuses, notes, airing information, and sequels. Public AniList and MyAnimeList profiles can be imported without sharing a password. |
| **Reminders** | Creates local reminders using regular date/time controls or smart titles such as `Call HCC on the 25th at 11am`. Supports recurring reminders, categories, priorities, notification sounds, and tray/startup behavior so alerts can continue in the background. |
| **(Beta) Smart File Transfer** | Sends files directly between Rex's Toolkit users with a transfer code. It tries LAN first, can attempt Direct Host through temporary port mapping, and includes a manual WebRTC peer-to-peer fallback. Availability depends on both users' networks. |

## App Features

- Favorites that persist between launches
- Searchable **All Tools** grid
- Dark, light, and additional color themes
- Configurable default download folder and clock format
- Choice of opening to Favorites or All Tools
- Optional minimize-to-tray and start-with-Windows behavior
- Automatic background update checks with an in-app installer
- Remembered window size and position

## Video Compressor Notes

Video Compressor version 1 accepts MP4, MOV, MKV, WEBM, AVI, and M4V inputs and creates MP4 output. CPU encoding is the quality-first default and Accurate mode uses two passes. Automatic GPU mode detects supported NVIDIA, Intel, or AMD hardware and falls back to CPU when hardware encoding is unavailable. Very small targets can cause poor visual quality. Metadata is removed by default; subtitles and additional audio tracks may not be preserved.

## Equalizer Notes

Equalizer uses a replaceable backend architecture and currently integrates with [Equalizer APO](https://sourceforge.net/projects/equalizerapo/) for real system-wide processing. The complete Rex's Toolkit bundle includes the unmodified official Equalizer APO 1.4.2 x64 installer, so no separate download is needed. Installation is optional and never starts without explicit consent: Rex verifies the bundled package, shows the exact Windows playback output it will configure, explains the system change, and waits for approval before Windows requests administrator access.

After approval, Rex runs the verified installer, enables the selected output through Equalizer APO's official Device Selector, and waits for its compatibility check. Existing configured outputs are preserved. If the endpoint cannot be matched exactly or Equalizer APO reports a compatibility issue, the official selector remains visible for manual confirmation instead of Rex guessing. Rex then creates a one-time backup, adds one managed Include line without replacing existing commands, and keeps its filters in a separate `rexs_toolkit_equalizer.txt` file. An existing compatible Equalizer APO installation is detected and reused. If setup requires an audio-service restart, Windows restart, or app restart, Rex records the pending setup locally and resumes with the correct next step.

The EQ applies to supported Windows audio on configured output endpoints. ASIO, WASAPI exclusive mode, and other paths that bypass normal Windows system effects may bypass it. True simultaneous per-application EQ is not exposed because this backend integration cannot honestly provide audio-session-specific DSP.

Headphone results are not bundled. **Update Profiles** opt-in downloads AutoEq's public result directory over HTTPS, and choosing an uncached model downloads its precomputed parametric-EQ result. Profile source and version are retained. Custom measurements can be imported and optimized locally with Rex's bounded native optimizer. Recommended correction is measurement-based and may vary with the individual unit, fit, seal, pad wear, and listener anatomy.

See [Equalizer Help](docs/EQUALIZER.md) for setup, supported paths, profile storage, imports, and advanced controls. See [Third-Party Notices](THIRD_PARTY_LICENSES.md) and the bundled [Equalizer APO source record](tools/equalizerapo/SOURCE.md) for licensing, source, and package-integrity details.
## Privacy and Safety

- Settings, favorites, reminders, macros, and anime progress are stored locally in the user's Windows AppData folders.
- Rex's Toolkit does not require an account.
- Network access is used only when a feature needs it, such as checking for updates, retrieving AniList data, importing a public anime list, analyzing a media link, or transferring a file.
- Smart File Transfer does not upload files to project-owned cloud storage. LAN and Direct Host use temporary HTTP hosting; manual P2P uses encrypted WebRTC data channels and public STUN servers. Direct Host may be unavailable on networks that block inbound connections or use CGNAT.
- The downloader is intended only for media the user is authorized to save. DRM-protected media is not bypassed.
- Equalizer audio processing and optimization are local. Profile updates download data only after the user requests them; audio, device identifiers, headphone selections, EQ settings, and measurements are not uploaded.
- Driver-enhanced Macro Recorder input is optional. Enabling it for the first time launches the bundled Interception installer, requests administrator permission, and requires a Windows restart. Standard recording and playback remain available without the driver.

## Updates

Rex's Toolkit silently checks for new releases when it opens and can also be checked manually from **Settings > Updates**. When an update is accepted, the app downloads the release package, installs it over the existing copy, and relaunches while preserving local settings and saved data.

Installing the app in a protected Windows folder may prevent an update from replacing files. A normal user-owned folder, such as a folder inside Documents, is recommended.

## Building From Source

Most users only need the packaged ZIP from the Releases page. To build the project yourself, install:

- Windows 10 or newer
- CMake 3.20 or newer
- Visual Studio with **Desktop development with C++**

From a Developer PowerShell or Developer Command Prompt:

```powershell
cmake --preset windows-release
cmake --build --preset windows-release
```

The executable is created at:

```text
build\windows-release\RexToolkit.exe
```

Visual Studio can also open the repository as a CMake project. Select the `windows-debug` or `windows-release` preset. If an older configuration used the wrong generator, remove the generated `build` folder and configure the project again.

New tools are registered in `CreateToolRegistry()` inside `src/ToolkitApp.cpp`. The registry supplies navigation, search, favorites, and launch metadata.

## Third-Party Components

Rex's Toolkit bundles or integrates with several third-party projects:

- [yt-dlp](https://github.com/yt-dlp/yt-dlp) for supported media retrieval
- [FFmpeg](https://ffmpeg.org/) for media processing and conversion
- [Essentia](https://essentia.upf.edu/) for music analysis
- [libdatachannel](https://github.com/paullouisageneau/libdatachannel) and [OpenSSL](https://www.openssl.org/) for WebRTC file transfer
- [Interception](https://github.com/oblitum/Interception) for optional driver-enhanced macro input
- [Equalizer APO](https://sourceforge.net/projects/equalizerapo/) as the optional system-wide audio backend; the complete x64 bundle carries its unmodified official 1.4.2 installer and GPL-2.0 license
- [AutoEq](https://github.com/jaakkopasanen/AutoEq) result documents for opt-in headphone profiles; the AutoEq application and measurement database are not bundled
- Windows Imaging Component for image conversion
- AniList's public API for anime metadata

Applicable third-party license files are included with bundled components. Equalizer-specific notices are in [`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md).

## Feedback

Found a bug or have an idea for another useful tool? [Open a GitHub issue](https://github.com/Rexarater/rex-toolkit/issues/new) with a clear description, the Windows version being used, and steps to reproduce the problem when possible.

Developed by **Rexarater**, with special thanks to **Addion**.
