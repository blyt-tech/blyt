#!/usr/bin/env bash
# Build the Spike A runner natively for THIS host (the dev Mac) via the
# self-contained host/CMakeLists.txt. Uses the repo's already-extracted rv32emu
# source if present, else fetches the pinned tarball.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

BUILD_DIR="$ART_DIR/host-build"
OUT="$ART_DIR/host"
mkdir -p "$OUT"
EMU_OPT="${SPIKE_A_EMU_OPT:--O2}"

# Always configure from clean: rv32emu is sensitive to the exact compile flags
# (needs -fno-strict-aliasing at -O2/-O3), and reusing a stale cache across opt
# changes has produced inconsistent objects.
rm -rf "$BUILD_DIR"

# Prefer the rv32emu source the repo already fetched (build/_deps), else let
# CMake fetch the pinned tarball itself.
RV32EMU_SRC_ARG=()
repo_rv32emu="$SPIKE_A_ROOT/../../build/_deps/rv32emu-src"
if [ -f "$repo_rv32emu/src/riscv.c" ]; then
  RV32EMU_SRC_ARG=(-DRV32EMU_SRC="$(cd "$repo_rv32emu" && pwd)")
  log "using repo rv32emu source"
else
  log "repo rv32emu source not found; CMake will fetch the pinned tarball"
fi

cmake -S "$SPIKE_A_ROOT/host" -B "$BUILD_DIR" -G Ninja \
  -DSPIKE_A_EMU_OPT="$EMU_OPT" "${RV32EMU_SRC_ARG[@]}" >/dev/null
cmake --build "$BUILD_DIR" >/dev/null
cp "$BUILD_DIR/runner" "$OUT/runner"
# macOS: cp invalidates the linker-applied ad-hoc code signature, which can get
# the binary SIGKILL'd by AMFI on launch. Re-sign the copy (no-op elsewhere).
if command -v codesign >/dev/null 2>&1; then
  codesign -f -s - "$OUT/runner" 2>/dev/null || true
fi
log "built host runner at $OUT/runner (interpreter opt $EMU_OPT)"
