#pragma once

#include <stdint.h>

/*
 * blyt_palettes — the four runtime-bundled built-in palettes (ADR-0042,
 * issue #201).
 *
 * Each is a 256-entry XRGB8888 table (top byte 0), addressed by a console-wide
 * tagged resource constant (blyt_handle.h) with runtime provenance:
 *
 *   aurora — DawnBringer "Aurora" (256), the console default.  Index 255 is
 *            the ADR-0049 transparency sacrificial slot.  See
 *            docs/contributing/third-party.md for sourcing/attribution.
 *   vga    — the standard VGA 256-color default DAC palette (16 EGA colors +
 *            16-step grayscale ramp + 216-color HSV cube; public domain PC
 *            standard).
 *   ega    — the standard 16-color EGA palette in indices 0-15; 16-255 are
 *            black (a sacrificial index 255 falls out of the padding).
 *   cga    — CGA palette 1, high intensity (black/cyan/magenta/white) in
 *            indices 0-3; 4-255 are black.
 *
 * This is a hand-authored static table, not the general runtime-shipped
 * resource registry (ADR-0134 defers that) -- proportionate to four fixed
 * assets. blyt_builtin_palette is the single resolver every leg (host, wasm,
 * native) calls to turn a BLYT_PALETTE_* handle into bytes.
 */

/* Packer-id space for built-in palettes (bits 23-0 of the encoded handle).
 * Stable once shipped -- these are baked into carts via BLYT_PALETTE_*. */
enum {
    BLYT_PAL_ID_AURORA = 1,
    BLYT_PAL_ID_VGA = 2,
    BLYT_PAL_ID_EGA = 3,
    BLYT_PAL_ID_CGA = 4,
};

/* Resolve a console-wide tagged handle to its 256-entry XRGB8888 table.
 * Returns NULL if the handle is not a RESOURCE-kind, RUNTIME-provenance
 * handle, or its id names no built-in palette. */
const uint32_t *blyt_builtin_palette(uint32_t handle);

/* Return the index of the palette entry closest to XRGB target `rgb`, measured
 * by squared-Euclidean distance in 8-bit RGB (the top/alpha byte is ignored).
 * Ties resolve to the lowest index, so the result is a pure, deterministic
 * function of (palette, rgb) -- identical across every leg (host/wasm/native),
 * which the testcard's palette-agnostic remap (#204) relies on.  `palette`
 * points at 256 XRGB8888 entries. */
uint8_t blyt_palette_nearest(const uint32_t *palette, uint32_t rgb);
