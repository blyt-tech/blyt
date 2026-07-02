//! Console-wide tagged-`u32` handle encoding — the packer's mirror of
//! `runtime/shared/blyt_handle.h` (ADR-0134, ADR-0096).
//!
//! The packer mints the baked resource constants (`R_<NAME>`) emitted into
//! `cart_resources.{h,lua,rs}`, so it must encode them exactly as the runtime
//! decodes them. The canonical definition is the C header; this is the Rust
//! mirror, kept in sync by the round-trip test below and the header's
//! `_Static_assert`s — the same arrangement as `BLYT_MEM_BUDGET_BYTES` ↔
//! [`super::persistent::MEM_BUDGET_BYTES`].
//!
//! Layout (forward-compat contract, shipped in the `.blyt`):
//! ```text
//! bits 31–29 : kind       (= RESOURCE)
//! bits 28–25 : reserved tag headroom
//! bit  24    : provenance (0 = cart-bundled, 1 = runtime-shipped)
//! bits 23–0  : id (16 M)
//! ```
//! `kind = h >> 29`; kind 0 is exclusively the null sentinel, so `RESOURCE` is 1.

/// First-level classifier value for a baked resource constant (`kind = h >> 29`).
/// Mirrors `BLYT_HANDLE_KIND_RESOURCE`. (NONE=0, SURFACE=2, LOCKVIEW=3 are the
/// other console-wide kinds; only RESOURCE is minted by the packer.)
pub(super) const HANDLE_KIND_RESOURCE: u32 = 1;

const HANDLE_KIND_SHIFT: u32 = 29;
const RESOURCE_ID_MASK: u32 = 0x00FF_FFFF;
const RESOURCE_PROVENANCE_SHIFT: u32 = 24;

/// Provenance: a cart-bundled resource (the only kind the packer mints today;
/// runtime-shipped/built-in population is deferred, ADR-0134). Mirrors
/// `BLYT_RESOURCE_PROV_CART`.
pub(super) const PROV_CART: u32 = 0;

/// Provenance: a runtime-shipped/built-in resource. Mirrors
/// `BLYT_RESOURCE_PROV_RUNTIME`. The only runtime-provenance constants the
/// packer mints today are the four built-in palettes (issue #201); general
/// built-in-resource population is still deferred (ADR-0134).
pub(super) const PROV_RUNTIME: u32 = 1;

/// Encode a packer-assigned resource id into the baked `u32` constant the cart
/// ships and passes back into the resource API. Mirrors `BLYT_RESOURCE_ENCODE`.
///
/// `id` must fit the 24-bit field; the packer assigns small sequential ids, far
/// under 16 M, so this never truncates in practice (asserted in the build).
pub(super) fn resource_encode(id: u32, provenance: u32) -> u32 {
    debug_assert!(
        id <= RESOURCE_ID_MASK,
        "resource id {id} exceeds the 24-bit field"
    );
    (HANDLE_KIND_RESOURCE << HANDLE_KIND_SHIFT)
        | ((provenance & 0x1) << RESOURCE_PROVENANCE_SHIFT)
        | (id & RESOURCE_ID_MASK)
}

/// Encode a cart-bundled resource id — the common case for the packer.
pub(super) fn resource_encode_cart(id: u32) -> u32 {
    resource_encode(id, PROV_CART)
}

/// Encode a built-in (runtime-shipped) resource id — used for the four
/// built-in palette constants (issue #201).
pub(super) fn resource_encode_runtime(id: u32) -> u32 {
    resource_encode(id, PROV_RUNTIME)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The contract pinned by `blyt_handle.h`'s `_Static_assert`s — if either
    /// side moves, one of these fails.
    #[test]
    fn resource_encoding_matches_the_baked_contract() {
        // id 1, cart provenance -> 0x2000_0001 (the canonical example: R_GREETING).
        assert_eq!(resource_encode_cart(1), 0x2000_0001);
        // kind bits classify as RESOURCE.
        assert_eq!(
            resource_encode_cart(0x12_3456) >> HANDLE_KIND_SHIFT,
            HANDLE_KIND_RESOURCE
        );
        // id round-trips through the 24-bit field.
        assert_eq!(
            resource_encode_cart(0x12_3456) & RESOURCE_ID_MASK,
            0x12_3456
        );
        // provenance lands at bit 24.
        assert_eq!(resource_encode(1, 1), 0x2100_0001);
        // full-width id (16 M - 1) does not collide with the kind/provenance bits.
        assert_eq!(resource_encode_cart(RESOURCE_ID_MASK), 0x20FF_FFFF);
        // resource_encode_runtime mirrors BLYT_PALETTE_AURORA (id 1, runtime
        // provenance) — the canonical example from runtime/shared/blyt_palettes.h.
        assert_eq!(resource_encode_runtime(1), 0x2100_0001);
    }
}
