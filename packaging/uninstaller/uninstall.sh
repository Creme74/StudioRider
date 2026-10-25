#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The StudioRider authors
#
# Run as root by "Uninstall StudioRider.app". Argument: the user's home folder.
# Removes everything the installer put on the Mac, plus cloudflared updates and
# copies of the plugin in the user's own Plug-Ins folder (built with build.sh),
# and what is left of the former name, StudioWire.
set -u
user_home="${1:-}"

rm -rf "/Library/Audio/Plug-Ins/Components/StudioRider.component" \
       "/Library/Audio/Plug-Ins/VST3/StudioRider.vst3" \
       "/Library/Application Support/StudioRider" \
       "/Library/Audio/Plug-Ins/Components/StudioWire.component" \
       "/Library/Audio/Plug-Ins/VST3/StudioWire.vst3" \
       "/Library/Application Support/StudioWire" \
       "/Applications/StudioWire"

if [[ "$user_home" == /* && -d "$user_home/Library" ]]; then
    rm -rf "$user_home/Library/Audio/Plug-Ins/Components/StudioRider.component" \
           "$user_home/Library/Audio/Plug-Ins/VST3/StudioRider.vst3" \
           "$user_home/Library/Application Support/StudioRider" \
           "$user_home/Library/Audio/Plug-Ins/Components/StudioWire.component" \
           "$user_home/Library/Audio/Plug-Ins/VST3/StudioWire.vst3" \
           "$user_home/Library/Application Support/StudioWire"
fi

/usr/sbin/pkgutil --forget net.studiorider.pkg >/dev/null 2>&1 || true
/usr/sbin/pkgutil --forget net.studiowire.pkg >/dev/null 2>&1 || true   # former name
killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true

# Last: the uninstaller itself.
rm -rf "/Applications/StudioRider"
exit 0
