#!/usr/bin/env bash
# Shared config + toolchain discovery for the Spike A harness.
# Sourced by the other scripts; not run directly.

set -euo pipefail

SPIKE_A_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEPS_DIR="$SPIKE_A_ROOT/.deps"
ART_DIR="$SPIKE_A_ROOT/artifacts"
GUEST_OUT="$ART_DIR/guest"
mkdir -p "$DEPS_DIR" "$GUEST_OUT"

# --- Pinned third-party versions (match blyt root CMakeLists.txt) -----------
COREMARK_REPO="https://github.com/eembc/coremark"
COREMARK_COMMIT="1f483d5b8316753a742cbf5590caf5bd0a4e4777" # v1.01-era
EMBENCH_REPO="https://github.com/embench/embench-iot"
EMBENCH_COMMIT="09c2ed8c3b7008c95d08b038de4a3f6dc103ed70" # pinned
MUSL_VERSION="v1.2.6-blyt-v0-p1"
MUSL_URL="https://github.com/blyt-tech/musl/archive/refs/tags/${MUSL_VERSION}.tar.gz"

# --- Guest ISA/ABI: the real blyt cart target (Spike U) ---------------------
GUEST_MARCH="rv32imafdc"
GUEST_MABI="ilp32d"
GUEST_TRIPLE="riscv32-linux-gnu"

# --- Toolchain discovery (override via env) ---------------------------------
find_tool() {
  # $1 = env var name, $2.. = candidate paths / bare names
  local var="$1"
  shift
  if [ -n "${!var:-}" ]; then
    echo "${!var}"
    return
  fi
  for c in "$@"; do
    if command -v "$c" >/dev/null 2>&1; then
      command -v "$c"
      return
    fi
  done
  echo ""
}

CLANG="$(find_tool BLYT_CLANG /opt/homebrew/opt/llvm/bin/clang clang-22 clang)"
LLD="$(find_tool BLYT_LLD /opt/homebrew/opt/lld/bin/ld.lld ld.lld-22 ld.lld)"
LLVM_AR="$(find_tool BLYT_AR /opt/homebrew/opt/llvm/bin/llvm-ar llvm-ar-22 llvm-ar)"
LLVM_RANLIB="$(find_tool BLYT_RANLIB /opt/homebrew/opt/llvm/bin/llvm-ranlib llvm-ranlib-22 llvm-ranlib)"

require_guest_toolchain() {
  if [ -z "$CLANG" ] || [ -z "$LLD" ]; then
    echo "error: need clang + ld.lld that can target $GUEST_TRIPLE." >&2
    echo "  brew install llvm lld   (or set BLYT_CLANG / BLYT_LLD)" >&2
    exit 1
  fi
  RESINC="$("$CLANG" -print-resource-dir)/include"
}

# Guest cross-compile flag arrays (bash arrays; expand with "${GUEST_TGT[@]}").
guest_flags() {
  GUEST_TGT=(--target="$GUEST_TRIPLE" -march="$GUEST_MARCH" -mabi="$GUEST_MABI"
    -fuse-ld="$LLD")
}

log() { printf '\033[1;36m[spike-a]\033[0m %s\n' "$*"; }
