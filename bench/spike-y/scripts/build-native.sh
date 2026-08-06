#!/usr/bin/env bash
# Spike Y — build the native aarch64 Lua per-pixel bench runner.
#
# Links the runner against the blyt-tech/lua fork's onelua.c compiled native
# aarch64 (no rv32emu layer) — this measures Lua running natively, i.e. what
# blyt's host-Lua fast path does.
#
# Requires the fork checked out at $FORK (default third_party/lua). For the
# PATCHED (fast paths) variant, apply the VM patches from branch
# `spike/208-lua-vm-fastpixel` to the fork first (see README.md); for the
# baseline variant, plain upstream-fork is enough and FASTPIXEL is a no-op.
set -euo pipefail
cd "$(dirname "$0")/../../.."   # repo root
FORK="${FORK:-third_party/lua}"
FASTPIXEL="${FASTPIXEL:-1}"     # 1 = patched fast paths, 0 = baseline

[ -f "$FORK/onelua.c" ] || { echo "error: $FORK/onelua.c not found — clone the lua fork (see README)"; exit 1; }

docker run --platform linux/arm64 --rm -v "$PWD:/repo" debian:trixie bash -c "
  set -e
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq && apt-get install -y -qq clang >/dev/null
  clang -O2 -fno-strict-aliasing -I /repo/$FORK \
    -DMAKE_LIB=1 -DBLYT_LUA_I32_F64=1 -DBLYT_LUA_FASTPIXEL=$FASTPIXEL -DLUA_USE_LONGJMP=1 \
    '-Dluai_makeseed()=0x424C5954u' \
    /repo/bench/spike-y/runner.c /repo/$FORK/onelua.c -lm \
    -o /repo/bench/spike-y/native_bench
"
echo "built bench/spike-y/native_bench (aarch64, FASTPIXEL=$FASTPIXEL)"
