/*
 * blyt_raster — integer 2D rasterizer core (issue #188, Spike X).
 *
 * Integer-only, no floating point, no libc beyond memset/memcpy (ADR-0008/0052,
 * high-level-design §5).  See blyt_raster.h for the rationale.
 */

#include "blyt_raster.h"

#include <stddef.h>
#include <string.h>

void blyt_raster_clear(uint8_t *fb, int stride, int width, int height, uint8_t color) {
    if (!fb || width <= 0 || height <= 0 || stride <= 0)
        return;
    for (int y = 0; y < height; y++)
        memset(fb + (size_t)y * (size_t)stride, color, (size_t)width);
}

void blyt_raster_pixel(uint8_t *fb, int stride, int width, int height, int x, int y,
                       uint8_t color) {
    if (!fb || stride <= 0)
        return;
    if (x < 0 || x >= width || y < 0 || y >= height)
        return;
    fb[(size_t)y * (size_t)stride + (size_t)x] = color;
}

void blyt_raster_rect_fill(uint8_t *fb, int stride, int width, int height, int x, int y, int w,
                           int h, uint8_t color) {
    if (!fb || stride <= 0 || w <= 0 || h <= 0)
        return;
    /* 64-bit intermediates: x+w / y+h can overflow int for extreme coords. */
    int64_t x0 = x, y0 = y;
    int64_t x1 = (int64_t)x + (int64_t)w, y1 = (int64_t)y + (int64_t)h;
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > width)
        x1 = width;
    if (y1 > height)
        y1 = height;
    if (x1 <= x0 || y1 <= y0)
        return;
    for (int64_t row = y0; row < y1; row++)
        memset(fb + (size_t)row * (size_t)stride + (size_t)x0, color, (size_t)(x1 - x0));
}

void blyt_raster_blit(uint8_t *dst, int dstride, int dwidth, int dheight, const uint8_t *src,
                      int sstride, int swidth, int sheight, int x, int y) {
    if (!dst || !src || dstride <= 0 || sstride <= 0 || swidth <= 0 || sheight <= 0)
        return;
    /* Destination window, clipped to dst; 64-bit so x+swidth can't overflow. */
    int64_t dx0 = x, dy0 = y;
    int64_t dx1 = (int64_t)x + (int64_t)swidth, dy1 = (int64_t)y + (int64_t)sheight;
    /* Source offset for the pixels the left/top clip skipped. */
    int64_t sox = 0, soy = 0;
    if (dx0 < 0) {
        sox = -dx0;
        dx0 = 0;
    }
    if (dy0 < 0) {
        soy = -dy0;
        dy0 = 0;
    }
    if (dx1 > dwidth)
        dx1 = dwidth;
    if (dy1 > dheight)
        dy1 = dheight;
    if (dx1 <= dx0 || dy1 <= dy0)
        return;
    size_t run = (size_t)(dx1 - dx0);
    for (int64_t row = dy0; row < dy1; row++) {
        int64_t srow = soy + (row - dy0);
        memcpy(dst + (size_t)row * (size_t)dstride + (size_t)dx0,
               src + (size_t)srow * (size_t)sstride + (size_t)sox, run);
    }
}

void blyt_raster_line(uint8_t *fb, int stride, int width, int height, int x0, int y0, int x1,
                      int y1, uint8_t color) {
    if (!fb || stride <= 0)
        return;
    /* Integer Bresenham (no libc abs).  dx/-dy are non-negative magnitudes; the
     * error term steps one axis per iteration.  Per-pixel clipping keeps the
     * walk on the true infinite-canvas line while only writing visible pixels,
     * so off-screen endpoints still produce the correct on-screen segment. */
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        blyt_raster_pixel(fb, stride, width, height, x0, y0, color);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}
