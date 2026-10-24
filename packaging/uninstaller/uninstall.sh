#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The StudioWire authors
#
# Run as root by "Uninstall StudioWire.app". Argument: the user's home folder.
# Removes everything the installer put on the Mac, plus cloudflared updates and
# copies of the plugin in the user's own Plug-Ins folder (built with build.sh).
set -u
user_home="${1:-}"

rm -rf "/Library/Audio/Plug-Ins/Components/StudioWire.component" \
       "/Library/Audio/Plug-Ins/VST3/StudioWire.vst3" \
       "/Library/Application Support/StudioWire"

if [[ "$user_home" == /* && -d "$user_home/Library" ]]; then
    rm -rf "$user_home/Library/Audio/Plug-Ins/Components/StudioWire.component" \
           "$user_home/Library/Audio/Plug-Ins/VST3/StudioWire.vst3" \
           "$user_home/Library/Application Support/StudioWire"
fi

/usr/sbin/pkgutil --forget net.studiowire.pkg >/dev/null 2>&1 || true
killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true

# Last: the uninstaller itself.
rm -rf "/Applications/StudioWire"
exit 0
