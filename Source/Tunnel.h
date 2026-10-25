// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The StudioRider authors
//
// StudioRider — starts a Cloudflare "Quick Tunnel" (cloudflared) to the local
// server and picks up the public address https://….trycloudflare.com.

#pragma once

#include <juce_core/juce_core.h>

class Tunnel : private juce::Thread
{
public:
    enum class State { idle, starting, ready, failed };

    Tunnel();
    ~Tunnel() override;

    bool start (const juce::File& executable, int localPort, juce::String& error);
    void stop();

    State getState() const;
    juce::String getPublicUrl() const;   // without a trailing "/"
    juce::String getMessage() const;

private:
    void run() override;

    juce::ChildProcess process;
    mutable juce::CriticalSection lock;
    State state = State::idle;
    juce::String url, message, log;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Tunnel)
};
