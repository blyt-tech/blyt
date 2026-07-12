#pragma once

/*
 * blyt_hostlua.h — native host-Lua fast path (#238, epic #230, ADR-0136).
 *
 * A SIBLING runner to blyt_session_*: it runs a cart's Lua bytecode in a Lua VM
 * compiled natively for the host (x86-64 / arm64) via the deterministic seam VM
 * (cmake/blyt_hostlua_vm.cmake, BLYT_HOSTLUA_FP_SEAM), instead of the RV32 Lua VM
 * under rv32emu.  This is the native port of the WASM host-Lua fast path
 * (frontends/wasm/wasm_main.c run_lua_cart); native cart code and the native half
 * of a hybrid cart keep using rv32emu (blyt_session_*), bridged via the ADR-0130
 * ECALL Lua C API for hybrids.
 *
 * Default path (ADR-0136, #236): the frontend routes a PURE-LUA cart here by
 * default on every non-RISC-V host — the emulated RV32 Lua VM is retired as a
 * shipped path for that case. A HYBRID cart (native .lua_exports and/or a
 * cart-native lifecycle) stays emulated by default and reaches this path only via
 * the BLYT_HOSTLUA opt-in, so `blyt debug` on a hybrid keeps native-half GDB/lldb
 * on the rv32 session (host-Lua native-half debug is deferred to #251). The whole
 * path is compiled only when the deterministic seam VM is available
 * (BLYT_HOSTLUA_EXEC, set on libblyt by CMake); on a build without it (e.g. real
 * RISC-V hardware, where carts run native RV32) every entry point below degrades to
 * a safe no-op / NULL so the frontend transparently uses the rv32 session.
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
 * The frontend's dispatch predicate (ADR-0136): true iff the native host-Lua path
 * should run `cart` — the runner is compiled in (BLYT_HOSTLUA_EXEC) and `cart` has
 * a .cart.lua section (pure-Lua OR hybrid-Lua; a hybrid's native half runs
 * emulated via the ADR-0130 bridge).  A pure-native cart (no .cart.lua) returns
 * false and keeps the rv32 session path.  On a build without the seam VM the
 * #else stub returns false, so real RISC-V hardware runs native RV32.
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
 * Debug variant of blyt_hostlua_create (BLYT_DAP builds only): build the VM and
 * arm the DAP master hook on it, but DO NOT run init()/on_new_state() yet — the
 * boot is deferred to blyt_hostlua_dap_wait_ready() so a breakpoint set in
 * init() can fire.  Mirrors blyt_session_create + the debug-gated boot on the
 * emulated path.  Returns NULL on failure or in a build without DAP / the seam
 * VM.  Pair with blyt_hostlua_dap_listen() (start the server) then
 * blyt_hostlua_dap_wait_ready() (gate + boot).
 */
blyt_hostlua_t *blyt_hostlua_create_debug(blyt_cart_t *cart, blyt_log_fn log_fn);

/*
 * Start the native host-Lua DAP server (TCP, 127.0.0.1:port; port 0 =
 * OS-assigned).  Writes the actual bound port to *actual_port when non-NULL.
 * Returns 0 on success, -1 on failure or a runner not created for debug.  The
 * host-Lua analog of blyt_session_dap_listen; the frontend reports the port.
 */
int blyt_hostlua_dap_listen(blyt_hostlua_t *hl, int *actual_port);

/*
 * Block until the DAP client sends configurationDone, then run the deferred
 * boot phase — init() + on_new_state() — under the armed master hook, so any
 * breakpoint in init() pauses.  Returns non-zero once the cart has booted; 0 if
 * the server shut down / the client never connected.  The host-Lua analog of
 * blyt_session_dap_wait_ready, called from the player's main loop before the
 * frame loop.  No-op returning 0 for a runner not created for debug.
 */
int blyt_hostlua_dap_wait_ready(blyt_hostlua_t *hl);

/*
 * Restart the debug session (issue #257): re-launch the cart from scratch by
 * tearing the VM down and rebuilding it fresh (no state preserved — a DAP restart
 * is a re-run, not a hot reload), with init() deferred so a breakpoint in init()
 * fires after the client re-sends configurationDone.  Called by the frontend when
 * blyt_hostlua_run_frame returns BLYT_RUN_RESTART; pair with a following
 * blyt_hostlua_dap_wait_ready to gate + boot the fresh VM.  Returns 0 on success,
 * -1 on failure or a runner not created for debug.  The host-Lua analog of
 * retro_reset + blyt_session_dap_reattach on the emulated restart path.
 */
