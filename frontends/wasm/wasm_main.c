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

#include <ctype.h>
#include <emscripten.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blyt_runtime.h"
#include "blyt_trace.h"

#ifdef BLYT_LUA
#include "save.h"
#include "state_buffer.h"
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
extern const unsigned char blyt32lua_so[];
extern const unsigned int blyt32lua_so_len;
#endif

/* -------------------------------------------------------------------------
 * State
 * ------------------------------------------------------------------------- */

static blyt_cart_t *g_cart = NULL;
static blyt_session_t *g_session = NULL;
static uint32_t g_xrgb[BLYT_FRAME_W * BLYT_FRAME_H];
static bool g_reset_every_frame = false; /* BLYT_RESET_EVERY_FRAME=1 */

#ifdef BLYT_LUA
/* Lua-direct path state (used when the cart contains a .cart.lua section).
 *
 * Game loop is driven by a C phase machine that resumes Lua coroutines.
 * A coroutine is kept for DAP yield support: fc_dap_pause_loop() calls
 * lua_yield() from a debug hook, which requires a resumable coroutine.
 *
 * INIT phase: init coroutine runs init() + on_new_state(), then finishes.
 * RUNNING phase: running coroutine loops update()/draw()/yield per frame.
 */
typedef enum { LUA_PHASE_INIT, LUA_PHASE_RUNNING } lua_phase_t;
static lua_phase_t g_lua_phase = LUA_PHASE_INIT;
static lua_State *g_lua = NULL;
static lua_State *g_lua_co = NULL; /* current phase coroutine */
static int g_lua_co_ref = LUA_NOREF;
static bool g_lua_quit = false; /* blyt.quit() called */
static bool g_lua_fatal = false; /* hard stop: error or DAP exception */
static bool g_lua_error = false;
static bool g_lua_active = false;
static lua_State *g_lua_exch = NULL; /* exchange thread for ECALL bridge */
static int g_lua_exch_ref = LUA_NOREF;
static bool g_trampoline_failed = false; /* FN_ERROR from a bridged call */
/* Saved at startup for wasm_lua_reset_cycle */
static const void *g_lua_bytecode = NULL;
static size_t g_lua_bytecode_size = 0;
static bool g_has_lua_exports = false;
/* Lightweight state-only context for pure Lua carts with .cart.layouts but no
 * native code.  Avoids allocating the full 256MB RV32 emulator for these carts.
 * When non-NULL, g_session is NULL and we use g_lua_state_ctx directly. */
static blyt_state_ctx_t *g_lua_state_ctx = NULL;
static char *g_lua_save_dir = NULL;
static char g_lua_cart_name[64];
#ifdef BLYT_DAP
static bool g_lua_dap_paused = false; /* hook yielded, waiting for DAP */
static bool g_lua_needs_start = false; /* waiting for configurationDone */
#endif
#ifdef BLYT_GDB
/* Set when a Lua-to-native trampoline hits a GDB breakpoint and yields the
 * coroutine.  wasm_lua_loop waits here (returning each tick) until the GDB
 * client sends vCont;c, then clears the flag and resumes the coroutine. */
static bool g_trampoline_gdb_paused = false;
#endif
/* BLYT_LUA_TYPE_* constants — must match blyt.h guest SDK definition. */
#define WASM_LUA_TYPE_VOID 0
#define WASM_LUA_TYPE_I32 1
#define WASM_LUA_TYPE_U32 2
#define WASM_LUA_TYPE_F32 3
#define WASM_LUA_TYPE_BOOL 4

/* Coroutine body: run init() + on_new_state(), then finish (LUA_OK). */
static const char co_body_init[] = "init() "
                                   "if type(on_new_state) == 'function' then on_new_state() end";

/* Coroutine body: per-frame update/draw loop with frame-boundary yield. */
static const char co_body_running[] = "while not blyt.should_quit() do "
                                      "  update() "
                                      "  if type(draw) == 'function' then draw() end "
                                      "  coroutine.yield() "
                                      "end "
                                      "if type(on_quit) == 'function' then on_quit() end "
                                      "if type(cleanup) == 'function' then cleanup() end";
#endif /* BLYT_LUA */

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

static int lua_wasm_should_quit(lua_State *L) {
    lua_pushboolean(L, g_lua_quit);
    return 1;
}

/* -------------------------------------------------------------------------
 * WASM hybrid trampolines — synchronous rv32 dispatch from host-side Lua
 *
 * Trampolines drive blyt_session_run_frame() synchronously within the Lua
 * C function call rather than yielding the coroutine.  This eliminates the
 * need for wasm_service_trampoline() and keeps each animation tick bounded
 * to the cost of one Lua frame.  The coroutine is retained for DAP yield
 * support: fc_dap_pause_loop() calls lua_yield() from a debug hook, which
 * requires a resumable coroutine context.
 * ------------------------------------------------------------------------- */

/* Convert a Lua stack value at index idx to a uint32_t for rv32 register. */
static uint32_t wasm_lua_to_rv32(lua_State *L, int idx, int type) {
    switch (type) {
    case WASM_LUA_TYPE_I32:
        return (uint32_t)(int32_t)lua_tointeger(L, idx);
    case WASM_LUA_TYPE_U32:
        return (uint32_t)lua_tointeger(L, idx);
    case WASM_LUA_TYPE_F32: {
        float f = (float)lua_tonumber(L, idx);
        uint32_t bits;
        memcpy(&bits, &f, 4);
        return bits;
    }
    case WASM_LUA_TYPE_BOOL:
        return lua_toboolean(L, idx) ? 1u : 0u;
    default:
        return 0u;
    }
}

/* Push a uint32_t rv32 return value onto the Lua stack. */
static void wasm_rv32_to_lua(lua_State *L, uint32_t val, int type) {
    switch (type) {
    case WASM_LUA_TYPE_I32:
        lua_pushinteger(L, (lua_Integer)(int32_t)val);
        break;
    case WASM_LUA_TYPE_U32:
        lua_pushinteger(L, (lua_Integer)(uint32_t)val);
        break;
    case WASM_LUA_TYPE_F32: {
        float f;
        memcpy(&f, &val, 4);
        lua_pushnumber(L, (lua_Number)f);
        break;
    }
    case WASM_LUA_TYPE_BOOL:
        lua_pushboolean(L, val ? 1 : 0);
        break;
    default:
        break; /* VOID: push nothing */
    }
}

/* Forward declaration for the GDB-pause continuation. */
#ifdef BLYT_GDB
static int trampoline_gdb_resume_k(lua_State *L, int status, lua_KContext ctx);
#endif

/* Run blyt_session_run_frame() in a loop until the native call completes.
 * When a GDB breakpoint fires (BLYT_RUN_GDB_PAUSED) the coroutine yields via
 * lua_yieldk so the Node.js event loop can relay the T05 packet and receive
 * vCont commands.  The continuation re-enters here after vCont;c is processed
 * by wasm_lua_loop (which clears g_trampoline_gdb_paused before resuming). */
