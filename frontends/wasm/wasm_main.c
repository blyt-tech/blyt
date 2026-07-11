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

#include "blyt_frame_hash.h" /* runtime/shared: cross-leg framebuffer hash (#188) */
#include "blyt_phase.h" /* runtime/shared: lifecycle phase (draw()-only, #205) */
#include "blyt_raster.h" /* runtime/shared: integer rasterizer core (#188) */
#include "blyt_runtime.h"
#include "blyt_trace.h"

#ifdef BLYT_LUA
#include "blyt_arena.h" /* runtime/shared: single-sourced cart-heap arena (#158) */
#include "blyt_handle.h" /* runtime/shared: console-wide resource-constant encoding (ADR-0134) */
#include "blyt_hostlua_heap.h" /* runtime/shared: host-Lua stack-exclusion flag (#231) */
#include "blyt_mem_budget.h" /* runtime/shared: unified 16 MB budget (ADR-0008 #158) */
#include "blyt_palettes.h" /* runtime/shared: built-in palette resolver (#201) */
#include "blyt_resource_codec.h" /* runtime/shared: BLYT_RES_ALGO_NONE (#157) */
#include "resource.h"
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
static bool g_evict_every_frame = false; /* BLYT_RESOURCE_EVICT_EVERY_FRAME=1 (#137) */

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
/* Host-side resource table for pure-Lua carts (g_session == NULL): there is no
 * session run ctx to carry ctx.resources, so the blyt.resource.* host binding
 * reads this instead (#93/#120).  Hybrid carts use the session's table. */
static blyt_resource_table_t g_lua_resources;
static bool g_lua_resources_loaded = false;

/* ── Host-Lua fast-path cart heap: the unified 16 MB budget (ADR-0008 #158) ────
 * The pure-Lua fast path runs the Lua VM natively as wasm32 (S proxy + state
 * buffers) instead of through rv32emu. For determinism the VM's heap must be
 * accounted byte-identically to the rv32 path: a Lua cart has to hit the 16 MB
 * cap at the same logical point on every leg. So the VM allocates through the
 * SAME runtime/shared arena the emulated/native libblytc runs — one arena, one
 * accounting block — rather than the default Lua allocator. This is structural
 * parity (the identical allocator implementation), not a per-allocator match;
 * it holds because wasm32 shares rv32's numeric model (BLYT_LUA_I32_F64) and
 * 32-bit pointer width, so every Lua object is the same size on both. The
 * resource cache stays on the host (emscripten) allocator — its bytes never
 * enter guest_heap_used; only loaded/pinned footprint gates the budget. */
_Static_assert(sizeof(void *) == 4, "host-Lua fast path must be wasm32 (32-bit ptr) "
                                    "for Lua object sizes to match rv32 (#158)");
_Static_assert(sizeof(lua_Integer) == 4 && sizeof(lua_Number) == 8,
               "host-Lua fast path must use the i32/f64 numeric model "
               "(BLYT_LUA_I32_F64) so guest_heap_used matches rv32 (#158)");

static blyt_mem_accounting_t g_lua_mem_acct; /* {guest_heap_used, non_evictable_footprint} */
static blyt_arena_t g_lua_arena;

/* The arena over a one-time 16 MB region from the host heap (the physical pool
 * the logical budget spans). Lazily obtained on first allocation. */
static blyt_arena_t *lua_cart_arena(void) {
    if (!g_lua_arena.base) {
        void *region = malloc(BLYT_MEM_BUDGET_BYTES);
        if (!region)
            return NULL;
        g_lua_arena.base = region;
        g_lua_arena.size = BLYT_MEM_BUDGET_BYTES;
        g_lua_arena.acct = &g_lua_mem_acct;
    }
    return &g_lua_arena;
}

/* lua_Alloc backed by the shared arena. Lua's contract: nsize==0 frees and
 * returns NULL; otherwise (re)allocates to nsize (ptr==NULL ⇒ fresh alloc,
 * osize is then a type tag, ignored). Shrinks never fail (the block already
 * fits), so a GC-driven realloc-down can't error; a grow past the 16 MB budget
 * returns NULL and Lua handles the allocation failure (nil/error). */
static void *wasm_lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud;
    (void)osize;
    blyt_arena_t *a = lua_cart_arena();
    if (!a)
        return NULL;
    if (nsize == 0) {
        blyt_arena_free(a, ptr); /* reads the no-acct marker from the block */
        return NULL;
    }
    /* VM execution scratch (a thread's data stack / CallInfo, #231) is excluded
     * from guest_heap_used + the budget so the cart-attributable heap does not
     * depend on this leg driving the cart through a coroutine (co_body) while the
     * native leg calls lifecycle fns from C.  Consumed atomically, identical to
     * the native runner (cart_run_hostlua.c). */
    int noacct = blyt_hostlua_heap_stack_pending;
    blyt_hostlua_heap_stack_pending = 0;
    return noacct ? blyt_arena_realloc_noacct(a, ptr, nsize) : blyt_arena_realloc(a, ptr, nsize);
}

/* Reset the cart heap to a fresh-load-identical state (VM recreate on reload):
 * empties the arena and zeroes the whole accounting block (guest_heap_used AND
 * the stale non_evictable_footprint from the previous cart, whose resource table
 * is cleared on reload) so a reloaded Lua cart's allocations are bit-identical to
 * a first load (mirrors the native/host hot-swap, which zeroes the block). Keeps
 * the 16 MB region mapped. No-op before the region exists. */
static void wasm_lua_arena_reset(void) {
    if (g_lua_arena.base)
        blyt_arena_reset(&g_lua_arena); /* zeroes guest_heap_used + empties arena */
    g_lua_mem_acct.non_evictable_footprint = 0;
    g_lua_mem_acct.guest_heap_baseline = 0; /* re-captured after the rebuild (#231) */
}

/* Recompute the non-evictable footprint from the active resource table and
 * publish it into the cart-heap accounting block, then bound the resident
 * evictable cache to the room it leaves — the host-Lua mirror of the host
 * mem_acct_publish_footprint (#158). Call after any load/release. */
static void wasm_lua_publish_footprint(blyt_resource_table_t *t) {
    if (!t)
        return;
    uint32_t footprint = blyt_resource_table_footprint(t);
    g_lua_mem_acct.non_evictable_footprint = footprint;
    blyt_resource_table_evict_to_fit(
        t, blyt_mem_cache_room(blyt_mem_cart_heap(&g_lua_mem_acct), footprint));
}

/* Would newly loading/pinning `e` (adding e->len to the footprint when it is so
 * far evictable) still fit the unified budget? Mirrors mem_acct_reference_fits. */
static int wasm_lua_reference_fits(blyt_resource_table_t *t, const blyt_resource_entry_t *e,
                                   int was_evictable) {
    uint32_t incoming = was_evictable ? (uint32_t)e->len : 0u;
    return blyt_mem_alloc_fits(blyt_mem_cart_heap(&g_lua_mem_acct),
                               blyt_resource_table_footprint(t), incoming);
}
#ifdef BLYT_DAP
static bool g_lua_dap_paused = false; /* hook yielded, waiting for DAP */
static bool g_lua_needs_start = false; /* waiting for configurationDone */
#endif
#ifdef BLYT_GDB
/* Set when a Lua-to-native trampoline hits a GDB breakpoint and yields the
 * coroutine.  wasm_lua_loop waits here (returning each tick) until the GDB
 * client sends vCont;c, then clears the flag and resumes the coroutine. */
static bool g_trampoline_gdb_paused = false;

/* Deferred debug-reload state machine (issue #170).  A hot reload during a live
 * lldb-dap session cannot re-arm breakpoints or run the reloaded init()
 * synchronously inside the dev-control ccall: the WASM gdb relay only delivers
 * packets when the event loop runs, which is blocked for the whole ccall.  So a
 * debug reload swaps the cart and BEGINS the async two-phase solib re-arm inside
 * the ccall, then hands off to the main loop — REARM pumps the solib swap each
 * tick (letting lldb re-read the new DWARF and rebind breakpoints), then
 * RUN_INIT runs the reloaded init() under the loop (where an init breakpoint can
 * round-trip) and restores the state snapshot once init completes. */
typedef enum { RELOAD_NONE = 0, RELOAD_REARM, RELOAD_INIT } reload_phase_t;
static reload_phase_t g_reload_phase = RELOAD_NONE;
static blyt_state_snapshot_t *g_reload_snap = NULL;
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

#ifdef BLYT_GDB
/* Coroutine body for a debug hot reload (issue #170): run ONLY init() under the
 * loop's INIT phase (so a Lua init() breakpoint fires under the master hook and
 * a Lua→native trampoline can yield for a native breakpoint).  No on_new_state()
 * — the reload preserves state: the loop runs the restore + on_load_state tail
 * once init() completes, not a fresh-boot reset. */
static const char co_body_reload_init[] = "init()";
#endif

/* Coroutine body: per-frame update/draw loop with frame-boundary yield.  The
 * __blyt_phase_* brackets mirror the emulated blyt_main's BLYT_ECALL_PHASE
 * signal so a hybrid cart's native half stays draw()-only (#205); they no-op for
 * a session-less pure-Lua cart. */
static const char co_body_running[] = "while not blyt.should_quit() do "
                                      "  __blyt_phase_update() "
                                      "  update() "
                                      "  __blyt_phase_draw() "
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

/* Read the host-side path of the debug cart ELF, injected by `blyt debug` into
 * shell.html (issue #144).  lldb-dap opens this path to read the cart's DWARF;
 * without it the svr4 list reports the in-memory "/cart.blyt" which the host
 * lldb cannot open, so cart breakpoints never bind.  Returns a malloc'd C string
 * (caller frees) or 0 when not set. */
