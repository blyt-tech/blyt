#!/usr/bin/env bash
# Spike Y — run the native Lua per-pixel bench on a target (e.g. a Pi Zero 2 W).
# Copies native_bench + the workload carts over and times each one.
#
#   ./run-native.sh pi@pizero.local [frames]
#
# Each line prints: <cart> <ms/frame> (checksum). The workload plots a W×H region
# per frame (1k≈32², 4k=64², 10k=100²); a cart under the 16.67 ms 60 Hz budget
# "fits" that many px/frame. The runner writes to a throwaway framebuffer, so
# there is no display dependency.
set -euo pipefail
cd "$(dirname "$0")/.."
TARGET="${1:?usage: run-native.sh user@host [frames]}"
FRAMES="${2:-2000}"

[ -x native_bench ] || { echo "error: native_bench not built — run scripts/build-native.sh first"; exit 1; }

ssh "$TARGET" 'mkdir -p ~/spike-y'
scp -q native_bench carts/*.lua "$TARGET:~/spike-y/"
ssh "$TARGET" "cd ~/spike-y && chmod +x native_bench && \
  for c in method setpixel index pset; do for n in 1k 4k 10k; do \
    [ -f \$c-\$n.lua ] && printf '%-14s ' \"\$c-\$n\" && ./native_bench \$c-\$n.lua $FRAMES; \
  done; done"
