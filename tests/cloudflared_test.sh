#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The StudioWire authors
#
# Runs tests/cloudflared_test against a local fake GitHub release server.
# Usage: tests/cloudflared_test.sh <path to studiowire_cloudflared_test>
set -euo pipefail
TEST_BIN="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
work="$(mktemp -d)"
trap 'kill $server 2>/dev/null || true; rm -rf "$work"' EXIT

asset="cloudflared-darwin-arm64.tgz"      # exercises the .tgz path (unpacked with /usr/bin/tar)
mkdir -p "$work/bundled" "$work/support" "$work/pkg" "$work/pkg2"

# A fake "bundled" cloudflared, old version
printf '#!/bin/sh\necho "cloudflared version 2025.1.0 (built 2025-01-10T00:00:00Z)"\n' > "$work/bundled/cloudflared"
chmod +x "$work/bundled/cloudflared"

# Release 2099.1.0, like Cloudflare's real releases: the release notes give the
# checksum of the program INSIDE the .tgz, GitHub's "digest" the one of the .tgz.
printf '#!/bin/sh\necho "cloudflared version 2099.1.0 (built 2099-01-01T00:00:00Z)"\n' > "$work/pkg/cloudflared"
chmod +x "$work/pkg/cloudflared"
tar -czf "$work/v1.tgz" -C "$work/pkg" cloudflared
exe_sha="$(sha256sum "$work/pkg/cloudflared" | cut -d' ' -f1)"
tgz_sha="$(sha256sum "$work/v1.tgz" | cut -d' ' -f1)"

# Release 2099.2.0, other style: the notes give the checksum of the .tgz, no digest.
printf '#!/bin/sh\necho "cloudflared version 2099.2.0 (built 2099-02-01T00:00:00Z)"\n' > "$work/pkg2/cloudflared"
chmod +x "$work/pkg2/cloudflared"
tar -czf "$work/v2.tgz" -C "$work/pkg2" cloudflared
v2_sha="$(sha256sum "$work/v2.tgz" | cut -d' ' -f1)"

zero="0000000000000000000000000000000000000000000000000000000000000000"

cat > "$work/server.mjs" <<JS
import http from 'node:http';
import { readFileSync } from 'node:fs';
const files = { '/dl/v1/$asset': readFileSync('$work/v1.tgz'), '/dl/v2/$asset': readFileSync('$work/v2.tgz') };
const release = (tag, file, notesSha, digest) => JSON.stringify({
  tag_name: tag,
  assets: [{ name: '$asset', browser_download_url: 'http://127.0.0.1:' + server.address().port + file,
             ...(digest ? { digest: 'sha256:' + digest } : {}) }],
  body: 'Some notes\n\n### SHA256 Checksums:\n\`\`\`\n' + (notesSha ? '$asset: ' + notesSha + '\n' : '')
        + 'other.tgz: ' + 'a'.repeat(64) + '\n\`\`\`\n',
});
const server = http.createServer((req, res) => {
  const routes = {
    '/good':      () => release('2099.1.0', '/dl/v1/$asset', '$exe_sha', '$tgz_sha'),
    '/tgzsha':    () => release('2099.2.0', '/dl/v2/$asset', '$v2_sha', ''),
    '/bad':       () => release('2100.1.0', '/dl/v1/$asset', '$zero', '$tgz_sha'),
    '/baddigest': () => release('2100.2.0', '/dl/v1/$asset', '$exe_sha', '$zero'),
    '/nosha':     () => release('2101.1.0', '/dl/v1/$asset', '', ''),
  };
  if (routes[req.url]) { res.writeHead(200, { 'content-type': 'application/json' }); res.end(routes[req.url]()); return; }
  const f = files[req.url];
  if (f) { res.writeHead(200, { 'content-type': 'application/gzip', 'content-length': f.length }); res.end(f); return; }
  res.writeHead(404); res.end('not found');
});
server.listen(0, '127.0.0.1', () => console.log(server.address().port));
JS
node "$work/server.mjs" > "$work/port" &
server=$!
for _ in $(seq 50); do [[ -s "$work/port" ]] && break; sleep 0.1; done
port="$(cat "$work/port")"

cd "$work"
env PATH=/usr/bin:/bin \
    STUDIOWIRE_TEST_SERVER="http://127.0.0.1:$port" \
    STUDIOWIRE_CLOUDFLARED_ASSET="$asset" \
    STUDIOWIRE_SUPPORT_DIR="$work/support" \
    STUDIOWIRE_BUNDLED_DIR="$work/bundled" \
    "$TEST_BIN"