EM_JS(char *, blyt_js_cart_path, (void), {
    var p =
        (typeof globalThis !== 'undefined' && globalThis.__blyt_cart_path) ||
        (typeof window !== 'undefined' && window.blyt_cart_path) ||
        0;
    if (!p)
        return 0;
    var len = lengthBytesUTF8(p) + 1;
    var ptr = _malloc(len);
    stringToUTF8(p, ptr, len);
    return ptr;
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

/* Host-Lua lifecycle phase mirror (#205).  The emulated path's blyt_main signals
 * the phase via BLYT_ECALL_PHASE; the host-Lua fast path drives update/draw
 * itself, so the running coroutine brackets each callback with these to keep
 * surface access draw()-only.  Only a hybrid cart's native half is affected — it
 * reaches the phase gate through the gfx ECALL handlers — since the host-Lua gfx
 * bindings rasterize directly and are not gated (that is slice-6 Lua-surface
 * work).  A session-less pure-Lua cart no-ops (blyt_session_set_phase(NULL)). */
static int lua_wasm_phase_update(lua_State *L) {
    (void)L;
    blyt_session_set_phase(g_session, BLYT_PHASE_UPDATE);
    return 0;
}
static int lua_wasm_phase_draw(lua_State *L) {
    (void)L;
    blyt_session_set_phase(g_session, BLYT_PHASE_DRAW);
    return 0;
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

/* Set true by the blyt32.gfx.* drawing functions; cleared each frame before
 * draw().  When set, the frame loop presents g_lua_pixels instead of the test
 * card. */
static bool g_lua_drawn = false;

/* The host-Lua fast path's paletted back buffer (#188 / Spike X): blyt32.gfx.*
 * rasterize into it directly (the host-Lua equivalent of the emulated path's
 * session->pixels[]), using the SAME runtime/shared integer rasterizer, so the
 * fast-path pixels are bit-identical to every emulated leg. */
static uint8_t g_lua_pixels[BLYT_FRAME_W * BLYT_FRAME_H];
static uint32_t g_lua_gfx_palette[256];
static bool g_lua_gfx_palette_init = false;

/* Seed the session-less host-Lua fast path's palette to the runtime default
 * (aurora), matching the pre-init default the emulated legs load into
 * session->palette (#201/#204).  A pure-Lua cart that never calls palette_set
 * then renders (and its test card remaps) against the same default palette on
 * wasm as on blytplay/libretro. */
static void lua_gfx_palette_ensure_default(void) {
    if (g_lua_gfx_palette_init)
        return;
    const uint32_t *pal =
        blyt_builtin_palette(BLYT_RESOURCE_ENCODE(BLYT_PAL_ID_AURORA, BLYT_RESOURCE_PROV_RUNTIME));
    for (int i = 0; i < 256; i++)
        g_lua_gfx_palette[i] = pal[i];
    g_lua_gfx_palette_init = true;
}

/* Expand a paletted frame to g_xrgb for presentation and, when BLYT_FRAME_HASH
 * is set, emit the cross-leg "[blyt:fbhash] <hex>" line — the host-Lua fast
 * path's equivalent of the host runtime's frame_done emit (cart_run.c), over the
 * same paletted bytes so every leg's hash matches (#188). */
static void lua_present_paletted(const uint8_t *pixels, const uint32_t *palette) {
    for (int i = 0; i < BLYT_FRAME_W * BLYT_FRAME_H; i++)
        g_xrgb[i] = palette[pixels[i]];
    static int s_hash_on = -1;
    if (s_hash_on < 0)
        s_hash_on = getenv("BLYT_FRAME_HASH") != NULL ? 1 : 0;
    if (s_hash_on) {
        uint64_t h = blyt_frame_hash(pixels, (size_t)BLYT_FRAME_W * (size_t)BLYT_FRAME_H);
        char buf[64];
        snprintf(buf, sizeof(buf), "[blyt:fbhash] %016llx", (unsigned long long)h);
        blyt_js_log(buf);
        /* Palette-sensitive oracle (#199/#204): mirror the host runtime's
         * palette-bytes hash so the host-Lua fast path proves colour parity too,
         * not just index parity. */
        uint64_t ph = blyt_frame_hash((const uint8_t *)palette, 256 * sizeof(uint32_t));
        snprintf(buf, sizeof(buf), "[blyt:palhash] %016llx", (unsigned long long)ph);
        blyt_js_log(buf);
    }
}

/* Render testcard into g_xrgb — used when draw() produces no gfx output.  The
 * test card is palette-agnostic (#204): it remaps its reference colours to the
 * nearest index in the *active* palette (session palette for a hybrid cart,
 * else the host-Lua fast path's palette — see lua_gfx_palette) and presents
 * through that same palette, so its colours track the cart's declared palette
 * exactly as on every emulated leg. */
static uint32_t *lua_gfx_palette(void);
static void render_testcard(void) {
    static uint8_t s_pixels[BLYT_FRAME_W * BLYT_FRAME_H];
    static uint32_t s_frame = 0;
    const uint32_t *pal = lua_gfx_palette();
    blyt_testcard_draw(s_frame++, pal, s_pixels);
    lua_present_paletted(s_pixels, pal);
}

/* The paletted framebuffer the host-Lua gfx bindings rasterize into.  For a
 * HYBRID cart a session is live (its native half runs in rv32emu per ADR-0130),
 * and that half draws via the gfx ECALL handlers into the session's canonical
 * framebuffer; the host-Lua half must draw into the SAME buffer or its output
 * diverges from the native half and is dropped on wasm (#193).  Only a genuinely
 * session-less pure-Lua cart uses the standalone g_lua_pixels.  (get_pixels is
 * declared const for read-mostly frontends; the host-Lua path legitimately
 * rasterizes into it, the same buffer the ECALL handlers mutate via the run
 * context.) */
static uint8_t *lua_gfx_fb(void) {
    if (g_session)
        return (uint8_t *)blyt_session_get_pixels(g_session);
    return g_lua_pixels;
}

/* The active 256-entry palette the host-Lua gfx bindings write into (#201,
 * fixing #199): the SAME "session when present, else standalone" routing as
 * lua_gfx_fb, so a hybrid cart's Lua-half palette_set() call lands in the
 * session's canonical palette (shared with the native half via cart_run.c's
 * ECALL handler) instead of a parallel buffer that silently drops on wasm. */
static uint32_t *lua_gfx_palette(void) {
    if (g_session)
        return (uint32_t *)blyt_session_get_palette(g_session);
    lua_gfx_palette_ensure_default();
    return g_lua_gfx_palette;
}

/* blyt32.gfx.* host-Lua bindings (#188).  Each rasterizes into the active
 * framebuffer (see lua_gfx_fb) via the shared integer core and flags
 * g_lua_drawn; the frame loop then presents + hashes it.  Mirror of the guest
 * binding in blyt32lua.c and the host ECALL handlers in cart_run.c — same
 * primitives, same back-buffer geometry. */
static int lua_wasm_gfx_clear(lua_State *L) {
    blyt_raster_clear(lua_gfx_fb(), BLYT_FRAME_W, BLYT_FRAME_W, BLYT_FRAME_H,
                      (uint8_t)luaL_checkinteger(L, 1));
    g_lua_drawn = true;
    return 0;
}
static int lua_wasm_gfx_pixel(lua_State *L) {
    blyt_raster_pixel(lua_gfx_fb(), BLYT_FRAME_W, BLYT_FRAME_W, BLYT_FRAME_H,
                      (int)luaL_checkinteger(L, 1), (int)luaL_checkinteger(L, 2),
                      (uint8_t)luaL_checkinteger(L, 3));
    g_lua_drawn = true;
    return 0;
}
static int lua_wasm_gfx_rect_fill(lua_State *L) {
    blyt_raster_rect_fill(lua_gfx_fb(), BLYT_FRAME_W, BLYT_FRAME_W, BLYT_FRAME_H,
                          (int)luaL_checkinteger(L, 1), (int)luaL_checkinteger(L, 2),
                          (int)luaL_checkinteger(L, 3), (int)luaL_checkinteger(L, 4),
                          (uint8_t)luaL_checkinteger(L, 5));
    g_lua_drawn = true;
    return 0;
}
static int lua_wasm_gfx_line(lua_State *L) {
    blyt_raster_line(lua_gfx_fb(), BLYT_FRAME_W, BLYT_FRAME_W, BLYT_FRAME_H,
                     (int)luaL_checkinteger(L, 1), (int)luaL_checkinteger(L, 2),
                     (int)luaL_checkinteger(L, 3), (int)luaL_checkinteger(L, 4),
                     (uint8_t)luaL_checkinteger(L, 5));
    g_lua_drawn = true;
    return 0;
}

/* Palette load (#201/#214).  Defined with the resource-constant machinery below
 * (it resolves CART palette assets against the active resource table and
 * recognises the typed palette constant R.<NAME>); forward-declared here for the
 * gfx method table. */
static int lua_wasm_gfx_palette_set(lua_State *L);

/* Host-Lua surface pool (blyt32.surface.*, #205).  A pure-Lua cart on the wasm
 * fast path has no rv32 session, so its surfaces live here — the host-Lua mirror
 * of the native pool (blyt32.c) and the host registry (cart_run.c).  Slot 0 is
 * the screen: its pixels resolve to lua_gfx_fb() on each access (the session
 * buffer for a hybrid, g_lua_pixels standalone).  Off-screen slots hold malloc'd
 * buffers, freed at the frame boundary (draw-scoped).  Lua is tier-1 only — no
 * acquire/release.  Drawing is ungated here (the emulated Lua legs enforce
 * draw()-only through the ECALL gate; the host-Lua per-callback enforcement is
 * slice-6+ work). */
#define LUA_SURFACE_MAX 64
typedef struct {
    uint8_t *pixels;
    int32_t w, h;
    uint16_t gen;
    bool in_use;
    bool is_screen;
    bool locked; /* exclusive tier-2 lock held (#207) — set by Stage 2's acquire */
} lua_surface_t;
static lua_surface_t g_lua_surf[LUA_SURFACE_MAX];
static bool g_lua_surf_init;

static void lua_surf_ensure_init(void) {
    if (g_lua_surf_init)
        return;
    g_lua_surf[0].in_use = true;
    g_lua_surf[0].is_screen = true;
    g_lua_surf[0].w = BLYT_FRAME_W;
    g_lua_surf[0].h = BLYT_FRAME_H;
    g_lua_surf_init = true;
}

static lua_surface_t *lua_surf_resolve(uint32_t h) {
    lua_surf_ensure_init();
    if (!blyt_handle_is_surface(h))
        return NULL;
    uint32_t idx = blyt_dyn_decode_index(h);
    if (idx >= LUA_SURFACE_MAX)
        return NULL;
    lua_surface_t *s = &g_lua_surf[idx];
    if (!s->in_use || (uint16_t)blyt_dyn_decode_gen(h) != s->gen)
        return NULL;
    if (s->is_screen)
        s->pixels = lua_gfx_fb(); /* dynamic: session buffer vs standalone */
    return s;
}

/* Reject handle-path access to a *locked* surface (#207): while a tier-2 lock is
 * held the lock owns the surface, so every tier-1 op / blit / destroy is a no-op
 * — the host-Lua mirror of the host (cart_run.c) and native (blyt32.c) lock
 * gates, keeping the exclusive-lock invariant uniform across all three surface
 * registries.  The host-Lua pool is tier-1 only today (no acquire — #205), so
 * `locked` is always false here; wiring the reject now means #195 Stage 2's Lua
 * lock inherits the invariant for free. */
static lua_surface_t *lua_surf_resolve_drawable(uint32_t h) {
    lua_surface_t *s = lua_surf_resolve(h);
    return (s && s->locked) ? NULL : s;
}

/* Unified tier-1 drawable target for the Lua surface bindings (#210).  A hybrid
 * cart (g_session != NULL) runs its native half in the rv32 session; resolving
 * against the session's ONE surface registry — via the host API, which
 * materializes the canonical buffer as a direct pointer (same address space, no
 * ECALL/copy) — keeps its Lua-created surfaces and handles coherent with that
 * native half, exactly as every other (single-registry) leg already is.  A
 * pure-Lua cart (g_session == NULL) has no session, so it resolves against the
 * local g_lua_surf pool.  Both apply the #207 locked-surface reject.  Returns
 * false (leaving *t untouched) when the handle is unresolvable or locked. */
typedef struct {
    uint8_t *pixels;
    int32_t w, h;
    bool is_screen;
} lua_draw_target_t;

static bool lua_resolve_target(uint32_t h, lua_draw_target_t *t) {
    if (g_session) {
        t->pixels = blyt_session_surface_drawable(g_session, h, &t->w, &t->h, &t->is_screen);
        return t->pixels != NULL;
    }
    lua_surface_t *s = lua_surf_resolve_drawable(h);
    if (!s)
        return false;
    t->pixels = s->pixels;
    t->w = s->w;
    t->h = s->h;
    t->is_screen = s->is_screen;
    return true;
}

/* Reap draw-scoped off-screen surfaces at the frame boundary (#205), and
 * force-release any tier-2 lock the cart forgot (#208) — including the screen
 * (slot 0), which is never freed but must not carry a lock into the next frame.
 * The lock userdata's epoch guard makes any stale reuse a defined rejection. */
static void lua_surf_reap(void) {
    lua_surf_ensure_init();
    g_lua_surf[0].locked = false; /* force-release a held screen lock */
    for (uint32_t i = 1; i < LUA_SURFACE_MAX; i++) {
        if (g_lua_surf[i].in_use) {
            free(g_lua_surf[i].pixels);
            g_lua_surf[i].pixels = NULL;
            g_lua_surf[i].in_use = false;
            g_lua_surf[i].locked = false;
            g_lua_surf[i].gen++;
        }
    }
}

static int lua_wasm_surface_create(lua_State *L) {
    int32_t w = (int32_t)luaL_checkinteger(L, 1);
    int32_t h = (int32_t)luaL_checkinteger(L, 2);
    if (g_session) { /* hybrid: create in the session's unified registry (#210) */
        lua_pushinteger(L, (lua_Integer)blyt_session_surface_create(g_session, w, h));
        return 1;
    }
    lua_surf_ensure_init();
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) {
        lua_pushinteger(L, (lua_Integer)BLYT_HANDLE_NONE);
        return 1;
    }
    uint32_t idx = 0;
    for (uint32_t i = 1; i < LUA_SURFACE_MAX; i++) {
        if (!g_lua_surf[i].in_use) {
            idx = i;
            break;
        }
    }
    uint8_t *buf = idx ? (uint8_t *)malloc((size_t)w * (size_t)h) : NULL;
    if (!buf) {
        lua_pushinteger(L, (lua_Integer)BLYT_HANDLE_NONE);
        return 1;
    }
    blyt_raster_clear(buf, w, w, h, 0); /* blank = palette index 0 */
    g_lua_surf[idx].pixels = buf;
    g_lua_surf[idx].w = w;
    g_lua_surf[idx].h = h;
    g_lua_surf[idx].in_use = true;
    g_lua_surf[idx].is_screen = false;
    lua_pushinteger(L, (lua_Integer)blyt_surface_encode(g_lua_surf[idx].gen, idx));
    return 1;
}
static int lua_wasm_surface_destroy(lua_State *L) {
    uint32_t h = (uint32_t)luaL_checkinteger(L, 1);
    if (g_session) { /* hybrid: destroy in the session's unified registry (#210) */
        blyt_session_surface_destroy(g_session, h);
        return 0;
    }
    lua_surface_t *s = lua_surf_resolve_drawable(h); /* #207 */
    if (s && !s->is_screen) {
        free(s->pixels);
        s->pixels = NULL;
        s->in_use = false;
        s->gen++;
    }
    return 0;
}
static int lua_wasm_surface_clear(lua_State *L) {
    lua_draw_target_t t;
    if (lua_resolve_target((uint32_t)luaL_checkinteger(L, 1), &t)) { /* #210 / #207 */
        blyt_raster_clear(t.pixels, t.w, t.w, t.h, (uint8_t)luaL_checkinteger(L, 2));
        if (t.is_screen)
            g_lua_drawn = true;
    }
    return 0;
}
static int lua_wasm_surface_pixel(lua_State *L) {
    lua_draw_target_t t;
    if (lua_resolve_target((uint32_t)luaL_checkinteger(L, 1), &t)) { /* #210 / #207 */
        blyt_raster_pixel(t.pixels, t.w, t.w, t.h, (int)luaL_checkinteger(L, 2),
                          (int)luaL_checkinteger(L, 3), (uint8_t)luaL_checkinteger(L, 4));
        if (t.is_screen)
            g_lua_drawn = true;
    }
    return 0;
}
static int lua_wasm_surface_rect_fill(lua_State *L) {
    lua_draw_target_t t;
    if (lua_resolve_target((uint32_t)luaL_checkinteger(L, 1), &t)) { /* #210 / #207 */
        blyt_raster_rect_fill(t.pixels, t.w, t.w, t.h, (int)luaL_checkinteger(L, 2),
                              (int)luaL_checkinteger(L, 3), (int)luaL_checkinteger(L, 4),
                              (int)luaL_checkinteger(L, 5), (uint8_t)luaL_checkinteger(L, 6));
        if (t.is_screen)
            g_lua_drawn = true;
    }
    return 0;
}
static int lua_wasm_surface_line(lua_State *L) {
    lua_draw_target_t t;
    if (lua_resolve_target((uint32_t)luaL_checkinteger(L, 1), &t)) { /* #210 / #207 */
        blyt_raster_line(t.pixels, t.w, t.w, t.h, (int)luaL_checkinteger(L, 2),
                         (int)luaL_checkinteger(L, 3), (int)luaL_checkinteger(L, 4),
                         (int)luaL_checkinteger(L, 5), (uint8_t)luaL_checkinteger(L, 6));
        if (t.is_screen)
            g_lua_drawn = true;
    }
    return 0;
}
static int lua_wasm_surface_blit(lua_State *L) {
    /* Reject if either endpoint is unresolvable or locked — a blit as dst OR
     * src (#210 unified resolve / #207 locked reject). */
    lua_draw_target_t d, s;
    if (lua_resolve_target((uint32_t)luaL_checkinteger(L, 1), &d) &&
        lua_resolve_target((uint32_t)luaL_checkinteger(L, 2), &s)) {
        blyt_raster_blit(d.pixels, d.w, d.w, d.h, s.pixels, s.w, s.w, s.h,
                         (int)luaL_checkinteger(L, 3), (int)luaL_checkinteger(L, 4));
        if (d.is_screen)
            g_lua_drawn = true;
    }
    return 0;
}

/* --- Tier-2 per-pixel surface lock on the host-Lua fast path (#208 Stage 2) ---
 *
 * The host-Lua mirror of the guest binding in blyt32lua.c.  blyt32.surface.
 * acquire(h) materializes a DIRECT pointer to the canonical buffer — for a
 * hybrid, the session's unified registry (blyt_session_surface_acquire, #210),
 * which also marks the slot locked so the native half's tier-1 ops on it are
 * rejected (#207); for a pure-Lua cart, the g_lua_surf pool.  Per-pixel get/set
 * then touch host memory with no crossing.  The per-frame epoch (g_lua_lock_
 * epoch, bumped in blyt_wasm_frame) is the staleness guard. */
#define BLYT_WASM_LOCK_MT "blyt.surface.lock"

static uint32_t g_lua_lock_epoch = 0;
/* Chooses hard-error vs defined no-op on a bad get/set (mirrors the guest
 * runtime-flags block).  The determinism-bearing behaviour is the no-op path,
 * which is uniform across every leg; the debug hard-error is a dev aid surfaced
 * on the emulated legs (blyt32.c reads its runtime-flags block).  Left false on
 * the fast path for now — a debug pure-Lua cart's OOB access no-ops here instead
 * of erroring; wiring the cart's debug flag through to host-Lua is a follow-up. */
static bool g_lua_cart_is_debug = false;

typedef struct {
    uint8_t *pixels;
    int32_t w, h;
    uint32_t handle; /* surface handle (pure-Lua release re-resolve) */
    uint32_t token; /* session release token (hybrid); BLYT_HANDLE_NONE pure-Lua */
    uint32_t epoch; /* g_lua_lock_epoch captured at acquire */
    bool released;
    bool is_screen; /* writes flip g_lua_drawn (displace the boot testcard) */
} lua_wasm_lock_t;

static int lua_wasm_surface_acquire(lua_State *L) {
    uint32_t h = (uint32_t)luaL_checkinteger(L, 1);
    uint8_t *pixels = NULL;
    int32_t w = 0, hh = 0;
    uint32_t token = BLYT_HANDLE_NONE;
    if (g_session) { /* hybrid: the session's unified registry (#210/#207) */
        pixels = blyt_session_surface_acquire(g_session, h, &w, &hh, &token);
    } else { /* pure-Lua: the local pool */
        lua_surface_t *s = lua_surf_resolve(h);
        if (s && !s->locked) {
            s->locked = true;
            pixels = s->pixels;
            w = s->w;
            hh = s->h;
        }
    }
    if (!pixels) {
        lua_pushnil(L);
        return 1;
    }
    lua_wasm_lock_t *u = (lua_wasm_lock_t *)lua_newuserdatauv(L, sizeof(*u), 0);
    u->pixels = pixels;
    u->w = w;
    u->h = hh;
    u->handle = h;
    u->token = token;
    u->epoch = g_lua_lock_epoch;
    u->released = false;
    u->is_screen = (h == (uint32_t)BLYT_SCREEN);
    luaL_setmetatable(L, BLYT_WASM_LOCK_MT);
    return 1;
}

static lua_wasm_lock_t *lua_wasm_lock_live(lua_State *L, int idx) {
    lua_wasm_lock_t *u = (lua_wasm_lock_t *)luaL_checkudata(L, idx, BLYT_WASM_LOCK_MT);
    if (u->released || u->epoch != g_lua_lock_epoch)
        return NULL;
    return u;
}

static int lua_wasm_lock_get(lua_State *L) {
    lua_wasm_lock_t *u = lua_wasm_lock_live(L, 1);
    int32_t x = (int32_t)luaL_checkinteger(L, 2);
    int32_t y = (int32_t)luaL_checkinteger(L, 3);
    if (!u || x < 0 || x >= u->w || y < 0 || y >= u->h) {
        if (g_lua_cart_is_debug)
            return luaL_error(L, "lk:get out of bounds or on a released lock");
        lua_pushinteger(L, 0);
        return 1;
    }
    lua_pushinteger(L, (lua_Integer)u->pixels[(uint32_t)y * (uint32_t)u->w + (uint32_t)x]);
    return 1;
}

static int lua_wasm_lock_set(lua_State *L) {
    lua_wasm_lock_t *u = lua_wasm_lock_live(L, 1);
    int32_t x = (int32_t)luaL_checkinteger(L, 2);
    int32_t y = (int32_t)luaL_checkinteger(L, 3);
    uint8_t c = (uint8_t)luaL_checkinteger(L, 4);
    if (!u || x < 0 || x >= u->w || y < 0 || y >= u->h) {
        if (g_lua_cart_is_debug)
            return luaL_error(L, "lk:set out of bounds or on a released lock");
        return 0;
    }
    u->pixels[(uint32_t)y * (uint32_t)u->w + (uint32_t)x] = c;
    if (u->is_screen)
        g_lua_drawn = true;
    return 0;
}

static int lua_wasm_lock_clear(lua_State *L) {
    lua_wasm_lock_t *u = lua_wasm_lock_live(L, 1);
    if (u) {
        blyt_raster_clear(u->pixels, u->w, u->w, u->h, (uint8_t)luaL_checkinteger(L, 2));
        if (u->is_screen)
            g_lua_drawn = true;
    }
    return 0;
}
static int lua_wasm_lock_rect_fill(lua_State *L) {
    lua_wasm_lock_t *u = lua_wasm_lock_live(L, 1);
    if (u) {
        blyt_raster_rect_fill(u->pixels, u->w, u->w, u->h, (int)luaL_checkinteger(L, 2),
                              (int)luaL_checkinteger(L, 3), (int)luaL_checkinteger(L, 4),
                              (int)luaL_checkinteger(L, 5), (uint8_t)luaL_checkinteger(L, 6));
        if (u->is_screen)
            g_lua_drawn = true;
    }
    return 0;
}
static int lua_wasm_lock_line(lua_State *L) {
    lua_wasm_lock_t *u = lua_wasm_lock_live(L, 1);
    if (u) {
        blyt_raster_line(u->pixels, u->w, u->w, u->h, (int)luaL_checkinteger(L, 2),
                         (int)luaL_checkinteger(L, 3), (int)luaL_checkinteger(L, 4),
                         (int)luaL_checkinteger(L, 5), (uint8_t)luaL_checkinteger(L, 6));
        if (u->is_screen)
            g_lua_drawn = true;
    }
    return 0;
}

static int lua_wasm_lock_release(lua_State *L) {
    lua_wasm_lock_t *u = (lua_wasm_lock_t *)luaL_checkudata(L, 1, BLYT_WASM_LOCK_MT);
    if (!u->released && u->epoch == g_lua_lock_epoch) {
        if (g_session) {
            blyt_session_surface_release(g_session, u->token);
        } else {
            lua_surface_t *s = lua_surf_resolve(u->handle);
            if (s)
                s->locked = false;
        }
    }
    u->released = true;
    return 0;
}

static void wasm_register_surface_lock_mt(lua_State *L) {
    luaL_newmetatable(L, BLYT_WASM_LOCK_MT);
    lua_newtable(L); /* __index */
    static const luaL_Reg lock_methods[] = {
        {"get", lua_wasm_lock_get},
        {"set", lua_wasm_lock_set},
        {"clear", lua_wasm_lock_clear},
        {"rect_fill", lua_wasm_lock_rect_fill},
        {"line", lua_wasm_lock_line},
        {"release", lua_wasm_lock_release},
        {NULL, NULL},
    };
    luaL_setfuncs(L, lock_methods, 0);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1); /* pop mt */
}

/* Register blyt32.gfx.* onto the existing blyt32 global.  Called from both the
 * initial run_lua_cart setup and the reset/reload re-register, like the state
 * and resource API helpers. */
static void wasm_register_gfx_api(lua_State *L) {
    lua_gfx_palette_ensure_default();
    lua_getglobal(L, "blyt32");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_newtable(L); /* blyt32.gfx */
    static const struct {
        const char *name;
        lua_CFunction fn;
    } gfx_fns[] = {
        {"clear", lua_wasm_gfx_clear},
        {"pixel", lua_wasm_gfx_pixel},
        {"rect_fill", lua_wasm_gfx_rect_fill},
        {"line", lua_wasm_gfx_line},
        {"palette_set", lua_wasm_gfx_palette_set},
        {NULL, NULL},
    };
    for (int i = 0; gfx_fns[i].name; i++) {
        lua_pushcfunction(L, gfx_fns[i].fn);
        lua_setfield(L, -2, gfx_fns[i].name);
    }
    /* Built-in palette constants (#201), mirroring blyt32lua.c's pure-Lua
     * binding and BLYT_SCREEN's plain-constant-field precedent.  wasm_main.c
     * is host-side (not cart code), so it encodes directly from the canonical
     * runtime/shared source rather than including the cart-facing blyt.h. */
    lua_pushinteger(
        L, (lua_Integer)BLYT_RESOURCE_ENCODE(BLYT_PAL_ID_AURORA, BLYT_RESOURCE_PROV_RUNTIME));
    lua_setfield(L, -2, "PALETTE_AURORA");
    lua_pushinteger(L,
                    (lua_Integer)BLYT_RESOURCE_ENCODE(BLYT_PAL_ID_VGA, BLYT_RESOURCE_PROV_RUNTIME));
    lua_setfield(L, -2, "PALETTE_VGA");
    lua_pushinteger(L,
                    (lua_Integer)BLYT_RESOURCE_ENCODE(BLYT_PAL_ID_EGA, BLYT_RESOURCE_PROV_RUNTIME));
    lua_setfield(L, -2, "PALETTE_EGA");
    lua_pushinteger(L,
                    (lua_Integer)BLYT_RESOURCE_ENCODE(BLYT_PAL_ID_CGA, BLYT_RESOURCE_PROV_RUNTIME));
    lua_setfield(L, -2, "PALETTE_CGA");
    lua_pushinteger(
        L, (lua_Integer)BLYT_RESOURCE_ENCODE(BLYT_PAL_ID_AURORA, BLYT_RESOURCE_PROV_RUNTIME));
    lua_setfield(L, -2, "PALETTE_DEFAULT");
    lua_setfield(L, -2, "gfx"); /* blyt32.gfx = gfx */

    /* blyt32.surface.* — tier-1 surface API on the host-Lua fast path (#205). */
    lua_newtable(L); /* blyt32.surface */
    static const struct {
        const char *name;
        lua_CFunction fn;
    } surface_fns[] = {
        {"create", lua_wasm_surface_create},
        {"destroy", lua_wasm_surface_destroy},
        {"clear", lua_wasm_surface_clear},
        {"pixel", lua_wasm_surface_pixel},
        {"rect_fill", lua_wasm_surface_rect_fill},
        {"line", lua_wasm_surface_line},
        {"blit", lua_wasm_surface_blit},
        {"acquire", lua_wasm_surface_acquire},
        {NULL, NULL},
    };
    for (int i = 0; surface_fns[i].name; i++) {
        lua_pushcfunction(L, surface_fns[i].fn);
        lua_setfield(L, -2, surface_fns[i].name);
    }
    lua_pushinteger(L, (lua_Integer)BLYT_SCREEN); /* blyt32.surface.SCREEN */
    lua_setfield(L, -2, "SCREEN");
    lua_setfield(L, -2, "surface"); /* blyt32.surface = surface */

    /* --- blyt32.color subtable: named color-index constants (#203) --- */
    /* Mirrors blyt32lua.c's pure-Lua binding.  wasm_main.c is host-side (not
     * cart code) so it cannot include the cart-facing blyt.h -- these raw
     * indices MUST match blyt.h's BLYT_EGA_* / BLYT_AURORA_*.  The cross-leg
     * parity test (identical frame hash on native/wasm/libretro) is the guard
     * against drift here. */
    static const char *const color_names[16] = {
        "BLACK",  "BLUE",    "GREEN",    "CYAN",    "RED",    "MAGENTA",    "BROWN",     "LTGRAY",
        "DKGRAY", "BR_BLUE", "BR_GREEN", "BR_CYAN", "BR_RED", "BR_MAGENTA", "BR_YELLOW", "WHITE",
    };
    static const uint8_t ega_idx[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    static const uint8_t aurora_idx[16] = {0, 223, 185, 195, 155, 239, 165, 10,
                                           5, 219, 189, 201, 160, 236, 175, 15};
    lua_newtable(L); /* blyt32.color */
    for (int pass = 0; pass < 2; pass++) { /* color.ega, color.vga (same set) */
        lua_newtable(L);
        for (int i = 0; i < 16; i++) {
            lua_pushinteger(L, (lua_Integer)ega_idx[i]);
            lua_setfield(L, -2, color_names[i]);
        }
        lua_setfield(L, -2, pass == 0 ? "ega" : "vga");
    }
    lua_newtable(L); /* color.aurora */
    for (int i = 0; i < 16; i++) {
        lua_pushinteger(L, (lua_Integer)aurora_idx[i]);
        lua_setfield(L, -2, color_names[i]);
    }
    lua_setfield(L, -2, "aurora");
    for (int i = 0; i < 16; i++) { /* default aliases on color root -> aurora */
        lua_pushinteger(L, (lua_Integer)aurora_idx[i]);
        lua_setfield(L, -2, color_names[i]);
    }
    lua_setfield(L, -2, "color"); /* blyt32.color = color */

    wasm_register_surface_lock_mt(L); /* blyt.surface.lock metatable (tier-2, #208) */

    lua_pop(L, 1); /* pop blyt32 */
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
    if (g_lua_resources_loaded) {
        blyt_resource_table_clear(&g_lua_resources);
        g_lua_resources_loaded = false;
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
        r = blyt_save_write(ctx, active_save_dir(), active_cart_name(), slot,
                            blyt_cart_save_version(g_cart));
    lua_pushinteger(L, r);
    return 1;
}

static int wasm_lua_save_read(lua_State *L) {
    uint32_t slot = (uint32_t)luaL_checkinteger(L, 1);
    int r = -1;
    uint32_t saved_version = 0;
    blyt_state_ctx_t *ctx = active_state_ctx();
    if (ctx)
        r = blyt_save_read(ctx, active_save_dir(), active_cart_name(), slot, &saved_version);
    lua_pushinteger(L, r);
    if (r == BLYT_RUN_OK) {
        lua_getglobal(L, "on_load_state");
        if (lua_isfunction(L, -1)) {
            blyt_tracef(BLYT_TRACE_LIFECYCLE, "call on_load_state");
            lua_newtable(L);
            lua_pushinteger(L, 0); /* reason=BLYT_LOAD_SAVE_GAME */
            lua_setfield(L, -2, "reason");
            lua_pushinteger(L, (lua_Integer)saved_version);
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

    /* active_state_ctx() — not g_lua_state_ctx directly — so the S proxy is
     * registered for HYBRID carts too (whose state ctx lives in g_session;
     * g_lua_state_ctx is only set for pure-Lua carts).  Without this, hybrid
     * carts get no `S` global and any S.* access errors. */
    blyt_state_ctx_t *ctx = active_state_ctx();
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
 * blyt.resource.* — host-Lua reimplementation (#93).
 *
 * On WASM all Lua runs host-side (g_lua), so the guest libblyt32lua binding is
 * never reached.  This mirrors it byte-for-byte behaviourally, reading the host
 * resource table directly (no ECALL): the session's ctx.resources for hybrid
 * carts, the standalone g_lua_resources for pure-Lua carts.  The handle/refcount
 * semantics replicate the resource lifecycle ECALL handlers in cart_run.c.
 * ------------------------------------------------------------------------- */

/* Kind-specific constant metatables (ADR-0068/#166): typed-ness is the metatable,
 * so the wrong accessor raises "attempt to call a nil value".  ADR-0134/#196
 * collapsed accessors onto the constant — no separate loaded handle. */
#define BLYT_RESOURCE_TEXT_CONST_MT "blyt.resource.text_const"
#define BLYT_RESOURCE_BYTES_CONST_MT "blyt.resource.bytes_const"
/* palette constant (#214): mirror of the guest binding — a cart palette asset's
 * R.<NAME>, consumed by gfx.palette_set (no bytes accessor). */
#define BLYT_RESOURCE_PALETTE_CONST_MT "blyt.resource.palette_const"

typedef struct {
    uint32_t id; /* the baked console-wide constant (ADR-0134) */
    int is_text;
} wasm_resource_const_t;

static blyt_resource_table_t *active_resource_table(void) {
    if (g_session)
        return blyt_session_resources(g_session);
    return g_lua_resources_loaded ? &g_lua_resources : NULL;
}

static wasm_resource_const_t *wasm_opt_const(lua_State *L, int idx) {
    void *p = luaL_testudata(L, idx, BLYT_RESOURCE_TEXT_CONST_MT);
    if (!p)
        p = luaL_testudata(L, idx, BLYT_RESOURCE_BYTES_CONST_MT);
    if (!p)
        p = luaL_testudata(L, idx, BLYT_RESOURCE_PALETTE_CONST_MT);
    return (wasm_resource_const_t *)p;
}

/* Resolve a baked resource constant to its host table entry: classify as a
 * cart-bundled RESOURCE and look up the decoded id (ADR-0134).  NULL for a
 * non-resource kind, runtime-shipped provenance, or an absent id. */
static blyt_resource_entry_t *wasm_resolve(blyt_resource_table_t *t, uint32_t handle) {
    if (!t || !blyt_handle_is_resource(handle) ||
        blyt_resource_decode_provenance(handle) != BLYT_RESOURCE_PROV_CART)
        return NULL;
    return blyt_resource_table_find_mut(t, blyt_resource_decode_id(handle));
}

/* Pin an entry exactly as the rv32 RESOURCE_PIN ECALL handler does (cart_run.c):
 * budget-gate, materialize (decode zstd), bump the pin, touch for LRU, republish
 * the footprint — so the WASM host-Lua path stays behaviourally identical to the
 * emulated path (#158).  Returns the resident bytes via *out (NULL/0-len is a
 * valid success), or false if absent / over budget / decode failure. */
static bool wasm_pin_entry(blyt_resource_table_t *t, blyt_resource_entry_t *e,
                           const uint8_t **out) {
    *out = NULL;
    if (!e || !wasm_lua_reference_fits(t, e, blyt_rl_is_evictable(&e->rl)))
        return false;
    const uint8_t *bytes = blyt_resource_entry_data(e);
    if (!bytes && e->len)
        return false; /* decode failed */
    blyt_rl_pin(&e->rl);
    blyt_resource_table_touch(t, e); /* recency for LRU (#158) */
    wasm_lua_publish_footprint(t); /* footprint grew; bound cache */
    *out = bytes;
    return true;
}

static void wasm_unpin_entry(blyt_resource_table_t *t, blyt_resource_entry_t *e) {
    blyt_rl_unpin(&e->rl);
    wasm_lua_publish_footprint(t); /* footprint may have shrunk (#158) */
}

static int wasm_text_resource(lua_State *L) {
    uint32_t id = (uint32_t)luaL_checkinteger(L, 1);
    wasm_resource_const_t *c = (wasm_resource_const_t *)lua_newuserdatauv(L, sizeof(*c), 0);
    c->id = id;
    c->is_text = 1;
    luaL_setmetatable(L, BLYT_RESOURCE_TEXT_CONST_MT);
    return 1;
}

static int wasm_bytes_resource(lua_State *L) {
    uint32_t id = (uint32_t)luaL_checkinteger(L, 1);
    wasm_resource_const_t *c = (wasm_resource_const_t *)lua_newuserdatauv(L, sizeof(*c), 0);
    c->id = id;
    c->is_text = 0;
    luaL_setmetatable(L, BLYT_RESOURCE_BYTES_CONST_MT);
    return 1;
}

static int wasm_palette_resource(lua_State *L) {
    uint32_t id = (uint32_t)luaL_checkinteger(L, 1);
    wasm_resource_const_t *c = (wasm_resource_const_t *)lua_newuserdatauv(L, sizeof(*c), 0);
    c->id = id;
    c->is_text = 0; /* unused for a palette constant */
    luaL_setmetatable(L, BLYT_RESOURCE_PALETTE_CONST_MT);
    return 1;
}

static int wasm_palette_tostring(lua_State *L) {
    wasm_resource_const_t *c = luaL_checkudata(L, 1, BLYT_RESOURCE_PALETTE_CONST_MT);
    lua_pushfstring(L, "palette<%d>", (int)c->id);
    return 1;
}

/* Resolve a palette handle to its 256-entry XRGB8888 bytes on the host-Lua fast
 * path (#201/#214), the mirror of cart_run.c's blyt_resolve_palette: RUNTIME ->
 * the built-in table; CART -> the active resource table (session for a hybrid,
 * g_lua_resources for pure-Lua), which must hold exactly 1024 bytes. */
static const uint8_t *lua_resolve_palette(uint32_t handle) {
    if (blyt_resource_decode_provenance(handle) == BLYT_RESOURCE_PROV_RUNTIME)
        return (const uint8_t *)blyt_builtin_palette(handle);
    blyt_resource_entry_t *e = wasm_resolve(active_resource_table(), handle);
    if (!e || e->len != 256u * sizeof(uint32_t))
        return NULL;
    return blyt_resource_entry_data(e);
}

/* Seed the session-less pure-Lua fast path's palette to the cart's DECLARED
 * default (#219): the built-in or palette-file asset named by `palettes:
 * default:` in blyt.config.yaml, or aurora when undeclared/unresolvable.  This
 * is the fast-path equivalent of cart_run.c's pre-init auto-load — without it a
 * pure-Lua cart's declared default was ignored and the fast path always showed
 * aurora, diverging from every emulated leg.  Must run AFTER g_lua_resources is
 * loaded (a PROV_CART default resolves against that table) and marks the palette
 * initialised so the lazy aurora seed (lua_gfx_palette_ensure_default) is a
 * no-op thereafter.  Hybrid carts are unaffected: their palette is the session's
 * (seeded by blyt_session_create), not g_lua_gfx_palette. */
static void lua_gfx_seed_declared_default(void) {
    uint32_t handle = g_cart ? blyt_cart_default_palette(g_cart) : 0;
    if (handle == 0)
        handle = BLYT_RESOURCE_ENCODE(BLYT_PAL_ID_AURORA, BLYT_RESOURCE_PROV_RUNTIME);
    const uint8_t *pal = lua_resolve_palette(handle);
    if (!pal)
        pal = lua_resolve_palette(
            BLYT_RESOURCE_ENCODE(BLYT_PAL_ID_AURORA, BLYT_RESOURCE_PROV_RUNTIME));
    /* Explicit little-endian decode: the cart-resource bytes may be unaligned,
     * and an explicit LE reconstruction stays bit-identical to every other leg. */
    for (int i = 0; i < 256; i++)
        g_lua_gfx_palette[i] = (uint32_t)pal[i * 4] | ((uint32_t)pal[i * 4 + 1] << 8) |
                               ((uint32_t)pal[i * 4 + 2] << 16) | ((uint32_t)pal[i * 4 + 3] << 24);
    g_lua_gfx_palette_init = true;
}

/* Palette load (#201/#214).  Routes to the session palette for hybrid carts, the
 * standalone buffer for session-less pure-Lua carts (lua_gfx_palette).  Accepts
 * an integer built-in handle or a palette constant userdata (R.<NAME>); a no-op
 * on a handle that does not resolve to a 256-entry palette. */
static int lua_wasm_gfx_palette_set(lua_State *L) {
    wasm_resource_const_t *c =
        (wasm_resource_const_t *)luaL_testudata(L, 1, BLYT_RESOURCE_PALETTE_CONST_MT);
    uint32_t handle = c ? c->id : (uint32_t)luaL_checkinteger(L, 1);
    const uint8_t *pal = lua_resolve_palette(handle);
    if (pal) {
        uint32_t *dst = lua_gfx_palette();
        for (int i = 0; i < 256; i++)
            dst[i] = (uint32_t)pal[i * 4] | ((uint32_t)pal[i * 4 + 1] << 8) |
                     ((uint32_t)pal[i * 4 + 2] << 16) | ((uint32_t)pal[i * 4 + 3] << 24);
    }
    return 0;
}

static int wasm_const_id(lua_State *L) {
    wasm_resource_const_t *c = wasm_opt_const(L, 1);
    luaL_argcheck(L, c != NULL, 1, "resource constant expected");
    lua_pushinteger(L, (lua_Integer)c->id);
    return 1;
}

static int wasm_const_eq(lua_State *L) {
    wasm_resource_const_t *a = wasm_opt_const(L, 1);
    wasm_resource_const_t *b = wasm_opt_const(L, 2);
    lua_pushboolean(L, a && b && a->id == b->id && a->is_text == b->is_text);
    return 1;
}

static int wasm_const_tostring(lua_State *L) {
    wasm_resource_const_t *c = wasm_opt_const(L, 1);
    luaL_argcheck(L, c != NULL, 1, "resource constant expected");
    lua_pushfstring(L, c->is_text ? "text_resource<%d>" : "bytes_resource<%d>", (int)c->id);
    return 1;
}

/* text constant :text() — owned copy, trailing storage NUL stripped (#166). */
static int wasm_const_text(lua_State *L) {
    wasm_resource_const_t *c = luaL_checkudata(L, 1, BLYT_RESOURCE_TEXT_CONST_MT);
    blyt_resource_table_t *t = active_resource_table();
    blyt_resource_entry_t *e = wasm_resolve(t, c->id);
    const uint8_t *bytes = NULL;
    if (!wasm_pin_entry(t, e, &bytes)) {
        lua_pushnil(L);
        return 1;
    }
    size_t content = (e->len >= 1 && bytes[e->len - 1] == '\0') ? e->len - 1 : e->len;
    lua_pushlstring(L, (const char *)bytes, content);
    wasm_unpin_entry(t, e);
    return 1;
}

/* bytes constant :bytes() — owned copy of the exact bytes, verbatim (#162). */
static int wasm_const_bytes(lua_State *L) {
    wasm_resource_const_t *c = luaL_checkudata(L, 1, BLYT_RESOURCE_BYTES_CONST_MT);
    blyt_resource_table_t *t = active_resource_table();
    blyt_resource_entry_t *e = wasm_resolve(t, c->id);
    const uint8_t *bytes = NULL;
    if (!wasm_pin_entry(t, e, &bytes)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, (const char *)bytes, e->len);
    wasm_unpin_entry(t, e);
    return 1;
}

/* Module-level pin/unpin: kind-agnostic raw escape hatch, takes the constant (#166). */
static int wasm_resource_pin(lua_State *L) {
    uint32_t id = (uint32_t)luaL_checkinteger(L, 1);
    blyt_resource_table_t *t = active_resource_table();
    blyt_resource_entry_t *e = wasm_resolve(t, id);
    const uint8_t *bytes = NULL;
    if (!wasm_pin_entry(t, e, &bytes)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlightuserdata(L, (void *)(uintptr_t)bytes);
    lua_pushinteger(L, (lua_Integer)e->len);
    return 2;
}

static int wasm_resource_unpin(lua_State *L) {
    uint32_t id = (uint32_t)luaL_checkinteger(L, 1);
    blyt_resource_table_t *t = active_resource_table();
    blyt_resource_entry_t *e = wasm_resolve(t, id);
    if (e)
        wasm_unpin_entry(t, e);
    return 0;
}

/* Build a kind-specific constant metatable carrying its accessor plus the shared
 * :id()/__eq/__tostring.  Mirrors the guest register_const_mt. */
static void wasm_register_const_mt(lua_State *L, const char *mt_name, lua_CFunction accessor,
                                   const char *accessor_name) {
    luaL_newmetatable(L, mt_name);
    lua_pushcfunction(L, wasm_const_eq);
    lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, wasm_const_tostring);
    lua_setfield(L, -2, "__tostring");
    lua_newtable(L);
    lua_pushcfunction(L, accessor);
    lua_setfield(L, -2, accessor_name);
    lua_pushcfunction(L, wasm_const_id);
    lua_setfield(L, -2, "id");
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1); /* pop mt */
}

/* Register blyt.resource.* + blyt32.resource.* into g_lua, mirroring the guest
 * register_resource_module.  Call after the blyt/blyt32 globals exist. */
static void wasm_register_resource_api(lua_State *L) {
    wasm_register_const_mt(L, BLYT_RESOURCE_TEXT_CONST_MT, wasm_const_text, "text");
    wasm_register_const_mt(L, BLYT_RESOURCE_BYTES_CONST_MT, wasm_const_bytes, "bytes");

    /* Palette constant metatable (#214): :id()/__eq shared, palette-specific
     * __tostring, no bytes accessor. */
    luaL_newmetatable(L, BLYT_RESOURCE_PALETTE_CONST_MT);
    lua_pushcfunction(L, wasm_const_eq);
    lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, wasm_palette_tostring);
    lua_setfield(L, -2, "__tostring");
    lua_newtable(L);
    lua_pushcfunction(L, wasm_const_id);
    lua_setfield(L, -2, "id");
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1); /* pop mt */

    lua_newtable(L); /* resource module */
    lua_pushcfunction(L, wasm_text_resource);
    lua_setfield(L, -2, "text_resource");
    lua_pushcfunction(L, wasm_bytes_resource);
    lua_setfield(L, -2, "bytes_resource");
    lua_pushcfunction(L, wasm_palette_resource);
    lua_setfield(L, -2, "palette");
    lua_pushcfunction(L, wasm_resource_pin);
    lua_setfield(L, -2, "pin");
    lua_pushcfunction(L, wasm_resource_unpin);
    lua_setfield(L, -2, "unpin");

    lua_getglobal(L, "blyt");
    lua_pushvalue(L, -2);
    lua_setfield(L, -2, "resource");
    lua_pop(L, 1);
    lua_getglobal(L, "blyt32");
    if (lua_istable(L, -1)) {
        lua_pushvalue(L, -2);
        lua_setfield(L, -2, "resource");
    }
    lua_pop(L, 1);
    lua_pop(L, 1); /* pop resource module */
}

/* blyt32.mem.stats() for the WASM host-Lua fast path (ADR-0029, #159). Mirrors
 * the guest lua_mem_stats / host MEM_STATS ECALL byte-for-byte behaviourally,
 * reading the host accounting block + resource table directly (no ECALL). The
 * deterministic-vs-advisory contract is documented on blyt_mem_stats in blyt.h.
 * Note: this path never decompresses (it reads e->data / e->owned as-is), so
 * resource_cache_used reflects only already-owned buffers — advisory, and not
 * cross-leg identical by design. */
static int wasm_mem_stats(lua_State *L) {
    blyt_resource_table_t *t = active_resource_table();
    uint32_t cache_used = t ? blyt_resource_table_resident_decompressed(t) : 0u;
    uint32_t heap_used = blyt_mem_cart_heap(&g_lua_mem_acct);

    lua_createtable(L, 0, 5);
    lua_pushinteger(L, (lua_Integer)cache_used);
    lua_setfield(L, -2, "resource_cache_used");
    lua_pushinteger(L, (lua_Integer)heap_used);
    lua_setfield(L, -2, "cart_allocations");
    lua_pushinteger(L, (lua_Integer)(heap_used + cache_used));
    lua_setfield(L, -2, "total_used");
    lua_pushinteger(L, (lua_Integer)BLYT_MEM_BUDGET_BYTES);
    lua_setfield(L, -2, "budget_cap");

    lua_newtable(L); /* resources_loaded */
    uint32_t shown = 0;
    if (t) {
        for (size_t i = 0; i < t->count; i++) {
            const blyt_resource_entry_t *e = &t->entries[i];
            /* Resident working set (ADR-0134, advisory): persistent (#160), a
             * decompressed entry in the cache (owned), or an uncompressed
             * zero-copy entry the cart has accessed (last_access > 0).  The id is
             * reported as the baked constant so it matches R_<NAME>. */
            if (!(e->persistent || e->owned != NULL ||
                  (e->algo == BLYT_RES_ALGO_NONE && e->last_access > 0)))
                continue;
            lua_createtable(L, 0, 2);
            lua_pushinteger(L, (lua_Integer)blyt_resource_encode(e->id, BLYT_RESOURCE_PROV_CART));
            lua_setfield(L, -2, "id");
            lua_pushinteger(L, (lua_Integer)(uint32_t)e->len);
            lua_setfield(L, -2, "size");
            lua_rawseti(L, -2, (lua_Integer)(++shown));
        }
    }
    lua_setfield(L, -2, "resources_loaded");
    return 1;
}

/* Register blyt.mem.* + blyt32.mem.* into g_lua, mirroring the guest
 * register_mem_module.  Call after the blyt/blyt32 globals exist. */
static void wasm_register_mem_api(lua_State *L) {
    lua_newtable(L); /* mem module */
    lua_pushcfunction(L, wasm_mem_stats);
    lua_setfield(L, -2, "stats");

    lua_getglobal(L, "blyt");
    lua_pushvalue(L, -2);
    lua_setfield(L, -2, "mem");
    lua_pop(L, 1);
    lua_getglobal(L, "blyt32");
    if (lua_istable(L, -1)) {
        lua_pushvalue(L, -2);
        lua_setfield(L, -2, "mem");
    }
    lua_pop(L, 1);
    lua_pop(L, 1); /* pop mem module */
}

/* Derive a require()-able module name from a loaded chunk's embedded source
 * (basename minus ".lua"); the chunk function must be on the stack top and is
 * left untouched.  Mirrors chunk_module_name in libblyt32lua. */
static void wasm_chunk_module_name(lua_State *L, char *out, size_t outsz) {
    out[0] = '\0';
    lua_Debug ar;
    lua_pushvalue(L, -1);
    if (lua_getinfo(L, ">S", &ar) && ar.source) {
        const char *src = ar.source;
        if (*src == '@' || *src == '=')
            src++;
        const char *base = src;
        for (const char *p = src; *p; p++)
            if (*p == '/' || *p == '\\')
                base = p + 1;
        size_t len = strlen(base);
        if (len > 4 && strcmp(base + len - 4, ".lua") == 0)
            len -= 4;
        if (len >= outsz)
            len = outsz - 1;
        memcpy(out, base, len);
        out[len] = '\0';
    }
}

/* Load the cart's .cart.lua: a single raw bytecode chunk, or the BLMC multi-
 * chunk container (issue #54).  A chunk returning a non-nil table is registered
 * as a require()-able module (cart_resources, ADR-0040), keyed by source
 * basename.  Mirrors open_state in libblyt32lua so the host-Lua fast path stays
 * behaviourally identical.  Returns 0 on success; on failure returns nonzero
 * with an error string on the stack top. */
static int wasm_load_lua_bytecode(lua_State *L, const unsigned char *data, size_t size) {
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
            char modname[64];
            wasm_chunk_module_name(L, modname, sizeof(modname));
            if (lua_pcall(L, 0, 1, 0) != LUA_OK)
                return 1;
            if (modname[0] != '\0' && !lua_isnil(L, -1)) {
                luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
                lua_pushvalue(L, -2);
                lua_setfield(L, -2, modname);
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
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

/* -------------------------------------------------------------------------
 * Reset-every-frame cycle for the Lua path
 *
 * Full VM teardown + recreate: destroys the coroutine and all Lua globals,
 * recreates a fresh VM, re-runs the cart script, calls init() from C, then
 * restores the state buffer snapshot so state persists across the reset.
 * Called after draw() completes when BLYT_RESET_EVERY_FRAME=1.
 * ------------------------------------------------------------------------- */
/* Rebuild the Lua VM from g_lua_bytecode in place.
 *   preserve_state — restore state buffers + replay on_load_state(HOT_RELOAD);
 *                    otherwise zero state and run on_new_state() (cold boot).
 *   ext_snap       — when non-NULL, a snapshot captured by the caller before a
 *                    cart swap (reload); used instead of capturing internally.
 *                    Ownership transfers to this function (it is freed here). */
static bool wasm_lua_rebuild(bool preserve_state, blyt_state_snapshot_t *ext_snap,
                             bool defer_init) {
    blyt_state_snapshot_t *snap = ext_snap;
    blyt_state_ctx_t *sctx = active_state_ctx();

    if (preserve_state && !snap) {
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
        if (sctx)
            snap = blyt_state_ctx_snapshot(sctx);
    }

    /* Step 3: zero state buffers (fresh baseline; restored below if preserving) */
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

    /* Step 5: create fresh Lua VM on the shared cart-heap arena. Reset the arena
     * first so the reloaded cart's allocations (and guest_heap_used) are
     * bit-identical to a fresh load (#158, mirrors the native hot-swap reset). */
    wasm_lua_arena_reset();
    /* luaL_makeseed(NULL) is the fixed blyt hash seed (luai_makeseed override) —
     * the same deterministic seed luaL_newstate uses, kept for determinism. */
    g_lua = lua_newstate(wasm_lua_alloc, NULL, luaL_makeseed(NULL));
    if (!g_lua) {
        if (snap)
            blyt_state_snapshot_free(snap);
        g_lua_fatal = true;
        return false;
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
    luaL_requiref(g_lua, LUA_UTF8LIBNAME, luaopen_utf8, 1);
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
    lua_pushcfunction(g_lua, lua_wasm_phase_update);
    lua_setglobal(g_lua, "__blyt_phase_update");
    lua_pushcfunction(g_lua, lua_wasm_phase_draw);
    lua_setglobal(g_lua, "__blyt_phase_draw");

    /* Step 8: re-register state + resource API */
    if (active_state_ctx())
        wasm_register_state_api(g_lua, g_session);
    wasm_register_s_proxy(g_lua);
    wasm_register_resource_api(g_lua);
    wasm_register_gfx_api(g_lua); /* blyt32.gfx.* (#188) */
    wasm_register_mem_api(g_lua);

    /* Step 8b: for hybrid carts, recreate the Lua↔native exchange thread and
     * register the native export modules BEFORE the bytecode runs — the cart's
     * top-level `require("<module>")` resolves during bytecode load (step 9), so
     * the trampolines must already be installed.  Mirrors the initial-load order
     * in run_lua_cart (bridge attach precedes wasm_load_lua_bytecode); doing it
     * after, as before, broke a hybrid reload of any cart that require()s a
     * native module at chunk top level (issue #124). */
    if (g_session && g_has_lua_exports) {
        g_lua_exch = lua_newthread(g_lua);
        g_lua_exch_ref = luaL_ref(g_lua, LUA_REGISTRYINDEX);
        blyt_session_lua_bridge_attach(g_session, g_lua_exch);
        wasm_register_lua_trampolines(g_lua, g_session);
    }

    /* Step 9: re-run .cart.lua section (handles BLMC + module registration). */
    if (wasm_load_lua_bytecode(g_lua, (const unsigned char *)g_lua_bytecode, g_lua_bytecode_size) !=
        0) {
        blyt_js_error(lua_tostring(g_lua, -1));
        lua_close(g_lua);
        g_lua = NULL;
        if (snap)
            blyt_state_snapshot_free(snap);
        g_lua_fatal = true;
        return false;
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
                return false;
            }
            maybe_inject_lifecycle_cb(g_lua, cbs[i].name, fn);
        }
    }

#ifdef BLYT_GDB
    if (defer_init) {
        /* Debug hot reload (issue #170): DON'T run init() synchronously here — it
         * would run outside any coroutine (so a Lua→native trampoline could not
         * yield for a native GDB breakpoint) and before the DAP master hook is
         * re-armed (so a Lua init() breakpoint could not fire), which is exactly
         * why the pre-fix reload reported native=1 lua=0.  Instead set up the
         * INIT-phase coroutine + master hook and let wasm_lua_loop drive init()
         * under the loop like a cold boot, so BOTH init breakpoints fire and
         * round-trip.  The restore + on_load_state(HOT_RELOAD) tail runs once
         * that init() completes (wasm_lua_reload_restore_tail), so the caller
         * keeps ownership of the snapshot (g_reload_snap). */
        g_lua_co = lua_newthread(g_lua);
        g_lua_co_ref = luaL_ref(g_lua, LUA_REGISTRYINDEX);
        luaL_loadstring(g_lua_co, co_body_reload_init);
#ifdef BLYT_DAP
        if (fc_master_hook_cfg.dap_enabled)
            fc_consolelua_master_hook_install(g_lua_co);
#endif
        g_lua_phase = LUA_PHASE_INIT;
        return true;
    }
#else
    (void)defer_init;
#endif

    /* Step 12: call init() from C (no coroutine needed for this reset call) */
    lua_getglobal(g_lua, "init");
    if (lua_isfunction(g_lua, -1)) {
        blyt_tracef(BLYT_TRACE_LIFECYCLE, "call init");
        lua_pcall(g_lua, 0, 0, 0);
        blyt_tracef(BLYT_TRACE_LIFECYCLE, "ret init");
    } else {
        lua_pop(g_lua, 1);
    }

    if (preserve_state) {
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
    } else {
        /* Fresh reset (no state preserved): run on_new_state() like a cold boot. */
        lua_getglobal(g_lua, "on_new_state");
        if (lua_isfunction(g_lua, -1)) {
            blyt_tracef(BLYT_TRACE_LIFECYCLE, "call on_new_state");
            lua_pcall(g_lua, 0, 0, 0);
            blyt_tracef(BLYT_TRACE_LIFECYCLE, "ret on_new_state");
        } else {
            lua_pop(g_lua, 1);
        }
    }

    /* Step 15: create running coroutine for next frame */
    g_lua_co = lua_newthread(g_lua);
    g_lua_co_ref = luaL_ref(g_lua, LUA_REGISTRYINDEX);
    luaL_loadstring(g_lua_co, co_body_running);
#ifdef BLYT_DAP
    /* Re-arm the DAP master hook on the freshly built running coroutine so
     * breakpoints keep firing after a hot reload (issue #90): the debug session
     * is uninterrupted (ADR-0045 DAP continuity), but the hook lives on the
     * coroutine we just replaced — without reinstalling it, the reloaded cart
     * would run with debugging silently dead.  Mirrors the INIT→RUNNING
     * transition and initial-start paths. */
    if (fc_master_hook_cfg.dap_enabled)
        fc_consolelua_master_hook_install(g_lua_co);
#endif
    /* g_lua_phase stays LUA_PHASE_RUNNING — next frame calls update() directly */
    return true;
}

#ifdef BLYT_GDB
/* Deferred tail of a hybrid debug hot reload (issue #170): once the reloaded
 * init() has run under wasm_lua_loop's INIT phase (both the Lua and native init
 * breakpoints having fired), restore the pre-reload state buffers and replay
 * on_load_state(HOT_RELOAD).  Mirrors wasm_lua_rebuild steps 13-14 — the part
 * skipped by defer_init so it lands after the deferred init() instead of before
 * the state existed. */
static void wasm_lua_reload_restore_tail(void) {
    if (g_reload_snap) {
        blyt_state_ctx_restore_snapshot(active_state_ctx(), g_reload_snap);
        blyt_state_snapshot_free(g_reload_snap);
        g_reload_snap = NULL;
    }
    lua_getglobal(g_lua, "on_load_state");
    if (lua_isfunction(g_lua, -1)) {
        blyt_tracef(BLYT_TRACE_LIFECYCLE, "call on_load_state");
        lua_newtable(g_lua);
        lua_pushinteger(g_lua, 3); /* BLYT_LOAD_HOT_RELOAD */
        lua_setfield(g_lua, -2, "reason");
        lua_pushinteger(g_lua, 0);
        lua_setfield(g_lua, -2, "saved_cart_version");
        lua_pcall(g_lua, 1, 0, 0);
        blyt_tracef(BLYT_TRACE_LIFECYCLE, "ret on_load_state");
    } else {
        lua_pop(g_lua, 1);
    }
}
#endif

/* --reset-every-frame stress cycle: rebuild the VM preserving state and replay
 * on_load_state(HOT_RELOAD).  Thin wrapper over the shared rebuild path. */
static void wasm_lua_reset_cycle(void) {
    wasm_lua_rebuild(true, NULL, false);
}

/* -------------------------------------------------------------------------
 * Dev control channel handler (issue #87, amends ADR-0045)
 *
 * The devtool relays single-line JSON lifecycle commands over a WebSocket; the
 * page glue (shell.html) hands each inbound line to blyt_dev_ctrl_command and
 * exposes globalThis.blyt_dev_ctrl_send for responses.  Commands:
 *   {"id":N,"cmd":"reset"}                 cold reboot, state discarded
 *   {"id":N,"cmd":"save_state","slot":S}   persist state to save slot S (default 0)
 *   {"id":N,"cmd":"load_state","slot":S}   restore state from save slot S
 *   {"id":N,"cmd":"reload"}                hot reload: refetch cart, preserve state
 *   {"id":N,"cmd":"update_assets","assets":[…]}  hot-swap edited resources, no restart
 * Responses echo the id and cmd: {"id":N,"status":"ok","cmd":"…"} or
 * {"id":N,"status":"error","cmd":"…","reason":"…"}.
 *
 * Scope: reset/save_state/load_state/reload service the Lua runtime path
 * (pure-Lua and Lua-with-layouts carts — what `blyt run ./dir` serves for the
 * dev loop); WASM carts with native rv32 code get a structured error for those,
 * and the native player (frontends/player) drives them via its own dev control
 * server against the live rv32 session.  update_assets is the exception: it
 * reloads the rv32 session's resource table, so it works for session-backed
 * (C/hybrid) carts and returns a structured error for pure-Lua carts, which have
 * no resource access yet (#120).
 * ------------------------------------------------------------------------- */

/* clang-format off */
EM_JS(void, blyt_js_dev_ctrl_send, (const char *json), {
    if (typeof globalThis !== 'undefined' &&
        typeof globalThis.blyt_dev_ctrl_send === 'function')
        globalThis.blyt_dev_ctrl_send(UTF8ToString(json));
});

EM_JS(void, blyt_js_dev_ctrl_fetch_cart, (void), {
    if (typeof globalThis !== 'undefined' &&
        typeof globalThis.blyt_dev_ctrl_fetch_cart === 'function')
        globalThis.blyt_dev_ctrl_fetch_cart();
});

/* update_assets (issue #118): ask the page to refetch the content-addressed
 * resource staging dir into MEMFS (refresh the index + any missing file, prune
 * superseded ones), then continue in blyt_dev_ctrl_assets_fetched. */
EM_JS(void, blyt_js_dev_ctrl_fetch_resources, (void), {
    if (typeof globalThis !== 'undefined' &&
        typeof globalThis.blyt_dev_ctrl_fetch_resources === 'function')
        globalThis.blyt_dev_ctrl_fetch_resources();
});
/* clang-format on */

/* Minimal single-line JSON readers for the trusted command stream from our own
 * relay.  Not a general parser: keys are matched literally and values are
 * scalars (integer or bare quoted string with no escapes). */
static bool dev_ctrl_find_value(const char *json, const char *key, const char **out) {
    char pat[24];
    int n = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof(pat))
        return false;
    const char *p = strstr(json, pat);
    if (!p)
        return false;
    p += n;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != ':')
        return false;
    p++;
    while (*p == ' ' || *p == '\t')
        p++;
    *out = p;
    return true;
}

static long dev_ctrl_int(const char *json, const char *key, long fallback) {
    const char *v;
    if (!dev_ctrl_find_value(json, key, &v))
        return fallback;
    char *end = NULL;
    long r = strtol(v, &end, 10);
    return (end == v) ? fallback : r;
}

/* Copy a quoted JSON string value for `key` into out[outsz].  Returns false if
 * the key is absent or its value is not a quoted string. */
static bool dev_ctrl_str(const char *json, const char *key, char *out, size_t outsz) {
    const char *v;
    if (!dev_ctrl_find_value(json, key, &v) || *v != '"')
        return false;
    v++;
    size_t i = 0;
    while (*v && *v != '"' && i + 1 < outsz)
        out[i++] = *v++;
    out[i] = '\0';
    return *v == '"';
}

/* Parse a JSON array of integers ("key":[1,2,3]) into out[0..cap), returning the
 * count parsed.  Used for the `assets` id list of the update_assets command. */
static size_t dev_ctrl_int_array(const char *json, const char *key, uint32_t *out, size_t cap) {
    const char *v;
    if (!dev_ctrl_find_value(json, key, &v) || *v != '[')
        return 0;
    v++;
    size_t cnt = 0;
    while (*v && *v != ']' && cnt < cap) {
        while (*v == ' ' || *v == '\t' || *v == ',')
            v++;
        if (*v == ']' || *v == '\0')
            break;
        char *end = NULL;
        long r = strtol(v, &end, 10);
        if (end == v)
            break;
        out[cnt++] = (uint32_t)r;
        v = end;
    }
    return cnt;
}

static void dev_ctrl_respond_ok(long id, const char *cmd) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"id\":%ld,\"status\":\"ok\",\"cmd\":\"%s\"}", id, cmd);
    blyt_js_dev_ctrl_send(buf);
}

static void dev_ctrl_respond_err(long id, const char *cmd, const char *reason) {
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"id\":%ld,\"status\":\"error\",\"cmd\":\"%s\",\"reason\":\"%s\"}",
             id, cmd, reason);
    blyt_js_dev_ctrl_send(buf);
}

