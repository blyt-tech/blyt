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
 * rv32emu headers — common.h must come first: it defines RV32_HAS,
 * FORCE_INLINE, HAVE_MMAP, and other macros the subsequent headers need.
 */
#include "../../third_party/rv32emu/src/common.h"
#include "../../third_party/rv32emu/src/io.h"
#include "../../third_party/rv32emu/src/riscv.h"
#include "../../third_party/rv32emu/src/riscv_private.h"

/* -------------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------------- */

/* rv32emu guest memory size: 256 MiB matches rv32emu's default MEM_SIZE.
 * libblyt32.so is mapped at GUEST_LIB_BASE (128 MiB), validated by Spike C.
 * The stack lives near the top of this range. */
#define BLYT_EMU_MEM_SIZE (256u * 1024u * 1024u)
#define BLYT_STACK_SIZE (1u * 1024u * 1024u)
#define BLYT_CYCLE_PER_STEP 500

/* Guest address at which runtime libraries are mapped.
 * 128 MiB: well above the cart (starts at ~0x10000) and well below the
 * stack (near top of 256 MiB).  Validated by Spike C. */
#define GUEST_LIB_BASE 0x08000000u

/* -------------------------------------------------------------------------
 * Global context shared with the ECALL handler
 * ------------------------------------------------------------------------- */

typedef struct {
    blyt_log_fn log_fn;
    bool ecall_trapped;
} blyt_run_ctx_t;

static blyt_run_ctx_t *g_run_ctx = NULL;

/* -------------------------------------------------------------------------
 * EXIT trampoline injection
 *
 * Writes a 12-byte EXIT stub into rv32emu guest memory at
 * BLYT_TRAMPOLINE_EXIT_ADDR.  The runtime sets RA to this address before
 * calling blyt_main, so when blyt_main returns the CPU lands here and
 * issues ECALL 0 → the host on_ecall handler halts the emulator.
 *
 * CONSOLE_DEBUG no longer needs a trampoline: its ecall fires from inside
 * libblyt32.so, which the dynamic loader maps into guest memory.
 * ------------------------------------------------------------------------- */

static void write_u32_le(uint8_t *dst, uint32_t val) {
    dst[0] = (uint8_t)val;
    dst[1] = (uint8_t)(val >> 8);
    dst[2] = (uint8_t)(val >> 16);
    dst[3] = (uint8_t)(val >> 24);
}

static bool inject_exit_trampoline(memory_t *mem) {
    uint8_t stub[12];
    write_u32_le(stub + 0, RV32_LI_A7_0); /* addi a7, x0, 0 */
    write_u32_le(stub + 4, RV32_ECALL);
    write_u32_le(stub + 8, RV32_UNIMP);
    return memory_write(mem, BLYT_TRAMPOLINE_EXIT_ADDR, stub, sizeof(stub));
}

/* -------------------------------------------------------------------------
 * ECALL handler (ADR-0085 calling convention)
 *
 * a0 = first argument (pointer for string args)
 * a1 = second argument (byte length for string args)
 * a7 = ECALL number
 * a0 = return value (blyt_result_t) after ecall returns
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

        /* Advance PC past the ecall: RVOP/fuse6 leave rv->PC at the ecall
         * instruction address; without this the emulator re-executes it. */
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
 * Mini dynamic loader (Spike C approach)
 *
 * After rv_create() has loaded the cart ELF into rv32emu guest memory,
 * this loader:
 *   1. Reads libblyt32.so from BLYT_LIB_DIR (env var).
 *   2. Maps its PT_LOAD segments into rv32emu guest memory at GUEST_LIB_BASE.
 *   3. Applies the library's own SHT_RELA relocations (R_RISCV_RELATIVE).
 *   4. Resolves the cart's PLT (R_RISCV_JUMP_SLOT) and data GOT
 *      (R_RISCV_GLOB_DAT) entries against the library's exported symbols.
 * ------------------------------------------------------------------------- */

typedef struct {
    char name[128];
    uint32_t guest_addr; /* virtual address inside rv32emu guest */
} blyt_sym_t;

#define MAX_SYMS 256

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

/* Map a library's PT_LOAD segments into rv32emu guest memory.
 * Returns the load bias (GUEST_LIB_BASE for ET_DYN, 0 for ET_EXEC). */
static bool map_lib_segments(const uint8_t *lib_map, size_t lib_size, memory_t *mem,
                             uint32_t bias) {
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)lib_map;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        size_t off = (size_t)eh->e_phoff + (size_t)i * eh->e_phentsize;
        if (off + sizeof(Elf32_Phdr) > lib_size)
            return false;
        const Elf32_Phdr *ph = (const Elf32_Phdr *)(lib_map + off);
        if (ph->p_type != PT_LOAD)
            continue;
        if ((size_t)ph->p_offset + ph->p_filesz > lib_size)
            return false;
        uint32_t guest_addr = bias + ph->p_vaddr;
        if (!memory_write(mem, guest_addr, lib_map + ph->p_offset, ph->p_filesz))
            return false;
        /* Zero BSS (memsz > filesz) */
        if (ph->p_memsz > ph->p_filesz) {
            uint32_t bss_start = guest_addr + ph->p_filesz;
            uint32_t bss_size = ph->p_memsz - ph->p_filesz;
            if (!memory_fill(mem, bss_start, bss_size, 0))
                return false;
        }
    }
    return true;
}

