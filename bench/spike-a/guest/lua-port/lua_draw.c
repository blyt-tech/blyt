/*
 * lua_doom.c — Spike B doom_tick workload driver for the host-Lua vs emulated
 * comparison (Pi Zero 2 W floor). The SAME source builds two ways:
 *   • native aarch64 (host-Lua leg): the blyt Lua fork compiled for the host.
 *   • RV32 (ilp32d, static musl) run under rv32emu (emulated leg).
 * Both use the same blyt Lua fork (BLYT_LUA_I32_F64, fixed hash seed) and the
 * same embedded doom_bench.lua, so the only difference is native-vs-emulated —
 * the VM-throughput comparison. Prints an integer checksum (no %f) so both legs
 * are byte-identical and it doubles as a determinism cross-check.
 *
 * Usage: lua_doom <repeats>   (default 1000)
 */
#include <stdio.h>
#include <stdlib.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include "draw_lua.h"

int main(int argc, char **argv) {
    long repeats = (argc > 1) ? atol(argv[1]) : 1000;

    lua_State *L = luaL_newstate();
    if (!L) {
        fprintf(stderr, "lua_draw: luaL_newstate failed\n");
        return 1;
    }
    /* Restricted stdlib subset the host-Lua fast path exposes (no io/os), opened
     * the same way — base/math/string/table are all doom_bench.lua needs. */
    luaL_requiref(L, "_G", luaopen_base, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "math", luaopen_math, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "string", luaopen_string, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "table", luaopen_table, 1);
    lua_pop(L, 1);

    if (luaL_dostring(L, DOOM_LUA) != LUA_OK) {
        fprintf(stderr, "lua_draw: setup: %s\n", lua_tostring(L, -1));
        lua_close(L);
        return 1;
    }

    lua_getglobal(L, "bench");
    lua_pushinteger(L, (lua_Integer)repeats);
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        fprintf(stderr, "lua_draw: bench: %s\n", lua_tostring(L, -1));
        lua_close(L);
        return 1;
    }
    long checksum = (long)lua_tointeger(L, -1);
    printf("[doom] repeats=%ld checksum=%ld\n", repeats, checksum);

    lua_close(L);
    return 0;
}