/* Pending reload request id, valid between blyt_dev_ctrl_command(reload) and
 * the blyt_dev_ctrl_reload_fetched callback. -1 when no reload is in flight. */
static long g_dev_ctrl_reload_id = -1;

/* Pending update_assets request id, valid between blyt_dev_ctrl_command and the
 * blyt_dev_ctrl_assets_fetched callback. -1 when no update_assets is in flight. */
static long g_dev_ctrl_assets_id = -1;

/* Changed resource ids carried by the in-flight update_assets command, captured
 * at command time and handed to on_assets_reloaded after the refetch completes
 * (issue #122). */
static uint32_t g_dev_ctrl_assets_ids[64];
static size_t g_dev_ctrl_assets_n;

/* True when the handler can service state/lifecycle commands: a Lua VM with no
 * rv32 session (the dev-loop sweet spot). */
static bool dev_ctrl_lua_path(void) {
    return g_lua != NULL && g_session == NULL;
}

EMSCRIPTEN_KEEPALIVE
void blyt_dev_ctrl_command(const char *json) {
    if (!json)
        return;
    long id = dev_ctrl_int(json, "id", 0);
    char cmd[32];
    if (!dev_ctrl_str(json, "cmd", cmd, sizeof(cmd))) {
        dev_ctrl_respond_err(id, "", "malformed command (missing cmd)");
        return;
    }
    blyt_tracef(BLYT_TRACE_LIFECYCLE, "dev_ctrl cmd=%s id=%ld", cmd, id);

    if (strcmp(cmd, "reset") == 0) {
        if (!dev_ctrl_lua_path()) {
            dev_ctrl_respond_err(id, cmd, "reset not supported for carts with native code yet");
            return;
        }
        if (wasm_lua_rebuild(false, NULL, false))
            dev_ctrl_respond_ok(id, cmd);
        else
            dev_ctrl_respond_err(id, cmd, "reset failed");
        return;
    }

    if (strcmp(cmd, "save_state") == 0) {
        if (!dev_ctrl_lua_path()) {
            dev_ctrl_respond_err(id, cmd,
                                 "save_state not supported for carts with native code yet");
            return;
        }
        uint32_t slot = (uint32_t)dev_ctrl_int(json, "slot", 0);
        lua_getglobal(g_lua, "on_save_state");
        if (lua_isfunction(g_lua, -1))
            lua_pcall(g_lua, 0, 0, 0);
        else
            lua_pop(g_lua, 1);
        blyt_state_ctx_t *ctx = active_state_ctx();
        int r = ctx ? blyt_save_write(ctx, active_save_dir(), active_cart_name(), slot,
                                      blyt_cart_save_version(g_cart))
                    : -1;
        if (r == BLYT_RUN_OK)
            dev_ctrl_respond_ok(id, cmd);
        else
            dev_ctrl_respond_err(id, cmd, "save_state failed");
        return;
    }

    if (strcmp(cmd, "load_state") == 0) {
        if (!dev_ctrl_lua_path()) {
            dev_ctrl_respond_err(id, cmd,
                                 "load_state not supported for carts with native code yet");
            return;
        }
        uint32_t slot = (uint32_t)dev_ctrl_int(json, "slot", 0);
        blyt_state_ctx_t *ctx = active_state_ctx();
        uint32_t saved_version = 0;
        int r =
            ctx ? blyt_save_read(ctx, active_save_dir(), active_cart_name(), slot, &saved_version)
                : -1;
        if (r != BLYT_RUN_OK) {
            dev_ctrl_respond_err(id, cmd, "load_state failed");
            return;
        }
        lua_getglobal(g_lua, "on_load_state");
        if (lua_isfunction(g_lua, -1)) {
            lua_newtable(g_lua);
            lua_pushinteger(g_lua, 0); /* reason = BLYT_LOAD_SAVE_GAME */
            lua_setfield(g_lua, -2, "reason");
            lua_pushinteger(g_lua, (lua_Integer)saved_version);
            lua_setfield(g_lua, -2, "saved_cart_version");
            lua_pcall(g_lua, 1, 0, 0);
        } else {
            lua_pop(g_lua, 1);
        }
        dev_ctrl_respond_ok(id, cmd);
        return;
    }

    if (strcmp(cmd, "reload") == 0) {
        /* Reload is supported for every cart type: pure-Lua carts swap host-Lua
         * bytecode (g_session == NULL), while native/hybrid carts (g_session !=
         * NULL) swap the cart's code in the live rv32 session via the in-VM
         * cart-as-library module swap (issue #124, on top of #127's β). */
        if (g_dev_ctrl_reload_id >= 0) {
            dev_ctrl_respond_err(id, cmd, "reload already in progress");
            return;
        }
        /* Hand off to JS to refetch /cart.blyt into MEMFS, then continue in
         * blyt_dev_ctrl_reload_fetched.  State is preserved across the swap. */
        g_dev_ctrl_reload_id = id;
        blyt_js_dev_ctrl_fetch_cart();
        return;
    }

    if (strcmp(cmd, "update_assets") == 0) {
        /* Hot-swap edited resources between frames; no VM restart (issue #118).
         * Reloading the resource table is language-agnostic: session-backed
         * (C/hybrid) carts reload the rv32 session's ctx.resources, pure-Lua
         * fast-path carts (g_session == NULL) reload the host-side g_lua_resources
         * the blyt.resource.* binding reads (issue #120).  The `assets` id list is
         * informational — the browser refetches the whole content-addressed
         * staging dir and the host re-reads the index. */
        if (g_dev_ctrl_assets_id >= 0) {
            dev_ctrl_respond_err(id, cmd, "update_assets already in progress");
            return;
        }
        /* Capture the changed ids now; on_assets_reloaded receives them once the
         * refetch + reload completes (issue #122). */
        g_dev_ctrl_assets_n =
            dev_ctrl_int_array(json, "assets", g_dev_ctrl_assets_ids,
                               sizeof(g_dev_ctrl_assets_ids) / sizeof(g_dev_ctrl_assets_ids[0]));
        /* Hand off to JS to refresh the MEMFS staging dir, then continue in
         * blyt_dev_ctrl_assets_fetched. */
        g_dev_ctrl_assets_id = id;
        blyt_js_dev_ctrl_fetch_resources();
        return;
    }

    dev_ctrl_respond_err(id, cmd, "unknown command");
}