static int run_trampoline_loop(lua_State *L, int ret_type) {
    blyt_cart_run_err_t ferr;
    for (;;) {
        ferr = blyt_session_run_frame(g_session);
        if (ferr == BLYT_RUN_FN_DONE)
            break;
        if (ferr == BLYT_RUN_FN_ERROR || ferr == BLYT_RUN_ERR_ECALL_TRAP ||
            ferr == BLYT_RUN_ERR_ABORT)
            break;
#ifdef BLYT_GDB
        if (ferr == BLYT_RUN_GDB_PAUSED) {
            /* Yield the Lua coroutine so the Node.js event loop is free to
             * relay the T05 stop notification and receive the GDB client's
             * response packets (register reads, steps, vCont;c).
             * wasm_lua_loop() will not resume the coroutine until
             * g_trampoline_gdb_paused is cleared (GDB unhalted). */
            g_trampoline_gdb_paused = true;
            return lua_yieldk(L, 0, (lua_KContext)(intptr_t)ret_type, trampoline_gdb_resume_k);
        }
#endif
    }
    if (ferr != BLYT_RUN_FN_DONE)
        return luaL_error(L, "native call failed");
    uint32_t ret_val = blyt_session_fn_return_value(g_session);
    if (!g_lua_quit && blyt_session_check_guest_quit(g_session))
        g_lua_quit = true;
    wasm_rv32_to_lua(L, ret_val, ret_type);
    return (ret_type == WASM_LUA_TYPE_VOID) ? 0 : 1;
}

#ifdef BLYT_GDB
static int trampoline_gdb_resume_k(lua_State *L, int status, lua_KContext ctx) {
    (void)status;
    return run_trampoline_loop(L, (int)(intptr_t)ctx);
}
#endif

/* Lua C closure: one per exported function, installed as a Lua global.
 * Upvalues: [1]=fn_guest_addr [2]=nargs [3..6]=arg_types [7]=ret_type */
static int wasm_make_trampoline(lua_State *L) {
    uint32_t fn_addr = (uint32_t)(uintptr_t)lua_touserdata(L, lua_upvalueindex(1));
    int nargs = (int)lua_tointeger(L, lua_upvalueindex(2));
    uint32_t args[4] = {0};
    for (int i = 0; i < nargs && i < 4; i++)
        args[i] = wasm_lua_to_rv32(L, i + 1, (int)lua_tointeger(L, lua_upvalueindex(3 + i)));
    blyt_session_begin_fn_call(g_session, fn_addr, nargs, args);
    int ret_type = (int)lua_tointeger(L, lua_upvalueindex(7));
    return run_trampoline_loop(L, ret_type);
}

/* -------------------------------------------------------------------------
 * ECALL-bridged trampoline (ADR-0130)
 *
 * For exports flagged BLYT_LUA_EXPORT_FLAG_BRIDGED the guest-side WRAPPER is
 * invoked (with an opaque call token as its lua_State*) and reads/pushes its
 * Lua values itself through BLYT_ECALL_LUA_OP, executed by the host against
 * the exchange thread g_lua_exch.  Arguments are lua_xmove'd from the calling
 * coroutine to the exchange thread before the call; results travel back the
 * same way.  The call is driven synchronously within the Lua C function.
 * ------------------------------------------------------------------------- */

/* Upvalues: [1]=wrap_guest_addr */
static int wasm_make_bridged_trampoline(lua_State *L) {
    uint32_t wrap_addr = (uint32_t)(uintptr_t)lua_touserdata(L, lua_upvalueindex(1));
    int n = lua_gettop(L);
    if (!lua_checkstack(g_lua_exch, n + 2))
        return luaL_error(L, "blyt bridge: exchange stack overflow");
    lua_settop(g_lua_exch, 0); /* defensive: previous call always cleans up */
    lua_xmove(L, g_lua_exch, n); /* wrapper sees args at exch indices 1..n */
    if (blyt_session_begin_bridged_call(g_session, wrap_addr) != 0)
        return luaL_error(L, "blyt bridge: call setup failed");
    blyt_cart_run_err_t ferr;
    do {
        ferr = blyt_session_run_frame(g_session);
    } while (ferr != BLYT_RUN_FN_DONE && ferr != BLYT_RUN_FN_ERROR &&
             ferr != BLYT_RUN_ERR_ECALL_TRAP && ferr != BLYT_RUN_ERR_ABORT);
    if (ferr == BLYT_RUN_FN_ERROR) {
        /* ADR-0130: bridged wrapper raised; guest registers were restored.
         * Re-raise inside the Lua call so a script-level pcall catches it. */
        if (lua_gettop(g_lua_exch) < 1)
            lua_pushstring(g_lua_exch, "blyt bridge: unknown error");
        lua_xmove(g_lua_exch, L, 1);
        lua_settop(g_lua_exch, 0);
        return lua_error(L);
    }
    if (ferr != BLYT_RUN_FN_DONE) {
        lua_settop(g_lua_exch, 0);
        return luaL_error(L, "blyt bridge: call failed");
    }
    /* a0 = the wrapper's lua_CFunction-style return count. */
    int m = (int)blyt_session_fn_return_value(g_session);
    if (!g_lua_quit && blyt_session_check_guest_quit(g_session))
        g_lua_quit = true;
    int avail = lua_gettop(g_lua_exch);
    if (m < 0 || m > avail)
        m = 0;
    luaL_checkstack(L, m + 1, "bridged results");
    lua_xmove(g_lua_exch, L, m);
    lua_settop(g_lua_exch, 0);
    return m;
}

/* Sandboxed require() for WASM Lua carts: looks up pre-registered modules in
 * the registry _LOADED table.  Mirrors lua_blyt_require in blyt32lua.c. */
static int wasm_lua_require(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
    lua_getfield(L, -1, name);
    if (!lua_isnil(L, -1)) {
        lua_remove(L, -2);
        return 1;
    }
    lua_pop(L, 2);
    return luaL_error(L, "module '%s' not found (blyt sandbox: only native exports available)",
                      name);
}

static void wasm_visit_export_cb(const char *lua_name, uint32_t fn_guest_addr,
                                 uint32_t wrap_guest_addr, uint8_t flags, uint8_t nargs,
                                 const uint8_t arg_types[4], uint8_t ret_type, void *userdata) {
    lua_State *L = (lua_State *)userdata;
    if (flags & BLYT_LUA_EXPORT_FLAG_BRIDGED) {
        /* ADR-0130: invoke the guest wrapper through the ECALL bridge. */
        lua_pushlightuserdata(L, (void *)(uintptr_t)wrap_guest_addr);
        lua_pushcclosure(L, wasm_make_bridged_trampoline, 1);
    } else {
        lua_pushlightuserdata(L, (void *)(uintptr_t)fn_guest_addr);
        lua_pushinteger(L, nargs);
        for (int j = 0; j < 4; j++)
            lua_pushinteger(L, arg_types[j]);
        lua_pushinteger(L, ret_type);
        lua_pushcclosure(L, wasm_make_trampoline, 7);
    }

    /* Dotted lua_name (e.g. "mylib.add") → module export; plain name → global. */
    const char *dot = strchr(lua_name, '.');
    if (!dot) {
        lua_setglobal(L, lua_name);
    } else {
        /* Extract module name (before dot) and fn name (after dot). */
        char mod[32];
        int mod_len = (int)(dot - lua_name);
        if (mod_len >= (int)sizeof(mod))
            mod_len = (int)sizeof(mod) - 1;
        for (int i = 0; i < mod_len; i++)
            mod[i] = lua_name[i];
        mod[mod_len] = '\0';
        const char *fn = dot + 1;

        /* Get or create the module table in globals and _LOADED. */
        lua_getglobal(L, mod);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_pushvalue(L, -1);
            lua_setglobal(L, mod);
            luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
            lua_pushvalue(L, -2);
            lua_setfield(L, -2, mod);
            lua_pop(L, 1); /* pop _LOADED */
        }
        /* The trampoline closure is at stack index -2 (module table is -1). */
        lua_pushvalue(L, -2);
        lua_setfield(L, -2, fn);
        lua_pop(L, 2); /* pop module table + original closure */
    }
}

