# Equalizer APO bundled component

Rex's Toolkit redistributes the unmodified official x64 installer for Equalizer APO 1.4.2 as an optional, user-approved system audio backend.

- Upstream project: https://sourceforge.net/projects/equalizerapo/
- Binary release folder: https://sourceforge.net/projects/equalizerapo/files/1.4.2/
- Bundled binary: `EqualizerAPO-x64-1.4.2.exe`
- Bundled corresponding source: `EqualizerAPO-src-1.4.2.zip`
- Upstream source archive: https://sourceforge.net/projects/equalizerapo/files/1.4.2/EqualizerAPO-src-1.4.2.zip/download
- License: GNU General Public License version 2; see `licenses/GPL-2.0.txt`

## Integrity

SourceForge publishes these checksums for the x64 1.4.2 installer:

- SHA-1: `f599c3a16d27864d330529a012fc23a37abdd06d`
- MD5: `410aab9749ae4673b950bc29a4eb226f`

Rex's Toolkit additionally pins and verifies this SHA-256 before starting the installer, including inside its elevated setup helper:

- SHA-256: `7403be7427bbe1936a40dded082829b6e217fc4f5990fee5cba501f0ae055afa`
- Source archive SHA-256: `bacd5c78be71f3aaab010684ab39f0e5c1bbb45fcd75cbb92c103a145a731dc4`

The upstream installer is not Authenticode-signed. The pinned digest prevents Rex's Toolkit from opening a missing, incomplete, or modified bundled installer. Installation is never silent: Rex explains the system changes first, and Windows requests administrator approval before the official installer opens.

## Source and modifications

The Equalizer APO installer and corresponding source archive are redistributed without modification. Rex's Toolkit does not link Equalizer APO into its own executable. Rex writes a separate, user-approved configuration include after installation. Releases that redistribute the installer must retain the bundled source archive, GPL-2.0 text, and this record.
