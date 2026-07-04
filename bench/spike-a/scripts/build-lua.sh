#!/usr/bin/env bash
# Build the Lua-VM throughput ELF (a Spike B probe off this harness): the blyt
# Lua 5.4 interpreter compiled to the real cart ISA (rv32imafdc / ilp32d) with
# the cart numeric model (BLYT_LUA_I32_F64) and fixed hash seed, running a
# steady-state entity update() workload. Output:
#   bench/spike-a/artifacts/guest/lua-bench.elf
#
# Uses the SAME pinned blyt Lua source the runtime ships (build/_deps/lua-src),
# built single-TU via onelua.c, linked against static musl (build-musl.sh).

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
require_guest_toolchain
guest_flags

MUSL_INSTALL="$DEPS_DIR/musl-install"
PORT="$SPIKE_A_ROOT/guest/lua-port"
REPO_ROOT="$(cd "$SPIKE_A_ROOT/../.." && pwd)"

# Locate the pinned Lua source the repo already fetched.
LUA_SRC=""
for c in "$REPO_ROOT/build/_deps/lua-src" "$DEPS_DIR/lua-src"; do
  [ -f "$c/onelua.c" ] && LUA_SRC="$(cd "$c" && pwd)" && break
done
[ -n "$LUA_SRC" ] || {
  echo "error: blyt Lua source (onelua.c) not found; run 'cmake -B build' in the repo root" >&2
  exit 1
}
log "using Lua source at $LUA_SRC"

# Berkeley SoftFloat source (from the rv32emu tree) — needed for the guest quad
# soft-float ABI builtins (__multf3/__getf2/…) that musl's strtod/printf drag in
# on ilp32d, exactly as blyt's guest Lua links them (blyt_guest_libs.cmake).
SF_SRC="$REPO_ROOT/build/_deps/rv32emu-src/src/softfloat/source"
SF_BUILTINS="$REPO_ROOT/runtime/guest/src/libblyt32lua/softfloat_builtins.c"
[ -f "$SF_SRC/f64_add.c" ] && [ -f "$SF_BUILTINS" ] || {
  echo "error: guest SoftFloat source / builtins not found; run 'cmake -B build'" >&2
  exit 1
}

# Ensure static musl exists.
if [ ! -f "$MUSL_INSTALL/lib/libc.a" ]; then
  "$(dirname "${BASH_SOURCE[0]}")/build-musl.sh"
fi

work="$DEPS_DIR/lua-build"
rm -rf "$work"
mkdir -p "$work"
cd "$work"

# --- Guest SoftFloat archive (f128 quad builtins for musl strtod/printf) ------
# Mirrors the file selection + defines in blyt_guest_libs.cmake's guest build.
sf_platform="$work/sf-platform"
mkdir -p "$sf_platform"
printf '#define THREAD_LOCAL\n#define LITTLEENDIAN 1\n' >"$sf_platform/platform.h"
# Cross libc includes so softfloat_builtins.c finds <stdint.h>/<string.h>
# (the pure SoftFloat kernels don't need them, but it's harmless).
SF_INC=(-nostdinc -isystem "$RESINC" -isystem "$MUSL_INSTALL/include"
  -I "$SF_SRC/include" -I "$SF_SRC/RISCV" -I "$sf_platform")
SF_DEF=(-DSOFTFLOAT_FAST_INT64=1 -DSOFTFLOAT_ROUND_ODD=1)

