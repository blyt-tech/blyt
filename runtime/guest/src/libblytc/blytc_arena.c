/*
 * blytc_arena.c — arena-backed allocator for libblytc.so (ADR-0120)
 *
 * blytc_arena_base and blytc_arena_size are written by the host runtime
 * (cart_run.c) directly into this library's .data section before cart entry.
 * malloc/free/realloc/calloc sub-allocate from that region; no syscalls.
 *
 * Block layout (every block is BLYTC_ALIGN-byte aligned):
 *
 *   [ blk_hdr_t (16 bytes) | ... user data ... ]
 *
 *   size : total block size including the 16-byte header
 *   free : 0 = used, 1 = free
 *   next : (free blocks only) byte offset of next free block from
 *           blytc_arena_base; BLKC_NONE if end of list
 *   _pad : reserved
 *
 * User data starts at offset 16 from the block base, so it is always
 * 16-byte aligned (matching the largest natural alignment on RV32IMAFC).
 *
 * Strategy: first-fit free list.  Coalescing: eager forward merge on free
 * (merge with the immediately following block when that block is also free).
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Written by the host runtime before cart entry (ADR-0120). */
void *blytc_arena_base = NULL;
size_t blytc_arena_size = 0;

/* ---- block header ---- */

#define BLYTC_ALIGN 16u
#define HDR_SIZE 16u
#define BLKC_NONE 0xffffffffu /* end-of-free-list sentinel */

typedef struct {
    uint32_t size; /* total block size incl. header, multiple of BLYTC_ALIGN */
    uint32_t free; /* 0=used, 1=free */
    uint32_t next; /* free list: offset of next free block, BLKC_NONE if tail */
    uint32_t _pad;
} blk_hdr_t;

/* ---- allocator state ---- */

/* Free-list head: byte offset of first free block, BLKC_NONE if empty.
 * Block h's next pointer is stored in h->next (plain offset, BLKC_NONE if none). */
static uint32_t g_free_head = BLKC_NONE;
static uint32_t g_bump = 0; /* offset past last initialised block */

/* Allocator-ready flag — also the host's hot-reload reset lever (issue #133).
 * The host re-stamps blytc_arena_base/size on every cart load; on an in-VM hot
 * swap (blyt_session_swap_cart) the libblytc image persists, so the bump pointer
 * and free list would otherwise carry over from the previous cart — drifting
 * allocation addresses and, accumulated over a long edit-reload session,
 * exhausting the arena.  The host re-zeros this exported symbol after the
 * base/size re-stamp; the next malloc() then re-initialises g_bump/g_free_head
 * from scratch, making a reloaded cart's allocations bit-identical to a fresh
 * load.  Kept here (not host-stamped directly) so the BLKC_NONE free-list
 * sentinel stays the allocator's secret.  0 = needs (re)init, 1 = ready. */
int blytc_arena_ready = 0;

/* ---- helpers ---- */

static inline blk_hdr_t *hdr_at(uint32_t off) {
    return (blk_hdr_t *)((char *)blytc_arena_base + off);
}

static inline uint32_t off_of(const blk_hdr_t *h) {
    return (uint32_t)((const char *)h - (const char *)blytc_arena_base);
}

static inline uint32_t align_up(uint32_t n, uint32_t a) {
    return (n + a - 1u) & ~(a - 1u);
}

/* ---- free-list splice-out ----
 *
 * Removes the block at offset `target` from the free list.
 * Walks from the head; returns 1 if found and removed, 0 otherwise.
 */
static int fl_remove(uint32_t target) {
    uint32_t *prev = &g_free_head;
    while (*prev != BLKC_NONE) {
        if (*prev == target) {
            blk_hdr_t *t = hdr_at(target);
            *prev = t->next;
            t->free = 0;
            t->next = BLKC_NONE;
            return 1;
        }
        prev = &hdr_at(*prev)->next;
    }
    return 0;
}

/* ---- forward coalescing ----
 *
 * If the block immediately after h in the arena is also free, merge them.
 * h must already be marked free with its next pointer set.
 */
