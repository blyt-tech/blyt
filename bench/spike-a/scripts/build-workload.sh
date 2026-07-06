#!/usr/bin/env bash
# Build a Lua game-shaped workload two ways for the host-Lua-vs-emulated
# comparison on the floor hardware (Pi Zero 2 W):
#   • RV32 guest ELF  (run under the rv32emu runner  → the EMULATED leg)
#   • native aarch64 static binary (→ the HOST-LUA leg), built in a
#     debian:trixie linux/arm64 container so it drops onto the Pi (Trixie/arm64).
# Both from the SAME blyt Lua fork (BLYT_LUA_I32_F64, fixed hash seed) and the
# same embedded <name>_bench.lua, so the only difference is native-vs-emulated —
# the VM-throughput comparison. Reuses build-lua.sh's static musl + SoftFloat +
# Lua core. Prints an integer checksum both legs must match (determinism check).
#
#   build-workload.sh doom     # Spike B P_Ticker game logic
#   build-workload.sh draw     # Doom R_DrawColumn (Lua per-pixel texture map)
#
# Outputs: artifacts/guest/lua-<name>.elf  and  artifacts/pi/lua-<name>-native
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
NAME="${1:?usage: build-workload.sh <doom|draw>}"
PORT="$SPIKE_A_ROOT/guest/lua-port"
REPO_ROOT="$(cd "$SPIKE_A_ROOT/../.." && pwd)"
LUA_SRC="$REPO_ROOT/build/_deps/lua-src"
W="$DEPS_DIR/lua-build"
MI="$DEPS_DIR/musl-install"

# The shared Lua core (.o) + guest SoftFloat archive come from build-lua.sh.
if [ ! -f "$W/lua-core.o" ] || [ ! -f "$W/libsf.a" ]; then
  log "shared Lua core/softfloat missing — running build-lua.sh first"
  "$(dirname "${BASH_SOURCE[0]}")/build-lua.sh"
fi
require_guest_toolchain
guest_flags

# Generate the embedded-Lua header (the C driver #includes <name>_lua.h).
python3 - "$PORT/${NAME}_bench.lua" "$PORT/${NAME}_lua.h" <<PY
src=open("$PORT/${NAME}_bench.lua").read()
esc=src.replace(chr(92),chr(92)*2).replace('"','\\\\"').replace(chr(10),'\\\\n"\n"')
open("$PORT/${NAME}_lua.h","w").write('/* generated from ${NAME}_bench.lua — do not edit */\n'
  'static const char DOOM_LUA[] =\n"'+esc+'";\n')
PY

RESINC="$("$CLANG" -print-resource-dir)/include"
INC=(-nostdinc -isystem "$RESINC" -isystem "$MI/include" -I "$LUA_SRC" -I "$PORT")
DEF=(-DBLYT_LUA_I32_F64=1 "-Dluai_makeseed()=0x424C5954u")

log "building lua-${NAME}.elf (RV32 guest → emulated leg)"
"$CLANG" "${GUEST_TGT[@]}" -O2 "${INC[@]}" "${DEF[@]}" -c "$PORT/lua_${NAME}.c" -o "$W/lua_${NAME}.o"
"$CLANG" "${GUEST_TGT[@]}" -O2 -nostdlib -static -Wl,--allow-multiple-definition \
  "$MI/lib/crt1.o" "$MI/lib/crti.o" "$W/lua_${NAME}.o" "$W/lua-core.o" \
  -L"$MI/lib" -lc "$W/libsf.a" "$MI/lib/crtn.o" -o "$GUEST_OUT/lua-${NAME}.elf"

log "building lua-${NAME}-native (aarch64 static → host-Lua leg) in debian:trixie linux/arm64"
mkdir -p "$ART_DIR/pi"
docker run --rm --platform linux/arm64 -v "$REPO_ROOT":/work -w /work debian:trixie-slim bash -eu -c "
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq >/dev/null
  apt-get install -y -qq --no-install-recommends gcc libc6-dev >/dev/null
  I='-Ibuild/_deps/lua-src -Ibench/spike-a/guest/lua-port'
  D='-DBLYT_LUA_I32_F64=1 -Dluai_makeseed()=0x424C5954u'
  gcc -O2 \$D -DMAKE_LIB=1 \$I -c build/_deps/lua-src/onelua.c -o /tmp/lua-core.o
  gcc -O2 \$D \$I -c bench/spike-a/guest/lua-port/lua_${NAME}.c -o /tmp/lua_${NAME}.o
  gcc -O2 -static /tmp/lua_${NAME}.o /tmp/lua-core.o -lm -o bench/spike-a/artifacts/pi/lua-${NAME}-native
"
log "built $GUEST_OUT/lua-${NAME}.elf and $ART_DIR/pi/lua-${NAME}-native"
