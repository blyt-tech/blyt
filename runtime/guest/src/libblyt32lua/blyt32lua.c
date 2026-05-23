/*
 * runtime/guest/src/libblyt32lua/blyt32lua.c
 *
 * Guest-side Lua cart library.
 *
 * Provides blyt_cart_init/update/draw as strong symbols so the cart validator
 * accepts them from this library (via DT_NEEDED: libblyt32lua.so).
 *
 * Registers:
 *   blyt.quit()           → blyt_quit()  (blyt and blyt32 are the same table)
 *   blyt.debug.print(s)   → blyt_console_debug(s)
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

static void register_blyt32(lua_State *L) {
    lua_newtable(L);                             /* blyt / blyt32 (same table) */
    lua_pushcfunction(L, lua_blyt_quit);
    lua_setfield(L, -2, "quit");
    lua_newtable(L);                             /* .debug */
    lua_pushcfunction(L, lua_blyt_debug_print);
    lua_setfield(L, -2, "print");
    lua_setfield(L, -2, "debug");
    lua_pushvalue(L, -1);
    lua_setglobal(L, "blyt32");
    lua_setglobal(L, "blyt");

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
        int load_result = luaL_loadbuffer(L, (const char *)cart_lua_bytecode,
                            cart_lua_bytecode_size, "@cart");
        blyt_console_debug(load_result == LUA_OK ? "open_state: loadbuffer OK" : "open_state: loadbuffer FAILED");
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
    lua_getglobal(g_L, name);
    if (lua_isfunction(g_L, -1)) {
        if (lua_pcall(g_L, 0, 0, 0) != LUA_OK) {
            blyt_console_debug(lua_tostring(g_L, -1));
            lua_pop(g_L, 1);
        }
    } else {
        lua_pop(g_L, 1);
    }
}

void blyt_cart_init(void) {
    blyt_console_debug("blyt_cart_init: start");
    g_L = open_state();
    blyt_console_debug(g_L ? "blyt_cart_init: open_state ok" : "blyt_cart_init: open_state FAILED");
    call_global("init");
    blyt_console_debug("blyt_cart_init: done");
}

void blyt_cart_update(void) { call_global("update"); }
void blyt_cart_draw(void)   { call_global("draw"); }
