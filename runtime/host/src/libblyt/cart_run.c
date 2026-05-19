#include "cart_run.h"

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "blyt_runtime.h"
#include "cart_load.h"
#include "ecall.h"
#include "elf32.h"

/*
 * rv32emu headers — common.h must come first; ${RV32EMU_DIR} is on the
 * include path so these can be referenced by name rather than relative path.
 */
#include "common.h"
#include "io.h"
#include "riscv.h"
#include "riscv_private.h"

/* -------------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------------- */

/* 256 MiB guest memory: libraries live at 128 MiB, validated by Spike C. */
#define BLYT_EMU_MEM_SIZE (256u * 1024u * 1024u)
#define BLYT_STACK_SIZE (1u * 1024u * 1024u)
#define BLYT_CYCLE_PER_STEP 500

/* Base guest address for the first runtime library.  Subsequent libraries
 * are placed at GUEST_LIB_BASE + n * GUEST_LIB_STRIDE (2 MiB each). */
#define GUEST_LIB_BASE 0x08000000u
#define GUEST_LIB_STRIDE 0x00200000u /* 2 MiB between libraries */

/* Maximum number of runtime libraries loaded per cart execution. */
#define MAX_RUNTIME_LIBS 8

/* Cart heap arena (ADR-0120): 16 MiB region in guest address space.
 * Placed at 64 MiB — well above the cart/trampoline region (~64 KiB) and
 * below the runtime library region (128 MiB). */
#define BLYT_ARENA_BASE 0x04000000u /* 64 MiB */
#define BLYT_ARENA_SIZE (16u * 1024u * 1024u) /* 16 MiB */

/* Maximum exported symbols tracked across all loaded runtime libraries. */
#define MAX_SYMS 512

/* -------------------------------------------------------------------------
 * In-memory library registry
 *
 * Frontends (e.g. libretro) that embed guest libraries as compiled-in data
 * call blyt_register_lib() before creating a session.  dynlink checks here
 * first before falling back to the BLYT_LIB_DIR filesystem path.
 * ------------------------------------------------------------------------- */

#define MAX_REGISTERED_LIBS 8

typedef struct {
    char name[64];
    const void *data;
    size_t size;
} blyt_registered_lib_t;

static blyt_registered_lib_t g_registered_libs[MAX_REGISTERED_LIBS];
static int g_registered_lib_count = 0;

void blyt_register_lib(const char *name, const void *data, size_t size) {
    if (g_registered_lib_count >= MAX_REGISTERED_LIBS)
        return;
    /* Ignore duplicate registrations */
    for (int i = 0; i < g_registered_lib_count; i++) {
        if (strcmp(g_registered_libs[i].name, name) == 0)
            return;
    }
    blyt_registered_lib_t *r = &g_registered_libs[g_registered_lib_count++];
    strncpy(r->name, name, sizeof(r->name) - 1);
    r->name[sizeof(r->name) - 1] = '\0';
    r->data = data;
    r->size = size;
}

void blyt_clear_libs(void) {
    g_registered_lib_count = 0;
}

/* -------------------------------------------------------------------------
 * ECALL handler context
 * ------------------------------------------------------------------------- */

typedef struct {
    blyt_log_fn log_fn;
    bool ecall_trapped; /* cart issued a non-permitted ecall */
    bool ecall_aborted; /* cart called abort() — ECALL_EXIT with a0 != 0 */
    bool frame_done; /* set by BLYT_ECALL_FRAME_DONE; cleared by run_frame */
} blyt_run_ctx_t;

static blyt_run_ctx_t *g_run_ctx = NULL;

/* -------------------------------------------------------------------------
 * Session
 * ------------------------------------------------------------------------- */

struct blyt_session {
    riscv_t *rv;
    /* vm_attr_t must outlive rv: rv_create stores &attr in rv->data (rv->priv),
     * so it must not be stack-allocated in blyt_session_create. */
    vm_attr_t attr;
    blyt_run_ctx_t ctx;
};

/* -------------------------------------------------------------------------
 * EXIT trampoline
 * ------------------------------------------------------------------------- */

static void write_u32_le(uint8_t *dst, uint32_t val) {
    dst[0] = (uint8_t)val;
    dst[1] = (uint8_t)(val >> 8);
    dst[2] = (uint8_t)(val >> 16);
    dst[3] = (uint8_t)(val >> 24);
}

