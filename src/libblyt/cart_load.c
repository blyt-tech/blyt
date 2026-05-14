#include "cart_load.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cart_info_reader.h"
#include "cart_config_reader.h"

/* -------------------------------------------------------------------------
 * Section name allowlist (ADR-0112: reject unknown ELF sections)
 * ------------------------------------------------------------------------- */

static const char *const KNOWN_SECTIONS_EXACT[] = {
    "",                    /* SHT_NULL */
    ".text",
    ".data",
    ".bss",
    ".rodata",
    ".dynamic",
    ".dynsym",
    ".dynstr",
    ".plt",
    ".got",
    ".got.plt",
    ".symtab",
    ".strtab",
    ".shstrtab",
    ".gnu.hash",
    ".hash",
    ".eh_frame",
    ".eh_frame_hdr",
    ".comment",
    ".gnu.version",
    ".gnu.version_r",
    ".gnu.version_d",
    ".cart.info",
    ".cart.config",
    ".cart.resources",
    ".cart.lua",
    ".cart.layouts",
    ".lua_exports",
    NULL,
};

static const char *const KNOWN_SECTIONS_PREFIX[] = {
    ".text.",
    ".data.",
    ".bss.",
    ".rodata.",
    ".rel.",
    ".rela.",
    ".note.",
    ".debug_",
    ".zdebug_",
    ".gnu.",
    NULL,
};

