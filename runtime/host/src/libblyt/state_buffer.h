#pragma once

#include <stdint.h>

#include "cart_load.h"

/* -------------------------------------------------------------------------
 * State buffer runtime context (ADR-0009, ADR-0010, ADR-0057, ADR-0058)
 *
 * blyt_state_ctx_t holds all SOA storage for the declared state buffers of
 * one cart session.  It is owned by blyt_session_t.
 * ------------------------------------------------------------------------- */

/* Maximum number of state buffers a cart may declare. */
#define BLYT_MAX_BUFFERS 32

/* Maximum number of fields per record (after flattening). */
#define BLYT_MAX_FIELDS 256

/* Maximum number of slots per buffer.  Slot indices are int32_t; the bitset
 * is 128 bits (16 bytes) for a 128-slot cap that covers Phase 9 gate needs.
 * Increase BLYT_MAX_SLOTS and the bitset size together if needed. */
#define BLYT_MAX_SLOTS 128

typedef struct {
    /* Packed buffer-ID (upper 16 bits of blyt_field_h). 1-based: 0 = none. */
    uint32_t buf_id;
    /* Number of slots (declared count in blyt.config.yaml). */
    uint32_t count;
    /* Number of primitive fields per slot (flattened field count). */
    uint32_t n_fields;
    /* type_tag for each field (0=i8 1=u8 2=i16 3=u16 4=i32 5=u32 6=f32 7=bool). */
    uint8_t field_types[BLYT_MAX_FIELDS];
    /* SOA data: one array per field, each of (count * sizeof(field)) bytes. */
    void *field_data[BLYT_MAX_FIELDS];
    /* Active-slot bitset: bit i set ↔ slot i is allocated. */
    uint8_t slot_bitset[BLYT_MAX_SLOTS / 8];
    /* Schema hash from .cart.layouts (used by save/load). */
    uint64_t schema_hash;
    /* Buffer name (pointer into FlatBuffer data owned by blyt_cart mmap). */
    const char *name;
} blyt_buffer_ctx_t;

typedef struct blyt_state_ctx {
    uint32_t n_buffers;
    blyt_buffer_ctx_t buffers[BLYT_MAX_BUFFERS];
} blyt_state_ctx_t;

/* Initialise the state context from a cart's .cart.layouts section.
 * Returns 0 on success, -1 if the section is absent (no state buffers),
 * -2 on allocation or parse error. */
int blyt_state_ctx_init(const blyt_cart_t *cart, blyt_state_ctx_t *ctx);

/* Free all SOA data arrays allocated by blyt_state_ctx_init. */
void blyt_state_ctx_destroy(blyt_state_ctx_t *ctx);

/* -------------------------------------------------------------------------
 * Typed get/set accessors (called from ECALL dispatch in cart_run.c)
 *
 * buf_id: 1-based buffer index (upper 16 bits of blyt_field_h)
 * slot:   0-based slot index
 * field:  1-based field index (lower 16 bits of blyt_field_h)
 *
 * All functions return 0 on success, -1 on out-of-range or unallocated slot.
 * GET functions write the raw bits to *out (reinterpreted by the caller).
 * SET functions write the value bits (already bit-cast by the guest stub).
 * f32 SET canonicalises NaN (ADR-0010).
 * ------------------------------------------------------------------------- */

int blyt_state_get(const blyt_state_ctx_t *ctx, uint32_t buf_id, int32_t slot, uint32_t field,
                   uint32_t *out_bits);
int blyt_state_set(blyt_state_ctx_t *ctx, uint32_t buf_id, int32_t slot, uint32_t field,
                   uint32_t value_bits, uint8_t type_tag);

int blyt_state_alloc_slot(blyt_state_ctx_t *ctx, uint32_t buf_id, int32_t *out_slot);
int blyt_state_free_slot(blyt_state_ctx_t *ctx, uint32_t buf_id, int32_t slot);

/* -------------------------------------------------------------------------
 * State snapshot (for --reset-every-frame cycle / hot-reload save-restore)
 * ------------------------------------------------------------------------- */

/* Opaque deep copy of all SOA field arrays + slot bitsets. */
typedef struct blyt_state_snapshot blyt_state_snapshot_t;

/* Deep-copy all SOA field arrays + slot bitsets into a heap allocation.
 * Returns NULL on allocation failure. */
blyt_state_snapshot_t *blyt_state_ctx_snapshot(const blyt_state_ctx_t *ctx);

/* Overwrite SOA arrays + bitsets from snap (n_buffers/count/n_fields must match). */
void blyt_state_ctx_restore_snapshot(blyt_state_ctx_t *ctx, const blyt_state_snapshot_t *snap);

/* Free a snapshot returned by blyt_state_ctx_snapshot. */
void blyt_state_snapshot_free(blyt_state_snapshot_t *snap);

/* Zero all SOA field arrays and slot bitsets in-place (does not reallocate). */
void blyt_state_ctx_zero_data(blyt_state_ctx_t *ctx);
