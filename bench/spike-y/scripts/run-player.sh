#!/usr/bin/env bash
# Spike Y repeat — ship an aarch64 blytplay + the .blyt workloads to a target
# (e.g. the Pi Zero 2 W) and time each cart's per-frame cost there.
#
#   ./run-player.sh <target> <player-binary> [remote-subdir] [cart...]
#
# Defaults to the shipped host-Lua player (dist/blytplay-aarch64) and every cart
# under cartprojects/. Pass extra files to ship (e.g. an emulated player's
# sdk/lib guest libraries) via EXTRA_FILES / EXTRA_DIR.
set -euo pipefail
cd "$(dirname "$0")/.."          # bench/spike-y
TARGET="${1:?usage: run-player.sh <target> <player-binary> [remote-subdir] [cart...]}"
PLAYER="${2:-dist/blytplay-aarch64}"
SUB="${3:-hostlua}"
shift $(( $# > 3 ? 3 : $# ))
CARTS=("$@")
if [ ${#CARTS[@]} -eq 0 ]; then
  CARTS=()
  for c in cartprojects/*/build/*.blyt; do CARTS+=("$c"); done
fi
REMOTE="spike-y/$SUB"

ssh "$TARGET" "mkdir -p ~/$REMOTE"
scp -q "$PLAYER" "$TARGET:~/$REMOTE/blytplay"
scp -q scripts/bench-player.sh "$TARGET:~/$REMOTE/"
scp -q "${CARTS[@]}" "$TARGET:~/$REMOTE/"
if [ -n "${EXTRA_DIR:-}" ]; then
  ssh "$TARGET" "mkdir -p ~/$REMOTE/lib"
  scp -q "$EXTRA_DIR"/*.so "$TARGET:~/$REMOTE/lib/"
fi

names=$(for c in "${CARTS[@]}"; do basename "$c"; done | tr '\n' ' ')
ssh "$TARGET" "cd ~/$REMOTE && chmod +x blytplay bench-player.sh && \
  ${EXTRA_ENV:-} ROUNDS=${ROUNDS:-3} TARGET_US=${TARGET_US:-4000000} \
  ./bench-player.sh ./blytplay $names"
