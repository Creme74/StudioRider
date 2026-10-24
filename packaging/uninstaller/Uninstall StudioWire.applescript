-- SPDX-License-Identifier: GPL-3.0-or-later
-- Copyright (C) 2026 The StudioWire authors
--
-- "Uninstall StudioWire" app, installed in /Applications/StudioWire by the installer.
-- package.sh compiles it with osacompile and puts uninstall.sh in its Resources.
-- (Plain ASCII on purpose.)

set dialogTitle to "Uninstall StudioWire"
set question to "Remove StudioWire from this Mac?" & return & return & "This removes the AU and VST3 plugins, cloudflared and this uninstaller. Your DAW projects are not touched."
display dialog question with title dialogTitle buttons {"Cancel", "Uninstall"} default button "Uninstall" cancel button "Cancel" with icon caution

set scriptPath to (POSIX path of (path to me)) & "Contents/Resources/uninstall.sh"
set homePath to POSIX path of (path to home folder)
try
	do shell script "/bin/bash " & quoted form of scriptPath & " " & quoted form of homePath with prompt "StudioWire needs your password to remove its files." with administrator privileges
on error errorMessage number errorNumber
	if errorNumber is -128 then return -- password dialog cancelled
	display dialog "StudioWire could not be removed:" & return & errorMessage with title dialogTitle buttons {"OK"} default button "OK" with icon stop
	return
end try

display dialog "StudioWire has been removed." & return & return & "If your DAW is open, quit and reopen it." with title dialogTitle buttons {"OK"} default button "OK"
