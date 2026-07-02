#include "cart_load.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "blyt_elf_section.h" /* runtime/shared: blyt_elf32_find_section (#128) */
#include "cart_config_reader.h"
#include "cart_config_verifier.h"
#include "cart_info_reader.h"
#include "cart_info_verifier.h"
#include "cart_layouts_reader.h"
#include "cart_layouts_verifier.h"

/* -------------------------------------------------------------------------
 * Section name denylist (ADR-0112 amendment: reject constructor/destructor
 * sections that would execute at load time, bypassing the cart lifecycle)
 * ------------------------------------------------------------------------- */

/* Interpreter path required in every cart (ADR-0112, ADR-0119).
 * The emulated-path dynlinker ignores this; the native launcher uses it.
 * Carts with a different PT_INTERP are rejected — unknown interpreter. */
#define BLYT_INTERP_PATH "/lib/ld-blyt.so.1"

static const char *const DENIED_SECTIONS[] = {
    ".init_array", ".fini_array", ".preinit_array", ".ctors", ".dtors", NULL,
};

static int section_name_denied(const char *name) {
    for (int i = 0; DENIED_SECTIONS[i]; i++)
        if (strcmp(name, DENIED_SECTIONS[i]) == 0)
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
    "blyt_exit", /* imported by _blyt_entry; calls exit_group on native path */
    "blyt_runtime_startup", /* imported by _blyt_entry; seccomp+FCSR init (issue #43) */
    "blyt_console_debug",
    "blyt_quit",
    "blyt_frame_done",

    /* blyt state buffer API (ADR-0009, ADR-0010, ADR-0057, ADR-0058) */
    "blyt_buffer_get_f32",
    "blyt_buffer_set_f32",
    "blyt_buffer_get_f64",
    "blyt_buffer_set_f64",
    "blyt_buffer_get_i32",
    "blyt_buffer_set_i32",
    "blyt_buffer_get_u32",
    "blyt_buffer_set_u32",
    "blyt_buffer_get_i16",
    "blyt_buffer_set_i16",
    "blyt_buffer_get_u16",
    "blyt_buffer_set_u16",
    "blyt_buffer_get_i8",
    "blyt_buffer_set_i8",
    "blyt_buffer_get_u8",
    "blyt_buffer_set_u8",
    "blyt_buffer_get_bool",
    "blyt_buffer_set_bool",
    "blyt_buffer_alloc_slot",
    "blyt_buffer_free_slot",
    /* Packed entity refs (ADR-0096); blyt_buffer_ref_slot is a static inline
     * in blyt.h (pure bit math) and is deliberately not an import. */
    "blyt_buffer_ref",
    "blyt_buffer_ref_valid",

    /* blyt save/load API (ADR-0087, ADR-0125) */
    "blyt_save_write",
    "blyt_save_read",

    /* blyt resource API (ADR-0027, ADR-0040, ADR-0088, ADR-0134, #91/#123/#162/#196).
     * pin/unpin are ECALL stubs in libblytcommon; the blyt_resource_text_get /
     * blyt_resource_bytes_get conveniences are guest-side helpers (also in
     * libblytcommon) built from pin -> copy -> unpin.  load/release are gone — a
     * resource is referenced by its baked constant directly (ADR-0134). */
    "blyt_resource_pin",
    "blyt_resource_unpin",
    "blyt_resource_text_get",
    "blyt_resource_bytes_get",

    /* blyt graphics / surface API (ADR-0052/0086/0008, #188 / #195 / #205).
     * Tier-1 serviced ops + surface lifecycle exported by libblyt32.so;
     * host-side on the emulated path.  gfx.* is inline sugar over these
     * (blyt.h), so blyt_gfx_{clear,pixel,rect_fill,line} are not imported. */
    "blyt_surface_create",
    "blyt_surface_destroy",
    "blyt_surface_clear",
    "blyt_surface_pixel",
    "blyt_surface_rect_fill",
    "blyt_surface_line",
    "blyt_surface_blit",
    "blyt_surface_acquire",
    "blyt_surface_release",
    "blyt_gfx_acquire",
    "blyt_gfx_present",
    "blyt_gfx_palette_set", /* #201 */
    /* Tier-2 in-lock primitives (#205): the freestanding rasterizer, exported by
     * libblyt32.so so a cart holding a lock draws guest-side with no ECALL. */
    "blyt_raster_clear",
    "blyt_raster_pixel",
    "blyt_raster_rect_fill",
    "blyt_raster_line",
    "blyt_raster_blit",

    /* blyt memory introspection (ADR-0029, #159): blyt_mem_stats reads the
     * guest-visible accounting block (no ECALL); blyt_mem_resources is the
     * on-demand loaded-list ECALL stub. Both live in libblytcommon. */
    "blyt_mem_stats",
    "blyt_mem_resources",

    /* libblytc.so — allocator (ADR-0120) */
    "malloc",
    "free",
    "realloc",
    "calloc",
    "aligned_alloc", /* C++ over-aligned operator new (libc++) */
    "posix_memalign",
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
    "strtok_r",
    "strdup",
    "strndup",
    "strcasecmp",
    "strncasecmp",
    "mempcpy",
    "stpcpy",
    "stpncpy",
    "explicit_bzero",
    "bcopy",
    "bzero",
    "bcmp",
    "strerror", /* libc++ std::error_code diagnostics (C++ carts) */

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

    /* libblytc.so — formatted input */
    "sscanf",
    "vsscanf",

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

    /* libblytc.so — non-local jumps (safe within a single frame callback;
     * using across frame boundaries would break save-state — ADR-0112) */
    "setjmp",
    "longjmp",

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

    /* libblytc.so — math (f64 less-common variants) */
    "erf",
    "erfc",
    "expm1",
    "log1p",
    "asinh",
    "acosh",
    "atanh",
    "lgamma",
    "tgamma",
    "j0",
    "j1",

    /* libblytc.so — locale (localeconv; C locale only) */
    "localeconv",

    /* compiler-rt ABI — floating-point and 64-bit integer helpers emitted by
     * the compiler for f32/f64/f128 arithmetic, conversions, and comparisons.
     * Provided by libblyt32.so (libblytc sources) and libblyt32lua.so
     * (SoftFloat).  Carts using double/float128 arithmetic import these. */
    /* f32 ↔ f64 ↔ f128 conversions */
    "__extendsfdf2",
    "__truncdfsf2",
    "__extendsftf2",
    "__extenddftf2",
    "__trunctfsf2",
    "__trunctfdf2",
    /* int → float */
    "__floatsidf",
    "__floatdidf",
    "__floatdisf",
    "__floatditf",
    "__floatsitf",
    "__floatunsidf",
    "__floatunsitf",
    /* float → int */
    "__fixdfsi",
    "__fixdfdi",
    "__fixsfdi",
    "__fixtfsi",
    "__fixtfdi",
    "__fixunstfsi",
    /* f64 arithmetic */
    "__adddf3",
    "__subdf3",
    "__muldf3",
    "__divdf3",
    /* f128 arithmetic */
    "__addtf3",
    "__subtf3",
    "__multf3",
    "__divtf3",
    /* f64 comparisons */
    "__eqdf2",
    "__nedf2",
    "__ltdf2",
    "__ledf2",
    "__gtdf2",
    "__gedf2",
    /* f128 comparisons */
    "__eqtf2",
    "__netf2",
    "__lttf2",
    "__letf2",
    "__gttf2",
    "__getf2",
    "__unordtf2",
    /* 64-bit integer arithmetic */
    "__udivdi3",
    "__umoddi3",
    /* 64-bit shifts and signed arithmetic (LLVM may emit as calls on RV32) */
    "__lshrdi3",
    "__ashrdi3",
    "__ashldi3",
    "__muldi3",
    "__divdi3",
    "__moddi3",
    /* classification helpers (isnan, isinf, signbit) */
    "__fpclassify",
    "__fpclassifyf",
    "__fpclassifyl",
    "__signbit",
    "__signbitf",
    "__signbitl",
    /* stack-protection canary (compiler stack-protector pass) */
    "__stack_chk_guard",
    "__stack_chk_fail",

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

/* Cart id rules — mirror the devtool's cart_id_valid (devtool/src/build.rs;
 * keep in sync): 1-63 bytes, [a-z0-9_-], first char alphanumeric.  The id
 * names the save-file subdirectory, so the 63-byte cap fits cart_name[64]
 * in cart_run.c. */
static int cart_id_valid(const char *id) {
    if (!id || !id[0])
        return 0;
    size_t len = strlen(id);
    if (len > 63)
        return 0;
    if (!((id[0] >= 'a' && id[0] <= 'z') || (id[0] >= '0' && id[0] <= '9')))
        return 0;
    for (size_t i = 0; i < len; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

/* title/version rules: non-empty, no control characters (the devtool
 * additionally enforces semver on version). */
static int cart_text_valid(const char *s) {
    if (!s || !s[0])
        return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 0x20 || *p == 0x7f)
            return 0;
    }
    return 1;
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
    /* .cart.info identity (read below; owned until success).  Declared before
     * the first `goto fail` so the fail path can free them unconditionally. */
    char *cart_id = NULL;
    char *cart_title = NULL;
    char *cart_version = NULL;

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
    const Elf32_Shdr *sect_cart_layouts = NULL;
    const Elf32_Shdr *sect_cart_lua = NULL;
    const Elf32_Shdr *sect_dynamic = NULL;
    const Elf32_Shdr *sect_dynsym = NULL;
    const Elf32_Shdr *sect_dynstr_sh = NULL;

    int has_dwarf = 0; /* set when a .debug_* section is present (ADR-0129) */
    int cart_is_debug = 0; /* .cart.info `debug` flag (read below) */
    uint32_t cart_save_version = 0; /* .cart.config `save_version` (ADR-0125) */
    uint32_t cart_default_palette = 0; /* .cart.config `default_palette` (#201) */

    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        const Elf32_Shdr *sh = &shdrs[i];

        if (sh->sh_name >= shstrtab_size) {
            err = BLYT_CART_ERR_BAD_SHDR;
            goto fail;
        }
        const char *name = shstrtab + sh->sh_name;

        if (section_name_denied(name)) {
            err = BLYT_CART_ERR_DENIED_SECT;
            goto fail;
        }

        if (sh->sh_type != SHT_NOBITS && sh->sh_size > 0) {
            if (!in_bounds(map, map_size, (const uint8_t *)map + sh->sh_offset, sh->sh_size)) {
                err = BLYT_CART_ERR_BAD_SHDR;
                goto fail;
            }
        }

        if (strncmp(name, ".debug_", 7) == 0 || strncmp(name, ".zdebug_", 8) == 0)
            has_dwarf = 1;
        if (strcmp(name, ".cart.info") == 0)
            sect_cart_info = sh;
        if (strcmp(name, ".cart.config") == 0)
            sect_cart_config = sh;
        if (strcmp(name, ".cart.layouts") == 0)
            sect_cart_layouts = sh;
        if (strcmp(name, ".cart.lua") == 0)
            sect_cart_lua = sh;
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

    int has_blyt32lua = 0;

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
            if (strcmp(dynstr + d->d_un.d_val, "libblyt32lua.so") == 0)
                has_blyt32lua = 1;
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
            const char *sym_name = symstr + sym->st_name;
            /* Lua carts may import any Lua C API symbol (lua_* / luaL_*)
             * when src/lib/ C code calls the Lua API directly.  These
             * symbols are pure VM operations with no host-side side effects. */
            int lua_api_sym = has_blyt32lua && (strncmp(sym_name, "lua_", 4) == 0 ||
                                                strncmp(sym_name, "luaL_", 5) == 0);
            if (!lua_api_sym && !symbol_name_allowed(sym_name)) {
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
        if (verify_result == flatcc_verify_ok && info) {
            cart_is_debug = blyt_CartInfo_debug(info) ? 1 : 0;
            const char *id = blyt_CartInfo_id(info);
            const char *title = blyt_CartInfo_title(info);
            const char *version = blyt_CartInfo_version(info);
            cart_id = id ? strdup(id) : NULL;
            cart_title = title ? strdup(title) : NULL;
            cart_version = version ? strdup(version) : NULL;
        }
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
        if (!cart_id_valid(cart_id)) {
            err = BLYT_CART_ERR_BAD_ID;
            goto fail;
        }
        if (!cart_text_valid(cart_title)) {
            err = BLYT_CART_ERR_BAD_TITLE;
            goto fail;
        }
        if (!cart_text_valid(cart_version)) {
            err = BLYT_CART_ERR_BAD_VERSION;
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
        if (verify_result != flatcc_verify_ok) {
            free(fb_aligned);
            err = BLYT_CART_ERR_BAD_CART_CONFIG;
            goto fail;
        }

        /* Read save_version (ADR-0125): stamped into the save header at write
         * time, reported back as blyt_load_info_t.saved_cart_version on load.
         * Read default_palette (#201): the built-in palette to auto-load
         * before init() when the cart declares one via `palettes: default:`. */
        blyt_CartConfig_table_t config = blyt_CartConfig_as_root(fb_aligned);
        if (config) {
            cart_save_version = blyt_CartConfig_save_version(config);
            cart_default_palette = blyt_CartConfig_default_palette(config);
        }
        free(fb_aligned);
    }

    /* -----------------------------------------------------------------------
     * .cart.layouts: optional, verify if present (ADR-0009)
     * --------------------------------------------------------------------- */

    if (sect_cart_layouts) {
        const void *sect_data = (const uint8_t *)map + sect_cart_layouts->sh_offset;
        size_t fb_size;
        const void *fb =
            check_preamble(sect_data, sect_cart_layouts->sh_size, CART_LAYOUTS_TAG, &fb_size);
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
        int verify_result = blyt_CartLayouts_verify_as_root(fb_aligned, fb_size);
        free(fb_aligned);

        if (verify_result != flatcc_verify_ok) {
            err = BLYT_CART_ERR_BAD_LAYOUTS;
            goto fail;
        }
    }

    /* -----------------------------------------------------------------------
     * Required entry point validation (ADR-0087)
     *
     * blyt_cart_init, blyt_cart_update, and blyt_cart_draw must be present
     * as defined (non-UNDEF, STB_GLOBAL) symbols in the cart's .dynsym.
     * --------------------------------------------------------------------- */

    /* Lua carts: blyt_cart_init/update/draw are provided by libblyt32lua.so
     * (a DT_NEEDED library).  Skip the defined-symbol check when both the
     * .cart.lua section and the libblyt32lua.so DT_NEEDED entry are present. */
    if (sect_cart_lua && has_blyt32lua)
        goto success;

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

success: {
    blyt_cart_t *cart = malloc(sizeof(*cart));
    if (!cart) {
        err = BLYT_CART_ERR_IO;
        goto fail;
    }
    cart->fd = fd;
    cart->map = map;
    cart->map_size = map_size;
    cart->is_debug = cart_is_debug;
    cart->has_dwarf = has_dwarf;
    cart->save_version = cart_save_version;
    cart->default_palette = cart_default_palette;
    cart->id = cart_id; /* ownership transferred; validated non-NULL above */
    cart->title = cart_title;
    cart->version = cart_version;
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
    free(cart_id);
    free(cart_title);
    free(cart_version);
    munmap(map, map_size);
    close(fd);
    return err;
}

const void *blyt_cart_find_section(const blyt_cart_t *cart, const char *name, size_t *size_out) {
    /* Bounds-checked ELF32 section walk shared with the native path (#128). */
    uint32_t off = 0, size = 0;
    if (!blyt_elf32_find_section((const uint8_t *)cart->map, cart->map_size, name, &off, &size))
        return NULL;
    if (size_out)
        *size_out = size;
    return (const uint8_t *)cart->map + off;
}

int blyt_cart_is_debug(const blyt_cart_t *cart) {
    return cart ? cart->is_debug : 0;
}

int blyt_cart_has_dwarf(const blyt_cart_t *cart) {
    return cart ? cart->has_dwarf : 0;
}

int blyt_cart_has_native_lifecycle(const blyt_cart_t *cart) {
    static const char *const names[] = {
        "blyt_cart_init",
        "blyt_cart_on_new_state",
        "blyt_cart_update",
        "blyt_cart_draw",
        "blyt_cart_on_quit",
        "blyt_cart_cleanup",
        NULL,
    };
    if (!cart)
        return 0;
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)cart->map;
    const uint8_t *base = (const uint8_t *)cart->map;
    const Elf32_Shdr *shdrs = (const Elf32_Shdr *)(base + eh->e_shoff);
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        if (shdrs[i].sh_type != SHT_DYNSYM)
            continue;
        uint32_t stridx = shdrs[i].sh_link;
        if (stridx >= eh->e_shnum)
            break;
        const char *strtab = (const char *)(base + shdrs[stridx].sh_offset);
        const Elf32_Sym *syms = (const Elf32_Sym *)(base + shdrs[i].sh_offset);
        uint32_t nsyms = shdrs[i].sh_size / sizeof(Elf32_Sym);
        for (uint32_t j = 0; j < nsyms; j++) {
            if (syms[j].st_shndx == SHN_UNDEF)
                continue;
            if (ELF32_ST_BIND(syms[j].st_info) == STB_LOCAL)
                continue;
            const char *sym_name = strtab + syms[j].st_name;
            for (int k = 0; names[k]; k++) {
                if (strcmp(sym_name, names[k]) == 0)
                    return 1;
            }
        }
        break;
    }
    return 0;
}

int blyt_cart_has_layouts(const blyt_cart_t *cart) {
    return blyt_cart_find_section(cart, ".cart.layouts", NULL) != NULL;
}

/* Lifecycle name→bit mapping shared by the two mask functions below.
 * init=0, update=1, draw=2, on_new_state=3, on_save_state=4,
 * on_load_state=5, on_quit=6, cleanup=7, on_assets_reloaded=8. */
static const struct {
    const char *native_name;
    const char *lua_name;
} lifecycle_map[] = {
    {"blyt_cart_init", "init"},
    {"blyt_cart_update", "update"},
    {"blyt_cart_draw", "draw"},
    {"blyt_cart_on_new_state", "on_new_state"},
    {"blyt_cart_on_save_state", "on_save_state"},
    {"blyt_cart_on_load_state", "on_load_state"},
    {"blyt_cart_on_quit", "on_quit"},
    {"blyt_cart_cleanup", "cleanup"},
    {"blyt_cart_on_assets_reloaded", "on_assets_reloaded"},
};
#define BLYT_LIFECYCLE_COUNT ((int)(sizeof(lifecycle_map) / sizeof(lifecycle_map[0])))

uint32_t blyt_cart_native_lifecycle_mask(const blyt_cart_t *cart) {
    if (!cart)
        return 0;
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)cart->map;
    const uint8_t *base = (const uint8_t *)cart->map;
    const Elf32_Shdr *shdrs = (const Elf32_Shdr *)(base + eh->e_shoff);
    uint32_t mask = 0;
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        if (shdrs[i].sh_type != SHT_DYNSYM)
            continue;
        uint32_t stridx = shdrs[i].sh_link;
        if (stridx >= eh->e_shnum)
            break;
        const char *strtab = (const char *)(base + shdrs[stridx].sh_offset);
        const Elf32_Sym *syms = (const Elf32_Sym *)(base + shdrs[i].sh_offset);
        uint32_t nsyms = shdrs[i].sh_size / sizeof(Elf32_Sym);
        for (uint32_t j = 0; j < nsyms; j++) {
            if (syms[j].st_shndx == SHN_UNDEF)
                continue;
            if (ELF32_ST_BIND(syms[j].st_info) == STB_LOCAL)
                continue;
            const char *sym_name = strtab + syms[j].st_name;
            for (int k = 0; k < BLYT_LIFECYCLE_COUNT; k++) {
                if (strcmp(sym_name, lifecycle_map[k].native_name) == 0)
                    mask |= (1u << k);
            }
        }
        break;
    }
    return mask;
}

