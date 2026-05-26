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
 * Debugging transport (DAP, Lua source-level):
 *   ws://localhost:<PORT+1>/dap — relay bridging WASM runtime and VS Code.
 *
 *   When BLYT_DAP is compiled in, `blyt run --debug` starts a WebSocket relay
 *   on PORT+1 and injects window.blyt_dap_port into the served shell.html.
 *   run_lua_cart() reads that port, connects outbound via dap_transport_wasm.c,
 *   and installs the master Lua hook so VS Code can set breakpoints and step.
 */

#include <emscripten.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "blyt_runtime.h"

#ifdef BLYT_LUA
#include "testcard.h"
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#endif

#ifdef BLYT_DAP
#include "dap_server.h"
#include "master_hook.h"
#endif
#ifdef BLYT_GDB
#include "gdb_stub.h"
#include "gdb_transport_wasm.h"
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
static lua_State *g_lua_co = NULL; /* game-loop coroutine */
static int g_lua_co_ref = LUA_NOREF;
static bool g_lua_quit = false;
static bool g_lua_active = false;
#ifdef BLYT_DAP
static bool g_lua_dap_paused = false; /* hook yielded, waiting for DAP */
static bool g_lua_needs_start = false; /* waiting for configurationDone */
#endif
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
    if (typeof document === 'undefined') return;
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

/* Read the DAP relay port injected by `blyt run --debug` into shell.html.
 * Returns 0 when not set (DAP disabled or running outside blyt run). */
#ifdef BLYT_DAP
EM_JS(int, blyt_js_dap_port, (void), {
    /* Node.js test driver sets globalThis.__blyt_dap_port.
     * Browser shell.html sets window.blyt_dap_port via {{BLYT_DAP_PORT}}. */
    if (typeof globalThis !== 'undefined' && globalThis.__blyt_dap_port > 0)
        return globalThis.__blyt_dap_port | 0;
    if (typeof window !== 'undefined' && window.blyt_dap_port > 0)
        return window.blyt_dap_port | 0;
    return 0;
});
#endif

/* Read the GDB relay port injected by `blyt run --gdb` into shell.html.
 * Returns 0 when not set. */
#ifdef BLYT_GDB
EM_JS(int, blyt_js_gdb_port, (void), {
    if (typeof globalThis !== 'undefined' && globalThis.__blyt_gdb_port > 0)
        return globalThis.__blyt_gdb_port | 0;
    if (typeof window !== 'undefined' && window.blyt_gdb_port > 0)
        return window.blyt_gdb_port | 0;
    return 0;
});
#endif

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
#ifdef BLYT_DAP
    fc_dap_output(s);
#endif
    return 0;
}

static int lua_wasm_quit(lua_State *L) {
    (void)L;
    g_lua_quit = true;
    return 0;
}

/* Set by host-provided blyt32 drawing functions; cleared each frame before draw(). */
static bool g_lua_drawn = false;

/* Render testcard into g_xrgb — used when draw() produces no output. */
static void render_testcard(void) {
    static uint8_t s_pixels[BLYT_FRAME_W * BLYT_FRAME_H];
    static uint32_t s_palette[256];
    static uint32_t s_frame = 0;
    static bool s_tc_init = false;
    if (!s_tc_init) {
        blyt_testcard_init_palette(s_palette);
        s_tc_init = true;
    }
    blyt_testcard_draw(s_frame++, s_pixels);
    for (int i = 0; i < BLYT_FRAME_W * BLYT_FRAME_H; i++)
        g_xrgb[i] = s_palette[s_pixels[i]];
}

/* Release all Lua and cart resources. */
static void lua_cleanup(void) {
    if (g_lua_co_ref != LUA_NOREF && g_lua) {
        luaL_unref(g_lua, LUA_REGISTRYINDEX, g_lua_co_ref);
        g_lua_co_ref = LUA_NOREF;
    }
    g_lua_co = NULL;
    if (g_lua) {
        lua_close(g_lua);
        g_lua = NULL;
    }
    if (g_cart) {
        blyt_cart_close(g_cart);
        g_cart = NULL;
    }
}

