#include "cart_load.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cart_config_reader.h"
#include "cart_config_verifier.h"
#include "cart_info_reader.h"
#include "cart_info_verifier.h"

/* -------------------------------------------------------------------------
 * Section name allowlist (ADR-0112: reject unknown ELF sections)
 * ------------------------------------------------------------------------- */

/* Interpreter path required in every cart (ADR-0112, ADR-0119).
 * The emulated-path dynlinker ignores this; the native launcher uses it.
 * Carts with a different PT_INTERP are rejected — unknown interpreter. */
#define BLYT_INTERP_PATH "/lib/ld-blyt.so.1"

static const char *const KNOWN_SECTIONS_EXACT[] = {
    "",
    ".interp",
    ".text",
    ".data",
    ".bss",
    ".sbss", /* RISC-V small-data BSS (GP-relative) */
    ".sdata", /* RISC-V small-data (GP-relative) */
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
    ".text.", ".data.",  ".bss.",    ".rodata.", ".rel.",   ".rela.",
    ".note.", ".debug_", ".zdebug_", ".gnu.",    ".riscv.", NULL,
};

static int section_name_known(const char *name) {
    for (int i = 0; KNOWN_SECTIONS_EXACT[i]; i++)
        if (strcmp(name, KNOWN_SECTIONS_EXACT[i]) == 0)
            return 1;
    for (int i = 0; KNOWN_SECTIONS_PREFIX[i]; i++)
        if (strncmp(name, KNOWN_SECTIONS_PREFIX[i], strlen(KNOWN_SECTIONS_PREFIX[i])) == 0)
            return 1;
    return 0;
}

/* -------------------------------------------------------------------------
 * DT_NEEDED allowlist (ADR-0024 + ADR-0120)
 * Carts may only declare DT_NEEDED: libblyt32.so (and libblyt32lua.so for Lua
 * carts).  libblytc.so is loaded transitively via libblyt32.so's own
 * DT_NEEDED; carts must not list it directly.
 * ------------------------------------------------------------------------- */

static const char *const NEEDED_ALLOWLIST[] = {
    "libblyt32.so",
    "libblyt32lua.so",
    NULL,
};

static int needed_name_allowed(const char *name) {
    for (int i = 0; NEEDED_ALLOWLIST[i]; i++)
        if (strcmp(name, NEEDED_ALLOWLIST[i]) == 0)
            return 1;
    return 0;
}

/* -------------------------------------------------------------------------
 * Symbol import allowlist (ADR-0112)
 * Populated as APIs are implemented. Carts with STB_GLOBAL/SHN_UNDEF
 * imports not on this list are rejected at load time.
 * ------------------------------------------------------------------------- */

