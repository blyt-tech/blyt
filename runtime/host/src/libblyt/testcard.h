#pragma once

#include <stdint.h>

/* Fill palette_out[256] with the test card's XRGB8888 palette.
 * Called once when a session is created. */
void blyt_testcard_init_palette(uint32_t *palette_out);

/* Draw the PM5544-style test card for the given frame into pixels_out as
 * palette indices.  pixels_out must hold BLYT_FRAME_W * BLYT_FRAME_H bytes.
 * Called by the session on every FRAME_DONE until the cart draws. */
void blyt_testcard_draw(uint32_t frame_count, uint8_t *pixels_out);
