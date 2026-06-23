#pragma once

/*
 * Host resource table (ADR-0040, ADR-0088, issue #91).
 *
 * Maps integer resource handles to their bytes. Two sources:
 *   - Packed (release): `.cart.resource.<id>` ELF sections in the cart; the
 *     entry data aliases the cart's mmap (not owned).
 *   - Dev (project-dir): `<dir>/resource-id-index` + content-addressed staging
 *     files under `<dir>/resources/`; each file is read into an owned buffer.
 *
 * The blyt_resource_text_get ECALL resolves a handle through this table and
 * copies the bytes into a guest scratch region (see cart_run.c).
 */

#include "blyt_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t id;
    const uint8_t *data; /* bytes; aliases the cart map, or points at `owned` */
    size_t len;
    void *owned; /* heap buffer to free on clear; NULL when data aliases the map */
} blyt_resource_entry_t;

typedef struct {
    blyt_resource_entry_t *entries;
    size_t count;
    size_t cap;
} blyt_resource_table_t;

void blyt_resource_table_init(blyt_resource_table_t *t);
void blyt_resource_table_clear(blyt_resource_table_t *t);
const blyt_resource_entry_t *blyt_resource_table_find(const blyt_resource_table_t *t, uint32_t id);

/* Release: populate from a packed cart's `.cart.resource.<id>` sections. Entry
 * data aliases the cart map. Clears existing entries first. Returns the number
 * of resources found. */
size_t blyt_resource_table_load_from_cart(blyt_resource_table_t *t, const blyt_cart_t *cart);

/* Dev: (re)populate from `<dir>/resource-id-index` and the content-addressed
 * staging files it references. Each file is read into an owned buffer. Clears
 * existing entries first. Returns 0 on success, -1 if the index is missing or
 * unreadable. Safe to call repeatedly (hot-swap re-reads the current index). */
int blyt_resource_table_load_from_index(blyt_resource_table_t *t, const char *dir);
