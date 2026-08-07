/*
 * state_buffer.c — SOA state buffer runtime (ADR-0009, ADR-0010, ADR-0057, ADR-0058)
 *
 * Allocated by blyt_state_ctx_init from .cart.layouts section data, owned
 * by blyt_session_t.  Called from ECALL dispatch in cart_run.c.
 */

#include "state_buffer.h"

#include <stdlib.h>
#include <string.h>

#include "blyt_fp_canon.h" /* runtime/shared: blyt_canon_f32/f64 (ADR-0010) */
#include "blyt_gen.h" /* runtime/shared: blyt_gen_next (ADR-0096) */
#include "cart_layouts_reader.h"

/* type_tag encoding (must match ecall.h and config.rs) */
#define TYPE_I8 0
#define TYPE_U8 1
#define TYPE_I16 2
#define TYPE_U16 3
#define TYPE_I32 4
#define TYPE_U32 5
#define TYPE_F32 6
#define TYPE_BOOL 7
#define TYPE_F64 8 /* Spike U: 64-bit double field */

static size_t field_sizeof(uint8_t type_tag) {
    switch (type_tag) {
    case TYPE_I8:
    case TYPE_U8:
    case TYPE_BOOL:
        return 1;
    case TYPE_I16:
    case TYPE_U16:
        return 2;
    case TYPE_F64:
        return 8;
    case TYPE_I32:
    case TYPE_U32:
    case TYPE_F32:
    default:
        return 4;
    }
}

int blyt_state_ctx_init(const blyt_cart_t *cart, blyt_state_ctx_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));

    size_t sect_size = 0;
    const void *sect_data = blyt_cart_find_section(cart, ".cart.layouts", &sect_size);
    if (!sect_data)
        return -1; /* no state buffers declared */

    /* Skip the 8-byte CLAY preamble */
    if (sect_size <= 8)
        return -2;
    const void *fb = (const uint8_t *)sect_data + 8;
    size_t fb_size = sect_size - 8;

    blyt_CartLayouts_table_t layouts = blyt_CartLayouts_as_root(fb);
    if (!layouts)
        return -2;

    blyt_BufferDecl_vec_t buf_vec = blyt_CartLayouts_buffers(layouts);
    size_t n_bufs = buf_vec ? blyt_BufferDecl_vec_len(buf_vec) : 0;
    if (n_bufs == 0)
        return 0; /* no buffers, not an error */

    blyt_RecordDecl_vec_t rec_vec = blyt_CartLayouts_records(layouts);
    uint64_t schema_hash = blyt_CartLayouts_schema_hash(layouts);

    for (size_t bi = 0; bi < n_bufs && bi < BLYT_MAX_BUFFERS; bi++) {
        blyt_BufferDecl_table_t bdecl = blyt_BufferDecl_vec_at(buf_vec, bi);
        if (!bdecl)
            continue;

        blyt_buffer_ctx_t *bc = &ctx->buffers[bi];
        bc->buf_id = (uint32_t)(bi + 1);
        bc->count = blyt_BufferDecl_count(bdecl);
        bc->schema_hash = schema_hash;
        bc->name = blyt_BufferDecl_name(bdecl);

        /* Find the matching record */
        const char *rec_name = blyt_BufferDecl_record_name(bdecl);
        blyt_RecordDecl_table_t rdecl = NULL;
        size_t n_recs = rec_vec ? blyt_RecordDecl_vec_len(rec_vec) : 0;
        for (size_t ri = 0; ri < n_recs; ri++) {
            blyt_RecordDecl_table_t r = blyt_RecordDecl_vec_at(rec_vec, ri);
            if (r && strcmp(blyt_RecordDecl_name(r), rec_name) == 0) {
                rdecl = r;
                break;
            }
        }
        if (!rdecl) {
            blyt_state_ctx_destroy(ctx);
            return -2;
        }

        blyt_FieldDecl_vec_t fld_vec = blyt_RecordDecl_fields(rdecl);
        size_t n_fields = fld_vec ? blyt_FieldDecl_vec_len(fld_vec) : 0;
        if (n_fields == 0 || n_fields > BLYT_MAX_FIELDS) {
            blyt_state_ctx_destroy(ctx);
            return -2;
        }
        bc->n_fields = (uint32_t)n_fields;

        for (size_t fi = 0; fi < n_fields; fi++) {
            blyt_FieldDecl_table_t fdecl = blyt_FieldDecl_vec_at(fld_vec, fi);
            if (!fdecl) {
                blyt_state_ctx_destroy(ctx);
                return -2;
            }
            uint8_t tag = blyt_FieldDecl_type_tag(fdecl);
            bc->field_types[fi] = tag;
            bc->field_names[fi] = blyt_FieldDecl_name(fdecl);

            size_t elem_size = field_sizeof(tag);
            size_t total = (size_t)bc->count * elem_size;
            bc->field_data[fi] = calloc(1, total);
            if (!bc->field_data[fi]) {
                blyt_state_ctx_destroy(ctx);
                return -2;
            }
        }

        /* Generation counters start at 1: 0 is reserved so a packed ref to
         * slot 0 (gen<<16 | slot) can never equal BLYT_ENTITY_REF_NONE. */
        for (uint32_t s = 0; s < BLYT_MAX_SLOTS; s++)
            bc->slot_gens[s] = 1;

        ctx->n_buffers++;
    }

    return 0;
}

