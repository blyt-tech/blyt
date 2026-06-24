/*
 * blyt_blys — the one definition of the BLYS save byte format (ADR-0125, #129).
 * Freestanding: no libc, only stdint/stddef and compiler builtins, so the same
 * object compiles into the host runtime and the native bare-metal guest lib.
 */

#include "blyt_blys.h"

/* ── Little-endian scalar writers (into a small stack buffer) ─────────────── */

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}
static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}
static void put_u64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v & 0xffu);
        v >>= 8;
    }
}
static uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t get_u64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

uint32_t blys_field_width(uint8_t type_tag) {
    switch (type_tag) {
    case 0: /* i8 */
    case 1: /* u8 */
    case 7: /* bool */
        return 1u;
    case 2: /* i16 */
    case 3: /* u16 */
        return 2u;
    case 8: /* f64 (Spike U) */
        return 8u;
    case 4: /* i32 */
    case 5: /* u32 */
    case 6: /* f32 */
    default:
        return 4u;
    }
}

uint32_t blys_buffer_payload_size(const blys_buffer_t *buf) {
    uint32_t payload = 4u + buf->name_len; /* name_len field + name bytes */
    for (uint32_t fi = 0; fi < buf->n_fields; fi++)
        payload += buf->count * blys_field_width(buf->field_types[fi]);
    payload += BLYS_SLOT_BITSET_BYTES;
    payload += BLYS_SLOT_GENS_BYTES;
    return payload;
}

/* ── Write ───────────────────────────────────────────────────────────────── */

int blys_write(const blys_header_t *hdr, const blys_buffer_t *bufs, uint32_t n_bufs,
               blys_write_fn sink, void *ctx) {
    uint8_t h[20];
    __builtin_memcpy(h, BLYS_MAGIC, 4);
    put_u16(h + 4, BLYS_FORMAT_VERSION_MAJOR);
    put_u16(h + 6, BLYS_FORMAT_VERSION_MINOR);
    put_u64(h + 8, hdr->schema_hash);
    put_u32(h + 16, hdr->save_version);
    if (sink(ctx, h, sizeof(h)) < 0)
        return BLYS_ERR_IO;

    for (uint32_t bi = 0; bi < n_bufs; bi++) {
        const blys_buffer_t *b = &bufs[bi];

        uint8_t sect[8];
        __builtin_memcpy(sect, BLYS_SECT_CART, 4);
        put_u32(sect + 4, blys_buffer_payload_size(b));
        if (sink(ctx, sect, sizeof(sect)) < 0)
            return BLYS_ERR_IO;

        uint8_t nl[4];
        put_u32(nl, b->name_len);
        if (sink(ctx, nl, sizeof(nl)) < 0)
            return BLYS_ERR_IO;
        if (b->name_len && sink(ctx, b->name, b->name_len) < 0)
            return BLYS_ERR_IO;

        for (uint32_t fi = 0; fi < b->n_fields; fi++) {
            uint32_t bytes = b->count * blys_field_width(b->field_types[fi]);
            if (bytes && sink(ctx, b->field_data[fi], bytes) < 0)
                return BLYS_ERR_IO;
        }

        if (sink(ctx, b->slot_bitset, BLYS_SLOT_BITSET_BYTES) < 0)
            return BLYS_ERR_IO;
        if (sink(ctx, b->slot_gens, BLYS_SLOT_GENS_BYTES) < 0)
            return BLYS_ERR_IO;
    }
    return BLYS_OK;
}

/* ── Read ────────────────────────────────────────────────────────────────── */

/* Discard `n` bytes from the source (no seek; read into a stack scratch). */
static int skip_bytes(blys_read_fn source, void *ctx, uint32_t n) {
    uint8_t scratch[256];
    while (n > 0) {
        uint32_t chunk = n > sizeof(scratch) ? (uint32_t)sizeof(scratch) : n;
        if (source(ctx, scratch, chunk) != 0)
            return BLYS_ERR_IO;
        n -= chunk;
    }
    return BLYS_OK;
}

static int name_eq(const blys_buffer_t *b, const char *name, uint32_t name_len) {
    if (b->name_len != name_len)
        return 0;
    return __builtin_memcmp(b->name, name, name_len) == 0;
}

int blys_read(uint64_t expect_schema_hash, const blys_buffer_t *bufs, uint32_t n_bufs,
              blys_read_fn source, void *ctx, uint32_t *out_save_version) {
    if (out_save_version)
        *out_save_version = 0;

    /* 16-byte base header: magic, major, minor, schema_hash. */
    uint8_t h[16];
    if (source(ctx, h, sizeof(h)) != 0)
        return BLYS_ERR_IO;
    if (__builtin_memcmp(h, BLYS_MAGIC, 4) != 0)
        return BLYS_ERR_IO;

    uint16_t minor = get_u16(h + 6);
    if (minor >= 1) {
        uint8_t sv[4];
        if (source(ctx, sv, sizeof(sv)) != 0)
            return BLYS_ERR_IO;
        if (out_save_version)
            *out_save_version = get_u32(sv);
    }

    if (get_u64(h + 8) != expect_schema_hash)
        return BLYS_ERR_SCHEMA;

    for (;;) {
        uint8_t sect[8];
        int r = source(ctx, sect, sizeof(sect));
        if (r == 1)
            break; /* clean EOF at a section boundary */
        if (r != 0)
            return BLYS_ERR_IO;

        uint32_t payload = get_u32(sect + 4);
        if (__builtin_memcmp(sect, BLYS_SECT_CART, 4) != 0) {
            if (skip_bytes(source, ctx, payload) != 0)
                return BLYS_ERR_IO;
            continue;
        }

        uint8_t nl[4];
        if (source(ctx, nl, sizeof(nl)) != 0)
            return BLYS_ERR_IO;
        uint32_t name_len = get_u32(nl);

        char name[256];
        if (name_len >= sizeof(name))
            return BLYS_ERR_IO;
        if (name_len && source(ctx, name, name_len) != 0)
            return BLYS_ERR_IO;

        const blys_buffer_t *b = NULL;
        for (uint32_t bi = 0; bi < n_bufs; bi++) {
            if (name_eq(&bufs[bi], name, name_len)) {
                b = &bufs[bi];
                break;
            }
        }

        if (!b) {
            /* Buffer not present in the current schema — skip its data. */
            if (skip_bytes(source, ctx, payload - 4u - name_len) != 0)
                return BLYS_ERR_IO;
            continue;
        }

        for (uint32_t fi = 0; fi < b->n_fields; fi++) {
            uint32_t bytes = b->count * blys_field_width(b->field_types[fi]);
            if (bytes && source(ctx, b->field_data[fi], bytes) != 0)
                return BLYS_ERR_IO;
        }
        if (source(ctx, b->slot_bitset, BLYS_SLOT_BITSET_BYTES) != 0)
            return BLYS_ERR_IO;
        if (source(ctx, b->slot_gens, BLYS_SLOT_GENS_BYTES) != 0)
            return BLYS_ERR_IO;
    }
    return BLYS_OK;
}
