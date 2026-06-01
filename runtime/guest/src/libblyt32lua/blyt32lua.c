/*
 * runtime/guest/src/libblyt32lua/blyt32lua.c
 *
 * Guest-side Lua cart library.
 *
 * Provides blyt_cart_init/update/draw as strong symbols so the cart validator
 * accepts them from this library (via DT_NEEDED: libblyt32lua.so).
 *
 * Registers:
 *   blyt.quit()      → blyt_quit()
 *   blyt.debug.print(s)    → blyt_console_debug(s)
 *   blyt32.quit()    → blyt_quit()
 *   blyt32.debug.print(s)  → blyt_console_debug(s)
 *
 * cart_lua_modules(L) is a weak symbol; C/Rust src/lib/ libraries define it
 * to register additional Lua modules before the cart bytecode is loaded.
 *
 * cart_lua_bytecode / cart_lua_bytecode_size are weak; the per-cart generated
 * cart_lua_data.c object provides the strong definitions.
 */

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include "blyt.h"
#ifdef BLYT_DAP
#include "master_hook.h"
/* Weak default: 0 (hook disabled).  master_hook_ecall.c provides the strong
 * definition that probes the host via BLYT_ECALL_DAP_HOOK. */
int blyt_dap_active(void) __attribute__((weak));
int blyt_dap_active(void) {
    return 0;
}
/* Weak default no-op; strong definitions in master_hook_ecall.c (ECALL path)
 * and dap_transport_wasm.c (WASM path) call the actual transport. */
int blyt_dap_report_exception(lua_State *L, int is_uncaught) __attribute__((weak));
int blyt_dap_report_exception(lua_State *L, int is_uncaught) {
    (void)L;
    (void)is_uncaught;
    return 0;
}
static int dap_error_handler(lua_State *L) {
    if (blyt_dap_report_exception(L, 1))
        fc_dap_pause_loop(L, NULL);
    return 1;
}
#endif

/* Per-cart bytecode — defined in the generated cart_lua_data.c object. */
extern const unsigned char cart_lua_bytecode[] __attribute__((weak));
extern const unsigned int cart_lua_bytecode_size __attribute__((weak));

/* Per-cart Lua module registration — defined by src/lib/ libraries. */
void cart_lua_modules(lua_State *L) __attribute__((weak));

static lua_State *g_L;

/*
 * Minimal require() for sandboxed carts.
 *
 * Only pre-registered modules (placed in LUA_LOADED_TABLE by luaL_requiref
 * from cart_lua_modules) are visible.  Filesystem loading is not supported.
 */
static int lua_blyt_require(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
    lua_getfield(L, -1, name);
    if (!lua_isnil(L, -1)) {
        lua_remove(L, -2); /* remove _LOADED table */
        return 1;
    }
    lua_pop(L, 2);
    return luaL_error(L, "module '%s' not found (only pre-registered modules available)", name);
}

static int lua_blyt_debug_print(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    blyt_console_debug(s);
    return 0;
}

static int lua_blyt_quit(lua_State *L) {
    (void)L;
    blyt_quit();
    return 0;
}

/*
 * register_blyt32 — create the blyt and blyt32 globals.
 *
 * Per ADR-0086:
 *   blyt.*   — canonical home for shared modules (audio, state, input, quit …)
 *   blyt32.* — variant-specific (gfx, color, spatial) PLUS aliases to every
 *              shared module so that blyt32.foo == blyt.foo is always true.
 *
 * Pattern: build each shared subtable/function once, then copy the reference
 * into both tables so the Lua equality check holds.
 */