static bool inject_exit_trampoline(memory_t *mem) {
    uint8_t stub[16];
    write_u32_le(stub + 0, RV32_LI_A0_0); /* a0 = 0 (clean exit code) */
    write_u32_le(stub + 4, RV32_LI_A7_0); /* a7 = BLYT_ECALL_EXIT */
    write_u32_le(stub + 8, RV32_ECALL);
    write_u32_le(stub + 12, RV32_UNIMP);
    return memory_write(mem, BLYT_TRAMPOLINE_EXIT_ADDR, stub, sizeof(stub));
}

/* -------------------------------------------------------------------------
 * ECALL handler (ADR-0085: a0=ptr, a1=len, a7=ecall_number)
 * ------------------------------------------------------------------------- */

static void blyt_ecall_handler(riscv_t *rv) {
    uint32_t num = rv_get_reg(rv, rv_reg_a7);

    switch (num) {
    case BLYT_ECALL_EXIT: {
        uint32_t code = rv_get_reg(rv, rv_reg_a0);
        rv_halt(rv);
        if (code != 0 && g_run_ctx)
            g_run_ctx->ecall_aborted = true;
        return;
    }

    case BLYT_ECALL_CONSOLE_DEBUG: {
        uint32_t vaddr = rv_get_reg(rv, rv_reg_a0);
        uint32_t len = rv_get_reg(rv, rv_reg_a1);
        vm_attr_t *attr = PRIV(rv);
        memory_t *mem = attr->mem;

        char buf[4096];
        if (len >= sizeof(buf))
            len = sizeof(buf) - 1;
        uint32_t i;
        for (i = 0; i < len; i++) {
            if (!GUEST_RAM_CONTAINS(mem, vaddr + i, 1))
                break;
            uint8_t c;
            memory_read(mem, &c, vaddr + i, 1);
            buf[i] = (char)c;
        }
        buf[i] = '\0';

        if (g_run_ctx && g_run_ctx->log_fn)
            g_run_ctx->log_fn(buf);

        rv->PC += 4;
        return;
    }

    case BLYT_ECALL_FRAME_DONE:
        rv->PC += 4;
        if (g_run_ctx)
            g_run_ctx->frame_done = true;
        return;

    default:
        rv_halt(rv);
        if (g_run_ctx)
            g_run_ctx->ecall_trapped = true;
        return;
    }
}

/* -------------------------------------------------------------------------
 * Combined symbol table across all loaded runtime libraries
 * ------------------------------------------------------------------------- */

typedef struct {
    char name[128];
    uint32_t guest_addr;
} blyt_sym_t;

typedef struct {
    blyt_sym_t syms[MAX_SYMS];
    int count;
} blyt_symtab_t;