/* Host-Lua fast path (g_session == NULL): fire the cart's global
 * on_assets_reloaded(ids) directly in g_lua, mirroring the emulated guest hook
 * (blyt_cart_on_assets_reloaded in libblyt32lua, issue #122) — the changed
 * resource ids are presented as a 1-based integer array.  No-op when the cart
 * defines no hook or the id set is empty. */
static void wasm_lua_notify_assets_reloaded(const uint32_t *ids, size_t n) {
    if (!g_lua || n == 0 || ids == NULL)
        return;
    lua_getglobal(g_lua, "on_assets_reloaded");
    if (lua_isfunction(g_lua, -1)) {
        blyt_tracef(BLYT_TRACE_LIFECYCLE, "call on_assets_reloaded n=%zu", n);
        lua_createtable(g_lua, (int)n, 0);
        for (size_t i = 0; i < n; i++) {
            lua_pushinteger(g_lua, (lua_Integer)ids[i]);
            lua_seti(g_lua, -2, (lua_Integer)(i + 1));
        }
        if (lua_pcall(g_lua, 1, 0, 0) != LUA_OK) {
            blyt_js_error(lua_tostring(g_lua, -1));
            lua_pop(g_lua, 1);
        }
        blyt_tracef(BLYT_TRACE_LIFECYCLE, "ret on_assets_reloaded");
    } else {
        lua_pop(g_lua, 1);
    }
}