/* Register one Lua global closure per entry in the cart's .lua_exports section. */
static void wasm_register_lua_trampolines(lua_State *L, blyt_session_t *s) {
    blyt_session_visit_lua_exports(s, wasm_visit_export_cb, L);
}

/* If fn is a cart-native address (non-zero), register a zero-arg void trampoline
 * as the Lua global lua_name.  Used to inject lifecycle callbacks for hybrid carts
 * that define them natively rather than as Lua functions. */
static void maybe_inject_lifecycle_cb(lua_State *L, const char *lua_name, uint32_t fn) {
    if (!fn)
        return;
    lua_pushlightuserdata(L, (void *)(uintptr_t)fn);
    lua_pushinteger(L, 0); /* nargs = 0 */
    lua_pushinteger(L, WASM_LUA_TYPE_VOID); /* arg_types[0..3] */
    lua_pushinteger(L, WASM_LUA_TYPE_VOID);
    lua_pushinteger(L, WASM_LUA_TYPE_VOID);
    lua_pushinteger(L, WASM_LUA_TYPE_VOID);
    lua_pushinteger(L, WASM_LUA_TYPE_VOID); /* ret_type */
    lua_pushcclosure(L, wasm_make_trampoline, 7);
    lua_setglobal(L, lua_name);
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
    if (g_lua_exch_ref != LUA_NOREF && g_lua) {
        luaL_unref(g_lua, LUA_REGISTRYINDEX, g_lua_exch_ref);
        g_lua_exch_ref = LUA_NOREF;
    }
    g_lua_exch = NULL;
    if (g_lua) {
        lua_close(g_lua);
        g_lua = NULL;
    }
    if (g_session) {
        blyt_session_destroy(g_session);
        g_session = NULL;
    }
    if (g_lua_state_ctx) {
        blyt_state_ctx_destroy(g_lua_state_ctx);
        free(g_lua_state_ctx);
        g_lua_state_ctx = NULL;
    }
    if (g_lua_save_dir) {
        free(g_lua_save_dir);
        g_lua_save_dir = NULL;
    }
    if (g_cart) {
        blyt_cart_close(g_cart);
        g_cart = NULL;
    }
}

/* -------------------------------------------------------------------------
 * State buffer + save/load Lua API for the WASM Lua path
 *
 * Mirrors the blyt.buf.* / blyt.save_write / blyt.save_read API from
 * blyt32lua.c, but calling blyt_state_get/set/alloc_slot/free_slot directly
 * rather than via ECALL stubs (which only run inside the RV32 emulator).
 * ------------------------------------------------------------------------- */

/* Return the active state context: from a full session (hybrid carts) or
 * from the lightweight context (pure Lua carts with layouts only). */
static blyt_state_ctx_t *active_state_ctx(void) {
    if (g_session)
        return blyt_session_state_ctx(g_session);
    return g_lua_state_ctx; /* may be NULL if no layouts */
}
static const char *active_save_dir(void) {
    if (g_session)
        return blyt_session_save_dir(g_session);
    return g_lua_save_dir;
}
static const char *active_cart_name(void) {
    if (g_session)
        return blyt_session_cart_name(g_session);
    return g_lua_cart_name;
}

/* Helpers: read bits from / write bits to the active state context.
 * arg3 is a blyt_field_h (packed: high 16 = buf_id, low 16 = field_idx).
 * blyt_state_get/set take the 1-based field index, so strip the high word. */
static uint32_t buf_get_bits(lua_State *L) {
    uint32_t bits = 0;
    blyt_state_ctx_t *ctx = active_state_ctx();
    if (ctx)
        blyt_state_get(ctx, (uint32_t)luaL_checkinteger(L, 1), (int32_t)luaL_checkinteger(L, 2),
                       (uint32_t)luaL_checkinteger(L, 3) & 0xFFFF, &bits);
    return bits;
}
static void buf_set_bits(lua_State *L, uint32_t bits, uint8_t type_tag) {
    blyt_state_ctx_t *ctx = active_state_ctx();
    if (ctx)
        blyt_state_set(ctx, (uint32_t)luaL_checkinteger(L, 1), (int32_t)luaL_checkinteger(L, 2),
                       (uint32_t)luaL_checkinteger(L, 3) & 0xFFFF, bits, type_tag);
}

/* Type tags: i8=0, u8=1, i16=2, u16=3, i32=4, u32=5, f32=6, bool=7, f64=8 */
static int wasm_buf_get_f32(lua_State *L) {
    uint32_t bits = buf_get_bits(L);
    float f;
    memcpy(&f, &bits, 4);
    lua_pushnumber(L, (lua_Number)f);
    return 1;
}
static int wasm_buf_set_f32(lua_State *L) {
    float f = (float)luaL_checknumber(L, 4);
    uint32_t bits;
    memcpy(&bits, &f, 4);
    buf_set_bits(L, bits, 6);
    return 0;
}
static int wasm_buf_get_f64(lua_State *L) {
    uint64_t bits = 0;
    blyt_state_ctx_t *ctx = active_state_ctx();
    if (ctx)
        blyt_state_get64(ctx, (uint32_t)luaL_checkinteger(L, 1), (int32_t)luaL_checkinteger(L, 2),
                         (uint32_t)luaL_checkinteger(L, 3) & 0xFFFF, &bits);
    double d;
    memcpy(&d, &bits, 8);
    lua_pushnumber(L, (lua_Number)d);
    return 1;
}
static int wasm_buf_set_f64(lua_State *L) {
    double d = (double)luaL_checknumber(L, 4);
    uint64_t bits;
    memcpy(&bits, &d, 8);
    blyt_state_ctx_t *ctx = active_state_ctx();
    if (ctx)
        blyt_state_set64(ctx, (uint32_t)luaL_checkinteger(L, 1), (int32_t)luaL_checkinteger(L, 2),
                         (uint32_t)luaL_checkinteger(L, 3) & 0xFFFF, bits);
    return 0;
}
static int wasm_buf_get_i32(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)(int32_t)buf_get_bits(L));
    return 1;
}
static int wasm_buf_set_i32(lua_State *L) {
    buf_set_bits(L, (uint32_t)(int32_t)luaL_checkinteger(L, 4), 4);
    return 0;
}
static int wasm_buf_get_u32(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)buf_get_bits(L));
    return 1;
}
static int wasm_buf_set_u32(lua_State *L) {
    buf_set_bits(L, (uint32_t)luaL_checkinteger(L, 4), 5);
    return 0;
}
static int wasm_buf_get_i16(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)(int16_t)(uint16_t)buf_get_bits(L));
    return 1;
}
static int wasm_buf_set_i16(lua_State *L) {
    buf_set_bits(L, (uint32_t)(uint16_t)(int16_t)luaL_checkinteger(L, 4), 2);
    return 0;
}
static int wasm_buf_get_u16(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)(uint16_t)buf_get_bits(L));
    return 1;
}
static int wasm_buf_set_u16(lua_State *L) {
    buf_set_bits(L, (uint32_t)(uint16_t)luaL_checkinteger(L, 4), 3);
    return 0;
}
static int wasm_buf_get_i8(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)(int8_t)(uint8_t)buf_get_bits(L));
    return 1;
}
static int wasm_buf_set_i8(lua_State *L) {
    buf_set_bits(L, (uint32_t)(uint8_t)(int8_t)luaL_checkinteger(L, 4), 0);
    return 0;
}
static int wasm_buf_get_u8(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)(uint8_t)buf_get_bits(L));
    return 1;
}
static int wasm_buf_set_u8(lua_State *L) {
    buf_set_bits(L, (uint32_t)(uint8_t)luaL_checkinteger(L, 4), 1);
    return 0;
}
static int wasm_buf_get_bool(lua_State *L) {
    lua_pushboolean(L, buf_get_bits(L) ? 1 : 0);
    return 1;
}
static int wasm_buf_set_bool(lua_State *L) {
    buf_set_bits(L, lua_toboolean(L, 4) ? 1u : 0u, 7);
    return 0;
}
static int wasm_buf_alloc_slot(lua_State *L) {
    int32_t slot = -1;
    blyt_state_ctx_t *ctx = active_state_ctx();
    if (ctx)
        blyt_state_alloc_slot(ctx, (uint32_t)luaL_checkinteger(L, 1), &slot);
    lua_pushinteger(L, slot);
    return 1;
}
static int wasm_buf_free_slot(lua_State *L) {
    blyt_state_ctx_t *ctx = active_state_ctx();
    if (ctx)
        blyt_state_free_slot(ctx, (uint32_t)luaL_checkinteger(L, 1),
                             (int32_t)luaL_checkinteger(L, 2));
    return 0;
}
/* Packed entity refs (ADR-0096) — host-Lua fast-path equivalents of the
 * blyt.buf.ref* bindings in libblyt32lua. */