static void coalesce_forward(blk_hdr_t *h) {
    uint32_t next_off = off_of(h) + h->size;
    if (next_off >= g_bump)
        return; /* no following block */

    blk_hdr_t *nb = hdr_at(next_off);
    if (!nb->free)
        return;

    /* Splice nb out of the free list, then grow h to absorb it. */
    fl_remove(next_off);
    h->size += nb->size;
    /* nb is now unreachable; no cleanup needed for its header fields */
}

/* ---- public API ---- */

void *malloc(size_t n) {
    if (!blytc_arena_ready) {
        if (!blytc_arena_base || !blytc_arena_size)
            return NULL;
        /* Fresh load OR post-hot-swap reset (issue #133): restart allocation
         * from the arena base with an empty free list.  On a genuine fresh load
         * these already hold their initial values, so this is a no-op there. */
        g_bump = 0;
        g_free_head = BLKC_NONE;
        blytc_arena_ready = 1;
    }
    if (n == 0)
        n = 1;
    uint32_t need = align_up((uint32_t)n + HDR_SIZE, BLYTC_ALIGN);

    /* First-fit search of the free list. */
    uint32_t *prev = &g_free_head;
    while (*prev != BLKC_NONE) {
        uint32_t coff = *prev;
        blk_hdr_t *h = hdr_at(coff);

        if (h->size >= need) {
            uint32_t rem = h->size - need;
            if (rem >= HDR_SIZE + BLYTC_ALIGN) {
                /* Split: carve `need` from the front, leave remainder as free. */
                blk_hdr_t *tail = hdr_at(coff + need);
                tail->size = rem;
                tail->free = 1;
                tail->next = h->next;
                tail->_pad = 0;
                *prev = coff + need;
                h->size = need;
            } else {
                /* Use the whole block. */
                *prev = h->next;
            }
            h->free = 0;
            h->next = BLKC_NONE;
            return (char *)h + HDR_SIZE;
        }
        prev = &h->next;
    }

    /* Bump-allocate a fresh block from the uninitialized tail. */
    uint32_t arena_sz = (uint32_t)blytc_arena_size;
    if ((uint64_t)g_bump + need > arena_sz)
        return NULL;

    blk_hdr_t *h = hdr_at(g_bump);
    h->size = need;
    h->free = 0;
    h->next = BLKC_NONE;
    h->_pad = 0;
    g_bump += need;
    return (char *)h + HDR_SIZE;
}

void free(void *p) {
    if (!p)
        return;
    blk_hdr_t *h = (blk_hdr_t *)((char *)p - HDR_SIZE);
    if (h->free)
        return; /* double-free: silently ignore */

    h->free = 1;
    h->next = g_free_head;
    g_free_head = off_of(h);

    coalesce_forward(h);
}

void *realloc(void *p, size_t n) {
    if (!p)
        return malloc(n);
    if (!n) {
        free(p);
        return NULL;
    }

    blk_hdr_t *h = (blk_hdr_t *)((char *)p - HDR_SIZE);
    size_t cap = h->size - HDR_SIZE;
    if (n <= cap)
        return p; /* fits in place */

    void *q = malloc(n);
    if (!q)
        return NULL;
    memcpy(q, p, cap);
    free(p);
    return q;
}

void *calloc(size_t nmemb, size_t sz) {
    if (sz && nmemb > (size_t)-1 / sz)
        return NULL;
    size_t total = nmemb * sz;
    void *p = malloc(total);
    if (p)
        memset(p, 0, total);
    return p;
}

/* C++ over-aligned operator new (libc++ stdlib_new_delete) calls aligned_alloc.
 * Every arena block is already BLYTC_ALIGN (16-byte) aligned, which satisfies
 * any alignment up to 16 — covering __STDCPP_DEFAULT_NEW_ALIGNMENT__ on rv32
 * ilp32. Larger over-alignment is unsupported: free() requires the exact
 * malloc() pointer, so we cannot hand back an offset (interior) block. */
void *aligned_alloc(size_t alignment, size_t size) {
    if (alignment > BLYTC_ALIGN)
        return NULL;
    return malloc(size);
}
