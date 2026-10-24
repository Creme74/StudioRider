// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The StudioWire authors
//
// Checks that Tunnel starts the binary, reads its output as it comes,
// extracts the trycloudflare URL and stops cleanly.
#include "Tunnel.h"
#include <iostream>

int main (int argc, char** argv)
{
    if (argc < 2) { std::cerr << "usage: tunnel_test <fake-cloudflared>\n"; return 2; }
    Tunnel t;
    juce::String err;
    if (! t.start (juce::File (argv[1]), 47800, err)) { std::cerr << "start: " << err << "\n"; return 1; }
    const auto t0 = juce::Time::getMillisecondCounter();
    while (t.getState() == Tunnel::State::starting && juce::Time::getMillisecondCounter() - t0 < 10000)
        juce::Thread::sleep (20);
    const auto ms = juce::Time::getMillisecondCounter() - t0;
    std::cout << "state=" << (int) t.getState() << " url=" << t.getPublicUrl() << " after " << ms << " ms\n";
    const bool ok = t.getState() == Tunnel::State::ready
                 && t.getPublicUrl() == "https://kingston-inside-best-graphic.trycloudflare.com";
    const auto s0 = juce::Time::getMillisecondCounter();
    t.stop();
    std::cout << "stop took " << (juce::Time::getMillisecondCounter() - s0) << " ms, state=" << (int) t.getState() << "\n";
    // Failure case: a binary that exits straight away
    if (argc > 2)
    {
        t.start (juce::File (argv[2]), 47800, err);
        juce::Thread::sleep (1500);
        std::cout << "fail-case state=" << (int) t.getState() << " msg=" << t.getMessage() << "\n";
        t.stop();
    }
    std::cout << (ok ? "OK" : "FAIL") << "\n";
    return ok ? 0 : 1;
}
