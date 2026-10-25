#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The StudioRider authors
#
# Makes the StudioRider installer: build/StudioRider-<version>.pkg
#   - AU + VST3, universal (Apple Silicon + Intel), macOS 13 or later
#   - cloudflared (latest release from Cloudflare's GitHub, checksums verified,
#     Apple Silicon + Intel), used for the public link
#   - licence texts, in /Library/Application Support/StudioRider
#   - an uninstaller app, in /Applications/StudioRider
#
# Also writes build/StudioRider-<version>-source.tar.gz (the matching source code,
# to hand out with the installer as the GPL asks).
#
# Optional signing (not needed to use the installer, see README):
#   SIGN_APP="Developer ID Application: …"  SIGN_INSTALLER="Developer ID Installer: …"
#   NOTARY_PROFILE=<notarytool keychain profile>   ./package.sh
set -euo pipefail
cd "$(dirname "$0")"
source scripts/common.sh

ARCHS="arm64;x86_64"
PKG_ID="net.studiorider.pkg"
VERSION="$(studiorider_version)"         # 1.0.0: for macOS (package and bundle versions)
SHOWN="$(studiorider_shown_version)"     # 1.0: in names and titles
[[ -n "$VERSION" ]] || die "Could not read the version from CMakeLists.txt."

check_tools
for t in pkgbuild productbuild lipo codesign otool curl python3 shasum ditto osacompile plutil; do
    command -v "$t" >/dev/null 2>&1 || die "$t is missing (it comes with Apple's developer tools)."
done
fetch_dependencies

# --- 1. The plugin, universal --------------------------------------------------
CMAKE_DIR=build/universal
cmake_build "$CMAKE_DIR" "$ARCHS" -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN_MACOS"
ART="$CMAKE_DIR/StudioRider_artefacts/Release"
AU_SRC="$ART/AU/StudioRider.component"
VST3_SRC="$ART/VST3/StudioRider.vst3"

say "Checking the binaries"
minos() { # minos <binary> <arch>: minimum macOS the slice was built for
    otool -arch "$2" -l "$1" | awk '/LC_BUILD_VERSION/ {b=1} /LC_VERSION_MIN_MACOSX/ {v=1}
                                    b && $1=="minos" {print $2; exit} v && $1=="version" {print $2; exit}'
}
for bin in "$AU_SRC/Contents/MacOS/StudioRider" "$VST3_SRC/Contents/MacOS/StudioRider"; do
    [[ -f "$bin" ]] || die "Missing build output: $bin"
    info="$(lipo -info "$bin")"
    [[ "$info" == *arm64* && "$info" == *x86_64* ]] || die "Not a universal binary: $info"
    echo "$(basename "$(dirname "$(dirname "$(dirname "$bin")")")"): arm64 (macOS $(minos "$bin" arm64)+), x86_64 (macOS $(minos "$bin" x86_64)+)"
done

# --- 2. cloudflared, universal -------------------------------------------------
CF=build/cloudflared
say "Getting cloudflared (latest release, from github.com/cloudflare/cloudflared)"
mkdir -p "$CF"
CF_API="${STUDIORIDER_CLOUDFLARED_API:-https://api.github.com/repos/cloudflare/cloudflared/releases/latest}"  # (tests use a fake one)
curl -fsSL --retry 3 -H "Accept: application/vnd.github+json" -H "User-Agent: StudioRider" \
     -o "$CF/release.json" "$CF_API" \
    || die "Could not reach GitHub to get cloudflared."
