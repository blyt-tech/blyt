#!/usr/bin/env bash
# Cross-build the Spike A runner for the Raspberry Pi Zero 2 W (64-bit Trixie /
# aarch64) from this Mac, using a linux/arm64 Docker container. The runner is
# linked statically so it is a single drop-on-and-run binary on the Pi — no
# toolchain or library install needed there. The guest benchmark ELFs are
# host-independent (build them once with build-guest.sh / build-embench.sh).
#
# Output: artifacts/pi/runner  (aarch64 static ELF)
# Copy artifacts/pi/runner + artifacts/guest/*.elf + scripts/ to the Pi and run
#   ./runner <bench>.elf --json   (or use run.sh --runner ./runner).

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

EMU_OPT="${SPIKE_A_EMU_OPT:--O2}"
REPO_ROOT="$(cd "$SPIKE_A_ROOT/../.." && pwd)"
RV32EMU_REL="build/_deps/rv32emu-src"
OUT="$ART_DIR/pi"
mkdir -p "$OUT"

command -v docker >/dev/null || {
  echo "error: docker not found (needed for the linux/arm64 cross-build)" >&2
  exit 1
}
[ -f "$REPO_ROOT/$RV32EMU_REL/src/riscv.c" ] || {
  echo "error: rv32emu source not found at $REPO_ROOT/$RV32EMU_REL" >&2
  echo "       run 'cmake -B build -G Ninja' in the repo root first." >&2
  exit 1
}

log "cross-building runner for linux/arm64 (Trixie) in Docker (opt $EMU_OPT, static)"
docker run --rm --platform linux/arm64 \
  -v "$REPO_ROOT":/work -w /work/bench/spike-a \
  debian:trixie-slim bash -eu -c "
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq >/dev/null
    apt-get install -y -qq --no-install-recommends \
      cmake ninja-build gcc g++ make libc6-dev ca-certificates >/dev/null
    rm -rf artifacts/pi-build
    cmake -S host -B artifacts/pi-build -G Ninja \
      -DSPIKE_A_STATIC=ON -DSPIKE_A_EMU_OPT='$EMU_OPT' \
      -DRV32EMU_SRC=/work/$RV32EMU_REL >/dev/null
    cmake --build artifacts/pi-build >/dev/null
    cp artifacts/pi-build/runner artifacts/pi/runner
  "
log "Pi runner at $OUT/runner"
file "$OUT/runner" 2>/dev/null | sed 's/^/[spike-a]   /' || true
