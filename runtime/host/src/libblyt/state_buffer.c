/*
 * state_buffer.c — SOA state buffer runtime (ADR-0009, ADR-0010, ADR-0057, ADR-0058)
 *
 * Allocated by blyt_state_ctx_init from .cart.layouts section data, owned
 * by blyt_session_t.  Called from ECALL dispatch in cart_run.c.
 */

#include "state_buffer.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

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

/* Canonical NaN bit pattern for f32 writes (ADR-0010). */
#define F32_CANONICAL_NAN UINT32_C(0x7FC00000)

static size_t field_sizeof(uint8_t type_tag) {
    switch (type_tag) {
    case TYPE_I8:
    case TYPE_U8:
    case TYPE_BOOL:
        return 1;
    case TYPE_I16:
    case TYPE_U16:
        return 2;
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

            size_t elem_size = field_sizeof(tag);
            size_t total = (size_t)bc->count * elem_size;
            bc->field_data[fi] = calloc(1, total);
            if (!bc->field_data[fi]) {
                blyt_state_ctx_destroy(ctx);
                return -2;
            }
        }

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

    /* NaN canonicalization for f32 (ADR-0010) */
    if (tag == TYPE_F32) {
        float fv;
        memcpy(&fv, &value_bits, 4);
        if (isnan(fv))
            value_bits = F32_CANONICAL_NAN;
    }

    size_t elem = field_sizeof(tag);
    uint8_t *ptr = (uint8_t *)bc->field_data[fi] + (size_t)slot * elem;
    memcpy(ptr, &value_bits, elem);
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
    return 0;
}
