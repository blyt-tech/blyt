#!/usr/bin/env bash
# Cross-compile the benchmark guest ELFs (CoreMark; Embench added by
# build-embench.sh) to the blyt cart ISA (rv32imafdc / ilp32d) against static
# musl. The resulting ELFs are host-independent and shipped to both the Mac and
# the Pi. Run build-musl.sh first (this script calls it if needed).
#
# CoreMark iteration count is compile-time (SEED_VOLATILE); override with
#   COREMARK_ITERATIONS=<n> ./build-guest.sh
# The runner's effective-MIPS figure is independent of this; a larger count
# only lengthens the run (needed for CoreMark's own >=10s "valid result" gate).

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
require_guest_toolchain
guest_flags

COREMARK_ITERATIONS="${COREMARK_ITERATIONS:-2000}"
MUSL_INSTALL="$DEPS_DIR/musl-install"
CM_SRC="$DEPS_DIR/coremark"
PORT="$SPIKE_A_ROOT/guest/coremark-port"

# Ensure static musl exists.
if [ ! -f "$MUSL_INSTALL/lib/libc.a" ]; then
  "$(dirname "${BASH_SOURCE[0]}")/build-musl.sh"
fi

# Fetch pinned CoreMark.
if [ ! -d "$CM_SRC" ]; then
  log "fetching CoreMark @ ${COREMARK_COMMIT:0:12}"
  git clone -q "$COREMARK_REPO" "$CM_SRC"
  git -C "$CM_SRC" checkout -q "$COREMARK_COMMIT"
fi

INC=(-nostdinc -isystem "$RESINC" -isystem "$MUSL_INSTALL/include"
  -I "$CM_SRC" -I "$CM_SRC/barebones" -include stddef.h)
DEF=(-DITERATIONS="$COREMARK_ITERATIONS" -DPERFORMANCE_RUN=1 -DMAIN_HAS_NOARGC=1
  -DHAS_PRINTF=0 "-DFLAGS_STR=\"O2 $GUEST_MARCH $GUEST_MABI musl barebones\"")

work="$DEPS_DIR/coremark-build"
rm -rf "$work"
mkdir -p "$work"
cd "$work"

# CoreMark's barebones ee_printf.c ships a "#error implement me" uart_send_char;
# the standard port step is to implement it. Route it to write(2). CoreMark's
# upstream tree stays pristine (we patch a build copy).
cp "$CM_SRC/barebones/ee_printf.c" ee_printf.c
perl -0pi -e 's/uart_send_char\(char c\)\s*\{\s*#error\s*"[^"]*";?/uart_send_char(char c){extern long write(int,const void*,unsigned long);write(1,\&c,1);/s' ee_printf.c
grep -q "write(1,&c,1)" ee_printf.c || {
  echo "error: ee_printf uart_send_char patch failed" >&2
  exit 1
}

log "building coremark.elf (ITERATIONS=$COREMARK_ITERATIONS)"
objs=()
compile() {
  "$CLANG" "${GUEST_TGT[@]}" -O2 "${INC[@]}" "${DEF[@]}" -c "$1" -o "$2" 2>/dev/null
  objs+=("$2")
}
for f in core_list_join core_main core_matrix core_state core_util; do
  compile "$CM_SRC/$f.c" "$f.o"
done
compile "$CM_SRC/barebones/cvt.c" cvt.o
compile "$work/ee_printf.c" ee_printf.o
compile "$PORT/core_portme.c" core_portme.o

"$CLANG" "${GUEST_TGT[@]}" -O2 -nostdlib -static \
  "$MUSL_INSTALL/lib/crt1.o" "$MUSL_INSTALL/lib/crti.o" "${objs[@]}" \
  -L"$MUSL_INSTALL/lib" -lc "$MUSL_INSTALL/lib/crtn.o" \
  -o "$GUEST_OUT/coremark.elf"

log "built $GUEST_OUT/coremark.elf"
file "$GUEST_OUT/coremark.elf" | sed 's/^/[spike-a]   /'
