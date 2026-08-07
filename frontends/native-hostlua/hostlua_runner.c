/*
 * frontends/native-hostlua/hostlua_runner.c — Spike Z native host-Lua leg.
 *
 * A minimal, faithful native host-Lua execution leg: it runs a cart's Lua the
 * SAME way the WASM host-Lua fast path does (frontends/wasm/wasm_main.c), but
 * compiled for the native host (x86-64 / arm64) instead of wasm32. The only
 * thing that differs from the WASM leg is the compile target — same Lua fork
 * (blyt-tech/lua, BLYT_LUA_I32_F64), same cart bytecode, same fixed hash seed,
 * same blyt_fpm transcendental seam wired to the in-house blyt-tech musl kernels
 * (BLYT_HOSTLUA_FP_SEAM), same -ffp-contract=off discipline.
 *
 * This is NOT a shipped player. It exists to produce the go/no-go determinism
 * evidence for the host-Lua-everywhere direction (issue #225, Spike Z, ADR-0135):
 * on FMA silicon, does the native host-Lua VM reproduce the Berkeley-SoftFloat
 * reference bit-for-bit? It reads a .blyt, extracts the `.cart.lua` bytecode
 * section, runs init()/update() with just the blyt.debug.print + blyt.quit shim
 * the parity cart needs, and prints the cart's [blyt:...] lines to stdout — which
 * the integration harness (tests/integration/tests/fp_parity.rs) hashes and
 * asserts identical across every leg.
 *
 * Deliberately omits everything the FP parity path does not exercise: no
 * rv32emu session, no state buffers, no gfx/surface fast path, no resource heap,
 * no DAP/GDB, no hybrid bridge. Those belong to Q5 (non-FP parity) and to the
 * real native player, if the decision is go.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include "blyt_elf_section.h" /* runtime/shared: freestanding .cart.lua finder */

/* blyt.quit() latch — the parity cart's update() sets it after one frame. */
static int g_quit = 0;

/* ── Q1 contraction torture (issue #225, ADR-0135 invariant 1) ───────────────
 *
 * The host-Lua parity corpus (transcendental + Zone-1) turns out to be
 * contraction-INVARIANT: flipping -ffp-contract does not move its digest,
 * because (a) the in-house musl kernels are written contraction-safe and (b) a
 * Lua cart's `a*b+c` compiles to two separate VM ops (OP_MUL then OP_ADD), each
 * a correctly-rounded C add/mul that the compiler cannot fuse. So the real path
 * has no FMA exposure — a good determinism result, but it means a flag-flip
 * negative control has no teeth *there*.
 *
 * This is where the teeth live: a deliberately contraction-prone Horner /
 * dot-product chain evaluated in C. Under -ffp-contract=off every `a*b+c` rounds
 * twice (== the SoftFloat reference by IEEE); under -ffp-contract=fast on FMA
 * silicon the compiler fuses it to a single-rounding FMA and the digest moves.
 * The runner is compiled with the variant's contraction flag, so the OFF build
 * and the FMA build emit DIFFERENT [blyt:fptorture] digests on FMA hardware —
 * proving the flag is load-bearing for C-level multiply-add and that the harness
 * detects an FMA divergence when one exists.
 *
 * Inputs are `volatile` so they are not constant-folded: the flag's effect only
 * appears on genuine runtime multiply-adds. */
static uint32_t torture_digest(void) {
    /* Polynomial coefficients + input seeds chosen to be ordinary finite values
     * whose Horner evaluation exercises the low mantissa bits an FMA would round
     * differently. */
    static const double coeffs[12] = {
        1.0,
        -0.5,
        0.16666666666666666,
        -0.041666666666666664,
        0.008333333,
        -0.001388888,
        0.0001984126984,
        -2.48015873e-06,
        2.755731922e-08,
        -2.505210839e-10,
        1.605904384e-12,
        -7.647163732e-15,
    };
    static const double seeds[8] = {
        1.0000001, 355.0 / 113.0,     1.4142135623730951, 3.141592653589793,
        0.1,       2.718281828459045, 1.0000000001,       9.9,
    };

    uint32_t h = 0x811c9dc5u;
    for (int s = 0; s < 8; s++) {
        volatile double xv = seeds[s];
        double x = xv;
        /* Horner polynomial: acc = acc*x + c — the canonical fusable form. */
        double acc = 0.0;
        for (int i = 0; i < 12; i++) {
            volatile double cv = coeffs[i];
            acc = acc * x + cv;
        }
        /* Dot product: another multiply-add reduction. */
        double dot = 0.0;
        for (int i = 0; i < 12; i++) {
            volatile double a = coeffs[i];
            volatile double b = seeds[i % 8];
            dot = dot + a * b;
        }
        double results[2] = {acc, dot};
        for (int r = 0; r < 2; r++) {
            uint64_t bits;
            memcpy(&bits, &results[r], sizeof bits);
            for (int b = 0; b < 8; b++) {
                h = (h ^ (uint32_t)(bits & 0xff)) * 0x01000193u;
                bits >>= 8;
            }
        }
    }
    return h;
}

