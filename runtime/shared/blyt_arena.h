/*
 * blyt_arena.h — single-sourced cart-heap arena allocator (ADR-0008 #158, #158).
 *
 * Part of runtime/shared: freestanding portable C (stdint/stddef/string only, no
 * libc allocator, no stdio, no global ctors). This is THE allocator the
 * cart-visible heap (malloc/free/realloc/calloc) runs through on every leg —
 * emulated rv32 libblytc, native libblytc, and the WASM host-Lua fast-path
 * lua_Alloc. Single-sourcing it here is what makes `guest_heap_used` (and hence
 * the point at which an allocation hits the 16 MB cap) byte-identical across
 * wasm32 and rv32 for every cart language, not by matching two allocators but by
 * running one (ADR-0007, the #158 determinism contract).
 *
 * Allocation strategy (ported verbatim from the original libblytc arena,
 * ADR-0120): first-fit free list, 16-byte aligned 16-byte-header blocks, eager
 * forward-merge on free. The only addition is unified-budget accounting: each
 * allocation/free updates `acct->guest_heap_used`, and malloc fails (NULL) when
 * the unified predicate (blyt_mem_budget.h) would be exceeded — which, because
 * the non-evictable resource footprint is part of the predicate, can happen
 * before the physical arena is full. The arena never evicts; reclaim is the
 * resource subsystem's job (ADR-0027 #158), and evicting cache cannot change the
 * predicate anyway.
 *
 * Per-instance state, not global: the caller owns one blyt_arena_t, sets `base`/
 * `size`/`acct` before first use, and the allocator drives `bump`/`free_head`/
 * `ready` from there.
 */

#ifndef BLYT_SHARED_ARENA_H
#define BLYT_SHARED_ARENA_H

#include <stddef.h>
#include <stdint.h>

#include "blyt_mem_budget.h" /* blyt_mem_accounting_t, blyt_mem_alloc_fits */

typedef struct {
    void *base; /* arena base; caller sets before first use (ADR-0120) */
    uint32_t size; /* arena byte capacity */
    uint32_t bump; /* offset past the last initialised block */
    uint32_t free_head; /* free-list head offset; BLYT_ARENA_NONE if empty */
    int ready; /* 0 = (re)initialise bump/free_head from base/size on next op */
    blyt_mem_accounting_t *acct; /* unified accounting; NULL = uncapped/unaccounted */
} blyt_arena_t;

/* End-of-free-list / "no block" sentinel (offset 0xffffffff is never valid). */
#define BLYT_ARENA_NONE 0xffffffffu

/* malloc/free/realloc/calloc over the arena. Semantics match the C library:
 * malloc(0) returns a unique freeable pointer; realloc(NULL,n)==malloc(n);
 * realloc(p,0) frees and returns NULL; free(NULL) is a no-op. Returns NULL on
 * physical exhaustion OR when the unified 16 MB budget would be exceeded. */
void *blyt_arena_malloc(blyt_arena_t *a, size_t n);
void blyt_arena_free(blyt_arena_t *a, void *p);
void *blyt_arena_realloc(blyt_arena_t *a, void *p, size_t n);
void *blyt_arena_calloc(blyt_arena_t *a, size_t nmemb, size_t sz);

/* Re-initialise to a fresh-load-identical state (hot-swap, #133): empties the
 * free list, restarts the bump pointer at the base, and zeroes
 * acct->guest_heap_used so a reloaded cart's allocations are bit-identical to a
 * fresh load. Leaves base/size (caller re-stamps if they changed) and the
 * resource footprint untouched. */
void blyt_arena_reset(blyt_arena_t *a);

#endif /* BLYT_SHARED_ARENA_H */