static uint32_t symtab_lookup(const blyt_symtab_t *st, const char *name) {
    for (int i = 0; i < st->count; i++) {
        if (strcmp(st->syms[i].name, name) == 0)
            return st->syms[i].guest_addr;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Per-library state during dynamic loading
 * ------------------------------------------------------------------------- */

typedef struct {
    const uint8_t *map; /* host-side mapping */
    size_t size;
    uint32_t bias; /* guest load bias */
    bool mmapped; /* true if map was mmap'd and must be munmap'd on cleanup */
} blyt_lib_t;

/* -------------------------------------------------------------------------
 * ELF virtual-address → host pointer (for ET_DYN with base=0 at link time)
 * ------------------------------------------------------------------------- */

static const void *vaddr_to_ptr(const uint8_t *map, size_t map_size, const Elf32_Ehdr *eh,
                                uint32_t vaddr) {
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        size_t off = (size_t)eh->e_phoff + (size_t)i * eh->e_phentsize;
        if (off + sizeof(Elf32_Phdr) > map_size)
            return NULL;
        const Elf32_Phdr *ph = (const Elf32_Phdr *)(map + off);
        if (ph->p_type != PT_LOAD)
            continue;
        if (vaddr >= ph->p_vaddr && vaddr < ph->p_vaddr + ph->p_filesz) {
            size_t foff = ph->p_offset + (vaddr - ph->p_vaddr);
            if (foff < map_size)
                return map + foff;
        }
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Map PT_LOAD segments into rv32emu guest memory
 * ------------------------------------------------------------------------- */

static bool map_lib_segments(const blyt_lib_t *lib, memory_t *mem) {
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)lib->map;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        size_t off = (size_t)eh->e_phoff + (size_t)i * eh->e_phentsize;
        if (off + sizeof(Elf32_Phdr) > lib->size)
            return false;
        const Elf32_Phdr *ph = (const Elf32_Phdr *)(lib->map + off);
        if (ph->p_type != PT_LOAD)
            continue;
        if ((size_t)ph->p_offset + ph->p_filesz > lib->size)
            return false;
        uint32_t g = lib->bias + ph->p_vaddr;
        if (!memory_write(mem, g, lib->map + ph->p_offset, ph->p_filesz))
            return false;
        if (ph->p_memsz > ph->p_filesz) {
            if (!memory_fill(mem, g + ph->p_filesz, ph->p_memsz - ph->p_filesz, 0))
                return false;
        }
    }
    return true;
}

/* -------------------------------------------------------------------------
 * Apply load-time relocations from a library's SHT_RELA sections.
 *
 * Handles:
 *   R_RISCV_RELATIVE — B + A  (load-base relative; most data pointers)
 *   R_RISCV_32       — S + A  (symbol-relative; e.g. internal GOT pointers
 *                              for module-local data variables like the arena
 *                              globals in libblytc.so)
 *
 * R_RISCV_JUMP_SLOT and R_RISCV_GLOB_DAT are handled separately by
 * resolve_elf_plt (they require the combined cross-library symbol table).
 * ------------------------------------------------------------------------- */

static void apply_lib_rela(const blyt_lib_t *lib, memory_t *mem) {
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)lib->map;
    if (eh->e_shentsize < sizeof(Elf32_Shdr) || eh->e_shnum == 0)
        return;

    /* Locate .dynsym (needed for R_RISCV_32 symbol lookup). */
    const Elf32_Shdr *shdrs = (const Elf32_Shdr *)(lib->map + eh->e_shoff);
    const Elf32_Shdr *dynsym_sh = NULL;
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        if (shdrs[i].sh_type == SHT_DYNSYM && !dynsym_sh)
            dynsym_sh = &shdrs[i];
    }

    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        size_t off = (size_t)eh->e_shoff + (size_t)i * eh->e_shentsize;
        if (off + sizeof(Elf32_Shdr) > lib->size)
            break;
        const Elf32_Shdr *sh = (const Elf32_Shdr *)(lib->map + off);
        if (sh->sh_type != SHT_RELA || sh->sh_entsize < sizeof(Elf32_Rela))
            continue;

        size_t count = sh->sh_size / sh->sh_entsize;
        for (size_t j = 0; j < count; j++) {
            size_t roff = sh->sh_offset + j * sh->sh_entsize;
            if (roff + sizeof(Elf32_Rela) > lib->size)
                break;
            const Elf32_Rela *r = (const Elf32_Rela *)(lib->map + roff);
            uint32_t type = ELF32_R_TYPE(r->r_info);
            uint8_t v[4];

            if (type == R_RISCV_RELATIVE) {
                write_u32_le(v, lib->bias + (uint32_t)r->r_addend);
                memory_write(mem, lib->bias + r->r_offset, v, 4);
            } else if (type == R_RISCV_32 && dynsym_sh &&
                       dynsym_sh->sh_entsize >= sizeof(Elf32_Sym)) {
                uint32_t sym_idx = ELF32_R_SYM(r->r_info);
                size_t sym_off =
                    (size_t)dynsym_sh->sh_offset + (size_t)sym_idx * dynsym_sh->sh_entsize;
                if (sym_off + sizeof(Elf32_Sym) > lib->size)
                    continue;
                const Elf32_Sym *sym = (const Elf32_Sym *)(lib->map + sym_off);
                if (sym->st_shndx != SHN_UNDEF) {
                    uint32_t val = lib->bias + sym->st_value + (uint32_t)r->r_addend;
                    write_u32_le(v, val);
                    memory_write(mem, lib->bias + r->r_offset, v, 4);
                }
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * Add a library's exported symbols to the combined symbol table
 * ------------------------------------------------------------------------- */

static void build_symtab(const blyt_lib_t *lib, blyt_symtab_t *st) {
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)lib->map;
    if (eh->e_shentsize < sizeof(Elf32_Shdr) || eh->e_shnum == 0)
        return;

    const Elf32_Shdr *shdrs = (const Elf32_Shdr *)(lib->map + eh->e_shoff);
    const Elf32_Shdr *dynsym = NULL;
    const Elf32_Shdr *dynstr = NULL;

    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        if (shdrs[i].sh_type == SHT_DYNSYM && !dynsym)
            dynsym = &shdrs[i];
        if (dynsym && i == dynsym->sh_link && shdrs[i].sh_type == SHT_STRTAB)
            dynstr = &shdrs[i];
    }
    if (dynsym && !dynstr && dynsym->sh_link < eh->e_shnum)
        dynstr = &shdrs[dynsym->sh_link];
    if (!dynsym || !dynstr || dynsym->sh_entsize < sizeof(Elf32_Sym))
        return;

    const char *strtab = (const char *)(lib->map + dynstr->sh_offset);
    size_t strsz = dynstr->sh_size;
    size_t nsyms = dynsym->sh_size / dynsym->sh_entsize;

    for (size_t j = 0; j < nsyms && st->count < MAX_SYMS; j++) {
        const Elf32_Sym *sym =
            (const Elf32_Sym *)(lib->map + dynsym->sh_offset + j * dynsym->sh_entsize);
        if (sym->st_shndx == SHN_UNDEF)
            continue;
        uint8_t bind = ELF32_ST_BIND(sym->st_info);
        if (bind != STB_GLOBAL && bind != STB_WEAK)
            continue;
        if (sym->st_name >= strsz || sym->st_name == 0)
            continue;
        const char *name = strtab + sym->st_name;

        if (symtab_lookup(st, name) != 0)
            continue;

        blyt_sym_t *s = &st->syms[st->count++];
        strncpy(s->name, name, sizeof(s->name) - 1);
        s->name[sizeof(s->name) - 1] = '\0';
        s->guest_addr = lib->bias + sym->st_value;
    }
}

