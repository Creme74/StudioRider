#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The StudioRider authors
#
# Double-click this file (or run it in Terminal) to build StudioRider and install it
# for yourself (~/Library/Audio/Plug-Ins). To make the installer for other people,
# use "Make StudioRider Installer.command".
cd "$(dirname "$0")"
mkdir -p build
./build.sh 2>&1 | tee build/run.log
echo "${PIPESTATUS[0]}" > build/run.status
echo
echo "You can close this window."