int blyt_hostlua_dap_restart(blyt_hostlua_t *hl);

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

/*
 * The host-Lua framebuffer for presentation (#231).  The runner rasterizes
 * blyt32.gfx.* into its own paletted back buffer (there is no session), so the
 * frontend presents by expanding these directly: get_pixels() returns the
 * BLYT_FRAME_W * BLYT_FRAME_H paletted bytes, get_palette() the active 256-entry
 * XRGB8888 palette.  Both return NULL for a NULL runner or a build without the
 * seam VM.  Valid after each blyt_hostlua_run_frame(); the buffers live for the
 * runner's lifetime.
 */
const uint8_t *blyt_hostlua_get_pixels(blyt_hostlua_t *hl);
const uint32_t *blyt_hostlua_get_palette(blyt_hostlua_t *hl);

/*
 * Run one --reset-every-frame save/clear/restore stress cycle (save-state
 * determinism testing): flush transient state via on_save_state(), snapshot +
 * zero the state buffers, rebuild the VM (all Lua globals wiped), re-run init(),
 * restore the snapshot, and replay on_load_state(HOT_RELOAD).  Mirrors
 * blyt_reset_every_frame_cycle on the emulated path; a cart's output must be
 * identical with and without it.  No-op after the cart has quit.
 */
void blyt_hostlua_reset_every_frame_cycle(blyt_hostlua_t *hl);

/*
 * Hot-reload the running cart against a freshly built image (#244, epic #230).
 * `new_cart` is a distinct, already-open cart handle (the rebuilt bytecode +
 * resources); the runner snapshots live state from the current VM, re-points its
 * cart/bytecode/resource table at `new_cart`, rebuilds the VM (re-running init()
 * and — in a debug build with the DAP hook armed — re-arming breakpoints so an
 * init() breakpoint re-fires), then restores the snapshot and replays
 * on_load_state(HOT_RELOAD).  The native counterpart of the WASM pure-Lua
 * fast-path reload (wasm_main.c blyt_dev_ctrl_reload_fetched) and of the emulated
 * path's reload_impl.
 *
 * The runner keeps its resource table pointed at `new_cart` (resources alias the
 * cart map zero-copy), so the caller MUST keep the old cart handle valid until
 * this returns, then may close it and adopt `new_cart` as the live cart.
 *
 * Returns true on success (VM rebuilt against `new_cart`, state restored); false
 * without disturbing the live VM if `new_cart` lacks a .cart.lua section, or —
 * after marking the runner done — if the VM rebuild failed.  No-op returning
 * false for a NULL/finished runner or a build without the seam VM.
 */
bool blyt_hostlua_reload(blyt_hostlua_t *hl, blyt_cart_t *new_cart);

/*
 * Dev-mode asset hot-swap (issue #118/#122): re-read the resource table from
 * `cart` (picking up edited bytes from the dev staging dir) WITHOUT rebuilding the
 * VM, then fire the cart's optional Lua `on_assets_reloaded(ids)` global with the
 * `n` changed resource `ids`.  The session-less mirror of
 * blyt_session_reload_resources + blyt_session_notify_assets_reloaded, used by the
 * libretro core's `update_assets` command when a host-Lua runner is active.
 * Returns false on reload failure or for a NULL/finished runner / seam-VM-less
 * build.
 */
bool blyt_hostlua_update_assets(blyt_hostlua_t *hl, blyt_cart_t *cart, const uint32_t *ids,
                                size_t n);

/*
 * Host-initiated state save/load to a disk slot (dev-control `save_state` /
 * `load_state`) — the session-less mirrors of blyt_session_save_state /
 * blyt_session_load_state.  save_state flushes transient state via on_save_state
 * then serialises the state buffers; load_state reads the slot then notifies
 * on_load_state(reason=SAVE_GAME).  Return 0 on success, non-zero on failure (incl.
 * a NULL/seam-VM-less runner).
 */
int blyt_hostlua_save_state(blyt_hostlua_t *hl, uint32_t slot);
int blyt_hostlua_load_state(blyt_hostlua_t *hl, uint32_t slot);

/* Destroy a runner and free its VM.  NULL-safe. */
void blyt_hostlua_destroy(blyt_hostlua_t *hl);

#ifdef __cplusplus
}
#endif