/* -------------------------------------------------------------------------
 * Parse DT_NEEDED entries from a library's PT_DYNAMIC segment
 * ------------------------------------------------------------------------- */

static int get_dt_needed(const blyt_lib_t *lib, const char **names, int max_out) {
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)lib->map;
    int count = 0;

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        size_t off = (size_t)eh->e_phoff + (size_t)i * eh->e_phentsize;
        if (off + sizeof(Elf32_Phdr) > lib->size)
            break;
        const Elf32_Phdr *ph = (const Elf32_Phdr *)(lib->map + off);
        if (ph->p_type != PT_DYNAMIC)
            continue;

        const Elf32_Dyn *dyn = (const Elf32_Dyn *)(lib->map + ph->p_offset);
        size_t ndyn = ph->p_filesz / sizeof(Elf32_Dyn);
        const char *strtab = NULL;

        for (size_t k = 0; k < ndyn; k++) {
            if (dyn[k].d_tag == DT_NULL)
                break;
            if (dyn[k].d_tag == DT_STRTAB) {
                strtab = (const char *)vaddr_to_ptr(lib->map, lib->size, eh, dyn[k].d_un.d_val);
                break;
            }
        }
        if (!strtab)
            break;

        for (size_t k = 0; k < ndyn && count < max_out; k++) {
            if (dyn[k].d_tag == DT_NULL)
                break;
            if (dyn[k].d_tag == DT_NEEDED)
                names[count++] = strtab + dyn[k].d_un.d_val;
        }
        break;
    }
    return count;
}

/* -------------------------------------------------------------------------
 * Resolve PLT/GOT entries in an ELF binary against the combined symbol table
 * ------------------------------------------------------------------------- */