/* blyt.debug.print(s): the cart's cross-leg output channel. Mirrors the WASM
 * leg's lua_wasm_debug_print (which routes to blyt_js_log → stdout). One line
 * per call, no extra formatting, so the [blyt:fphash]/[blyt:fpspot] markers the
 * harness greps for arrive verbatim. */
static int l_debug_print(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    fputs(s, stdout);
    fputc('\n', stdout);
    return 0;
}

static int l_quit(lua_State *L) {
    (void)L;
    g_quit = 1;
    return 0;
}

static int l_should_quit(lua_State *L) {
    lua_pushboolean(L, g_quit);
    return 1;
}

/* Sandboxed require(): the parity cart pulls in no modules, but the host-Lua
 * fast path replaces the default searcher with a hard error so a cart cannot
 * reach the host filesystem. Mirror that so behaviour matches the WASM leg. */
static int l_require(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
    lua_getfield(L, -1, name);
    if (!lua_isnil(L, -1))
        return 1;
    return luaL_error(L, "module '%s' not found (blyt sandbox: only native exports available)",
                      name);
}

/* Load the cart's .cart.lua: a single raw bytecode chunk, or the BLMC
 * multi-chunk container (issue #54). Byte-for-byte the WASM leg's
 * wasm_load_lua_bytecode, minus the require()-able module registration the
 * parity cart does not use. Returns 0 on success; on failure leaves an error
 * string on the stack top and returns nonzero. */
static int load_lua_bytecode(lua_State *L, const unsigned char *data, size_t size) {
    if (size >= 8 && data[0] == 'B' && data[1] == 'L' && data[2] == 'M' && data[3] == 'C') {
        unsigned int nchunks = (unsigned int)data[4] | ((unsigned int)data[5] << 8) |
                               ((unsigned int)data[6] << 16) | ((unsigned int)data[7] << 24);
        data += 8;
        size -= 8;
        for (unsigned int ci = 0; ci < nchunks; ci++) {
            if (size < 4) {
                lua_pushstring(L, "BLMC truncated");
                return 1;
            }
            unsigned int csz = (unsigned int)data[0] | ((unsigned int)data[1] << 8) |
                               ((unsigned int)data[2] << 16) | ((unsigned int)data[3] << 24);
            data += 4;
            size -= 4;
            if (csz > size) {
                lua_pushstring(L, "BLMC chunk size overflow");
                return 1;
            }
            if (luaL_loadbuffer(L, (const char *)data, csz, "@chunk") != LUA_OK)
                return 1;
            if (lua_pcall(L, 0, 0, 0) != LUA_OK)
                return 1;
            data += csz;
            size -= csz;
        }
        return 0;
    }
    if (luaL_loadbuffer(L, (const char *)data, size, "@cart") != LUA_OK)
        return 1;
    if (lua_pcall(L, 0, 0, 0) != LUA_OK)
        return 1;
    return 0;
}

/* Register the blyt/blyt32 API surface the parity cart reaches: blyt.debug.print,
 * blyt.quit, blyt.should_quit, blyt_quit, blyt32.debug.print. Mirrors the WASM
 * leg's registration in run_lua_cart. */
