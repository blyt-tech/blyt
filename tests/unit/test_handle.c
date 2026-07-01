/*
 * test_handle — pins the console-wide tagged-u32 handle encoding
 * (runtime/shared/blyt_handle.h, ADR-0134/ADR-0096, #195/#196).
 *
 * The resource-constant half (kind RESOURCE, provenance, 24-bit id) is a baked
 * forward-compat contract already pinned by the header's _Static_asserts and the
 * devtool round-trip (handle.rs). This test focuses on the *dynamic* surface
 * model half (#205): the SURFACE / LOCKVIEW kinds and the reserved BLYT_SCREEN
 * constant, whose kind|gen|index layout is runtime-private but must classify and
 * round-trip consistently everywhere the runtime resolves a handle (host, native,
 * wasm). Header-only and freestanding, so the test just needs the include dir.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "blyt_handle.h"

/* The four first-level kinds partition the handle space; kind 0 is exclusively
 * the null sentinel. */
static void test_kind_classification(void) {
    assert(blyt_handle_kind(BLYT_HANDLE_NONE) == BLYT_HANDLE_KIND_NONE);
    assert(!blyt_handle_is_resource(BLYT_HANDLE_NONE));
    assert(!blyt_handle_is_surface(BLYT_HANDLE_NONE));
    assert(!blyt_handle_is_lockview(BLYT_HANDLE_NONE));

    uint32_t res = blyt_resource_encode(1u, BLYT_RESOURCE_PROV_CART);
    assert(blyt_handle_is_resource(res));
    assert(!blyt_handle_is_surface(res));
    assert(!blyt_handle_is_lockview(res));
}

/* BLYT_SCREEN is the reserved built-in surface: SURFACE kind, registry slot 0,
 * generation 0. Unlike dynamically minted surface handles it is compiled into
 * cart code as an immediate, so its value is a stable contract (0x40000000). */
static void test_screen_constant(void) {
    assert(BLYT_SCREEN == 0x40000000u);
    assert(blyt_handle_is_surface(BLYT_SCREEN));
    assert(!blyt_handle_is_lockview(BLYT_SCREEN));
    assert(blyt_handle_kind(BLYT_SCREEN) == BLYT_HANDLE_KIND_SURFACE);
    assert(blyt_dyn_decode_index(BLYT_SCREEN) == 0u);
    assert(blyt_dyn_decode_gen(BLYT_SCREEN) == 0u);
}

/* Surface handles carry a runtime-private gen|index in the low 29 bits below the
 * kind tag; they classify as SURFACE and round-trip both fields. */
static void test_surface_encode_roundtrip(void) {
    uint32_t h = blyt_surface_encode(7u, 3u);
    assert(blyt_handle_kind(h) == BLYT_HANDLE_KIND_SURFACE);
    assert(blyt_handle_is_surface(h));
    assert(!blyt_handle_is_lockview(h));
    assert(blyt_dyn_decode_index(h) == 3u);
    assert(blyt_dyn_decode_gen(h) == 7u);

    /* Full-width fields do not bleed into the kind tag. */
    uint32_t wide = blyt_surface_encode(BLYT_DYN_GEN_MASK, BLYT_DYN_INDEX_MASK);
    assert(blyt_handle_kind(wide) == BLYT_HANDLE_KIND_SURFACE);
    assert(blyt_dyn_decode_index(wide) == BLYT_DYN_INDEX_MASK);
    assert(blyt_dyn_decode_gen(wide) == BLYT_DYN_GEN_MASK);
}

/* Lock-view tokens share the dynamic gen|index layout but a distinct kind, so a
 * token can never be mistaken for the surface it locks (the enforcement floor:
 * a released token fails the next op's kind check). */
static void test_lockview_encode_roundtrip(void) {
    uint32_t t = blyt_lockview_encode(9u, 5u);
    assert(blyt_handle_kind(t) == BLYT_HANDLE_KIND_LOCKVIEW);
    assert(blyt_handle_is_lockview(t));
    assert(!blyt_handle_is_surface(t));
    assert(blyt_dyn_decode_index(t) == 5u);
    assert(blyt_dyn_decode_gen(t) == 9u);

    /* Same slot/gen, different kind => different value; the tag disambiguates. */
    uint32_t s = blyt_surface_encode(9u, 5u);
    assert(t != s);
    assert(blyt_dyn_decode_index(t) == blyt_dyn_decode_index(s));
    assert(blyt_dyn_decode_gen(t) == blyt_dyn_decode_gen(s));
}

int main(void) {
    test_kind_classification();
    test_screen_constant();
    test_surface_encode_roundtrip();
    test_lockview_encode_roundtrip();
    printf("test_handle: all passed\n");
    return 0;
}
