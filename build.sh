#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The StudioRider authors
#
# Builds StudioRider (AU + VST3) for this Mac and installs it for the current user.
# To make the installer (.pkg) for other people, use ./package.sh instead.
set -euo pipefail
cd "$(dirname "$0")"
source scripts/common.sh

check_tools
fetch_dependencies

mkdir -p build
if [[ -f build/CMakeCache.txt ]]; then
    # Layout of earlier versions (CMake directly in build/): clear it, keep this run's log.
    find build -mindepth 1 -maxdepth 1 ! -name run.log ! -name run.status -exec rm -rf {} +
fi

CMAKE_DIR=build/cmake
cmake_build "$CMAKE_DIR" "$(uname -m)" -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN_MACOS"

PLUGINS="$HOME/Library/Audio/Plug-Ins"
AU="$PLUGINS/Components/StudioRider.component"
VST3="$PLUGINS/VST3/StudioRider.vst3"

say "Installing and signing locally"
mkdir -p "$(dirname "$AU")" "$(dirname "$VST3")"
# The former names (StudioWire, MixShare) use the same plugin codes: remove them so the DAW doesn't see it twice.
rm -rf "$PLUGINS/Components/StudioWire.component" "$PLUGINS/VST3/StudioWire.vst3" \
       "$PLUGINS/Components/MixShare.component" "$PLUGINS/VST3/MixShare.vst3"
rm -rf "$AU" "$VST3"
cp -R "$CMAKE_DIR/StudioRider_artefacts/Release/AU/StudioRider.component" "$AU"
cp -R "$CMAKE_DIR/StudioRider_artefacts/Release/VST3/StudioRider.vst3" "$VST3"
codesign --force --deep --sign - "$AU" >/dev/null 2>&1
codesign --force --deep --sign - "$VST3" >/dev/null 2>&1
echo "AU:   $AU"
echo "VST3: $VST3"
if [[ -d "/Library/Audio/Plug-Ins/Components/StudioRider.component" ]]; then
    echo "Note: StudioRider is also installed for all users (from the installer). Your DAW may see two copies."
fi

say "Validating the Audio Unit (auval)"
killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true
sleep 2
if auval -v aufx Mxs1 Mxsr > build/auval.log 2>&1 && grep -q "AU VALIDATION SUCCEEDED" build/auval.log; then
    echo "auval: AU VALIDATION SUCCEEDED"
else
    tail -25 build/auval.log
    die "auval failed (details in build/auval.log)."
fi

say "Done"
echo "In your DAW: insert StudioRider last on the master track."
echo "If it doesn't show up, make your DAW rescan its plugins."
echo "If cloudflared is missing, the plugin offers to download it (Download cloudflared)."
