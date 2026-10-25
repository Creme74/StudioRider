// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The StudioRider authors
//
// Test tool without JUCE: simulates a DAW's audio thread and feeds the server
// in real time.
//   --pattern sine   : two sines (440 Hz left, 660 Hz right), to listen to
//   --pattern ramp   : deterministic sequence to check bit-exactness
// Example: studiorider_harness --port 47800 --token test --seconds 60

#include "StreamServer.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static std::atomic<bool> stopFlag { false };
static void onSignal (int) { stopFlag = true; }

int main (int argc, char** argv)
{
    int port = 47800, seconds = 0, block = 512;
    bool dither = true;
    double sr = 48000.0;
    std::string token, pattern = "sine", maxQ = "cd", schedule;

    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string (argv[++i]) : std::string(); };
        if (a == "--port") port = std::atoi (next().c_str());
        else if (a == "--token") token = next();
        else if (a == "--seconds") seconds = std::atoi (next().c_str());
        else if (a == "--rate") sr = std::atof (next().c_str());
        else if (a == "--block") block = std::atoi (next().c_str());
        else if (a == "--pattern") pattern = next();
        else if (a == "--no-dither") dither = false;
        else if (a == "--max") maxQ = next();
        else if (a == "--max-schedule") schedule = next(); // e.g. "eco@4,hires@8"
    }

    std::signal (SIGINT, onSignal);
    std::signal (SIGTERM, onSignal);

    studiorider::StreamServer server;
    studiorider::StreamServer::Options opts;
    opts.firstPort = port;
    opts.token = token;
    opts.dither = dither;
    studiorider::parseQuality (maxQ.c_str(), opts.maxQuality);

    // Scheduled stream quality changes (to test changing the cap while live).
    std::vector<std::pair<double, studiorider::Quality>> changes;
    for (std::size_t pos = 0; pos < schedule.size();)
    {
        auto comma = schedule.find (',', pos);
        if (comma == std::string::npos) comma = schedule.size();
        const auto item = schedule.substr (pos, comma - pos);
        const auto at = item.find ('@');
        studiorider::Quality q;
        if (at != std::string::npos && studiorider::parseQuality (item.substr (0, at).c_str(), q))
            changes.emplace_back (std::atof (item.substr (at + 1).c_str()), q);
        pos = comma + 1;
    }
    std::size_t nextChange = 0;

    std::string error;
    if (! server.start (opts, error))
    {
        std::fprintf (stderr, "Error: %s\n", error.c_str());
        return 1;
    }
    server.setSampleRate (sr);
    std::printf ("URL %s\n", server.getLocalUrl().c_str());
    std::fflush (stdout);

    std::vector<float> left ((size_t) block), right ((size_t) block);
    const float* chans[2] = { left.data(), right.data() };
    std::uint64_t n = 0;
    const auto period = std::chrono::nanoseconds ((long long) (1e9 * block / sr));
    auto next = std::chrono::steady_clock::now();
    const auto end = next + std::chrono::seconds (seconds > 0 ? seconds : 1000000);
    int lastListeners = -1;
    const auto t0 = std::chrono::steady_clock::now();

    while (! stopFlag && std::chrono::steady_clock::now() < end)
    {
        for (int i = 0; i < block; ++i, ++n)
        {
            if (pattern == "ramp16")
            {
                // Exact 16-bit values: CD mode without dither must return them unchanged.
                left[(size_t) i]  = (float) ((double) ((std::int64_t) (n % 65536u) - 32768) / 32768.0);
                right[(size_t) i] = (float) ((double) ((std::int64_t) ((n * 7919u) % 65536u) - 32768) / 32768.0);
            }
            else if (pattern == "music")
            {
                // More realistic signal: chords + light pink noise, to estimate bitrates.
                const double t = (double) n / sr;
                double v = 0.0;
                static const double f[] = { 110.0, 164.8, 220.0, 277.2, 329.6, 440.0, 659.3, 880.0 };
                for (int k = 0; k < 8; ++k)
                    v += std::sin (2.0 * M_PI * f[k] * t * (1.0 + 0.002 * std::sin (0.3 * t + k))) * (0.12 / (1 + k * 0.4))
                         * (0.6 + 0.4 * std::sin (2.0 * M_PI * (0.5 + 0.13 * k) * t));
                static double b0 = 0, b1 = 0, b2 = 0;
                const double white = (double) std::rand() / RAND_MAX * 2.0 - 1.0;
                b0 = 0.99765 * b0 + white * 0.0990460; b1 = 0.96300 * b1 + white * 0.2965164; b2 = 0.57000 * b2 + white * 1.0526913;
                const double pink = (b0 + b1 + b2 + white * 0.1848) * 0.02;
                left[(size_t) i]  = (float) (v + pink);
                right[(size_t) i] = (float) (v * 0.9 - pink * 0.8 + 0.05 * std::sin (2.0 * M_PI * 3.1 * t));
            }
            else if (pattern == "ramp")
            {
                left[(size_t) i]  = (float) ((double) ((std::int64_t) (n % 16777216u) - 8388608) / 8388608.0);
                right[(size_t) i] = (float) ((double) ((std::int64_t) ((n * 7919u) % 16777216u) - 8388608) / 8388608.0);
            }
            else
            {
                const double t = (double) n / sr;
                left[(size_t) i]  = (float) (0.25 * std::sin (2.0 * M_PI * 440.0 * t));
                right[(size_t) i] = (float) (0.25 * std::sin (2.0 * M_PI * 660.0 * t));
            }
        }
        server.pushAudio (chans, 2, block);

        const double elapsed = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        while (nextChange < changes.size() && elapsed >= changes[nextChange].first)
        {
            server.setMaxQuality (changes[nextChange].second);
            std::printf ("Stream quality: %s (%.1f s)\n", studiorider::qualityId (changes[nextChange].second), elapsed);
            std::fflush (stdout);
            ++nextChange;
        }

        const int nl = server.getListenerCount();
        if (nl != lastListeners)
        {
            std::printf ("Listeners: %d\n", nl);
            std::fflush (stdout);
            lastListeners = nl;
        }

        next += period;
        std::this_thread::sleep_until (next);
    }

    std::printf ("Bytes sent: %llu, frames dropped: %llu\n",
                 (unsigned long long) server.getBytesSent(),
                 (unsigned long long) server.getDroppedFrames());
    server.stop();
    return 0;
}