/* Called once per animation frame by Emscripten when a Lua cart is active. */
static void wasm_lua_loop(void) {
    if (!g_lua)
        return;

#ifdef BLYT_DAP
    /* Drain WebSocket queue: delivers new breakpoints, continue/step responses, etc.
     * Must be called outside the Lua hook to avoid interfering with step state. */
    fc_dap_poll_messages();

    /* If DAP is active, wait for the client to finish configuration (setBreakpoints,
     * configurationDone) before calling init() and starting the game. */
    if (g_lua_needs_start) {
        if (!fc_dap_configuration_done()) {
            blyt_js_present(g_xrgb, BLYT_FRAME_W, BLYT_FRAME_H);
            return;
        }
        g_lua_needs_start = false;
        fc_consolelua_master_hook_install(g_lua_co);
        /* fall through to start the game */
    }

    /* If a line hook yielded the coroutine (DAP pause), wait here until the
     * client sends continue/step. */
    if (g_lua_dap_paused) {
        if (!fc_dap_continue_pending()) {
            /* Re-present the last rendered frame unchanged — the game is
             * suspended so neither the test card nor the Lua frame advance. */
            blyt_js_present(g_xrgb, BLYT_FRAME_W, BLYT_FRAME_H);
            return;
        }
        /* Client sent continue/step — apply step mode and send "continued". */
        fc_dap_do_resume();
        g_lua_dap_paused = false;
        /* fall through to resume the coroutine */
    }
#endif

    if (g_lua_quit) {
        emscripten_cancel_main_loop();
        lua_cleanup();
        return;
    }

    if (!g_lua_co)
        return;

    /* Advance the game-loop coroutine by one frame. */
    g_lua_drawn = false;
    int nresults = 0;
    int status = lua_resume(g_lua_co, g_lua, 0, &nresults);
    lua_settop(g_lua_co, 0); /* discard any yielded values */

    if (status == LUA_YIELD) {
#ifdef BLYT_DAP
        /* Distinguish a DAP pause yield from the normal per-frame coroutine.yield(). */
        if (fc_dap_hook_yielded()) {
            g_lua_dap_paused = true;
            blyt_js_present(g_xrgb, BLYT_FRAME_W, BLYT_FRAME_H);
            return;
        }
#endif
        /* Normal frame-end yield — fall through to render. */
    } else {
        /* LUA_OK = coroutine finished (shouldn't happen with while true);
         * anything else = Lua error. */
        if (status != LUA_OK) {
            const char *msg = lua_tostring(g_lua_co, -1);
            blyt_js_error(msg ? msg : "Lua runtime error");
        }
        g_lua_quit = true;
        return;
    }

    if (!g_lua_drawn)
        render_testcard();

    blyt_js_present(g_xrgb, BLYT_FRAME_W, BLYT_FRAME_H);

    if (blyt_js_dump_frame0_if_headless(g_xrgb, BLYT_FRAME_W * BLYT_FRAME_H)) {
        emscripten_cancel_main_loop();
        lua_cleanup();
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

    /* Register blyt.quit() — also exposed as blyt_quit() for compatibility. */
    lua_newtable(g_lua);
    lua_pushcfunction(g_lua, lua_wasm_quit);
    lua_setfield(g_lua, -2, "quit");
    lua_setglobal(g_lua, "blyt");
    lua_pushcfunction(g_lua, lua_wasm_quit);
    lua_setglobal(g_lua, "blyt_quit");

    /* Load and execute the bytecode chunk (defines init/update/draw globals). */
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

    /* Create the game-loop coroutine.
     *
     * The body calls init() once, then loops: update(), optional draw(), yield.
     * Running inside a coroutine lets fc_dap_pause_loop() suspend execution via
     * lua_yield() rather than emscripten_sleep() / ASYNCIFY — Lua's own
     * CIST_HOOKYIELD mechanism correctly preserves the VM PC across the pause. */
    g_lua_co = lua_newthread(g_lua);
    if (!g_lua_co) {
        blyt_js_error("failed to create game-loop coroutine");
        lua_close(g_lua);
        g_lua = NULL;
        return 1;
    }
    /* lua_newthread pushes the thread onto g_lua's stack; anchor it via registry
     * so the GC doesn't collect it. */
    g_lua_co_ref = luaL_ref(g_lua, LUA_REGISTRYINDEX);

    /* Compile the loop body and push it onto the coroutine's stack.
     * The first lua_resume() will invoke this chunk. */
    static const char co_body[] = "init() "
                                  "while true do "
                                  "  update() "
                                  "  if type(draw) == 'function' then draw() end "
                                  "  coroutine.yield() "
                                  "end";
    if (luaL_loadstring(g_lua_co, co_body) != LUA_OK) {
        blyt_js_error(lua_tostring(g_lua_co, -1));
        lua_close(g_lua);
        g_lua = NULL;
        return 1;
    }

#ifdef BLYT_DAP
    {
        int dap_port = blyt_js_dap_port();
        if (dap_port > 0 && fc_consolelua_dap_listen(dap_port) > 0) {
            fc_master_hook_cfg.dap_enabled = true;
            /* Defer hook installation and game start until the DAP client sends
             * configurationDone, so breakpoints set during launch are in place
             * before init() or update() run. */
            g_lua_needs_start = true;
        }
    }
    /* No DAP, or DAP port not set: install hook immediately (no-op when disabled). */
    if (fc_master_hook_cfg.dap_enabled && !g_lua_needs_start)
        fc_consolelua_master_hook_install(g_lua_co);
#endif

    /* Render one static test card frame into g_xrgb before starting the loop.
     * This gives the display a non-black image during the configurationDone
     * wait (DAP) and as the fallback frozen frame for any DAP pause. */
    render_testcard();

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

#ifdef BLYT_GDB
    /* GDB breakpoint/step pause: session polls for vCont internally each tick.
     * Re-present the last frame unchanged until the client sends continue/step. */
    if (err == BLYT_RUN_GDB_PAUSED) {
        blyt_js_present(g_xrgb, BLYT_FRAME_W, BLYT_FRAME_H);
        return;
    }
#endif

    blyt_session_expand_frame(g_session, g_xrgb);

    bool done = (err != BLYT_RUN_FRAME_DONE);

    int headless_dump = blyt_js_dump_frame0_if_headless(g_xrgb, BLYT_FRAME_W * BLYT_FRAME_H);

    if (headless_dump) {
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

#ifdef BLYT_GDB
    {
        int gdb_port = blyt_js_gdb_port();
        if (gdb_port > 0)
            blyt_session_gdb_listen(g_session, &gdb_port);
    }
#endif

    /* 0 fps = use requestAnimationFrame (browser) or setTimeout (Node.js);
     * simulate_infinite_loop=1 makes main() block until the loop is cancelled. */
    emscripten_set_main_loop(wasm_loop, 0, 1);
    return 0;
}
