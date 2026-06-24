#pragma once

/*
 * blyt_blys — the one definition of the BLYS save byte format (ADR-0125).
 *
 * Freestanding (stdint/stddef only) so the *identical* byte-layout logic
 * compiles into both the host runtime (runtime/host/src/libblyt/save.c) and the
 * native bare-metal guest lib (frontends/native/src/libblytcommon).  File I/O
 * stays per-side via the sink/source callbacks below — FILE* on host, raw
 * syscalls on native — so host and native cannot drift (issue #129).
 *
 * On-disk layout (little-endian; matches the pre-existing host serializer, which
 * is a pragmatic 20-byte-header subset of the full ADR-0125 envelope):
 *
 *   Header (20 bytes):
 *     [0..3]   magic "BLYS"
 *     [4..5]   format_version_major (u16)
 *     [6..7]   format_version_minor (u16)
 *     [8..15]  schema_hash (u64)
 *     [16..19] save_version (u32) — the writing cart's .cart.config save_version
 *
 *   Then one CART section per state buffer, in manifest (declaration) order:
 *     [0..3]   tag "CART"
 *     [4..7]   payload_size (u32)
 *     payload:
 *       name_len   (u32)
 *       name       [name_len bytes]
 *       SOA arrays per field: count * blys_field_width(type) bytes
 *       slot_bitset BLYS_SLOT_BITSET_BYTES bytes
 *       slot_gens   BLYS_SLOT_GENS_BYTES bytes (u16 each, unbiased; ADR-0096)
 */

#include <stddef.h>
#include <stdint.h>

#define BLYS_MAGIC "BLYS"
#define BLYS_FORMAT_VERSION_MAJOR ((uint16_t)1)
/* minor 1 (ADR-0125): the 20-byte header carries the writing cart's
 * save_version (u32 LE) at offset 16; minor 0 saves end at 16 bytes and report
 * save_version 0. */
#define BLYS_FORMAT_VERSION_MINOR ((uint16_t)1)
#define BLYS_SECT_CART "CART"

/* Fixed per-buffer slot dimension on disk.  Both the host (BLYT_MAX_SLOTS) and
 * the native (NATIVE_MAX_SLOTS) storage caps must equal this so the bitset and
 * generation blobs are byte-identical across platforms. */
#define BLYS_MAX_SLOTS 128
#define BLYS_SLOT_BITSET_BYTES (BLYS_MAX_SLOTS / 8) /* 16 */
#define BLYS_SLOT_GENS_BYTES (BLYS_MAX_SLOTS * 2) /* 256 (u16 each) */

/* Return codes. */
#define BLYS_OK 0
#define BLYS_ERR_IO (-1)
#define BLYS_ERR_SCHEMA (-2) /* saved schema_hash != expected */

/* Byte sink: write exactly `len` bytes.  Returns 0 on success, <0 on error. */
typedef int (*blys_write_fn)(void *ctx, const void *buf, uint32_t len);

/* Byte source: read exactly `len` bytes.  Returns 0 on success, 1 on clean EOF
 * (zero bytes available at a section boundary), <0 on error / short read. */
typedef int (*blys_read_fn)(void *ctx, void *buf, uint32_t len);

/* Header fields the caller supplies on write / receives on read. */
typedef struct {
    uint64_t schema_hash;
    uint32_t save_version;
} blys_header_t;

/* One state buffer's view, shared by both directions.  The pointed-to arrays are
 * read on write and written on read; the pointers themselves are not modified.
 *   field_data[fi]: count * blys_field_width(field_types[fi]) contiguous bytes
 *   slot_bitset:    BLYS_SLOT_BITSET_BYTES bytes
 *   slot_gens:      BLYS_SLOT_GENS_BYTES bytes (unbiased u16 generations) */
typedef struct {
    const char *name;
    uint32_t name_len;
    uint32_t n_fields;
    const uint8_t *field_types; /* n_fields type tags (schemas/cart_layouts.fbs) */
    uint32_t count; /* slots per buffer */
    void *const *field_data; /* n_fields writable arrays */
    void *slot_bitset;
    void *slot_gens;
} blys_buffer_t;

/* True width (bytes) of a field's stored value by type_tag (ADR-0125 / Spike U):
 *   0=i8 1=u8 2=i16 3=u16 4=i32 5=u32 6=f32 7=bool 8=f64 */
uint32_t blys_field_width(uint8_t type_tag);

/* Byte size of one buffer's CART section payload (name_len field + name + SOA +
 * bitset + gens). */
uint32_t blys_buffer_payload_size(const blys_buffer_t *buf);

/* Write a full BLYS file (header + one CART section per buffer, in order)
 * through `sink`.  Returns BLYS_OK or BLYS_ERR_IO. */
int blys_write(const blys_header_t *hdr, const blys_buffer_t *bufs, uint32_t n_bufs,
               blys_write_fn sink, void *ctx);

/* Read a BLYS file through `source`, matching CART sections to `bufs` by name and
 * filling their arrays.  Unknown sections are skipped; buffers absent from the
 * file are left untouched.  *out_save_version (if non-NULL) receives the header's
 * save_version (0 for minor-0 saves).  Returns BLYS_OK, BLYS_ERR_IO, or
 * BLYS_ERR_SCHEMA (saved schema_hash != expect_schema_hash). */
int blys_read(uint64_t expect_schema_hash, const blys_buffer_t *bufs, uint32_t n_bufs,
              blys_read_fn source, void *ctx, uint32_t *out_save_version);
