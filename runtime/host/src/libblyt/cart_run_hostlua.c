/*
 * cart_run_hostlua.c — native host-Lua fast path runner (#238, epic #230).
 *
 * Sibling to cart_run.c's blyt_session_*: runs a pure-Lua cart's bytecode in a
 * Lua VM compiled natively for the host (the deterministic seam VM,
 * cmake/blyt_hostlua_vm.cmake) instead of the RV32 Lua VM under rv32emu.  The
 * native port of frontends/wasm/wasm_main.c's run_lua_cart — same Lua fork
 * (BLYT_LUA_I32_F64), same cart bytecode, same fixed hash seed, same blyt_fpm
 * transcendental seam, same restricted stdlib subset — so its cart-visible
 * output is byte-identical to every other leg (determinism is the core
 * contract, ADR-0007).
 *
 * The whole execution body compiles only when the seam VM is available
 * (BLYT_HOSTLUA_EXEC, set on libblyt by CMake); otherwise the entry points below
 * degrade to no-ops so the frontend falls back to the rv32 session transparently.
 *
 * S2 scope (#238): VM create + restricted stdlib + the minimal blyt/blyt32 API a
 * pure-Lua cart reaches for output and termination (debug.print, quit,
 * should_quit) + sandboxed require + BLMC/raw bytecode loader + direct-call
 * init/update/draw/on_quit/cleanup lifecycle.  State buffers (S-proxy) land in S3,
 * save/restore + reset-every-frame in S4.  Unimplemented cart APIs error LOUDLY
 * rather than silently no-op (anti-#98).
 */

#include <stdbool.h>

#include "blyt_hostlua.h"

#ifdef BLYT_HOSTLUA_EXEC

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

struct blyt_hostlua {
    lua_State *L;
    blyt_log_fn log_fn;
    int quit; /* blyt.quit() latch (mirrors g_quit_requested in blyt_main) */
    bool done; /* on_quit() + cleanup() already run */
};

/* The runner is stashed in the lua_State's extra space so the C API callbacks
 * can reach its log channel + quit latch without a file-scoped global (unlike
 * the WASM leg's g_lua — a native player could in principle host more than one). */
static blyt_hostlua_t *hl_from(lua_State *L) {
    return *(blyt_hostlua_t **)lua_getextraspace(L);
}

/* blyt.debug.print(s) / blyt32.debug.print(s): the cart's cross-leg output
 * channel.  Routed through the runner's log_fn — the SAME callback the emulated
 * path drives from blyt_console_debug — so a line printed here is byte-identical
 * to the emulated leg (the frontend's log sink appends the newline). */
static int l_debug_print(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    blyt_hostlua_t *hl = hl_from(L);
    if (hl && hl->log_fn)
        hl->log_fn(s);
    return 0;
}

static int l_quit(lua_State *L) {
    blyt_hostlua_t *hl = hl_from(L);
    if (hl)
        hl->quit = 1;
    return 0;
}

static int l_should_quit(lua_State *L) {
    blyt_hostlua_t *hl = hl_from(L);
    lua_pushboolean(L, hl ? hl->quit : 0);
    return 1;
}

/* Sandboxed require(): the host-Lua fast path replaces the default searcher with
 * a hard error so a cart cannot reach the host filesystem — only modules already
 * registered in package.loaded (native exports) resolve.  Mirrors the WASM leg. */
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
 * multi-chunk container (issue #54).  Byte-for-byte the WASM leg's loader.
 * Returns 0 on success; on failure leaves an error string on the stack top. */
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

/* Register the blyt/blyt32 API surface a pure-Lua cart reaches for output and
 * termination: blyt.debug.print, blyt.quit, blyt.should_quit, blyt_quit,
 * blyt32.debug.print.  Mirrors the WASM leg's core registration in run_lua_cart
 * (the fuller surface — state buffers, gfx — lands in S3/#231). */
static void register_blyt_api(lua_State *L) {
    /* blyt32 = { debug = { print } } */
    lua_newtable(L);
    lua_newtable(L);
    lua_pushcfunction(L, l_debug_print);
    lua_setfield(L, -2, "print");
    lua_setfield(L, -2, "debug");
    lua_setglobal(L, "blyt32");

    /* blyt = { debug = { print }, quit, should_quit } */
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

/* Call a global lifecycle function `name` if it is defined.  Returns 0 when the
 * callback ran cleanly (or is undefined) and -1 when it raised a Lua error (the
 * message is logged). */
static int call_lifecycle(blyt_hostlua_t *hl, const char *name) {
    lua_State *L = hl->L;
    lua_getglobal(L, name);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return 0;
    }
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        if (hl->log_fn) {
            char buf[512];
            snprintf(buf, sizeof(buf), "blyt-hostlua: error in %s(): %s", name,
                     msg ? msg : "(no message)");
            hl->log_fn(buf);
        } else {
            fprintf(stderr, "blyt-hostlua: error in %s(): %s\n", name, msg ? msg : "(no message)");
        }
        lua_pop(L, 1);
        return -1;
    }
    return 0;
}

