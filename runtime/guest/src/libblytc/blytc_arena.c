/*
 * blytc_arena.c — emulated-path libblytc.so cart-heap allocator (ADR-0120, #158).
 *
 * Thin shim. The allocation logic AND the unified-budget accounting now live in
 * the single-sourced runtime/shared arena (blyt_arena.c), run byte-identically
 * on every leg so `guest_heap_used` matches across wasm32 and rv32 (#158,
 * ADR-0007). This file owns only:
 *   - the host-stamped ABI symbols (`blytc_arena_base`/`blytc_arena_size`, the
 *     arena region written by cart_run.c before cart entry, ADR-0120; and
 *     `blytc_arena_ready`, the hot-swap reset lever the host re-zeros, #133),
 *   - the exported unified-budget accounting block (`blyt_mem_acct`) the host
 *     resolves by symbol to publish the resource footprint and read the heap
 *     total, and
 *   - the libc allocator entry points (malloc/free/realloc/calloc/aligned_alloc)
 *     forwarding to one blyt_arena_t over that region.
 */

#include <stddef.h>

#include "blyt_arena.h" /* runtime/shared: the single-sourced allocator core */
#include "blyt_mem_budget.h" /* runtime/shared: blyt_mem_accounting_t */

/* Written by the host runtime before cart entry (ADR-0120). */
void *blytc_arena_base = NULL;
size_t blytc_arena_size = 0;

/* Allocator-ready flag + the host's hot-reload reset lever (issue #133). The
 * host re-zeros this on an in-VM hot swap (after re-stamping base/size) so the
 * next allocation re-initialises the arena from scratch, making a reloaded
 * cart's allocations bit-identical to a fresh load. 0 = needs (re)init. */
int blytc_arena_ready = 0;

/* Unified-budget accounting block (ADR-0008 #158). The guest arena writes
 * `guest_heap_used`; the host publishes `non_evictable_footprint` into it and
 * reads `guest_heap_used` when checking the 16 MB budget in the resource ECALLs.
 * Exported (like blytc_arena_base) so the host can resolve its guest address. */
blyt_mem_accounting_t blyt_mem_acct = {0};

/* The one arena instance backing this lib's malloc family. */
static blyt_arena_t g_arena;

/* Re-stamp the arena from the host-written globals and honour the reset lever
 * before each op. base/size are cheap to copy; the reset lever (#133) maps onto
 * blyt_arena_reset, which also zeroes guest_heap_used so a reloaded cart starts
 * from a clean heap total. The shared arena's own `ready` then drives the actual
 * bump/free-list (re)init on the next allocation. */
static blyt_arena_t *arena(void) {
    g_arena.base = blytc_arena_base;
    g_arena.size = (uint32_t)blytc_arena_size;
    g_arena.acct = &blyt_mem_acct;
    if (!blytc_arena_ready) {
        blyt_arena_reset(&g_arena);
        blytc_arena_ready = 1;
    }
    return &g_arena;
}

void *malloc(size_t n) {
    return blyt_arena_malloc(arena(), n);
}

void free(void *p) {
    blyt_arena_free(arena(), p);
}

void *realloc(void *p, size_t n) {
    return blyt_arena_realloc(arena(), p, n);
}

void *calloc(size_t nmemb, size_t sz) {
    return blyt_arena_calloc(arena(), nmemb, sz);
}

/* C++ over-aligned operator new (libc++ stdlib_new_delete) calls aligned_alloc.
 * Every arena block is already 16-byte aligned, which satisfies any alignment up
 * to 16 — covering __STDCPP_DEFAULT_NEW_ALIGNMENT__ on rv32 ilp32. Larger
 * over-alignment is unsupported: free() requires the exact malloc() pointer, so
 * we cannot hand back an offset (interior) block. */
void *aligned_alloc(size_t alignment, size_t size) {
    if (alignment > 16u)
        return NULL;
    return blyt_arena_malloc(arena(), size);
}
