# Equalizer Help

## What it does

Equalizer applies headphone-specific correction and simple sound preferences to supported Windows audio on a selected output. The everyday page is intentionally simple: choose an output, choose headphones, select a sound preset, and optionally adjust Bass, Warmth, Presence, or Treble.

Processing is local. Rex's Toolkit does not upload audio, output-device information, headphone selections, custom EQ profiles, or imported measurements.

## System-wide backend setup

Rex's Toolkit uses Equalizer APO as its replaceable Windows audio backend. The complete Rex bundle includes the unmodified official Equalizer APO 1.4.2 x64 installer. Installation is optional and never starts without the user's explicit confirmation.

1. Open Rex's Toolkit > Equalizer, select the Windows playback output to use, and choose **Install engine**.
2. Rex verifies the bundled installer and shows the selected output by name before requesting confirmation.
3. After Windows administrator approval, Rex runs the verified installer, enables that output through Equalizer APO's official Device Selector, and waits for the official compatibility check.
4. Return to Rex. Choose **Finish setup** if prompted. Rex adds one managed Include line to Equalizer APO's `config.txt`, creates `config.txt.rex-backup` if no backup exists, and writes Rex-owned filters to `rexs_toolkit_equalizer.txt`.

The automatic selector changes only the approved output and preserves outputs that were already configured. If Rex cannot match the endpoint exactly, Windows accessibility is unavailable, or Equalizer APO reports a compatibility problem, the official selector remains visible for manual confirmation. Rex does not guess or report the endpoint ready until its attachment can be verified.

No separate Equalizer APO download is required. If a compatible installation already exists, Rex detects and reuses it instead of reinstalling it. Rex stores a small local setup marker when a restart or output-selection step is pending, then reports the exact next action when the app opens again.

Administrator approval is requested for the official system installation and only when the Equalizer APO configuration folder otherwise cannot be updated. Existing Equalizer APO commands remain in place. Diagnostics can open Device Selector, repair or finish the managed include, re-enumerate outputs, show the managed data folder, update profiles, or copy technical details.

The main ON/OFF control updates the managed endpoint configuration immediately. Equalizer APO observes configuration changes without restarting Rex's Toolkit.

Rex never installs the backend during app startup or without the setup confirmation. Audio and Equalizer settings remain local.

## Supported audio and limitations

The EQ applies system-wide to supported Windows audio output through Equalizer APO. Some applications using ASIO, WASAPI exclusive mode, or another path that bypasses normal Windows system effects can bypass the EQ. Rex's Toolkit does not claim true per-application EQ because Equalizer APO's normal configuration path does not provide simultaneous, session-specific DSP to this integration.

Endpoint format information is read through Windows Core Audio. Equalizer APO performs the real-time processing and handles the active sample rate and channel layout; Rex emits endpoint-specific, all-channel filters. The modeled response graph is a calculation of the final EQ filters, not a microphone measurement or live audio analyzer.

## Headphone profiles

Rex's Toolkit ships without a third-party headphone measurement database. Choose Update Profiles to download the AutoEq result directory over HTTPS. Selecting an uncached model downloads its precomputed parametric-EQ result and caches it under:

```text
%APPDATA%\RexToolkit\equalizer\
```

Profile source, attribution URL, and database version are retained. Recommended EQ is measurement-based, but individual units, fit, seal, pad wear, and ear anatomy can change the result.

For custom measurements, import CSV or text containing strictly increasing frequency and response values, for example:

```csv
frequency,raw
20,-3.2
21,-3.1
20000,-5.0
```

Imports must contain at least 20 valid points and cover approximately 20 Hz through 20 kHz. Rex validates file size, values, ordering, duplicates, and filter limits, then runs its bounded native ten-filter optimizer on a worker-safe local path.

## Simple and advanced controls

Simple mode layers the selected sound preference and four constrained +/-6 dB controls over the headphone correction. Presets include Balanced, Bass+, Bass Reduce, Warm, Bright, Music, Movies, Voice, Gaming, Footsteps, and Late Night.

Customize EQ adds:

- A modeled 20 Hz to 20 kHz final-EQ response graph
- Automatic or manual preamp with clipping warnings
- Ten-band graphic EQ from 31 Hz through 16 kHz
- Parametric peak, shelf, pass, and notch filters
- Exact frequency, gain, and Q editing
- `.rexeq` import/export
- Frequency-response import and native optimization
- Recommended/custom/device reset actions
- Optional backend diagnostics

Prevent Clipping is on by default and reserves negative preamp headroom for positive boosts. Rex's Toolkit never raises Windows master volume. EQ cannot guarantee a safe listening level.

## Data and settings

Equalizer settings are stored locally by stable Windows endpoint identifier. By default, each output remembers its enabled state, headphones, sound preference, controls, graphic/parametric edits, and preamp behavior. Follow Windows Default reacts to endpoint changes; automatic apply can be disabled in Settings > Equalizer.

Downloaded AutoEq directory/profile documents and generated custom profiles are data only and are never executed. Imported `.rexeq` JSON is schema-checked and bounded before use.
## Current scope limits

- AutoEq downloads use the target and filter set published with each selected result. The current UI does not provide an arbitrary target-curve library or a separate target selector.
- Custom frequency-response imports use Rex's bounded native ten-band optimizer against a locally normalized reference. This is not AutoEq's full Python optimizer and is labeled separately in profile metadata.
- The graph shows the modeled response of the filters Rex will apply. It does not reconstruct the headphone's measured response or display a live original/target/final measurement comparison.
- `.rexeq` import/export and per-output persistence are available, but a full named-profile organizer with rename, duplicate, favorites, and A/B comparison is not included yet.
- True per-application EQ is not exposed because this backend cannot guarantee isolated, simultaneous processing for individual Windows audio sessions.