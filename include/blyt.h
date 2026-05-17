#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Cart lifecycle entry points (ADR-0087)
 *
 * Required — the runtime verifies all three are present at cart load time.
 * The cart defines these; libblytcommon.so's blyt_main calls them in order:
 *   init → on_new_state → [update → draw] loop → on_quit → cleanup
 * ------------------------------------------------------------------------- */
void blyt_cart_init(void);
void blyt_cart_update(void);
void blyt_cart_draw(void);

/* Optional — libblytcommon.so provides weak no-op defaults for these. */
void blyt_cart_on_new_state(void);
void blyt_cart_on_save_state(void);
void blyt_cart_on_quit(void);
void blyt_cart_cleanup(void);

/* -------------------------------------------------------------------------
 * Cart signals to the runtime (ADR-0087)
 * ------------------------------------------------------------------------- */

/* Signal that the cart is ready to exit the update/draw loop.
 * Call from blyt_cart_on_quit, or directly from blyt_cart_update when the
 * cart decides it is finished (e.g. after showing a credits sequence). */
void blyt_quit_ready(void);

/* -------------------------------------------------------------------------
 * Debug output (ADR-0085, ECALL 1)
 * ------------------------------------------------------------------------- */
void blyt_console_debug(const char *s);

#ifdef __cplusplus
}
#endif
