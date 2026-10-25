// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The StudioRider authors
//
// Checks the server's HTTP surface (security and served files).
// Usage: node tests/check_http.mjs <local-url>
const base = process.argv[2];
const u = new URL(base);
const origin = `${u.protocol}//${u.host}`;
let failures = 0;
const ok = (c, m) => { console.log(`${c ? 'OK  ' : 'FAIL'} ${m}`); if (!c) failures++; };

for (const [path, type] of [['', 'text/html'], ['app.js', 'text/javascript'], ['worklet.js', 'text/javascript'],
                            ['style.css', 'text/css'], ['flac-decoder.min.js', 'text/javascript'], ['opus-decoder.min.js', 'text/javascript']]) {
  const r = await fetch(base + path);
  ok(r.status === 200 && (r.headers.get('content-type') || '').startsWith(type), `GET /…/${path} → ${r.status} ${r.headers.get('content-type')}`);
  if (path === '') {
    const csp = r.headers.get('content-security-policy') || '';
    ok(csp.includes("default-src 'self'") && csp.includes("'wasm-unsafe-eval'") && !csp.includes("'unsafe-eval'"),
       "CSP : 'self' + WebAssembly uniquement");
  }
}
const info = await (await fetch(base + 'info')).json();
ok(info.maxQuality === 'cd' && info.byQuality && 'eco' in info.byQuality, `/info → ${JSON.stringify(info)}`);
for (const p of ['/', '/index.html', '/wrongtoken/', '/wrongtoken/info', u.pathname.replace(/\/$/, '') + 'x/', '/../etc/passwd', u.pathname + '../app.js']) {
  const r = await fetch(origin + p);
  ok(r.status === 404, `GET ${p} → ${r.status} (expected 404)`);
}
ok((await fetch(base + 'info', { method: 'POST', body: 'x' })).status === 405, 'POST refused (405)');
ok((await fetch(base.replace(/\/$/, ''), { redirect: 'manual' })).status === 302, 'no trailing "/" → redirect');

await new Promise((resolve) => {
  const bad = new WebSocket(origin.replace('http', 'ws') + '/wrongtoken/ws');
  bad.onopen = () => { ok(false, 'WebSocket outside the token refused'); bad.close(); resolve(); };
  bad.onerror = () => { ok(true, 'WebSocket outside the token refused'); resolve(); };
});

// Above the stream quality (CD by default in the harness) → brought down to CD.
await new Promise((resolve) => {
  const s = new WebSocket(base.replace(/^http/, 'ws') + 'ws?q=hires');
  s.binaryType = 'arraybuffer';
  const t = setTimeout(() => { ok(false, 'Hi-Res refused when the stream is CD'); s.close(); resolve(); }, 3000);
  s.onmessage = (ev) => {
    if (typeof ev.data === 'string') return;
    const dv = new DataView(ev.data);
    ok(dv.getUint8(6) === 16 && dv.getUint8(7) === 0, 'Hi-Res requested, stream is CD → gets CD (16-bit)');
    clearTimeout(t); s.close(); resolve();
  };
});

// Unknown quality → the stream quality (CD here).
await new Promise((resolve) => {
  const s = new WebSocket(base.replace(/^http/, 'ws') + 'ws?q=n%27importe%20quoi');
  s.binaryType = 'arraybuffer';
  const t = setTimeout(() => { ok(false, 'unknown quality → CD'); s.close(); resolve(); }, 3000);
  s.onmessage = (ev) => {
    if (typeof ev.data === 'string') return;
    const dv = new DataView(ev.data);
    ok(dv.getUint8(4) === 2 && dv.getUint8(6) === 16 && dv.getUint8(7) === 0, 'unknown quality → CD (FLAC 16-bit)');
    clearTimeout(t); s.close(); resolve();
  };
});

console.log(failures ? `\n${failures} test(s) failed` : '\nAll tests pass.');
process.exit(failures ? 1 : 0);