/* Continue an update_assets after JS refreshed the resource staging dir in MEMFS
 * (issue #118).  ok==0 means the JS-side refetch failed.  Reloading the table
 * re-reads the (now updated) resource-id-index from BLYT_RESOURCE_DIR; the next
 * frame's resource read sees the new bytes.  Session-backed carts reload the
 * rv32 session's ctx.resources; pure-Lua fast-path carts (g_session == NULL)
 * reload the host-side g_lua_resources table the blyt.resource.* binding reads
 * (issue #120). */
EMSCRIPTEN_KEEPALIVE
void blyt_dev_ctrl_assets_fetched(int ok) {
    long id = g_dev_ctrl_assets_id;
    g_dev_ctrl_assets_id = -1;
    if (id < 0)
        return;
    if (!ok) {
        dev_ctrl_respond_err(id, "update_assets", "resource refetch failed");
        return;
    }
    if (g_session) {
        if (blyt_session_reload_resources(g_session, g_cart)) {
            blyt_session_notify_assets_reloaded(g_session, g_dev_ctrl_assets_ids,
                                                g_dev_ctrl_assets_n);
            dev_ctrl_respond_ok(id, "update_assets");
        } else {
            dev_ctrl_respond_err(id, "update_assets", "reload_resources failed");
        }
        return;
    }
    /* Pure-Lua fast path: reload the standalone host table (load_for_cart clears
     * it first, so superseded entries drop and current ones re-read), then fire
     * the cart's Lua callback with the changed ids. */
    if (g_lua_resources_loaded) {
        blyt_resource_table_load_for_cart(&g_lua_resources, g_cart);
        /* Re-apply the reloaded cart's persistent set (#160), mirroring the
         * initial load and the session reload path. */
        blyt_resource_table_load_persistent_from_cart(&g_lua_resources, g_cart);
        blyt_resource_table_preload_persistent(&g_lua_resources);
        wasm_lua_publish_footprint(&g_lua_resources);
        wasm_lua_notify_assets_reloaded(g_dev_ctrl_assets_ids, g_dev_ctrl_assets_n);
        dev_ctrl_respond_ok(id, "update_assets");
    } else {
        dev_ctrl_respond_err(id, "update_assets", "no resource table to reload");
    }
}

