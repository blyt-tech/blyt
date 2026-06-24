/*
 * test_blys — pins the BLYS byte format produced by runtime/shared/blyt_blys.c
 * (ADR-0125, #129).  Asserts exact on-disk bytes (header layout, section
 * framing, per-field true-width SOA, fixed bitset/gens blobs), then a
 * write→read round-trip and the schema-mismatch path.  Host-side ctest; the
 * module itself is freestanding and also compiles into the native guest lib.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "blyt_blys.h"

/* ── Memory sink / source backing the freestanding I/O callbacks ──────────── */

typedef struct {
    uint8_t buf[8192];
    uint32_t len;
} sink_ctx_t;

static int mem_sink(void *c, const void *buf, uint32_t len) {
    sink_ctx_t *s = c;
    if (s->len + len > sizeof(s->buf))
        return -1;
    memcpy(s->buf + s->len, buf, len);
    s->len += len;
    return 0;
}

typedef struct {
    const uint8_t *buf;
    uint32_t n;
    uint32_t pos;
} src_ctx_t;

static int mem_source(void *c, void *buf, uint32_t len) {
    src_ctx_t *s = c;
    if (s->pos == s->n)
        return 1; /* clean EOF at boundary */
    if (s->pos + len > s->n)
        return -1;
    memcpy(buf, s->buf + s->pos, len);
    s->pos += len;
    return 0;
}

static uint32_t rd_u16(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}
static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd_u64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

int main(void) {
    /* ── Source state: one buffer "game", count=3, fields i32, bool, f64 ──── */
    const uint64_t SCHEMA = 0x1122334455667788ull;
    const uint32_t SAVE_VERSION = 7;

    uint8_t types[3] = {4 /*i32*/, 7 /*bool*/, 8 /*f64*/};
    uint8_t f_i32[3 * 4];
    uint8_t f_bool[3 * 1] = {1, 0, 1};
    uint8_t f_f64[3 * 8];
    for (int i = 0; i < 12; i++)
        f_i32[i] = (uint8_t)(0x10 + i);
    for (int i = 0; i < 24; i++)
        f_f64[i] = (uint8_t)(0xA0 + i);

    uint8_t bitset[BLYS_SLOT_BITSET_BYTES];
    memset(bitset, 0, sizeof(bitset));
    bitset[0] = 0x05; /* slots 0 and 2 allocated */

    uint8_t gens[BLYS_SLOT_GENS_BYTES];
    memset(gens, 0, sizeof(gens));
    for (uint32_t s = 0; s < BLYS_MAX_SLOTS; s++) {
        gens[s * 2] = 1; /* all slots gen 1 (low byte), unbiased */
    }
    gens[1 * 2] = 5; /* slot 1 gen 5 */

    void *fd[3] = {f_i32, f_bool, f_f64};
    blys_buffer_t buf = {
        .name = "game",
        .name_len = 4,
        .n_fields = 3,
        .field_types = types,
        .count = 3,
        .field_data = fd,
        .slot_bitset = bitset,
        .slot_gens = gens,
    };
    blys_header_t hdr = {.schema_hash = SCHEMA, .save_version = SAVE_VERSION};

    /* ── Write ─────────────────────────────────────────────────────────────*/
    sink_ctx_t sink = {0};
    assert(blys_write(&hdr, &buf, 1, mem_sink, &sink) == BLYS_OK);

    const uint8_t *o = sink.buf;
    /* Header (20 bytes). */
    assert(memcmp(o, "BLYS", 4) == 0);
    assert(rd_u16(o + 4) == BLYS_FORMAT_VERSION_MAJOR);
    assert(rd_u16(o + 6) == BLYS_FORMAT_VERSION_MINOR);
    assert(rd_u64(o + 8) == SCHEMA);
    assert(rd_u32(o + 16) == SAVE_VERSION);

    /* CART section header. */
    assert(memcmp(o + 20, "CART", 4) == 0);
    uint32_t payload = rd_u32(o + 24);
    /* 4 (name_len) + 4 (name) + (12 + 3 + 24) SOA + 16 bitset + 256 gens. */
    assert(payload == 4 + 4 + (12 + 3 + 24) + BLYS_SLOT_BITSET_BYTES + BLYS_SLOT_GENS_BYTES);
    assert(rd_u32(o + 28) == 4); /* name_len */
    assert(memcmp(o + 32, "game", 4) == 0);

    /* SOA arrays at true widths, field order, contiguous. */
    uint32_t off = 36;
    assert(memcmp(o + off, f_i32, 12) == 0);
    off += 12;
    assert(memcmp(o + off, f_bool, 3) == 0);
    off += 3;
    assert(memcmp(o + off, f_f64, 24) == 0);
    off += 24;
    assert(memcmp(o + off, bitset, BLYS_SLOT_BITSET_BYTES) == 0);
    off += BLYS_SLOT_BITSET_BYTES;
    assert(memcmp(o + off, gens, BLYS_SLOT_GENS_BYTES) == 0);
    off += BLYS_SLOT_GENS_BYTES;
    assert(off == sink.len); /* no trailing bytes */
    assert(off == 20 + 8 + payload);
    printf("write: %u bytes, layout OK\n", sink.len);

    /* ── Round-trip read into fresh zeroed targets ──────────────────────────*/
    uint8_t r_i32[12] = {0}, r_bool[3] = {0}, r_f64[24] = {0};
    uint8_t r_bitset[BLYS_SLOT_BITSET_BYTES] = {0};
    uint8_t r_gens[BLYS_SLOT_GENS_BYTES] = {0};
    void *rfd[3] = {r_i32, r_bool, r_f64};
    blys_buffer_t rbuf = {
        .name = "game",
        .name_len = 4,
        .n_fields = 3,
        .field_types = types,
        .count = 3,
        .field_data = rfd,
        .slot_bitset = r_bitset,
        .slot_gens = r_gens,
    };

    src_ctx_t src = {.buf = sink.buf, .n = sink.len, .pos = 0};
    uint32_t got_version = 0;
    assert(blys_read(SCHEMA, &rbuf, 1, mem_source, &src, &got_version) == BLYS_OK);
    assert(got_version == SAVE_VERSION);
    assert(memcmp(r_i32, f_i32, 12) == 0);
    assert(memcmp(r_bool, f_bool, 3) == 0);
    assert(memcmp(r_f64, f_f64, 24) == 0);
    assert(memcmp(r_bitset, bitset, sizeof(bitset)) == 0);
    assert(memcmp(r_gens, gens, sizeof(gens)) == 0);
    printf("round-trip: state recovered\n");

    /* ── Schema mismatch rejected ───────────────────────────────────────────*/
    src_ctx_t src2 = {.buf = sink.buf, .n = sink.len, .pos = 0};
    assert(blys_read(SCHEMA ^ 1, &rbuf, 1, mem_source, &src2, &got_version) == BLYS_ERR_SCHEMA);
    printf("schema mismatch: rejected\n");

    printf("test_blys: PASS\n");
    return 0;
}
