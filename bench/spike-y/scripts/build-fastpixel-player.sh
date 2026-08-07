#!/usr/bin/env bash
# Spike Y repeat — build the two EXPERIMENTAL aarch64 players that measure the
# #208 per-pixel VM fast paths through the real host-Lua runtime:
#
#   blytplay-bench-aarch64     BLYT_HOSTLUA_PIXEL_BENCH: adds the implicit
#                              blyt32.surface.set_pixel binding, stock VM
#   blytplay-fastpixel-aarch64 the same PLUS the patched VM (BLYT_LUA_FASTPIXEL:
#                              fast OP_SELF + OP_CALL-inline for lk:set/lk:get
#                              and set_pixel)
#
# Both link the p4 fork with ONLY the two method-dispatch hunks of
# `spike/208-lua-vm-fastpixel` applied (the OP_GETTABLE/OP_SETTABLE `lk[i]=c`
# hunks are deliberately left out — that form is a cart-visible API change, not
# a dispatch optimisation). Prepare the fork with:
#
#   cp -R build/_deps/lua-src bench/spike-y/dist/lua-fastpixel-method
#   (cd bench/spike-y/dist/lua-fastpixel-method && patch -p1 < method-dispatch.diff)
#
# NEITHER IS A SHIPPABLE CONFIGURATION — see REPEAT-RESULTS.md.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
FORK="/repo/bench/spike-y/dist/lua-fastpixel-method"

[ -f "$REPO/bench/spike-y/dist/lua-fastpixel-method/onelua.c" ] || {
  echo "error: patched fork not at bench/spike-y/dist/lua-fastpixel-method (see header)"; exit 1; }

EXTRA_CMAKE="-DFETCHCONTENT_SOURCE_DIR_LUA=$FORK" \
EXTRA_DEFS="-DBLYT_HOSTLUA_PIXEL_BENCH=1" \
VOL_SUFFIX="-bench" \
  "$HERE/build-aarch64-player.sh" "$REPO" blytplay-bench-aarch64

EXTRA_CMAKE="-DFETCHCONTENT_SOURCE_DIR_LUA=$FORK" \
EXTRA_DEFS="-DBLYT_HOSTLUA_PIXEL_BENCH=1 -DBLYT_LUA_FASTPIXEL=1" \
VOL_SUFFIX="-fastpixel" \
  "$HERE/build-aarch64-player.sh" "$REPO" blytplay-fastpixel-aarch64