#ifdef BLYT_HAVE_HOST_LUA
#include <lauxlib.h>
#include <lua.h>

uint32_t blyt_cart_lua_lifecycle_mask(const blyt_cart_t *cart) {
    if (!cart)
        return 0;
    size_t lua_size = 0;
    const void *lua_bytes = blyt_cart_find_section(cart, ".cart.lua", &lua_size);
    if (!lua_bytes || !lua_size)
        return 0;
    lua_State *L = luaL_newstate();
    if (!L)
        return 0;

    /* Handle BLMC multi-chunk format (multi-file carts, issue #54). */
    const unsigned char *data = (const unsigned char *)lua_bytes;
    size_t remaining = lua_size;
    if (remaining >= 8 && data[0] == 'B' && data[1] == 'L' && data[2] == 'M' && data[3] == 'C') {
        unsigned int nchunks = (unsigned int)data[4] | ((unsigned int)data[5] << 8) |
                               ((unsigned int)data[6] << 16) | ((unsigned int)data[7] << 24);
        data += 8;
        remaining -= 8;
        for (unsigned int ci = 0; ci < nchunks; ci++) {
            if (remaining < 4) {
                lua_close(L);
                return 0;
            }
            unsigned int csz = (unsigned int)data[0] | ((unsigned int)data[1] << 8) |
                               ((unsigned int)data[2] << 16) | ((unsigned int)data[3] << 24);
            data += 4;
            remaining -= 4;
            if (csz > remaining) {
                lua_close(L);
                return 0;
            }
            if (luaL_loadbuffer(L, (const char *)data, csz, "@chunk") != LUA_OK ||
                lua_pcall(L, 0, 0, 0) != LUA_OK) {
                lua_close(L);
                return 0;
            }
            data += csz;
            remaining -= csz;
        }
    } else {
        if (luaL_loadbuffer(L, (const char *)lua_bytes, lua_size, "@cart") != LUA_OK ||
            lua_pcall(L, 0, 0, 0) != LUA_OK) {
            lua_close(L);
            return 0;
        }
    }
    uint32_t mask = 0;
    for (int k = 0; k < BLYT_LIFECYCLE_COUNT; k++) {
        lua_getglobal(L, lifecycle_map[k].lua_name);
        if (lua_isfunction(L, -1))
            mask |= (1u << k);
        lua_pop(L, 1);
    }
    lua_close(L);
    return mask;
}