/* Native/hybrid live reload (issue #124): swap the cart's code in the live rv32
 * session via the in-VM cart-as-library module swap (blyt_session_swap_cart,
 * issue #127), mirroring the player's blyt_libretro_reload — the rv32 VM,
 * libblyt32/libblyt32lua and the dev-control connection all PERSIST (no session
 * recreate, so the rv32emu single-VM-per-process global-state hazard is never
 * crossed).  State is preserved across the swap exactly as the player does it:
 * snapshot → swap → boot init() → restore(HOT_RELOAD).
 *
 * Two cart shapes reach here (g_session != NULL):
 *   - pure-native (g_lua == NULL): the whole cart runs in the VM; snapshot and
 *     restore go through the session, which drives the native cart's
 *     on_save_state/on_load_state hooks.
 *   - hybrid (g_lua != NULL): the cart's Lua runs host-side over a native
 *     bridge.  The native module is swapped, then the host-Lua VM is rebuilt
 *     from the new cart's bytecode (re-binding the native lifecycle trampolines
 *     to the swapped module's exports) with state restored and the host-Lua
 *     on_load_state(HOT_RELOAD) replayed.  The state snapshot is taken BEFORE
 *     the swap because the swap re-boots the cart and re-inits its state ctx.
 * Reports its own ok/error response. */
