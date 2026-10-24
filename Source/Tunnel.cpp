// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The StudioWire authors
//
#include "Tunnel.h"

#include <regex>
#include <string>

namespace
{
    juce::String lastLines (const std::string& text, int count)
    {
        auto lines = juce::StringArray::fromLines (juce::String (text));
        lines.removeEmptyStrings();
        juce::StringArray tail;
        for (int i = juce::jmax (0, lines.size() - count); i < lines.size(); ++i)
            tail.add (lines[i].trim());
        return tail.joinIntoString (" / ").substring (0, 400);
    }
}

Tunnel::Tunnel() : juce::Thread ("StudioWire tunnel") {}

Tunnel::~Tunnel()
{
    stop();
}

bool Tunnel::start (const juce::File& executable, int localPort, juce::String& error)
{
    stop();

    if (! executable.existsAsFile())
    {
        error = "cloudflared not found.";
        return false;
    }

    juce::StringArray args { executable.getFullPathName(),
                             "tunnel", "--no-autoupdate",
                             "--url", "http://127.0.0.1:" + juce::String (localPort) };

    {
        const juce::ScopedLock sl (lock);
        state = State::starting;
        url.clear();
        log.clear();
        message = juce::String::fromUTF8 ("Starting the tunnel…");
    }

    if (! process.start (args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
    {
        const juce::ScopedLock sl (lock);
        state = State::failed;
        message = error = "Could not start cloudflared.";
        return false;
    }

    startThread();
    return true;
}

void Tunnel::stop()
{
    signalThreadShouldExit();
    if (process.isRunning())
        process.kill();
    stopThread (4000);

    const juce::ScopedLock sl (lock);
    state = State::idle;
    url.clear();
    message.clear();
}

Tunnel::State Tunnel::getState() const        { const juce::ScopedLock sl (lock); return state; }
juce::String Tunnel::getPublicUrl() const     { const juce::ScopedLock sl (lock); return url; }
juce::String Tunnel::getMessage() const       { const juce::ScopedLock sl (lock); return message; }

void Tunnel::run()
{
    static const std::regex urlPattern (R"(https://[a-z0-9-]+\.trycloudflare\.com)");
    std::string output, line;
    char c = 0;

    while (! threadShouldExit())
    {
        // Read byte by byte: the output is buffered by stdio, and we don't want
        // to wait for a large block to fill up before we see the URL.
        const int n = process.readProcessOutput (&c, 1);
        if (n <= 0)
        {
            if (! process.isRunning())
                break;
            wait (50);
            continue;
        }

        output.push_back (c);
        if (output.size() > 32768)
            output.erase (0, output.size() - 16384);

        if (c != '\n')
        {
            line.push_back (c);
            continue;
        }

        std::smatch m;
        if (std::regex_search (line, m, urlPattern))
        {
            const auto found = m.str();
            if (found != "https://api.trycloudflare.com")
            {
                const juce::ScopedLock sl (lock);
                if (url.isEmpty())
                {
                    url = juce::String (found);
                    state = State::ready;
                    message = "Tunnel ready.";
                }
            }
        }
        line.clear();
    }

    if (! threadShouldExit())
    {
        const juce::ScopedLock sl (lock);
        state = State::failed;
        url.clear();
        message = "cloudflared stopped: " + lastLines (output, 2);
    }
}
