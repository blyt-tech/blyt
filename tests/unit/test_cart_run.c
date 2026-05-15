/*
 * Unit tests for the Phase 3 ECALL dispatch mechanism.
 *
 * These tests create minimal RISC-V ELF binaries containing ecall
 * instructions (bypassing Phase 2 security checks, which is intentional
 * for unit testing the executor in isolation) and verify that the blyt
 * ecall handler dispatches correctly.
 *
 * Tested:
 *   1. BLYT_ECALL_CONSOLE_DEBUG fires the log callback with the right string.
 *   2. BLYT_ECALL_EXIT halts the emulator cleanly.
 *   3. An unknown ecall number sets the trap flag and halts.
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * rv32emu headers — common.h first to define RV32_HAS, FORCE_INLINE, etc.
 */
#include "../../third_party/rv32emu/src/common.h"
#include "../../third_party/rv32emu/src/io.h"
#include "../../third_party/rv32emu/src/log.h"
#include "../../third_party/rv32emu/src/riscv.h"
#include "../../third_party/rv32emu/src/riscv_private.h"

/* blyt ecall constants */
#include "ecall.h"
#include "elf32.h"

/* -------------------------------------------------------------------------
 * Minimal ELF builder for the test
 *
 * Produces a single-segment (PT_LOAD PF_R|PF_X) ELF that rv32emu can load.
 * The segment contains:
 *   code_len bytes of RISC-V instructions at virtual address CODE_VADDR
 *   padding to data_offset
 *   NUL-terminated string data at CODE_VADDR + data_offset
 * ------------------------------------------------------------------------- */

#define CODE_VADDR 0x10000u
#define DATA_OFFSET 0x100u /* string placed 256 bytes into the segment */

static char *build_and_write_test_elf(const uint32_t *insns, size_t n_insns, const char *data_str) {
    size_t code_bytes = n_insns * 4;
    size_t str_bytes = strlen(data_str) + 1;
    size_t seg_size = DATA_OFFSET + str_bytes;
    if (code_bytes > DATA_OFFSET)
        return NULL; /* code overflows into data area */

    size_t ehdr_sz = sizeof(Elf32_Ehdr);
    size_t phdr_sz = sizeof(Elf32_Phdr);
    size_t file_size = ehdr_sz + phdr_sz + seg_size;

    uint8_t *buf = calloc(1, file_size);
    if (!buf)
        return NULL;

    /* ELF header — rv32emu only checks magic, ELFCLASS32, EM_RISCV */
    Elf32_Ehdr *eh = (Elf32_Ehdr *)buf;
    eh->e_ident[EI_MAG0] = ELFMAG0;
    eh->e_ident[EI_MAG1] = ELFMAG1;
    eh->e_ident[EI_MAG2] = ELFMAG2;
    eh->e_ident[EI_MAG3] = ELFMAG3;
    eh->e_ident[EI_CLASS] = ELFCLASS32;
    eh->e_ident[EI_DATA] = ELFDATA2LSB;
    eh->e_ident[EI_OSABI] = ELFOSABI_NONE;
    eh->e_machine = EM_RISCV;
    eh->e_flags = BLYT_CART_EF_FLAGS;
    eh->e_ehsize = (Elf32_Half)ehdr_sz;
    eh->e_phentsize = (Elf32_Half)sizeof(Elf32_Phdr);
    eh->e_phnum = 1;
    eh->e_phoff = (Elf32_Off)ehdr_sz;
    eh->e_shnum = 0;
    eh->e_shoff = 0;
    eh->e_shstrndx = 0;
    eh->e_entry = CODE_VADDR; /* entry point = start of code */

    /* Program header: one PT_LOAD covering code + data */
    Elf32_Phdr *ph = (Elf32_Phdr *)(buf + ehdr_sz);
    ph->p_type = PT_LOAD;
    ph->p_offset = (Elf32_Off)(ehdr_sz + phdr_sz);
    ph->p_vaddr = CODE_VADDR;
    ph->p_paddr = CODE_VADDR;
    ph->p_filesz = (Elf32_Word)seg_size;
    ph->p_memsz = (Elf32_Word)seg_size;
    ph->p_flags = PF_R | PF_X;
    ph->p_align = 4;

    /* Copy instructions (little-endian, already encoded) */
    uint8_t *seg = buf + ehdr_sz + phdr_sz;
    for (size_t i = 0; i < n_insns; i++) {
        uint32_t w = insns[i];
        seg[i * 4 + 0] = (uint8_t)w;
        seg[i * 4 + 1] = (uint8_t)(w >> 8);
        seg[i * 4 + 2] = (uint8_t)(w >> 16);
        seg[i * 4 + 3] = (uint8_t)(w >> 24);
    }

    /* String data */
    memcpy(seg + DATA_OFFSET, data_str, str_bytes);

    /* Write to temp file */
    char *path = strdup("/tmp/blyt_test_rv_XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0) {
        free(buf);
        free(path);
        return NULL;
    }
    ssize_t written = write(fd, buf, file_size);
    close(fd);
    free(buf);
    if (written < 0 || (size_t)written != file_size) {
        unlink(path);
        free(path);
        return NULL;
    }
    return path;
}

