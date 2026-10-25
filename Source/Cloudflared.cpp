// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The StudioRider authors
#include "Cloudflared.h"

#include <juce_cryptography/juce_cryptography.h>

#include <regex>

namespace
{
    // Test hooks: let the tests use a fake release server and scratch folders.
    juce::String env (const char* name, const juce::String& fallback)
    {
        const auto v = juce::SystemStats::getEnvironmentVariable (name, {});
        return v.isNotEmpty() ? v : fallback;
    }

    juce::String releaseApiUrl()
    {
        return env ("STUDIORIDER_CLOUDFLARED_API", "https://api.github.com/repos/cloudflare/cloudflared/releases/latest");
    }

    // Name of the release file for this computer.
    juce::String defaultAssetName()
    {
       #if JUCE_MAC
        #if defined (__aarch64__) || defined (__arm64__)
         return "cloudflared-darwin-arm64.tgz";
        #else
         return "cloudflared-darwin-amd64.tgz";
        #endif
       #elif JUCE_LINUX
        #if defined (__aarch64__)
         return "cloudflared-linux-arm64";
        #else
         return "cloudflared-linux-amd64";
        #endif
       #else
        return "cloudflared-windows-amd64.exe";
       #endif
    }

    const char* exeName()
    {
       #if JUCE_WINDOWS
        return "cloudflared.exe";
       #else
        return "cloudflared";
       #endif
    }

