#include "cart_run.h"

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "blyt_runtime.h"
#include "cart_load.h"
#include "ecall.h"
#include "elf32.h"

/*
 * rv32emu headers — internal access to riscv_t fields.
 * common.h must come first: it defines RV32_HAS, FORCE_INLINE, HAVE_MMAP,
 * and other macros that the subsequent rv32emu headers depend on.
 */
#include "../../third_party/rv32emu/src/common.h"
#include "../../third_party/rv32emu/src/io.h"
#include "../../third_party/rv32emu/src/riscv.h"
#include "../../third_party/rv32emu/src/riscv_private.h"

/* -------------------------------------------------------------------------
 * Emulator constants
 * ------------------------------------------------------------------------- */

#define BLYT_EMU_MEM_SIZE (32u * 1024u * 1024u) /* 32 MB */
#define BLYT_STACK_SIZE (1u * 1024u * 1024u) /* 1 MB */
#define BLYT_CYCLE_PER_STEP 500

/* -------------------------------------------------------------------------
 * Global context shared with the ecall handler
 *
 * rv32emu's vm_attr_t has no spare user-data slot, so we keep a global.
 * Cart execution is single-threaded; a global is safe here.
 * ------------------------------------------------------------------------- */

typedef struct {
    blyt_log_fn log_fn;
    bool ecall_trapped; /* set if a non-permitted ecall fired */
} blyt_run_ctx_t;

static blyt_run_ctx_t *g_run_ctx = NULL;

/* -------------------------------------------------------------------------
 * ECALL handler
 * ------------------------------------------------------------------------- */

static void blyt_ecall_handler(riscv_t *rv) {
    uint32_t num = rv_get_reg(rv, rv_reg_a7);

    switch (num) {
    case BLYT_ECALL_EXIT:
        rv_halt(rv);
        return;

    case BLYT_ECALL_CONSOLE_DEBUG: {
        uint32_t vaddr = rv_get_reg(rv, rv_reg_a0);
        vm_attr_t *attr = PRIV(rv);
        memory_t *mem = attr->mem;

        /* Read NUL-terminated string from emulated memory with bounds check */
        char buf[4096];
        size_t i;
        for (i = 0; i < sizeof(buf) - 1; i++) {
            if (!GUEST_RAM_CONTAINS(mem, vaddr + i, 1))
                break;
            uint8_t c;
            memory_read(mem, &c, vaddr + (uint32_t)i, 1);
            if (c == 0)
                break;
            buf[i] = (char)c;
        }
        buf[i] = '\0';

        if (g_run_ctx && g_run_ctx->log_fn)
            g_run_ctx->log_fn(buf);

        /* Advance PC past the ecall instruction. The RVOP and fuse6 handlers
         * both leave rv->PC pointing at the ecall itself; without this advance
         * the emulator re-executes the ecall on the next cycle. */
        rv->PC += 4;
        return;
    }

    default:
        /* Non-permitted ecall — halt and record the violation */
        rv_halt(rv);
        if (g_run_ctx)
            g_run_ctx->ecall_trapped = true;
        return;
    }
}

/* -------------------------------------------------------------------------
 * Trampoline injection
 *
 * Writes the blyt trampoline page at BLYT_TRAMPOLINE_BASE in emulated memory.
 * Each entry is a short RV32 stub: set a7, ecall, ret/unimp.
 * ------------------------------------------------------------------------- */

static void write_u32_le(uint8_t *dst, uint32_t val) {
    dst[0] = (uint8_t)(val);
    dst[1] = (uint8_t)(val >> 8);
    dst[2] = (uint8_t)(val >> 16);
    dst[3] = (uint8_t)(val >> 24);
}

static bool inject_trampolines(memory_t *mem) {
    uint8_t page[32];
    memset(page, 0, sizeof(page));

    /* EXIT trampoline at offset 0 */
    write_u32_le(page + 0, RV32_LI_A7_0);
    write_u32_le(page + 4, RV32_ECALL);
    write_u32_le(page + 8, RV32_UNIMP);

    /* CONSOLE_DEBUG trampoline at offset 12 */
    write_u32_le(page + 12, RV32_LI_A7_1);
    write_u32_le(page + 16, RV32_ECALL);
    write_u32_le(page + 20, RV32_RET);

    return memory_write(mem, BLYT_TRAMPOLINE_BASE, page, sizeof(page));
}

/* -------------------------------------------------------------------------
 * GOT patching — resolve blyt API symbols to their trampolines
 *
 * Walks the cart's .rela.plt section (type SHT_RELA). For each entry with
 * relocation type R_RISCV_JUMP_SLOT, looks up the symbol name and, if it's
 * a known blyt API function, writes the trampoline address into the
 * emulated GOT at r_offset.
 * ------------------------------------------------------------------------- */

typedef struct {
    const char *name;
    uint32_t trampoline_addr;
} blyt_sym_map_t;

static const blyt_sym_map_t BLYT_SYM_MAP[] = {
    {"blyt_console_debug", BLYT_TRAMPOLINE_CONSOLE_DEBUG_ADDR},
    {NULL, 0},
};

static uint32_t lookup_trampoline(const char *name) {
    for (int i = 0; BLYT_SYM_MAP[i].name; i++) {
        if (strcmp(BLYT_SYM_MAP[i].name, name) == 0)
            return BLYT_SYM_MAP[i].trampoline_addr;
    }
    return 0;
}