static void wasm_session_reload(long id) {
    blyt_cart_t *new_cart = NULL;

    if (g_lua) {
        /* Hybrid: flush live host-Lua state via on_save_state, then snapshot the
         * shared state buffers before the swap. */
        blyt_state_ctx_t *sctx = active_state_ctx();
        blyt_state_snapshot_t *snap = NULL;
        if (sctx) {
            lua_getglobal(g_lua, "on_save_state");
            if (lua_isfunction(g_lua, -1))
                lua_pcall(g_lua, 0, 0, 0);
            else
                lua_pop(g_lua, 1);
            snap = blyt_state_ctx_snapshot(sctx);
        }

        blyt_cart_err_t cerr = blyt_cart_open("/cart.blyt", &new_cart);
        if (cerr != BLYT_CART_OK) {
            if (snap)
                blyt_state_snapshot_free(snap);
            dev_ctrl_respond_err(id, "reload", blyt_cart_err_str(cerr));
            return;
        }

        /* Debug reload (issue #170): under a live lldb-dap session, re-map the
         * cart's native module at a FRESH base + host-resolvable path so lldb
         * re-reads the new DWARF.  In run mode it stays at the native base with
         * no debugger to notify. */
        uint32_t base = 0u;
        char *reported = NULL;
#ifdef BLYT_GDB
        bool debug_reload = blyt_session_gdb_is_debugging(g_session);
        if (debug_reload) {
            base = blyt_session_next_reload_base(g_session);
            reported = blyt_js_cart_path(); /* host path lldb opens for DWARF (#144) */
        }
#endif
        if (!blyt_session_swap_cart(g_session, new_cart, base, reported)) {
            free(reported);
            blyt_cart_close(new_cart);
            if (snap)
                blyt_state_snapshot_free(snap);
            dev_ctrl_respond_err(id, "reload", "cart module swap failed");
            return;
        }
        blyt_cart_close(g_cart);
        g_cart = new_cart;

        size_t bc_size = 0;
        const void *bc = blyt_cart_find_section(g_cart, ".cart.lua", &bc_size);
        if (!bc) {
            free(reported);
            if (snap)
                blyt_state_snapshot_free(snap);
            dev_ctrl_respond_err(id, "reload", "reloaded cart has no Lua section");
            g_lua_fatal = true;
            return;
        }
        g_lua_bytecode = bc;
        g_lua_bytecode_size = bc_size;

#ifdef BLYT_GDB
        if (debug_reload) {
            /* Do NOT rename the OLD cart svr4 entry to the new path here (issue
             * #179): the two-phase re-arm ADDS the new module alongside the old
             * one, and lldb distinguishes them by `name=` (the host path).
             * reload_notify_begin already gives the NEW entry `reported`, and
             * phase 2's drop leaves the survivor at it; pre-renaming the old
             * entry made BOTH entries share the v2 path, so lldb saw two modules
             * with the same name, removed the stale breakpoint and never
             * re-resolved/continued onto the new code → the reload wedged. */
            blyt_session_gdb_reload_notify_begin(g_session, new_cart, base, reported);
            free(reported);
            /* Rebuild host Lua from the new bytecode (re-binding the native
             * lifecycle trampolines to the swapped module) but DEFER init() to
             * the loop's INIT phase so both init breakpoints fire; the state
             * restore + on_load_state(HOT_RELOAD) tail runs once that init()
             * completes.  Hand off to wasm_lua_loop: REARM drives the solib
             * re-arm, then INIT runs the reloaded init().  On failure rebuild
             * has already freed snap; on the deferred-success path it leaves
             * snap untouched, so the restore tail takes ownership below. */
            if (!wasm_lua_rebuild(true, snap, true /* defer_init */)) {
                dev_ctrl_respond_err(id, "reload", "rebuild failed");
                return;
            }
            g_reload_snap = snap; /* owned by the deferred restore tail */
            g_reload_phase = RELOAD_REARM;
            dev_ctrl_respond_ok(id, "reload");
            return;
        }
        free(reported); /* NULL in run mode; harmless */
#endif

        /* Run-mode reload: rebuild host Lua from the new bytecode, restoring the
         * pre-swap snapshot and replaying on_load_state(HOT_RELOAD).  rebuild()
         * takes ownership of snap and re-binds the native lifecycle trampolines
         * to the new module. */
        if (wasm_lua_rebuild(true, snap, false))
            dev_ctrl_respond_ok(id, "reload");
        else
            dev_ctrl_respond_err(id, "reload", "rebuild failed");
        return;
    }

    /* Pure-native: snapshot via the session (drives native on_save_state), swap
     * the cart module, boot the new init(), then restore over the fresh buffers
     * and notify the cart via on_load_state(HOT_RELOAD). */
    blyt_state_snapshot_t *snap = blyt_session_snapshot(g_session);
    blyt_cart_err_t cerr = blyt_cart_open("/cart.blyt", &new_cart);
    if (cerr != BLYT_CART_OK) {
        blyt_session_snapshot_free(snap);
        dev_ctrl_respond_err(id, "reload", blyt_cart_err_str(cerr));
        return;
    }

#ifdef BLYT_GDB
    /* Debug reload (issue #170): under a live lldb-dap session, re-map the cart
     * at a FRESH base + host-resolvable path (so lldb re-reads the new DWARF and
     * rebinds onto the new code) and drive the solib re-arm + reloaded init()
     * asynchronously under wasm_loop — the synchronous boot+restore below cannot
     * round-trip the debugger from inside this ccall.  Reply ok now; the loop
     * carries the reload to completion. */
    if (blyt_session_gdb_is_debugging(g_session)) {
        uint32_t base = blyt_session_next_reload_base(g_session);
        char *reported = blyt_js_cart_path(); /* host path lldb opens for DWARF (#144) */
        if (!blyt_session_swap_cart(g_session, new_cart, base, reported)) {
            free(reported);
            blyt_cart_close(new_cart);
            blyt_session_snapshot_free(snap);
            dev_ctrl_respond_err(id, "reload", "cart module swap failed");
            return;
        }
        blyt_cart_close(g_cart);
        g_cart = new_cart;
        /* Do NOT rename the OLD cart svr4 entry to the new path (issue #179):
         * keep the old and new entries distinct by host path so lldb re-resolves
         * the breakpoint onto the new module instead of seeing two same-named
         * modules and wedging.  reload_notify_begin gives the NEW entry
         * `reported`; phase 2's drop leaves the survivor at it. */
        blyt_session_gdb_reload_notify_begin(g_session, new_cart, base, reported);
        free(reported);
        g_reload_snap = snap; /* init()+restore run under wasm_loop after re-arm */
        g_reload_phase = RELOAD_REARM;
        dev_ctrl_respond_ok(id, "reload");
        return;
    }
#endif

    /* Run-mode reload: same base, synchronous boot + restore (no debugger). */
    if (!blyt_session_swap_cart(g_session, new_cart, 0u, NULL)) {
        blyt_cart_close(new_cart);
        blyt_session_snapshot_free(snap);
        dev_ctrl_respond_err(id, "reload", "cart module swap failed");
        return;
    }
    blyt_cart_close(g_cart);
    g_cart = new_cart;
    blyt_session_run_frame(g_session); /* runs the new cart's init() */
    blyt_session_restore(g_session, snap, 3u); /* BLYT_LOAD_HOT_RELOAD; frees snap */
    dev_ctrl_respond_ok(id, "reload");
}

/* Continue a reload after JS rewrote /cart.blyt in MEMFS (ADR-0045 sequence).
 * ok==0 means the JS-side refetch failed. */
