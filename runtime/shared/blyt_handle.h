/*
 * blyt_handle.h — console-wide tagged-u32 handle encoding (ADR-0134, ADR-0096).
 *
 * Part of runtime/shared: freestanding portable C (stdint only, no libc, no
 * allocator, no stdio).  Compiled into the host runtime, the guest libs and the
 * native bare-metal path — and *mirrored* in the devtool packer
 * (`devtool/src/build/handle.rs`), which mints the baked constants.  The C
 * `_Static_assert`s below and the Rust round-trip test keep the two in sync, the
 * way `blyt_mem_budget.h` ↔ `persistent.rs` does for the memory cap.
 *
 * First-level classifier: `kind = h >> 29` (top 3 bits), shared console-wide
 * across baked resource constants and the dynamic surface / lock-view handles
 * (#195).  Kind 0 is *exclusively* the null sentinel, so `RESOURCE` is 1 (not 0)
 * — there is never an overload between "null" and "a resource whose id is 0".
 *
 * Resource constants are compiled into `cart_resources.h` and shipped inside the
 * `.blyt`, so their layout is a forward-compat contract a future runtime must
 * still read:
 *
 *   bits 31–29 : kind        (= RESOURCE)            ┐ upper 7 bits = kind-tag
 *   bits 28–25 : reserved tag headroom              ┘ region; the tag may widen
 *                                                     3→7 bits without ever
 *                                                     moving provenance or id
 *   bit  24    : provenance  (0 = cart-bundled, 1 = runtime-shipped / built-in)
 *   bits 23–0  : id          (16 M)
 *
 * The resource *type* (text/bytes/image/…) is NOT encoded — it is resolved by
 * registry lookup on the id, so the runtime stays byte-blind (ADR-0068, #166).
 *
 * `blyt_entity_ref_t` keeps its own `gen«16» | index«16»` space and is
 * deliberately OUTSIDE this scheme (a high-generation entity ref aliases the
 * kind bits); that is harmless because entity refs never flow into the resource
 * / gfx APIs and vice versa — their type discipline is by API boundary, not tag.
 */

#ifndef BLYT_SHARED_HANDLE_H
#define BLYT_SHARED_HANDLE_H

#include <stdint.h>

/* First-level classifier values (kind = h >> 29). */
enum {
    BLYT_HANDLE_KIND_NONE = 0, /* universal null sentinel (the whole word is 0) */
    BLYT_HANDLE_KIND_RESOURCE = 1, /* baked resource constant (ADR-0134) */
    BLYT_HANDLE_KIND_SURFACE = 2, /* reserved for the surface model (#195) */
    BLYT_HANDLE_KIND_LOCKVIEW = 3, /* reserved for the surface model (#195) */
};

/* The universal null handle. */
#define BLYT_HANDLE_NONE 0u

#define BLYT_HANDLE_KIND_SHIFT 29

/* Resource-constant field layout. */
#define BLYT_RESOURCE_ID_MASK 0x00FFFFFFu /* bits 23–0 */
#define BLYT_RESOURCE_PROVENANCE_SHIFT 24 /* bit 24 */
#define BLYT_RESOURCE_PROVENANCE_MASK 0x1u
#define BLYT_RESOURCE_PROV_CART 0u /* cart-bundled */
#define BLYT_RESOURCE_PROV_RUNTIME 1u /* runtime-shipped / built-in (reserved) */

/* Encode a resource constant from its packer-assigned id + provenance.  A macro
 * (not an inline fn) so it is usable in a constant expression — the packer emits
 * the literal value, and the `_Static_assert`s below pin the contract. */
#define BLYT_RESOURCE_ENCODE(id, prov)                                                             \
    (((uint32_t)BLYT_HANDLE_KIND_RESOURCE << BLYT_HANDLE_KIND_SHIFT) |                             \
     (((uint32_t)(prov) & BLYT_RESOURCE_PROVENANCE_MASK) << BLYT_RESOURCE_PROVENANCE_SHIFT) |      \
     ((uint32_t)(id) & BLYT_RESOURCE_ID_MASK))

/* First-level classifier on any console handle. */
static inline uint32_t blyt_handle_kind(uint32_t h) {
    return h >> BLYT_HANDLE_KIND_SHIFT;
}

/* Encode a resource constant from its packer-assigned id + provenance (the
 * runtime-callable form of BLYT_RESOURCE_ENCODE; e.g. to report a table id back
 * to the cart as the constant it shipped). */
static inline uint32_t blyt_resource_encode(uint32_t id, uint32_t provenance) {
    return BLYT_RESOURCE_ENCODE(id, provenance);
}

/* Decode the 24-bit packer id from a resource constant. */
static inline uint32_t blyt_resource_decode_id(uint32_t h) {
    return h & BLYT_RESOURCE_ID_MASK;
}

/* Decode the provenance bit from a resource constant. */
static inline uint32_t blyt_resource_decode_provenance(uint32_t h) {
    return (h >> BLYT_RESOURCE_PROVENANCE_SHIFT) & BLYT_RESOURCE_PROVENANCE_MASK;
}

/* True if h is classified as a resource constant (kind RESOURCE).  Provenance is
 * a separate check: runtime-shipped (provenance 1) has no registry yet (ADR-0134
 * defers built-in population), so the resolver treats it as not-found. */
static inline int blyt_handle_is_resource(uint32_t h) {
    return blyt_handle_kind(h) == (uint32_t)BLYT_HANDLE_KIND_RESOURCE;
}

/* Contract pins — mirrored by the devtool round-trip test (handle.rs). */
_Static_assert(BLYT_RESOURCE_ENCODE(1u, 0u) == 0x20000001u, "id 1 / cart encodes to 0x20000001");
_Static_assert((BLYT_RESOURCE_ENCODE(0x123456u, 0u) >> BLYT_HANDLE_KIND_SHIFT) ==
                   (uint32_t)BLYT_HANDLE_KIND_RESOURCE,
               "kind bits = RESOURCE");
_Static_assert((BLYT_RESOURCE_ENCODE(0x123456u, 0u) & BLYT_RESOURCE_ID_MASK) == 0x123456u,
               "id round-trips through the 24-bit field");
_Static_assert(BLYT_RESOURCE_ENCODE(1u, 1u) == 0x21000001u, "provenance bit lands at bit 24");
_Static_assert(BLYT_HANDLE_KIND_NONE == 0, "kind 0 is exclusively NONE");

#endif /* BLYT_SHARED_HANDLE_H */