static int section_name_known(const char *name) {
    for (int i = 0; KNOWN_SECTIONS_EXACT[i]; i++) {
        if (strcmp(name, KNOWN_SECTIONS_EXACT[i]) == 0)
            return 1;
    }
    for (int i = 0; KNOWN_SECTIONS_PREFIX[i]; i++) {
        if (strncmp(name, KNOWN_SECTIONS_PREFIX[i],
                    strlen(KNOWN_SECTIONS_PREFIX[i])) == 0)
            return 1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * DT_NEEDED allowlist (ADR-0024 + ADR-0120 amendment)
 *
 * Valid DT_NEEDED sets:
 *   { libblyt32.so }
 *   { libblyt32.so, libblyt32lua.so }
 *   { libblyt32.so, libblytc.so }
 *   { libblyt32.so, libblyt32lua.so, libblytc.so }
 * ------------------------------------------------------------------------- */

static const char *const NEEDED_ALLOWLIST[] = {
    "libblyt32.so",
    "libblyt32lua.so",
    "libblytc.so",
    NULL,
};

static int needed_name_allowed(const char *name) {
    for (int i = 0; NEEDED_ALLOWLIST[i]; i++) {
        if (strcmp(name, NEEDED_ALLOWLIST[i]) == 0)
            return 1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Bounds-checked pointer helpers
 * ------------------------------------------------------------------------- */

static int in_bounds(const void *map, size_t map_size,
                     const void *ptr, size_t len) {
    const uint8_t *base = (const uint8_t *)map;
    const uint8_t *p    = (const uint8_t *)ptr;
    if (p < base || len > map_size)
        return 0;
    if ((size_t)(p - base) > map_size - len)
        return 0;
    return 1;
}

/* -------------------------------------------------------------------------
 * Preamble validation (ADR-0073)
 *
 * Layout: [4-byte ASCII tag][uint16-LE major][uint16-LE minor][FlatBuffers…]
 * Returns a pointer to the start of the FlatBuffers buffer, or NULL on error.
 * ------------------------------------------------------------------------- */

static const void *check_preamble(const void *sect_data, size_t sect_size,
                                  const char *expected_tag) {
    if (sect_size < SECT_PREAMBLE_SIZE)
        return NULL;
    if (memcmp(sect_data, expected_tag, 4) != 0)
        return NULL;
    /* Major version must be 0 for this runtime. */
    const uint8_t *p = (const uint8_t *)sect_data;
    uint16_t major = (uint16_t)(p[4] | (p[5] << 8));
    if (major != 0)
        return NULL;
    return p + SECT_PREAMBLE_SIZE;
}

/* -------------------------------------------------------------------------
 * api_version parsing: "MAJOR.MINOR" → major component
 * Returns -1 on parse error.
 * ------------------------------------------------------------------------- */

static int parse_api_major(const char *s) {
    if (!s || *s == '\0')
        return -1;
    char *end;
    long major = strtol(s, &end, 10);
    if (end == s || (*end != '.' && *end != '\0') || major < 0 || major > 9999)
        return -1;
    return (int)major;
}

/* -------------------------------------------------------------------------
 * blyt_cart_open
 * ------------------------------------------------------------------------- */

blyt_cart_err_t blyt_cart_open(const char *path, blyt_cart_t **out) {
    *out = NULL;

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return BLYT_CART_ERR_IO;

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return BLYT_CART_ERR_IO;
    }

    if ((size_t)st.st_size < sizeof(Elf32_Ehdr)) {
        close(fd);
        return BLYT_CART_ERR_TOO_SMALL;
    }

    void *map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return BLYT_CART_ERR_IO;
    }

    size_t map_size = (size_t)st.st_size;
    blyt_cart_err_t err;

    /* --- ELF identity checks (ADR-0024) ---------------------------------- */

    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)map;

    if (eh->e_ident[EI_MAG0] != ELFMAG0 || eh->e_ident[EI_MAG1] != ELFMAG1 ||
        eh->e_ident[EI_MAG2] != ELFMAG2 || eh->e_ident[EI_MAG3] != ELFMAG3) {
        err = BLYT_CART_ERR_NOT_ELF;
        goto fail;
    }

    if (eh->e_ident[EI_CLASS] != ELFCLASS32) {
        err = BLYT_CART_ERR_BAD_CLASS;
        goto fail;
    }

    if (eh->e_ident[EI_DATA] != ELFDATA2LSB) {
        err = BLYT_CART_ERR_BAD_ENDIAN;
        goto fail;
    }

    if (eh->e_ident[EI_OSABI] != ELFOSABI_NONE) {
        err = BLYT_CART_ERR_BAD_OSABI;
        goto fail;
    }

    if (eh->e_machine != EM_RISCV) {
        err = BLYT_CART_ERR_BAD_MACHINE;
        goto fail;
    }

    if (eh->e_flags != BLYT_CART_EF_FLAGS) {
        err = BLYT_CART_ERR_BAD_FLAGS;
        goto fail;
    }

    /* --- Section header table bounds check ------------------------------- */

    if (eh->e_shentsize < sizeof(Elf32_Shdr) || eh->e_shnum == 0 ||
        eh->e_shstrndx >= eh->e_shnum) {
        err = BLYT_CART_ERR_BAD_SHDR;
        goto fail;
    }

    /* Overflow-safe: e_shoff + shnum * shentsize */
    size_t shdr_total;
    if (__builtin_mul_overflow((size_t)eh->e_shnum,
                               (size_t)eh->e_shentsize, &shdr_total) ||
        __builtin_add_overflow((size_t)eh->e_shoff, shdr_total, &shdr_total) ||
        shdr_total > map_size) {
        err = BLYT_CART_ERR_BAD_SHDR;
        goto fail;
    }

    const uint8_t *shdr_base = (const uint8_t *)map + eh->e_shoff;
    const Elf32_Shdr *shdrs  = (const Elf32_Shdr *)shdr_base;

    /* --- String table for section names ---------------------------------- */

    const Elf32_Shdr *shstrtab_hdr = &shdrs[eh->e_shstrndx];
    if (!in_bounds(map, map_size,
                   (const uint8_t *)map + shstrtab_hdr->sh_offset,
                   shstrtab_hdr->sh_size)) {
        err = BLYT_CART_ERR_BAD_SHDR;
        goto fail;
    }
    const char *shstrtab =
        (const char *)((const uint8_t *)map + shstrtab_hdr->sh_offset);
    size_t shstrtab_size = shstrtab_hdr->sh_size;

    /* --- Walk sections: validate names, locate key sections -------------- */

    const Elf32_Shdr *sect_cart_info   = NULL;
    const Elf32_Shdr *sect_cart_config = NULL;
    const Elf32_Shdr *sect_dynamic     = NULL;

    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        const Elf32_Shdr *sh = &shdrs[i];

        /* Bounds-check sh_name within string table */
        if (sh->sh_name >= shstrtab_size) {
            err = BLYT_CART_ERR_BAD_SHDR;
            goto fail;
        }

        const char *name = shstrtab + sh->sh_name;

        if (!section_name_known(name)) {
            err = BLYT_CART_ERR_UNKNOWN_SECT;
            goto fail;
        }

        /* Bounds-check section data for non-NOBITS sections */
        if (sh->sh_type != SHT_NOBITS && sh->sh_size > 0) {
            if (!in_bounds(map, map_size,
                           (const uint8_t *)map + sh->sh_offset,
                           sh->sh_size)) {
                err = BLYT_CART_ERR_BAD_SHDR;
                goto fail;
            }
        }

        if (strcmp(name, ".cart.info")   == 0) sect_cart_info   = sh;
        if (strcmp(name, ".cart.config") == 0) sect_cart_config = sh;
        if (strcmp(name, ".dynamic")     == 0) sect_dynamic     = sh;
    }

    /* --- DT_NEEDED allowlist (ADR-0024 + ADR-0120) ----------------------- */

    if (sect_dynamic) {
        if (sect_dynamic->sh_entsize < sizeof(Elf32_Dyn)) {
            err = BLYT_CART_ERR_BAD_SHDR;
            goto fail;
        }

        /* Locate the dynamic string table via DT_STRTAB / DT_STRSZ */
        const uint8_t *dyn_data =
            (const uint8_t *)map + sect_dynamic->sh_offset;
        size_t dyn_count = sect_dynamic->sh_size / sect_dynamic->sh_entsize;

        const char *dynstr = NULL;
        size_t dynstr_size = 0;

        for (size_t j = 0; j < dyn_count; j++) {
            const Elf32_Dyn *d =
                (const Elf32_Dyn *)(dyn_data + j * sect_dynamic->sh_entsize);
            if (d->d_tag == DT_NULL)
                break;
            if (d->d_tag == DT_STRTAB) {
                /* d_val is a virtual address; find matching SHT_STRTAB */
                for (uint16_t k = 0; k < eh->e_shnum; k++) {
                    if (shdrs[k].sh_type == SHT_STRTAB &&
                        shdrs[k].sh_addr == d->d_un.d_val) {
                        dynstr = (const char *)((const uint8_t *)map +
                                               shdrs[k].sh_offset);
                        break;
                    }
                }
            }
            if (d->d_tag == DT_STRSZ)
                dynstr_size = d->d_un.d_val;
        }

        /* Walk DT_NEEDED entries */
        for (size_t j = 0; j < dyn_count; j++) {
            const Elf32_Dyn *d =
                (const Elf32_Dyn *)(dyn_data + j * sect_dynamic->sh_entsize);
            if (d->d_tag == DT_NULL)
                break;
            if (d->d_tag != DT_NEEDED)
                continue;
            if (!dynstr || d->d_un.d_val >= dynstr_size) {
                err = BLYT_CART_ERR_BAD_NEEDED;
                goto fail;
            }
            const char *soname = dynstr + d->d_un.d_val;
            if (!needed_name_allowed(soname)) {
                err = BLYT_CART_ERR_BAD_NEEDED;
                goto fail;
            }
        }
    }

    /* --- .cart.info: required (ADR-0024) --------------------------------- */

    if (!sect_cart_info) {
        err = BLYT_CART_ERR_NO_CART_INFO;
        goto fail;
    }

    {
        const void *sect_data =
            (const uint8_t *)map + sect_cart_info->sh_offset;
        const void *fb = check_preamble(sect_data, sect_cart_info->sh_size,
                                        CART_INFO_TAG);
        if (!fb) {
            err = BLYT_CART_ERR_BAD_PREAMBLE;
            goto fail;
        }

        blyt_CartInfo_table_t info = blyt_CartInfo_as_root(fb);
        if (!info) {
            err = BLYT_CART_ERR_BAD_CART_INFO;
            goto fail;
        }

        flatbuffers_string_t api_ver = blyt_CartInfo_api_version(info);
        int major = parse_api_major(api_ver);
        if (major < 0 || major != BLYT_API_VERSION_MAJOR) {
            err = BLYT_CART_ERR_API_VERSION;
            goto fail;
        }
    }

    /* --- .cart.config: optional but validated if present ----------------- */

    if (sect_cart_config) {
        const void *sect_data =
            (const uint8_t *)map + sect_cart_config->sh_offset;
        const void *fb = check_preamble(sect_data, sect_cart_config->sh_size,
                                        CART_CONFIG_TAG);
        if (!fb) {
            err = BLYT_CART_ERR_BAD_PREAMBLE;
            goto fail;
        }

        blyt_CartConfig_table_t config = blyt_CartConfig_as_root(fb);
        if (!config) {
            err = BLYT_CART_ERR_BAD_CART_CONFIG;
            goto fail;
        }
    }

    /* --- Success --------------------------------------------------------- */

    blyt_cart_t *cart = malloc(sizeof(*cart));
    if (!cart) {
        err = BLYT_CART_ERR_IO;
        goto fail;
    }
    cart->fd       = fd;
    cart->map      = map;
    cart->map_size = map_size;
    *out = cart;
    return BLYT_CART_OK;

fail:
    munmap(map, map_size);
    close(fd);
    return err;
}