bool blyt_hostlua_available(void) {
    return true;
}

blyt_hostlua_t *blyt_hostlua_create(blyt_cart_t *cart, blyt_log_fn log_fn) {
    if (!cart)
        return NULL;

    size_t lua_size = 0;
    const void *bytecode = blyt_cart_find_section(cart, ".cart.lua", &lua_size);
    if (!bytecode || !lua_size)
        return NULL;

    blyt_hostlua_t *hl = calloc(1, sizeof(*hl));
    if (!hl)
        return NULL;
    hl->log_fn = log_fn;

    /* luaL_newstate uses the seam VM's pinned hash seed (luai_makeseed ==
     * 0x424C5954) — the SAME VM construction every other leg uses. */
    hl->L = luaL_newstate();
    if (!hl->L) {
        free(hl);
        return NULL;
    }
    *(blyt_hostlua_t **)lua_getextraspace(hl->L) = hl;

    open_libs(hl->L);
    register_blyt_api(hl->L);

    if (load_lua_bytecode(hl->L, (const unsigned char *)bytecode, lua_size) != 0) {
        const char *msg = lua_tostring(hl->L, -1);
        if (log_fn) {
            char buf[512];
            snprintf(buf, sizeof(buf), "blyt-hostlua: failed to load cart bytecode: %s",
                     msg ? msg : "(no message)");
            log_fn(buf);
        } else {
            fprintf(stderr, "blyt-hostlua: failed to load cart bytecode: %s\n",
                    msg ? msg : "(no message)");
        }
        lua_close(hl->L);
        free(hl);
        return NULL;
    }

    /* Boot phase of the guest blyt_main loop: init() then on_new_state(). */
    if (call_lifecycle(hl, "init") != 0 || call_lifecycle(hl, "on_new_state") != 0) {
        lua_close(hl->L);
        free(hl);
        return NULL;
    }

    return hl;
}

blyt_cart_run_err_t blyt_hostlua_run_frame(blyt_hostlua_t *hl) {
    if (!hl)
        return BLYT_RUN_ERR_EMU;
    if (hl->done)
        return BLYT_RUN_OK;

    /* Quit is tested at the top of the call, mirroring blyt_main's
     * `while (!g_quit_requested)`: a quit requested during a prior update() still
     * ran that frame's draw(); the exit runs on_quit() + cleanup() once. */
    if (hl->quit) {
        call_lifecycle(hl, "on_quit");
        call_lifecycle(hl, "cleanup");
        hl->done = true;
        return BLYT_RUN_OK;
    }

    if (call_lifecycle(hl, "update") != 0 || call_lifecycle(hl, "draw") != 0) {
        hl->done = true;
        return BLYT_RUN_ERR_ABORT;
    }
    return BLYT_RUN_FRAME_DONE;
}

void blyt_hostlua_destroy(blyt_hostlua_t *hl) {
    if (!hl)
        return;
    if (hl->L)
        lua_close(hl->L);
    free(hl);
}

/* Opt-in dispatch predicate.  Pure-Lua = has .cart.lua, no cart-native lifecycle
 * symbol, and no .lua_exports (typed/bridged exports ⇒ hybrid, stays on rv32). */
bool blyt_hostlua_should_use(const blyt_cart_t *cart) {
    if (!cart)
        return false;
    if (!getenv("BLYT_HOSTLUA"))
        return false;
    if (!blyt_cart_find_section(cart, ".cart.lua", NULL))
        return false;
    if (blyt_cart_has_native_lifecycle(cart))
        return false;
    if (blyt_cart_find_section(cart, ".lua_exports", NULL))
        return false;
    return true;
}

#else /* !BLYT_HOSTLUA_EXEC — seam VM absent; the frontend falls back to rv32. */

bool blyt_hostlua_available(void) {
    return false;
}

bool blyt_hostlua_should_use(const blyt_cart_t *cart) {
    (void)cart;
    return false;
}

blyt_hostlua_t *blyt_hostlua_create(blyt_cart_t *cart, blyt_log_fn log_fn) {
    (void)cart;
    (void)log_fn;
    return NULL;
}

blyt_cart_run_err_t blyt_hostlua_run_frame(blyt_hostlua_t *hl) {
    (void)hl;
    return BLYT_RUN_ERR_EMU;
}

void blyt_hostlua_destroy(blyt_hostlua_t *hl) {
    (void)hl;
}

#endif /* BLYT_HOSTLUA_EXEC */
