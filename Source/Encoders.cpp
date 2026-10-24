// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The StudioWire authors
//
#include "Encoders.h"

#include <FLAC/stream_encoder.h>
#include <opus.h>
#include "speex_resampler.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace studiowire
{

const char* qualityId (Quality q) noexcept
{
    switch (q)
    {
        case Quality::cd:    return "cd";
        case Quality::hires: return "hires";
        case Quality::eco:   return "eco";
    }
    return "cd";
}

bool parseQuality (const char* text, Quality& out) noexcept
{
    if (text == nullptr) return false;
    if (std::strcmp (text, "cd") == 0)    { out = Quality::cd;    return true; }
    if (std::strcmp (text, "hires") == 0) { out = Quality::hires; return true; }
    if (std::strcmp (text, "eco") == 0)   { out = Quality::eco;   return true; }
    return false;
}

namespace
{
    // Fast pseudo-random generator for the dither (xorshift64*).
    struct Rng
    {
        std::uint64_t s;
        explicit Rng (std::uint64_t seed) : s (seed ? seed : 0x9E3779B97F4A7C15ull) {}
        inline double next01() noexcept
        {
            s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
            return (double) ((s * 2685821657736338717ull) >> 11) * (1.0 / 9007199254740992.0);
        }
    };

    inline std::int32_t quantize (double v, std::int32_t lo, std::int32_t hi) noexcept
    {
        if (! (v == v)) return 0; // NaN
        const auto r = std::lrint (v);
        return (std::int32_t) std::clamp<long> (r, lo, hi);
    }

    //==========================================================================
    class FlacEncoder final : public Encoder
    {
    public:
        FlacEncoder (std::uint32_t sr, int bitsIn, bool ditherIn)
            : rate (sr), bits (bitsIn), dither (ditherIn && bitsIn < 24),
              rngL (0x1234567ull ^ sr), rngR (0x89abcdefull ^ sr)
        {
        }

        ~FlacEncoder() override
        {
            if (enc != nullptr)
            {
                sink = nullptr;
                FLAC__stream_encoder_delete (enc); // no finish(): nothing left to write
            }
        }

        bool init (std::string& error)
        {
            enc = FLAC__stream_encoder_new();
            if (enc == nullptr) { error = "FLAC: out of memory"; return false; }

            bool ok = true;
            ok &= FLAC__stream_encoder_set_channels (enc, 2) != 0;
            ok &= FLAC__stream_encoder_set_bits_per_sample (enc, (unsigned) bits) != 0;
            ok &= FLAC__stream_encoder_set_sample_rate (enc, rate) != 0;
            ok &= FLAC__stream_encoder_set_compression_level (enc, 5) != 0;
            ok &= FLAC__stream_encoder_set_blocksize (enc, 4096) != 0;
            ok &= FLAC__stream_encoder_set_streamable_subset (enc, true) != 0;
            ok &= FLAC__stream_encoder_set_verify (enc, false) != 0;

            if (! ok || FLAC__stream_encoder_init_stream (enc, &FlacEncoder::onWrite, nullptr, nullptr, nullptr, this)
                            != FLAC__STREAM_ENCODER_INIT_STATUS_OK)
            {
                error = "FLAC: initialisation failed";
                return false;
            }
            return true;
        }

        void encode (const float* left, const float* right, int numFrames, std::vector<EncodedPacket>& out) override
        {
            if (numFrames <= 0) return;
            interleaved.resize ((std::size_t) numFrames * 2);

            if (bits == 16)
            {
                for (int i = 0; i < numFrames; ++i)
                {
                    double l = (double) left[i] * 32768.0, r = (double) right[i] * 32768.0;
                    if (dither)
                    {
                        l += rngL.next01() - rngL.next01(); // TPDF ±1 LSB
                        r += rngR.next01() - rngR.next01();
                    }
                    interleaved[(std::size_t) i * 2]     = quantize (l, -32768, 32767);
                    interleaved[(std::size_t) i * 2 + 1] = quantize (r, -32768, 32767);
                }
            }
            else
            {
                for (int i = 0; i < numFrames; ++i)
                {
                    interleaved[(std::size_t) i * 2]     = quantize ((double) left[i]  * 8388608.0, -8388608, 8388607);
                    interleaved[(std::size_t) i * 2 + 1] = quantize ((double) right[i] * 8388608.0, -8388608, 8388607);
                }
            }

            sink = &out;
            FLAC__stream_encoder_process_interleaved (enc, interleaved.data(), (unsigned) numFrames);
            sink = nullptr;
        }

        Codec codec() const noexcept override              { return codecFlac; }
        std::uint32_t outputRate() const noexcept override { return rate; }
        int bitsPerSample() const noexcept override        { return bits; }

    private:
        static FLAC__StreamEncoderWriteStatus onWrite (const FLAC__StreamEncoder*, const FLAC__byte buffer[],
                                                       size_t bytes, uint32_t samples, uint32_t, void* client)
        {
            auto* self = static_cast<FlacEncoder*> (client);
            // samples == 0: metadata (the file header), not needed here.
            if (samples > 0 && self->sink != nullptr)
            {
                EncodedPacket p;
                p.payload.assign (buffer, buffer + bytes);
                p.frames = samples;
                self->sink->push_back (std::move (p));
            }
            return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
        }

        FLAC__StreamEncoder* enc = nullptr;
        std::uint32_t rate;
        int bits;
        bool dither;
        Rng rngL, rngR;
        std::vector<FLAC__int32> interleaved;
        std::vector<EncodedPacket>* sink = nullptr;
    };

    //==========================================================================
    class OpusStreamEncoder final : public Encoder
    {
    public:
        static constexpr int kFrame = 960;          // 20 ms at 48 kHz
        static constexpr int kFramesPerPacket = 5;  // 100 ms per network message

        explicit OpusStreamEncoder (std::uint32_t inRateIn) : inRate (inRateIn) {}

        ~OpusStreamEncoder() override
        {
            if (enc != nullptr) opus_encoder_destroy (enc);
            if (resampler != nullptr) speex_resampler_destroy (resampler);
        }

        bool init (std::string& error)
        {
            int err = 0;
            enc = opus_encoder_create (48000, 2, OPUS_APPLICATION_AUDIO, &err);
            if (err != OPUS_OK || enc == nullptr) { error = "Opus: initialisation failed"; return false; }

            opus_encoder_ctl (enc, OPUS_SET_BITRATE (kOpusBitrate));
            opus_encoder_ctl (enc, OPUS_SET_VBR (1));
            opus_encoder_ctl (enc, OPUS_SET_VBR_CONSTRAINT (1));
            opus_encoder_ctl (enc, OPUS_SET_COMPLEXITY (10));
            opus_encoder_ctl (enc, OPUS_SET_SIGNAL (OPUS_SIGNAL_MUSIC));
            opus_encoder_ctl (enc, OPUS_SET_BANDWIDTH (OPUS_BANDWIDTH_FULLBAND));
            opus_encoder_ctl (enc, OPUS_GET_LOOKAHEAD (&lookahead));

            if (inRate != 48000)
            {
                resampler = speex_resampler_init (2, inRate, 48000, 10, &err);
                if (resampler == nullptr || err != 0) { error = "Opus: resampler unavailable"; return false; }
                speex_resampler_skip_zeros (resampler);
            }
            return true;
        }

        void encode (const float* left, const float* right, int numFrames, std::vector<EncodedPacket>& out) override
        {
            if (numFrames <= 0) return;

            inter.resize ((std::size_t) numFrames * 2);
            for (int i = 0; i < numFrames; ++i)
            {
                inter[(std::size_t) i * 2]     = left[i];
                inter[(std::size_t) i * 2 + 1] = right[i];
            }

            if (resampler == nullptr)
            {
                pending.insert (pending.end(), inter.begin(), inter.end());
            }
            else
            {
                spx_uint32_t inLen = (spx_uint32_t) numFrames;
                const auto cap = (std::size_t) ((double) numFrames * 48000.0 / inRate) + 64;
                resampled.resize (cap * 2);
                spx_uint32_t outLen = (spx_uint32_t) cap;
                speex_resampler_process_interleaved_float (resampler, inter.data(), &inLen, resampled.data(), &outLen);
                pending.insert (pending.end(), resampled.begin(), resampled.begin() + (std::ptrdiff_t) outLen * 2);
            }

            std::size_t offset = 0;
            while (pending.size() - offset >= (std::size_t) kFrame * 2)
            {
                const int n = opus_encode_float (enc, pending.data() + offset, kFrame, packetBuf, (opus_int32) sizeof (packetBuf));
                offset += (std::size_t) kFrame * 2;
                if (n <= 0) continue;

                batch.payload.push_back ((std::uint8_t) (n & 0xff));
                batch.payload.push_back ((std::uint8_t) ((n >> 8) & 0xff));
                batch.payload.insert (batch.payload.end(), packetBuf, packetBuf + n);
                batch.frames += kFrame;

                if (++batchCount >= kFramesPerPacket)
                {
                    out.push_back (std::move (batch));
                    batch = {};
                    batchCount = 0;
                }
            }
            pending.erase (pending.begin(), pending.begin() + (std::ptrdiff_t) offset);
        }

        Codec codec() const noexcept override              { return codecOpus; }
        std::uint32_t outputRate() const noexcept override { return 48000; }
        int bitsPerSample() const noexcept override        { return 0; }
        int preSkip() const noexcept override              { return lookahead; }

    private:
        std::uint32_t inRate;
        OpusEncoder* enc = nullptr;
        SpeexResamplerState* resampler = nullptr;
        opus_int32 lookahead = 0;
        std::vector<float> inter, resampled, pending;
        unsigned char packetBuf[1500];
        EncodedPacket batch;
        int batchCount = 0;
    };
}

std::unique_ptr<Encoder> makeEncoder (Quality q, std::uint32_t sampleRate, bool dither, std::string& error)
{
    if (q == Quality::eco)
    {
        auto e = std::make_unique<OpusStreamEncoder> (sampleRate);
        if (! e->init (error)) return nullptr;
        return e;
    }

    auto e = std::make_unique<FlacEncoder> (sampleRate, q == Quality::hires ? 24 : 16, dither);
    if (! e->init (error)) return nullptr;
    return e;
}

} // namespace studiowire
