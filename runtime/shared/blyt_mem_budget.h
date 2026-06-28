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

/* The unified-budget shared accounting counters (ADR-0008 #158). One instance is
 * shared between the two writers: the guest arena allocator owns
 * `guest_heap_used`; the resource subsystem owns `non_evictable_footprint`.
 * 32-bit fields: every term is bounded by the 16 MB budget, and a 32-bit width
 * keeps the layout identical across wasm32 and rv32 (determinism). */
typedef struct {
    uint32_t guest_heap_used; /* live guest heap bytes; guest-written, host-read */
    uint32_t non_evictable_footprint; /* loaded/pinned/persistent resident bytes;
                                       * host-written, guest-read */
} blyt_mem_accounting_t;

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
