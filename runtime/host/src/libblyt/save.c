/*
 * save.c — Save/load infrastructure (ADR-0125)
 *
 * Save file format (little-endian):
 *   [0..3]  magic "BLYS"
 *   [4..5]  format_version_major (uint16)
 *   [6..7]  format_version_minor (uint16)
 *   [8..15] schema_hash (uint64)
 *   [16..N] sections: for each buffer:
 *     [0..3]  section tag "CART"
 *     [4..7]  payload_size (uint32): size of buffer name + SOA data + bitset
 *             + generation counters
 *     [8..11] name_len (uint32)
 *     [12..]  name bytes (name_len, no NUL)
 *             SOA data: n_fields arrays of (count * sizeof(field)) bytes
 *             slot_bitset: BLYT_MAX_SLOTS/8 bytes
 *             slot_gens: BLYT_MAX_SLOTS uint16 generation counters (ADR-0096)
 */

#include "save.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* type_tag field sizes */
static size_t field_sizeof_tag(uint8_t tag) {
    switch (tag) {
    case 0:
    case 1:
    case 7:
        return 1; /* i8, u8, bool */
    case 2:
    case 3:
        return 2; /* i16, u16 */
    default:
        return 4; /* i32, u32, f32 */
    }
}

/* Build the save file path into buf (buf_len bytes).
 * Returns 0 on success, -1 on truncation. */
static int build_path(char *buf, size_t buf_len, const char *save_dir, const char *cart_name,
                      uint32_t slot) {
    int n = snprintf(buf, buf_len, "%s/%s/slot_%u.blys", save_dir, cart_name, slot);
    return (n > 0 && (size_t)n < buf_len) ? 0 : -1;
}

