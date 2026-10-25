// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The StudioRider authors
//
#include "StreamServer.h"

#include "civetweb.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <random>

namespace studiorider
{

namespace
{
    using Clock = std::chrono::steady_clock;

    std::int64_t nowNs() noexcept
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds> (Clock::now().time_since_epoch()).count();
    }

    constexpr std::size_t kMaxQueuedBytes = 4u * 1024u * 1024u; // ~7 s of 24-bit/48 kHz PCM per listener
    constexpr std::uint32_t kMaxFramesPerChunk = 4096;

    inline void putU32 (std::uint8_t* p, std::uint32_t v) noexcept
    {
        p[0] = (std::uint8_t) (v & 0xff);
        p[1] = (std::uint8_t) ((v >> 8) & 0xff);
        p[2] = (std::uint8_t) ((v >> 16) & 0xff);
        p[3] = (std::uint8_t) ((v >> 24) & 0xff);
    }

    int quietLog (const mg_connection*, const char*) { return 1; }

    std::once_flag libraryInit;
}

//==============================================================================
StreamServer::StreamServer()
{
    ring.assign (kRingFrames * kChannels, 0.0f);
}

StreamServer::~StreamServer()
{
    stop();
}

std::string StreamServer::randomToken (std::size_t length)
{
    static const char alphabet[] = "abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    std::random_device rd;
    std::uniform_int_distribution<std::size_t> dist (0, sizeof (alphabet) - 2);
    std::string s;
    s.reserve (length);
    for (std::size_t i = 0; i < length; ++i)
        s.push_back (alphabet[dist (rd)]);
    return s;
}

std::string StreamServer::getToken() const   { return token; }
std::string StreamServer::getPath() const    { return "/" + token + "/"; }

std::string StreamServer::getLocalUrl() const
{
    return "http://127.0.0.1:" + std::to_string (port.load()) + getPath();
}

bool StreamServer::isReceivingAudio() const noexcept
{
    const auto last = lastPushNs.load (std::memory_order_relaxed);
    return last != 0 && (nowNs() - last) < 500'000'000;
}

void StreamServer::setSampleRate (double sr) noexcept
{
    if (sr > 0)
        sampleRate.store ((std::uint32_t) std::lround (sr), std::memory_order_relaxed);
}

//==============================================================================
bool StreamServer::start (const Options& options, std::string& error)
{
    if (running.load())
        return true;

    std::call_once (libraryInit, [] { mg_init_library (0); });

    token = options.token.empty() ? randomToken() : options.token;
    wsPath = "/" + token + "/ws";
    maxListeners = std::max (1, options.maxListeners);
    dither = options.dither;
    maxQuality.store ((int) options.maxQuality);
    for (auto& c : qualityCount) c.store (0);
    setEncoderError ({});

    mg_callbacks callbacks;
    std::memset (&callbacks, 0, sizeof (callbacks));
    callbacks.log_message = quietLog;

    for (int i = 0; i < options.portAttempts && ctx == nullptr; ++i)
    {
        const int p = options.firstPort + i;
        const std::string ports = "127.0.0.1:" + std::to_string (p); // never 0.0.0.0
        const char* cfg[] = {
            "listening_ports",            ports.c_str(),
            "num_threads",                "16",
            "request_timeout_ms",         "5000",
            "websocket_timeout_ms",       "30000",
            "enable_websocket_ping_pong", "yes",
            "enable_keep_alive",          "yes",
            "max_request_size",           "16384",
            nullptr
        };

        ctx = mg_start (&callbacks, this, cfg);
        if (ctx != nullptr)
            port.store (p);
    }

    if (ctx == nullptr)
    {
        error = "Could not open a local port (" + std::to_string (options.firstPort) + "-"
              + std::to_string (options.firstPort + options.portAttempts - 1) + ").";
        return false;
    }

    mg_set_request_handler (ctx, "/", &StreamServer::onHttpRequest, this);
    mg_set_websocket_handler (ctx, wsPath.c_str(),
                              &StreamServer::onWsConnect, &StreamServer::onWsReady,
                              &StreamServer::onWsData, &StreamServer::onWsClose, this);

    readPos.store (writePos.load());
    bytesSent.store (0);
    droppedFrames.store (0);
    quit.store (false);
    running.store (true);
    sender = std::thread ([this] { senderLoop(); });
    return true;
}

