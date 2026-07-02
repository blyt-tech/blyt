/*
 * test_palettes — pins the built-in palette resolver (runtime/shared/
 * blyt_palettes.h, ADR-0042, issue #201).
 *
 * Header-only-adjacent: links against blyt_palettes.c + blyt_handle.h is
 * header-only, so this just needs the include dir. Verifies the resolver
 * correctly classifies handles (RESOURCE kind, RUNTIME provenance) and
 * returns the right table -- not the exact RGB content (that's an eyeball/
 * data-generation concern, not a resolver-logic one), except for the
 * sacrificial-index-255 invariant every built-in palette must uphold
 * (ADR-0049) and aurora's pinned idx0/idx255 values (the two bytes the
 * design explicitly calls out).
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "blyt_handle.h"
#include "blyt_palettes.h"

/* A handle with RESOURCE kind but CART provenance must not resolve -- only
 * runtime-provenance built-ins are served by this table. */
static void test_cart_provenance_rejected(void) {
    uint32_t h = blyt_resource_encode(BLYT_PAL_ID_AURORA, BLYT_RESOURCE_PROV_CART);
    assert(blyt_builtin_palette(h) == 0);
}

/* A non-resource handle (e.g. the null sentinel, or a surface handle) never
 * resolves. */
static void test_non_resource_rejected(void) {
    assert(blyt_builtin_palette(BLYT_HANDLE_NONE) == 0);
}

/* An unknown id under RUNTIME provenance resolves to nothing (not a random
 * built-in) -- there is no wildcard match. */
static void test_unknown_runtime_id_rejected(void) {
    uint32_t h = blyt_resource_encode(0xFFu, BLYT_RESOURCE_PROV_RUNTIME);
    assert(blyt_builtin_palette(h) == 0);
}

/* Each of the four built-in ids resolves to a distinct, non-null 256-entry
 * table. */
static void test_all_four_resolve_and_differ(void) {
    uint32_t aurora = blyt_resource_encode(BLYT_PAL_ID_AURORA, BLYT_RESOURCE_PROV_RUNTIME);
    uint32_t vga = blyt_resource_encode(BLYT_PAL_ID_VGA, BLYT_RESOURCE_PROV_RUNTIME);
    uint32_t ega = blyt_resource_encode(BLYT_PAL_ID_EGA, BLYT_RESOURCE_PROV_RUNTIME);
    uint32_t cga = blyt_resource_encode(BLYT_PAL_ID_CGA, BLYT_RESOURCE_PROV_RUNTIME);

    const uint32_t *pa = blyt_builtin_palette(aurora);
    const uint32_t *pv = blyt_builtin_palette(vga);
    const uint32_t *pe = blyt_builtin_palette(ega);
    const uint32_t *pc = blyt_builtin_palette(cga);

    assert(pa && pv && pe && pc);
    assert(pa != pv && pa != pe && pa != pc);
    assert(pv != pe && pv != pc);
    assert(pe != pc);
}

/* Every built-in palette carries a valid sacrificial index 255 (ADR-0049):
 * that byte is a real, fully-formed XRGB8888 entry (top byte 0), whatever its
 * color -- the transparent-sprite quantizer can always target it. */
static void test_index_255_is_well_formed_everywhere(void) {
    uint32_t ids[] = {BLYT_PAL_ID_AURORA, BLYT_PAL_ID_VGA, BLYT_PAL_ID_EGA, BLYT_PAL_ID_CGA};
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        uint32_t h = blyt_resource_encode(ids[i], BLYT_RESOURCE_PROV_RUNTIME);
        const uint32_t *pal = blyt_builtin_palette(h);
        assert(pal != 0);
        assert((pal[255] & 0xFF000000u) == 0); /* top byte 0 (XRGB, not ARGB) */
    }
}

/* aurora is pinned exactly: index 0 is black, index 255 is #911437 (the
 * sacrificial slot the design settled on), per the canonical Lospec source. */
static void test_aurora_pinned_endpoints(void) {
    uint32_t h = blyt_resource_encode(BLYT_PAL_ID_AURORA, BLYT_RESOURCE_PROV_RUNTIME);
    const uint32_t *pal = blyt_builtin_palette(h);
    assert(pal[0] == 0x00000000u);
    assert(pal[255] == 0x00911437u);
}

/* vga's first 16 entries are the standard EGA 16, matching the ega table's
 * first 16 -- the documented VGA construction's low end is EGA-compatible. */
static void test_vga_low_16_match_ega(void) {
    uint32_t hv = blyt_resource_encode(BLYT_PAL_ID_VGA, BLYT_RESOURCE_PROV_RUNTIME);
    uint32_t he = blyt_resource_encode(BLYT_PAL_ID_EGA, BLYT_RESOURCE_PROV_RUNTIME);
    const uint32_t *vga = blyt_builtin_palette(hv);
    const uint32_t *ega = blyt_builtin_palette(he);
    for (int i = 0; i < 16; i++)
        assert(vga[i] == ega[i]);
}

/* cga's first 4 entries are the high-intensity palette 1 (black/cyan/magenta/
 * white); the remaining 252 are the black padding. */
static void test_cga_palette1(void) {
    uint32_t h = blyt_resource_encode(BLYT_PAL_ID_CGA, BLYT_RESOURCE_PROV_RUNTIME);
    const uint32_t *cga = blyt_builtin_palette(h);
    assert(cga[0] == 0x00000000u);
    assert(cga[1] == 0x0055FFFFu);
    assert(cga[2] == 0x00FF55FFu);
    assert(cga[3] == 0x00FFFFFFu);
    for (int i = 4; i < 256; i++)
        assert(cga[i] == 0x00000000u);
}

int main(void) {
    test_cart_provenance_rejected();
    test_non_resource_rejected();
    test_unknown_runtime_id_rejected();
    test_all_four_resolve_and_differ();
    test_index_255_is_well_formed_everywhere();
    test_aurora_pinned_endpoints();
    test_vga_low_16_match_ega();
    test_cga_palette1();
    printf("test_palettes: all passed\n");
    return 0;
}
