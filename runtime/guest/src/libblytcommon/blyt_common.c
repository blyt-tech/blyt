/*
 * libblytcommon — shared RV32IMAFC library for variant-portable blyt_* API.
 *
 * Symbols here are declared in blyt.h and work identically on every console
 * variant.  Variant libraries link against this library and declare it as
 * DT_NEEDED; carts see the symbols through the variant library's DT_NEEDED
 * chain without listing libblytcommon.so directly.
 *
 * On emulated platforms ECALL stubs forward to the host runtime (ADR-0085).
 * On native RISC-V hardware these are real implementations.
 */

#include "blyt.h"

/* -------------------------------------------------------------------------
 * ECALL numbers (ADR-0085, range 1–49: lifecycle)
 * ------------------------------------------------------------------------- */

#define ECALL_CONSOLE_DEBUG 1

/* -------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

static unsigned int blytcommon_strlen(const char *s) {
    const char *p = s;
    while (*p)
        p++;
    return (unsigned int)(p - s);
}

/* -------------------------------------------------------------------------
 * blyt_console_debug — ADR-0085 ECALL stub (a0=ptr, a1=len)
 * ------------------------------------------------------------------------- */

void blyt_console_debug(const char *s) {
    register const char *a0 __asm__("a0") = s;
    register unsigned int a1 __asm__("a1") = blytcommon_strlen(s);
    register long a7 __asm__("a7") = ECALL_CONSOLE_DEBUG;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
}

/* -------------------------------------------------------------------------
 * blyt_quit_ready — set the quit flag; blyt_main exits its loop next tick
 * ------------------------------------------------------------------------- */

static int g_quit_requested = 0;

void blyt_quit_ready(void) {
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
 * what they need.  on_quit calls blyt_quit_ready() so unhandled quit
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
__attribute__((weak)) void blyt_cart_on_quit(void) {
    blyt_quit_ready();
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
