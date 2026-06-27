#include "resource.h"

#include "blyt_elf_section.h" /* runtime/shared: prefix enumerate + id parse (#141) */
#include "blyt_resource_codec.h" /* runtime/shared: per-resource compression (#157) */
#include "cart_load.h" /* struct blyt_cart */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RESOURCE_SECTION_PREFIX ".cart.resource."

void blyt_resource_table_init(blyt_resource_table_t *t) {
    t->entries = NULL;
    t->count = 0;
    t->cap = 0;
}

void blyt_resource_table_clear(blyt_resource_table_t *t) {
    for (size_t i = 0; i < t->count; i++) {
        free(t->entries[i].owned);
    }
    free(t->entries);
    t->entries = NULL;
    t->count = 0;
    t->cap = 0;
}

const blyt_resource_entry_t *blyt_resource_table_find(const blyt_resource_table_t *t, uint32_t id) {
    for (size_t i = 0; i < t->count; i++) {
        if (t->entries[i].id == id)
            return &t->entries[i];
    }
    return NULL;
}

blyt_resource_entry_t *blyt_resource_table_find_mut(blyt_resource_table_t *t, uint32_t id) {
    for (size_t i = 0; i < t->count; i++) {
        if (t->entries[i].id == id)
            return &t->entries[i];
    }
    return NULL;
}

void blyt_resource_table_force_release_pins(blyt_resource_table_t *t) {
    for (size_t i = 0; i < t->count; i++)
        blyt_rl_force_release_pins(&t->entries[i].rl);
}

static blyt_resource_entry_t *table_push(blyt_resource_table_t *t) {
    if (t->count == t->cap) {
        size_t ncap = t->cap ? t->cap * 2 : 8;
        blyt_resource_entry_t *ne = realloc(t->entries, ncap * sizeof(*ne));
        if (!ne)
            return NULL;
        t->entries = ne;
        t->cap = ncap;
    }
    blyt_resource_entry_t *e = &t->entries[t->count++];
    e->id = 0;
    e->data = NULL;
    e->len = 0;
    e->owned = NULL;
    e->algo = BLYT_RES_ALGO_NONE;
    e->cdata = NULL;
    e->clen = 0;
    e->rl = (blyt_rl_state_t){0};
    return e;
}

const uint8_t *blyt_resource_entry_data(blyt_resource_entry_t *e) {
    if (!e)
        return NULL;
    if (e->data)
        return e->data; /* zero-copy (none) or already-materialized (zstd) */
    if (e->algo != BLYT_RES_ALGO_ZSTD)
        return NULL; /* a NONE entry always has data set; nothing to decode */
    /* Decompress the cart-map frame into an owned buffer, cached for reuse. */
    uint8_t *buf = malloc(e->len ? e->len : 1);
    if (!buf)
        return NULL;
    if (blyt_res_decode(e->algo, e->cdata, e->clen, buf, e->len) != 0) {
        free(buf);
        return NULL;
    }
    e->owned = buf;
    e->data = buf;
    return e->data;
}

/* Context + callback for the shared `.cart.resource.<id>` section enumerator. */
struct load_from_cart_ctx {
    blyt_resource_table_t *t;
    const uint8_t *map;
};

static void load_resource_section(const char *suffix, uint32_t suffix_len, uint32_t off,
                                  uint32_t size, void *vctx) {
    struct load_from_cart_ctx *c = (struct load_from_cart_ctx *)vctx;
    uint32_t id;
    if (!blyt_elf32_parse_u32(suffix, suffix_len, &id))
        return; /* not a numeric resource section */
    blyt_resource_entry_t *e = table_push(c->t);
    if (!e)
        return; /* OOM: stop adding; entries so far stay valid */
    e->id = id;
    e->owned = NULL;

    /* Each packed section carries an 8-byte compression header (#157). Parse it:
     * NONE serves the body zero-copy (data aliases the map at off+8); ZSTD keeps
     * the compressed body for lazy decode and exposes the decompressed length up
     * front. A malformed/too-small section falls back to serving it verbatim. */
    const uint8_t *section = c->map + off;
    uint8_t algo;
    uint32_t dsize;
    const uint8_t *body;
    size_t body_len;
    if (!blyt_res_header_parse(section, size, &algo, &dsize, &body, &body_len)) {
        e->algo = BLYT_RES_ALGO_NONE;
        e->data = section;
        e->len = size;
        e->cdata = NULL;
        e->clen = 0;
        return;
    }
    e->algo = algo;
    e->len = dsize;
    if (algo == BLYT_RES_ALGO_ZSTD) {
        e->data = NULL; /* decoded lazily on first access */
        e->cdata = body;
        e->clen = body_len;
    } else {
        e->data = body; /* zero-copy alias into the cart map */
        e->cdata = NULL;
        e->clen = 0;
    }
}

size_t blyt_resource_table_load_from_cart(blyt_resource_table_t *t, const blyt_cart_t *cart) {
    blyt_resource_table_clear(t);
    if (!cart || !cart->map)
        return 0;

    /* Enumerate `.cart.resource.<id>` sections through the shared ELF walk so the
     * host and the native bare-metal path discover resources identically (#141). */
    struct load_from_cart_ctx ctx = {t, (const uint8_t *)cart->map};
    blyt_elf32_for_each_section_prefix((const uint8_t *)cart->map, cart->map_size,
                                       RESOURCE_SECTION_PREFIX, load_resource_section, &ctx);
    return t->count;
}

/* Read an entire file into a freshly malloc'd buffer (NUL-terminated for
 * convenience, though the length is authoritative). Returns NULL on failure. */
static uint8_t *read_whole_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    uint8_t *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) {
        free(buf);
        return NULL;
    }
    buf[sz] = '\0';
    *len_out = (size_t)sz;
    return buf;
}

int blyt_resource_table_load_from_index(blyt_resource_table_t *t, const char *dir) {
    blyt_resource_table_clear(t);
    if (!dir)
        return -1;

    char index_path[4096];
    snprintf(index_path, sizeof(index_path), "%s/resource-id-index", dir);
    FILE *f = fopen(index_path, "rb");
    if (!f)
        return -1;

    char line[4096];
    int ok = 0;
    while (fgets(line, sizeof(line), f)) {
        /* Format: "<id> <relpath>\n" */
        char *sp = strchr(line, ' ');
        if (!sp)
            continue;
        *sp = '\0';
        char *rel = sp + 1;
        /* strip trailing newline / CR */
        size_t rl = strlen(rel);
        while (rl > 0 && (rel[rl - 1] == '\n' || rel[rl - 1] == '\r'))
            rel[--rl] = '\0';
        char *end = NULL;
        unsigned long id = strtoul(line, &end, 10);
        if (end == line || *end != '\0' || id == 0 || rl == 0)
            continue;

        char data_path[4096];
        snprintf(data_path, sizeof(data_path), "%s/%s", dir, rel);
        size_t len = 0;
        uint8_t *bytes = read_whole_file(data_path, &len);
        if (!bytes)
            continue; /* skip missing staged file; keep loading the rest */

        blyt_resource_entry_t *e = table_push(t);
        if (!e) {
            free(bytes);
            break;
        }
        e->id = (uint32_t)id;
        e->data = bytes;
        e->len = len;
        e->owned = bytes;
        ok = 1;
    }
    fclose(f);
    return ok ? 0 : -1;
}