static int wasm_buf_ref(lua_State *L) {
    uint32_t ref = 0;
    blyt_state_ctx_t *ctx = active_state_ctx();
    if (ctx)
        ref = blyt_state_ref(ctx, (uint32_t)luaL_checkinteger(L, 1),
                             (int32_t)luaL_checkinteger(L, 2));
    lua_pushinteger(L, (lua_Integer)ref);
    return 1;
}
static int wasm_buf_ref_valid(lua_State *L) {
    int v = 0;
    blyt_state_ctx_t *ctx = active_state_ctx();
    if (ctx)
        v = blyt_state_ref_valid(ctx, (uint32_t)luaL_checkinteger(L, 1),
                                 (uint32_t)luaL_checkinteger(L, 2));
    lua_pushboolean(L, v);
    return 1;
}
static int wasm_buf_ref_slot(lua_State *L) {
    /* Pure bit math — must match blyt_buffer_ref_slot in blyt.h. */
    lua_pushinteger(L, (lua_Integer)((uint32_t)luaL_checkinteger(L, 1) & 0xFFFFu));
    return 1;
}

static int wasm_lua_save_write(lua_State *L) {
    uint32_t slot = (uint32_t)luaL_checkinteger(L, 1);
    /* Ask cart to flush transient state into buffers before persisting. */
    lua_getglobal(L, "on_save_state");
    if (lua_isfunction(L, -1)) {
        blyt_tracef(BLYT_TRACE_LIFECYCLE, "call on_save_state");
        lua_pcall(L, 0, 0, 0);
        blyt_tracef(BLYT_TRACE_LIFECYCLE, "ret on_save_state");
    } else {
        lua_pop(L, 1);
    }
    int r = -1;
    blyt_state_ctx_t *ctx = active_state_ctx();
    if (ctx)
        r = blyt_save_write(ctx, active_save_dir(), active_cart_name(), slot);
    lua_pushinteger(L, r);
    return 1;
}

static int wasm_lua_save_read(lua_State *L) {
    uint32_t slot = (uint32_t)luaL_checkinteger(L, 1);
    int r = -1;
    blyt_state_ctx_t *ctx = active_state_ctx();
    if (ctx)
        r = blyt_save_read(ctx, active_save_dir(), active_cart_name(), slot);
    lua_pushinteger(L, r);
    if (r == BLYT_RUN_OK) {
        lua_getglobal(L, "on_load_state");
        if (lua_isfunction(L, -1)) {
            blyt_tracef(BLYT_TRACE_LIFECYCLE, "call on_load_state");
            lua_newtable(L);
            lua_pushinteger(L, 0); /* reason=BLYT_LOAD_EXPLICIT */
            lua_setfield(L, -2, "reason");
            lua_pushinteger(L, 0);
            lua_setfield(L, -2, "saved_cart_version");
            lua_pcall(L, 1, 0, 0);
            blyt_tracef(BLYT_TRACE_LIFECYCLE, "ret on_load_state");
        } else {
            lua_pop(L, 1);
        }
    }
    return 1;
}

/* Build and eval a Lua chunk that creates the S proxy global for pure Lua
 * carts with state buffers.  Mirrors the register_cart_state_S() function
 * that the packer generates as native C in __blyt_lua_glue.c, but uses
 * blyt.buf.get_T/set_T instead of ECALL stubs so it runs without rv32emu. */