static void resolve_elf_plt(const uint8_t *map, size_t map_size, memory_t *mem, uint32_t elf_bias,
                            const blyt_symtab_t *syms) {
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)map;
    if (eh->e_shentsize < sizeof(Elf32_Shdr) || eh->e_shnum == 0)
        return;

    const Elf32_Shdr *shdrs = (const Elf32_Shdr *)(map + eh->e_shoff);
    const Elf32_Shdr *shstrtab = &shdrs[eh->e_shstrndx];
    const char *shstr = (const char *)(map + shstrtab->sh_offset);

    const Elf32_Shdr *dynsym = NULL;
    const Elf32_Shdr *dynstr = NULL;
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        if (shdrs[i].sh_type == SHT_DYNSYM && !dynsym)
            dynsym = &shdrs[i];
        if (shdrs[i].sh_type == SHT_STRTAB && strcmp(shstr + shdrs[i].sh_name, ".dynstr") == 0)
            dynstr = &shdrs[i];
    }
    if (!dynsym || !dynstr || dynsym->sh_entsize < sizeof(Elf32_Sym))
        return;

    const char *sym_strtab = (const char *)(map + dynstr->sh_offset);
    size_t sym_strsz = dynstr->sh_size;

    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        const Elf32_Shdr *sh = &shdrs[i];
        if (sh->sh_type != SHT_RELA || sh->sh_entsize < sizeof(Elf32_Rela))
            continue;

        size_t rcount = sh->sh_size / sh->sh_entsize;
        for (size_t j = 0; j < rcount; j++) {
            const Elf32_Rela *r = (const Elf32_Rela *)(map + sh->sh_offset + j * sh->sh_entsize);
            uint32_t type = ELF32_R_TYPE(r->r_info);
            if (type != R_RISCV_JUMP_SLOT && type != R_RISCV_GLOB_DAT)
                continue;

            uint32_t sym_idx = ELF32_R_SYM(r->r_info);
            if (sym_idx * dynsym->sh_entsize + sizeof(Elf32_Sym) > dynsym->sh_size)
                continue;

            const Elf32_Sym *sym =
                (const Elf32_Sym *)(map + dynsym->sh_offset + sym_idx * dynsym->sh_entsize);
            if (sym->st_name >= sym_strsz)
                continue;

            const char *sym_name = sym_strtab + sym->st_name;
            uint32_t resolved = symtab_lookup(syms, sym_name);
            if (resolved == 0)
                continue;

            uint8_t v[4];
            write_u32_le(v, resolved);
            memory_write(mem, elf_bias + r->r_offset, v, 4);
        }
    }
}

/* -------------------------------------------------------------------------
 * Open a library — check the in-memory registry first, then fall back to
 * loading from BLYT_LIB_DIR.  Sets *mmapped_out to true only when the data
 * was mmap'd from a file and must be munmap'd by the caller.
 * ------------------------------------------------------------------------- */

static bool open_lib(const char *lib_dir, const char *lib_name, const uint8_t **map_out,
                     size_t *size_out, bool *mmapped_out) {
    /* Registry lookup (e.g. libs embedded in the libretro core) */
    for (int i = 0; i < g_registered_lib_count; i++) {
        if (strcmp(g_registered_libs[i].name, lib_name) == 0) {
            *map_out = (const uint8_t *)g_registered_libs[i].data;
            *size_out = g_registered_libs[i].size;
            *mmapped_out = false;
            return true;
        }
    }

    /* Filesystem fallback */
    if (!lib_dir || lib_dir[0] == '\0') {
        fprintf(stderr, "blyt: %s not in registry and BLYT_LIB_DIR not set\n", lib_name);
        return false;
    }

    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/%s", lib_dir, lib_name);
    if (n < 0 || (size_t)n >= sizeof(path))
        return false;

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "blyt: cannot open %s\n", path);
        return false;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return false;
    }

    void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED)
        return false;

    *map_out = (const uint8_t *)m;
    *size_out = (size_t)st.st_size;
    *mmapped_out = true;
    return true;
}

/* -------------------------------------------------------------------------
 * Top-level dynamic loader
 * ------------------------------------------------------------------------- */