void StreamServer::stop()
{
    if (! running.exchange (false))
        return;

    quit.store (true);
    if (sender.joinable())
        sender.join();

    if (ctx != nullptr)
    {
        // Tell every page that the studio is stopping the share, then close
        // cleanly (code 4000) before the server and the tunnel go away.
        static const std::string bye = "{\"type\":\"bye\",\"reason\":\"stopped\"}";
        broadcast (MG_WEBSOCKET_OPCODE_TEXT, std::make_shared<const std::vector<std::uint8_t>> (bye.begin(), bye.end()));
        std::vector<std::uint8_t> closeFrame { (std::uint8_t) (4000 >> 8), (std::uint8_t) (4000 & 0xff) };
        const std::string reason = "sharing stopped";
        closeFrame.insert (closeFrame.end(), reason.begin(), reason.end());
        broadcast (MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE, std::make_shared<const std::vector<std::uint8_t>> (std::move (closeFrame)));
        waitForWriters (800);

        mg_stop (ctx); // closes whatever is left; onWsClose is called for each
        ctx = nullptr;
    }

    std::map<const mg_connection*, std::unique_ptr<Listener>> remaining;
    {
        std::lock_guard<std::mutex> lock (listenersMutex);
        remaining.swap (listeners);
        numListeners.store (0);
        for (auto& c : qualityCount) c.store (0);
    }
    for (auto& kv : remaining)
    {
        auto& l = *kv.second;
        { std::lock_guard<std::mutex> lk (l.mutex); l.closing = true; }
        l.cv.notify_all();
        if (l.writer.joinable())
            l.writer.join();
    }

    port.store (0);
    lastPushNs.store (0);
}

//==============================================================================
void StreamServer::pushAudio (const float* const* channels, int numChannels, int numFrames) noexcept
{
    if (! running.load (std::memory_order_relaxed) || numFrames <= 0 || numChannels <= 0 || channels == nullptr)
        return;

    lastPushNs.store (nowNs(), std::memory_order_relaxed);

    if (numListeners.load (std::memory_order_relaxed) == 0)
        return;

    const auto w = writePos.load (std::memory_order_relaxed);
    const auto r = readPos.load (std::memory_order_acquire);
    const auto freeFrames = (std::uint64_t) kRingFrames - (w - r);
    const auto n = (std::uint64_t) std::min<std::uint64_t> ((std::uint64_t) numFrames, freeFrames);

    if (n < (std::uint64_t) numFrames)
        droppedFrames.fetch_add ((std::uint64_t) numFrames - n, std::memory_order_relaxed);

    const float* left  = channels[0];
    const float* right = numChannels > 1 ? channels[1] : channels[0];
    constexpr auto mask = (std::uint64_t) kRingFrames - 1;

    for (std::uint64_t i = 0; i < n; ++i)
    {
        const auto idx = (std::size_t) (((w + i) & mask) * kChannels);
        ring[idx]     = left[i];
        ring[idx + 1] = right[i];
    }

    writePos.store (w + n, std::memory_order_release);
}