static void wasm_register_s_proxy(lua_State *L) {
    static const char *type_names[] = {"i8", "u8", "i16", "u16", "i32", "u32", "f32", "bool"};

    blyt_state_ctx_t *ctx = g_lua_state_ctx;
    if (!ctx || ctx->n_buffers == 0)
        return;

    /* Estimate buffer size: ~4 KB base + ~600 bytes per buffer + ~150 bytes per field */
    size_t cap = 4096 + ctx->n_buffers * 600;
    for (uint32_t bi = 0; bi < ctx->n_buffers; bi++)
        cap += ctx->buffers[bi].n_fields * 150;

    char *buf = malloc(cap);
    if (!buf)
        return;
    size_t pos = 0;

#define APPEND(s)                                                                                  \
    do {                                                                                           \
        size_t _n = strlen(s);                                                                     \
        if (pos + _n + 1 <= cap) {                                                                 \
            memcpy(buf + pos, s, _n);                                                              \
            pos += _n;                                                                             \
            buf[pos] = '\0';                                                                       \
        }                                                                                          \
    } while (0)
#define APPENDF(...)                                                                               \
    do {                                                                                           \
        int _n = snprintf(buf + pos, cap - pos, __VA_ARGS__);                                      \
        if (_n > 0 && (size_t)_n < cap - pos)                                                      \
            pos += (size_t)_n;                                                                     \
    } while (0)

    APPEND("do\nlocal _buf=blyt.buf\nS={}\n");

    /* Integer constants: S.BUFNAME = buf_id, S.BUFNAME_FIELDNAME = field_h */
    for (uint32_t bi = 0; bi < ctx->n_buffers; bi++) {
        blyt_buffer_ctx_t *bc = &ctx->buffers[bi];
        uint32_t buf_id = bc->buf_id;

        /* S.GLOBALS = 1 */
        APPENDF("S.");
        for (const char *p = bc->name; *p; p++)
            buf[pos++] = (char)toupper((unsigned char)*p);
        buf[pos] = '\0';
        APPENDF("=%u\n", buf_id);

        /* S.GLOBALS_FRAME = 0x00010001 */
        for (uint32_t fi = 0; fi < bc->n_fields; fi++) {
            uint32_t field_h = (buf_id << 16) | (fi + 1);
            APPENDF("S.");
            for (const char *p = bc->name; *p; p++)
                buf[pos++] = (char)toupper((unsigned char)*p);
            buf[pos++] = '_';
            buf[pos] = '\0';
            for (const char *p = bc->field_names[fi]; *p; p++)
                buf[pos++] = (char)toupper((unsigned char)*p);
            buf[pos] = '\0';
            APPENDF("=%u\n", field_h);
        }
    }

    /* Proxy tables: one per buffer */
    for (uint32_t bi = 0; bi < ctx->n_buffers; bi++) {
        blyt_buffer_ctx_t *bc = &ctx->buffers[bi];
        uint32_t buf_id = bc->buf_id;

        APPENDF("local _b%u_rmt={}\n", buf_id);

        /* __index: _buf.get_T(buf_id, slot, field_idx) */
        APPENDF("_b%u_rmt.__index=function(t,k)\nlocal s=rawget(t,1)\n", buf_id);
        for (uint32_t fi = 0; fi < bc->n_fields; fi++) {
            uint8_t tag = bc->field_types[fi];
            const char *tname = (tag < 8) ? type_names[tag] : "i32";
            APPENDF("%s k==\"%s\" then return _buf.get_%s(%u,s,%u)\n", fi == 0 ? "if" : "elseif",
                    bc->field_names[fi], tname, buf_id, fi + 1);
        }
        APPEND("end\nend\n");

        /* __newindex: _buf.set_T(buf_id, slot, field_idx, value) */
        APPENDF("_b%u_rmt.__newindex=function(t,k,v)\nlocal s=rawget(t,1)\n", buf_id);
        for (uint32_t fi = 0; fi < bc->n_fields; fi++) {
            uint8_t tag = bc->field_types[fi];
            const char *tname = (tag < 8) ? type_names[tag] : "i32";
            APPENDF("%s k==\"%s\" then _buf.set_%s(%u,s,%u,v)\n", fi == 0 ? "if" : "elseif",
                    bc->field_names[fi], tname, buf_id, fi + 1);
        }
        APPEND("end\nend\n");

        /* Pre-create row tables */
        APPENDF("local _b%u_rows={}\n", buf_id);
        APPENDF("for i=0,%u do local r={i};setmetatable(r,_b%u_rmt);_b%u_rows[i]=r end\n",
                bc->count > 0 ? bc->count - 1 : 0, buf_id, buf_id);

        /* Buffer proxy assigned to S.<name> */
        APPENDF("S.%s=setmetatable({},{__index=function(t,k) if k==\"count\" then return %u end "
                "return _b%u_rows[k] end})\n",
                bc->name, bc->count, buf_id);
    }

    APPEND("end\n");

#undef APPEND
#undef APPENDF

    if (luaL_loadbuffer(L, buf, pos, "@s_proxy") != LUA_OK) {
        fprintf(stderr, "[blyt] wasm_register_s_proxy load error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    } else if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        fprintf(stderr, "[blyt] wasm_register_s_proxy eval error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }

    free(buf);
}

/* Register blyt.buf.* and blyt.save_write/read into the active Lua state.
 * Must be called after the blyt and blyt32 globals are created. */
static void wasm_register_state_api(lua_State *L, blyt_session_t *s) {
    (void)s; /* inner functions use g_session directly */
    static const struct {
        const char *name;
        lua_CFunction fn;
    } buf_fns[] = {
        {"get_f32", wasm_buf_get_f32},
        {"set_f32", wasm_buf_set_f32},
        {"get_f64", wasm_buf_get_f64},
        {"set_f64", wasm_buf_set_f64},
        {"get_i32", wasm_buf_get_i32},
        {"set_i32", wasm_buf_set_i32},
        {"get_u32", wasm_buf_get_u32},
        {"set_u32", wasm_buf_set_u32},
        {"get_i16", wasm_buf_get_i16},
        {"set_i16", wasm_buf_set_i16},
        {"get_u16", wasm_buf_get_u16},
        {"set_u16", wasm_buf_set_u16},
        {"get_i8", wasm_buf_get_i8},
        {"set_i8", wasm_buf_set_i8},
        {"get_u8", wasm_buf_get_u8},
        {"set_u8", wasm_buf_set_u8},
        {"get_bool", wasm_buf_get_bool},
        {"set_bool", wasm_buf_set_bool},
        {"alloc_slot", wasm_buf_alloc_slot},
        {"free_slot", wasm_buf_free_slot},
        {"ref", wasm_buf_ref},
        {"ref_valid", wasm_buf_ref_valid},
        {"ref_slot", wasm_buf_ref_slot},
        {NULL, NULL},
    };
    lua_newtable(L); /* buf subtable */
    for (int i = 0; buf_fns[i].name; i++) {
        lua_pushcfunction(L, buf_fns[i].fn);
        lua_setfield(L, -2, buf_fns[i].name);
    }
    /* blyt.buf */
    lua_getglobal(L, "blyt");
    lua_pushvalue(L, -2);
    lua_setfield(L, -2, "buf");
    /* blyt32.buf = blyt.buf */
    lua_getglobal(L, "blyt32");
    if (lua_istable(L, -1)) {
        lua_pushvalue(L, -3);
        lua_setfield(L, -2, "buf");
    }
    lua_pop(L, 2); /* pop blyt32 (or nil) + blyt */
    lua_pop(L, 1); /* pop buf table */

    /* blyt.save_write / blyt.save_read */
    lua_getglobal(L, "blyt");
    lua_pushcfunction(L, wasm_lua_save_write);
    lua_setfield(L, -2, "save_write");
    lua_pushcfunction(L, wasm_lua_save_read);
    lua_setfield(L, -2, "save_read");
    /* blyt32.save_write = blyt.save_write, blyt32.save_read = blyt.save_read */
    lua_getglobal(L, "blyt32");
    if (lua_istable(L, -1)) {
        /* stack: [blyt@-2, blyt32@-1] */
        lua_getfield(L, -2, "save_write"); /* push blyt.save_write → [blyt, blyt32, sw] */
        lua_setfield(L, -2, "save_write"); /* blyt32.save_write = sw, pop → [blyt, blyt32] */
        lua_getfield(L, -2, "save_read"); /* push blyt.save_read → [blyt, blyt32, sr] */
        lua_setfield(L, -2, "save_read"); /* blyt32.save_read = sr, pop → [blyt, blyt32] */
    }
    lua_pop(L, 2); /* pop blyt32 (or nil) + blyt */
}

/* -------------------------------------------------------------------------
 * Reset-every-frame cycle for the Lua path
 *
 * Full VM teardown + recreate: destroys the coroutine and all Lua globals,
 * recreates a fresh VM, re-runs the cart script, calls init() from C, then
 * restores the state buffer snapshot so state persists across the reset.
 * Called after draw() completes when BLYT_RESET_EVERY_FRAME=1.
 * ------------------------------------------------------------------------- */
static void wasm_lua_reset_cycle(void) {
    /* Step 1: flush live state to buffers */
    lua_getglobal(g_lua, "on_save_state");
    if (lua_isfunction(g_lua, -1)) {
        blyt_tracef(BLYT_TRACE_LIFECYCLE, "call on_save_state");
        lua_pcall(g_lua, 0, 0, 0);
        blyt_tracef(BLYT_TRACE_LIFECYCLE, "ret on_save_state");
    } else {
        lua_pop(g_lua, 1);
    }

    /* Step 2: snapshot state buffers */
    blyt_state_snapshot_t *snap = NULL;
    blyt_state_ctx_t *sctx = active_state_ctx();
    if (sctx)
        snap = blyt_state_ctx_snapshot(sctx);

    /* Step 3: zero state buffers */
    if (sctx)
        blyt_state_ctx_zero_data(sctx);

    /* Step 4: destroy entire Lua VM (all coroutines, globals gone) */
    if (g_lua_co_ref != LUA_NOREF && g_lua) {
        luaL_unref(g_lua, LUA_REGISTRYINDEX, g_lua_co_ref);
        g_lua_co_ref = LUA_NOREF;
    }
    g_lua_co = NULL;
    if (g_lua_exch_ref != LUA_NOREF && g_lua) {
        luaL_unref(g_lua, LUA_REGISTRYINDEX, g_lua_exch_ref);
        g_lua_exch_ref = LUA_NOREF;
    }
    g_lua_exch = NULL;
    lua_close(g_lua);
    g_lua = NULL;

    /* Step 5: create fresh Lua VM */
    g_lua = luaL_newstate();
    if (!g_lua) {
        if (snap)
            blyt_state_snapshot_free(snap);
        g_lua_fatal = true;
        return;
    }

    /* Step 6: re-open stdlib subset */
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

    /* Step 7: re-register core blyt API */
    lua_newtable(g_lua);
    lua_newtable(g_lua);
    lua_pushcfunction(g_lua, lua_wasm_debug_print);
    lua_setfield(g_lua, -2, "print");
    lua_setfield(g_lua, -2, "debug");
    lua_setglobal(g_lua, "blyt32");

    lua_newtable(g_lua);
    lua_newtable(g_lua);
    lua_pushcfunction(g_lua, lua_wasm_debug_print);
    lua_setfield(g_lua, -2, "print");
    lua_setfield(g_lua, -2, "debug");
    lua_pushcfunction(g_lua, lua_wasm_quit);
    lua_setfield(g_lua, -2, "quit");
    lua_pushcfunction(g_lua, lua_wasm_should_quit);
    lua_setfield(g_lua, -2, "should_quit");
    lua_setglobal(g_lua, "blyt");
    lua_pushcfunction(g_lua, lua_wasm_quit);
    lua_setglobal(g_lua, "blyt_quit");
    lua_pushcfunction(g_lua, wasm_lua_require);
    lua_setglobal(g_lua, "require");

    /* Step 8: re-register state API */
    if (active_state_ctx())
        wasm_register_state_api(g_lua, g_session);
    wasm_register_s_proxy(g_lua);

    /* Step 9: re-run .cart.lua section */
    if (luaL_loadbuffer(g_lua, (const char *)g_lua_bytecode, g_lua_bytecode_size, "@cart") !=
            LUA_OK ||
        lua_pcall(g_lua, 0, 0, 0) != LUA_OK) {
        blyt_js_error(lua_tostring(g_lua, -1));
        lua_close(g_lua);
        g_lua = NULL;
        if (snap)
            blyt_state_snapshot_free(snap);
        g_lua_fatal = true;
        return;
    }

    /* Step 10: re-inject lifecycle trampolines. Error if both Lua and native
     * define the same callback; otherwise install native trampoline when
     * Lua hasn't defined it. */
    if (g_session) {
        static const struct {
            const char *name;
            uint32_t (*fn)(blyt_session_t *);
        } cbs[] = {
            {"init", blyt_session_cart_fn_init},
            {"on_new_state", blyt_session_cart_fn_on_new_state},
            {"update", blyt_session_cart_fn_update},
            {"draw", blyt_session_cart_fn_draw},
            {"on_quit", blyt_session_cart_fn_on_quit},
            {"cleanup", blyt_session_cart_fn_cleanup},
        };
        for (int i = 0; i < 6; i++) {
            uint32_t fn = cbs[i].fn(g_session);
            if (!fn)
                continue;
            lua_getglobal(g_lua, cbs[i].name);
            int has_lua = lua_isfunction(g_lua, -1);
            lua_pop(g_lua, 1);
            if (has_lua) {
                char buf[128];
                snprintf(buf, sizeof(buf), "lifecycle '%s' defined in both native and Lua",
                         cbs[i].name);
                blyt_js_error(buf);
                lua_close(g_lua);
                g_lua = NULL;
                if (snap)
                    blyt_state_snapshot_free(snap);
                g_lua_fatal = true;
                return;
            }
            maybe_inject_lifecycle_cb(g_lua, cbs[i].name, fn);
        }
    }

    /* Step 11: if has_lua_exports, recreate exchange thread and bridge */
    if (g_session && g_has_lua_exports) {
        g_lua_exch = lua_newthread(g_lua);
        g_lua_exch_ref = luaL_ref(g_lua, LUA_REGISTRYINDEX);
        blyt_session_lua_bridge_attach(g_session, g_lua_exch);
        wasm_register_lua_trampolines(g_lua, g_session);
    }

    /* Step 12: call init() from C (no coroutine needed for this reset call) */
    lua_getglobal(g_lua, "init");
    if (lua_isfunction(g_lua, -1)) {
        blyt_tracef(BLYT_TRACE_LIFECYCLE, "call init");
        lua_pcall(g_lua, 0, 0, 0);
        blyt_tracef(BLYT_TRACE_LIFECYCLE, "ret init");
    } else {
        lua_pop(g_lua, 1);
    }

    /* Step 13: restore state buffers from snapshot */
    if (snap) {
        blyt_state_ctx_restore_snapshot(active_state_ctx(), snap);
        blyt_state_snapshot_free(snap);
    }

    /* Step 14: notify cart that state was restored (reason=BLYT_LOAD_HOT_RELOAD=3) */
    lua_getglobal(g_lua, "on_load_state");
    if (lua_isfunction(g_lua, -1)) {
        blyt_tracef(BLYT_TRACE_LIFECYCLE, "call on_load_state");
        lua_newtable(g_lua);
        lua_pushinteger(g_lua, 3);
        lua_setfield(g_lua, -2, "reason");
        lua_pushinteger(g_lua, 0);
        lua_setfield(g_lua, -2, "saved_cart_version");
        lua_pcall(g_lua, 1, 0, 0);
        blyt_tracef(BLYT_TRACE_LIFECYCLE, "ret on_load_state");
    } else {
        lua_pop(g_lua, 1);
    }

    /* Step 15: create running coroutine for next frame */
    g_lua_co = lua_newthread(g_lua);
    g_lua_co_ref = luaL_ref(g_lua, LUA_REGISTRYINDEX);
    luaL_loadstring(g_lua_co, co_body_running);
    /* g_lua_phase stays LUA_PHASE_RUNNING — next frame calls update() directly */
}

/* -------------------------------------------------------------------------
 * wasm_lua_loop — Emscripten animation tick for Lua carts
 *
 * C phase machine: INIT phase runs init()+on_new_state() via the init
 * coroutine, then transitions to RUNNING phase which drives update()/draw()
 * per frame via the running coroutine.  The coroutine is retained so DAP
 * line hooks can yield mid-frame for breakpoints/stepping.
 * ------------------------------------------------------------------------- */
static void wasm_lua_loop(void) {
    if (!g_lua)
        return;

#ifdef BLYT_DAP
    /* Drain WebSocket queue: delivers new breakpoints, continue/step responses. */
    fc_dap_poll_messages();

    if (fc_dap_is_restart_pending()) {
        /* Restart: go back to INIT phase with a fresh init coroutine. */
        if (g_lua_co_ref != LUA_NOREF) {
            luaL_unref(g_lua, LUA_REGISTRYINDEX, g_lua_co_ref);
            g_lua_co_ref = LUA_NOREF;
        }
        g_lua_co = lua_newthread(g_lua);
        g_lua_co_ref = luaL_ref(g_lua, LUA_REGISTRYINDEX);
        luaL_loadstring(g_lua_co, co_body_init);
        g_lua_quit = false;
        g_lua_fatal = false;
        g_lua_dap_paused = false;
        g_lua_needs_start = true;
        g_lua_phase = LUA_PHASE_INIT;
        blyt_js_present(g_xrgb, BLYT_FRAME_W, BLYT_FRAME_H);
        return;
    }

    if (g_lua_needs_start) {
        if (!fc_dap_configuration_done()) {
            blyt_js_present(g_xrgb, BLYT_FRAME_W, BLYT_FRAME_H);
            return;
        }
        g_lua_needs_start = false;
        fc_consolelua_master_hook_install(g_lua_co);
#ifdef BLYT_GDB
        /* DAP breakpoints are now registered; clear the GDB startup halt so
         * native calls during init() can proceed without a GDB client
         * needing to send vCont;c first.  Real halts (breakpoint/step,
         * pending_action >= 0) are not affected. */
        if (g_session)
            blyt_session_gdb_continue_initial_halt(g_session);
#endif
        /* fall through to start execution */
    }

    if (g_lua_dap_paused) {
        if (!fc_dap_continue_pending()) {
            blyt_js_present(g_xrgb, BLYT_FRAME_W, BLYT_FRAME_H);
            return;
        }
        fc_dap_do_resume();
        g_lua_dap_paused = false;
        /* fall through to resume the coroutine */
    }
#endif

#ifdef BLYT_GDB
    /* Trampoline GDB pause: a Lua-to-native call hit a GDB breakpoint and
     * yielded the coroutine.  Wait here (returning each tick to keep the
     * event loop free) until the GDB client sends vCont;c, then fall through
     * to resume the coroutine which will call trampoline_gdb_resume_k.
     *
     * Also clears the stub's initial startup halt (pending_action < 0) so
     * that pure-DAP debug sessions — where no GDB client ever sends vCont;c
     * — are not permanently blocked.  Real GDB halts (breakpoint / step,
     * pending_action >= 0) are unaffected. */
    if (g_trampoline_gdb_paused) {
        if (g_session)
            blyt_session_gdb_continue_initial_halt(g_session);
        if (fc_gdb_stub_is_halted()) {
            blyt_js_present(g_xrgb, BLYT_FRAME_W, BLYT_FRAME_H);
            return;
        }
        g_trampoline_gdb_paused = false;
        /* GDB unhalted — fall through to resume the coroutine */
    }
#endif

    if (g_lua_fatal) {
        emscripten_cancel_main_loop();
        lua_cleanup();
        if (g_lua_error)
            emscripten_force_exit(1);
        return;
    }

    if (!g_lua_co)
        return;

    /* ---- INIT phase ---- */
    if (g_lua_phase == LUA_PHASE_INIT) {
        int nres = 0;
        int status = lua_resume(g_lua_co, g_lua, 0, &nres);
        if (status == LUA_OK) {
            /* init() + on_new_state() done — create running coroutine */
            luaL_unref(g_lua, LUA_REGISTRYINDEX, g_lua_co_ref);
            g_lua_co = lua_newthread(g_lua);
            g_lua_co_ref = luaL_ref(g_lua, LUA_REGISTRYINDEX);
            if (luaL_loadstring(g_lua_co, co_body_running) != LUA_OK) {
                blyt_js_error(lua_tostring(g_lua_co, -1));
                g_lua_error = true;
                g_lua_fatal = true;
                blyt_js_present(g_xrgb, BLYT_FRAME_W, BLYT_FRAME_H);
                return;
            }
#ifdef BLYT_DAP
            if (fc_master_hook_cfg.dap_enabled)
                fc_consolelua_master_hook_install(g_lua_co);
#endif
            g_lua_phase = LUA_PHASE_RUNNING;
        } else if (status == LUA_YIELD) {
#ifdef BLYT_DAP
            if (fc_dap_hook_yielded()) {
                g_lua_dap_paused = true;
                blyt_js_present(g_xrgb, BLYT_FRAME_W, BLYT_FRAME_H);
                return;
            }
#endif
            lua_settop(g_lua_co, 0); /* discard any yielded values */
        } else {
            /* Error in init */
            const char *msg = lua_tostring(g_lua_co, -1);
#ifdef BLYT_DAP
            if (blyt_dap_report_exception(g_lua_co, 1)) {
                g_lua_dap_paused = true;
                g_lua_fatal = true;
                blyt_js_present(g_xrgb, BLYT_FRAME_W, BLYT_FRAME_H);
                return;
            }
#endif
            blyt_js_error(msg ? msg : "Lua init error");
            g_lua_error = true;
            emscripten_cancel_main_loop();
            lua_cleanup();
            emscripten_force_exit(1);
            return;
        }
        blyt_js_present(g_xrgb, BLYT_FRAME_W, BLYT_FRAME_H);
        return;
    }

    /* ---- RUNNING phase ---- */
    /* BLYT_TRACE frame channel for the host-Lua fast path.  The open flag
     * survives DAP/GDB pause round-trips so a resumed frame does not emit a
     * second "start". */
    static bool trace_frame_open;
    static uint32_t trace_frame_no;
    if (!trace_frame_open && blyt_trace_enabled(BLYT_TRACE_FRAME)) {
        blyt_tracef(BLYT_TRACE_FRAME, "start");
        trace_frame_open = true;
    }
    g_lua_drawn = false;
    int nres = 0;
    int status = lua_resume(g_lua_co, g_lua, 0, &nres);

    if (status == LUA_YIELD) {
#ifdef BLYT_DAP
        if (fc_dap_hook_yielded()) {
            g_lua_dap_paused = true;
            blyt_js_present(g_xrgb, BLYT_FRAME_W, BLYT_FRAME_H);
            return;
        }
#endif
        lua_settop(g_lua_co, 0); /* discard yielded values — normal frame yield */
    } else if (status == LUA_OK) {
        /* on_quit/cleanup ran successfully */
        emscripten_cancel_main_loop();
        lua_cleanup();
        return;
    } else {
        /* Lua error */
        const char *msg = lua_tostring(g_lua_co, -1);
#ifdef BLYT_DAP
        if (blyt_dap_report_exception(g_lua_co, 1)) {
            g_lua_dap_paused = true;
            g_lua_fatal = true;
            blyt_js_present(g_xrgb, BLYT_FRAME_W, BLYT_FRAME_H);
            return;
        }
#endif
        blyt_js_error(msg ? msg : "Lua runtime error");
        g_lua_error = true;
        emscripten_cancel_main_loop();
        lua_cleanup();
        emscripten_force_exit(1);
        return;
    }

    if (!g_lua_drawn)
        render_testcard();

    if (trace_frame_open) {
        blyt_tracef(BLYT_TRACE_FRAME, "end");
        trace_frame_open = false;
    }
    blyt_trace_frame_mark(++trace_frame_no);

    blyt_js_present(g_xrgb, BLYT_FRAME_W, BLYT_FRAME_H);

    if (blyt_js_dump_frame0_if_headless(g_xrgb, BLYT_FRAME_W * BLYT_FRAME_H)) {
        emscripten_cancel_main_loop();
        lua_cleanup();
        return;
    }

    /* Reset-every-frame: full Lua VM teardown + recreate after each draw(). */
    if (g_reset_every_frame && g_lua)
        wasm_lua_reset_cycle();
}

/* Initialise and start a Lua cart from its embedded bytecode section. */
static int run_lua_cart(const void *bytecode, size_t bytecode_size) {
    g_lua_bytecode = bytecode;
    g_lua_bytecode_size = bytecode_size;

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

    /* Register blyt API: blyt.debug.print + blyt.quit + blyt.should_quit */
    lua_newtable(g_lua);
    lua_newtable(g_lua);
    lua_pushcfunction(g_lua, lua_wasm_debug_print);
    lua_setfield(g_lua, -2, "print");
    lua_setfield(g_lua, -2, "debug");
    lua_pushcfunction(g_lua, lua_wasm_quit);
    lua_setfield(g_lua, -2, "quit");
    lua_pushcfunction(g_lua, lua_wasm_should_quit);
    lua_setfield(g_lua, -2, "should_quit");
    lua_setglobal(g_lua, "blyt");
    lua_pushcfunction(g_lua, lua_wasm_quit);
    lua_setglobal(g_lua, "blyt_quit");

    /* Register sandboxed require() */
    lua_pushcfunction(g_lua, wasm_lua_require);
    lua_setglobal(g_lua, "require");

    /* Create rv32emu session when the cart has native code.
     * For pure Lua carts with only .cart.layouts, use a lightweight state ctx
     * to avoid allocating the full 256MB RV32 emulator. */
    {
        size_t exports_sz = 0;
        g_has_lua_exports = (blyt_cart_find_section(g_cart, ".lua_exports", &exports_sz) != NULL);
        int has_native_lifecycle = blyt_cart_has_native_lifecycle(g_cart);
        int has_layouts = blyt_cart_has_layouts(g_cart);
        if (g_has_lua_exports || has_native_lifecycle) {
            g_session = blyt_session_create(g_cart, wasm_log);
            if (!g_session) {
                blyt_js_error("hybrid session failed");
                lua_close(g_lua);
                g_lua = NULL;
                return 1;
            }
#ifdef BLYT_GDB
            {
                int p = blyt_js_gdb_port();
                if (p > 0)
                    blyt_session_gdb_listen(g_session, &p);
            }
#endif
            if (g_has_lua_exports) {
                g_lua_exch = lua_newthread(g_lua);
                g_lua_exch_ref = luaL_ref(g_lua, LUA_REGISTRYINDEX);
                blyt_session_lua_bridge_attach(g_session, g_lua_exch);
                wasm_register_lua_trampolines(g_lua, g_session);
            }
        } else if (has_layouts) {
            /* Pure Lua cart with state buffers: lightweight ctx, no emulator. */
            g_lua_state_ctx = malloc(sizeof(blyt_state_ctx_t));
            if (!g_lua_state_ctx || blyt_state_ctx_init(g_cart, g_lua_state_ctx) < 0) {
                blyt_js_error("state ctx init failed");
                free(g_lua_state_ctx);
                g_lua_state_ctx = NULL;
                lua_close(g_lua);
                g_lua = NULL;
                return 1;
            }
            const char *save_dir = getenv("BLYT_SAVE_DIR");
            if (save_dir)
                g_lua_save_dir = strdup(save_dir);
            /* The manifest id names the save subdirectory (validated at load
             * time: ≤63 bytes, so it always fits g_lua_cart_name[64]). */
            snprintf(g_lua_cart_name, sizeof(g_lua_cart_name), "%s", blyt_cart_id(g_cart));
        }
        /* Register state buffer + save/load API for any cart with state. */
        if (active_state_ctx())
            wasm_register_state_api(g_lua, g_session);
        wasm_register_s_proxy(g_lua);
    }

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

    /* Inject native lifecycle trampolines for hybrid carts — after bytecode so
     * Lua globals are already defined.  Error if both Lua and native define the
     * same callback; otherwise install native trampoline when Lua hasn't defined it. */
    if (g_session) {
        static const struct {
            const char *name;
            uint32_t (*fn)(blyt_session_t *);
        } cbs[] = {
            {"init", blyt_session_cart_fn_init},
            {"on_new_state", blyt_session_cart_fn_on_new_state},
            {"update", blyt_session_cart_fn_update},
            {"draw", blyt_session_cart_fn_draw},
            {"on_quit", blyt_session_cart_fn_on_quit},
            {"cleanup", blyt_session_cart_fn_cleanup},
        };
        for (int i = 0; i < 6; i++) {
            uint32_t fn = cbs[i].fn(g_session);
            if (!fn)
                continue;
            lua_getglobal(g_lua, cbs[i].name);
            int has_lua = lua_isfunction(g_lua, -1);
            lua_pop(g_lua, 1);
            if (has_lua) {
                char buf[128];
                snprintf(buf, sizeof(buf), "lifecycle '%s' defined in both native and Lua",
                         cbs[i].name);
                blyt_js_error(buf);
                lua_close(g_lua);
                g_lua = NULL;
                return 1;
            }
            maybe_inject_lifecycle_cb(g_lua, cbs[i].name, fn);
        }
    }

    /* Create the INIT phase coroutine.
     * Runs init() + on_new_state(); the coroutine finishes (LUA_OK) when done.
     * C transitions to RUNNING phase and creates the per-frame running coroutine.
     * The coroutine allows DAP line hooks to yield mid-init for breakpoints. */
    g_lua_co = lua_newthread(g_lua);
    if (!g_lua_co) {
        blyt_js_error("failed to create init coroutine");
        lua_close(g_lua);
        g_lua = NULL;
        return 1;
    }
    g_lua_co_ref = luaL_ref(g_lua, LUA_REGISTRYINDEX);
    g_lua_phase = LUA_PHASE_INIT;

    if (luaL_loadstring(g_lua_co, co_body_init) != LUA_OK) {
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
            g_lua_needs_start = true;
        }
    }
    if (fc_master_hook_cfg.dap_enabled && !g_lua_needs_start)
        fc_consolelua_master_hook_install(g_lua_co);
#endif

    /* Render one static test card frame before starting the loop. */
    render_testcard();

    g_lua_active = true;
    emscripten_set_main_loop(wasm_lua_loop, 0, 1);
    return 0;
}
#endif /* BLYT_LUA */