static void patch_got(const blyt_cart_t *cart, memory_t *mem) {
    const uint8_t *map = (const uint8_t *)cart->map;
    size_t map_size = cart->map_size;
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)map;

    if (eh->e_shnum == 0 || eh->e_shentsize < sizeof(Elf32_Shdr))
        return;

    const Elf32_Shdr *shdrs = (const Elf32_Shdr *)(map + eh->e_shoff);
    const Elf32_Shdr *shstrtab_sh = &shdrs[eh->e_shstrndx];
    const char *shstrtab = (const char *)(map + shstrtab_sh->sh_offset);

    /* Find .rela.plt, .dynsym, .dynstr */
    const Elf32_Shdr *rela_plt = NULL;
    const Elf32_Shdr *dynsym_sh = NULL;
    const Elf32_Shdr *dynstr_sh = NULL;

    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        const Elf32_Shdr *sh = &shdrs[i];
        const char *name = shstrtab + sh->sh_name;

        if (sh->sh_type == SHT_RELA && strcmp(name, ".rela.plt") == 0)
            rela_plt = sh;
        else if (sh->sh_type == SHT_DYNSYM)
            dynsym_sh = sh;
        else if (sh->sh_type == SHT_STRTAB && strcmp(name, ".dynstr") == 0)
            dynstr_sh = sh;
    }

    if (!rela_plt || !dynsym_sh || !dynstr_sh)
        return;

    if (rela_plt->sh_entsize < sizeof(Elf32_Rela))
        return;
    if (dynsym_sh->sh_entsize < sizeof(Elf32_Sym))
        return;

    const char *dynstr = (const char *)(map + dynstr_sh->sh_offset);
    size_t dynstr_size = dynstr_sh->sh_size;
    const uint8_t *sym_data = map + dynsym_sh->sh_offset;
    size_t sym_count = dynsym_sh->sh_size / dynsym_sh->sh_entsize;
    const uint8_t *rela_data = map + rela_plt->sh_offset;
    size_t rela_count = rela_plt->sh_size / rela_plt->sh_entsize;

    for (size_t j = 0; j < rela_count; j++) {
        const Elf32_Rela *r = (const Elf32_Rela *)(rela_data + j * rela_plt->sh_entsize);

        if (ELF32_R_TYPE(r->r_info) != R_RISCV_JUMP_SLOT)
            continue;

        uint32_t sym_idx = ELF32_R_SYM(r->r_info);
        if (sym_idx >= sym_count)
            continue;

        const Elf32_Sym *sym = (const Elf32_Sym *)(sym_data + sym_idx * dynsym_sh->sh_entsize);
        if (sym->st_name >= dynstr_size)
            continue;

        const char *sym_name = dynstr + sym->st_name;
        uint32_t trampoline = lookup_trampoline(sym_name);
        if (trampoline == 0)
            continue;

        /* Write the trampoline address into the GOT entry (little-endian) */
        uint8_t addr_le[4];
        write_u32_le(addr_le, trampoline);
        (void)memory_write(mem, r->r_offset, addr_le, 4);
    }

    /*
     * If the cart has a .got.plt section with an initial PLT resolver entry,
     * overwrite the first 12 bytes with zeros to disable lazy binding.
     * Real dynamic linkers set slot 0 = link_map, slot 1 = resolver, slot 2
     * = dl_runtime_resolve; we have none of those — zeroing them out is safe
     * because our GOT patches above already supply the final addresses.
     */
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        const Elf32_Shdr *sh = &shdrs[i];
        const char *name = shstrtab + sh->sh_name;
        if (strcmp(name, ".got.plt") == 0 && sh->sh_size >= 12 &&
            sh->sh_addr + 12 <= (uint32_t)map_size) {
            uint8_t zeros[12] = {0};
            (void)memory_write(mem, sh->sh_addr, zeros, 12);
            break;
        }
    }
}

/* -------------------------------------------------------------------------
 * blyt_cart_run — public entry point (wraps the implementation)
 * ------------------------------------------------------------------------- */

blyt_cart_run_err_t blyt_cart_run(blyt_cart_t *cart, blyt_log_fn log_fn) {
    /* Write cart data to a temp file so rv32emu can open it */
    char tmp_path[] = "/tmp/blyt_cart_XXXXXX";
    int tmp_fd = mkstemp(tmp_path);
    if (tmp_fd < 0)
        return BLYT_RUN_ERR_TMPFILE;

    ssize_t written = write(tmp_fd, cart->map, cart->map_size);
    close(tmp_fd);
    if (written < 0 || (size_t)written != cart->map_size) {
        unlink(tmp_path);
        return BLYT_RUN_ERR_TMPFILE;
    }

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
        .data.user.elf_program = tmp_path,
    };

    riscv_t *rv = rv_create(&attr);
    unlink(tmp_path);

    if (!rv) {
        g_run_ctx = NULL;
        return BLYT_RUN_ERR_EMU;
    }

    /* Inject trampolines and patch GOT before execution */
    vm_attr_t *rattr = PRIV(rv);
    inject_trampolines(rattr->mem);
    patch_got(cart, rattr->mem);

    /* Override ecall handler with our blyt dispatcher */
    rv->io.on_ecall = blyt_ecall_handler;

    /*
     * When blyt_main returns, PC goes to wherever RA points. Set RA to the
     * EXIT trampoline so a clean return halts the emulator.
     */
    rv_set_reg(rv, rv_reg_ra, BLYT_TRAMPOLINE_EXIT_ADDR);

    rv_run(rv);
    rv_delete(rv);

    bool trapped = ctx.ecall_trapped;
    g_run_ctx = NULL;

    return trapped ? BLYT_RUN_ERR_ECALL_TRAP : BLYT_RUN_OK;
}
