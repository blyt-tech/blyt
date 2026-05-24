/*
 * frontends/wasm/wasm_main.c — Emscripten (WASM) frontend for blyt.
 *
 * Normal mode (browser / `blyt run`):
 *   The session API drives one frame per Emscripten animation tick via
 *   emscripten_set_main_loop.  Guest libraries are embedded as compiled-in
 *   byte arrays (BLYT_EMBED_LIBS) and registered with blyt_register_lib so
 *   dynlink can load them from memory without a filesystem path.  The cart
 *   is expected at "/cart.blyt" in Emscripten's MEMFS; shell.html pre-populates
 *   it via FS.writeFile before main() runs.
 *
 * Headless mode (Node.js test driver):
 *   tests/wasm/run_cart.js sets globalThis.__blyt_frame0_path to a host
 *   filesystem path.  After the first BLYT_ECALL_FRAME_DONE, wasm_main
 *   writes the XRGB8888 frame directly to that path via require('fs') and
 *   exits.  This is the mechanism for wasm_testcard_frame0_matches_golden.
 *
 * Debugging transport (planned for the future):
 *   DAP (Lua source-level):  ws://localhost:<PORT+1>/dap
 *   GDB RSP (native cart):   ws://localhost:<PORT+2>/gdb
 *
 *   `blyt run` will start two WebSocket bridge servers alongside the HTTP
 *   server.  The WASM runtime opens outbound WebSocket connections to
 *   these bridges; VS Code connects to the same endpoints.  Registration
 *   points are marked below.  The bridge protocol follows Spike J's
 *   dap_server.c / gdb_stub.c surfaces, translated to WebSocket framing.
 */

#include <emscripten.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "blyt_runtime.h"

#ifdef BLYT_LUA
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#endif

/* -------------------------------------------------------------------------
 * rv32emu no-op stubs
 *
 * emulate.c calls enable_run_button() / disable_run_button() when the
 * emulator finishes a run.  These are EM_JS helpers defined in rv32emu's
 * em_runtime.c that target an upstream HTML shell button we don't have.
 * Provide no-op replacements so the linker is satisfied.
 * ------------------------------------------------------------------------- */

void enable_run_button(void) {
}
void disable_run_button(void) {
}

/* -------------------------------------------------------------------------
 * Embedded guest libraries
 * ------------------------------------------------------------------------- */

#ifdef BLYT_EMBED_LIBS
extern const unsigned char blytcommon_so[];
extern const unsigned int blytcommon_so_len;
extern const unsigned char blytc_so[];
extern const unsigned int blytc_so_len;
extern const unsigned char blyt32_so[];
extern const unsigned int blyt32_so_len;
#endif

/* -------------------------------------------------------------------------
 * State
 * ------------------------------------------------------------------------- */

static blyt_cart_t *g_cart = NULL;
static blyt_session_t *g_session = NULL;
static uint32_t g_xrgb[BLYT_FRAME_W * BLYT_FRAME_H];

#ifdef BLYT_LUA
/* Lua-direct path state (used when the cart contains a .cart.lua section). */
static lua_State *g_lua = NULL;
static bool g_lua_quit = false;
static bool g_lua_active = false;
#endif

/* -------------------------------------------------------------------------
 * JavaScript helpers (defined once, called from C; clang-format off guards
 * prevent the JS bodies from being mangled by the C formatter).
 * ------------------------------------------------------------------------- */

/* clang-format off */

/* Route blyt_console_debug output to the browser console. */
EM_JS(void, blyt_js_log, (const char *msg), {
    console.log(UTF8ToString(msg));
});

/* Report a fatal runtime error to the browser console. */
EM_JS(void, blyt_js_error, (const char *msg), {
    console.error('[blyt] ' + UTF8ToString(msg));
});

