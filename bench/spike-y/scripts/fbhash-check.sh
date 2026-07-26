#!/usr/bin/env bash
# Spike Y repeat — cross-leg correctness check: run every workload for N frames
# under both players on the target and diff the per-frame [blyt:fbhash] streams.
# The carts write c=(x+y+t)&0xFF over a W×H region, so an identical hash stream
# means both legs produced byte-identical framebuffers (#188/#204 oracle).
#
#   ./fbhash-check.sh <target> [frames]
set -euo pipefail
cd "$(dirname "$0")/.."
TARGET="${1:?usage: fbhash-check.sh <target> [frames]}"
N="${2:-5}"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

status=0
for cart in cartprojects/*/build/*.blyt; do
  name=$(basename "$cart")
  ssh "$TARGET" "cd ~/spike-y/hostlua && BLYT_FRAME_HASH=1 ./blytplay --headless --quit-after $N $name" \
    2>/dev/null | grep -E '^\[blyt:(fb|pal)hash\]' >"$tmp/hostlua" || true
  ssh "$TARGET" "cd ~/spike-y/emulated && BLYT_LIB_DIR=\$PWD/lib BLYT_FRAME_HASH=1 ./blytplay --headless --quit-after $N $name" \
    2>/dev/null | grep -E '^\[blyt:(fb|pal)hash\]' >"$tmp/emulated" || true
  if [ ! -s "$tmp/hostlua" ]; then
    echo "$name: FAIL (no host-Lua hashes)"; status=1; continue
  fi
  if diff -q "$tmp/hostlua" "$tmp/emulated" >/dev/null 2>&1; then
    echo "$name: MATCH ($(wc -l <"$tmp/hostlua" | tr -d ' ') hash lines, $(head -1 "$tmp/hostlua" | awk '{print $2}') …)"
  else
    echo "$name: MISMATCH"; diff "$tmp/hostlua" "$tmp/emulated" | head -6; status=1
  fi
done
exit $status
