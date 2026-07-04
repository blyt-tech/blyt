/*
 * Lua operation-coverage suite for the blyt Spike A harness (Spike B breadth).
 *
 * Standard lua.org/Computer-Language-Benchmarks-Game style micro-benchmarks,
 * each stressing a different Lua VM operation class, so the Lua-VM effective
 * MIPS is characterised across workload types (not just the entity `update()`
 * of lua_bench.c). Same blyt Lua VM (BLYT_LUA_I32_F64, fixed hash seed) at the
 * cart ISA; run single-shot through host/runner.c, effective MIPS from csr_cycle.
 *
 * Usage (guest argv): lua-suite.elf <name> [size]
 *   fib          recursive calls / integer          (call-heavy)
 *   binarytrees  allocate + walk + discard trees     (GC / allocation)
 *   nbody        pairwise gravity, f64 + sqrt        (softfloat-heavy)
 *   mandelbrot   escape-time, tight f64 loops        (float numeric)
 *   strings      concat / gmatch / gsub              (string ops)
 *   tables       insert / sort / iterate             (table ops)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

typedef struct {
    const char *name;
    long def_size;
    const char *code; /* reads global SIZE, sets global RESULT (a number) */
} bench_t;

static const bench_t BENCHES[] = {
    {"fib", 29,
     "local function fib(n) if n < 2 then return n end return fib(n-1)+fib(n-2) end\n"
     "RESULT = fib(SIZE)\n"},

    {"binarytrees", 11,
     "local function make(d) if d == 0 then return {nil, nil} end\n"
     "  local d2 = d - 1 return {make(d2), make(d2)} end\n"
     "local function check(t) if t[1] == nil then return 1 end\n"
     "  return 1 + check(t[1]) + check(t[2]) end\n"
     "local sum = 0\n"
     "for d = 4, SIZE, 2 do\n"
     "  local n = 1 << (SIZE - d + 4)\n"
     "  for i = 1, n do sum = sum + check(make(d)) end\n"
     "end\n"
     "RESULT = sum\n"},

    {"nbody", 30,
     "local B = 24\n"
     "local x, y, z, vx, vy, vz, m = {}, {}, {}, {}, {}, {}, {}\n"
     "for i = 1, B do\n"
     "  x[i] = (i % 7) - 3.0; y[i] = (i % 5) - 2.0; z[i] = (i % 3) - 1.0\n"
     "  vx[i] = 0.0; vy[i] = 0.0; vz[i] = 0.0; m[i] = 1.0 + (i % 4)\n"
     "end\n"
     "local sqrt = math.sqrt\n"
     "local dt = 0.01\n"
     "for step = 1, SIZE * 20 do\n"
     "  for i = 1, B do\n"
     "    for j = i + 1, B do\n"
     "      local dx = x[i]-x[j]; local dy = y[i]-y[j]; local dz = z[i]-z[j]\n"
     "      local d2 = dx*dx + dy*dy + dz*dz + 0.01\n"
     "      local mag = dt / (d2 * sqrt(d2))\n"
     "      vx[i] = vx[i] - dx*m[j]*mag; vy[i] = vy[i] - dy*m[j]*mag; vz[i] = vz[i] - dz*m[j]*mag\n"
     "      vx[j] = vx[j] + dx*m[i]*mag; vy[j] = vy[j] + dy*m[i]*mag; vz[j] = vz[j] + dz*m[i]*mag\n"
     "    end\n"
     "  end\n"
     "  for i = 1, B do x[i]=x[i]+dt*vx[i]; y[i]=y[i]+dt*vy[i]; z[i]=z[i]+dt*vz[i] end\n"
     "end\n"
     "local s = 0.0 for i = 1, B do s = s + x[i] + y[i] + z[i] end\n"
     "RESULT = s\n"},

    {"mandelbrot", 128,
     "local sum = 0\n"
     "local size = SIZE\n"
     "for py = 0, size - 1 do\n"
     "  for px = 0, size - 1 do\n"
     "    local cr = px / size * 2.5 - 2.0\n"
     "    local ci = py / size * 2.5 - 1.25\n"
     "    local zr, zi, i = 0.0, 0.0, 0\n"
     "    while i < 100 and zr*zr + zi*zi < 4.0 do\n"
     "      local t = zr*zr - zi*zi + cr; zi = 2.0*zr*zi + ci; zr = t; i = i + 1\n"
     "    end\n"
     "    sum = sum + i\n"
     "  end\n"
     "end\n"
     "RESULT = sum\n"},

    {"strings", 40000,
     "local t = {}\n"
     "for i = 1, SIZE do t[i] = tostring(i * 2654435761 % 1000000) end\n"
     "local s = table.concat(t, ',')\n"
     "local c = 0\n"
     "for w in s:gmatch('%d+') do c = c + #w end\n"
     "local r = s:gsub('0', 'x')\n"
     "RESULT = c + #r\n"},

    {"tables", 150000,
     "local t = {}\n"
     "for i = 1, SIZE do t[i] = (i * 1103515245 + 12345) % 2147483648 end\n"
     "table.sort(t)\n"
     "local s = 0\n"
     "for i = 1, SIZE do s = s + (t[i] % 997) end\n"
     "RESULT = s\n"},
};

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <name> [size]\nnames:", argv[0]);
        for (size_t i = 0; i < sizeof(BENCHES) / sizeof(BENCHES[0]); i++)
            fprintf(stderr, " %s", BENCHES[i].name);
        fprintf(stderr, "\n");
        return 2;
    }
    const bench_t *b = NULL;
    for (size_t i = 0; i < sizeof(BENCHES) / sizeof(BENCHES[0]); i++)
        if (strcmp(argv[1], BENCHES[i].name) == 0)
            b = &BENCHES[i];
    if (!b) {
        fprintf(stderr, "lua-suite: unknown benchmark '%s'\n", argv[1]);
        return 2;
    }
    long size = (argc > 2) ? atol(argv[2]) : b->def_size;

    lua_State *L = luaL_newstate();
    if (!L) {
        fprintf(stderr, "lua-suite: luaL_newstate failed\n");
        return 1;
    }
    luaL_openlibs(L);
    lua_pushinteger(L, (lua_Integer)size);
    lua_setglobal(L, "SIZE");

    if (luaL_dostring(L, b->code) != LUA_OK) {
        fprintf(stderr, "lua-suite: %s: %s\n", b->name, lua_tostring(L, -1));
        lua_close(L);
        return 1;
    }

    lua_getglobal(L, "RESULT");
    double r = lua_tonumber(L, -1);
    unsigned long long bits;
    memcpy(&bits, &r, sizeof(bits));
    printf("lua-suite %s size=%ld digest=%016llx\n", b->name, size, bits);

    lua_close(L);
    return 0;
}