# Prints: <tag>, then "<asset> <url> <notes checksum or -> <GitHub digest or ->" per architecture.
# Cloudflare's release notes give, for each .tgz, the checksum of the program inside it;
# GitHub's "digest" is the checksum of the .tgz itself. Both are checked when present.
python3 -I - "$CF/release.json" > "$CF/assets.txt" 2> "$CF/assets.err" <<'PY' || die "Unexpected cloudflared release: $(cat "$CF/assets.err" 2>/dev/null)"
import json, re, sys
r = json.load(open(sys.argv[1]))
print(r["tag_name"])
body = r.get("body") or ""
for arch in ("arm64", "amd64"):
    name = "cloudflared-darwin-%s.tgz" % arch
    assets = [a for a in r.get("assets", []) if a.get("name") == name]
    if not assets:
        sys.exit("%s is missing from release %s" % (name, r["tag_name"]))
    m = re.search(re.escape(name) + r"\s*:\s*([0-9a-fA-F]{64})", body)
    notes = m.group(1).lower() if m else "-"
    d = (assets[0].get("digest") or "").lower()
    digest = d[7:] if d.startswith("sha256:") and len(d) == 71 else "-"
    if notes == "-" and digest == "-":
        sys.exit("%s has no published checksum in release %s" % (name, r["tag_name"]))
    print(name, assets[0]["browser_download_url"], notes, digest)
PY
CF_VERSION="$(head -1 "$CF/assets.txt")"
if [[ -x "$CF/cloudflared" && "$(cat "$CF/version" 2>/dev/null)" == "$CF_VERSION" ]]; then
    echo "cloudflared $CF_VERSION (already downloaded)"
else
    rm -rf "$CF/arm64" "$CF/amd64" "$CF/cloudflared" "$CF/version"
    while read -r name url notes digest; do
        arch="${name#cloudflared-darwin-}"; arch="${arch%.tgz}"
        echo "Downloading $name"
        curl -fL --retry 3 --progress-bar -o "$CF/$name" "$url" || die "Could not download $name."
        tgz_sha="$(shasum -a 256 "$CF/$name" | cut -d' ' -f1)"
        [[ "$digest" == "-" || "$tgz_sha" == "$digest" ]] || die "$name doesn't match GitHub's checksum."
        mkdir -p "$CF/$arch"
        tar -xzf "$CF/$name" -C "$CF/$arch"
        [[ -f "$CF/$arch/cloudflared" ]] || die "$name does not contain cloudflared."
        if [[ "$notes" != "-" ]]; then
            exe_sha="$(shasum -a 256 "$CF/$arch/cloudflared" | cut -d' ' -f1)"
            [[ "$notes" == "$tgz_sha" || "$notes" == "$exe_sha" ]] || die "$name doesn't match Cloudflare's checksum."
        fi
        echo "  checksum OK"
        rm "$CF/$name"
    done < <(tail -n +2 "$CF/assets.txt")
    lipo -create "$CF/arm64/cloudflared" "$CF/amd64/cloudflared" -output "$CF/cloudflared"
    chmod 755 "$CF/cloudflared"
    # Keeps Cloudflare's own signatures (one per architecture); if there is none, sign locally.
    codesign -v "$CF/cloudflared" >/dev/null 2>&1 || codesign --force --sign - "$CF/cloudflared"
    echo "$CF_VERSION" > "$CF/version"
    rm -rf "$CF/arm64" "$CF/amd64"
fi
echo "cloudflared $CF_VERSION: $(lipo -info "$CF/cloudflared" | sed 's/.*: //')"
MACOS="$(sw_vers -productVersion)"
if "$CF/cloudflared" --version >/dev/null 2>&1; then
    echo "Runs on this Mac: $("$CF/cloudflared" --version 2>&1 | head -1)"
else
    printf "\033[1;33mWarning:\033[0m this cloudflared doesn't run on this Mac (macOS %s).\n" "$MACOS"
    echo "      The installer is still made. On this Mac, StudioRider will use Homebrew's cloudflared if there is one."
fi