/* Apply the library's own RELA relocations (R_RISCV_RELATIVE).
 * Called after map_lib_segments so the bytes are in guest memory. */
static void apply_lib_rela(const uint8_t *lib_map, size_t lib_size, memory_t *mem, uint32_t bias) {
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)lib_map;
    if (eh->e_shentsize < sizeof(Elf32_Shdr) || eh->e_shnum == 0)
        return;

    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        size_t off = (size_t)eh->e_shoff + (size_t)i * eh->e_shentsize;
        if (off + sizeof(Elf32_Shdr) > lib_size)
            break;
        const Elf32_Shdr *sh = (const Elf32_Shdr *)(lib_map + off);
        if (sh->sh_type != SHT_RELA || sh->sh_entsize < sizeof(Elf32_Rela))
            continue;

        size_t count = sh->sh_size / sh->sh_entsize;
        for (size_t j = 0; j < count; j++) {
            size_t roff = sh->sh_offset + j * sh->sh_entsize;
            if (roff + sizeof(Elf32_Rela) > lib_size)
                break;
            const Elf32_Rela *r = (const Elf32_Rela *)(lib_map + roff);
            uint32_t type = ELF32_R_TYPE(r->r_info);

            if (type == R_RISCV_RELATIVE) {
                /* S + A where S = bias (for ET_DYN) */
                uint32_t val = bias + (uint32_t)r->r_addend;
                uint8_t val_le[4];
                write_u32_le(val_le, val);
                memory_write(mem, bias + r->r_offset, val_le, 4);
            }
        }
    }
}

/* Build a symbol table from the library's .dynsym. */
static void build_symtab(const uint8_t *lib_map, size_t lib_size, uint32_t bias,
                         blyt_symtab_t *st) {
    st->count = 0;
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)lib_map;
    if (eh->e_shentsize < sizeof(Elf32_Shdr) || eh->e_shnum == 0)
        return;

    const Elf32_Shdr *dynsym_sh = NULL;
    const Elf32_Shdr *dynstr_sh = NULL;

    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        size_t off = (size_t)eh->e_shoff + (size_t)i * eh->e_shentsize;
        if (off + sizeof(Elf32_Shdr) > lib_size)
            break;
        const Elf32_Shdr *sh = (const Elf32_Shdr *)(lib_map + off);
        if (sh->sh_type == SHT_DYNSYM && !dynsym_sh)
            dynsym_sh = sh;
        /* .dynstr is the string table linked from .dynsym */
        if (sh->sh_type == SHT_STRTAB && dynsym_sh &&
            (uint16_t)(dynsym_sh - (const Elf32_Shdr *)(lib_map + eh->e_shoff)) ==
                (uint16_t)(i - 1)) {
            dynstr_sh = sh;
        }
    }

    /* Fallback: find .dynstr by sh_link of .dynsym */
    if (!dynstr_sh && dynsym_sh && dynsym_sh->sh_link < eh->e_shnum) {
        size_t off = (size_t)eh->e_shoff + (size_t)dynsym_sh->sh_link * eh->e_shentsize;
        if (off + sizeof(Elf32_Shdr) <= lib_size)
            dynstr_sh = (const Elf32_Shdr *)(lib_map + off);
    }

    if (!dynsym_sh || !dynstr_sh)
        return;
    if (dynsym_sh->sh_entsize < sizeof(Elf32_Sym))
        return;

    const char *strtab = (const char *)(lib_map + dynstr_sh->sh_offset);
    size_t strtab_size = dynstr_sh->sh_size;
    size_t nsyms = dynsym_sh->sh_size / dynsym_sh->sh_entsize;

    for (size_t j = 0; j < nsyms && st->count < MAX_SYMS; j++) {
        size_t sym_off = dynsym_sh->sh_offset + j * dynsym_sh->sh_entsize;
        if (sym_off + sizeof(Elf32_Sym) > lib_size)
            break;
        const Elf32_Sym *sym = (const Elf32_Sym *)(lib_map + sym_off);

        /* Only exported (defined, global/weak) symbols */
        if (sym->st_shndx == SHN_UNDEF)
            continue;
        uint8_t bind = ELF32_ST_BIND(sym->st_info);
        if (bind != STB_GLOBAL && bind != STB_WEAK)
            continue;
        if (sym->st_name >= strtab_size)
            continue;

        const char *name = strtab + sym->st_name;
        if (name[0] == '\0')
            continue;

        blyt_sym_t *s = &st->syms[st->count++];
        strncpy(s->name, name, sizeof(s->name) - 1);
        s->name[sizeof(s->name) - 1] = '\0';
        s->guest_addr = bias + sym->st_value;
    }
}