void blyt_cart_close(blyt_cart_t *cart) {
    if (!cart)
        return;
    munmap(cart->map, cart->map_size);
    close(cart->fd);
    free(cart);
}

const char *blyt_cart_err_str(blyt_cart_err_t err) {
    switch (err) {
    case BLYT_CART_OK:                return "ok";
    case BLYT_CART_ERR_IO:            return "I/O error";
    case BLYT_CART_ERR_TOO_SMALL:     return "file too small";
    case BLYT_CART_ERR_NOT_ELF:       return "not an ELF file";
    case BLYT_CART_ERR_BAD_CLASS:     return "not a 32-bit ELF";
    case BLYT_CART_ERR_BAD_ENDIAN:    return "not little-endian";
    case BLYT_CART_ERR_BAD_OSABI:     return "EI_OSABI must be ELFOSABI_NONE";
    case BLYT_CART_ERR_BAD_MACHINE:   return "not a RISC-V ELF";
    case BLYT_CART_ERR_BAD_FLAGS:     return "e_flags: expected RV32IMAFC ILP32F";
    case BLYT_CART_ERR_BAD_SHDR:      return "section header table malformed";
    case BLYT_CART_ERR_UNKNOWN_SECT:  return "unknown ELF section";
    case BLYT_CART_ERR_BAD_NEEDED:    return "DT_NEEDED: library not on allowlist";
    case BLYT_CART_ERR_NO_CART_INFO:  return ".cart.info section missing";
    case BLYT_CART_ERR_BAD_PREAMBLE:  return "section preamble invalid";
    case BLYT_CART_ERR_BAD_CART_INFO: return ".cart.info: FlatBuffers parse error";
    case BLYT_CART_ERR_BAD_CART_CONFIG: return ".cart.config: FlatBuffers parse error";
    case BLYT_CART_ERR_API_VERSION:   return "api_version not supported";
    }
    return "unknown error";
}
