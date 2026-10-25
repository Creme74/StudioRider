// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The StudioRider authors
//
// Checks the three qualities with the REAL decoders of the listening page
// (web/flac-decoder.min.js and web/opus-decoder.min.js, run as they are).
//
//   node tests/check_codecs.mjs exact <url> <cd|hires> [seconds]
//       harness started with --pattern ramp16 --no-dither (cd) or --pattern ramp --max hires (hires):
//       the decoded output must be bit-identical.
//   node tests/check_codecs.mjs sine <url> [seconds]
//       harness started with --pattern sine: the eco mode (Opus) must reproduce 440/660 Hz.
//   node tests/check_codecs.mjs rates <url> [seconds]
//       measures the real bitrate of the three qualities in parallel (harness with --max hires).

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
globalThis.Worker ??= class {}; // the bundles declare a Web Worker variant (unused here)
for (const f of ['flac-decoder.min.js', 'opus-decoder.min.js'])
  new Function(readFileSync(join(here, '..', 'web', f), 'utf8'))();
const { FLACDecoder } = globalThis['flac-decoder'];
const { OpusDecoder } = globalThis['opus-decoder'];

const [mode, base, a3, a4] = process.argv.slice(2);
let failures = 0;
const ok = (c, m) => { console.log(`${c ? 'OK  ' : 'FAIL'} ${m}`); if (!c) failures++; };

function parse(buf) {
  const dv = new DataView(buf);
  if (dv.getUint32(0, false) !== 0x4D585332) return null; // "MXS2"
  return {
    codec: dv.getUint8(4), channels: dv.getUint8(5), bits: dv.getUint8(6), quality: dv.getUint8(7),
    rate: dv.getUint32(8, true), frames: dv.getUint32(12, true), seq: dv.getUint32(16, true),
    payload: new Uint8Array(buf, 20),
  };
}

function opusPackets(payload) {
  const out = [];
  for (let p = 0; p + 2 <= payload.length;) {
    const n = payload[p] | (payload[p + 1] << 8); p += 2;
    out.push(payload.subarray(p, p + n)); p += n;
  }
  return out;
}

function listen(q, seconds, onPacket) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(base.replace(/^http/, 'ws') + 'ws?q=' + q);
    ws.binaryType = 'arraybuffer';
    const st = { q, bytes: 0, packets: 0, gaps: 0, lastSeq: -1, bad: 0, t0: 0, t1: 0, info: null };
    ws.onmessage = (ev) => {
      if (typeof ev.data === 'string') { st.info = JSON.parse(ev.data); return; }
      const h = parse(ev.data);
      if (!h) { st.bad++; return; }
      if (st.lastSeq >= 0 && h.seq !== st.lastSeq + 1) st.gaps++;
      st.lastSeq = h.seq;
      const now = performance.now();
      if (!st.t0) st.t0 = now; else st.bytes += ev.data.byteLength; // the first packet opens the window
      st.t1 = now;
      st.packets++;
      onPacket && onPacket(h);
    };
    ws.onerror = () => reject(new Error('WebSocket ' + q));
    ws.onopen = () => setTimeout(() => { ws.close(); resolve(st); }, seconds * 1000);
  });
}

// ------------------------------------------------------------------ exact --
if (mode === 'exact') {
  const q = a3, seconds = Number(a4 || 5);
  const bits = q === 'cd' ? 16 : 24;
  const M = 2 ** bits, half = M / 2;
  const dec = new FLACDecoder();
  await dec.ready;
  let chain = Promise.resolve(), n = null, checked = 0, errors = 0, decErrors = 0, hdrOk = true, rate = 0;
  const st = await listen(q, seconds, (h) => {
    hdrOk &&= h.codec === 2 && h.bits === bits && h.channels === 2 && h.quality === (q === 'cd' ? 0 : 1);
    rate = h.rate;
    chain = chain.then(async () => {
      const out = await dec.decodeFrames([h.payload]);
      decErrors += (out.errors || []).length;
      if (out.samplesDecoded !== h.frames) errors++;
      // Same correction as the page: the decoder divides by 2^(b-1)-1, go back to the exact integer.
      const [L, R] = out.channelData.map((c) => c.map((x) => Math.round(x * (half - 1)) / half));
      for (let i = 0; i < out.samplesDecoded; i++) {
        if (n === null) n = Math.round(L[i] * half) + half;
        const el = ((n % M) - half) / half;
        const er = (((n * 7919) % M) - half) / half;
        if (L[i] !== el || R[i] !== er) { if (errors++ < 3) console.log(`  mismatch @${checked}: ${L[i]} / ${el}  ${R[i]} / ${er}`); }
        n = (n + 1) % M; checked++;
      }
    });
  });
  await chain;
  const dur = (st.t1 - st.t0) / 1000;
  ok(hdrOk, `headers: FLAC ${bits}-bit, stereo, ${rate} Hz`);
  ok(st.gaps === 0 && st.bad === 0, `continuous sequence (${st.packets} packets)`);
  ok(decErrors === 0, `no decoding errors (${decErrors})`);
  ok(checked > rate * (seconds - 1.5), `samples decoded and checked: ${checked}`);
  ok(errors === 0, `bit-identical output (${errors} mismatch(es))`);
  console.log(`  bitrate: ${(st.bytes * 8 / dur / 1e6).toFixed(3)} Mbps`);
}