/* Blit the XRGB8888 frame buffer to the HTML canvas. */
EM_JS(void, blyt_js_present, (const uint32_t *xrgb, int w, int h), {
    var canvas = document.getElementById('canvas');
    if (!canvas) return;
    var ctx = canvas.getContext('2d');
    var imageData = ctx.createImageData(w, h);
    var heap32 = new Uint32Array(HEAPU8.buffer, xrgb, w * h);
    var rgba = imageData.data;
    for (var i = 0; i < w * h; i++) {
        var p = heap32[i];
        rgba[i * 4 + 0] = (p >> 16) & 0xff;
        rgba[i * 4 + 1] = (p >>  8) & 0xff;
        rgba[i * 4 + 2] = p & 0xff;
        rgba[i * 4 + 3] = 0xff;
    }
    ctx.putImageData(imageData, 0, 0);
});

/*
 * Headless frame dump (Node.js test driver only).
 *
 * Returns non-zero if globalThis.__blyt_frame0_path is set (we are in headless
 * mode).  When active, writes the XRGB8888 frame directly to the host
 * filesystem via require('fs') and clears the path so it fires only once.
 * In a browser, require is undefined so this is always a no-op.
 */
EM_JS(int, blyt_js_dump_frame0_if_headless, (const uint32_t *xrgb, int npixels), {
    var hostPath = typeof globalThis !== 'undefined' && globalThis.__blyt_frame0_path;
    if (!hostPath) return 0;
    if (typeof require !== 'undefined') {
        var data = new Uint8Array(HEAPU8.buffer, xrgb, npixels * 4);
        require('fs').writeFileSync(
            hostPath, Buffer.from(data.buffer, data.byteOffset, data.byteLength));
    }
    globalThis.__blyt_frame0_path = null;
    return 1;
});

/* clang-format on */

/* -------------------------------------------------------------------------
 * Log callback: routes blyt_console_debug to the browser console
 * ------------------------------------------------------------------------- */

static void wasm_log(const char *msg) {
    blyt_js_log(msg);
}

#ifdef BLYT_LUA
/* -------------------------------------------------------------------------
 * Lua-direct path: host-side Lua execution for carts with .cart.lua sections
 * ------------------------------------------------------------------------- */

static int lua_wasm_debug_print(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    blyt_js_log(s);
    return 0;
}

static int lua_wasm_quit(lua_State *L) {
    (void)L;
    g_lua_quit = true;
    return 0;
}