void blyt_state_ctx_destroy(blyt_state_ctx_t *ctx) {
    for (uint32_t bi = 0; bi < ctx->n_buffers; bi++) {
        blyt_buffer_ctx_t *bc = &ctx->buffers[bi];
        for (uint32_t fi = 0; fi < bc->n_fields; fi++) {
            free(bc->field_data[fi]);
            bc->field_data[fi] = NULL;
        }
    }
    ctx->n_buffers = 0;
}

/* -------------------------------------------------------------------------
 * Internal lookup helpers
 * ------------------------------------------------------------------------- */

static blyt_buffer_ctx_t *find_buffer(blyt_state_ctx_t *ctx, uint32_t buf_id) {
    if (buf_id == 0 || buf_id > ctx->n_buffers)
        return NULL;
    return &ctx->buffers[buf_id - 1];
}

static int slot_is_allocated(const blyt_buffer_ctx_t *bc, int32_t slot) {
    if (slot < 0 || (uint32_t)slot >= bc->count || (uint32_t)slot >= BLYT_MAX_SLOTS)
        return 0;
    return (bc->slot_bitset[slot / 8] >> (slot % 8)) & 1;
}

/* -------------------------------------------------------------------------
 * Get/set
 * ------------------------------------------------------------------------- */

int blyt_state_get(const blyt_state_ctx_t *ctx, uint32_t buf_id, int32_t slot, uint32_t field,
                   uint32_t *out_bits) {
    blyt_buffer_ctx_t *bc = find_buffer((blyt_state_ctx_t *)ctx, buf_id);
    if (!bc)
        return -1;
    if (!slot_is_allocated(bc, slot))
        return -1;
    if (field == 0 || field > bc->n_fields)
        return -1;

    uint32_t fi = field - 1;
    uint8_t tag = bc->field_types[fi];
    /* The 32-bit accessor never handles f64 — that is the 64-bit get64 path
     * (#253 audit). Guarding here mirrors get64/set64's f64-only checks and,
     * defensively, stops a stray f64 field (elem=8) from overflowing the 4-byte
     * `bits` below should a future caller misroute it. */
    if (tag == TYPE_F64)
        return -1;
    size_t elem = field_sizeof(tag);
    const uint8_t *ptr = (const uint8_t *)bc->field_data[fi] + (size_t)slot * elem;

    uint32_t bits = 0;
    memcpy(&bits, ptr, elem);
    *out_bits = bits;
    return 0;
}

int blyt_state_set(blyt_state_ctx_t *ctx, uint32_t buf_id, int32_t slot, uint32_t field,
                   uint32_t value_bits, uint8_t type_tag) {
    blyt_buffer_ctx_t *bc = find_buffer(ctx, buf_id);
    if (!bc)
        return -1;
    if (!slot_is_allocated(bc, slot))
        return -1;
    if (field == 0 || field > bc->n_fields)
        return -1;

    uint32_t fi = field - 1;
    uint8_t tag = bc->field_types[fi];
    (void)type_tag; /* tag from the field declaration is authoritative */

    /* f64 is the 64-bit set64 path only (#253 audit). Guard mirrors get above
     * and get64/set64: a stray f64 field (elem=8) would over-read the 4-byte
     * `value_bits` source in the memcpy below. */
    if (tag == TYPE_F64)
        return -1;

    /* NaN canonicalization for f32 (ADR-0010); shared with the native path. */
    if (tag == TYPE_F32)
        value_bits = blyt_canon_f32(value_bits);

    size_t elem = field_sizeof(tag);
    uint8_t *ptr = (uint8_t *)bc->field_data[fi] + (size_t)slot * elem;
    memcpy(ptr, &value_bits, elem);
    return 0;
}

/* 64-bit (f64) get/set — the scalar path above is 32-bit; f64 is the only
 * field type wider than a word (Spike U). */
