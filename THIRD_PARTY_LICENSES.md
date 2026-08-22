# Third-Party Notices

This file records notices for third-party software and data used, bundled, or integrated by Rex's Toolkit.

## Equalizer APO

- Project: https://sourceforge.net/projects/equalizerapo/
- Bundled version: 1.4.2 x64 installer
- License: GNU General Public License version 2.0 (GPL-2.0)
- Bundled corresponding source: `tools/equalizerapo/EqualizerAPO-src-1.4.2.zip`
- Upstream source: https://sourceforge.net/projects/equalizerapo/files/1.4.2/EqualizerAPO-src-1.4.2.zip/download
- Local license copy: `tools/equalizerapo/licenses/GPL-2.0.txt`
- Package provenance and checksums: `tools/equalizerapo/SOURCE.md`

The complete Rex's Toolkit x64 bundle redistributes Equalizer APO's unmodified official installer as an optional system component. Rex's Toolkit does not link Equalizer APO code into its executable. It validates the pinned installer before use, explains the system change, and runs the installer only after explicit user consent and Windows administrator approval. Rex uses Windows UI Automation to select the user-approved endpoint through Equalizer APO's own Device Selector; if that cannot be done unambiguously, the selector remains visible for manual confirmation. Rex then writes a separate, user-approved configuration include while preserving existing Equalizer APO commands and a one-time backup.

The bundled corresponding-source archive, full GPL-2.0 text, and provenance record must remain with releases that redistribute this installer.

## AutoEq

- Project: https://github.com/jaakkopasanen/AutoEq
- License for AutoEq software: MIT
- Copyright (c) 2018-2022 Jaakko Pasanen

Rex's Toolkit does not bundle the AutoEq Python application or its scientific runtime. With the user's explicit Update Profiles action, Rex's Toolkit downloads the public AutoEq result index and selected precomputed parametric-EQ result documents over HTTPS, validates and parses them as data, and caches them locally. The selected profile's source, URL, and version are preserved in exported `.rexeq` files.

The AutoEq software license does not automatically grant redistribution rights to every headphone measurement source referenced by AutoEq. Rex's Toolkit therefore ships no third-party headphone measurement database. Measurement-source attribution from the AutoEq index is retained with downloaded profiles. Any underlying measurement data remains subject to its original source's terms.

### AutoEq MIT License

MIT License

Copyright (c) 2018-2022 Jaakko Pasanen

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## QuickJS-NG

- Project: https://github.com/quickjs-ng/quickjs
- Bundled version: 0.15.1, Windows x86_64
- License: MIT
- Local license copy: `tools/quickjs-ng-LICENSE.txt`

Rex's Toolkit bundles the unmodified QuickJS-NG command-line runtime so yt-dlp can solve current YouTube JavaScript challenges without requiring users to install a separate runtime.

## Other bundled and integrated components

Rex's Toolkit also bundles or integrates yt-dlp, FFmpeg, Essentia, libdatachannel, OpenSSL, Interception, nlohmann/json, AniList, and Windows system APIs. Their project links and roles are listed in `README.md`; license files supplied with source dependencies remain in their respective `third_party` directories. Release packaging must retain all license files supplied alongside bundled binaries.