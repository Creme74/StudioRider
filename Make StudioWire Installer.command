#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The StudioWire authors
#
# Double-click this file (or run it in Terminal) to make the StudioWire installer
# (build/StudioWire-<version>.pkg): AU + VST3 for Apple Silicon and Intel Macs,
# with cloudflared included.
cd "$(dirname "$0")"
mkdir -p build
./package.sh 2>&1 | tee build/package.log
echo "${PIPESTATUS[0]}" > build/package.status
echo
echo "You can close this window."