static const char *const SYMBOL_ALLOWLIST[] = {
    /* blyt lifecycle (blyt.h / libblytcommon.so) */
    "blyt_main", /* imported by _blyt_entry */
    "blyt_console_debug",
    "blyt_quit_ready",
    "blyt_frame_done",

    /* libblytc.so — allocator (ADR-0120) */
    "malloc",
    "free",
    "realloc",
    "calloc",
    "abort",

    /* libblytc.so — string */
    "memcpy",
    "memmove",
    "memset",
    "memcmp",
    "memchr",
    "strlen",
    "strcmp",
    "strncmp",
    "strcpy",
    "strncpy",
    "strcat",
    "strncat",
    "strchr",
    "strrchr",
    "strstr",
    "strtok",
    "mempcpy",
    "stpcpy",
    "stpncpy",
    "explicit_bzero",
    "bcopy",
    "bzero",
    "bcmp",

    /* libblytc.so — stdlib */
    "strtol",
    "strtoul",
    "strtoll",
    "strtoull",
    "strtod",
    "strtof",
    "atoi",
    "atol",
    "atoll",
    "atof",
    "qsort",
    "bsearch",
    "abs",
    "labs",
    "llabs",

    /* libblytc.so — printf (snprintf / vsnprintf only; no fd-based I/O) */
    "snprintf",
    "vsnprintf",

    /* libblytc.so — in-memory FILE bridge (ADR-0120) */
    "fmemopen",
    "fclose",
    "fread",
    "fwrite",
    "fseek",
    "ftell",
    "rewind",
    "feof",
    "ferror",
    "clearerr",

    /* libblytc.so — ctype */
    "isalpha",
    "isdigit",
    "isalnum",
    "isspace",
    "ispunct",
    "islower",
    "isupper",
    "isprint",
    "iscntrl",
    "isxdigit",
    "tolower",
    "toupper",

    /* libblytc.so — math (f32 variants; ADR-0005, ADR-0007) */
    "sinf",
    "cosf",
    "tanf",
    "asinf",
    "acosf",
    "atanf",
    "atan2f",
    "expf",
    "exp2f",
    "logf",
    "log2f",
    "log10f",
    "powf",
    "sqrtf",
    "cbrtf",
    "fabsf",
    "floorf",
    "ceilf",
    "roundf",
    "truncf",
    "nearbyintf",
    "rintf",
    "fmodf",
    "remainderf",
    "hypotf",
    "fmaf",
    "ldexpf",
    "frexpf",
    "modff",
    "copysignf",
    "fmaxf",
    "fminf",
    "fdimf",
    "scalbnf",
    "scalblnf",
    "logbf",
    "significandf",
    "ilogbf",
    "j0f",
    "j1f",
    "lgammaf",
    "tgammaf",
    "erff",
    "erfcf",
    "sinh",
    "cosh",
    "tanh",
    "sinhf",
    "coshf",
    "tanhf",
    "asinhf",
    "acoshf",
    "atanhf",
    "expm1f",
    "log1pf",

    /* libblytc.so — math (f64 variants for completeness) */
    "sin",
    "cos",
    "tan",
    "asin",
    "acos",
    "atan",
    "atan2",
    "exp",
    "exp2",
    "log",
    "log2",
    "log10",
    "pow",
    "sqrt",
    "cbrt",
    "fabs",
    "floor",
    "ceil",
    "round",
    "trunc",
    "nearbyint",
    "rint",
    "fmod",
    "remainder",
    "hypot",
    "fma",
    "ldexp",
    "frexp",
    "modf",
    "copysign",
    "fmax",
    "fmin",
    "fdim",
    "scalbn",
    "scalbln",
    "logb",
    "ilogb",

    /* libblytc.so — locale (localeconv; C locale only) */
    "localeconv",

    NULL,
};

/* Required exports: every cart must define these (ADR-0087). */
static const char *const REQUIRED_ENTRY_POINTS[] = {
    "blyt_cart_init",
    "blyt_cart_update",
    "blyt_cart_draw",
    NULL,
};

static int symbol_name_allowed(const char *name) {
    for (int i = 0; SYMBOL_ALLOWLIST[i]; i++)
        if (strcmp(name, SYMBOL_ALLOWLIST[i]) == 0)
            return 1;
    return 0;
}

/* -------------------------------------------------------------------------
 * Bounds helpers
 * ------------------------------------------------------------------------- */

static int in_bounds(const void *map, size_t map_size, const void *ptr, size_t len) {
    const uint8_t *base = (const uint8_t *)map;
    const uint8_t *p = (const uint8_t *)ptr;
    if (p < base || len > map_size)
        return 0;
    if ((size_t)(p - base) > map_size - len)
        return 0;
    return 1;
}

/* Overflow-safe a + b <= limit */
static int u32_add_le(uint32_t a, uint32_t b, size_t limit) {
    uint64_t sum = (uint64_t)a + (uint64_t)b;
    return sum <= (uint64_t)limit;
}

/* Ranges [a, a+alen) and [b, b+blen) overlap? */
static int ranges_overlap(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen) {
    if (alen == 0 || blen == 0)
        return 0;
    return a < (uint64_t)b + blen && b < (uint64_t)a + alen;
}

/* -------------------------------------------------------------------------
 * Preamble validation (ADR-0073)
 * Returns pointer to the FlatBuffers buffer (after preamble), or NULL.
 * ------------------------------------------------------------------------- */

