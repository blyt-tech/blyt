/*
 * Lua-VM throughput workload for the blyt Spike A harness (a Spike B probe).
 *
 * Measures the effective guest MIPS of rv32emu running the *Lua bytecode
 * interpreter* — the load-bearing authoring path — rather than native C
 * (CoreMark/Embench). The Lua VM is compiled to the real cart ISA
 * (RV32IMAFDC/ilp32d) with the cart numeric model (BLYT_LUA_I32_F64: lua_Integer
 * = int32, lua_Number = double) and the same fixed hash seed the runtime uses,
 * so the instruction mix matches what a Lua cart actually executes under the
 * emulator. Run through host/runner.c; effective MIPS comes from rv->csr_cycle.
 *
 * The workload is a steady-state entity `update()` — the pattern a non-trivial
 * retro game loop uses: table-indexed position/velocity integration, boundary
 * checks, and f64 transcendentals (sqrt/sin). It exercises exactly the parts of
 * the Lua VM that stress the interpreter: bytecode dispatch (indirect branches),
 * tagged-value loads/stores, and softfloat-backed f64 math.
 *
 * Frame count is argv[1] (default 20000), so run length is tunable without a
 * rebuild — like CoreMark's iteration count.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

/* Steady-state entity update. `FRAMES` is injected as a Lua global from C. */
static const char *BENCH_LUA = "local N = 256\n"
                               "local ex, ey, evx, evy = {}, {}, {}, {}\n"
                               "for i = 1, N do\n"
                               "  ex[i] = i * 1.0; ey[i] = i * 0.5\n"
                               "  evx[i] = (i % 7) * 0.13 - 0.4; evy[i] = -(i % 5) * 0.17\n"
                               "end\n"
                               "local sqrt, sin = math.sqrt, math.sin\n"
                               "local function update()\n"
                               "  for i = 1, N do\n"
                               "    evy[i] = evy[i] + 0.05        -- gravity\n"
                               "    ex[i]  = ex[i] + evx[i]\n"
                               "    ey[i]  = ey[i] + evy[i]\n"
                               "    if ex[i] < 0 or ex[i] > 320 then evx[i] = -evx[i] end\n"
                               "    if ey[i] > 240 then ey[i] = 240; evy[i] = -evy[i] * 0.8 end\n"
                               "    local d = sqrt(evx[i] * evx[i] + evy[i] * evy[i])\n"
                               "    ex[i] = ex[i] + sin(d) * 0.01\n"
                               "  end\n"
                               "end\n"
                               "for f = 1, FRAMES do update() end\n"
                               "local s = 0.0\n"
                               "for i = 1, N do s = s + ex[i] + ey[i] end\n"
                               "RESULT = s\n";

int main(int argc, char **argv) {
    long frames = (argc > 1) ? atol(argv[1]) : 20000;

    lua_State *L = luaL_newstate();
    if (!L) {
        fprintf(stderr, "lua-bench: luaL_newstate failed\n");
        return 1;
    }
    luaL_openlibs(L);

    lua_pushinteger(L, (lua_Integer)frames);
    lua_setglobal(L, "FRAMES");

    if (luaL_dostring(L, BENCH_LUA) != LUA_OK) {
        fprintf(stderr, "lua-bench: %s\n", lua_tostring(L, -1));
        lua_close(L);
        return 1;
    }

    lua_getglobal(L, "RESULT");
    double r = lua_tonumber(L, -1);
    unsigned long long bits;
    memcpy(&bits, &r, sizeof(bits));
    /* Integer digest only — avoid %f so musl's long-double printf path (and its
     * quad soft-float builtins) is never linked. */
    printf("lua-bench frames=%ld digest=%016llx\n", frames, bits);

    lua_close(L);
    return 0;
}
