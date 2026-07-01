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

/* -------------------------------------------------------------------------
 * Dynamic handles: SURFACE / LOCKVIEW (the surface model, #195/#205).
 *
 * Off-screen surfaces and their transient per-pixel lock views are minted by
 * the *running* runtime, never baked into a `.blyt` or a save-state (surfaces
 * are draw-scoped derived artifacts).  So — unlike the resource-constant layout
 * above — this `kind | gen | index` packing carries NO forward-compat contract
 * and is the running runtime's private choice; it may evolve per version.  The
 * one exception is BLYT_SCREEN, which a cart *does* compile in as an immediate
 * (see below), so its value is pinned.
 *
 *   bits 31–29 : kind   (= SURFACE or LOCKVIEW)
 *   bits 28–12 : gen    (17-bit generation — bumped to invalidate stale handles)
 *   bits 11–0  : index  (registry slot, 4096)
 *
 * The generation guards use-after-reap (a surface handle) and use-after-release
 * (a lock-view token): a resolver rejects a handle whose gen no longer matches
 * its registry slot.  SURFACE and LOCKVIEW share this layout but the distinct
 * kind tag is the enforcement floor — a released, inert lock-view token can
 * never be mistaken for the surface it locked, so it fails the next op's kind
 * check by construction (#195).
 * ------------------------------------------------------------------------- */

#define BLYT_DYN_INDEX_MASK 0x00000FFFu /* bits 11–0 : registry slot */
#define BLYT_DYN_GEN_SHIFT 12
#define BLYT_DYN_GEN_MASK 0x0001FFFFu /* bits 28–12 : 17-bit generation */

/* Pack a dynamic handle from its kind + generation + registry index.  A macro
 * (not an inline fn) so BLYT_SCREEN below is a constant expression usable as a
 * cart-side immediate and in the _Static_asserts. */
#define BLYT_DYN_ENCODE(kind, gen, index)                                                          \
    (((uint32_t)(kind) << BLYT_HANDLE_KIND_SHIFT) |                                                \
     (((uint32_t)(gen) & BLYT_DYN_GEN_MASK) << BLYT_DYN_GEN_SHIFT) |                               \
     ((uint32_t)(index) & BLYT_DYN_INDEX_MASK))

/* Encode a surface handle from its registry slot + generation. */
static inline uint32_t blyt_surface_encode(uint32_t gen, uint32_t index) {
    return BLYT_DYN_ENCODE(BLYT_HANDLE_KIND_SURFACE, gen, index);
}

/* Encode a lock-view token from the locked surface's slot + a lock generation. */
static inline uint32_t blyt_lockview_encode(uint32_t gen, uint32_t index) {
    return BLYT_DYN_ENCODE(BLYT_HANDLE_KIND_LOCKVIEW, gen, index);
}

/* Decode the registry slot from any dynamic (surface / lock-view) handle. */
static inline uint32_t blyt_dyn_decode_index(uint32_t h) {
    return h & BLYT_DYN_INDEX_MASK;
}

/* Decode the generation from any dynamic (surface / lock-view) handle. */
static inline uint32_t blyt_dyn_decode_gen(uint32_t h) {
    return (h >> BLYT_DYN_GEN_SHIFT) & BLYT_DYN_GEN_MASK;
}

/* True if h is classified as an off-screen (or screen) surface handle. */
static inline int blyt_handle_is_surface(uint32_t h) {
    return blyt_handle_kind(h) == (uint32_t)BLYT_HANDLE_KIND_SURFACE;
}

/* True if h is classified as a transient lock-view token. */
static inline int blyt_handle_is_lockview(uint32_t h) {
    return blyt_handle_kind(h) == (uint32_t)BLYT_HANDLE_KIND_LOCKVIEW;
}

/* The built-in screen surface: SURFACE kind, registry slot 0, generation 0.
 * The runtime's own session->pixels[] — never created, destroyed, reaped, or
 * budget-counted.  A cart compiles this in as an immediate (e.g.
 * `blyt_surface_clear(BLYT_SCREEN, 7)`), so — alone among dynamic handles — its
 * value (0x40000000) is a reserved forward-compat contract. */
#define BLYT_SCREEN BLYT_DYN_ENCODE(BLYT_HANDLE_KIND_SURFACE, 0u, 0u)

/* Contract pins — mirrored by the devtool round-trip test (handle.rs). */
_Static_assert(BLYT_RESOURCE_ENCODE(1u, 0u) == 0x20000001u, "id 1 / cart encodes to 0x20000001");
_Static_assert((BLYT_RESOURCE_ENCODE(0x123456u, 0u) >> BLYT_HANDLE_KIND_SHIFT) ==
                   (uint32_t)BLYT_HANDLE_KIND_RESOURCE,
               "kind bits = RESOURCE");
_Static_assert((BLYT_RESOURCE_ENCODE(0x123456u, 0u) & BLYT_RESOURCE_ID_MASK) == 0x123456u,
               "id round-trips through the 24-bit field");
_Static_assert(BLYT_RESOURCE_ENCODE(1u, 1u) == 0x21000001u, "provenance bit lands at bit 24");
_Static_assert(BLYT_HANDLE_KIND_NONE == 0, "kind 0 is exclusively NONE");
/* Dynamic-handle (surface / lock-view) layout pins. */
_Static_assert(BLYT_SCREEN == 0x40000000u, "screen = SURFACE kind, slot 0, gen 0");
_Static_assert((BLYT_SCREEN >> BLYT_HANDLE_KIND_SHIFT) == (uint32_t)BLYT_HANDLE_KIND_SURFACE,
               "screen classifies as SURFACE");
_Static_assert((BLYT_SCREEN & BLYT_DYN_INDEX_MASK) == 0u, "screen is registry slot 0");
_Static_assert(BLYT_DYN_ENCODE(BLYT_HANDLE_KIND_LOCKVIEW, 5u, 7u) ==
                   (((uint32_t)BLYT_HANDLE_KIND_LOCKVIEW << 29) | (5u << 12) | 7u),
               "lock-view packs kind|gen|index");
_Static_assert((BLYT_DYN_GEN_MASK << BLYT_DYN_GEN_SHIFT) < (1u << BLYT_HANDLE_KIND_SHIFT),
               "gen field stays below the kind tag");

#endif /* BLYT_SHARED_HANDLE_H */