/* Resolve the cart's PLT and data-GOT entries against the loaded library. */
static void resolve_cart_plt(const blyt_cart_t *cart, memory_t *mem,
                             const blyt_symtab_t *lib_syms) {
    const uint8_t *map = (const uint8_t *)cart->map;
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)map;

    if (eh->e_shnum == 0 || eh->e_shentsize < sizeof(Elf32_Shdr))
        return;

    const Elf32_Shdr *shdrs = (const Elf32_Shdr *)(map + eh->e_shoff);
    const Elf32_Shdr *shstrtab_sh = &shdrs[eh->e_shstrndx];
    const char *shstrtab = (const char *)(map + shstrtab_sh->sh_offset);

    /* Find .dynsym, .dynstr, and all RELA sections (.rela.plt, .rela.dyn) */
    const Elf32_Shdr *dynsym_sh = NULL;
    const Elf32_Shdr *dynstr_sh = NULL;

    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        const Elf32_Shdr *sh = &shdrs[i];
        if (sh->sh_type == SHT_DYNSYM)
            dynsym_sh = sh;
        if (sh->sh_type == SHT_STRTAB && strcmp(shstrtab + sh->sh_name, ".dynstr") == 0)
            dynstr_sh = sh;
    }

    if (!dynsym_sh || !dynstr_sh)
        return;
    if (dynsym_sh->sh_entsize < sizeof(Elf32_Sym))
        return;

    const char *sym_strtab = (const char *)(map + dynstr_sh->sh_offset);
    size_t sym_strtab_size = dynstr_sh->sh_size;

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
            if (sym_idx * dynsym_sh->sh_entsize + sizeof(Elf32_Sym) > dynsym_sh->sh_size)
                continue;

            const Elf32_Sym *sym =
                (const Elf32_Sym *)(map + dynsym_sh->sh_offset + sym_idx * dynsym_sh->sh_entsize);
            if (sym->st_name >= sym_strtab_size)
                continue;

            const char *sym_name = sym_strtab + sym->st_name;
            uint32_t resolved = symtab_lookup(lib_syms, sym_name);
            if (resolved == 0)
                continue;

            uint8_t val_le[4];
            write_u32_le(val_le, resolved);
            memory_write(mem, r->r_offset, val_le, 4);
        }
    }
}

/* Top-level dynamic loader: finds libblyt32.so, loads it, resolves links. */
static blyt_cart_run_err_t dynlink(riscv_t *rv, const blyt_cart_t *cart) {
    /* Locate libblyt32.so via BLYT_LIB_DIR environment variable.
     * The integration tests and CI both set this to the build directory. */
    const char *lib_dir = getenv("BLYT_LIB_DIR");
    if (!lib_dir || lib_dir[0] == '\0') {
        fprintf(stderr, "blyt: BLYT_LIB_DIR not set — cannot locate libblyt32.so\n");
        return BLYT_RUN_ERR_EMU;
    }

    char lib_path[4096];
    int n = snprintf(lib_path, sizeof(lib_path), "%s/libblyt32.so", lib_dir);
    if (n < 0 || (size_t)n >= sizeof(lib_path))
        return BLYT_RUN_ERR_EMU;

    int fd = open(lib_path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "blyt: cannot open %s\n", lib_path);
        return BLYT_RUN_ERR_EMU;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return BLYT_RUN_ERR_EMU;
    }

    void *lib_map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (lib_map == MAP_FAILED)
        return BLYT_RUN_ERR_EMU;

    const uint8_t *lib = (const uint8_t *)lib_map;
    size_t lib_size = (size_t)st.st_size;
    const Elf32_Ehdr *leh = (const Elf32_Ehdr *)lib;
    bool ok = true;

    /* Basic ELF sanity for the library */
    if (lib_size < sizeof(Elf32_Ehdr) || leh->e_ident[EI_MAG0] != ELFMAG0 ||
        leh->e_machine != EM_RISCV) {
        ok = false;
        goto done;
    }

    /* ET_DYN (PIC .so): load bias = GUEST_LIB_BASE */
    uint32_t bias = (leh->e_type == ET_DYN) ? GUEST_LIB_BASE : 0;

    vm_attr_t *attr = PRIV(rv);
    memory_t *mem = attr->mem;

    if (!map_lib_segments(lib, lib_size, mem, bias)) {
        ok = false;
        goto done;
    }

    apply_lib_rela(lib, lib_size, mem, bias);

    blyt_symtab_t lib_syms;
    build_symtab(lib, lib_size, bias, &lib_syms);
    resolve_cart_plt(cart, mem, &lib_syms);

done:
    munmap(lib_map, lib_size);
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

    /* Inject the EXIT trampoline and run the dynamic loader */
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
