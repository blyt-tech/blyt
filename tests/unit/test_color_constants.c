/*
 * test_color_constants — pins the named color-index constants shipped in the
 * SDK header (runtime/guest/include/blyt.h, ADR-0059, issue #203) against the
 * actual bytes of the bundled palettes (runtime/shared/blyt_palettes.c).
 *
 * The constants are plain compile-time uint8 palette *indices* (not tagged
 * resource handles). The value of pinning them here is the cross-check: a typo
 * in BLYT_AURORA_RED's index would still compile and still "draw a color" in a
 * cart, so a green cart test could not catch it. This test asserts the index
 * each constant names really is the intended color in that palette's table.
 *
 * Includes the guest blyt.h directly: it is declarations-only for the constants
 * we read, so a host build links with no guest symbols (we never call the API).
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

/* Shared headers first: blyt_handle.h defines BLYT_SCREEN unconditionally,
 * while blyt.h guards it with #ifndef -- this order avoids a redefinition
 * warning (same value, different spelling) when both are pulled in. */
#include "blyt_handle.h"
#include "blyt_palettes.h"

#include <blyt.h>

static const uint32_t *pal(uint32_t id) {
    const uint32_t *p = blyt_builtin_palette(blyt_resource_encode(id, BLYT_RESOURCE_PROV_RUNTIME));
    assert(p != 0);
    return p;
}

/* EGA/VGA share the canonical 16 (vga's low 16 ARE the EGA 16), so the two
 * constant sets must be numerically identical and both index the EGA colors. */
static void test_ega_vga_canonical_16(void) {
    const uint32_t *ega = pal(BLYT_PAL_ID_EGA);
    const uint32_t *vga = pal(BLYT_PAL_ID_VGA);

    /* The constants ARE the indices 0..15, in EGA's canonical order. */
    assert(BLYT_EGA_BLACK == 0 && BLYT_EGA_BLUE == 1 && BLYT_EGA_GREEN == 2 && BLYT_EGA_CYAN == 3 &&
           BLYT_EGA_RED == 4 && BLYT_EGA_MAGENTA == 5 && BLYT_EGA_BROWN == 6 &&
           BLYT_EGA_LTGRAY == 7 && BLYT_EGA_DKGRAY == 8 && BLYT_EGA_BR_BLUE == 9 &&
           BLYT_EGA_BR_GREEN == 10 && BLYT_EGA_BR_CYAN == 11 && BLYT_EGA_BR_RED == 12 &&
           BLYT_EGA_BR_MAGENTA == 13 && BLYT_EGA_BR_YELLOW == 14 && BLYT_EGA_WHITE == 15);

    /* VGA aliases the same indices. */
    assert(BLYT_VGA_BLACK == BLYT_EGA_BLACK && BLYT_VGA_WHITE == BLYT_EGA_WHITE &&
           BLYT_VGA_BROWN == BLYT_EGA_BROWN && BLYT_VGA_BR_YELLOW == BLYT_EGA_BR_YELLOW);

    /* And the named index really is that color in both tables. */
    assert(ega[BLYT_EGA_BLACK] == 0x00000000u && vga[BLYT_VGA_BLACK] == 0x00000000u);
    assert(ega[BLYT_EGA_RED] == 0x00AA0000u && vga[BLYT_VGA_RED] == 0x00AA0000u);
    assert(ega[BLYT_EGA_BROWN] == 0x00AA5500u); /* EGA quirk: 6 is brown, not dk-yellow */
    assert(ega[BLYT_EGA_LTGRAY] == 0x00AAAAAAu);
    assert(ega[BLYT_EGA_BR_YELLOW] == 0x00FFFF55u);
    assert(ega[BLYT_EGA_WHITE] == 0x00FFFFFFu && vga[BLYT_VGA_WHITE] == 0x00FFFFFFu);
}

/* Aurora uses the nearest-to-EGA index per #203's distance table; the grays and
 * black/white land exact, the chromatics are hue-shifted approximations. Assert
 * every one of the 16 names indexes the exact byte the table promises. */
static void test_aurora_nearest_to_ega(void) {
    const uint32_t *a = pal(BLYT_PAL_ID_AURORA);

    assert(BLYT_AURORA_BLACK == 0 && a[BLYT_AURORA_BLACK] == 0x00000000u);
    assert(BLYT_AURORA_BLUE == 223 && a[BLYT_AURORA_BLUE] == 0x000010BDu);
    assert(BLYT_AURORA_GREEN == 185 && a[BLYT_AURORA_GREEN] == 0x00149605u);
    assert(BLYT_AURORA_CYAN == 195 && a[BLYT_AURORA_CYAN] == 0x0006C491u);
    assert(BLYT_AURORA_RED == 155 && a[BLYT_AURORA_RED] == 0x00A5140Au);
    assert(BLYT_AURORA_MAGENTA == 239 && a[BLYT_AURORA_MAGENTA] == 0x00BD10C5u);
    assert(BLYT_AURORA_BROWN == 165 && a[BLYT_AURORA_BROWN] == 0x00B45A00u);
    assert(BLYT_AURORA_LTGRAY == 10 && a[BLYT_AURORA_LTGRAY] == 0x00AAAAAAu);
    assert(BLYT_AURORA_DKGRAY == 5 && a[BLYT_AURORA_DKGRAY] == 0x00555555u);
    assert(BLYT_AURORA_BR_BLUE == 219 && a[BLYT_AURORA_BR_BLUE] == 0x004A5AFFu);
    assert(BLYT_AURORA_BR_GREEN == 189 && a[BLYT_AURORA_BR_GREEN] == 0x004BF05Au);
    assert(BLYT_AURORA_BR_CYAN == 201 && a[BLYT_AURORA_BR_CYAN] == 0x0055E6FFu);
    assert(BLYT_AURORA_BR_RED == 160 && a[BLYT_AURORA_BR_RED] == 0x00FF6262u);
    assert(BLYT_AURORA_BR_MAGENTA == 236 && a[BLYT_AURORA_BR_MAGENTA] == 0x00FF52FFu);
    assert(BLYT_AURORA_BR_YELLOW == 175 && a[BLYT_AURORA_BR_YELLOW] == 0x00FFEA4Au);
    assert(BLYT_AURORA_WHITE == 15 && a[BLYT_AURORA_WHITE] == 0x00FFFFFFu);
}

/* The unprefixed default alias tracks the console default palette (aurora), so
 * BLYT_WHITE draws white with zero config before any palette swap. */
static void test_default_alias_is_aurora(void) {
    assert(BLYT_BLACK == BLYT_AURORA_BLACK);
    assert(BLYT_WHITE == BLYT_AURORA_WHITE);
    assert(BLYT_RED == BLYT_AURORA_RED);
    assert(BLYT_BR_YELLOW == BLYT_AURORA_BR_YELLOW);
}

int main(void) {
    test_ega_vga_canonical_16();
    test_aurora_nearest_to_ega();
    test_default_alias_is_aurora();
    printf("test_color_constants: all passed\n");
    return 0;
}