static blyt_cart_run_err_t dynlink(riscv_t *rv, const blyt_cart_t *cart) {
    const char *lib_dir = getenv("BLYT_LIB_DIR");

    vm_attr_t *attr = PRIV(rv);
    memory_t *mem = attr->mem;

    blyt_lib_t libs[MAX_RUNTIME_LIBS];
    int nlibs = 0;

    blyt_symtab_t all_syms;
    all_syms.count = 0;

    /* Seed cart symbols first so cart's strong definitions win over library stubs. */
    {
        blyt_lib_t cart_syms = {
            .map = (const uint8_t *)cart->map,
            .size = cart->map_size,
            .bias = 0,
            .mmapped = false,
        };
        build_symtab(&cart_syms, &all_syms);
    }

    char name_buf[MAX_RUNTIME_LIBS][64];
    int qhead = 0;
    int qtail = 0;

    /* Seed BFS queue from the cart's DT_NEEDED */
    {
        const Elf32_Ehdr *ceh = (const Elf32_Ehdr *)cart->map;
        if (ceh->e_shentsize >= sizeof(Elf32_Shdr) && ceh->e_shnum > 0) {
            const Elf32_Shdr *shdrs = (const Elf32_Shdr *)(cart->map + ceh->e_shoff);
            const Elf32_Shdr *shstrtab = &shdrs[ceh->e_shstrndx];
            const char *shstr = (const char *)(cart->map + shstrtab->sh_offset);
            const Elf32_Shdr *dynamic = NULL;
            const Elf32_Shdr *dynstr = NULL;
            for (uint16_t i = 0; i < ceh->e_shnum; i++) {
                if (shdrs[i].sh_type == SHT_DYNAMIC)
                    dynamic = &shdrs[i];
                if (shdrs[i].sh_type == SHT_STRTAB &&
                    strcmp(shstr + shdrs[i].sh_name, ".dynstr") == 0)
                    dynstr = &shdrs[i];
            }
            if (dynamic && dynstr) {
                const char *strtab = (const char *)(cart->map + dynstr->sh_offset);
                size_t ndyn = dynamic->sh_size / sizeof(Elf32_Dyn);
                const Elf32_Dyn *dyn = (const Elf32_Dyn *)(cart->map + dynamic->sh_offset);
                for (size_t k = 0; k < ndyn && qtail < MAX_RUNTIME_LIBS; k++) {
                    if (dyn[k].d_tag == DT_NULL)
                        break;
                    if (dyn[k].d_tag == DT_NEEDED) {
                        strncpy(name_buf[qtail], strtab + dyn[k].d_un.d_val,
                                sizeof(name_buf[qtail]) - 1);
                        name_buf[qtail][sizeof(name_buf[qtail]) - 1] = '\0';
                        qtail++;
                    }
                }
            }
        }
    }

    bool ok = true;

    while (qhead < qtail && nlibs < MAX_RUNTIME_LIBS) {
        const char *lib_name = name_buf[qhead++];

        const uint8_t *lmap;
        size_t lsz;
        bool mmapped;
        if (!open_lib(lib_dir, lib_name, &lmap, &lsz, &mmapped)) {
            ok = false;
            break;
        }

        const Elf32_Ehdr *leh = (const Elf32_Ehdr *)lmap;
        if (lsz < sizeof(Elf32_Ehdr) || leh->e_ident[EI_MAG0] != ELFMAG0 ||
            leh->e_machine != EM_RISCV) {
            if (mmapped)
                munmap((void *)lmap, lsz);
            ok = false;
            break;
        }

        uint32_t bias =
            (leh->e_type == ET_DYN) ? GUEST_LIB_BASE + (uint32_t)nlibs * GUEST_LIB_STRIDE : 0;

        blyt_lib_t lib = {.map = lmap, .size = lsz, .bias = bias, .mmapped = mmapped};

        if (!map_lib_segments(&lib, mem)) {
            if (mmapped)
                munmap((void *)lmap, lsz);
            ok = false;
            break;
        }

        apply_lib_rela(&lib, mem);
        build_symtab(&lib, &all_syms);
        libs[nlibs++] = lib;

        /* Enqueue transitive DT_NEEDED dependencies */
        const char *needed[MAX_RUNTIME_LIBS];
        int nneeded = get_dt_needed(&lib, needed, MAX_RUNTIME_LIBS);
        for (int i = 0; i < nneeded && qtail < MAX_RUNTIME_LIBS; i++) {
            bool already = false;
            for (int q = 0; q < qtail; q++) {
                if (strcmp(name_buf[q], needed[i]) == 0) {
                    already = true;
                    break;
                }
            }
            if (!already) {
                strncpy(name_buf[qtail], needed[i], sizeof(name_buf[qtail]) - 1);
                name_buf[qtail][sizeof(name_buf[qtail]) - 1] = '\0';
                qtail++;
            }
        }
    }

    if (ok) {
        for (int i = 0; i < nlibs; i++) {
            resolve_elf_plt(libs[i].map, libs[i].size, mem, libs[i].bias, &all_syms);
        }
        resolve_elf_plt(cart->map, cart->map_size, mem, 0, &all_syms);

        /* Initialise the libblytc.so arena (ADR-0120). */
        uint32_t sym_base = symtab_lookup(&all_syms, "blytc_arena_base");
        uint32_t sym_size = symtab_lookup(&all_syms, "blytc_arena_size");
        if (sym_base != 0 && sym_size != 0) {
            uint8_t v[4];
            write_u32_le(v, BLYT_ARENA_BASE);
            memory_write(mem, sym_base, v, 4);
            write_u32_le(v, BLYT_ARENA_SIZE);
            memory_write(mem, sym_size, v, 4);
        }
    }

    for (int i = 0; i < nlibs; i++) {
        if (libs[i].mmapped)
            munmap((void *)libs[i].map, libs[i].size);
    }

    return ok ? BLYT_RUN_OK : BLYT_RUN_ERR_EMU;
}

