// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The StudioWire authors
//
(() => {
  'use strict';

  const base = location.pathname.endsWith('/') ? location.pathname : location.pathname + '/';
  const $ = (id) => document.getElementById(id);
  const ui = {
    state: $('state'), format: $('format'), play: $('play'), playLabel: $('playLabel'),
    quality: $('quality'), volume: $('volume'), volOut: $('volOut'), latency: $('latency'),
    buf: $('sBuf'), rate: $('sRate'), under: $('sUnder'), listeners: $('sListeners'),
    mL: $('meterL'), mR: $('meterR'),
    notice: $('notice'), noticeTitle: $('noticeTitle'), noticeText: $('noticeText'),
  };

  const nf = (x, d) => x.toLocaleString('en-US', { minimumFractionDigits: d, maximumFractionDigits: d });
  const khz = (r) => { const k = r / 1000; return nf(k, Number.isInteger(k) ? 0 : 1); };

  // The three qualities offered by the plugin, best to lightest.
  const QUALITIES = {
    hires: { rank: 2, codec: 'flac', label: 'Hi-Res · FLAC 24-bit',
             rate: (src) => src, text: (r) => `FLAC 24-bit · ${khz(r)} kHz · stereo · lossless` },
    cd:    { rank: 1, codec: 'flac', label: 'CD · FLAC 16-bit',
             rate: (src) => src, text: (r) => `FLAC 16-bit · ${khz(r)} kHz · stereo · lossless, CD quality` },
    eco:   { rank: 0, codec: 'opus', label: 'Data saver · Opus 256 kbps',
             rate: () => 48000,  text: () => `Opus 256 kbps · 48 kHz · stereo · compressed` },
  };
  const CODEC_FLAC = 2, CODEC_OPUS = 3;

  let info = null;
  let cap = null;        // stream quality set in the plugin (the cap)
  let pref = 'auto';     // listener's choice: 'auto' = same as the stream
  let quality = 'cd';    // quality actually being played
  let ctx = null, node = null, gain = null, ctxRate = 0, streamRate = 0, building = null;
  let ws = null, running = false, retryTimer = 0, kaTimer = 0;
  let decoder = null, decoderKind = '', decodeChain = Promise.resolve(), session = 0;
  let pending = [];
  let bytes = 0, lastRateT = performance.now(), lastData = 0;
  let workletUnder = 0, netUnder = 0, gaps = 0, decodeErrors = 0, expectedSeq = -1;
  let lastStats = null;
  let ended = false;                         // the studio stopped sharing: this link is dead
  let failures = 0, lostSince = 0, infoFailures = 0;

  const NOTICES = {
    ended: ['Sharing has ended',
            'The studio stopped sharing. This link no longer works: ask for a new link to listen again.', 'err'],
    gone:  ['Sharing seems to have ended',
            'The studio has not responded for 2 minutes. If sharing was restarted, you need the new link.', 'err'],
    lost:  ['Connection lost',
            'Can’t reach the studio right now. Trying again…', 'wait'],
    idle:  ['Not sharing right now',
            'The studio isn’t sharing at the moment, or this link has expired.', 'wait'],
  };

  function showNotice(kind) {
    const [title, text, cls] = NOTICES[kind];
    ui.noticeTitle.textContent = title;
    ui.noticeText.textContent = text;
    ui.notice.className = 'notice ' + cls;
    ui.notice.hidden = false;
  }

  function hideNotice() {
    if (!ended) ui.notice.hidden = true;
  }

  // Final end: no more playback, no more reconnection attempts.
  function endSession(kind) {
    if (ended) return;
    ended = true;
    if (running) stop();
    clearTimeout(retryTimer);
    clearInterval(kaTimer);
    showNotice(kind);
    setState('Sharing ended', 'err');
    ui.play.disabled = true;
    ui.quality.disabled = true;
    ui.format.textContent = 'The studio is no longer sending any audio.';
    ui.listeners.textContent = '—';
    document.title = 'StudioWire · Sharing ended';
  }

  try {
    const saved = localStorage.getItem('studiowire.pref');
    if (saved === 'auto' || QUALITIES[saved]) pref = saved;
  } catch (e) {}

  // Quality to play: the stream quality, unless the listener picked a lower one.
  function effectiveQuality() {
    if (!cap) return quality;
    if (pref === 'auto' || !QUALITIES[pref] || QUALITIES[pref].rank > QUALITIES[cap].rank) return cap;
    return pref;
  }

  // Only qualities at or below the stream quality are offered.
  function renderOptions() {
    ui.quality.textContent = '';
    for (const q of ['hires', 'cd', 'eco']) {
      if (cap && QUALITIES[q].rank > QUALITIES[cap].rank) continue;
      const o = document.createElement('option');
      o.value = q;
      o.textContent = QUALITIES[q].label + (q === cap ? ' (studio’s choice)' : '');
      ui.quality.appendChild(o);
    }
    ui.quality.value = quality;
    ui.quality.disabled = ui.quality.options.length <= 1;
  }

  function applyInfo(m) {
    info = Object.assign(info || {}, m);
    if (m.maxQuality && QUALITIES[m.maxQuality]) cap = m.maxQuality;
    const q = effectiveQuality();
    if (q !== quality) switchQuality(q); // the studio changed the stream quality
    renderOptions();
    describe();
    showListeners(info);
  }

  function setState(text, cls) {
    ui.state.textContent = text;
    ui.state.className = 'pill ' + (cls || '');
  }

  function describe() {
    const src = info ? info.sampleRate : 48000;
    let t = QUALITIES[quality].text(QUALITIES[quality].rate(src));
    if (ctx && ctxRate !== streamRate)
      t += ` (your browser resamples to ${khz(ctxRate)} kHz)`;
    ui.format.textContent = t;
  }

  function showListeners(m) {
    if (m && typeof m.listeners === 'number') ui.listeners.textContent = `${m.listeners} / ${m.maxListeners || 5}`;
  }

  async function loadInfo() {
    try {
      const r = await fetch(base + 'info', { cache: 'no-store' });
      if (!r.ok) throw new Error('HTTP ' + r.status);
      applyInfo(await r.json());
      infoFailures = 0;
      hideNotice();
      ui.play.disabled = false;
      if (!running) {
        if (info.receiving) setState('Signal present', 'ok');
        else setState('Waiting for signal', 'wait');
      }
      return true;
    } catch (e) {
      infoFailures++;
      if (!running && !ended) {
        setState('Offline', 'err');
        ui.format.textContent = 'Sharing is stopped or this link has expired.';
        ui.play.disabled = true;
        if (infoFailures >= 2) showNotice('idle');
      }
      return false;
    }
  }

  // ------------------------------------------------------------- decoders --
  const scripts = {};
  function loadScript(name) {
    if (!scripts[name]) {
      scripts[name] = new Promise((resolve, reject) => {
        const s = document.createElement('script');
        s.src = base + name;
        s.onload = resolve;
        s.onerror = () => { delete scripts[name]; reject(new Error('Could not load ' + name)); };
        document.head.appendChild(s);
      });
    }
    return scripts[name];
  }

  async function makeDecoder(kind) {
    if (kind === 'flac') {
      await loadScript('flac-decoder.min.js');
      const d = new globalThis['flac-decoder'].FLACDecoder();
      await d.ready;
      return d;
    }
    await loadScript('opus-decoder.min.js');
    const d = new globalThis['opus-decoder'].OpusDecoder({ channels: 2, sampleRate: 48000 });
    await d.ready;
    return d;
  }

  function releaseDecoder() {
    const old = decoder;
    decoder = null;
    decoderKind = '';
    // Freed after the decodes in progress (the chain is sequential).
    const free = () => { if (old) { try { old.free(); } catch (e) {} } };
    decodeChain = decodeChain.then(free, free);
  }

  // The FLAC decoder divides by 2^(b-1)-1; we go back to the exact integer and
  // then to the usual convention (division by 2^(b-1)): the output is bit-identical
  // to what the plugin encoded.
  function exactFlac(ch, bits) {
    const half = 2 ** (bits - 1), m = half - 1;
    const out = new Float32Array(ch.length);
    for (let i = 0; i < ch.length; i++) out[i] = Math.round(ch[i] * m) / half;
    return out;
  }

  function splitOpus(payload) {
    const out = [];
    for (let p = 0; p + 2 <= payload.length;) {
      const n = payload[p] | (payload[p + 1] << 8);
      p += 2;
      if (p + n > payload.length) break;
      out.push(payload.subarray(p, p + n));
      p += n;
    }
    return out;
  }

  async function decodePacket(h, mySession) {
    if (mySession !== session || !decoder) return;
    let L, R;
    if (h.codec === CODEC_FLAC) {
      const out = await decoder.decodeFrames([h.payload]);
      if (mySession !== session) return;
      if (!out || !out.samplesDecoded) { decodeErrors++; return; }
      const [a, b] = out.channelData;
      L = exactFlac(a, h.bits);
      R = exactFlac(b || a, h.bits);
    } else {
      const out = decoder.decodeFrames(splitOpus(h.payload));
      if (!out || !out.samplesDecoded) { decodeErrors++; return; }
      const [a, b] = out.channelData;
      L = a.slice();
      R = (b || a).slice();
    }
    sendPcm(L, R);
  }

  function sendPcm(l, r) {
    if (!node || building) {
      pending.push([l, r]);
      if (pending.length > 200) pending.shift();
      return;
    }
    node.port.postMessage({ type: 'pcm', l, r }, [l.buffer, r.buffer]);
  }

  // ---------------------------------------------------------------- audio --
  function teardownAudio() {
    if (node) { try { node.port.onmessage = null; node.disconnect(); } catch (e) {} }
    if (ctx) { ctx.close().catch(() => {}); }
    ctx = null; node = null; gain = null; ctxRate = 0; streamRate = 0;
  }

  // Must be called synchronously from a user gesture (click, quality change).
  function buildAudio(rate) {
    teardownAudio();
    let c;
    try { c = new AudioContext({ sampleRate: rate, latencyHint: 'playback' }); }
    catch (e) { c = new AudioContext({ latencyHint: 'playback' }); }
    ctx = c;
    ctxRate = c.sampleRate;
    streamRate = rate;
    if (c.state === 'suspended') c.resume().catch(() => {});

    building = (async () => {
      await c.audioWorklet.addModule(base + 'worklet.js');
      if (ctx !== c) return;
      const n = new AudioWorkletNode(c, 'studiowire-player', {
        numberOfInputs: 0,
        numberOfOutputs: 1,
        outputChannelCount: [2],
        processorOptions: {
          capacity: Math.ceil(c.sampleRate * 8),
          target: Math.round(c.sampleRate * parseFloat(ui.latency.value)),
        },
      });
      const g = new GainNode(c, { gain: parseFloat(ui.volume.value) });
      n.connect(g).connect(c.destination);
      n.port.onmessage = onStats;
      node = n; gain = g;
      const queued = pending;
      pending = [];
      for (const [l, r] of queued) n.port.postMessage({ type: 'pcm', l, r }, [l.buffer, r.buffer]);
    })().catch((e) => {
      console.error(e);
      setState('Audio error', 'err');
    }).finally(() => { building = null; });

    describe();
  }

  function onStats(e) {
    const s = e.data;
    lastStats = s;
    if (s.under > workletUnder) {
      // A dropout while data was still arriving = a network problem.
      if (performance.now() - lastData < 400) netUnder += s.under - workletUnder;
      workletUnder = s.under;
    }
    ui.buf.textContent = ctxRate ? `${nf(s.fill / ctxRate, 2)} s` : '—';
    ui.under.textContent = String(netUnder);
    meter(ui.mL, s.pkL);
    meter(ui.mR, s.pkR);
  }

  function meter(el, peak) {
    const db = peak > 0 ? 20 * Math.log10(peak) : -120;
    const pct = Math.max(0, Math.min(100, (db + 60) / 60 * 100));
    el.style.width = pct + '%';
    el.classList.toggle('hot', db > -1);
  }

  // ------------------------------------------------------------ websocket --
  function connect() {
    clearTimeout(retryTimer);
    const url = (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + base + 'ws?q=' + quality;
    const s = new WebSocket(url);
    s.binaryType = 'arraybuffer';
    ws = s;
    expectedSeq = -1;
    setState('Connecting…', 'wait');

    s.onopen = () => {
      if (ws !== s) return;
      failures = 0;
      lostSince = 0;
      hideNotice();
      setState('Connected', 'ok');
      clearInterval(kaTimer);
      kaTimer = setInterval(() => { if (s.readyState === 1) s.send('ka'); }, 10000);
    };

    s.onmessage = (ev) => {
      if (ws !== s) return;
      if (typeof ev.data === 'string') {
        try {
          const m = JSON.parse(ev.data);
          if (m.type === 'info') applyInfo(m);
          else if (m.type === 'bye') endSession('ended'); // the studio clicked "Stop sharing"
        } catch (e) {}
        return;
      }
      onPacket(ev.data);
    };

    s.onclose = (ev) => {
      if (ws !== s) return;
      clearInterval(kaTimer);
      ws = null;
      if (!running || ended) return;
      if (ev.code === 4000) { endSession('ended'); return; } // "sharing stopped" close
      scheduleReconnect();
    };
  }

  // Unexpected disconnection (network, DAW crashed…): retry, warn after a few
  // attempts, and give up after 2 minutes.
  function scheduleReconnect() {
    failures++;
    if (!lostSince) lostSince = Date.now();
    if (Date.now() - lostSince > 120000) { endSession('gone'); return; }
    if (failures >= 3) showNotice('lost');
    setState('Reconnecting…', 'wait');
    retryTimer = setTimeout(async () => {
      if (!running || ended) return;
      const ok = await loadInfo();
      if (!running || ended) return;
      if (ok && info && info.listeners >= info.maxListeners) setState('Full: too many listeners', 'err');
      if (ok) connect(); else scheduleReconnect();
    }, failures < 3 ? 2000 : 5000);
  }

  function onPacket(buf) {
    if (!(buf instanceof ArrayBuffer) || buf.byteLength < 20) return;
    const dv = new DataView(buf);
    if (dv.getUint32(0, false) !== 0x4D585332) return; // "MXS2" (protocol id)
    const h = {
      codec: dv.getUint8(4), bits: dv.getUint8(6),
      rate: dv.getUint32(8, true), seq: dv.getUint32(16, true),
      payload: new Uint8Array(buf, 20),
    };
    const kind = h.codec === CODEC_FLAC ? 'flac' : h.codec === CODEC_OPUS ? 'opus' : '';
    if (kind !== QUALITIES[quality].codec) return; // leftover from a previous quality
    if (expectedSeq >= 0 && h.seq !== expectedSeq) gaps++;
    expectedSeq = (h.seq + 1) >>> 0;
    bytes += buf.byteLength;
    lastData = performance.now();

    if (h.rate !== streamRate && !building && h.rate >= 8000 && h.rate <= 384000) {
      // The project sample rate changed: rebuild the audio context.
      if (info && kind === 'flac') info.sampleRate = h.rate;
      pending = [];
      buildAudio(h.rate);
    }

    const mySession = session;
    decodeChain = decodeChain.then(() => decodePacket(h, mySession)).catch((e) => {
      decodeErrors++;
      console.error(e);
    });
  }

  // -------------------------------------------------------------- control --
  async function beginStream(mySession) {
    const q = QUALITIES[quality];
    setState('Loading decoder…', 'wait');
    try {
      const d = await makeDecoder(q.codec);
      if (mySession !== session) { try { d.free(); } catch (e) {} return; }
      decoder = d;
      decoderKind = q.codec;
    } catch (e) {
      console.error(e);
      setState('Decoder unavailable', 'err');
      return;
    }
    connect();
  }

  function start() {
    running = true;
    const mySession = ++session;
    workletUnder = 0; netUnder = 0; gaps = 0; decodeErrors = 0;
    ui.play.classList.add('on');
    ui.playLabel.textContent = 'Stop';
    buildAudio(QUALITIES[quality].rate(info ? info.sampleRate : 48000)); // synchronous, inside the gesture
    beginStream(mySession);
  }

  // Switches quality without stopping everything. At the same sample rate (CD <->
  // Hi-Res), the buffered audio keeps playing during the switch; otherwise (FLAC
  // <-> Opus) the audio context is rebuilt and the buffer refills.
  function switchQuality(q) {
    quality = q;
    if (!running) { describe(); return; }
    const mySession = ++session;
    clearTimeout(retryTimer);
    clearInterval(kaTimer);
    if (ws) { const s = ws; ws = null; try { s.close(); } catch (e) {} }
    releaseDecoder();
    pending = [];
    expectedSeq = -1;
    const rate = QUALITIES[q].rate(info ? info.sampleRate : 48000);
    if (!ctx || rate !== streamRate) buildAudio(rate);
    describe();
    beginStream(mySession);
  }

  function stop() {
    running = false;
    session++;
    clearTimeout(retryTimer);
    clearInterval(kaTimer);
    if (ws) { const s = ws; ws = null; try { s.close(); } catch (e) {} }
    releaseDecoder();
    teardownAudio();
    pending = [];
    ui.play.classList.remove('on');
    ui.playLabel.textContent = 'Listen';
    meter(ui.mL, 0); meter(ui.mR, 0);
    ui.buf.textContent = '—';
    ui.rate.textContent = '—';
    failures = 0;
    lostSince = 0;
    if (!ended) loadInfo();
  }

  ui.play.addEventListener('click', () => {
    if (running && ctx && ctx.state === 'suspended') { // context rebuilt outside a gesture: resume it
      ctx.resume().catch(() => {});
      ui.playLabel.textContent = 'Stop';
      return;
    }
    running ? stop() : start();
  });

  ui.quality.addEventListener('change', () => {
    const q = ui.quality.value;
    if (!QUALITIES[q]) return;
    pref = q === cap ? 'auto' : q; // picking the stream quality = follow the stream
    try { localStorage.setItem('studiowire.pref', pref); } catch (e) {}
    if (q !== quality) switchQuality(q); // inside the gesture: the audio context may restart
  });

  ui.volume.addEventListener('input', () => {
    const v = parseFloat(ui.volume.value);
    ui.volOut.textContent = `${Math.round(v * 100)}%`;
    if (gain && ctx) {
      const t = ctx.currentTime;
      gain.gain.cancelScheduledValues(t);
      gain.gain.setValueAtTime(gain.gain.value, t);
      gain.gain.linearRampToValueAtTime(v, t + 0.03); // ends exactly on the value
    }
  });

  ui.latency.addEventListener('change', () => {
    if (node && ctxRate) node.port.postMessage({ type: 'target', frames: Math.round(ctxRate * parseFloat(ui.latency.value)) });
  });

  // Displayed state, measured bitrate.
  setInterval(() => {
    const now = performance.now();
    const dt = (now - lastRateT) / 1000;
    if (running) {
      const mbps = bytes * 8 / dt / 1e6;
      ui.rate.textContent = mbps < 1 ? `${nf(mbps * 1000, 0)} kbps` : `${nf(mbps, 2)} Mbps`;
      if (ctx && ctx.state === 'suspended') {
        setState('Click Resume', 'wait');
        ui.playLabel.textContent = 'Resume';
      } else if (ws && ws.readyState === 1) {
        if (now - lastData > 1000) setState('Waiting for signal', 'wait');
        else if (lastStats && lastStats.playing) setState('Playing', 'ok');
        else setState('Buffering…', 'wait');
      }
    }
    bytes = 0;
    lastRateT = now;
  }, 1000);

  // While not listening, refresh the stream state from time to time.
  setInterval(() => { if (!running && !ended) loadInfo(); }, 5000);

  // For automated tests.
  window.__studiowire = () => ({
    running, ended, notice: ui.notice.hidden ? '' : ui.noticeTitle.textContent, failures, quality, cap, pref, options: [...ui.quality.options].map((o) => o.value), decoder: decoderKind, ctxRate, streamRate, gaps, netUnder, decodeErrors,
    stats: lastStats, ws: ws ? ws.readyState : -1,
  });

  loadInfo();
})();