static const void *check_preamble(const void *sect_data, size_t sect_size, const char *expected_tag,
                                  size_t *fb_size_out) {
    if (sect_size < SECT_PREAMBLE_SIZE)
        return NULL;
    if (memcmp(sect_data, expected_tag, 4) != 0)
        return NULL;
    const uint8_t *p = (const uint8_t *)sect_data;
    uint16_t major = (uint16_t)((unsigned)p[4] | ((unsigned)p[5] << 8));
    if (major != 0)
        return NULL;
    *fb_size_out = sect_size - SECT_PREAMBLE_SIZE;
    return p + SECT_PREAMBLE_SIZE;
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

    /* -----------------------------------------------------------------------
     * ELF identity checks (ADR-0024)
     * --------------------------------------------------------------------- */

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

    /* -----------------------------------------------------------------------
     * Program header checks (ADR-0112)
     * --------------------------------------------------------------------- */

    if (eh->e_phnum > 0) {
        /* Bounds-check program header table */
        if (eh->e_phentsize < sizeof(Elf32_Phdr)) {
            err = BLYT_CART_ERR_BAD_SEGMENT;
            goto fail;
        }
        size_t phdr_total;
        if (__builtin_mul_overflow((size_t)eh->e_phnum, (size_t)eh->e_phentsize, &phdr_total) ||
            __builtin_add_overflow((size_t)eh->e_phoff, phdr_total, &phdr_total) ||
            phdr_total > map_size) {
            err = BLYT_CART_ERR_BAD_SEGMENT;
            goto fail;
        }

        const uint8_t *phdr_base = (const uint8_t *)map + eh->e_phoff;

        /* Collect LOAD segments for pairwise overlap check and entry-point check */
        enum { MAX_LOAD = 64 };
        const Elf32_Phdr *loads[MAX_LOAD];
        int nloads = 0;
        int has_relro = 0;
        int has_interp = 0;
        int entry_ok = (eh->e_entry == 0); /* e_entry == 0 means no entry point */

        for (uint16_t i = 0; i < eh->e_phnum; i++) {
            const Elf32_Phdr *ph = (const Elf32_Phdr *)(phdr_base + (size_t)i * eh->e_phentsize);

            if (ph->p_type == PT_INTERP) {
                /* PT_INTERP must equal BLYT_INTERP_PATH exactly (ADR-0119).
                 * The emulated-path dynlinker ignores it; the native launcher
                 * uses it.  Any other interpreter path is rejected. */
                if (!u32_add_le(ph->p_offset, ph->p_filesz, map_size)) {
                    err = BLYT_CART_ERR_BAD_INTERP;
                    goto fail;
                }
                const char *interp = (const char *)map + ph->p_offset;
                if (ph->p_filesz != sizeof(BLYT_INTERP_PATH) ||
                    memcmp(interp, BLYT_INTERP_PATH, sizeof(BLYT_INTERP_PATH)) != 0) {
                    err = BLYT_CART_ERR_BAD_INTERP;
                    goto fail;
                }
                has_interp = 1;
            }

            if (ph->p_type == PT_GNU_RELRO) {
                has_relro = 1;
            }

            if (ph->p_type == PT_GNU_STACK && (ph->p_flags & PF_X)) {
                /* Executable stack forbidden */
                err = BLYT_CART_ERR_BAD_SEGMENT;
                goto fail;
            }

            if (ph->p_type == PT_LOAD) {
                /* W+X forbidden */
                if ((ph->p_flags & (PF_W | PF_X)) == (PF_W | PF_X)) {
                    err = BLYT_CART_ERR_BAD_SEGMENT;
                    goto fail;
                }

                /* p_offset + p_filesz must not exceed file size */
                if (!u32_add_le(ph->p_offset, ph->p_filesz, map_size)) {
                    err = BLYT_CART_ERR_BAD_SEGMENT;
                    goto fail;
                }

                /* e_entry within an executable LOAD segment? */
                if ((ph->p_flags & PF_X) && ph->p_filesz > 0 && eh->e_entry >= ph->p_vaddr &&
                    eh->e_entry < ph->p_vaddr + ph->p_filesz) {
                    entry_ok = 1;
                }

                if (nloads < MAX_LOAD)
                    loads[nloads++] = ph;
            }
        }

        /* e_entry must be in an executable LOAD segment */
        if (!entry_ok) {
            err = BLYT_CART_ERR_BAD_SEGMENT;
            goto fail;
        }

        /* PT_INTERP = /lib/ld-blyt.so.1 required (ADR-0112, ADR-0119) */
        if (!has_interp) {
            err = BLYT_CART_ERR_BAD_INTERP;
            goto fail;
        }

        /* PT_GNU_RELRO required (ADR-0112) */
        if (!has_relro) {
            err = BLYT_CART_ERR_NO_RELRO;
            goto fail;
        }

        /* Pairwise overlap check: file ranges and virtual address ranges */
        for (int i = 0; i < nloads; i++) {
            for (int j = i + 1; j < nloads; j++) {
                if (ranges_overlap(loads[i]->p_offset, loads[i]->p_filesz, loads[j]->p_offset,
                                   loads[j]->p_filesz)) {
                    err = BLYT_CART_ERR_BAD_SEGMENT;
                    goto fail;
                }
                if (ranges_overlap(loads[i]->p_vaddr, loads[i]->p_memsz, loads[j]->p_vaddr,
                                   loads[j]->p_memsz)) {
                    err = BLYT_CART_ERR_BAD_SEGMENT;
                    goto fail;
                }
            }
        }

        /* -----------------------------------------------------------------------
         * Opcode scan: reject ecall/ebreak in executable segments (ADR-0112)
         * Check at every 2-byte-aligned offset to catch both 32-bit and RVC cases.
         * --------------------------------------------------------------------- */

        static const uint8_t ECALL[] = {0x73, 0x00, 0x00, 0x00};
        static const uint8_t EBREAK[] = {0x73, 0x00, 0x10, 0x00};

        for (int i = 0; i < nloads; i++) {
            if (!(loads[i]->p_flags & PF_X))
                continue;
            if (loads[i]->p_filesz < 4)
                continue;

            const uint8_t *seg = (const uint8_t *)map + loads[i]->p_offset;
            size_t seg_size = loads[i]->p_filesz;

            for (size_t off = 0; off + 4 <= seg_size; off += 2) {
                if (memcmp(seg + off, ECALL, 4) == 0 || memcmp(seg + off, EBREAK, 4) == 0) {
                    err = BLYT_CART_ERR_BAD_OPCODE;
                    goto fail;
                }
            }
        }
    }

    /* -----------------------------------------------------------------------
     * Section header table bounds check
     * --------------------------------------------------------------------- */

    if (eh->e_shentsize < sizeof(Elf32_Shdr) || eh->e_shnum == 0 || eh->e_shstrndx >= eh->e_shnum) {
        err = BLYT_CART_ERR_BAD_SHDR;
        goto fail;
    }

    {
        size_t shdr_total;
        if (__builtin_mul_overflow((size_t)eh->e_shnum, (size_t)eh->e_shentsize, &shdr_total) ||
            __builtin_add_overflow((size_t)eh->e_shoff, shdr_total, &shdr_total) ||
            shdr_total > map_size) {
            err = BLYT_CART_ERR_BAD_SHDR;
            goto fail;
        }
    }

    const Elf32_Shdr *shdrs = (const Elf32_Shdr *)((const uint8_t *)map + eh->e_shoff);

    /* String table for section names */
    const Elf32_Shdr *shstrtab_hdr = &shdrs[eh->e_shstrndx];
    if (!in_bounds(map, map_size, (const uint8_t *)map + shstrtab_hdr->sh_offset,
                   shstrtab_hdr->sh_size)) {
        err = BLYT_CART_ERR_BAD_SHDR;
        goto fail;
    }
    const char *shstrtab = (const char *)((const uint8_t *)map + shstrtab_hdr->sh_offset);
    size_t shstrtab_size = shstrtab_hdr->sh_size;

    /* -----------------------------------------------------------------------
     * Walk sections: validate names, locate key sections
     * --------------------------------------------------------------------- */

    const Elf32_Shdr *sect_cart_info = NULL;
    const Elf32_Shdr *sect_cart_config = NULL;
    const Elf32_Shdr *sect_dynamic = NULL;
    const Elf32_Shdr *sect_dynsym = NULL;
    const Elf32_Shdr *sect_dynstr_sh = NULL;

    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        const Elf32_Shdr *sh = &shdrs[i];

        if (sh->sh_name >= shstrtab_size) {
            err = BLYT_CART_ERR_BAD_SHDR;
            goto fail;
        }
        const char *name = shstrtab + sh->sh_name;

        if (!section_name_known(name)) {
            err = BLYT_CART_ERR_UNKNOWN_SECT;
            goto fail;
        }

        if (sh->sh_type != SHT_NOBITS && sh->sh_size > 0) {
            if (!in_bounds(map, map_size, (const uint8_t *)map + sh->sh_offset, sh->sh_size)) {
                err = BLYT_CART_ERR_BAD_SHDR;
                goto fail;
            }
        }

        if (strcmp(name, ".cart.info") == 0)
            sect_cart_info = sh;
        if (strcmp(name, ".cart.config") == 0)
            sect_cart_config = sh;
        if (strcmp(name, ".dynamic") == 0)
            sect_dynamic = sh;
        if (strcmp(name, ".dynstr") == 0)
            sect_dynstr_sh = sh;
        if (sh->sh_type == SHT_DYNSYM)
            sect_dynsym = sh;
    }

    /* -----------------------------------------------------------------------
     * DT_NEEDED allowlist (ADR-0024 + ADR-0120)
     * --------------------------------------------------------------------- */

    if (sect_dynamic) {
        if (sect_dynamic->sh_entsize < sizeof(Elf32_Dyn)) {
            err = BLYT_CART_ERR_BAD_SHDR;
            goto fail;
        }

        const uint8_t *dyn_data = (const uint8_t *)map + sect_dynamic->sh_offset;
        size_t dyn_count = sect_dynamic->sh_size / sect_dynamic->sh_entsize;

        /* Locate dynamic string table */
        const char *dynstr = NULL;
        size_t dynstr_size = 0;

        for (size_t j = 0; j < dyn_count; j++) {
            const Elf32_Dyn *d = (const Elf32_Dyn *)(dyn_data + j * sect_dynamic->sh_entsize);
            if (d->d_tag == DT_NULL)
                break;
            if (d->d_tag == DT_STRTAB) {
                for (uint16_t k = 0; k < eh->e_shnum; k++) {
                    if (shdrs[k].sh_type == SHT_STRTAB && shdrs[k].sh_addr == d->d_un.d_val) {
                        dynstr = (const char *)((const uint8_t *)map + shdrs[k].sh_offset);
                        break;
                    }
                }
            }
            if (d->d_tag == DT_STRSZ)
                dynstr_size = d->d_un.d_val;
        }

        for (size_t j = 0; j < dyn_count; j++) {
            const Elf32_Dyn *d = (const Elf32_Dyn *)(dyn_data + j * sect_dynamic->sh_entsize);
            if (d->d_tag == DT_NULL)
                break;
            if (d->d_tag != DT_NEEDED)
                continue;
            if (!dynstr || d->d_un.d_val >= dynstr_size) {
                err = BLYT_CART_ERR_BAD_NEEDED;
                goto fail;
            }
            if (!needed_name_allowed(dynstr + d->d_un.d_val)) {
                err = BLYT_CART_ERR_BAD_NEEDED;
                goto fail;
            }
        }
    }

    /* -----------------------------------------------------------------------
     * Symbol import allowlist (ADR-0112)
     * Every STB_GLOBAL symbol with SHN_UNDEF must be on the allowlist.
     * --------------------------------------------------------------------- */

    if (sect_dynsym) {
        if (sect_dynsym->sh_entsize < sizeof(Elf32_Sym)) {
            err = BLYT_CART_ERR_BAD_SHDR;
            goto fail;
        }

        /* Find the symbol string table via sh_link */
        const char *symstr = NULL;
        size_t symstr_size = 0;
        if (sect_dynstr_sh) {
            symstr = (const char *)((const uint8_t *)map + sect_dynstr_sh->sh_offset);
            symstr_size = sect_dynstr_sh->sh_size;
        } else if (sect_dynsym->sh_link < eh->e_shnum) {
            const Elf32_Shdr *link_sh = &shdrs[sect_dynsym->sh_link];
            symstr = (const char *)((const uint8_t *)map + link_sh->sh_offset);
            symstr_size = link_sh->sh_size;
        }

        const uint8_t *sym_data = (const uint8_t *)map + sect_dynsym->sh_offset;
        size_t sym_count = sect_dynsym->sh_size / sect_dynsym->sh_entsize;

        for (size_t j = 0; j < sym_count; j++) {
            const Elf32_Sym *sym = (const Elf32_Sym *)(sym_data + j * sect_dynsym->sh_entsize);

            if (ELF32_ST_BIND(sym->st_info) != STB_GLOBAL)
                continue;
            if (sym->st_shndx != SHN_UNDEF)
                continue;

            if (!symstr || sym->st_name >= symstr_size) {
                err = BLYT_CART_ERR_BAD_IMPORT;
                goto fail;
            }
            if (!symbol_name_allowed(symstr + sym->st_name)) {
                err = BLYT_CART_ERR_BAD_IMPORT;
                goto fail;
            }
        }
    }

    /* -----------------------------------------------------------------------
     * .cart.info: required (ADR-0024), verify + parse
     * --------------------------------------------------------------------- */

    if (!sect_cart_info) {
        err = BLYT_CART_ERR_NO_CART_INFO;
        goto fail;
    }

    {
        const void *sect_data = (const uint8_t *)map + sect_cart_info->sh_offset;
        size_t fb_size;
        const void *fb =
            check_preamble(sect_data, sect_cart_info->sh_size, CART_INFO_TAG, &fb_size);
        if (!fb) {
            err = BLYT_CART_ERR_BAD_PREAMBLE;
            goto fail;
        }

        /* The FlatBuffers verifier requires 4-byte-aligned input.  The
         * section may land at any file offset, so copy to an aligned buffer. */
        void *fb_aligned = malloc(fb_size);
        if (!fb_aligned) {
            err = BLYT_CART_ERR_IO;
            goto fail;
        }
        memcpy(fb_aligned, fb, fb_size);
        int verify_result = blyt_CartInfo_verify_as_root(fb_aligned, fb_size);
        blyt_CartInfo_table_t info = blyt_CartInfo_as_root(fb_aligned);
        uint16_t api_major = (verify_result == flatcc_verify_ok && info)
                                 ? blyt_CartInfo_api_version_major(info)
                                 : BLYT_API_VERSION_MAJOR + 1;
        free(fb_aligned);

        if (verify_result != flatcc_verify_ok) {
            err = BLYT_CART_ERR_BAD_CART_INFO;
            goto fail;
        }
        if (!info) {
            err = BLYT_CART_ERR_BAD_CART_INFO;
            goto fail;
        }
        if (api_major != BLYT_API_VERSION_MAJOR) {
            err = BLYT_CART_ERR_API_VERSION;
            goto fail;
        }
    }

    /* -----------------------------------------------------------------------
     * .cart.config: optional, verify if present
     * --------------------------------------------------------------------- */

    if (sect_cart_config) {
        const void *sect_data = (const uint8_t *)map + sect_cart_config->sh_offset;
        size_t fb_size;
        const void *fb =
            check_preamble(sect_data, sect_cart_config->sh_size, CART_CONFIG_TAG, &fb_size);
        if (!fb) {
            err = BLYT_CART_ERR_BAD_PREAMBLE;
            goto fail;
        }

        void *fb_aligned = malloc(fb_size);
        if (!fb_aligned) {
            err = BLYT_CART_ERR_IO;
            goto fail;
        }
        memcpy(fb_aligned, fb, fb_size);
        int verify_result = blyt_CartConfig_verify_as_root(fb_aligned, fb_size);
        free(fb_aligned);

        if (verify_result != flatcc_verify_ok) {
            err = BLYT_CART_ERR_BAD_CART_CONFIG;
            goto fail;
        }
    }

    /* -----------------------------------------------------------------------
     * Required entry point validation (ADR-0087)
     *
     * blyt_cart_init, blyt_cart_update, and blyt_cart_draw must be present
     * as defined (non-UNDEF, STB_GLOBAL) symbols in the cart's .dynsym.
     * --------------------------------------------------------------------- */

    if (sect_dynsym && sect_dynstr_sh) {
        const char *symstr = (const char *)((const uint8_t *)map + sect_dynstr_sh->sh_offset);
        size_t symstr_size = sect_dynstr_sh->sh_size;
        const uint8_t *sym_data = (const uint8_t *)map + sect_dynsym->sh_offset;
        size_t sym_count = sect_dynsym->sh_size / sect_dynsym->sh_entsize;

        for (int r = 0; REQUIRED_ENTRY_POINTS[r]; r++) {
            int found = 0;
            for (size_t j = 0; j < sym_count; j++) {
                const Elf32_Sym *sym = (const Elf32_Sym *)(sym_data + j * sect_dynsym->sh_entsize);
                if (ELF32_ST_BIND(sym->st_info) != STB_GLOBAL)
                    continue;
                if (sym->st_shndx == SHN_UNDEF)
                    continue;
                if (sym->st_name >= symstr_size)
                    continue;
                if (strcmp(symstr + sym->st_name, REQUIRED_ENTRY_POINTS[r]) == 0) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                err = BLYT_CART_ERR_MISSING_ENTRY;
                goto fail;
            }
        }
    }

    /* -----------------------------------------------------------------------
     * Success
     * --------------------------------------------------------------------- */

    {
        blyt_cart_t *cart = malloc(sizeof(*cart));
        if (!cart) {
            err = BLYT_CART_ERR_IO;
            goto fail;
        }
        cart->fd = fd;
        cart->map = map;
        cart->map_size = map_size;
        cart->path = strdup(path);
        if (!cart->path) {
            free(cart);
            err = BLYT_CART_ERR_IO;
            goto fail;
        }
        *out = cart;
        return BLYT_CART_OK;
    }

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
    free(cart->path);
    free(cart);
}