/* Create directory and parent in one shot (mkdir -p equivalent for one level). */
static int ensure_dir(const char *save_dir, const char *cart_name) {
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/%s", save_dir, cart_name);
    if (n <= 0 || (size_t)n >= sizeof(path))
        return -1;

    /* create save_dir */
    mkdir(save_dir, 0755);
    /* create save_dir/cart_name */
    if (mkdir(path, 0755) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

/* Write little-endian integers to a buffer. */
static void write_u16le(uint8_t *p, uint16_t v) {
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
}
static void write_u32le(uint8_t *p, uint32_t v) {
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
    p[3] = (v >> 24) & 0xff;
}
static void write_u64le(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        p[i] = v & 0xff;
        v >>= 8;
    }
}
static uint32_t read_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t read_u64le(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

int blyt_save_write(blyt_state_ctx_t *state, const char *save_dir, const char *cart_name,
                    uint32_t slot) {
    if (!save_dir || !cart_name)
        return -1;

    if (ensure_dir(save_dir, cart_name) < 0)
        return -1;

    char path[1024];
    if (build_path(path, sizeof(path), save_dir, cart_name, slot) < 0)
        return -1;

    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;

    /* File header: 16 bytes */
    uint8_t hdr[16];
    memcpy(hdr, BLYS_MAGIC, 4);
    write_u16le(hdr + 4, BLYS_FORMAT_VERSION_MAJOR);
    write_u16le(hdr + 6, BLYS_FORMAT_VERSION_MINOR);
    /* Use schema_hash from first buffer (all buffers share the same schema_hash). */
    uint64_t schema_hash = state->n_buffers > 0 ? state->buffers[0].schema_hash : 0;
    write_u64le(hdr + 8, schema_hash);
    if (fwrite(hdr, 1, 16, f) != 16) {
        fclose(f);
        return -1;
    }

    /* One CART section per buffer */
    for (uint32_t bi = 0; bi < state->n_buffers; bi++) {
        const blyt_buffer_ctx_t *bc = &state->buffers[bi];
        const char *name = bc->name ? bc->name : "";
        uint32_t name_len = (uint32_t)strlen(name);

        /* Compute payload size: name_len field (4) + name bytes + SOA +
         * bitset + generation counters */
        uint32_t payload = 4 + name_len;
        for (uint32_t fi = 0; fi < bc->n_fields; fi++) {
            payload += (uint32_t)(bc->count * field_sizeof_tag(bc->field_types[fi]));
        }
        payload += BLYT_MAX_SLOTS / 8; /* slot bitset */
        payload += BLYT_MAX_SLOTS * (uint32_t)sizeof(uint16_t); /* slot gens */

        /* Section header: tag (4) + payload_size (4) */
        uint8_t sect_hdr[8];
        memcpy(sect_hdr, BLYS_SECT_CART, 4);
        write_u32le(sect_hdr + 4, payload);
        if (fwrite(sect_hdr, 1, 8, f) != 8) {
            fclose(f);
            return -1;
        }

        /* name_len + name */
        uint8_t nl[4];
        write_u32le(nl, name_len);
        if (fwrite(nl, 1, 4, f) != 4) {
            fclose(f);
            return -1;
        }
        if (name_len && fwrite(name, 1, name_len, f) != name_len) {
            fclose(f);
            return -1;
        }

        /* SOA field arrays */
        for (uint32_t fi = 0; fi < bc->n_fields; fi++) {
            size_t arr_sz = bc->count * field_sizeof_tag(bc->field_types[fi]);
            if (fwrite(bc->field_data[fi], 1, arr_sz, f) != arr_sz) {
                fclose(f);
                return -1;
            }
        }

        /* slot bitset */
        if (fwrite(bc->slot_bitset, 1, BLYT_MAX_SLOTS / 8, f) != BLYT_MAX_SLOTS / 8) {
            fclose(f);
            return -1;
        }

        /* slot generation counters (ADR-0096) */
        if (fwrite(bc->slot_gens, 1, sizeof(bc->slot_gens), f) != sizeof(bc->slot_gens)) {
            fclose(f);
            return -1;
        }
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);
    return 0;
}

int blyt_save_read(blyt_state_ctx_t *state, const char *save_dir, const char *cart_name,
                   uint32_t slot) {
    if (!save_dir || !cart_name)
        return -1;

    char path[1024];
    if (build_path(path, sizeof(path), save_dir, cart_name, slot) < 0)
        return -1;

    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;

    /* Read header */
    uint8_t hdr[16];
    if (fread(hdr, 1, 16, f) != 16) {
        fclose(f);
        return -1;
    }

    if (memcmp(hdr, BLYS_MAGIC, 4) != 0) {
        fclose(f);
        return -1;
    }

    uint64_t saved_hash = read_u64le(hdr + 8);
    uint64_t current_hash = state->n_buffers > 0 ? state->buffers[0].schema_hash : 0;

    /* For Phase 9 we support exact-match only; schema migration is deferred. */
    if (saved_hash != current_hash) {
        fclose(f);
        return -2; /* schema mismatch */
    }

    /* Read CART sections */
    for (;;) {
        uint8_t sect_hdr[8];
        size_t n = fread(sect_hdr, 1, 8, f);
        if (n == 0)
            break; /* EOF */
        if (n < 8) {
            fclose(f);
            return -1;
        }

        if (memcmp(sect_hdr, BLYS_SECT_CART, 4) != 0) {
            /* Unknown section — skip */
            uint32_t payload = read_u32le(sect_hdr + 4);
            fseek(f, (long)payload, SEEK_CUR);
            continue;
        }

        /* Read name_len + name */
        uint8_t nl[4];
        if (fread(nl, 1, 4, f) < 4) {
            fclose(f);
            return -1;
        }
        uint32_t name_len = read_u32le(nl);

        char name_buf[256];
        if (name_len > 0) {
            if (name_len >= sizeof(name_buf)) {
                fclose(f);
                return -1;
            }
            if (fread(name_buf, 1, name_len, f) != name_len) {
                fclose(f);
                return -1;
            }
        }
        name_buf[name_len] = '\0';

        /* Find the matching buffer by name */
        blyt_buffer_ctx_t *bc = NULL;
        for (uint32_t bi = 0; bi < state->n_buffers; bi++) {
            const char *bname = state->buffers[bi].name ? state->buffers[bi].name : "";
            if (strcmp(bname, name_buf) == 0) {
                bc = &state->buffers[bi];
                break;
            }
        }

        if (!bc) {
            /* Buffer removed from schema — skip its data */
            uint32_t remaining = read_u32le(sect_hdr + 4) - 4 - name_len;
            fseek(f, (long)remaining, SEEK_CUR);
            continue;
        }

        /* Read SOA arrays */
        for (uint32_t fi = 0; fi < bc->n_fields; fi++) {
            size_t arr_sz = bc->count * field_sizeof_tag(bc->field_types[fi]);
            if (fread(bc->field_data[fi], 1, arr_sz, f) != arr_sz) {
                fclose(f);
                return -1;
            }
        }

        /* Read slot bitset */
        if (fread(bc->slot_bitset, 1, BLYT_MAX_SLOTS / 8, f) != BLYT_MAX_SLOTS / 8) {
            fclose(f);
            return -1;
        }

        /* Read slot generation counters (ADR-0096) */
        if (fread(bc->slot_gens, 1, sizeof(bc->slot_gens), f) != sizeof(bc->slot_gens)) {
            fclose(f);
            return -1;
        }
    }

    fclose(f);
    return 0;
}
