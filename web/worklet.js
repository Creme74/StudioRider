// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The StudioWire authors
//
// StudioWire — AudioWorklet player.
//
// Receives already-decoded PCM blocks (Float32, left/right), stores them in a
// ring buffer and plays them as they are: in normal operation every output
// sample is exactly the one received (no resampling, no interpolation). The
// only times the signal is touched: a 3 ms fade-in when playback resumes after
// a dropout, and a 5 ms crossfade if the buffer has grown too much (clock drift
// or a network burst) and has to be brought back to the target.

class StudioWirePlayer extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const o = (options && options.processorOptions) || {};
    this.cap = Math.max(4096, o.capacity | 0 || Math.ceil(sampleRate * 8));
    this.L = new Float32Array(this.cap);
    this.R = new Float32Array(this.cap);
    this.w = 0;            // frames written (absolute counter)
    this.r = 0;            // frames read (absolute counter)
    this.target = Math.max(128, o.target | 0 || Math.round(sampleRate * 0.5));
    this.playing = false;
    this.FADE = Math.round(sampleRate * 0.003);
    this.fade = 0;
    this.xf = null;
    this.under = 0;
    this.jumps = 0;
    this.pkL = 0;
    this.pkR = 0;
    this.tick = 0;
    this.port.onmessage = (e) => this.onMessage(e.data);
  }

  onMessage(m) {
    if (!m) return;
    if (m.type === 'pcm') this.push(m.l, m.r);
    else if (m.type === 'target') this.target = Math.max(128, m.frames | 0);
    else if (m.type === 'flush') { this.r = this.w; this.playing = false; this.xf = null; }
  }

  push(l, r) {
    if (!(l instanceof Float32Array)) return;
    if (!(r instanceof Float32Array) || r.length !== l.length) r = l;
    const frames = l.length;
    if (frames === 0) return;

    // Overflow: drop the oldest.
    if (this.w + frames - this.r > this.cap) this.r = this.w + frames - this.cap;

    const cap = this.cap;
    let idx = this.w % cap;
    let i = 0;
    while (i < frames) {
      const n = Math.min(frames - i, cap - idx);
      this.L.set(l.subarray(i, i + n), idx);
      this.R.set(r.subarray(i, i + n), idx);
      i += n;
      idx = (idx + n) % cap;
    }
    this.w += frames;
  }

  process(inputs, outputs) {
    const out = outputs[0];
    const oL = out[0];
    const oR = out.length > 1 ? out[1] : null;
    const n = oL.length;
    let i = 0;

    if (!this.playing && this.w - this.r >= this.target) {
      this.playing = true;
      this.fade = this.FADE;
    }

    if (this.playing) {
      const fill = this.w - this.r;
      const high = this.target * 2 + Math.round(sampleRate * 0.5);
      if (fill > high && !this.xf) {
        const from = this.r;
        this.r = this.w - this.target;
        this.xf = { from, pos: 0, len: Math.round(sampleRate * 0.005) };
        this.jumps++;
      }

      const cap = this.cap, L = this.L, R = this.R;
      for (; i < n; i++) {
        if (this.r >= this.w) { this.playing = false; this.under++; this.xf = null; break; }
        const k = this.r % cap;
        let l = L[k], r = R[k];
        const xf = this.xf;
        if (xf) {
          const g = (xf.pos + 1) / (xf.len + 1);
          const j = (xf.from + xf.pos) % cap;
          l = l * g + L[j] * (1 - g);
          r = r * g + R[j] * (1 - g);
          if (++xf.pos >= xf.len) this.xf = null;
        }
        if (this.fade > 0) {
          const g = 1 - this.fade / this.FADE;
          l *= g; r *= g;
          this.fade--;
        }
        oL[i] = l;
        if (oR) oR[i] = r;
        const al = l < 0 ? -l : l, ar = r < 0 ? -r : r;
        if (al > this.pkL) this.pkL = al;
        if (ar > this.pkR) this.pkR = ar;
        this.r++;
      }
    }

    for (; i < n; i++) { oL[i] = 0; if (oR) oR[i] = 0; }

    if (++this.tick >= Math.max(1, Math.round(sampleRate / n / 12))) {
      this.tick = 0;
      this.port.postMessage({
        fill: this.w - this.r, playing: this.playing, under: this.under,
        jumps: this.jumps, pkL: this.pkL, pkR: this.pkR,
      });
      this.pkL = 0;
      this.pkR = 0;
    }
    return true;
  }
}

registerProcessor('studiowire-player', StudioWirePlayer);