    bool runAndWait (const juce::StringArray& args, int timeoutMs, juce::String* output = nullptr)
    {
        juce::ChildProcess p;
        if (! p.start (args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
            return false;
        if (! p.waitForProcessToFinish (timeoutMs))
        {
            p.kill();
            return false;
        }
        if (output != nullptr)
            *output = p.readAllProcessOutput();
        return p.getExitCode() == 0;
    }

    std::unique_ptr<juce::InputStream> openUrl (const juce::String& address, int timeoutMs, int* status)
    {
        const auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                              .withExtraHeaders ("Accept: application/vnd.github+json\r\nUser-Agent: StudioRider")
                              .withConnectionTimeoutMs (timeoutMs)
                              .withNumRedirectsToFollow (10)
                              .withStatusCode (status);
        return juce::URL (address).createInputStream (opts);
    }

    juce::String escapeRegex (const juce::String& s)
    {
        juce::String out;
        for (auto c : s)
        {
            if (juce::String ("\\^$.|?*+()[]{}").containsChar (c))
                out << '\\';
            out << c;
        }
        return out;
    }
}

//==============================================================================
Cloudflared::Cloudflared() : juce::Thread ("StudioRider cloudflared")
{
    assetName = env ("STUDIORIDER_CLOUDFLARED_ASSET", defaultAssetName());
    startThread();   // waits for jobs for the whole lifetime of the plugin
    rescan();
}

Cloudflared::~Cloudflared()
{
    signalThreadShouldExit();
    notify();
    stopThread (10000);
}

juce::File Cloudflared::userDir()
{
    const auto custom = juce::SystemStats::getEnvironmentVariable ("STUDIORIDER_SUPPORT_DIR", {});
    if (custom.isNotEmpty())
        return juce::File (custom);
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("StudioRider");
}

juce::File Cloudflared::bundledDir()
{
    const auto custom = juce::SystemStats::getEnvironmentVariable ("STUDIORIDER_BUNDLED_DIR", {});
    if (custom.isNotEmpty())
        return juce::File (custom);
   #if JUCE_MAC
    return juce::File ("/Library/Application Support/StudioRider");
   #else
    return juce::File::getSpecialLocation (juce::File::commonApplicationDataDirectory).getChildFile ("StudioRider");
   #endif
}

void Cloudflared::setOverride (const juce::File& f)
{
    {
        const juce::ScopedLock sl (lock);
        override = f;
    }
    rescan();
}

juce::File Cloudflared::getOverride() const        { const juce::ScopedLock sl (lock); return override; }
Cloudflared::Binary Cloudflared::getCurrent() const { const juce::ScopedLock sl (lock); return current; }
Cloudflared::Activity Cloudflared::getActivity() const { const juce::ScopedLock sl (lock); return activity; }
juce::String Cloudflared::getMessage() const       { const juce::ScopedLock sl (lock); return message; }
juce::String Cloudflared::getLatestVersion() const { const juce::ScopedLock sl (lock); return latestVersion; }
float Cloudflared::getProgress() const             { return progress.load(); }

void Cloudflared::setMessage (const juce::String& m)
{
    const juce::ScopedLock sl (lock);
    message = m;
}

bool Cloudflared::isUpdateAvailable() const
{
    const juce::ScopedLock sl (lock);
    if (latestVersion.isEmpty())
        return false;
    return current.file == juce::File() || current.version.isEmpty()
        || compareVersions (latestVersion, current.version) > 0;
}

bool Cloudflared::isGettingOld() const
{
    const auto v = getCurrent().version;
    const auto parts = juce::StringArray::fromTokens (v, ".", {});
    if (parts.size() < 2)
        return false;
    const auto now = juce::Time::getCurrentTime();
    const int ageMonths = (now.getYear() * 12 + now.getMonth() + 1) - (parts[0].getIntValue() * 12 + parts[1].getIntValue());
    return ageMonths > 6;
}

juce::File Cloudflared::getExecutable() const
{
    {
        const juce::ScopedLock sl (lock);
        if (current.file.existsAsFile())
            return current.file;
        if (override.existsAsFile())
            return override;
    }

    // Scan not finished yet: take the first copy found, without checking its version.
    for (const auto& f : { userDir().getChildFile (exeName()), bundledDir().getChildFile (exeName()) })
        if (f.existsAsFile())
            return f;

    const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    juce::Array<juce::File> dirs { juce::File ("/opt/homebrew/bin"), juce::File ("/usr/local/bin"),
                                   home.getChildFile (".homebrew/bin"), home.getChildFile ("homebrew/bin") };
    for (const auto& d : juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("PATH", {}), ":", {}))
        if (juce::File::isAbsolutePath (d))
            dirs.add (juce::File (d));
    for (const auto& d : dirs)
        if (d.getChildFile (exeName()).existsAsFile())
            return d.getChildFile (exeName());
    return {};
}

int Cloudflared::compareVersions (const juce::String& a, const juce::String& b)
{
    const auto pa = juce::StringArray::fromTokens (a.trimCharactersAtStart ("v"), ".", {});
    const auto pb = juce::StringArray::fromTokens (b.trimCharactersAtStart ("v"), ".", {});
    for (int i = 0; i < juce::jmax (pa.size(), pb.size()); ++i)
    {
        const int x = pa[i].getIntValue(), y = pb[i].getIntValue();
        if (x != y)
            return x < y ? -1 : 1;
    }
    return 0;
}

juce::String Cloudflared::readVersion (const juce::File& exe)
{
    if (! exe.existsAsFile())
        return {};
    juce::String out;
    if (! runAndWait ({ exe.getFullPathName(), "--version" }, 5000, &out))
        return {};
    static const std::regex pattern (R"(version\s+v?([0-9]+\.[0-9]+\.[0-9]+))");
    std::smatch m;
    const auto text = out.toStdString();
    return std::regex_search (text, m, pattern) ? juce::String (m[1].str()) : juce::String();
}

//==============================================================================
void Cloudflared::rescan()          { startJob (Job::scan); }
void Cloudflared::checkForUpdate()  { startJob (Job::check); }
void Cloudflared::installUpdate()   { startJob (Job::install); }

void Cloudflared::startJob (Job job)
{
    {
        const juce::ScopedLock sl (lock);
        const bool busy = activity == Activity::checking || activity == Activity::downloading;
        if (busy && job != Job::scan)
            return;                 // a check or a download is already running
        if (pending == Job::none || job != Job::scan)
            pending = job;          // a rescan never replaces a check or an install
        if (job == Job::check)   activity = Activity::checking;
        if (job == Job::install) activity = Activity::downloading;
    }
    notify();
}

void Cloudflared::run()
{
    while (! threadShouldExit())
    {
        Job job;
        {
            const juce::ScopedLock sl (lock);
            job = pending;
            pending = Job::none;
            if (job == Job::none)
                activity = Activity::idle;
        }

        if (job == Job::none)
        {
            wait (-1);
            continue;
        }

        if (job == Job::scan)          doScan();
        else if (job == Job::check)    doCheck();
        else if (job == Job::install)  doInstall();
    }
}

void Cloudflared::doScan()
{
    {
        const juce::ScopedLock sl (lock);
        if (activity == Activity::idle)
            activity = Activity::scanning;
    }

    const auto chosen = getOverride();
    Binary best;

    if (chosen.existsAsFile())
    {
        best = { chosen, readVersion (chosen), "custom" };
    }
    else
    {
        struct Candidate { juce::File file; const char* source; };
        juce::Array<Candidate> candidates;
        candidates.add ({ userDir().getChildFile (exeName()), "updated" });
        if (juce::SystemStats::getEnvironmentVariable ("STUDIORIDER_SUPPORT_DIR", {}).isEmpty())   // update made under the former name
            candidates.add ({ juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                                  .getChildFile ("StudioWire").getChildFile (exeName()), "updated" });
        candidates.add ({ bundledDir().getChildFile (exeName()), "bundled" });

        const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
        juce::Array<juce::File> dirs { juce::File ("/opt/homebrew/bin"), juce::File ("/usr/local/bin"),
                                       home.getChildFile (".homebrew/bin"), home.getChildFile ("homebrew/bin") };
        for (const auto& d : juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("PATH", {}), ":", {}))
            if (juce::File::isAbsolutePath (d))
                dirs.add (juce::File (d));
        for (const auto& d : dirs)
            candidates.add ({ d.getChildFile (exeName()), "Homebrew" });

        juce::StringArray seen;
        for (const auto& c : candidates)
        {
            if (threadShouldExit())
                return;
            if (! c.file.existsAsFile())
                continue;
            const auto real = c.file.getLinkedTarget().getFullPathName();
            if (seen.contains (real))
                continue;
            seen.add (real);

            const auto v = readVersion (c.file);
            // Newest wins; on a tie, the earlier source (updated > bundled > Homebrew) stays.
            if (best.file == juce::File()
                || (v.isNotEmpty() && (best.version.isEmpty() || compareVersions (v, best.version) > 0)))
                best = { c.file, v, c.source };
        }
    }