# --- 3. Files to install ---------------------------------------------------------
say "Preparing the installer contents"
ROOT=build/pkgroot
SUPPORT="$ROOT/Library/Application Support/StudioRider"
UNINSTALLER="$ROOT/Applications/StudioRider/Uninstall StudioRider.app"
rm -rf "$ROOT" build/pkgparts
mkdir -p "$ROOT/Library/Audio/Plug-Ins/Components" "$ROOT/Library/Audio/Plug-Ins/VST3" "$SUPPORT/Licenses" \
         "$(dirname "$UNINSTALLER")" build/pkgparts
ditto --norsrc --noextattr "$AU_SRC"   "$ROOT/Library/Audio/Plug-Ins/Components/StudioRider.component"
ditto --norsrc --noextattr "$VST3_SRC" "$ROOT/Library/Audio/Plug-Ins/VST3/StudioRider.vst3"
install -m 755 "$CF/cloudflared" "$SUPPORT/cloudflared"

# The uninstaller: a small AppleScript app (asks for the password with the usual macOS dialog).
osacompile -o "$UNINSTALLER" "packaging/uninstaller/Uninstall StudioRider.applescript" \
    || die "Could not make the uninstaller (osacompile)."
install -m 755 packaging/uninstaller/uninstall.sh "$UNINSTALLER/Contents/Resources/uninstall.sh"
plutil -replace CFBundleIdentifier -string net.studiorider.uninstaller "$UNINSTALLER/Contents/Info.plist"
plutil -replace CFBundleShortVersionString -string "$VERSION" "$UNINSTALLER/Contents/Info.plist"
plutil -replace LSMinimumSystemVersion -string "$MIN_MACOS" "$UNINSTALLER/Contents/Info.plist"

L="$SUPPORT/Licenses"
cp LICENSE                                   "$L/StudioRider (GPL-3.0).txt"
cp THIRD_PARTY_NOTICES.md                    "$L/THIRD_PARTY_NOTICES.md"
cp packaging/licenses/cloudflared-LICENSE.txt "$L/cloudflared (Apache-2.0).txt"
cp packaging/licenses/wasm-audio-decoders-LICENSE.txt "$L/wasm-audio-decoders (MIT).txt"
cp third_party/civetweb/LICENSE.md           "$L/civetweb (MIT).md"
cp third_party/flac/COPYING.Xiph             "$L/libFLAC (BSD-3-Clause).txt"
cp third_party/opus/COPYING                  "$L/libopus (BSD-3-Clause).txt"
cp third_party/speexdsp/COPYING              "$L/SpeexDSP (BSD-3-Clause).txt"
mkdir -p "$L/JUCE"
cp JUCE/LICENSE.md JUCE/JUCE.spdx.json       "$L/JUCE/"
cp JUCE/modules/juce_gui_extra/native/typescript/webview-interop/LICENSE-AGPL.md "$L/JUCE/AGPL-3.0.md"
cp "$(find JUCE/modules -path '*VST3_SDK/LICENSE.txt' | head -1)"  "$L/VST3 SDK (MIT).txt"
cp JUCE/modules/juce_audio_plugin_client/AU/AudioUnitSDK/LICENSE.txt "$L/AudioUnitSDK (Apache-2.0).txt"
find "$ROOT" -name .DS_Store -delete
xattr -cr "$ROOT" 2>/dev/null || true

say "Signing"
if [[ -n "${SIGN_APP:-}" ]]; then
    for b in "$ROOT/Library/Audio/Plug-Ins/Components/StudioRider.component" "$ROOT/Library/Audio/Plug-Ins/VST3/StudioRider.vst3" "$UNINSTALLER"; do
        codesign --force --timestamp --options runtime --sign "$SIGN_APP" "$b"
    done
    codesign -dv "$SUPPORT/cloudflared" 2>&1 | grep -q "TeamIdentifier=not set" \
        && codesign --force --timestamp --options runtime --sign "$SIGN_APP" "$SUPPORT/cloudflared"
    echo "Signed with: $SIGN_APP"