//==============================================================================
void StreamServer::senderLoop()
{
    auto lastSend = Clock::now();
    auto lastPing = Clock::now();
    bool hadListeners = false;
    constexpr auto mask = (std::uint64_t) kRingFrames - 1;

    std::array<std::unique_ptr<Encoder>, kNumQualities> encoders;
    std::array<std::uint32_t, kNumQualities> encoderRate {};
    std::array<std::uint32_t, kNumQualities> sequence {};
    std::array<bool, kNumQualities> failed {};
    std::vector<float> left (kMaxFramesPerChunk), right (kMaxFramesPerChunk);
    std::vector<EncodedPacket> packets;

    while (! quit.load())
    {
        std::this_thread::sleep_for (std::chrono::milliseconds (5));

        if (numListeners.load() == 0)
        {
            readPos.store (writePos.load (std::memory_order_acquire), std::memory_order_release);
            if (hadListeners)
                for (auto& e : encoders) e.reset();
            hadListeners = false;
            continue;
        }

        if (! hadListeners)
        {
            // First listener: start from now, not from an old leftover.
            readPos.store (writePos.load (std::memory_order_acquire), std::memory_order_release);
            hadListeners = true;
        }

        const auto now = Clock::now();

        if (now - lastPing > std::chrono::seconds (10))
        {
            const auto json = infoJson();
            broadcast (MG_WEBSOCKET_OPCODE_TEXT,
                       std::make_shared<const std::vector<std::uint8_t>> (json.begin(), json.end()));
            lastPing = now;
        }

        auto r = readPos.load (std::memory_order_relaxed);
        const auto w = writePos.load (std::memory_order_acquire);
        auto available = w - r;
        if (available == 0)
            continue;

        const auto sr = sampleRate.load (std::memory_order_relaxed);
        const auto chunkTarget = (std::uint64_t) std::max<std::uint32_t> (64, sr / 50); // ~20 ms
        if (available < chunkTarget && now - lastSend < std::chrono::milliseconds (40))
            continue;

        while (available > 0)
        {
            const auto frames = (std::uint32_t) std::min<std::uint64_t> (available, kMaxFramesPerChunk);
            for (std::uint32_t i = 0; i < frames; ++i)
            {
                const auto idx = (std::size_t) (((r + i) & mask) * kChannels);
                left[i]  = ring[idx];
                right[i] = ring[idx + 1];
            }
            r += frames;
            available -= frames;
            readPos.store (r, std::memory_order_release);

            for (int q = 0; q < kNumQualities; ++q)
            {
                if (qualityCount[(std::size_t) q].load() == 0)
                {
                    encoders[(std::size_t) q].reset(); // nobody listens to this format: nothing to encode
                    failed[(std::size_t) q] = false;
                    continue;
                }

                auto& enc = encoders[(std::size_t) q];
                if (enc == nullptr || encoderRate[(std::size_t) q] != sr)
                {
                    if (failed[(std::size_t) q] && encoderRate[(std::size_t) q] == sr)
                        continue;
                    std::string err;
                    enc = makeEncoder ((Quality) q, sr, dither, err);
                    encoderRate[(std::size_t) q] = sr;
                    failed[(std::size_t) q] = enc == nullptr;
                    if (enc == nullptr)
                    {
                        setEncoderError (err);
                        continue;
                    }
                }

                packets.clear();
                enc->encode (left.data(), right.data(), (int) frames, packets);

                for (auto& pkt : packets)
                {
                    auto msg = std::make_shared<std::vector<std::uint8_t>> (kHeaderSize + pkt.payload.size());
                    auto* p = msg->data();
                    p[0] = 'M'; p[1] = 'X'; p[2] = 'S'; p[3] = '2';
                    p[4] = (std::uint8_t) enc->codec();
                    p[5] = (std::uint8_t) kChannels;
                    p[6] = (std::uint8_t) enc->bitsPerSample();
                    p[7] = (std::uint8_t) q;
                    putU32 (p + 8, enc->outputRate());
                    putU32 (p + 12, pkt.frames);
                    putU32 (p + 16, sequence[(std::size_t) q]++);
                    std::memcpy (p + kHeaderSize, pkt.payload.data(), pkt.payload.size());
                    broadcast (MG_WEBSOCKET_OPCODE_BINARY, std::move (msg), q);
                }
            }
        }

        lastSend = now;
    }
}

void StreamServer::broadcast (int opcode, std::shared_ptr<const std::vector<std::uint8_t>> data, int onlyQuality)
{
    std::lock_guard<std::mutex> lock (listenersMutex);
    for (auto& kv : listeners)
        if (onlyQuality < 0 || (int) kv.second->quality == onlyQuality)
            enqueue (*kv.second, Message { opcode, data });
}

void StreamServer::setMaxQuality (Quality q)
{
    if ((int) q == maxQuality.exchange ((int) q))
        return;

    {
        std::lock_guard<std::mutex> lock (listenersMutex);
        for (auto& kv : listeners)
        {
            auto& l = *kv.second;
            const auto clamped = clampQuality (l.quality, q);
            if (clamped != l.quality)
            {
                qualityCount[(std::size_t) l.quality].fetch_sub (1);
                qualityCount[(std::size_t) clamped].fetch_add (1);
                l.quality = clamped; // the sender thread switches them to the new format on the next packet
            }
        }
    }

    if (running.load())
    {
        const auto json = infoJson();
        broadcast (MG_WEBSOCKET_OPCODE_TEXT, std::make_shared<const std::vector<std::uint8_t>> (json.begin(), json.end()));
    }
}

void StreamServer::setEncoderError (const std::string& e)
{
    std::lock_guard<std::mutex> lock (errorMutex);
    encoderError = e;
}

std::string StreamServer::getEncoderError() const
{
    std::lock_guard<std::mutex> lock (errorMutex);
    return encoderError;
}

void StreamServer::enqueue (Listener& l, Message msg)
{
    {
        std::lock_guard<std::mutex> lk (l.mutex);
        if (l.closing)
            return;

        const auto size = msg.data->size();
        if (msg.opcode == MG_WEBSOCKET_OPCODE_BINARY && l.queuedBytes + size > kMaxQueuedBytes)
            return; // listener too slow: skip this packet rather than hold everyone else up

        l.queuedBytes += size;
        l.queue.push_back (std::move (msg));
    }
    l.cv.notify_one();
}

