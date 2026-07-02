#pragma once

#include <stdint.h>

/* Draw the PM5544-style test card for the given frame into pixels_out as
 * palette indices.  The test card carries no palette of its own (#204): it
 * holds reference RGB targets and renders each as the nearest index in the
 * active `palette` (256 XRGB8888 entries, the cart-declared palette per
 * ADR-0088, else the runtime default), so the bars/wedge track whatever the
 * cart declared.  pixels_out must hold BLYT_FRAME_W * BLYT_FRAME_H bytes.
 * Called by the session on every FRAME_DONE until the cart draws. */
void blyt_testcard_draw(uint32_t frame_count, const uint32_t *palette, uint8_t *pixels_out);