else
    codesign --force --deep --sign - "$ROOT/Library/Audio/Plug-Ins/Components/StudioRider.component"
    codesign --force --deep --sign - "$ROOT/Library/Audio/Plug-Ins/VST3/StudioRider.vst3"
    codesign --force --deep --sign - "$UNINSTALLER"
    echo "Signed locally (ad hoc). The installer is not signed: see README, \"Installing\"."
fi

# --- 4. The .pkg -------------------------------------------------------------------
say "Building the installer"
pkgbuild --analyze --root "$ROOT" build/pkgparts/components.plist >/dev/null
# Always install into /Library, even if a copy of the plugin exists somewhere else on the disk.
python3 -I - build/pkgparts/components.plist <<'PY'
import plistlib, sys
path = sys.argv[1]
items = plistlib.load(open(path, "rb"))
def fix(entries):
    for e in entries:
        e["BundleIsRelocatable"] = False
        e["BundleHasStrictIdentifier"] = False
        e["BundleOverwriteAction"] = "upgrade"
        fix(e.get("ChildBundles", []))
fix(items)
plistlib.dump(items, open(path, "wb"))
PY
pkgbuild --root "$ROOT" \
         --component-plist build/pkgparts/components.plist \
         --scripts packaging/scripts \
         --identifier "$PKG_ID" \
         --version "$VERSION" \
         --install-location / \
         build/pkgparts/StudioRider.pkg >/dev/null

sed -e "s/@VERSION@/$VERSION/g" -e "s/@SHOWN@/$SHOWN/g" -e "s/@MIN_MACOS@/$MIN_MACOS/g" -e "s/@PKG_ID@/$PKG_ID/g" \
    packaging/Distribution.xml > build/pkgparts/Distribution.xml
mkdir -p build/pkgparts/resources
for f in packaging/resources/*.html; do
    sed -e "s/@VERSION@/$SHOWN/g" -e "s/@CLOUDFLARED@/$CF_VERSION/g" "$f" > "build/pkgparts/resources/$(basename "$f")"
done

PKG="build/StudioRider-$SHOWN.pkg"
rm -f "$PKG"
if [[ -n "${SIGN_INSTALLER:-}" ]]; then
    productbuild --distribution build/pkgparts/Distribution.xml --resources build/pkgparts/resources \
                 --package-path build/pkgparts --sign "$SIGN_INSTALLER" --timestamp "$PKG" >/dev/null
else
    productbuild --distribution build/pkgparts/Distribution.xml --resources build/pkgparts/resources \
                 --package-path build/pkgparts "$PKG" >/dev/null
fi
if [[ -n "${NOTARY_PROFILE:-}" ]]; then
    say "Notarizing (Apple)"
    xcrun notarytool submit "$PKG" --keychain-profile "$NOTARY_PROFILE" --wait
    xcrun stapler staple "$PKG"
fi

# --- 5. Matching source code -----------------------------------------------------
SRC="build/StudioRider-$SHOWN-source.tar.gz"
if git rev-parse --is-inside-work-tree >/dev/null 2>&1 && [[ -z "$(git status --porcelain 2>/dev/null)" ]]; then
    git archive --format=tar.gz --prefix="StudioRider-$SHOWN/" -o "$SRC" HEAD
else
    COPYFILE_DISABLE=1 tar -czf "$SRC" -s ",^\.,StudioRider-$SHOWN," \
        --exclude ./build --exclude './build-*' --exclude ./JUCE --exclude ./third_party \
        --exclude ./.tools --exclude ./.git --exclude .DS_Store --exclude '*.pkg' .
fi

say "Done"
echo "Installer:   $PKG ($(du -h "$PKG" | cut -f1 | tr -d ' '))"
echo "Source code: $SRC"
echo "Installs StudioRider $SHOWN (AU + VST3, Apple Silicon + Intel, macOS $MIN_MACOS+) and cloudflared $CF_VERSION."
pkgutil --check-signature "$PKG" 2>/dev/null | sed -n 2p || true