void StreamServer::waitForWriters (int maxMs)
{
    for (int waited = 0; waited < maxMs; waited += 10)
    {
        bool idle = true;
        {
            std::lock_guard<std::mutex> lock (listenersMutex);
            for (auto& kv : listeners)
            {
                std::lock_guard<std::mutex> lk (kv.second->mutex);
                if (! kv.second->closing && (kv.second->busy || ! kv.second->queue.empty()))
                    idle = false;
            }
        }
        if (idle)
            return;
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
}

void StreamServer::writerLoop (Listener* l)
{
    for (;;)
    {
        std::vector<Message> batch;
        {
            std::unique_lock<std::mutex> lk (l->mutex);
            l->cv.wait (lk, [l] { return l->closing || ! l->queue.empty(); });
            if (l->closing)
                return;
            batch.swap (l->queue);
            l->queuedBytes = 0;
            l->busy = true;
        }

        for (auto& m : batch)
        {
            const int rc = mg_websocket_write (l->conn, m.opcode,
                                               reinterpret_cast<const char*> (m.data->data()),
                                               m.data->size());
            if (rc <= 0)
            {
                // Connection broken: wait for civetweb to close it.
                std::unique_lock<std::mutex> lk (l->mutex);
                l->busy = false;
                l->cv.wait (lk, [l] { return l->closing; });
                return;
            }
            bytesSent.fetch_add (m.data->size(), std::memory_order_relaxed);
        }

        std::lock_guard<std::mutex> lk (l->mutex);
        l->busy = false;
    }
}

//==============================================================================
std::string StreamServer::infoJson() const
{
    std::string s = "{\"type\":\"info\",\"name\":\"StudioRider\"";
    s += std::string (",\"maxQuality\":\"") + qualityId (getMaxQuality()) + "\"";
    s += ",\"sampleRate\":"   + std::to_string (sampleRate.load());
    s += ",\"channels\":"     + std::to_string (kChannels);
    s += ",\"opusBitrate\":"  + std::to_string (kOpusBitrate);
    s += ",\"listeners\":"    + std::to_string (numListeners.load());
    s += ",\"maxListeners\":" + std::to_string (maxListeners);
    s += ",\"byQuality\":{";
    for (int q = 0; q < kNumQualities; ++q)
    {
        if (q > 0) s += ",";
        s += std::string ("\"") + qualityId ((Quality) q) + "\":" + std::to_string (qualityCount[(std::size_t) q].load());
    }
    s += "}";
    s += std::string (",\"receiving\":") + (isReceivingAudio() ? "true" : "false");
    s += "}";
    return s;
}

void StreamServer::sendResponse (mg_connection* conn, int status, const char* contentType,
                                 const void* body, std::size_t size, bool isHtml)
{
    const auto* ri = mg_get_request_info (conn);
    const bool isHead = ri != nullptr && ri->request_method != nullptr
                         && std::strcmp (ri->request_method, "HEAD") == 0;
    const auto length = std::to_string (size);

    mg_response_header_start (conn, status);
    mg_response_header_add (conn, "Content-Type", contentType, -1);
    mg_response_header_add (conn, "Content-Length", length.c_str(), -1);
    mg_response_header_add (conn, "Cache-Control", "no-store", -1);
    mg_response_header_add (conn, "X-Content-Type-Options", "nosniff", -1);
    mg_response_header_add (conn, "Referrer-Policy", "no-referrer", -1);
    mg_response_header_add (conn, "X-Frame-Options", "DENY", -1);
    mg_response_header_add (conn, "X-Robots-Tag", "noindex, nofollow", -1);
    if (isHtml)
        mg_response_header_add (conn, "Content-Security-Policy",
                                "default-src 'self'; script-src 'self' 'wasm-unsafe-eval'; style-src 'self'; "
                                "connect-src 'self' ws: wss:; img-src 'self' data:; "
                                "frame-ancestors 'none'; base-uri 'none'; form-action 'none'", -1);
    mg_response_header_send (conn);

    if (! isHead && size > 0)
        mg_write (conn, body, size);
}

int StreamServer::handleHttp (mg_connection* conn)
{
    static const char notFound[] = "Not found\n";
    const auto* ri = mg_get_request_info (conn);
    if (ri == nullptr || ri->local_uri == nullptr || ri->request_method == nullptr)
        return 0;

    const bool isGet = std::strcmp (ri->request_method, "GET") == 0
                    || std::strcmp (ri->request_method, "HEAD") == 0;
    if (! isGet)
    {
        static const char msg[] = "Method not allowed\n";
        sendResponse (conn, 405, "text/plain; charset=utf-8", msg, sizeof (msg) - 1, false);
        return 405;
    }

    const std::string uri = ri->local_uri;
    const std::string base = "/" + token;

    if (uri == base)
    {
        const auto target = base + "/";
        mg_send_http_redirect (conn, target.c_str(), 302);
        return 302;
    }

    if (uri.size() > base.size() && uri.compare (0, base.size() + 1, base + "/") == 0)
    {
        std::string rest = uri.substr (base.size() + 1);
        if (rest.empty())
            rest = "index.html";

        if (rest == "info")
        {
            const auto json = infoJson();
            sendResponse (conn, 200, "application/json; charset=utf-8", json.data(), json.size(), false);
            return 200;
        }

        for (std::size_t i = 0; i < kNumWebAssets; ++i)
        {
            const auto& a = kWebAssets[i];
            if (rest == a.name)
            {
                sendResponse (conn, 200, a.contentType, a.data, a.size, rest == "index.html");
                return 200;
            }
        }
    }

    sendResponse (conn, 404, "text/plain; charset=utf-8", notFound, sizeof (notFound) - 1, false);
    return 404;
}

//==============================================================================
int StreamServer::onHttpRequest (mg_connection* conn, void* cbdata)
{
    return static_cast<StreamServer*> (cbdata)->handleHttp (conn);
}

int StreamServer::onWsConnect (const mg_connection* conn, void* cbdata)
{
    auto* self = static_cast<StreamServer*> (cbdata);
    if (! self->running.load())
        return 1; // sharing is stopping
    const auto* ri = mg_get_request_info (conn);
    if (ri == nullptr || ri->local_uri == nullptr || self->wsPath != ri->local_uri)
        return 1; // refused

    std::lock_guard<std::mutex> lock (self->listenersMutex);
    return (int) self->listeners.size() >= self->maxListeners ? 1 : 0;
}

void StreamServer::onWsReady (mg_connection* conn, void* cbdata)
{
    auto* self = static_cast<StreamServer*> (cbdata);
    auto listener = std::make_unique<Listener>();
    listener->conn = conn;

    // Without an explicit choice, the listener gets the stream quality; never more.
    Quality requested = self->getMaxQuality();
    const auto* ri = mg_get_request_info (conn);
    if (ri != nullptr && ri->query_string != nullptr)
    {
        char value[16] = {};
        Quality q;
        if (mg_get_var (ri->query_string, std::strlen (ri->query_string), "q", value, sizeof (value)) > 0
            && parseQuality (value, q))
            requested = q;
    }

    auto* raw = listener.get();

    {
        std::lock_guard<std::mutex> lock (self->listenersMutex);
        raw->quality = clampQuality (requested, self->getMaxQuality());
        self->qualityCount[(std::size_t) raw->quality].fetch_add (1);
        self->listeners[conn] = std::move (listener);
        self->numListeners.store ((int) self->listeners.size());
    }

    raw->writer = std::thread ([self, raw] { self->writerLoop (raw); });

    const auto json = self->infoJson();
    self->enqueue (*raw, Message { MG_WEBSOCKET_OPCODE_TEXT,
                                   std::make_shared<const std::vector<std::uint8_t>> (json.begin(), json.end()) });
}

int StreamServer::onWsData (mg_connection*, int bits, char*, std::size_t, void*)
{
    // The browser only sends keep-alives; everything else is ignored.
    return (bits & 0x0f) == MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE ? 0 : 1;
}

void StreamServer::onWsClose (const mg_connection* conn, void* cbdata)
{
    auto* self = static_cast<StreamServer*> (cbdata);
    std::unique_ptr<Listener> l;
    {
        std::lock_guard<std::mutex> lock (self->listenersMutex);
        auto it = self->listeners.find (conn);
        if (it != self->listeners.end())
        {
            l = std::move (it->second);
            self->listeners.erase (it);
            self->qualityCount[(std::size_t) l->quality].fetch_sub (1);
        }
        self->numListeners.store ((int) self->listeners.size());
    }

    if (l != nullptr)
    {
        { std::lock_guard<std::mutex> lk (l->mutex); l->closing = true; }
        l->cv.notify_all();
        if (l->writer.joinable())
            l->writer.join();
    }
}

} // namespace studiorider
