#include "blyt_resource_codec.h"

#include <zstd.h>

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
        size_t got = ZSTD_decompress(out, out_len, body, body_len);
        if (ZSTD_isError(got) || got != out_len)
            return -1;
        return 0;
    }
    return -1;
}