int blyt_state_set64(blyt_state_ctx_t *ctx, uint32_t buf_id, int32_t slot, uint32_t field,
                     uint64_t value_bits) {
    blyt_buffer_ctx_t *bc = find_buffer(ctx, buf_id);
    if (!bc)
        return -1;
    if (!slot_is_allocated(bc, slot))
        return -1;
    if (field == 0 || field > bc->n_fields)
        return -1;

    uint32_t fi = field - 1;
    if (bc->field_types[fi] != TYPE_F64)
        return -1; /* 64-bit path is f64-only */

    /* NaN canonicalization for f64 (ADR-0010); shared with the native path. */
    value_bits = blyt_canon_f64(value_bits);

    uint8_t *ptr = (uint8_t *)bc->field_data[fi] + (size_t)slot * 8;
    memcpy(ptr, &value_bits, 8);
    return 0;
}

int blyt_state_get64(const blyt_state_ctx_t *ctx, uint32_t buf_id, int32_t slot, uint32_t field,
                     uint64_t *out_bits) {
    blyt_buffer_ctx_t *bc = find_buffer((blyt_state_ctx_t *)ctx, buf_id);
    if (!bc)
        return -1;
    if (!slot_is_allocated(bc, slot))
        return -1;
    if (field == 0 || field > bc->n_fields)
        return -1;

    uint32_t fi = field - 1;
    if (bc->field_types[fi] != TYPE_F64)
        return -1;

    const uint8_t *ptr = (const uint8_t *)bc->field_data[fi] + (size_t)slot * 8;
    uint64_t bits = 0;
    memcpy(&bits, ptr, 8);
    *out_bits = bits;
    return 0;
}

/* -------------------------------------------------------------------------
 * Slot management
 * ------------------------------------------------------------------------- */

int blyt_state_alloc_slot(blyt_state_ctx_t *ctx, uint32_t buf_id, int32_t *out_slot) {
    blyt_buffer_ctx_t *bc = find_buffer(ctx, buf_id);
    if (!bc)
        return -1;

    for (uint32_t s = 0; s < bc->count && s < BLYT_MAX_SLOTS; s++) {
        if (!((bc->slot_bitset[s / 8] >> (s % 8)) & 1)) {
            bc->slot_bitset[s / 8] |= (uint8_t)(1u << (s % 8));
            *out_slot = (int32_t)s;
            return 0;
        }
    }
    *out_slot = -1; /* BLYT_INVALID_SLOT */
    return -1;
}

/* -------------------------------------------------------------------------
 * Snapshot / restore / zero (for --reset-every-frame cycle)
 * ------------------------------------------------------------------------- */

struct blyt_state_snapshot {
    uint32_t n_buffers;
    struct {
        uint32_t count;
        uint32_t n_fields;
        uint8_t field_types[BLYT_MAX_FIELDS];
        void *field_data[BLYT_MAX_FIELDS];
        uint8_t slot_bitset[BLYT_MAX_SLOTS / 8];
        uint16_t slot_gens[BLYT_MAX_SLOTS];
    } buffers[BLYT_MAX_BUFFERS];
};

blyt_state_snapshot_t *blyt_state_ctx_snapshot(const blyt_state_ctx_t *ctx) {
    blyt_state_snapshot_t *snap = calloc(1, sizeof(*snap));
    if (!snap)
        return NULL;
    snap->n_buffers = ctx->n_buffers;
    for (uint32_t bi = 0; bi < ctx->n_buffers; bi++) {
        const blyt_buffer_ctx_t *bc = &ctx->buffers[bi];
        snap->buffers[bi].count = bc->count;
        snap->buffers[bi].n_fields = bc->n_fields;
        memcpy(snap->buffers[bi].field_types, bc->field_types, sizeof(bc->field_types));
        memcpy(snap->buffers[bi].slot_bitset, bc->slot_bitset, sizeof(bc->slot_bitset));
        memcpy(snap->buffers[bi].slot_gens, bc->slot_gens, sizeof(bc->slot_gens));
        for (uint32_t fi = 0; fi < bc->n_fields; fi++) {
            size_t sz = (size_t)bc->count * field_sizeof(bc->field_types[fi]);
            snap->buffers[bi].field_data[fi] = malloc(sz);
            if (!snap->buffers[bi].field_data[fi]) {
                blyt_state_snapshot_free(snap);
                return NULL;
            }
            memcpy(snap->buffers[bi].field_data[fi], bc->field_data[fi], sz);
        }
    }
    return snap;
}