#else

uint32_t blyt_cart_lua_lifecycle_mask(const blyt_cart_t *cart) {
    (void)cart;
    return 0;
}

#endif /* BLYT_HAVE_HOST_LUA */

void blyt_cart_close(blyt_cart_t *cart) {
    if (!cart)
        return;
    munmap(cart->map, cart->map_size);
    close(cart->fd);
    free(cart->path);
    free(cart->id);
    free(cart->title);
    free(cart->version);
    free(cart);
}

const char *blyt_cart_id(const blyt_cart_t *cart) {
    return cart ? cart->id : NULL;
}

const char *blyt_cart_title(const blyt_cart_t *cart) {
    return cart ? cart->title : NULL;
}

const char *blyt_cart_version(const blyt_cart_t *cart) {
    return cart ? cart->version : NULL;
}

uint32_t blyt_cart_save_version(const blyt_cart_t *cart) {
    return cart ? cart->save_version : 0u;
}

uint32_t blyt_cart_default_palette(const blyt_cart_t *cart) {
    return cart ? cart->default_palette : 0u;
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
        return "e_flags: expected RV32IMAFDC ILP32D";
    case BLYT_CART_ERR_BAD_SHDR:
        return "section header table malformed";
    case BLYT_CART_ERR_DENIED_SECT:
        return "denied ELF section";
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
    case BLYT_CART_ERR_BAD_LAYOUTS:
        return ".cart.layouts FlatBuffers parse error";
    case BLYT_CART_ERR_BAD_ID:
        return ".cart.info: cart id missing or invalid (1-63 chars [a-z0-9_-], "
               "alphanumeric first; rebuild with current blyt)";
    case BLYT_CART_ERR_BAD_TITLE:
        return ".cart.info: cart title missing or invalid (rebuild with current blyt)";
    case BLYT_CART_ERR_BAD_VERSION:
        return ".cart.info: cart version missing or invalid (rebuild with current blyt)";
    }
    return "unknown error";
}
