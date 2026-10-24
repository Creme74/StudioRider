// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The StudioWire authors
// Checks the cloudflared finder and updater against a local fake release server
// (started by tests/cloudflared_test.sh, which also sets the STUDIOWIRE_* variables).
#include "Cloudflared.h"
#include <cstdlib>
#include <iostream>

static int fails = 0;
static void check (bool c, const juce::String& m)
{
    std::cout << (c ? "OK   " : "FAIL ") << m << "\n";
    if (! c) ++fails;
}

static void waitIdle (Cloudflared& cf)
{
    juce::Thread::sleep (100);
    for (int i = 0; i < 300 && cf.getActivity() != Cloudflared::Activity::idle; ++i)
        juce::Thread::sleep (100);
}

int main()
{
    const auto api = juce::SystemStats::getEnvironmentVariable ("STUDIOWIRE_TEST_SERVER", {});
    const auto support = Cloudflared::userDir();

    Cloudflared cf;
    waitIdle (cf);
    auto b = cf.getCurrent();
    check (b.source == "bundled" && b.version == "2025.1.0", "finds the bundled copy: " + b.version + " (" + b.source + ")");
    check (cf.isGettingOld(), "a 2025.1 version counts as getting old");
    check (! cf.isUpdateAvailable(), "no update known before checking");

    ::setenv ("STUDIOWIRE_CLOUDFLARED_API", (api + "/good").toRawUTF8(), 1);
    cf.checkForUpdate();
    waitIdle (cf);
    check (cf.getLatestVersion() == "2099.1.0" && cf.isUpdateAvailable(), "check: 2099.1.0 available - " + cf.getMessage());

    cf.installUpdate();
    waitIdle (cf);
    b = cf.getCurrent();
    check (b.source == "updated" && b.version == "2099.1.0", "install: now using " + b.version + " (" + b.source + ") - " + cf.getMessage());
    check (support.getChildFile ("cloudflared").existsAsFile() && ! support.getChildFile (".download").exists(),
           "installed into the support folder, download folder cleaned up");
    check (cf.getExecutable() == support.getChildFile ("cloudflared"), "the updated copy is the one that will run");

    ::setenv ("STUDIOWIRE_CLOUDFLARED_API", (api + "/tgzsha").toRawUTF8(), 1);
    cf.checkForUpdate();
    waitIdle (cf);
    cf.installUpdate();
    waitIdle (cf);
    b = cf.getCurrent();
    check (b.version == "2099.2.0", "checksum of the .tgz in the notes, no GitHub digest: accepted - " + cf.getMessage());

    ::setenv ("STUDIOWIRE_CLOUDFLARED_API", (api + "/bad").toRawUTF8(), 1);
    cf.checkForUpdate();
    waitIdle (cf);
    cf.installUpdate();
    waitIdle (cf);
    b = cf.getCurrent();
    check (cf.getMessage().contains ("Cloudflare's checksum") && b.version == "2099.2.0",
           "wrong checksum in the notes: rejected, previous copy kept - " + cf.getMessage());

    ::setenv ("STUDIOWIRE_CLOUDFLARED_API", (api + "/baddigest").toRawUTF8(), 1);
    cf.checkForUpdate();
    waitIdle (cf);
    cf.installUpdate();
    waitIdle (cf);
    b = cf.getCurrent();
    check (cf.getMessage().contains ("GitHub's checksum") && b.version == "2099.2.0"
               && ! support.getChildFile (".download").exists(),
           "wrong GitHub digest: rejected, download cleaned up - " + cf.getMessage());

    ::setenv ("STUDIOWIRE_CLOUDFLARED_API", (api + "/nosha").toRawUTF8(), 1);
    cf.checkForUpdate();
    waitIdle (cf);
    check (cf.getMessage().contains ("no published checksum"), "no checksum published: refused - " + cf.getMessage());

    ::setenv ("STUDIOWIRE_CLOUDFLARED_API", (api + "/missing").toRawUTF8(), 1);
    cf.checkForUpdate();
    waitIdle (cf);
    check (cf.getMessage().contains ("error 404"), "server error reported - " + cf.getMessage());

    check (Cloudflared::compareVersions ("2026.10.3", "2026.9.12") > 0 && Cloudflared::compareVersions ("2026.1.0", "2026.1.0") == 0,
           "version comparison");

    std::cout << (fails ? "Failures\n" : "All tests pass.\n");
    return fails;
}
