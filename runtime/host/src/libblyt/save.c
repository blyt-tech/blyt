/*
 * save.c — Save/load infrastructure (ADR-0125)
 *
 * The BLYS byte format is defined once in runtime/shared/blyt_blys.{c,h} and
 * shared with the native bare-metal save path (#129).  This file only builds the
 * per-buffer descriptor from the host state context and adapts FILE* I/O to the
 * shared serializer's sink/source callbacks.
 */

#include "save.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "blyt_blys.h"

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

/* ── FILE* adapters for the shared serializer's byte sink/source ──────────── */

static int file_sink(void *ctx, const void *buf, uint32_t len) {
    return fwrite(buf, 1, len, (FILE *)ctx) == len ? 0 : -1;
}

static int file_source(void *ctx, void *buf, uint32_t len) {
    FILE *f = (FILE *)ctx;
    size_t n = fread(buf, 1, len, f);
    if (n == len)
        return 0;
    if (n == 0 && feof(f))
        return 1; /* clean EOF at a section boundary */
    return -1;
}

/* Build the shared per-buffer view array from the host state context. */
static void fill_views(const blyt_state_ctx_t *state, blys_buffer_t views[BLYT_MAX_BUFFERS]) {
    for (uint32_t bi = 0; bi < state->n_buffers; bi++) {
        const blyt_buffer_ctx_t *bc = &state->buffers[bi];
        const char *name = bc->name ? bc->name : "";
        views[bi].name = name;
        views[bi].name_len = (uint32_t)strlen(name);
        views[bi].n_fields = bc->n_fields;
        views[bi].field_types = bc->field_types;
        views[bi].count = bc->count;
        views[bi].field_data = bc->field_data;
        views[bi].slot_bitset = bc->slot_bitset;
        views[bi].slot_gens = bc->slot_gens;
    }
}

int blyt_save_write(blyt_state_ctx_t *state, const char *save_dir, const char *cart_name,
                    uint32_t slot, uint32_t save_version) {
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

    blys_buffer_t views[BLYT_MAX_BUFFERS];
    fill_views(state, views);

    /* All buffers share one schema_hash. */
    blys_header_t hdr = {
        .schema_hash = state->n_buffers > 0 ? state->buffers[0].schema_hash : 0,
        .save_version = save_version,
    };

    int rc = blys_write(&hdr, views, state->n_buffers, file_sink, f);
    if (rc != BLYS_OK) {
        fclose(f);
        return -1;
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);
    return 0;
}

int blyt_save_read(blyt_state_ctx_t *state, const char *save_dir, const char *cart_name,
                   uint32_t slot, uint32_t *out_save_version) {
    if (out_save_version)
        *out_save_version = 0;
    if (!save_dir || !cart_name)
        return -1;

    char path[1024];
    if (build_path(path, sizeof(path), save_dir, cart_name, slot) < 0)
        return -1;

    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;

    blys_buffer_t views[BLYT_MAX_BUFFERS];
    fill_views(state, views);

    uint64_t current_hash = state->n_buffers > 0 ? state->buffers[0].schema_hash : 0;

    /* blys_read returns BLYS_OK / BLYS_ERR_IO(-1) / BLYS_ERR_SCHEMA(-2), which
     * are this function's documented 0 / -1 / -2 return values. */
    int rc = blys_read(current_hash, views, state->n_buffers, file_source, f, out_save_version);
    fclose(f);
    return rc;
}
