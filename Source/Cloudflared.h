// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The StudioWire authors
// StudioWire — finds the cloudflared program, reports its version, and updates it
// from Cloudflare's official GitHub releases when asked to.
//
// Where cloudflared can come from, newest version wins:
//   - "updated"  : ~/Library/Application Support/StudioWire/cloudflared (written by the updater)
//   - "bundled"  : /Library/Application Support/StudioWire/cloudflared (installed by the PKG)
//   - "Homebrew" : /opt/homebrew/bin, /usr/local/bin, … and the PATH
// A file chosen by hand ("custom") always wins.
//
// Nothing here touches the network unless checkForUpdate() / installUpdate() is called.

#pragma once

#include <juce_core/juce_core.h>

class Cloudflared : private juce::Thread
{
public:
    struct Binary
    {
        juce::File file;
        juce::String version;   // e.g. "2026.10.3", empty if unknown
        juce::String source;    // "updated", "bundled", "Homebrew", "custom"
    };

    enum class Activity { idle, scanning, checking, downloading };

    Cloudflared();
    ~Cloudflared() override;

    static juce::File userDir();      // ~/Library/Application Support/StudioWire
    static juce::File bundledDir();   // /Library/Application Support/StudioWire

    // Chosen by hand in the plugin window (empty = automatic).
    void setOverride (const juce::File& f);
    juce::File getOverride() const;

    // Looks for every copy and runs "cloudflared --version" on each (in the background).
    void rescan();

    // Asks GitHub for the latest release (in the background).
    void checkForUpdate();

    // Downloads the latest release, checks its SHA-256 against the release notes,
    // and installs it into userDir() (in the background). Checks first if needed.
    void installUpdate();

    Binary getCurrent() const;            // file is empty if none was found
    juce::File getExecutable() const;     // best file to run right now
    Activity getActivity() const;
    juce::String getMessage() const;      // last result or error, for the UI
    juce::String getLatestVersion() const; // empty until a check succeeded
    bool isUpdateAvailable() const;
    bool isGettingOld() const;            // older than about 6 months
    float getProgress() const;            // 0..1 while downloading

    // Version strings look like "2026.10.3": returns <0, 0 or >0.
    static int compareVersions (const juce::String& a, const juce::String& b);
    static juce::String readVersion (const juce::File& exe);

private:
    enum class Job { none, scan, check, install };

    void run() override;
    void startJob (Job);
    void doScan();
    bool doCheck();
    void doInstall();
    void setMessage (const juce::String&);

    mutable juce::CriticalSection lock;
    Binary current;
    juce::File override;
    juce::String message, latestVersion, latestUrl, assetName;
    juce::String latestSha256;   // published by Cloudflare in the release notes (of the archive or of the program inside)
    juce::String latestDigest;   // computed by GitHub for the archive ("digest" field of the asset), if given
    Activity activity = Activity::idle;
    Job pending = Job::none;
    std::atomic<float> progress { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Cloudflared)
};
