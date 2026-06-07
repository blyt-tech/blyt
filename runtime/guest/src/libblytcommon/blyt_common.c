/*
 * libblytcommon — shared RV32IMAFC library, cross-platform cart runtime.
 *
 * Contains only truly platform-independent logic: the cart lifecycle driver,
 * state management, and other code that is identical across all execution
 * targets (emulated, native, WASM, libretro).
 *
 * Platform-specific API implementations (blyt_console_debug, blyt_frame_done,
 * graphics, audio, input) live in the variant library (libblyt32.so) which
 * has separate source trees for emulated and native paths.
 */

#include "blyt.h"

/* -------------------------------------------------------------------------
 * blyt_quit — set the quit flag; blyt_main exits its loop next tick
 * ------------------------------------------------------------------------- */

static int g_quit_requested = 0;

void blyt_quit(void) {
    g_quit_requested = 1;
}

/* -------------------------------------------------------------------------
 * blyt_frame_done — end-of-frame signal (ECALL 2)
 *
 * Called by blyt_main after each blyt_cart_draw().  The host intercepts this
 * ECALL, runs its frame callback (SDL event polling, frame-rate cap, etc.),
 * then resumes the emulator for the next frame without halting it.
 * ------------------------------------------------------------------------- */

void blyt_frame_done(void) {
    register long a7 __asm__("a7") = 2; /* BLYT_ECALL_FRAME_DONE */
    __asm__ volatile("ecall" : : "r"(a7) : "memory");
}

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

    blyt_cart_init();
    blyt_cart_on_new_state();

    while (!g_quit_requested) {
        /* blyt_frame_done() fires ECALL 2 after draw; the host handles it
         * (SDL events, frame-rate cap, etc.) then resumes the emulator. */
        blyt_cart_update();
        blyt_cart_draw();
        blyt_frame_done();
    }

    blyt_cart_on_quit();
    blyt_cart_cleanup();
}