/* -------------------------------------------------------------------------
 * Main loop — called by Emscripten once per animation frame
 * ------------------------------------------------------------------------- */

static void wasm_loop(void) {
    if (!g_session)
        return;

    blyt_cart_run_err_t err = blyt_session_run_frame(g_session);

#ifdef BLYT_GDB
    /* GDB breakpoint/step pause: session polls for vCont internally each tick.
     * Re-present the last frame unchanged until the client sends continue/step.
     *
     * While paused, switch to setTimeout(0) so the loop polls at ~1ms instead
     * of waiting up to 16.67ms for the next rAF.  LLDB sends vCont;c via the
     * WebSocket onmessage handler; without this the cart wastes a full rAF
     * frame per conditional breakpoint hit regardless of how fast LLDB
     * evaluates the condition. */
    static bool s_gdb_timing_polling = false;
    if (err == BLYT_RUN_GDB_PAUSED) {
        if (!s_gdb_timing_polling) {
            emscripten_set_main_loop_timing(EM_TIMING_SETTIMEOUT, 0);
            s_gdb_timing_polling = true;
        }
        blyt_js_present(g_xrgb, BLYT_FRAME_W, BLYT_FRAME_H);
        return;
    }
    if (s_gdb_timing_polling) {
        emscripten_set_main_loop_timing(EM_TIMING_RAF, 0);
        s_gdb_timing_polling = false;
    }
#endif

    blyt_session_expand_frame(g_session, g_xrgb);

    if (err == BLYT_RUN_FRAME_DONE && g_reset_every_frame)
        blyt_reset_every_frame_cycle(g_session);

    bool done = (err != BLYT_RUN_FRAME_DONE);

    int headless_dump = blyt_js_dump_frame0_if_headless(g_xrgb, BLYT_FRAME_W * BLYT_FRAME_H);

    if (headless_dump) {
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
    blyt_register_lib("libblyt32lua.so", blyt32lua_so, blyt32lua_so_len);
#endif

    g_reset_every_frame = (getenv("BLYT_RESET_EVERY_FRAME") != NULL);

    blyt_cart_err_t cerr = blyt_cart_open("/cart.blyt", &g_cart);
    if (cerr != BLYT_CART_OK) {
        blyt_js_error(blyt_cart_err_str(cerr));
        return 1;
    }

    fprintf(stderr, "Blyt %s - %s (%s %s)\n", blyt_runtime_version(), blyt_cart_title(g_cart),
            blyt_cart_id(g_cart), blyt_cart_version(g_cart));

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