static void lua_wasm_call_global(lua_State *L, const char *name) {
    lua_getglobal(L, name);
    if (lua_isfunction(L, -1)) {
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            blyt_js_error(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
}

/* Called once per animation frame by Emscripten when a Lua cart is active. */
static void wasm_lua_loop(void) {
    if (!g_lua)
        return;

    if (g_lua_quit) {
        emscripten_cancel_main_loop();
        lua_close(g_lua);
        g_lua = NULL;
        blyt_cart_close(g_cart);
        g_cart = NULL;
        return;
    }

    lua_wasm_call_global(g_lua, "update");
    lua_wasm_call_global(g_lua, "draw");
    blyt_js_present(g_xrgb, BLYT_FRAME_W, BLYT_FRAME_H);

    if (blyt_js_dump_frame0_if_headless(g_xrgb, BLYT_FRAME_W * BLYT_FRAME_H)) {
        emscripten_cancel_main_loop();
        lua_close(g_lua);
        g_lua = NULL;
        blyt_cart_close(g_cart);
        g_cart = NULL;
    }
}

/* Initialise and start a Lua cart from its embedded bytecode section. */
static int run_lua_cart(const void *bytecode, size_t bytecode_size) {
    g_lua = luaL_newstate();
    if (!g_lua) {
        blyt_js_error("failed to create Lua state");
        return 1;
    }

    luaL_requiref(g_lua, "_G", luaopen_base, 1);
    lua_pop(g_lua, 1);
    luaL_requiref(g_lua, "math", luaopen_math, 1);
    lua_pop(g_lua, 1);
    luaL_requiref(g_lua, "string", luaopen_string, 1);
    lua_pop(g_lua, 1);
    luaL_requiref(g_lua, "table", luaopen_table, 1);
    lua_pop(g_lua, 1);
    luaL_requiref(g_lua, "coroutine", luaopen_coroutine, 1);
    lua_pop(g_lua, 1);

    /* Register blyt32 API */
    lua_newtable(g_lua);
    lua_newtable(g_lua);
    lua_pushcfunction(g_lua, lua_wasm_debug_print);
    lua_setfield(g_lua, -2, "print");
    lua_setfield(g_lua, -2, "debug");
    lua_setglobal(g_lua, "blyt32");

    lua_pushcfunction(g_lua, lua_wasm_quit);
    lua_setglobal(g_lua, "blyt_quit");

    /* Load and execute the bytecode chunk (defines init/update/draw globals) */
    if (luaL_loadbuffer(g_lua, (const char *)bytecode, bytecode_size, "@cart") != LUA_OK) {
        blyt_js_error(lua_tostring(g_lua, -1));
        lua_close(g_lua);
        g_lua = NULL;
        return 1;
    }
    if (lua_pcall(g_lua, 0, 0, 0) != LUA_OK) {
        blyt_js_error(lua_tostring(g_lua, -1));
        lua_close(g_lua);
        g_lua = NULL;
        return 1;
    }

    lua_wasm_call_global(g_lua, "init");
    if (g_lua_quit) {
        lua_close(g_lua);
        g_lua = NULL;
        return 0;
    }

    g_lua_active = true;
    emscripten_set_main_loop(wasm_lua_loop, 0, 1);
    return 0;
}
#endif

/* -------------------------------------------------------------------------
 * Main loop — called by Emscripten once per animation frame
 * ------------------------------------------------------------------------- */

static void wasm_loop(void) {
    if (!g_session)
        return;

    blyt_cart_run_err_t err = blyt_session_run_frame(g_session);
    blyt_session_expand_frame(g_session, g_xrgb);

    bool done = (err != BLYT_RUN_FRAME_DONE);

    if (blyt_js_dump_frame0_if_headless(g_xrgb, BLYT_FRAME_W * BLYT_FRAME_H)) {
        /* Headless dump complete — exit after this frame. */
        done = true;
    } else {
        blyt_js_present(g_xrgb, BLYT_FRAME_W, BLYT_FRAME_H);
        if (done) {
            if (err == BLYT_RUN_ERR_ECALL_TRAP)
                blyt_js_error("cart attempted a non-permitted ecall");
            else if (err == BLYT_RUN_ERR_ABORT)
                blyt_js_error("cart aborted");
        }
    }

    if (done) {
        emscripten_cancel_main_loop();
        blyt_session_destroy(g_session);
        g_session = NULL;
        blyt_cart_close(g_cart);
        g_cart = NULL;
    }
}

/* -------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------- */

int main(void) {
#ifdef BLYT_EMBED_LIBS
    blyt_register_lib("libblytcommon.so", blytcommon_so, blytcommon_so_len);
    blyt_register_lib("libblytc.so", blytc_so, blytc_so_len);
    blyt_register_lib("libblyt32.so", blyt32_so, blyt32_so_len);
#endif

    /* TODO: register DAP WebSocket bridge connection here (planned). */
    /* TODO: register GDB RSP WebSocket bridge connection here (planned). */

    blyt_cart_err_t cerr = blyt_cart_open("/cart.blyt", &g_cart);
    if (cerr != BLYT_CART_OK) {
        blyt_js_error(blyt_cart_err_str(cerr));
        return 1;
    }

#ifdef BLYT_LUA
    /* Lua-direct path: if the cart has a .cart.lua section, run it directly
     * in a host-side Lua state rather than through rv32emu. */
    size_t lua_bytecode_size = 0;
    const void *lua_bytecode = blyt_cart_find_section(g_cart, ".cart.lua", &lua_bytecode_size);
    if (lua_bytecode) {
        return run_lua_cart(lua_bytecode, lua_bytecode_size);
    }
#endif

    g_session = blyt_session_create(g_cart, wasm_log);
    if (!g_session) {
        blyt_js_error("failed to create session");
        blyt_cart_close(g_cart);
        return 1;
    }

    /* 0 fps = use requestAnimationFrame (browser) or setTimeout (Node.js);
     * simulate_infinite_loop=1 makes main() block until the loop is cancelled. */
    emscripten_set_main_loop(wasm_loop, 0, 1);
    return 0;
}
