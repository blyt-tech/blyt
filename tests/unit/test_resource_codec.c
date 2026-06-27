/*
 * test_resource_codec — the shared per-resource compression format + decode
 * (runtime/shared/blyt_resource_codec.{h,c}, #157, ADR-0026).
 *
 * Pins the on-disk header contract (algo + decompressed-size, always
 * uncompressed) and both decode paths: `none` is a verbatim copy; `zstd`
 * decompresses a real frame to the exact decompressed length. The frame below
 * was produced by the zstd CLI (`zstd -19`) — any valid frame decodes
 * deterministically regardless of zstd version, which is the property the
 * cross-leg byte-identity contract relies on. The packer's encode side + the
 * compress/skip threshold are covered by the devtool's Rust unit tests; the
 * end-to-end zstd decode across native/WASM/libretro lives in the integration
 * suite (assets.rs).
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "blyt_resource_codec.h"

/* 83-byte payload and its zstd frame (zstd -19). */
static const char PAYLOAD[] =
    "hello, blyt resource codec! hello, blyt resource codec! hello, blyt resource codec!";
static const uint8_t FRAME[] = {0x28, 0xb5, 0x2f, 0xfd, 0x24, 0x53, 0x25, 0x01, 0x00, 0xe8,
                                0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x2c, 0x20, 0x62, 0x6c, 0x79,
                                0x74, 0x20, 0x72, 0x65, 0x73, 0x6f, 0x75, 0x72, 0x63, 0x65,
                                0x20, 0x63, 0x6f, 0x64, 0x65, 0x63, 0x21, 0x20, 0x68, 0x01,
                                0x00, 0xed, 0xcf, 0xe9, 0x04, 0x41, 0xf9, 0x41, 0x97};

/* Build an 8-byte header [algo|0|0|0|dsize_le] prefix into `buf`. */
static void put_header(uint8_t *buf, uint8_t algo, uint32_t dsize) {
    buf[0] = algo;
    buf[1] = buf[2] = buf[3] = 0;
    buf[4] = (uint8_t)(dsize & 0xFF);
    buf[5] = (uint8_t)((dsize >> 8) & 0xFF);
    buf[6] = (uint8_t)((dsize >> 16) & 0xFF);
    buf[7] = (uint8_t)((dsize >> 24) & 0xFF);
}

static void test_header_parse_valid(void) {
    uint8_t sec[16];
    put_header(sec, BLYT_RES_ALGO_ZSTD, 1234);
    memcpy(sec + 8, "abcdefgh", 8);

    uint8_t algo = 0xFF;
    uint32_t dsize = 0;
    const uint8_t *body = NULL;
    size_t body_len = 0;
    assert(blyt_res_header_parse(sec, sizeof(sec), &algo, &dsize, &body, &body_len) == 1);
    assert(algo == BLYT_RES_ALGO_ZSTD);
    assert(dsize == 1234);
    assert(body == sec + BLYT_RES_HEADER_LEN);
    assert(body_len == sizeof(sec) - BLYT_RES_HEADER_LEN);
}

static void test_header_parse_rejects_short_and_bad_algo(void) {
    uint8_t sec[8];
    put_header(sec, BLYT_RES_ALGO_NONE, 0);
    /* A section shorter than the header is malformed. */
    assert(blyt_res_header_parse(sec, BLYT_RES_HEADER_LEN - 1, NULL, NULL, NULL, NULL) == 0);
    /* An unknown algo byte is malformed. */
    sec[0] = 0x7F;
    assert(blyt_res_header_parse(sec, sizeof(sec), NULL, NULL, NULL, NULL) == 0);
}

static void test_decode_none_copies_verbatim(void) {
    const uint8_t body[] = {0x00, 0xFF, 'h', 'i', 0x00};
    uint8_t out[sizeof(body)];
    assert(blyt_res_decode(BLYT_RES_ALGO_NONE, body, sizeof(body), out, sizeof(out)) == 0);
    assert(memcmp(out, body, sizeof(body)) == 0);
    /* A length mismatch (out_len != body_len) is rejected. */
    assert(blyt_res_decode(BLYT_RES_ALGO_NONE, body, sizeof(body), out, sizeof(out) - 1) == -1);
}

static void test_decode_zstd_round_trips(void) {
    size_t dsize = strlen(PAYLOAD); /* 83 */
    uint8_t out[128];
    assert(dsize <= sizeof(out));
    assert(blyt_res_decode(BLYT_RES_ALGO_ZSTD, FRAME, sizeof(FRAME), out, dsize) == 0);
    assert(memcmp(out, PAYLOAD, dsize) == 0);
    /* Wrong expected length (not what the frame encodes) is rejected. */
    assert(blyt_res_decode(BLYT_RES_ALGO_ZSTD, FRAME, sizeof(FRAME), out, dsize - 1) == -1);
}

static void test_decode_rejects_bad_algo(void) {
    uint8_t out[4];
    assert(blyt_res_decode(0x7F, (const uint8_t *)"xxxx", 4, out, 4) == -1);
}

int main(void) {
    test_header_parse_valid();
    test_header_parse_rejects_short_and_bad_algo();
    test_decode_none_copies_verbatim();
    test_decode_zstd_round_trips();
    test_decode_rejects_bad_algo();
    printf("test_resource_codec: all passed\n");
    return 0;
}
