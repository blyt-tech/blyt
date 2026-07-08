/*
 * blyt_arena.c — single-sourced cart-heap arena allocator (ADR-0008 #158, #158).
 *
 * See blyt_arena.h. The allocation core is ported verbatim from the original
 * libblytc arena (blytc_arena.c, ADR-0120); the additions are (a) per-instance
 * state (blyt_arena_t) instead of file-static globals, so one implementation can
 * back several arenas on the host, and (b) unified-budget accounting + the
 * 16 MB logical cap (blyt_mem_budget.h).
 *
 * Block layout (every block is 16-byte aligned):
 *   [ blk_hdr_t (16 bytes) | ... user data ... ]
 * User data starts 16 bytes in, so it is always 16-byte aligned (the largest
 * natural alignment on RV32IMAFDC).
 */

#include "blyt_arena.h"

#include <string.h>

#define BLYT_ARENA_ALIGN 16u
#define HDR_SIZE 16u

typedef struct {
    uint32_t size; /* total block size incl. header, multiple of BLYT_ARENA_ALIGN */
    uint32_t free; /* 0=used, 1=free */
    uint32_t next; /* free list: offset of next free block, BLYT_ARENA_NONE if tail */
    uint32_t _pad;
} blk_hdr_t;

static inline blk_hdr_t *hdr_at(blyt_arena_t *a, uint32_t off) {
    return (blk_hdr_t *)((char *)a->base + off);
}

static inline uint32_t off_of(blyt_arena_t *a, const blk_hdr_t *h) {
    return (uint32_t)((const char *)h - (const char *)a->base);
}

static inline uint32_t align_up(uint32_t n, uint32_t al) {
    return (n + al - 1u) & ~(al - 1u);
}

/* Unified-budget gate: may we hand out a block that consumes `consumed` live
 * bytes? Uncapped when there is no accounting block. */
static inline int cap_allows(const blyt_arena_t *a, uint32_t consumed) {
    if (!a->acct)
        return 1;
    /* Budget the cart-attributable heap (excludes the runtime-scaffolding
     * baseline, #231) so the fail-point is cross-leg deterministic. Baseline is 0
     * where unset (C carts, emulated), so this is the plain guest_heap_used there. */
    return blyt_mem_alloc_fits(blyt_mem_cart_heap(a->acct), a->acct->non_evictable_footprint,
                               consumed);
}

static inline void acct_add(blyt_arena_t *a, uint32_t bytes) {
    if (a->acct)
        a->acct->guest_heap_used += bytes;
}

static inline void acct_sub(blyt_arena_t *a, uint32_t bytes) {
    if (a->acct)
        a->acct->guest_heap_used -= bytes;
}

/* Remove the block at offset `target` from the free list. */
static int fl_remove(blyt_arena_t *a, uint32_t target) {
    uint32_t *prev = &a->free_head;
    while (*prev != BLYT_ARENA_NONE) {
        if (*prev == target) {
            blk_hdr_t *t = hdr_at(a, target);
            *prev = t->next;
            t->free = 0;
            t->next = BLYT_ARENA_NONE;
            return 1;
        }
        prev = &hdr_at(a, *prev)->next;
    }
    return 0;
}

/* If the block immediately after h is also free, merge it into h. */
static void coalesce_forward(blyt_arena_t *a, blk_hdr_t *h) {
    uint32_t next_off = off_of(a, h) + h->size;
    if (next_off >= a->bump)
        return;
    blk_hdr_t *nb = hdr_at(a, next_off);
    if (!nb->free)
        return;
    fl_remove(a, next_off);
    h->size += nb->size;
}

static void ensure_ready(blyt_arena_t *a) {
    if (a->ready)
        return;
    a->bump = 0;
    a->free_head = BLYT_ARENA_NONE;
    a->ready = 1;
}

void *blyt_arena_malloc(blyt_arena_t *a, size_t n) {
    if (!a->base || !a->size)
        return NULL;
    ensure_ready(a);
    if (n == 0)
        n = 1;
    uint32_t need = align_up((uint32_t)n + HDR_SIZE, BLYT_ARENA_ALIGN);

    /* First-fit search of the free list. */
    uint32_t *prev = &a->free_head;
    while (*prev != BLYT_ARENA_NONE) {
        uint32_t coff = *prev;
        blk_hdr_t *h = hdr_at(a, coff);
        if (h->size >= need) {
            uint32_t rem = h->size - need;
            int split = (rem >= HDR_SIZE + BLYT_ARENA_ALIGN);
            /* Consumed live bytes: `need` if we split off the remainder, else
             * the whole block. Gate the unified budget on the honest figure. */
            uint32_t consumed = split ? need : h->size;
            if (!cap_allows(a, consumed))
                return NULL;
            if (split) {
                blk_hdr_t *tail = hdr_at(a, coff + need);
                tail->size = rem;
                tail->free = 1;
                tail->next = h->next;
                tail->_pad = 0;
                *prev = coff + need;
                h->size = need;
            } else {
                *prev = h->next;
            }
            h->free = 0;
            h->next = BLYT_ARENA_NONE;
            acct_add(a, h->size);
            return (char *)h + HDR_SIZE;
        }
        prev = &h->next;
    }

    /* Bump-allocate a fresh block from the uninitialised tail. */
    if ((uint64_t)a->bump + need > (uint64_t)a->size)
        return NULL;
    if (!cap_allows(a, need))
        return NULL;
    blk_hdr_t *h = hdr_at(a, a->bump);
    h->size = need;
    h->free = 0;
    h->next = BLYT_ARENA_NONE;
    h->_pad = 0;
    a->bump += need;
    acct_add(a, need);
    return (char *)h + HDR_SIZE;
}

void blyt_arena_free(blyt_arena_t *a, void *p) {
    if (!p)
        return;
    blk_hdr_t *h = (blk_hdr_t *)((char *)p - HDR_SIZE);
    if (h->free)
        return; /* double-free: silently ignore */
    acct_sub(a, h->size);
    h->free = 1;
    h->next = a->free_head;
    a->free_head = off_of(a, h);
    coalesce_forward(a, h);
}

void *blyt_arena_realloc(blyt_arena_t *a, void *p, size_t n) {
    if (!p)
        return blyt_arena_malloc(a, n);
    if (!n) {
        blyt_arena_free(a, p);
        return NULL;
    }
    blk_hdr_t *h = (blk_hdr_t *)((char *)p - HDR_SIZE);
    size_t cap = h->size - HDR_SIZE;
    if (n <= cap)
        return p; /* fits in place; accounting unchanged */
    void *q = blyt_arena_malloc(a, n);
    if (!q)
        return NULL;
    memcpy(q, p, cap);
    blyt_arena_free(a, p);
    return q;
}

void *blyt_arena_calloc(blyt_arena_t *a, size_t nmemb, size_t sz) {
    if (sz && nmemb > (size_t)-1 / sz)
        return NULL;
    size_t total = nmemb * sz;
    void *p = blyt_arena_malloc(a, total);
    if (p)
        memset(p, 0, total);
    return p;
}

void blyt_arena_reset(blyt_arena_t *a) {
    a->bump = 0;
    a->free_head = BLYT_ARENA_NONE;
    a->ready = 0; /* next op re-initialises from (possibly re-stamped) base/size */
    if (a->acct)
        a->acct->guest_heap_used = 0;
}
