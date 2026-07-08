#pragma once

/*
 * blyt_hostlua.h — native host-Lua fast path (#238, epic #230, ADR-0136).
 *
 * A SIBLING runner to blyt_session_*: it runs a PURE-LUA cart's bytecode in a
 * Lua VM compiled natively for the host (x86-64 / arm64) via the deterministic
 * seam VM (cmake/blyt_hostlua_vm.cmake, BLYT_HOSTLUA_FP_SEAM), instead of the
 * RV32 Lua VM under rv32emu.  This is the native port of the WASM host-Lua fast
 * path (frontends/wasm/wasm_main.c run_lua_cart); native cart code and the
 * native half of a hybrid cart keep using rv32emu (blyt_session_*).
 *
 * Opt-in (issue #238): the frontend routes a cart here only when BLYT_HOSTLUA is
 * set in the environment AND the cart is pure Lua.  #236 flips the default and
 * removes the gate.  The whole path is compiled only when the deterministic seam
 * VM is available (BLYT_HOSTLUA_EXEC, set on libblyt by CMake); on a build
 * without it every entry point below degrades to a safe no-op / NULL so the
 * frontend transparently falls back to the rv32 session.
 *
 * Lifecycle mirrors the guest blyt_main loop (runtime/guest .../blyt_common.c):
 *   create  → init(); on_new_state()
 *   run_frame (while !quit) → update(); draw()      [one frame per call]
 *   on quit → on_quit(); cleanup()                  [driven by run_frame]
 * one blyt_hostlua_run_frame() == one blyt_session_run_frame() (ADR-0087).
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#include "blyt_runtime.h" /* blyt_cart_t, blyt_log_fn, blyt_cart_run_err_t */

/* Opaque native host-Lua runner state (one per cart run). */
typedef struct blyt_hostlua blyt_hostlua_t;

/*
 * True when this build carries the deterministic seam VM, i.e. the native
 * host-Lua execution path is present (BLYT_HOSTLUA_EXEC).  False otherwise —
 * blyt_hostlua_create() then always returns NULL.
 */
bool blyt_hostlua_available(void);

/*
 * The frontend's opt-in dispatch predicate: true iff the native host-Lua path
 * should run `cart` — the runner is compiled in (blyt_hostlua_available()), the
 * BLYT_HOSTLUA environment gate is set, and `cart` is PURE Lua (has a .cart.lua
 * section, defines no cart-native lifecycle symbol, and has no .lua_exports).
 * A native / hybrid cart returns false and keeps the rv32 session path.
 */
bool blyt_hostlua_should_use(const blyt_cart_t *cart);

/*
 * Create a native host-Lua runner for `cart` and run its init() + on_new_state()
 * (the boot phase of the guest blyt_main loop).  `log_fn` receives the cart's
 * blyt.debug.print / blyt32.debug.print lines (one call per line, no trailing
 * newline — same channel as blyt_console_debug on the emulated path).  Returns
 * NULL on failure (VM alloc, bytecode load/parse, or an error in init()) or when
 * the native host-Lua path is not compiled in.  The cart must outlive the runner.
 */
blyt_hostlua_t *blyt_hostlua_create(blyt_cart_t *cart, blyt_log_fn log_fn);

/*
 * Run one frame: update() then draw(), mirroring one iteration of the guest
 * blyt_main loop.  Returns BLYT_RUN_FRAME_DONE when a frame completed and the
 * cart is still running; BLYT_RUN_OK once the cart has requested quit (the final
 * call also runs on_quit() + cleanup()); or an error code (BLYT_RUN_ERR_ABORT)
 * if a lifecycle callback raised a Lua error.  Quit is tested at the top of the
 * call, so a quit requested during update() still runs that frame's draw() — the
 * same ordering as blyt_main's `while (!g_quit_requested)`.
 */
blyt_cart_run_err_t blyt_hostlua_run_frame(blyt_hostlua_t *hl);

/* Destroy a runner and free its VM.  NULL-safe. */
void blyt_hostlua_destroy(blyt_hostlua_t *hl);

#ifdef __cplusplus
}
#endif