static void register_blyt_api(lua_State *L) {
    /* blyt32.debug.print */
    lua_newtable(L);
    lua_newtable(L);
    lua_pushcfunction(L, l_debug_print);
    lua_setfield(L, -2, "print");
    lua_setfield(L, -2, "debug");
    lua_setglobal(L, "blyt32");

    /* blyt.debug.print + blyt.quit + blyt.should_quit */
    lua_newtable(L);
    lua_newtable(L);
    lua_pushcfunction(L, l_debug_print);
    lua_setfield(L, -2, "print");
    lua_setfield(L, -2, "debug");
    lua_pushcfunction(L, l_quit);
    lua_setfield(L, -2, "quit");
    lua_pushcfunction(L, l_should_quit);
    lua_setfield(L, -2, "should_quit");
    lua_setglobal(L, "blyt");

    lua_pushcfunction(L, l_quit);
    lua_setglobal(L, "blyt_quit");

    lua_pushcfunction(L, l_require);
    lua_setglobal(L, "require");
}

/* Open the sandboxed standard-library subset the host-Lua fast path exposes
 * (base/math/string/table/coroutine/utf8) — the SAME set, opened the SAME way
 * (luaL_requiref, not luaL_openlibs, so io/os stay out). */
static void open_libs(lua_State *L) {
    luaL_requiref(L, "_G", luaopen_base, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "math", luaopen_math, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "string", luaopen_string, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "table", luaopen_table, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "coroutine", luaopen_coroutine, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    lua_pop(L, 1);
}

/* Call a global lifecycle function `name` if it is defined; abort on error. */
static int call_lifecycle(lua_State *L, const char *name) {
    lua_getglobal(L, name);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return 0;
    }
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        fprintf(stderr, "blyt-hostlua: error in %s(): %s\n", name, lua_tostring(L, -1));
        return -1;
    }
    return 0;
}

/* Read an entire file into a malloc'd buffer. */
static unsigned char *read_file(const char *path, size_t *size_out) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    unsigned char *buf = malloc((size_t)n);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size_out = (size_t)n;
    return buf;
}

int main(int argc, char **argv) {
    /* Usage: blyt_hostlua_native [--frames N] <cart.blyt>
     * The parity cart terminates itself via blyt.quit(); --frames caps the loop
     * for carts that do not. Default cap is small — the FP work is in init(). */
    int max_frames = 2;
    const char *cart_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            max_frames = atoi(argv[++i]);
        } else {
            cart_path = argv[i];
        }
    }
    if (!cart_path) {
        fprintf(stderr, "usage: %s [--frames N] <cart.blyt>\n", argv[0]);
        return 2;
    }

    size_t file_size = 0;
    unsigned char *file = read_file(cart_path, &file_size);
    if (!file) {
        fprintf(stderr, "blyt-hostlua: cannot read cart %s\n", cart_path);
        return 2;
    }

    uint32_t off = 0, sz = 0;
    if (!blyt_elf32_find_section(file, file_size, ".cart.lua", &off, &sz)) {
        fprintf(stderr, "blyt-hostlua: cart %s has no .cart.lua section (not a pure-Lua cart?)\n",
                cart_path);
        free(file);
        return 2;
    }
    const unsigned char *bytecode = file + off;

    /* luaL_newstate uses the fork's fixed hash seed (determinism, ADR-0007) —
     * the SAME VM construction the WASM leg uses via lua_newstate. */
    lua_State *L = luaL_newstate();
    if (!L) {
        fprintf(stderr, "blyt-hostlua: failed to create Lua state\n");
        free(file);
        return 1;
    }

    open_libs(L);
    register_blyt_api(L);

    if (load_lua_bytecode(L, bytecode, sz) != 0) {
        fprintf(stderr, "blyt-hostlua: failed to load cart bytecode: %s\n", lua_tostring(L, -1));
        lua_close(L);
        free(file);
        return 1;
    }

    int rc = 0;
    if (call_lifecycle(L, "init") != 0) {
        rc = 1;
    } else {
        for (int frame = 0; frame < max_frames && !g_quit; frame++) {
            if (call_lifecycle(L, "update") != 0) {
                rc = 1;
                break;
            }
            if (call_lifecycle(L, "draw") != 0) {
                rc = 1;
                break;
            }
        }
    }

    /* Q1 contraction teeth (#225): emitted regardless of cart, so the OFF vs
     * FMA variant runners can be diffed on FMA hardware. */
    printf("[blyt:fptorture] %08x\n", torture_digest());

    fflush(stdout);
    lua_close(L);
    free(file);
    return rc;
}