EMSCRIPTEN_KEEPALIVE
void blyt_dev_ctrl_reload_fetched(int ok) {
    long id = g_dev_ctrl_reload_id;
    g_dev_ctrl_reload_id = -1;
    if (id < 0)
        return;

    if (!ok) {
        dev_ctrl_respond_err(id, "reload", "cart refetch failed");
        return;
    }

    /* Native/hybrid carts (g_session != NULL) swap the cart module in the live
     * session; pure-Lua fast-path carts rebuild the host-Lua VM below. */
    if (g_session) {
        wasm_session_reload(id);
        return;
    }

    /* Snapshot live state against the current layout before swapping carts. */
    blyt_state_ctx_t *sctx = active_state_ctx();
    blyt_state_snapshot_t *snap = NULL;
    if (sctx) {
        lua_getglobal(g_lua, "on_save_state");
        if (lua_isfunction(g_lua, -1))
            lua_pcall(g_lua, 0, 0, 0);
        else
            lua_pop(g_lua, 1);
        snap = blyt_state_ctx_snapshot(sctx);
    }

    /* Reopen the cart from the freshly fetched bytes. */
    blyt_cart_close(g_cart);
    g_cart = NULL;
    blyt_cart_err_t cerr = blyt_cart_open("/cart.blyt", &g_cart);
    if (cerr != BLYT_CART_OK) {
        if (snap)
            blyt_state_snapshot_free(snap);
        dev_ctrl_respond_err(id, "reload", blyt_cart_err_str(cerr));
        g_lua_fatal = true;
        return;
    }

    size_t bc_size = 0;
    const void *bc = blyt_cart_find_section(g_cart, ".cart.lua", &bc_size);
    if (!bc) {
        if (snap)
            blyt_state_snapshot_free(snap);
        dev_ctrl_respond_err(id, "reload", "reloaded cart has no Lua section");
        g_lua_fatal = true;
        return;
    }
    g_lua_bytecode = bc;
    g_lua_bytecode_size = bc_size;

    /* Reload the host-Lua resource table from the NEW cart before the reloaded
     * init() (in wasm_lua_rebuild) can read a resource (issue #246).  Entries are
     * zero-copy aliases into the cart map (resource.c e->data = body); the old
     * cart was just closed, so without this every entry dangles into freed memory
     * → a use-after-free / stale read on the first post-swap resource access.
     * Mirrors the native blyt_hostlua_reload (#244) and the update_assets sibling
     * above: clear → load_for_cart → re-apply persistent (#160) → preload →
     * publish footprint.  blyt_resource_table_clear frees only owned/src_path
     * buffers, never the old zero-copy aliases, so clearing after the old cart is
     * freed is safe. */
    if (g_lua_resources_loaded) {
        blyt_resource_table_clear(&g_lua_resources);
        blyt_resource_table_load_for_cart(&g_lua_resources, g_cart);
        blyt_resource_table_load_persistent_from_cart(&g_lua_resources, g_cart);
        blyt_resource_table_preload_persistent(&g_lua_resources);
        wasm_lua_publish_footprint(&g_lua_resources);
    }

    /* Rebuild from the new bytecode, restoring the pre-swap snapshot and
     * replaying on_load_state(HOT_RELOAD).  rebuild() takes ownership of snap.
     * Pure-Lua carts have no native (rv32) GDB breakpoints to re-arm, so the
     * synchronous rebuild is fine here — the debug-reload deferral (issue #170)
     * is only needed for hybrid carts (handled in wasm_session_reload). */
    if (wasm_lua_rebuild(true, snap, false))
        dev_ctrl_respond_ok(id, "reload");
    else
        dev_ctrl_respond_err(id, "reload", "rebuild failed");
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

#ifdef BLYT_GDB
    /* Debug reload (issue #170), phase 1: pump the async two-phase solib re-arm
     * before running the reloaded init().  Holding off init() until lldb has
     * re-read the new DWARF and rebound its breakpoints is what arms BOTH the
     * Lua and native init breakpoints before init() runs under the INIT phase
     * below — the reload-time equivalent of the startup armed-before-init gate. */
    if (g_reload_phase == RELOAD_REARM) {
#ifdef BLYT_DAP
        fc_dap_poll_messages(); /* keep the companion Lua DAP connection serviced */
#endif
        if (blyt_session_gdb_reload_notify_pump(g_session))
            g_reload_phase = RELOAD_INIT; /* re-armed → INIT phase runs init() */
        blyt_js_present(g_xrgb, BLYT_FRAME_W, BLYT_FRAME_H); /* hold the last frame */
        return;
    }
#endif

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
        lua_cleanup();
        /* Use exactly one keepalive mechanism here.  emscripten_cancel_main_loop()
         * makes the in-flight tick stale, so checkIsRunning() pops the runtime
         * keepalive once on return; emscripten_force_exit() instead clears the
         * keepalive counter to zero.  Doing both pops twice and aborts the debug
         * runtime at runtimeKeepalivePop (issue #102), so pick one:
         *   error  -> force_exit(1): exit code 1, single keepalive account.
         *   no err -> cancel: clean stop (exit code 0), as the done paths do. */
        if (g_lua_error)
            emscripten_force_exit(1);
        else
            emscripten_cancel_main_loop();
        return;
    }

    if (!g_lua_co)
        return;

    /* ---- INIT phase ---- */
    if (g_lua_phase == LUA_PHASE_INIT) {
        int nres = 0;
        int status = lua_resume(g_lua_co, g_lua, 0, &nres);
        if (status == LUA_OK) {
#ifdef BLYT_GDB
            /* Debug reload (issue #170), phase 2: the reloaded init() finished
             * (both init breakpoints having fired under the loop) — run the
             * deferred state restore + on_load_state(HOT_RELOAD) tail before the
             * cart starts its RUNNING frames. */
            if (g_reload_phase == RELOAD_INIT) {
                wasm_lua_reload_restore_tail();
                g_reload_phase = RELOAD_NONE;
            }
#endif
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
            g_lua_fatal = true;
            /* Tear down on the next tick via the g_lua_fatal branch, which keeps
             * the keepalive accounting balanced (issue #102). */
            blyt_js_present(g_xrgb, BLYT_FRAME_W, BLYT_FRAME_H);
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
    /* Reap the previous frame's off-screen surfaces (draw-scoped, #205) before
     * the new frame's draw creates fresh ones.  This is the once-per-real-frame
     * reap: a hybrid's surfaces live in the session's unified registry (#210), so
     * reap THAT — the run_frame reap is suppressed for the fast-path trampoline
     * (it fires once per C-export call, which would reap mid-frame).  A pure-Lua
     * cart reaps its local g_lua_surf pool. */
    if (g_session)
        blyt_session_reap_surfaces(g_session);
    else
        lua_surf_reap();
    /* Bump the tier-2 lock epoch once per real frame (#208): a lock held from a
     * previous frame is now stale (its surface may have been reaped/freed above),
     * so its get/set/release reject rather than touch freed memory. */
    g_lua_lock_epoch++;
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
        g_lua_fatal = true;
        /* Tear down on the next tick via the g_lua_fatal branch, which keeps
         * the keepalive accounting balanced (issue #102). */
        blyt_js_present(g_xrgb, BLYT_FRAME_W, BLYT_FRAME_H);
        return;
    }

    if (g_session) {
        /* Hybrid cart (#193): the native half (gfx ECALL handlers) and the
         * host-Lua half (lua_gfx_fb) draw into the session's ONE canonical
         * framebuffer — present + hash THAT so the native half's output is no
         * longer dropped on wasm.  Either half having drawn displaces the test
         * card (cart_has_drawn covers the native half; g_lua_drawn the Lua one). */
        if (g_lua_drawn || blyt_session_cart_has_drawn(g_session))
            lua_present_paletted(blyt_session_get_pixels(g_session),
                                 blyt_session_get_palette(g_session));
        else
            render_testcard();
    } else if (!g_lua_drawn) {
        render_testcard();
    } else {
        lua_present_paletted(g_lua_pixels, g_lua_gfx_palette);
    }

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

    /* The Lua VM allocates through the shared cart-heap arena so a pure-Lua cart's
     * guest_heap_used (and the 16 MB cap) matches rv32 byte-for-byte (#158). */
    wasm_lua_arena_reset();
    g_lua = lua_newstate(wasm_lua_alloc, NULL, luaL_makeseed(NULL));
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
    luaL_requiref(g_lua, LUA_UTF8LIBNAME, luaopen_utf8, 1);
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

    /* Host-Lua lifecycle phase mirror (#205): the running coroutine brackets
     * update()/draw() with these so a hybrid cart's native half stays
     * draw()-only through the gfx ECALL gate. */
    lua_pushcfunction(g_lua, lua_wasm_phase_update);
    lua_setglobal(g_lua, "__blyt_phase_update");
    lua_pushcfunction(g_lua, lua_wasm_phase_draw);
    lua_setglobal(g_lua, "__blyt_phase_draw");

    /* Create rv32emu session when the cart has native code.
     * For pure Lua carts with only .cart.layouts, use a lightweight state ctx
     * to avoid allocating the full 256MB RV32 emulator. */
    {
        size_t exports_sz = 0;
        g_has_lua_exports = (blyt_cart_find_section(g_cart, ".lua_exports", &exports_sz) != NULL);
        int has_native_lifecycle = blyt_cart_has_native_lifecycle(g_cart);
        int has_layouts = blyt_cart_has_layouts(g_cart);
        if (g_has_lua_exports || has_native_lifecycle) {
            /* Hybrid: the host-side Lua VM drives; the native half traps its Lua
             * C API through the ECALL bridge (ADR-0130).  Bridge mode resolves
             * .lua_exports host-side for the trampolines below.  On WASM the
             * bridge stub is already embedded as libblyt32lua.so, so no DT_NEEDED
             * remap is needed (that is the native-only path, #232). */
            g_session = blyt_session_create_lua_bridge(g_cart, wasm_log);
            if (!g_session) {
                blyt_js_error("hybrid session failed");
                lua_close(g_lua);
                g_lua = NULL;
                return 1;
            }
#ifdef BLYT_GDB
            {
                /* Report a host-resolvable cart path so lldb-dap can read the
                 * native (C) side's DWARF over the browser relay (issue #144). */
                char *cart_path = blyt_js_cart_path();
                if (cart_path) {
                    blyt_session_gdb_set_cart_path(g_session, cart_path);
                    free(cart_path);
                }
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
        /* Pure-Lua carts have no session run ctx to carry resources, so load a
         * host-side table the blyt.resource.* binding reads (#93).  Hybrid carts
         * use the session's ctx.resources (populated by blyt_session_create). */
        if (!g_session) {
            blyt_resource_table_init(&g_lua_resources);
            blyt_resource_table_load_for_cart(&g_lua_resources, g_cart);
            /* Persistent resources (#160): mark + pre-load before the cart's
             * init() runs, then publish the footprint so it is reserved from
             * frame 0 — the host-Lua mirror of blyt_session_create's preload. */
            blyt_resource_table_load_persistent_from_cart(&g_lua_resources, g_cart);
            if (blyt_resource_table_preload_persistent(&g_lua_resources) != 0) {
                blyt_js_error("persistent resources exceed the 16 MB budget (or failed to load)");
                blyt_resource_table_clear(&g_lua_resources);
                lua_close(g_lua);
                g_lua = NULL;
                return 1;
            }
            g_lua_resources_loaded = true;
            wasm_lua_publish_footprint(&g_lua_resources);
            /* Auto-load the cart's declared `palettes: default:` (#219), now the
             * resource table exists (a custom PROV_CART default resolves against
             * it) — the fast-path mirror of cart_run.c's pre-init auto-load. */
            lua_gfx_seed_declared_default();
        }
        /* Register state buffer + save/load + resource API. */
        if (active_state_ctx())
            wasm_register_state_api(g_lua, g_session);
        wasm_register_s_proxy(g_lua);
        wasm_register_resource_api(g_lua);
        wasm_register_gfx_api(g_lua); /* blyt32.gfx.* (#188) */
        wasm_register_mem_api(g_lua);
    }

    /* Load and execute the bytecode (single chunk or BLMC; registers bundled
     * require()-able modules such as cart_resources). */
    if (wasm_load_lua_bytecode(g_lua, (const unsigned char *)bytecode, bytecode_size) != 0) {
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

    /* Runtime-scaffolding baseline (#231): everything allocated so far — VM,
     * stdlibs, blyt/blyt32 API, S proxy, the loaded cart bytecode, and the INIT +
     * (later) RUNNING driver coroutines — is per-leg runtime overhead the native /
     * emulated legs don't share (they drive frames from C, not a Lua coroutine).
     * Excluding it makes cart_allocations + the 16 MB fail-point count only the
     * cart's own runtime allocations, byte-identical across legs.
     *
     * Collect first (#231): pin the baseline to the *settled* scaffolding, not a
     * snapshot still holding build-time transient garbage — the uncollected
     * remainder differs between legs and would bias the subtraction. Mirrors the
     * native runner (cart_run_hostlua.c). */
    lua_gc(g_lua, LUA_GCCOLLECT);
    g_lua_mem_acct.guest_heap_baseline = g_lua_mem_acct.guest_heap_used;

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

#ifdef BLYT_GDB
    /* Debug reload (issue #170), phase 1: pump the async two-phase solib re-arm.
     * Do NOT run the cart until lldb has re-read the new DWARF and rebound its
     * breakpoints, so an init() breakpoint binds to the new code before init
     * runs (the reload-time equivalent of the startup armed-before-init gate). */
    if (g_reload_phase == RELOAD_REARM) {
        if (blyt_session_gdb_reload_notify_pump(g_session))
            g_reload_phase = RELOAD_INIT; /* re-armed → run the reloaded init() */
        blyt_js_present(g_xrgb, BLYT_FRAME_W, BLYT_FRAME_H); /* hold the last frame */
        return;
    }
#endif

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

    /* Debug reload (issue #170), phase 2: the reloaded init() frame completed —
     * restore state over the fresh buffers and replay on_load_state(HOT_RELOAD),
     * the deferred tail of the swap.  Any init() breakpoint already round-tripped
     * via the GDB_PAUSED handling above across prior ticks. */
    if (g_reload_phase == RELOAD_INIT && err == BLYT_RUN_FRAME_DONE) {
        blyt_session_restore(g_session, g_reload_snap, 3u /* BLYT_LOAD_HOT_RELOAD */);
        g_reload_snap = NULL;
        g_reload_phase = RELOAD_NONE;
    }
#endif

    blyt_session_expand_frame(g_session, g_xrgb);

    if (err == BLYT_RUN_FRAME_DONE && g_reset_every_frame)
        blyt_reset_every_frame_cycle(g_session);

    /* Force-evict all evictable resources after the frame's reads so the next
     * frame rehydrates from scratch — the byte-identity oracle (#137). */
    if (err == BLYT_RUN_FRAME_DONE && g_evict_every_frame)
        blyt_session_resource_evict_all(g_session);

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
    g_evict_every_frame = (getenv("BLYT_RESOURCE_EVICT_EVERY_FRAME") != NULL);

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
        /* Report a host-resolvable cart path to lldb-dap before listening, so
         * cart breakpoints bind over the browser relay (issue #144). */
        char *cart_path = blyt_js_cart_path();
        if (cart_path) {
            blyt_session_gdb_set_cart_path(g_session, cart_path);
            free(cart_path);
        }
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
