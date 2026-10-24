# Third-party software

StudioWire is free software, licensed under the GNU General Public License,
version 3 or (at your option) any later version: see [LICENSE](LICENSE).

It is built with the software below. The build scripts download it from its
authors' GitHub repositories, at the versions pinned in `scripts/common.sh`; none
of it is modified. The installer copies these licence texts to
`/Library/Application Support/StudioWire/Licenses`.

## Inside the plugin (statically linked)

| Software | Version | Licence |
|---|---|---|
| [JUCE](https://github.com/juce-framework/JUCE) | 9.0.4 | AGPL-3.0-only (or the commercial JUCE licence) |
| VST3 SDK (Steinberg, included in JUCE) | | MIT |
| AudioUnitSDK (Apple, included in JUCE) | | Apache-2.0 |
| [civetweb](https://github.com/civetweb/civetweb) | 1.16 | MIT |
| [libFLAC](https://github.com/xiph/flac) | 1.5.0 | BSD-3-Clause |
| [libopus](https://github.com/xiph/opus) | 1.5.2 | BSD-3-Clause |
| [SpeexDSP](https://github.com/xiph/speexdsp) resampler | 1.2.1 | BSD-3-Clause |

JUCE is used under the AGPLv3. Section 13 of the GPLv3 and of the AGPLv3 allows
combining the two: the JUCE part of the plugin stays under the AGPLv3, the rest
under the GPLv3. JUCE itself contains other libraries (zlib, HarfBuzz, SheenBidi,
libpng, the IJG JPEG library, LunaSVG, PlutoVG, …); they are listed with their
licences in JUCE's `JUCE.spdx.json`.

## Sent to listeners (the listening page)

| Software | Version | Licence |
|---|---|---|
| [wasm-audio-decoders](https://github.com/eshaz/wasm-audio-decoders): `@wasm-audio-decoders/flac` | 0.2.11 | MIT |
| [wasm-audio-decoders](https://github.com/eshaz/wasm-audio-decoders): `opus-decoder` | 0.7.12 | MIT |

They are in `web/` as published (minified), and contain libFLAC and libopus
compiled to WebAssembly (BSD-3-Clause).

## Included in the installer, as a separate program

| Software | Licence |
|---|---|
| [cloudflared](https://github.com/cloudflare/cloudflared) (Cloudflare, Inc.) | Apache-2.0 |

cloudflared is not part of the plugin: it is a separate program, run as its own
process to open the Quick Tunnel. `package.sh` downloads the latest official
macOS release, checks it against the SHA-256 checksums published with the
release, and combines the Apple Silicon and Intel versions into one file
(`lipo`) without changing them. The plugin can update it the same way.

## Trademarks

Audio Units is a trademark of Apple Inc. VST is a registered
trademark of Steinberg Media Technologies GmbH. Cloudflare is a trademark of
Cloudflare, Inc. StudioWire is not affiliated with any of them.