// ------------------------------------------------------------------- sine --
if (mode === 'sine') {
  const seconds = Number(a3 || 5);
  const dec = new OpusDecoder({ channels: 2, sampleRate: 48000 });
  await dec.ready;
  const L = [], R = [];
  let rate = 0, frames = 0, decoded = 0, hdrOk = true;
  const st = await listen('eco', seconds, (h) => {
    hdrOk &&= h.codec === 3 && h.quality === 2 && h.rate === 48000;
    rate = h.rate; frames += h.frames;
    for (const pkt of opusPackets(h.payload)) {
      const out = dec.decodeFrame(pkt);
      decoded += out.samplesDecoded;
      L.push(out.channelData[0]); R.push(out.channelData[1]);
    }
  });
  const cat = (arr) => { const n = arr.reduce((s, a) => s + a.length, 0); const o = new Float32Array(n); let p = 0; for (const a of arr) { o.set(a, p); p += a.length; } return o; };
  const l = cat(L).subarray(9600), r = cat(R).subarray(9600); // skip the first 200 ms
  const tone = (x, f) => { // share of the energy at frequency f (Goertzel)
    const w = 2 * Math.PI * f / 48000, c = 2 * Math.cos(w); let s1 = 0, s2 = 0, e = 0;
    for (let i = 0; i < x.length; i++) { const s0 = x[i] + c * s1 - s2; s2 = s1; s1 = s0; e += x[i] * x[i]; }
    const p = s1 * s1 + s2 * s2 - c * s1 * s2;
    return (2 * p / x.length) / e;
  };
  const rms = (x) => Math.sqrt(x.reduce((s, v) => s + v * v, 0) / x.length);
  const dur = (st.t1 - st.t0) / 1000;
  ok(hdrOk, `headers: Opus, 48000 Hz`);
  ok(st.gaps === 0, `continuous sequence (${st.packets} packets)`);
  ok(decoded === frames, `samples decoded = announced (${decoded})`);
  const tl = tone(l, 440), tr = tone(r, 660);
  ok(tl > 0.999 && tr > 0.999, `left = 440 Hz (${(tl * 100).toFixed(3)}%), right = 660 Hz (${(tr * 100).toFixed(3)}%)`);
  ok(Math.abs(rms(l) - 0.25 / Math.SQRT2) < 0.01, `level preserved: ${rms(l).toFixed(4)} (expected 0.1768)`);
  ok(Math.abs(st.bytes * 8 / dur - 256000) < 40000, `bitrate: ${(st.bytes * 8 / dur / 1000).toFixed(0)} kbps (target 256)`);
}

// ------------------------------------------------------------------ rates --
if (mode === 'rates') {
  const seconds = Number(a3 || 8);
  const res = await Promise.all(['cd', 'hires', 'eco'].map((q) => listen(q, seconds)));
  for (const st of res) {
    const dur = (st.t1 - st.t0) / 1000;
    console.log(`  ${st.q.padEnd(5)}: ${(st.bytes * 8 / dur / 1e6).toFixed(3)} Mbps  (${st.packets} packets, ${st.gaps} gap(s))`);
    ok(st.gaps === 0 && st.bad === 0, `${st.q}: continuous stream`);
  }
  const info = await (await fetch(base + 'info')).json();
  console.log('  /info:', JSON.stringify(info));
}

console.log(failures ? `\n${failures} test(s) failed` : '\nAll tests pass.');
process.exit(failures ? 1 : 0);
