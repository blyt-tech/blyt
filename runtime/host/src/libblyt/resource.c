#include "resource.h"

#include "blyt_elf_section.h" /* runtime/shared: prefix enumerate + id parse (#141) */
#include "blyt_mem_budget.h" /* runtime/shared: 16 MB budget cap (#160 preload guard) */
#include "blyt_resource_codec.h" /* runtime/shared: per-resource compression (#157) */
#include "cart_load.h" /* struct blyt_cart */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RESOURCE_SECTION_PREFIX ".cart.resource."

/* Forward decl: dev-staging rehydration (#137) re-reads the staging file from
 * blyt_resource_entry_data(); the definition lives with the dev-index loader. */
static uint8_t *read_whole_file(const char *path, size_t *len_out);

void blyt_resource_table_init(blyt_resource_table_t *t) {
    t->entries = NULL;
    t->count = 0;
    t->cap = 0;
    t->lru_clock = 0;
}

void blyt_resource_table_clear(blyt_resource_table_t *t) {
    for (size_t i = 0; i < t->count; i++) {
        free(t->entries[i].owned);
        free(t->entries[i].src_path);
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
    e->src_path = NULL;
    e->rl = (blyt_rl_state_t){0};
    e->last_access = 0;
    e->persistent = false;
    return e;
}

blyt_resource_entry_t *blyt_resource_table_test_push(blyt_resource_table_t *t) {
    return table_push(t);
}

const uint8_t *blyt_resource_entry_data(blyt_resource_entry_t *e) {
    if (!e)
        return NULL;
    if (e->data)
        return e->data; /* zero-copy (none) or already-materialized (zstd) */
    /* Not resident. Rehydrate from the entry's source (#137): a dev-staging
     * entry re-reads its staging file (the only copy of its bytes), a packed
     * zstd entry re-decodes from the still-mapped frame. */
    if (e->src_path) {
        size_t len = 0;
        uint8_t *buf = read_whole_file(e->src_path, &len);
        if (!buf)
            return NULL;
        e->owned = buf;
        e->data = buf;
        e->len = len;
        return e->data;
    }
    if (e->algo != BLYT_RES_ALGO_ZSTD)
        return NULL; /* a NONE packed entry always has data set; nothing to decode */
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

size_t blyt_resource_entry_evict(blyt_resource_entry_t *e) {
    if (!e)
        return 0;
    /* The refcount predicate is the hard gate (single-sourced in runtime/shared);
     * persistence (ADR-0028, #160) ANDs in: a persistent entry is never evicted,
     * even with no outstanding load/pin. */
    if (e->persistent || !blyt_rl_is_evictable(&e->rl))
        return 0;
    if (!e->owned)
        return 0; /* no owned bytes: map-aliased uncompressed, or already evicted */
    size_t reclaimed = e->len;
    free(e->owned);
    e->owned = NULL;
    e->data = NULL; /* not resident; next blyt_resource_entry_data() rehydrates */
    return reclaimed;
}

size_t blyt_resource_table_evict_all_evictable(blyt_resource_table_t *t) {
    size_t total = 0;
    for (size_t i = 0; i < t->count; i++)
        total += blyt_resource_entry_evict(&t->entries[i]);
    return total;
}

/* ── Unified-budget accounting + eviction policy (ADR-0008/0027 #158) ──────── */

uint32_t blyt_resource_table_footprint(const blyt_resource_table_t *t) {
    uint32_t total = 0;
    for (size_t i = 0; i < t->count; i++) {
        /* Non-evictable = referenced (loaded/pinned) OR persistent (#160). Count
         * e->len up front, materialized or not (#157): a persistent resource
         * reserves the budget from cart start (ADR-0028). */
        const blyt_resource_entry_t *e = &t->entries[i];
        if (e->persistent || !blyt_rl_is_evictable(&e->rl))
            total += (uint32_t)e->len;
    }
    return total;
}

uint32_t blyt_resource_table_resident_evictable(const blyt_resource_table_t *t) {
    uint32_t total = 0;
    for (size_t i = 0; i < t->count; i++) {
        const blyt_resource_entry_t *e = &t->entries[i];
        /* Reclaimable cache excludes persistent entries: they hold owned bytes
         * but are non-evictable, so they are never "resident evictable" (#160). */
        if (e->owned && !e->persistent && blyt_rl_is_evictable(&e->rl))
            total += (uint32_t)e->len;
    }
    return total;
}

uint32_t blyt_resource_table_resident_decompressed(const blyt_resource_table_t *t) {
    uint32_t total = 0;
    for (size_t i = 0; i < t->count; i++) {
        const blyt_resource_entry_t *e = &t->entries[i];
        if (e->owned) /* owned == materialized decompressed/staged buffer */
            total += (uint32_t)e->len;
    }
    return total;
}

void blyt_resource_table_touch(blyt_resource_table_t *t, blyt_resource_entry_t *e) {
    e->last_access = ++t->lru_clock;
}

size_t blyt_resource_table_evict_to_fit(blyt_resource_table_t *t, uint32_t max_resident_evictable) {
    size_t freed = 0;
    /* Repeatedly evict the least-recently-used evictable resident entry until the
     * resident evictable cache fits, or none remain. LRU-incremental: the inner
     * scan picks the single oldest victim each pass, so we free the minimum and
     * stop the instant we fit (ADR-0027 #158). */
    for (;;) {
        if (blyt_resource_table_resident_evictable(t) <= max_resident_evictable)
            return freed;
        blyt_resource_entry_t *victim = NULL;
        for (size_t i = 0; i < t->count; i++) {
            blyt_resource_entry_t *e = &t->entries[i];
            /* Persistent entries are never victims (#160) — and excluding them
             * here is also what keeps this loop from spinning on one that
             * entry_evict would refuse to free. */
            if (!e->owned || e->persistent || !blyt_rl_is_evictable(&e->rl))
                continue;
            if (!victim || e->last_access < victim->last_access)
                victim = e;
        }
        if (!victim)
            return freed; /* nothing left to reclaim */
        freed += blyt_resource_entry_evict(victim);
    }
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

/* ── Persistent resources (ADR-0028, #160) ──────────────────────────────────── */

#define PERSISTENT_SECTION ".cart.persistent"

/* Parse a little-endian u32 at `p`. */
static uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void blyt_resource_table_mark_persistent(blyt_resource_table_t *t, const uint8_t *ids_le,
                                         size_t n_bytes) {
    if (!ids_le)
        return;
    for (size_t off = 0; off + 4 <= n_bytes; off += 4) {
        uint32_t id = read_u32_le(ids_le + off);
        blyt_resource_entry_t *e = blyt_resource_table_find_mut(t, id);
        if (e)
            e->persistent = true; /* unknown ids ignored (packer validates) */
    }
}

void blyt_resource_table_load_persistent_from_cart(blyt_resource_table_t *t,
                                                   const blyt_cart_t *cart) {
    if (!cart || !cart->map)
        return;
    uint32_t off = 0, size = 0;
    if (!blyt_elf32_find_section((const uint8_t *)cart->map, cart->map_size, PERSISTENT_SECTION,
                                 &off, &size))
        return; /* no persistent set declared */
    blyt_resource_table_mark_persistent(t, (const uint8_t *)cart->map + off, size);
}

int blyt_resource_table_preload_persistent(blyt_resource_table_t *t) {
    /* Layer-2 budget guard (#160): a persistent set whose decompressed total
     * exceeds the budget is rejected before any byte is materialised — refuse to
     * start rather than over-subscribe. footprint() already sums persistent len. */
    if (blyt_resource_table_footprint(t) > BLYT_MEM_BUDGET_BYTES)
        return -1;
    for (size_t i = 0; i < t->count; i++) {
        blyt_resource_entry_t *e = &t->entries[i];
        if (!e->persistent)
            continue;
        /* Materialise so the bytes are resident before init() runs. A decode/OOM
         * failure on a declared-essential resource fails cart start. */
        if (blyt_resource_entry_data(e) == NULL)
            return -1;
    }
    return 0;
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
        /* Remember the staging path so eviction can re-read it (#137): for a
         * dev-staging entry `owned` is the only copy of the bytes. strdup may
         * fail under OOM — then the entry simply stays non-rehydratable. */
        e->src_path = strdup(data_path);
        ok = 1;
    }
    fclose(f);
    return ok ? 0 : -1;
}