    const juce::ScopedLock sl (lock);
    current = best;
}

bool Cloudflared::doCheck()
{
    {
        const juce::ScopedLock sl (lock);
        activity = Activity::checking;
        message = juce::String::fromUTF8 ("Checking for updates…");
    }

    int status = 0;
    auto in = openUrl (releaseApiUrl(), 15000, &status);
    if (in == nullptr)
    {
        setMessage ("Could not reach GitHub. Check your Internet connection.");
        return false;
    }
    const auto body = in->readEntireStreamAsString();
    if (status != 200)
    {
        setMessage ("GitHub answered with error " + juce::String (status) + ". Try again later.");
        return false;
    }

    const auto json = juce::JSON::parse (body);
    const auto tag = json["tag_name"].toString().trimCharactersAtStart ("v");
    juce::String url, digest;
    if (const auto* assets = json["assets"].getArray())
        for (const auto& a : *assets)
            if (a["name"].toString() == assetName)
            {
                url = a["browser_download_url"].toString();
                const auto d = a["digest"].toString().trim();
                if (d.startsWithIgnoreCase ("sha256:") && d.length() == 7 + 64)
                    digest = d.substring (7).toLowerCase();
            }

    // Cloudflare's release notes list "file: sha256" lines (for the .tgz files, the
    // checksum is the one of the program inside). GitHub also gives the archive's own
    // checksum. Anything that can't be verified is refused.
    juce::String sha;
    const auto notes = json["body"].toString().toStdString();
    const std::regex shaPattern (escapeRegex (assetName).toStdString() + R"(\s*:\s*([0-9a-fA-F]{64}))");
    std::smatch m;
    if (std::regex_search (notes, m, shaPattern))
        sha = juce::String (m[1].str()).toLowerCase();

    if (tag.isEmpty() || url.isEmpty())
    {
        setMessage ("The latest cloudflared release has no download for this computer.");
        return false;
    }
    if (sha.isEmpty() && digest.isEmpty())
    {
        setMessage ("The latest cloudflared release (" + tag + ") has no published checksum, so it won't be installed.");
        return false;
    }

    {
        const juce::ScopedLock sl (lock);
        latestVersion = tag;
        latestUrl = url;
        latestSha256 = sha;
        latestDigest = digest;
    }

    setMessage (isUpdateAvailable() ? "cloudflared " + tag + " is available."
                                    : "cloudflared is up to date (" + tag + ").");
    return true;
}