const char *blyt_cart_err_str(blyt_cart_err_t err) {
    switch (err) {
    case BLYT_CART_OK:
        return "ok";
    case BLYT_CART_ERR_IO:
        return "I/O error";
    case BLYT_CART_ERR_TOO_SMALL:
        return "file too small";
    case BLYT_CART_ERR_NOT_ELF:
        return "not an ELF file";
    case BLYT_CART_ERR_BAD_CLASS:
        return "not a 32-bit ELF";
    case BLYT_CART_ERR_BAD_ENDIAN:
        return "not little-endian";
    case BLYT_CART_ERR_BAD_OSABI:
        return "EI_OSABI must be ELFOSABI_NONE";
    case BLYT_CART_ERR_BAD_MACHINE:
        return "not a RISC-V ELF";
    case BLYT_CART_ERR_BAD_FLAGS:
        return "e_flags: expected RV32IMAFC ILP32F";
    case BLYT_CART_ERR_BAD_SHDR:
        return "section header table malformed";
    case BLYT_CART_ERR_UNKNOWN_SECT:
        return "unknown ELF section";
    case BLYT_CART_ERR_BAD_NEEDED:
        return "DT_NEEDED: library not on allowlist";
    case BLYT_CART_ERR_NO_CART_INFO:
        return ".cart.info section missing";
    case BLYT_CART_ERR_BAD_PREAMBLE:
        return "section preamble invalid";
    case BLYT_CART_ERR_BAD_CART_INFO:
        return ".cart.info: FlatBuffers parse error";
    case BLYT_CART_ERR_BAD_CART_CONFIG:
        return ".cart.config: FlatBuffers parse error";
    case BLYT_CART_ERR_API_VERSION:
        return "api_version not supported";
    case BLYT_CART_ERR_BAD_SEGMENT:
        return "segment layout violation";
    case BLYT_CART_ERR_BAD_INTERP:
        return "PT_INTERP must be /lib/ld-blyt.so.1";
    case BLYT_CART_ERR_NO_RELRO:
        return "PT_GNU_RELRO required but absent";
    case BLYT_CART_ERR_BAD_OPCODE:
        return "ecall or ebreak in executable segment";
    case BLYT_CART_ERR_BAD_IMPORT:
        return "imported symbol not on allowlist";
    case BLYT_CART_ERR_MISSING_ENTRY:
        return "required cart entry point missing (blyt_cart_init/update/draw)";
    }
    return "unknown error";
}
