/*
 * blyt_mem_budget.h — unified 16 MB working-memory budget accounting
 * (ADR-0008 #158 amendment, issue #158).
 *
 * Part of runtime/shared: freestanding portable C (stdint only, no libc, no
 * allocator, no stdio, no global ctors). Compiled into the host runtime, the
 * rv32 guest libs (libblytc arena allocator), the native bare-metal path, AND
 * the WASM host-Lua fast path. Single-sourcing the budget *decision* here is
 * what makes a `malloc`/`load` succeed-or-fail identically on every leg
 * (ADR-0007) — the core determinism contract of #158.
 *
 * The model (ADR-0008 #158): one unified 16 MB logical budget spans the guest
 * heap AND the decompressed resource cache. An allocation — a guest `malloc`
 * *or* a resource decompress/`pin` — succeeds iff
 *
 *     guest_heap_used + non_evictable_footprint + incoming <= 16 MB.
 *
 * Determinism is carried entirely by that predicate. The *evictable* resource
 * cache is NOT in the sum (it can always be reclaimed), so `resource_cache_used`,
 * residency, and LRU order stay advisory and are never serialized (ADR-0027 v2).
 * `guest_heap_used` is published by the guest arena allocator; the host publishes
 * `non_evictable_footprint` (Sum of decompressed length over loaded/pinned/
 * persistent entries). Both live in a guest-visible counter block on the
 * emulated path and a plain shared struct on native — but the arithmetic below
 * is identical everywhere.
 */

#ifndef BLYT_SHARED_MEM_BUDGET_H
#define BLYT_SHARED_MEM_BUDGET_H

#include <stdint.h>

/* The cart-visible working-memory cap: 16 MiB (ADR-0008). The other 16 MiB of
 * the 32 MiB runtime is overhead (framebuffer, mixer, VM state). This is the
 * *logical* cap enforced by the predicate below; it equals the physical rv32
 * arena size today but the two are distinct concepts (the arena is one of two
 * physical pools the single logical budget spans). */
#define BLYT_MEM_BUDGET_BYTES (16u * 1024u * 1024u)

/* The deterministic tier of the introspection API (ADR-0029, #159) promises
 * budget_cap is 16 MB bit-identically on every leg. Pin that here so the promise
 * cannot silently rot if the cap is ever retuned without revisiting the API. */
_Static_assert(BLYT_MEM_BUDGET_BYTES == 16u * 1024u * 1024u, "budget_cap is the 16 MB cap");

/* The unified-budget shared accounting counters (ADR-0008 #158, ADR-0029 #159).
 * One instance is shared between the writers: the guest arena allocator owns
 * `guest_heap_used`; the resource subsystem owns `non_evictable_footprint` and
 * `resource_cache_used`. 32-bit fields: every term is bounded by the 16 MB
 * budget, and a 32-bit width keeps the layout identical across wasm32 and rv32
 * (determinism). The block is guest-readable so the introspection API
 * (blyt_mem_stats, #159) reads the scalar totals directly — no ECALL, no
 * traversal — exactly the "running totals" ADR-0029 calls for.
 *
 * Only `non_evictable_footprint` (with guest_heap_used) bears determinism: it is
 * the budget predicate's term. `resource_cache_used` is ADVISORY — resident
 * decompressed bytes, history/LRU-dependent — published for display only and
 * never fed back into any allocation decision. */
typedef struct {
    uint32_t guest_heap_used; /* live guest heap bytes; guest-written, host-read */
    uint32_t non_evictable_footprint; /* loaded/pinned/persistent reserved bytes
                                       * (deterministic); host-written, guest-read */
    uint32_t resource_cache_used; /* resident decompressed cache bytes (advisory,
                                   * #159); host-written, guest-read */
    uint32_t guest_heap_baseline; /* runtime-scaffolding bytes to exclude from the
                                   * cart-attributable heap (#231): guest_heap_used
                                   * captured once after VM/runtime setup, before the
                                   * cart's first allocation. Zero on legs with no
                                   * scaffolding (C carts, emulated) → no-op. Makes
                                   * cart_allocations + the fail-point cross-leg
                                   * deterministic despite per-leg runtime overhead
                                   * (the host-Lua fast path's driver coroutines vs a
                                   * direct C frame loop). Leg-local; never serialized. */
} blyt_mem_accounting_t;

/* Cart-attributable heap: live bytes minus the runtime-scaffolding baseline,
 * clamped at 0. This is the figure the 16 MB budget predicate and the
 * introspection API (cart_allocations) use, so both are identical across legs for
 * the same cart regardless of each leg's runtime overhead (#231). */
static inline uint32_t blyt_mem_cart_heap(const blyt_mem_accounting_t *a) {
    return a->guest_heap_used > a->guest_heap_baseline ? a->guest_heap_used - a->guest_heap_baseline
                                                       : 0u;
}

/* Does an allocation of `incoming` bytes fit the unified budget, given the
 * current heap usage and non-evictable resource footprint? Overflow-safe (sums
 * in 64-bit so near-UINT32_MAX terms can never wrap to a false "fits"). This is
 * the single success/failure decision for every allocation site on every leg. */
static inline int blyt_mem_alloc_fits(uint32_t guest_heap_used, uint32_t non_evictable_footprint,
                                      uint32_t incoming) {
    return (uint64_t)guest_heap_used + (uint64_t)non_evictable_footprint + (uint64_t)incoming <=
           (uint64_t)BLYT_MEM_BUDGET_BYTES;
}

/* Bytes available to the *evictable* resource cache: the budget minus the
 * non-reclaimable footprint (live heap + non-evictable resident resources).
 * Clamped to 0 — never underflows/wraps. The resource decompress path evicts
 * LRU-evictable down to this figure before materializing new bytes (#158). */
static inline uint32_t blyt_mem_cache_room(uint32_t guest_heap_used,
                                           uint32_t non_evictable_footprint) {
    uint64_t used = (uint64_t)guest_heap_used + (uint64_t)non_evictable_footprint;
    if (used >= (uint64_t)BLYT_MEM_BUDGET_BYTES)
        return 0u;
    return (uint32_t)((uint64_t)BLYT_MEM_BUDGET_BYTES - used);
}

#endif /* BLYT_SHARED_MEM_BUDGET_H */
