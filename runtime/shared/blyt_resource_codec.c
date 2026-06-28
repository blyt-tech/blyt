#include "blyt_resource_codec.h"

#define ZSTD_STATIC_LINKING_ONLY /* ZSTD_createDCtx_advanced / ZSTD_customMem */
#include <zstd.h>

#include <stdlib.h> /* malloc/free for the default (weak) decode-scratch hooks */

/* Decode-scratch allocator (#158, ADR-0008/0027). zstd's working memory for a
 * decompress must come from a raw allocator that is NOT the cart's unified-budget
 * heap arena: the decompressed *output* is the resource cache (accounted as
 * footprint, never in guest_heap_used), and the transient DCtx workspace must not
 * touch the cart heap at all — otherwise it would consume the 16 MB budget, fail
 * spuriously under heap pressure, and perturb arena layout, breaking determinism.
 *
 * On the host this is automatic (the codec runs in a different address space from
 * the guest arena, so the default `malloc` is already a separate allocator). On
 * the native bare-metal path the codec shares one process with the cart, where
 * `malloc` is interposed by the cart-heap arena (libblytc) — so native
 * libblytcommon provides STRONG overrides of these weak defaults, routing zstd
 * scratch to its private mmap allocator instead. Signature matches
 * ZSTD_allocFunction / ZSTD_freeFunction (ADR-0026). */
__attribute__((weak)) void *blyt_res_scratch_alloc(void *opaque, size_t size) {
    (void)opaque;
    return malloc(size);
}
__attribute__((weak)) void blyt_res_scratch_free(void *opaque, void *address) {
    (void)opaque;
    free(address);
}

int blyt_res_header_parse(const uint8_t *section, size_t section_len, uint8_t *algo,
                          uint32_t *dsize, const uint8_t **body, size_t *body_len) {
    if (!section || section_len < BLYT_RES_HEADER_LEN)
        return 0;
    uint8_t a = section[0];
    if (a != BLYT_RES_ALGO_NONE && a != BLYT_RES_ALGO_ZSTD)
        return 0;
    /* dsize @4, little-endian — width-explicit so it is identical on the LP64
     * host and the ILP32 native target. */
    uint32_t d;
    __builtin_memcpy(&d, section + 4, 4);
    if (algo)
        *algo = a;
    if (dsize)
        *dsize = d;
    if (body)
        *body = section + BLYT_RES_HEADER_LEN;
    if (body_len)
        *body_len = section_len - BLYT_RES_HEADER_LEN;
    return 1;
}

int blyt_res_decode(uint8_t algo, const uint8_t *body, size_t body_len, uint8_t *out,
                    size_t out_len) {
    if (algo == BLYT_RES_ALGO_NONE) {
        if (body_len != out_len)
            return -1;
        if (out_len)
            __builtin_memcpy(out, body, out_len);
        return 0;
    }
    if (algo == BLYT_RES_ALGO_ZSTD) {
        /* Decode through a DCtx whose working memory comes from the (overridable)
         * decode-scratch allocator, never the cart-heap arena (#158). This is
         * exactly what ZSTD_decompress() does internally, but with our customMem
         * instead of stdlib — the decoded bytes are bit-identical either way. */
        ZSTD_customMem cmem = {blyt_res_scratch_alloc, blyt_res_scratch_free, NULL};
        ZSTD_DCtx *dctx = ZSTD_createDCtx_advanced(cmem);
        if (!dctx)
            return -1;
        size_t got = ZSTD_decompressDCtx(dctx, out, out_len, body, body_len);
        ZSTD_freeDCtx(dctx);
        if (ZSTD_isError(got) || got != out_len)
            return -1;
        return 0;
    }
    return -1;
}
