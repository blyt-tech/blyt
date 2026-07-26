#!/usr/bin/env bash
# Spike Y repeat — generate blyt cart projects for the shipped-API per-pixel
# workloads and build them into .blyt carts.
#
#   ./make-carts.sh [outdir]        (default: bench/spike-y/cartprojects)
#
# The Spike Y `carts/*.lua` sources target a *hypothetical* API (surface
# set_pixel / pset globals, and `lk[i]=c` index assignment). The SHIPPED tier-2
# API (#205/#208) exposes only the lock userdata with get/set/clear/rect_fill/
# line/release, so the workloads that survive unchanged are:
#
#   method  lk:set(x,y,c)                 — the shipped 2a form (OP_SELF+OP_CALL)
#   pset    local set = lk.set; set(...)  — same C function, plain call, no OP_SELF
#
# plus `floor` (acquire, ONE pixel, release) which measures the per-frame runtime
# floor so the per-pixel cost can be reported net of frame overhead. It has to
# plot at least one pixel: a cart that never draws keeps the PM5544 test card
# alive, and the runtime re-renders that (76 800 px of C) every frame — a
# no-pixel "noop" cart measures the test card, not the frame floor.
#
# Every cart plots the same W×H region with c=(x+y+t)&0xFF, exactly as the
# Spike Y workloads do, so the framebuffer is comparable across legs
# (BLYT_FRAME_HASH=1 emits [blyt:fbhash] per frame).
set -euo pipefail
cd "$(dirname "$0")/.."          # bench/spike-y
REPO="${REPO:-$(cd ../.. && pwd)}"   # override to build the carts with another tree's SDK
OUT="${1:-$PWD/cartprojects}"
BLYT="${BLYT:-$REPO/build/sdk/bin/blyt}"
[ -x "$BLYT" ] || { echo "error: $BLYT not found — build the sdk target first"; exit 1; }

mkdir -p "$OUT"

emit_project() {          # emit_project <name> <W> <H> <body-template>
  local name="$1" w="$2" h="$3" form="$4"
  local dir="$OUT/$name"
  mkdir -p "$dir/src/game/lua"
  cat >"$dir/blyt.info.yaml" <<EOF
id: $name
title: Spike Y $name
version: 0.1.0
EOF
  {
    echo "-- Spike Y repeat: per-pixel throughput workload ($form, ${w}x${h})."
    echo "local W, H = $w, $h"
    echo "local t = 0"
    echo "function init() end"
    echo "function update() t = t + 1 end"
    echo "function draw()"
    echo "    local lk = blyt32.surface.acquire(blyt32.surface.SCREEN)"
    case "$form" in
      method)
        echo "    for y = 0, H - 1 do"
        echo "        for x = 0, W - 1 do"
        echo "            lk:set(x, y, (x + y + t) & 0xFF)"
        echo "        end"
        echo "    end"
        ;;
      pset)
        echo "    local set = lk.set"
        echo "    for y = 0, H - 1 do"
        echo "        for x = 0, W - 1 do"
        echo "            set(lk, x, y, (x + y + t) & 0xFF)"
        echo "        end"
        echo "    end"
        ;;
      floor)
        echo "    lk:set(0, 0, t & 0xFF)"
        ;;
    esac
    echo "    lk:release()"
    echo "end"
  } >"$dir/src/game/lua/main.lua"
}

for form in method pset; do
  emit_project "$form-1k" 32 32 "$form"
  emit_project "$form-4k" 64 64 "$form"
  emit_project "$form-10k" 100 100 "$form"
done
emit_project "floor" 1 1 floor

for d in "$OUT"/*/; do
  name="$(basename "$d")"
  BLYT_SDK_DIR="$REPO/build/sdk" "$BLYT" build "$d" >/dev/null
  echo "built $name → $d/build/$name.blyt"
done
