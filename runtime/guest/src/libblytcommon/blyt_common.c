/*
 * libblytcommon — shared RV32IMAFC library, cross-platform cart runtime.
 *
 * Contains only truly platform-independent logic: the cart lifecycle driver,
 * state management, and other code that is identical across all execution
 * targets (emulated, native, WASM, libretro).
 *
 * blyt_frame_done is the one libblytcommon symbol whose implementation is
 * variant-specific; it lives in its own TU (blyt_frame_done.c emulated /
 * frontends/native/src/libblytcommon/ native) so each variant links exactly
 * one definition while sharing the portable driver below (issue #128).
 *
 * Other platform-specific API implementations (blyt_console_debug, graphics,
 * audio, input) live in the variant library (libblyt32.so) which has separate
 * source trees for emulated and native paths.
 */

#include "blyt.h"

#include "blyt_phase.h" /* runtime/shared: lifecycle phase signal (#205) */
#include "blyt_runtime_flags.h" /* runtime/shared: host→guest runtime flags (#208) */

/* Host→guest runtime flags block (#208).  Lives in libblytcommon — the portable
 * runtime lib every cart loads — so the tier-2 Lua lock's guest binding (which
 * already depends on libblytcommon for blyt_mem_stats) resolves it on the native
 * dynamic-link path too, and the host resolves its guest address (symtab_lookup)
 * to write cart_is_debug once at session setup.  Exported (no version script on
 * libblytcommon); read with no ECALL to pick hard-error vs no-op on a bad
 * per-pixel access.  A per-build constant, so it does not affect determinism. */
blyt_runtime_flags_t blyt_runtime_flags = {0};

/* -------------------------------------------------------------------------
 * blyt_quit — set the quit flag; blyt_main exits its loop next tick
 * ------------------------------------------------------------------------- */

static int g_quit_requested = 0;

void blyt_quit(void) {
    g_quit_requested = 1;
}

/* Callable by the host (via blyt_session_begin_fn_call) to check whether
 * cart-native code called blyt_quit() inside a trampoline invocation.
 * The WASM frontend uses this to propagate the guest quit signal to the
 * Lua coroutine's blyt.should_quit() check after each lifecycle callback. */
int blyt_is_quit_requested(void) {
    return g_quit_requested;
}

/* -------------------------------------------------------------------------
 * blyt_frame_done — end-of-frame signal (ECALL 2 on the emulated path).
 *
 * Called by blyt_main after each blyt_cart_draw().  Its definition is
 * variant-specific and lives in its own TU (see the file header); declared
 * here for blyt_main's call below.
 * ------------------------------------------------------------------------- */

void blyt_frame_done(void);

/* -------------------------------------------------------------------------
 * Cart lifecycle entry point stubs (ADR-0087)
 *
 * Required callbacks (init/update/draw) have weak no-op stubs so that
 * libblytcommon.so builds cleanly on all platforms.  The load-time security
 * check (blyt_cart_open) verifies carts always override them with strong
 * definitions before execution begins.
 *
 * Optional callbacks also have weak no-op defaults; carts override only
 * what they need.  on_quit calls blyt_quit() so unhandled quit
 * requests exit the update/draw loop cleanly.
 * ------------------------------------------------------------------------- */

__attribute__((weak)) void blyt_cart_init(void) {
}
__attribute__((weak)) void blyt_cart_update(void) {
}
__attribute__((weak)) void blyt_cart_draw(void) {
}

__attribute__((weak)) void blyt_cart_on_new_state(void) {
}
__attribute__((weak)) void blyt_cart_on_save_state(void) {
}
__attribute__((weak)) void blyt_cart_on_load_state(blyt_load_info_t info) {
    (void)info;
}
__attribute__((weak)) void blyt_cart_on_quit(void) {
    blyt_quit();
}
__attribute__((weak)) void blyt_cart_cleanup(void) {
}

/* Dev-only asset hot-swap hook (issue #122).  Weak no-op so carts override only
 * if they cache/derive from resources; never fires in a shipped cart. */
__attribute__((weak)) void blyt_cart_on_assets_reloaded(const uint32_t *ids, size_t n) {
    (void)ids;
    (void)n;
}

/* -------------------------------------------------------------------------
 * blyt_main — runtime-owned lifecycle driver (ADR-0087)
 *
 * The runtime sets the emulator's initial PC to this function after the
 * dynamic loader has resolved all PLT/GOT entries.  It calls the cart's
 * required entry points (resolved from the cart's dynsym) and manages the
 * update/draw loop.
 * ------------------------------------------------------------------------- */

void blyt_main(void) {
    g_quit_requested = 0;

    /* Phase signal (#205): tell the runtime which lifecycle callback is running
     * so it can make all surface access draw()-only.  Two per frame — the phase
     * stays put until the next enter, so update() runs at UPDATE and everything
     * up to the next update (draw, frame_done) at DRAW.  init/on_new_state run
     * at INIT.  Only DRAW permits surface ops. */
    blyt_phase_enter(BLYT_PHASE_INIT);
    blyt_cart_init();
    blyt_cart_on_new_state();

    while (!g_quit_requested) {
        /* blyt_frame_done() fires ECALL 2 after draw; the host handles it
         * (SDL events, frame-rate cap, etc.) then resumes the emulator. */
        blyt_phase_enter(BLYT_PHASE_UPDATE);
        blyt_cart_update();
        blyt_phase_enter(BLYT_PHASE_DRAW);
        blyt_cart_draw();
        blyt_frame_done();
    }

    blyt_cart_on_quit();
    blyt_cart_cleanup();
}
