#!/usr/bin/env bash
# Cross-compile the Embench-IoT benchmark suite to the blyt cart ISA
# (rv32imafdc / ilp32d) against static musl, one bare ELF per benchmark at
#   bench/spike-a/artifacts/guest/embench-<name>.elf
# Structure mirrors build-guest.sh (CoreMark): same toolchain, same musl link
# recipe, same include layout.
#
# Each Embench benchmark iterates its work LOCAL_SCALE_FACTOR * GLOBAL_SCALE_FACTOR
# times. LOCAL_SCALE_FACTOR is baked per benchmark (calibrated upstream so one
# gsf unit is roughly comparable across benchmarks); GLOBAL_SCALE_FACTOR is the
# knob we set to land each run in the ~1e8-1e9 guest-instruction band (a few
# seconds under interpretation on this Mac). Override with:
#   EMBENCH_GSF=<n> ./build-embench.sh
#
# Robustness: a benchmark that fails to compile or link is skipped with a logged
# reason; the script prints a build summary and always exits 0 if at least the
# fetch/toolchain succeeded, so one bad benchmark does not sink the rest.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
require_guest_toolchain
guest_flags

# gsf=100 lands every benchmark in the ~1e8-7e8 guest-instruction band
# (sub-second to ~1.5s each under interpretation on this Mac) — long enough for
# a stable effective-MIPS measure, short enough to keep the matrix quick.
EMBENCH_GSF="${EMBENCH_GSF:-100}"
WARMUP_HEAT="${EMBENCH_WARMUP_HEAT:-1}"
MUSL_INSTALL="$DEPS_DIR/musl-install"
EMB_SRC="$DEPS_DIR/embench"
PORT="$SPIKE_A_ROOT/guest/embench-port"

# Ensure static musl exists.
if [ ! -f "$MUSL_INSTALL/lib/libc.a" ]; then
  "$(dirname "${BASH_SOURCE[0]}")/build-musl.sh"
fi

# Fetch pinned Embench-IoT.
if [ ! -d "$EMB_SRC" ]; then
  log "fetching Embench-IoT @ ${EMBENCH_COMMIT:0:12}"
  git clone -q "$EMBENCH_REPO" "$EMB_SRC"
  git -C "$EMB_SRC" checkout -q "$EMBENCH_COMMIT"
fi

SUPPORT="$EMB_SRC/support"

INC=(-nostdinc -isystem "$RESINC" -isystem "$MUSL_INSTALL/include"
  -I "$SUPPORT" -include stddef.h)
DEF=(-DWARMUP_HEAT="$WARMUP_HEAT" -DGLOBAL_SCALE_FACTOR="$EMBENCH_GSF")

work="$DEPS_DIR/embench-build"
rm -rf "$work"
mkdir -p "$work"
cd "$work"

# Shared support + board objects (compiled once, reused by every benchmark).
# We use our own blyt_board.c instead of Embench's board.c/chip.c, which would
# #include a per-config boardsupport.c/chipsupport.c that this port omits.
compile() { # <src.c> <out.o> [extra include dirs...]
  local src="$1" out="$2"
  shift 2
  local extra=()
  local d
  for d in "$@"; do extra+=(-I "$d"); done
  "$CLANG" "${GUEST_TGT[@]}" -O2 "${INC[@]}" "${extra[@]}" "${DEF[@]}" \
    -c "$src" -o "$out" 2>"$out.log"
}

log "building shared support objects (gsf=$EMBENCH_GSF, warmup=$WARMUP_HEAT)"
shared_ok=1
for pair in "main:$SUPPORT/main.c" "beebsc:$SUPPORT/beebsc.c" "blyt_board:$PORT/blyt_board.c"; do
  name="${pair%%:*}"
  src="${pair#*:}"
  if ! compile "$src" "$work/$name.o"; then
    log "FATAL: shared object $name failed to compile:"
    sed 's/^/[spike-a]     /' "$work/$name.o.log" >&2
    shared_ok=0
  fi
done
[ "$shared_ok" = 1 ] || {
  echo "error: shared Embench support failed to build; cannot continue" >&2
  exit 1
}
SHARED_OBJS=("$work/main.o" "$work/beebsc.o" "$work/blyt_board.o")

built=()
skipped=()

build_one() { # <benchmark-name>
  local bench="$1"
  local bdir="$EMB_SRC/src/$bench"
  local bwork="$work/$bench"
  mkdir -p "$bwork"

  local objs=("${SHARED_OBJS[@]}")
  local csrc rc=0
  shopt -s nullglob
  local srcs=("$bdir"/*.c)
  shopt -u nullglob
  if [ ${#srcs[@]} -eq 0 ]; then
    skipped+=("$bench: no .c sources found")
    return
  fi
  for csrc in "${srcs[@]}"; do
    local obj="$bwork/$(basename "${csrc%.c}").o"
    if ! compile "$csrc" "$obj" "$bdir"; then
      skipped+=("$bench: compile failed ($(basename "$csrc")) — see $obj.log")
      return
    fi
    objs+=("$obj")
  done

  local out="$GUEST_OUT/embench-$bench.elf"
  if ! "$CLANG" "${GUEST_TGT[@]}" -O2 -nostdlib -static \
    "$MUSL_INSTALL/lib/crt1.o" "$MUSL_INSTALL/lib/crti.o" "${objs[@]}" \
    -L"$MUSL_INSTALL/lib" -lc "$MUSL_INSTALL/lib/crtn.o" \
    -o "$out" 2>"$bwork/link.log"; then
    # Surface the most likely cause (undefined long-double soft-float builtins
    # from float printf) prominently, then skip.
    local reason="link failed"
    if grep -qE "__[a-z]*tf[0-9]?|__floatditf|__extenddftf2" "$bwork/link.log"; then
      reason="link failed (long-double soft-float builtins — float printf?)"
    fi
    skipped+=("$bench: $reason — see $bwork/link.log")
    rm -f "$out"
    return
  fi
  built+=("$bench")
}

for bdir in "$EMB_SRC"/src/*/; do
  build_one "$(basename "$bdir")"
done

echo
log "==== Embench build summary ===="
log "built ${#built[@]}:"
for b in "${built[@]}"; do log "  OK    $b -> embench-$b.elf"; done
if [ ${#skipped[@]} -gt 0 ]; then
  log "skipped ${#skipped[@]}:"
  for s in "${skipped[@]}"; do log "  SKIP  $s"; done
fi
log "ELFs in $GUEST_OUT"