sf_files=()
for f in "$SF_SRC"/*.c; do
  b="$(basename "$f")"
  case "$b" in
  ._* | extF80* | bf16* | f16* | *M.c | *M_* | \
    f32_to_extF80* | f64_to_extF80* | f128_to_extF80*) continue ;;
  esac
  sf_files+=("$f")
done
# Multi-word helpers f128_mul needs even with FAST_INT64 (excluded by *M.c above).
sf_files+=("$SF_SRC/s_add256M.c" "$SF_SRC/s_sub256M.c"
  "$SF_SRC/s_mul128To256M.c" "$SF_SRC/s_shiftRightJam256M.c")
# RISC-V NaN specialisations (same set the runtime uses → bit-identical).
for r in s_propagateNaNF16UI s_propagateNaNF32UI s_propagateNaNF64UI \
  s_propagateNaNF128UI s_propagateNaNExtF80UI s_f16UIToCommonNaN \
  s_f32UIToCommonNaN s_f64UIToCommonNaN s_f128UIToCommonNaN \
  s_extF80UIToCommonNaN s_commonNaNToF16UI s_commonNaNToF32UI \
  s_commonNaNToF64UI s_commonNaNToF128UI s_commonNaNToExtF80UI \
  softfloat_raiseFlags; do
  sf_files+=("$SF_SRC/RISCV/$r.c")
done

log "building guest SoftFloat (${#sf_files[@]} files) + quad builtins"
sf_objs=()
n=0
for f in "${sf_files[@]}" "$SF_BUILTINS"; do
  o="$work/sf_$((n++)).o"
  "$CLANG" "${GUEST_TGT[@]}" -O2 "${SF_INC[@]}" "${SF_DEF[@]}" -w \
    -c "$f" -o "$o" 2>/dev/null
  sf_objs+=("$o")
done
"$LLVM_AR" rcs "$work/libsf.a" "${sf_objs[@]}"

INC=(-nostdinc -isystem "$RESINC" -isystem "$MUSL_INSTALL/include" -I "$LUA_SRC")
# BLYT_LUA_I32_F64: lua_Integer=int32, lua_Number=double (the cart identity).
# luai_makeseed()=0x424C5954u ("BLYT"): the runtime's fixed hash seed → the VM's
# string hashing (hence instruction count) is deterministic and matches carts.
DEF=(-DBLYT_LUA_I32_F64=1 "-Dluai_makeseed()=0x424C5954u")

log "building lua-bench.elf (blyt Lua VM, $GUEST_MARCH / $GUEST_MABI)"
# Lua core+libs as one TU (MAKE_LIB suppresses onelua's standalone main()).
"$CLANG" "${GUEST_TGT[@]}" -O2 "${INC[@]}" "${DEF[@]}" -DMAKE_LIB=1 \
  -c "$LUA_SRC/onelua.c" -o lua-core.o 2>/dev/null
"$CLANG" "${GUEST_TGT[@]}" -O2 "${INC[@]}" "${DEF[@]}" \
  -c "$PORT/lua_bench.c" -o lua_bench.o

# softfloat_builtins.c redundantly defines a few libc stubs (wctomb, fenv) for
# blyt's -nostdlib guest; here we link full musl, so let musl's real versions win
# (--allow-multiple-definition). musl still gets the __*tf* quad builtins it lacks
# from libsf. Order: libc first (its stubs win), then libsf for the builtins.
# lua_suite.c — operation-coverage benchmarks sharing the same Lua core.
"$CLANG" "${GUEST_TGT[@]}" -O2 "${INC[@]}" "${DEF[@]}" \
  -c "$PORT/lua_suite.c" -o lua_suite.o

# softfloat_builtins.c redundantly defines a few libc stubs (wctomb, fenv) for
# blyt's -nostdlib guest; here we link full musl, so let musl's real versions win
# (--allow-multiple-definition). musl still gets the __*tf* quad builtins it lacks
# from libsf. Order: libc first (its stubs win), then libsf for the builtins.
link_elf() { # <main.o> <out.elf>
  "$CLANG" "${GUEST_TGT[@]}" -O2 -nostdlib -static \
    -Wl,--allow-multiple-definition \
    "$MUSL_INSTALL/lib/crt1.o" "$MUSL_INSTALL/lib/crti.o" \
    "$1" lua-core.o \
    -L"$MUSL_INSTALL/lib" -lc "$work/libsf.a" "$MUSL_INSTALL/lib/crtn.o" \
    -o "$2"
}
link_elf lua_bench.o "$GUEST_OUT/lua-bench.elf"
link_elf lua_suite.o "$GUEST_OUT/lua-suite.elf"

log "built $GUEST_OUT/lua-bench.elf and lua-suite.elf"
file "$GUEST_OUT/lua-bench.elf" | sed 's/^/[spike-a]   /'