void Cloudflared::doInstall()
{
    if (getLatestVersion().isEmpty() && ! doCheck())
        return;

    if (! isUpdateAvailable())
    {
        setMessage ("cloudflared is already up to date (" + getCurrent().version + ").");
        return;
    }

    juce::String url, sha, digest, version;
    {
        const juce::ScopedLock sl (lock);
        activity = Activity::downloading;
        url = latestUrl;
        sha = latestSha256;
        digest = latestDigest;
        version = latestVersion;
    }
    progress = 0.0f;
    setMessage (juce::String::fromUTF8 ("Downloading cloudflared ") + version + juce::String::fromUTF8 ("…"));

    const auto work = userDir().getChildFile (".download");
    work.deleteRecursively();
    if (! work.createDirectory())
    {
        setMessage ("Could not create " + work.getFullPathName() + ".");
        return;
    }
    auto fail = [&] (const juce::String& why) { work.deleteRecursively(); setMessage (why); };

    // 1. Download
    const auto archive = work.getChildFile (assetName);
    {
        int status = 0;
        auto in = openUrl (url, 30000, &status);
        if (in == nullptr || status != 200)
            return fail ("The download failed (" + juce::String (status) + ").");

        juce::FileOutputStream out (archive);
        if (! out.openedOk())
            return fail ("Could not write " + archive.getFullPathName() + ".");

        const auto total = in->getTotalLength();
        juce::HeapBlock<char> buffer (1 << 16);
        juce::int64 written = 0;
        while (! in->isExhausted())
        {
            if (threadShouldExit())
                return fail ("Download cancelled.");
            const int n = in->read (buffer, 1 << 16);
            if (n < 0)
                return fail ("The download was interrupted.");
            if (n == 0)
                break;
            out.write (buffer, (size_t) n);
            written += n;
            if (total > 0)
                progress = (float) ((double) written / (double) total);
        }
        out.flush();
    }

    // 2. The archive must match GitHub's checksum (when GitHub gives one)
    const auto archiveSha = juce::SHA256 (archive).toHexString().toLowerCase();
    if (digest.isNotEmpty() && archiveSha != digest)
        return fail ("The download doesn't match GitHub's checksum, so it was discarded.");

    // 3. Unpack
    auto exe = work.getChildFile (exeName());
    if (assetName.endsWithIgnoreCase (".tgz"))
    {
        if (! runAndWait ({ "/usr/bin/tar", "-xzf", archive.getFullPathName(), "-C", work.getFullPathName() }, 60000)
            || ! exe.existsAsFile())
            return fail ("Could not unpack the download.");
    }
    else if (! archive.moveFileTo (exe))
    {
        return fail ("Could not prepare the download.");
    }
    // 3b. ...and the archive or the program inside must match Cloudflare's checksum
    if (sha.isNotEmpty() && archiveSha != sha && juce::SHA256 (exe).toHexString().toLowerCase() != sha)
        return fail ("The download doesn't match Cloudflare's checksum, so it was discarded.");

    exe.setExecutePermission (true);
   #if JUCE_MAC
    runAndWait ({ "/usr/bin/xattr", "-d", "com.apple.quarantine", exe.getFullPathName() }, 5000); // usually absent
   #endif

    // 4. Make sure it runs before replacing anything
    const auto got = readVersion (exe);
    if (got.isEmpty())
        return fail ("The downloaded cloudflared doesn't run on this version of macOS, so the current copy is kept.");

    const auto target = userDir().getChildFile (exeName());
    target.deleteFile();
    if (! exe.moveFileTo (target))
        return fail ("Could not install cloudflared into " + userDir().getFullPathName() + ".");
    target.setExecutePermission (true);
    work.deleteRecursively();

    {
        const juce::ScopedLock sl (lock);
        override = juce::File(); // the updated copy is used from now on
    }
    doScan();
    setMessage ("cloudflared updated to " + got + ". It will be used the next time you start sharing.");
}
