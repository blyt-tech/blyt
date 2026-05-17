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
 * rv32emu headers — common.h must come first.
 */
#include "../../third_party/rv32emu/src/common.h"
#include "../../third_party/rv32emu/src/io.h"
#include "../../third_party/rv32emu/src/riscv.h"
#include "../../third_party/rv32emu/src/riscv_private.h"

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

/* Maximum exported symbols tracked across all loaded runtime libraries. */
#define MAX_SYMS 512

/* -------------------------------------------------------------------------
 * ECALL handler global context
 * ------------------------------------------------------------------------- */

typedef struct {
    blyt_log_fn log_fn;
    bool ecall_trapped;
} blyt_run_ctx_t;

static blyt_run_ctx_t *g_run_ctx = NULL;

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
    uint8_t stub[12];
    write_u32_le(stub + 0, RV32_LI_A7_0);
    write_u32_le(stub + 4, RV32_ECALL);
    write_u32_le(stub + 8, RV32_UNIMP);
    return memory_write(mem, BLYT_TRAMPOLINE_EXIT_ADDR, stub, sizeof(stub));
}

/* -------------------------------------------------------------------------
 * ECALL handler (ADR-0085: a0=ptr, a1=len, a7=ecall_number)
 * ------------------------------------------------------------------------- */

static void blyt_ecall_handler(riscv_t *rv) {
    uint32_t num = rv_get_reg(rv, rv_reg_a7);

    switch (num) {
    case BLYT_ECALL_EXIT:
        rv_halt(rv);
        return;

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
    const uint8_t *map; /* host-side mmap */
    size_t size;
    uint32_t bias; /* guest load bias (GUEST_LIB_BASE + n*STRIDE for ET_DYN) */
} blyt_lib_t;

/* -------------------------------------------------------------------------
 * ELF virtual-address → host pointer (for ET_DYN with base=0 at link time)
 *
 * Converts an ELF virtual address (as emitted by the linker for a PIC .so
 * with assumed base 0) to a pointer into the host-mapped library bytes.
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
 * Apply R_RISCV_RELATIVE relocations from a library's SHT_RELA sections
 * ------------------------------------------------------------------------- */

static void apply_lib_rela(const blyt_lib_t *lib, memory_t *mem) {
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)lib->map;
    if (eh->e_shentsize < sizeof(Elf32_Shdr) || eh->e_shnum == 0)
        return;

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
            if (ELF32_R_TYPE(r->r_info) == R_RISCV_RELATIVE) {
                uint32_t val = lib->bias + (uint32_t)r->r_addend;
                uint8_t v[4];
                write_u32_le(v, val);
                memory_write(mem, lib->bias + r->r_offset, v, 4);
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
    /* Fallback: use sh_link from dynsym */
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

        /* Don't override a symbol already in the table (first wins). */
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
 *
 * Writes at most max_out library names (pointers into lib->map) to names[].
 * Returns the number of names found.
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

        /* Find the dynamic string table via DT_STRTAB */
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
        break; /* only one PT_DYNAMIC */
    }
    return count;
}

/* -------------------------------------------------------------------------
 * Resolve PLT/GOT entries in an ELF binary against the combined symbol table
 *
 * Handles R_RISCV_JUMP_SLOT and R_RISCV_GLOB_DAT.  For ET_DYN libraries,
 * elf_bias is the load bias; for ET_EXEC carts elf_bias is 0 (r_offset is
 * already an absolute virtual address).
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
            /* elf_bias=0 for ET_EXEC (r_offset is absolute);
             * elf_bias=load_base for ET_DYN (r_offset is relative). */
            memory_write(mem, elf_bias + r->r_offset, v, 4);
        }
    }
}

/* -------------------------------------------------------------------------
 * Open a library file and mmap it
 * ------------------------------------------------------------------------- */

static bool open_lib(const char *lib_dir, const char *lib_name, const uint8_t **map_out,
                     size_t *size_out) {
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
    return true;
}

/* -------------------------------------------------------------------------
 * Top-level dynamic loader
 *
 * Loads the runtime libraries required by the cart using a BFS over DT_NEEDED
 * entries (cart → libblyt32.so → libblytcommon.so → …).  For each library:
 *   1. Map its PT_LOAD segments into rv32emu guest memory.
 *   2. Apply its R_RISCV_RELATIVE relocations.
 *   3. Add its exported symbols to the combined symbol table.
 * Then resolve PLT/GOT entries for each loaded library (against the combined
 * table), and finally resolve the cart's PLT/GOT.
 * ------------------------------------------------------------------------- */

