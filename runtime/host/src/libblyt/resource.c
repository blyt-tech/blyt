#include "resource.h"

#include "cart_load.h" /* struct blyt_cart, Elf32_* */
#include "elf32.h"

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
    e->rl = (blyt_rl_state_t){0};
    return e;
}

size_t blyt_resource_table_load_from_cart(blyt_resource_table_t *t, const blyt_cart_t *cart) {
    blyt_resource_table_clear(t);
    if (!cart || !cart->map)
        return 0;

    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)cart->map;
    const Elf32_Shdr *shdrs = (const Elf32_Shdr *)((const uint8_t *)cart->map + eh->e_shoff);
    const Elf32_Shdr *shstrtab_hdr = &shdrs[eh->e_shstrndx];
    const char *shstrtab = (const char *)((const uint8_t *)cart->map + shstrtab_hdr->sh_offset);

    const size_t prefix_len = sizeof(RESOURCE_SECTION_PREFIX) - 1;
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        const Elf32_Shdr *sh = &shdrs[i];
        const char *name = shstrtab + sh->sh_name;
        if (strncmp(name, RESOURCE_SECTION_PREFIX, prefix_len) != 0)
            continue;
        const char *id_str = name + prefix_len;
        char *end = NULL;
        unsigned long id = strtoul(id_str, &end, 10);
        if (end == id_str || *end != '\0' || id == 0)
            continue; /* not a numeric resource section */

        blyt_resource_entry_t *e = table_push(t);
        if (!e)
            break;
        e->id = (uint32_t)id;
        e->data = (const uint8_t *)cart->map + sh->sh_offset;
        e->len = sh->sh_size;
        e->owned = NULL; /* aliases the cart map */
    }
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
