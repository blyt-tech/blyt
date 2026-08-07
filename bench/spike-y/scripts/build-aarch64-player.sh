#!/usr/bin/env bash
# Spike Y repeat — cross-build an aarch64 (linux/arm64) blytplay for the Pi Zero 2 W.
#
#   ./build-aarch64-player.sh [repo-root] [out-name]
#
# Builds inside a `debian:trixie` linux/arm64 container (Dockerfile.arm64) — the
# Pi runs Debian 13 trixie, so the container's glibc/SDL2 exactly match the
# target. The build tree lives in a named docker volume (blyt-aarch64-build-<repo>)
# so repeat runs are incremental and the FetchContent tarballs download once.
#
# The emulator core is pinned to `-O2 -fno-strict-aliasing` — the repo default
# (no CMAKE_BUILD_TYPE) is -O0, which makes rv32emu ~4x slower and silently
# measures the wrong floor (Spike Y's secondary finding). -O3 miscompiles rv32emu
# unless -fno-strict-aliasing is set; with it, -O3 == -O2.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="${1:-$(cd "$HERE/../../.." && pwd)}"
OUT="${2:-blytplay-aarch64}"
VOL="blyt-aarch64-build-$(basename "$REPO")${VOL_SUFFIX:-}"
DEST="$HERE/../dist"
mkdir -p "$DEST"

docker build --platform linux/arm64 -t blyt-spike-y-arm64 -f "$HERE/Dockerfile.arm64" "$HERE" >/dev/null
docker volume create "$VOL" >/dev/null

docker run --platform linux/arm64 --rm \
  -v "$REPO:/repo" -v "$VOL:/build" -v "$DEST:/out" \
  blyt-spike-y-arm64 bash -euc "
    cmake -B /build -G Ninja -S /repo \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_C_FLAGS='-O2 -fno-strict-aliasing ${EXTRA_DEFS:-}' \
      -DCMAKE_CXX_FLAGS='-O2 -fno-strict-aliasing' ${EXTRA_CMAKE:-}
    cmake --build /build --target blytplay
    cp /build/sdk/bin/blytplay /out/$OUT
    file /out/$OUT
  "
echo "built $DEST/$OUT"
