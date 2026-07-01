#pragma once

#include <stdint.h>

/*
 * blyt_raster — Spike X integer 2D rasterizer core (issue #188).
 *
 * The paletted 2D model is Blyt32-specific (ADR-0086), but the rasterizer is a
 * freestanding determinism-critical primitive shared between the host runtime's
 * graphics ECALL handlers and the native libblyt32 variant — so it lives in
 * runtime/shared alongside the other single-source determinism cores (#158).
 *
 * Option A (the decision this spike validates): the body is integer / fixed
 * point only — no floating point — so the *same source* compiled into the host
 * runtime, the wasm module, and native RV32 produces bit-identical pixels and
 * cannot be miscompiled into divergence by a dropped -ffp-contract flag on one
 * target (ADR-0008/0052, high-level-design §5). Build with -fwrapv; use only
 * fixed-width types and memset/memcpy.
 *
 * Every primitive operates on a caller-owned paletted back buffer (one byte per
 * pixel, the palette index) addressed by (fb, stride) so the identical code
 * serves both the host's session->pixels[] and a guest-side buffer.
 */

/* All primitives clip to the [0,width) x [0,height) bounds of fb and are
 * no-ops on a NULL buffer / non-positive stride.  Coordinates are signed and
 * may be negative or off-screen (the torture frame exercises this); clipping is
 * computed with 64-bit intermediates so extreme coordinates never overflow. */

/* Fill the width x height rectangle at the top-left of fb with `color`.
 * Used for whole-frame clears (stride == width == BLYT_FRAME_W). */
void blyt_raster_clear(uint8_t *fb, int stride, int width, int height, uint8_t color);

/* Set a single pixel (no-op if (x,y) is off-screen). */
void blyt_raster_pixel(uint8_t *fb, int stride, int width, int height, int x, int y, uint8_t color);

/* Fill the w x h rectangle whose top-left is (x,y).  Top/left edges inclusive,
 * bottom/right exclusive; clipped to the framebuffer.  No-op if w<=0 or h<=0. */
void blyt_raster_rect_fill(uint8_t *fb, int stride, int width, int height, int x, int y, int w,
                           int h, uint8_t color);

/* Draw a line from (x0,y0) to (x1,y1) inclusive of both endpoints, integer
 * Bresenham, clipped per-pixel to the framebuffer. */
void blyt_raster_line(uint8_t *fb, int stride, int width, int height, int x0, int y0, int x1,
                      int y1, uint8_t color);

/* Copy the swidth x sheight src buffer into dst with its top-left at (x,y),
 * clipped to dst's [0,dwidth) x [0,dheight) bounds.  Plain palette-index copy
 * (the palette is global); no transparency.  No-op on NULL buffers or when the
 * clipped destination window is empty.  Coordinates may be negative / off-screen
 * (clipped with 64-bit intermediates). */
void blyt_raster_blit(uint8_t *dst, int dstride, int dwidth, int dheight, const uint8_t *src,
                      int sstride, int swidth, int sheight, int x, int y);