/* -------------------------------------------------------------------------
 * Test ecall handler (mirrors cart_run.c's handler)
 * ------------------------------------------------------------------------- */

typedef struct {
    void (*log_fn)(const char *);
    int ecall_trapped;
} test_ctx_t;

static test_ctx_t *g_test_ctx = NULL;

static void test_ecall_handler(riscv_t *rv) {
    uint32_t num = rv_get_reg(rv, rv_reg_a7);

    switch (num) {
    case BLYT_ECALL_EXIT:
        rv_halt(rv);
        return;

    case BLYT_ECALL_CONSOLE_DEBUG: {
        uint32_t vaddr = rv_get_reg(rv, rv_reg_a0);
        vm_attr_t *attr = PRIV(rv);
        memory_t *mem = attr->mem;

        char buf[256];
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

        if (g_test_ctx && g_test_ctx->log_fn)
            g_test_ctx->log_fn(buf);

        /* Advance PC past the ecall: RVOP/fuse6 leave rv->PC at the ecall
         * address; without this the emulator re-executes it forever. */
        rv->PC += 4;
        return;
    }

    default:
        rv_halt(rv);
        if (g_test_ctx)
            g_test_ctx->ecall_trapped = 1;
        return;
    }
}

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static int failures = 0;

#define PASS(name) printf("PASS %s\n", (name))
#define FAIL(name, ...)                                                                            \
    do {                                                                                           \
        fprintf(stderr, "FAIL %s: ", (name));                                                      \
        fprintf(stderr, __VA_ARGS__);                                                              \
        fprintf(stderr, "\n");                                                                     \
        failures++;                                                                                \
    } while (0)

/* Create rv32emu instance from a temp ELF path, override on_ecall */
static riscv_t *make_rv_from_path(char *elf_path, vm_attr_t *attr) {
    memset(attr, 0, sizeof(*attr));
    attr->mem_size = 4u * 1024u * 1024u; /* 4 MB */
    attr->stack_size = 64u * 1024u; /* 64 KB */
    attr->log_level = LOG_FATAL; /* suppress emulator noise */
    attr->cycle_per_step = 200;
    attr->fd_stdin = STDIN_FILENO;
    attr->fd_stdout = STDOUT_FILENO;
    attr->fd_stderr = STDERR_FILENO;
    attr->data.user.elf_program = elf_path;

    riscv_t *rv = rv_create(attr);
    if (!rv)
        return NULL;

    /* Install our test ecall dispatcher */
    rv->io.on_ecall = test_ecall_handler;

    /* Point RA to EXIT trampoline so any stray `ret` halts cleanly */
    rv_set_reg(rv, rv_reg_ra, BLYT_TRAMPOLINE_EXIT_ADDR);

    return rv;
}

/* -------------------------------------------------------------------------
 * Test 1: BLYT_ECALL_CONSOLE_DEBUG delivers the string to the callback.
 *
 * RV32 program:
 *   lui  a0, (CODE_VADDR >> 12)  ; a0 = CODE_VADDR = 0x10000
 *   addi a0, a0, DATA_OFFSET     ; a0 = CODE_VADDR + DATA_OFFSET = 0x10100
 *   addi a7, x0, 1               ; a7 = BLYT_ECALL_CONSOLE_DEBUG
 *   ecall
 *   addi a7, x0, 0               ; a7 = BLYT_ECALL_EXIT
 *   ecall
 * ------------------------------------------------------------------------- */

static const char *captured_msg;