static blyt_cart_run_err_t dynlink(riscv_t *rv, const blyt_cart_t *cart) {
    const char *lib_dir = getenv("BLYT_LIB_DIR");
    if (!lib_dir || lib_dir[0] == '\0') {
        fprintf(stderr, "blyt: BLYT_LIB_DIR not set — cannot locate runtime libraries\n");
        return BLYT_RUN_ERR_EMU;
    }

    vm_attr_t *attr = PRIV(rv);
    memory_t *mem = attr->mem;

    blyt_lib_t libs[MAX_RUNTIME_LIBS];
    int nlibs = 0;

    blyt_symtab_t all_syms;
    all_syms.count = 0;

    /* BFS queue of library names to load (names come from DT_NEEDED entries
     * which are pointers into mmap'd files — collected before munmap). */
    char name_buf[MAX_RUNTIME_LIBS][64]; /* stable copies of library names */
    int qhead = 0;
    int qtail = 0;

    /* Seed the queue from the cart's DT_NEEDED */
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

        /* Skip if already loaded */
        bool dup = false;
        for (int i = 0; i < nlibs; i++) {
            /* Check against the SONAME embedded in the library — simplified:
             * compare load order name against queue name. */
            (void)i;
            /* Use library index as proxy: just check name_buf uniqueness. */
        }
        (void)dup; /* handled implicitly — each unique name loaded once */

        const uint8_t *lmap;
        size_t lsz;
        if (!open_lib(lib_dir, lib_name, &lmap, &lsz)) {
            ok = false;
            break;
        }

        const Elf32_Ehdr *leh = (const Elf32_Ehdr *)lmap;
        if (lsz < sizeof(Elf32_Ehdr) || leh->e_ident[EI_MAG0] != ELFMAG0 ||
            leh->e_machine != EM_RISCV) {
            munmap((void *)lmap, lsz);
            ok = false;
            break;
        }

        uint32_t bias =
            (leh->e_type == ET_DYN) ? GUEST_LIB_BASE + (uint32_t)nlibs * GUEST_LIB_STRIDE : 0;

        blyt_lib_t lib = {.map = lmap, .size = lsz, .bias = bias};

        if (!map_lib_segments(&lib, mem)) {
            munmap((void *)lmap, lsz);
            ok = false;
            break;
        }

        apply_lib_rela(&lib, mem);
        build_symtab(&lib, &all_syms);
        libs[nlibs++] = lib;

        /* Enqueue this library's DT_NEEDED for transitive loading */
        const char *needed[MAX_RUNTIME_LIBS];
        int nneeded = get_dt_needed(&lib, needed, MAX_RUNTIME_LIBS);
        for (int i = 0; i < nneeded && qtail < MAX_RUNTIME_LIBS; i++) {
            /* Check if already queued */
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
        /* Resolve PLT/GOT for each loaded library (e.g. libblyt32.so's PLT
         * entries pointing into libblytcommon.so must be resolved before
         * the cart executes). */
        for (int i = 0; i < nlibs; i++) {
            resolve_elf_plt(libs[i].map, libs[i].size, mem, libs[i].bias, &all_syms);
        }

        /* Resolve the cart's PLT/GOT against the combined symbol table. */
        resolve_elf_plt(cart->map, cart->map_size, mem, 0, &all_syms);
    }

    for (int i = 0; i < nlibs; i++)
        munmap((void *)libs[i].map, libs[i].size);

    return ok ? BLYT_RUN_OK : BLYT_RUN_ERR_EMU;
}

/* -------------------------------------------------------------------------
 * blyt_cart_run — public entry point
 * ------------------------------------------------------------------------- */

blyt_cart_run_err_t blyt_cart_run(blyt_cart_t *cart, blyt_log_fn log_fn) {
    blyt_run_ctx_t ctx = {.log_fn = log_fn, .ecall_trapped = false};
    g_run_ctx = &ctx;

    vm_attr_t attr = {
        .mem_size = BLYT_EMU_MEM_SIZE,
        .stack_size = BLYT_STACK_SIZE,
        .args_offset_size = 0,
        .argc = 0,
        .argv = NULL,
        .log_level = LOG_WARN,
        .cycle_per_step = BLYT_CYCLE_PER_STEP,
        .allow_misalign = false,
        .fd_stdin = STDIN_FILENO,
        .fd_stdout = STDOUT_FILENO,
        .fd_stderr = STDERR_FILENO,
        .data.user.elf_program = cart->path,
    };

    riscv_t *rv = rv_create(&attr);
    if (!rv) {
        g_run_ctx = NULL;
        return BLYT_RUN_ERR_EMU;
    }

    vm_attr_t *rattr = PRIV(rv);
    inject_exit_trampoline(rattr->mem);

    blyt_cart_run_err_t load_err = dynlink(rv, cart);
    if (load_err != BLYT_RUN_OK) {
        rv_delete(rv);
        g_run_ctx = NULL;
        return load_err;
    }

    rv->io.on_ecall = blyt_ecall_handler;
    rv_set_reg(rv, rv_reg_ra, BLYT_TRAMPOLINE_EXIT_ADDR);

    rv_run(rv);
    rv_delete(rv);

    bool trapped = ctx.ecall_trapped;
    g_run_ctx = NULL;

    return trapped ? BLYT_RUN_ERR_ECALL_TRAP : BLYT_RUN_OK;
}
