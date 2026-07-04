/*
 * Lua-VM throughput + per-frame budget workload for the blyt Spike A harness
 * (a Spike B probe).
 *
 * Measures the effective guest MIPS of rv32emu running the *Lua bytecode
 * interpreter* — the load-bearing authoring path — and, via the runner's
 * per-frame marker, the per-frame instruction distribution (mean/p99/max) that
 * decides whether a Lua game loop fits the 16.67 ms budget on the Pi with
 * headroom. The Lua VM is the blyt build (BLYT_LUA_I32_F64: lua_Integer = int32,
 * lua_Number = double) at the real cart ISA (RV32IMAFDC/ilp32d) with the
 * runtime's fixed hash seed, so the instruction mix matches a real cart.
 *
 * C drives the frame loop and emits a marker (SYS_BLYT_FRAME ecall) at the start
 * of each frame; the runner records per-frame retired instructions. Two
 * workloads select GC behaviour:
 *   - "steady" (default): reused tables, ~no per-frame allocation.
 *   - "alloc": allocates temporary tables/strings each entity each frame, so GC
 *     runs and its pauses appear as high-percentile frames.
 *
 * Usage (guest argv): lua-bench.elf <frames> [steady|alloc]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

/* Per-frame marker: the harness-private SYS_BLYT_FRAME ecall (see runner.c). */
static void frame_mark(void)
{
    register long a7 asm("a7") = 0xB1700001;
    register long a0 asm("a0") = 0;
    asm volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
}

/* Steady-state entity update: no per-frame allocation (reuses the arrays). */
static const char *STEADY_LUA =
    "local N = 256\n"
    "ex, ey, evx, evy = {}, {}, {}, {}\n"
    "for i = 1, N do\n"
    "  ex[i] = i * 1.0; ey[i] = i * 0.5\n"
    "  evx[i] = (i % 7) * 0.13 - 0.4; evy[i] = -(i % 5) * 0.17\n"
    "end\n"
    "local sqrt, sin = math.sqrt, math.sin\n"
    "function update()\n"
    "  for i = 1, N do\n"
    "    evy[i] = evy[i] + 0.05\n"
    "    ex[i] = ex[i] + evx[i]; ey[i] = ey[i] + evy[i]\n"
    "    if ex[i] < 0 or ex[i] > 320 then evx[i] = -evx[i] end\n"
    "    if ey[i] > 240 then ey[i] = 240; evy[i] = -evy[i] * 0.8 end\n"
    "    local d = sqrt(evx[i] * evx[i] + evy[i] * evy[i])\n"
    "    ex[i] = ex[i] + sin(d) * 0.01\n"
    "  end\n"
    "end\n"
    "function checksum() local s = 0.0 for i = 1, N do s = s + ex[i] + ey[i] end return s end\n";

/* Allocation-heavy update: a temporary table + string per entity per frame, so
 * the Lua GC runs under real pressure and its pauses land in the p99/max frame. */
static const char *ALLOC_LUA =
    "local N = 256\n"
    "ex, ey = {}, {}\n"
    "for i = 1, N do ex[i] = i * 1.0; ey[i] = i * 0.5 end\n"
    "local sqrt = math.sqrt\n"
    "function update()\n"
    "  local acc = {}\n"
    "  for i = 1, N do\n"
    "    local e = { x = ex[i], y = ey[i], tag = 'ent' .. i }\n"
    "    e.x = e.x + 0.5; e.y = e.y + sqrt(e.x * 0.001 + 1.0)\n"
    "    acc[i] = e\n"
    "  end\n"
    "  for i = 1, N do ex[i] = acc[i].x; ey[i] = acc[i].y end\n"
    "end\n"
    "function checksum() local s = 0.0 for i = 1, N do s = s + ex[i] + ey[i] end return s end\n";

int main(int argc, char **argv)
{
    long frames = (argc > 1) ? atol(argv[1]) : 20000;
    const char *mode = (argc > 2) ? argv[2] : "steady";
    const char *script = (strcmp(mode, "alloc") == 0) ? ALLOC_LUA : STEADY_LUA;

    lua_State *L = luaL_newstate();
    if (!L) {
        fprintf(stderr, "lua-bench: luaL_newstate failed\n");
        return 1;
    }
    luaL_openlibs(L);

    if (luaL_dostring(L, script) != LUA_OK) {
        fprintf(stderr, "lua-bench: setup: %s\n", lua_tostring(L, -1));
        lua_close(L);
        return 1;
    }

    for (long f = 0; f < frames; f++) {
        frame_mark();
        lua_getglobal(L, "update");
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            fprintf(stderr, "lua-bench: update: %s\n", lua_tostring(L, -1));
            lua_close(L);
            return 1;
        }
    }

    lua_getglobal(L, "checksum");
    lua_call(L, 0, 1);
    double r = lua_tonumber(L, -1);
    unsigned long long bits;
    memcpy(&bits, &r, sizeof(bits));
    /* Integer digest only — no %f, to keep musl's long-double printf path out. */
    printf("lua-bench mode=%s frames=%ld digest=%016llx\n", mode, frames, bits);

    lua_close(L);
    return 0;
}