/* -------------------------------------------------------------------------
 * Session API — public entry points
 * ------------------------------------------------------------------------- */

blyt_session_t *blyt_session_create(blyt_cart_t *cart, blyt_log_fn log_fn) {
    blyt_session_t *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;

    s->ctx.log_fn = log_fn;

    s->attr.mem_size = BLYT_EMU_MEM_SIZE;
    s->attr.stack_size = BLYT_STACK_SIZE;
    s->attr.args_offset_size = 0;
    s->attr.argc = 0;
    s->attr.argv = NULL;
    s->attr.log_level = LOG_WARN;
    s->attr.cycle_per_step = BLYT_CYCLE_PER_STEP;
    s->attr.allow_misalign = false;
    s->attr.fd_stdin = STDIN_FILENO;
    s->attr.fd_stdout = STDOUT_FILENO;
    s->attr.fd_stderr = STDERR_FILENO;
    s->attr.data.user.elf_program = cart->path;

    s->rv = rv_create(&s->attr);
    if (!s->rv) {
        free(s);
        return NULL;
    }

    vm_attr_t *rattr = PRIV(s->rv);
    inject_exit_trampoline(rattr->mem);

    blyt_cart_run_err_t load_err = dynlink(s->rv, cart);
    if (load_err != BLYT_RUN_OK) {
        rv_delete(s->rv);
        free(s);
        return NULL;
    }

    s->rv->io.on_ecall = blyt_ecall_handler;
    rv_set_reg(s->rv, rv_reg_ra, BLYT_TRAMPOLINE_EXIT_ADDR);

    return s;
}

blyt_cart_run_err_t blyt_session_run_frame(blyt_session_t *session) {
    g_run_ctx = &session->ctx;
    session->ctx.frame_done = false;

    while (!rv_has_halted(session->rv) && !session->ctx.ecall_trapped &&
           !session->ctx.ecall_aborted) {
        rv_step(session->rv);
        if (session->ctx.frame_done) {
            g_run_ctx = NULL;
            return BLYT_RUN_FRAME_DONE;
        }
    }

    bool trapped = session->ctx.ecall_trapped;
    bool aborted = session->ctx.ecall_aborted;
    g_run_ctx = NULL;

    if (trapped)
        return BLYT_RUN_ERR_ECALL_TRAP;
    if (aborted)
        return BLYT_RUN_ERR_ABORT;
    return BLYT_RUN_OK;
}

void blyt_session_destroy(blyt_session_t *session) {
    if (!session)
        return;
    rv_delete(session->rv);
    free(session);
}

/* -------------------------------------------------------------------------
 * blyt_cart_run — blocking wrapper around the session API
 * ------------------------------------------------------------------------- */

blyt_cart_run_err_t blyt_cart_run(blyt_cart_t *cart, blyt_log_fn log_fn, blyt_frame_fn frame_fn,
                                  void *userdata) {
    blyt_session_t *s = blyt_session_create(cart, log_fn);
    if (!s)
        return BLYT_RUN_ERR_EMU;

    blyt_cart_run_err_t err;
    while ((err = blyt_session_run_frame(s)) == BLYT_RUN_FRAME_DONE) {
        if (frame_fn)
            frame_fn(userdata);
    }

    blyt_session_destroy(s);
    return err;
}
