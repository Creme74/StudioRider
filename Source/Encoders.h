// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The StudioWire authors
//
// StudioWire — audio encoders (no JUCE dependency).
//
//   cd    : FLAC 16-bit with TPDF dither (CD quality, the default)
//   hires : FLAC 24-bit, lossless relative to the plugin's output
//   eco   : Opus 256 kbps at 48 kHz (lossy, for slow connections)

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace studiowire
{

enum class Quality : int { cd = 0, hires = 1, eco = 2 };
constexpr int kNumQualities = 3;

const char* qualityId (Quality q) noexcept;              // "cd", "hires", "eco"
bool parseQuality (const char* text, Quality& out) noexcept;

// Quality order: eco (0) < cd (1) < hires (2).
constexpr int qualityRank (Quality q) noexcept
{
    return q == Quality::eco ? 0 : q == Quality::cd ? 1 : 2;
}
constexpr Quality qualityFromRank (int r) noexcept
{
    return r <= 0 ? Quality::eco : r == 1 ? Quality::cd : Quality::hires;
}
// A quality above the cap is lowered to the cap.
constexpr Quality clampQuality (Quality q, Quality cap) noexcept
{
    return qualityRank (q) > qualityRank (cap) ? cap : q;
}

enum Codec : std::uint8_t { codecFlac = 2, codecOpus = 3 };

constexpr int kOpusBitrate = 256000;

struct EncodedPacket
{
    std::vector<std::uint8_t> payload; // FLAC: one frame; Opus: [u16 size][packet]…
    std::uint32_t frames = 0;          // samples per channel contained
};

class Encoder
{
public:
    virtual ~Encoder() = default;

    // Adds samples (stereo, non-interleaved); complete packets are
    // appended to "out".
    virtual void encode (const float* left, const float* right, int numFrames,
                         std::vector<EncodedPacket>& out) = 0;

    virtual Codec codec() const noexcept = 0;
    virtual std::uint32_t outputRate() const noexcept = 0;
    virtual int bitsPerSample() const noexcept = 0;   // 0 for Opus
    virtual int preSkip() const noexcept { return 0; }
};

// Returns nullptr (and fills in "error") on failure.
std::unique_ptr<Encoder> makeEncoder (Quality q, std::uint32_t sampleRate, bool dither, std::string& error);

} // namespace studiowire