void blyt_state_ctx_restore_snapshot(blyt_state_ctx_t *ctx, const blyt_state_snapshot_t *snap) {
    if (!snap || snap->n_buffers != ctx->n_buffers)
        return;
    for (uint32_t bi = 0; bi < ctx->n_buffers; bi++) {
        blyt_buffer_ctx_t *bc = &ctx->buffers[bi];
        const void *sb_slot = snap->buffers[bi].slot_bitset;
        if (snap->buffers[bi].n_fields != bc->n_fields || snap->buffers[bi].count != bc->count)
            continue;
        memcpy(bc->slot_bitset, sb_slot, sizeof(bc->slot_bitset));
        memcpy(bc->slot_gens, snap->buffers[bi].slot_gens, sizeof(bc->slot_gens));
        for (uint32_t fi = 0; fi < bc->n_fields; fi++) {
            if (!snap->buffers[bi].field_data[fi] || !bc->field_data[fi])
                continue;
            size_t sz = (size_t)bc->count * field_sizeof(bc->field_types[fi]);
            memcpy(bc->field_data[fi], snap->buffers[bi].field_data[fi], sz);
        }
    }
}

void blyt_state_snapshot_free(blyt_state_snapshot_t *snap) {
    if (!snap)
        return;
    for (uint32_t bi = 0; bi < snap->n_buffers; bi++) {
        for (uint32_t fi = 0; fi < snap->buffers[bi].n_fields; fi++)
            free(snap->buffers[bi].field_data[fi]);
    }
    free(snap);
}

void blyt_state_ctx_zero_data(blyt_state_ctx_t *ctx) {
    for (uint32_t bi = 0; bi < ctx->n_buffers; bi++) {
        blyt_buffer_ctx_t *bc = &ctx->buffers[bi];
        memset(bc->slot_bitset, 0, sizeof(bc->slot_bitset));
        /* Fresh-boot state for generation counters is 1, not 0 (see header). */
        for (uint32_t s = 0; s < BLYT_MAX_SLOTS; s++)
            bc->slot_gens[s] = 1;
        for (uint32_t fi = 0; fi < bc->n_fields; fi++) {
            if (!bc->field_data[fi])
                continue;
            size_t sz = (size_t)bc->count * field_sizeof(bc->field_types[fi]);
            memset(bc->field_data[fi], 0, sz);
        }
    }
}

int blyt_state_free_slot(blyt_state_ctx_t *ctx, uint32_t buf_id, int32_t slot) {
    blyt_buffer_ctx_t *bc = find_buffer(ctx, buf_id);
    if (!bc)
        return -1;
    if (slot < 0 || (uint32_t)slot >= bc->count || (uint32_t)slot >= BLYT_MAX_SLOTS)
        return -1;
    if (!slot_is_allocated(bc, slot))
        return -1;

    bc->slot_bitset[slot / 8] &= (uint8_t)~(1u << (slot % 8));

    /* Zero out field data for the freed slot */
    for (uint32_t fi = 0; fi < bc->n_fields; fi++) {
        size_t elem = field_sizeof(bc->field_types[fi]);
        uint8_t *ptr = (uint8_t *)bc->field_data[fi] + (size_t)slot * elem;
        memset(ptr, 0, elem);
    }

    /* Stale-ref detection (ADR-0096): bump the generation on successful free
     * only, wrapping 65535 -> 1 (0 is reserved as the invalid sentinel).  Host
     * stores the generation unbiased, so the shared primitive applies directly. */
    bc->slot_gens[slot] = blyt_gen_next(bc->slot_gens[slot]);
    return 0;
}

/* -------------------------------------------------------------------------
 * Packed entity refs (ADR-0096): gen:16 | slot:16, 0 = invalid sentinel
 * ------------------------------------------------------------------------- */

uint32_t blyt_state_ref(const blyt_state_ctx_t *ctx, uint32_t buf_id, int32_t slot) {
    blyt_buffer_ctx_t *bc = find_buffer((blyt_state_ctx_t *)ctx, buf_id);
    if (!bc || !slot_is_allocated(bc, slot))
        return 0;
    return ((uint32_t)bc->slot_gens[slot] << 16) | (uint32_t)slot;
}

int blyt_state_ref_valid(const blyt_state_ctx_t *ctx, uint32_t buf_id, uint32_t ref) {
    if (ref == 0)
        return 0;
    blyt_buffer_ctx_t *bc = find_buffer((blyt_state_ctx_t *)ctx, buf_id);
    if (!bc)
        return 0;
    int32_t slot = (int32_t)(ref & 0xFFFFu);
    if (!slot_is_allocated(bc, slot))
        return 0;
    return bc->slot_gens[slot] == (uint16_t)(ref >> 16);
}