static void capture_log(const char *msg) {
    static char buf[256];
    strncpy(buf, msg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    captured_msg = buf;
}

static void test_console_debug(void) {
    const char *name = "console_debug_ecall_fires_callback";
    const char *test_str = "hello from cart";

    /*
     * lui a0, 0x10    -> a0 = 0x10000    encoding: 0x00010537
     * addi a0,a0,0x100-> a0 = 0x10100   encoding: 0x10050513
     * addi a7,x0,1                       encoding: RV32_LI_A7_1
     * ecall                              encoding: RV32_ECALL
     * addi a7,x0,0                       encoding: RV32_LI_A7_0
     * ecall                              encoding: RV32_ECALL
     */
    static const uint32_t prog[] = {
        UINT32_C(0x00010537), /* lui  a0, 0x10 */
        UINT32_C(0x10050513), /* addi a0, a0, 0x100 */
        RV32_LI_A7_1,         RV32_ECALL, RV32_LI_A7_0, RV32_ECALL,
    };

    char *path = build_and_write_test_elf(prog, 6, test_str);
    if (!path) {
        FAIL(name, "failed to build test ELF");
        return;
    }

    vm_attr_t attr;
    riscv_t *rv = make_rv_from_path(path, &attr);
    unlink(path);
    free(path);
    if (!rv) {
        FAIL(name, "rv_create failed");
        return;
    }

    test_ctx_t ctx = {.log_fn = capture_log, .ecall_trapped = 0};
    g_test_ctx = &ctx;
    captured_msg = NULL;

    rv_run(rv);
    rv_delete(rv);
    g_test_ctx = NULL;

    if (ctx.ecall_trapped)
        FAIL(name, "unexpected ecall trap");
    else if (!captured_msg)
        FAIL(name, "log callback was never called");
    else if (strcmp(captured_msg, test_str) != 0)
        FAIL(name, "expected '%s', got '%s'", test_str, captured_msg);
    else
        PASS(name);
}

/* -------------------------------------------------------------------------
 * Test 2: BLYT_ECALL_EXIT (a7=0) halts cleanly, no trap flag set.
 *
 * RV32 program:
 *   addi a7, x0, 0   ; BLYT_ECALL_EXIT
 *   ecall
 * ------------------------------------------------------------------------- */

static void test_exit_halts(void) {
    const char *name = "exit_ecall_halts_cleanly";

    static const uint32_t prog[] = {
        RV32_LI_A7_0,
        RV32_ECALL,
    };

    char *path = build_and_write_test_elf(prog, 2, "");
    if (!path) {
        FAIL(name, "failed to build test ELF");
        return;
    }

    vm_attr_t attr;
    riscv_t *rv = make_rv_from_path(path, &attr);
    unlink(path);
    free(path);
    if (!rv) {
        FAIL(name, "rv_create failed");
        return;
    }

    test_ctx_t ctx = {.log_fn = NULL, .ecall_trapped = 0};
    g_test_ctx = &ctx;

    rv_run(rv);
    rv_delete(rv);
    g_test_ctx = NULL;

    if (ctx.ecall_trapped)
        FAIL(name, "ecall_trapped was set; expected clean exit");
    else
        PASS(name);
}

/* -------------------------------------------------------------------------
 * Test 3: An unknown ecall number sets ecall_trapped and halts.
 *
 * RV32 program:
 *   addi a7, x0, 0x99   ; unknown ecall = 153
 *   ecall
 * ------------------------------------------------------------------------- */

static void test_unknown_ecall_traps(void) {
    const char *name = "unknown_ecall_traps";

    /* addi x17, x0, 153 = 0x09900893 */
    static const uint32_t prog[] = {
        UINT32_C(0x09900893), /* addi a7, x0, 153 (unknown ecall) */
        RV32_ECALL,
    };

    char *path = build_and_write_test_elf(prog, 2, "");
    if (!path) {
        FAIL(name, "failed to build test ELF");
        return;
    }

    vm_attr_t attr;
    riscv_t *rv = make_rv_from_path(path, &attr);
    unlink(path);
    free(path);
    if (!rv) {
        FAIL(name, "rv_create failed");
        return;
    }

    test_ctx_t ctx = {.log_fn = NULL, .ecall_trapped = 0};
    g_test_ctx = &ctx;

    rv_run(rv);
    rv_delete(rv);
    g_test_ctx = NULL;

    if (!ctx.ecall_trapped)
        FAIL(name, "ecall_trapped was not set for ecall 153");
    else
        PASS(name);
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(void) {
    test_console_debug();
    test_exit_halts();
    test_unknown_ecall_traps();

    if (failures == 0) {
        printf("All tests passed.\n");
        return 0;
    }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}