static void register_blyt32(lua_State *L) {
    /* --- shared: blyt.debug subtable --- */
    lua_newtable(L); /* idx A: blyt.debug */
    lua_pushcfunction(L, lua_blyt_debug_print);
    lua_setfield(L, -2, "print");

    /* --- blyt table --- */
    lua_newtable(L); /* blyt */
    lua_pushvalue(L, -2); /* copy ref to blyt.debug */
    lua_setfield(L, -2, "debug");
    lua_pushcfunction(L, lua_blyt_quit); /* shared: blyt.quit */
    lua_setfield(L, -2, "quit");
    lua_setglobal(L, "blyt"); /* pops blyt */

    /* --- blyt32 table — distinct table; shared entries alias blyt.* --- */
    lua_newtable(L); /* blyt32 */
    lua_pushvalue(L, -2); /* copy ref to blyt.debug (idx A still on stack) */
    lua_setfield(L, -2, "debug");
    lua_getglobal(L, "blyt");
    lua_getfield(L, -1, "quit"); /* blyt.quit */
    lua_setfield(L, -3, "quit"); /* blyt32.quit = blyt.quit */
    lua_pop(L, 1); /* pop blyt */
    lua_setglobal(L, "blyt32"); /* pops blyt32 */

    lua_pop(L, 1); /* pop blyt.debug (idx A) */

    lua_pushcfunction(L, lua_blyt_require);
    lua_setglobal(L, "require");
}

static lua_State *open_state(void) {
    blyt_console_debug("open_state: before luaL_newstate");
    lua_State *L = luaL_newstate();
    blyt_console_debug(L ? "open_state: newstate ok" : "open_state: newstate NULL");
    if (!L)
        return NULL;

    blyt_console_debug("open_state: skipping requiref");

    blyt_console_debug("open_state: before register_blyt32");
    register_blyt32(L);
    blyt_console_debug("open_state: register_blyt32 done");

    if (cart_lua_modules)
        cart_lua_modules(L);

    if (cart_lua_bytecode && cart_lua_bytecode_size) {
        blyt_console_debug("open_state: before luaL_loadbuffer");
        int load_result =
            luaL_loadbuffer(L, (const char *)cart_lua_bytecode, cart_lua_bytecode_size, "@cart");
        blyt_console_debug(load_result == LUA_OK ? "open_state: loadbuffer OK"
                                                 : "open_state: loadbuffer FAILED");
        if (load_result != LUA_OK) {
            blyt_console_debug(lua_tostring(L, -1));
            lua_close(L);
            return NULL;
        }
        blyt_console_debug("open_state: before lua_pcall");
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            blyt_console_debug(lua_tostring(L, -1));
            lua_close(L);
            return NULL;
        }
        blyt_console_debug("open_state: lua_pcall done");
    }

    return L;
}

static void call_global(const char *name) {
    if (!g_L)
        return;
    int msgh = 0;
#ifdef BLYT_DAP
    if (blyt_dap_active()) {
        lua_pushcfunction(g_L, dap_error_handler);
        msgh = lua_gettop(g_L);
    }
#endif
    lua_getglobal(g_L, name);
    if (lua_isfunction(g_L, -1)) {
        if (lua_pcall(g_L, 0, 0, msgh) != LUA_OK) {
            blyt_console_debug(lua_tostring(g_L, -1));
            lua_pop(g_L, 1);
        }
    } else {
        lua_pop(g_L, 1);
    }
#ifdef BLYT_DAP
    if (msgh > 0)
        lua_remove(g_L, msgh);
#endif
}

void blyt_cart_init(void) {
    blyt_console_debug("blyt_cart_init: start");
    g_L = open_state();
    blyt_console_debug(g_L ? "blyt_cart_init: open_state ok" : "blyt_cart_init: open_state FAILED");
#ifdef BLYT_DAP
    if (g_L && blyt_dap_active()) {
        fc_master_hook_cfg.dap_enabled = true;
        fc_consolelua_master_hook_install(g_L);
    }
#endif
    call_global("init");
    blyt_console_debug("blyt_cart_init: done");
}

void blyt_cart_update(void) {
    call_global("update");
}
void blyt_cart_draw(void) {
    call_global("draw");
}
