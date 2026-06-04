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
#ifdef BLYT_DAP
#include "dap_server.h"
#endif
#ifdef BLYT_GDB
#include "gdb_stub.h"
#include <pthread.h>
#ifdef __EMSCRIPTEN__
#include "gdb_transport_wasm.h"
#else
#include "gdb_transport_tcp.h"
#endif
#endif
#include "elf32.h"
#include "testcard.h"

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
#define MAX_SYMS 2048

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

typedef enum {
    BLYT_DEBUG_RUNNING = 0,
    BLYT_DEBUG_PAUSED_GDB = 1,
    BLYT_DEBUG_PAUSED_DAP = 2,
} blyt_debug_state_t;

typedef struct {
    blyt_log_fn log_fn;
    bool ecall_trapped; /* cart issued a non-permitted ecall */
    bool ecall_aborted; /* cart called abort() — ECALL_EXIT with a0 != 0 */
    bool frame_done; /* set by BLYT_ECALL_FRAME_DONE; cleared by run_frame */
    bool dap_enabled; /* set by blyt_session_dap_listen(); enables ECALL_DAP_HOOK */
    blyt_debug_state_t debug_state;
    bool gdb_enabled; /* set by blyt_session_gdb_listen() */
    bool gdb_single_step; /* set when vCont;s received; cleared after one step */
    bool gdb_ebreak_pending; /* ebreak fired inside rv_step; cleared by post-step handler */
    uint32_t gdb_bp_resume_addr; /* WASM: bp addr we paused at; cleared when stepping over */
} blyt_run_ctx_t;

static blyt_run_ctx_t *g_run_ctx = NULL;

/* -------------------------------------------------------------------------
 * Session
 * ------------------------------------------------------------------------- */

#define MAX_GDB_LIBS 8
#define MAX_GDB_BREAKS 128

typedef struct {
    char path[256]; /* host name or filesystem path of the .so */
    uint32_t l_addr; /* load base in guest memory */
    uint32_t l_ld; /* runtime address of .dynamic section */
} blyt_gdb_lib_t;

typedef struct {
    uint32_t addr;
    uint32_t original_word;
} blyt_gdb_bp_t;

struct blyt_session {
    riscv_t *rv;
    /* vm_attr_t must outlive rv: rv_create stores &attr in rv->data (rv->priv),
     * so it must not be stack-allocated in blyt_session_create. */
    vm_attr_t attr;
    blyt_run_ctx_t ctx;
    /* Palette-indexed framebuffer: filled by the runtime until the cart draws.
     * Frontends call blyt_session_expand_frame() to convert to XRGB8888. */
    uint8_t pixels[BLYT_FRAME_W * BLYT_FRAME_H];
    uint32_t palette[256]; /* XRGB8888 — set at session create, updated by cart */
    uint32_t frame_count;
    bool cart_has_drawn;

#ifdef BLYT_GDB
    /* Library layout for GDB qXfer:libraries-svr4:read. */
    blyt_gdb_lib_t gdb_libs[MAX_GDB_LIBS];
    fc_gdb_library_t gdb_libs_ffi[MAX_GDB_LIBS]; /* stable pointers into gdb_libs */
    int gdb_nlibs;
    char gdb_exec_path[4096]; /* cart path for qXfer:exec-file:read */
    /* Software breakpoints: saved original words. */
    blyt_gdb_bp_t gdb_bps[MAX_GDB_BREAKS];
    int gdb_nbp;
#endif
};

#ifdef BLYT_GDB
static blyt_session_t *g_gdb_session = NULL;
static pthread_mutex_t g_bp_mutex = PTHREAD_MUTEX_INITIALIZER;

static void gdb_read_regs(uint8_t out[33 * 4]) {
    if (!g_gdb_session)
        return;
    for (int i = 0; i < 32; i++) {
        uint32_t v = rv_get_reg(g_gdb_session->rv, (uint32_t)i);
        out[i * 4 + 0] = (uint8_t)(v);
        out[i * 4 + 1] = (uint8_t)(v >> 8);
        out[i * 4 + 2] = (uint8_t)(v >> 16);
        out[i * 4 + 3] = (uint8_t)(v >> 24);
    }
    uint32_t pc = rv_get_pc(g_gdb_session->rv);
    out[32 * 4 + 0] = (uint8_t)(pc);
    out[32 * 4 + 1] = (uint8_t)(pc >> 8);
    out[32 * 4 + 2] = (uint8_t)(pc >> 16);
    out[32 * 4 + 3] = (uint8_t)(pc >> 24);
}

static void gdb_write_regs(const uint8_t in[33 * 4]) {
    if (!g_gdb_session)
        return;
    for (int i = 0; i < 32; i++) {
        uint32_t v = (uint32_t)in[i * 4] | ((uint32_t)in[i * 4 + 1] << 8) |
                     ((uint32_t)in[i * 4 + 2] << 16) | ((uint32_t)in[i * 4 + 3] << 24);
        rv_set_reg(g_gdb_session->rv, (uint32_t)i, v);
    }
    /* PC is register index 32 */
    uint32_t pc = (uint32_t)in[32 * 4] | ((uint32_t)in[32 * 4 + 1] << 8) |
                  ((uint32_t)in[32 * 4 + 2] << 16) | ((uint32_t)in[32 * 4 + 3] << 24);
    g_gdb_session->rv->PC = pc;
}

static uint32_t gdb_read_mem(uint32_t addr, uint8_t *dst, uint32_t n) {
    if (!g_gdb_session)
        return 0;
    vm_attr_t *attr = PRIV(g_gdb_session->rv);
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (!GUEST_RAM_CONTAINS(attr->mem, addr + i, 1))
            break;
        memory_read(attr->mem, &dst[i], addr + i, 1);
    }
    return i;
}

static uint32_t gdb_write_mem(uint32_t addr, const uint8_t *src, uint32_t n) {
    if (!g_gdb_session)
        return 0;
    vm_attr_t *attr = PRIV(g_gdb_session->rv);
    if (!memory_write(attr->mem, addr, src, n))
        return 0;
    return n;
}

static int gdb_set_bp(uint32_t addr) {
    if (!g_gdb_session)
        return -1;
    blyt_session_t *s = g_gdb_session;
    pthread_mutex_lock(&g_bp_mutex);
    if (s->gdb_nbp >= MAX_GDB_BREAKS) {
        pthread_mutex_unlock(&g_bp_mutex);
        return -1;
    }
    /* Read original word. */
    uint8_t orig[4];
    if (gdb_read_mem(addr, orig, 4) != 4) {
        pthread_mutex_unlock(&g_bp_mutex);
        return -1;
    }
    uint32_t orig_word = (uint32_t)orig[0] | ((uint32_t)orig[1] << 8) | ((uint32_t)orig[2] << 16) |
                         ((uint32_t)orig[3] << 24);
    /* Use C.EBREAK (2-byte) for compressed instructions to avoid overwriting adjacent bytes. */
    bool is_compressed = (orig_word & 0x3) != 0x3;
    if (is_compressed) {
        static const uint8_t cebreak[2] = {0x02, 0x90}; /* C.EBREAK = 0x9002 */
        if (gdb_write_mem(addr, cebreak, 2) != 2) {
            pthread_mutex_unlock(&g_bp_mutex);
            return -1;
        }
    } else {
        static const uint8_t ebreak[4] = {0x73, 0x00, 0x10, 0x00};
        if (gdb_write_mem(addr, ebreak, 4) != 4) {
            pthread_mutex_unlock(&g_bp_mutex);
            return -1;
        }
    }
    s->gdb_bps[s->gdb_nbp].addr = addr;
    s->gdb_bps[s->gdb_nbp].original_word = orig_word;
    s->gdb_nbp++;
    pthread_mutex_unlock(&g_bp_mutex);
    return 0;
}

static int gdb_clear_bp(uint32_t addr) {
    if (!g_gdb_session)
        return -1;
    blyt_session_t *s = g_gdb_session;
    pthread_mutex_lock(&g_bp_mutex);
    for (int i = 0; i < s->gdb_nbp; i++) {
        if (s->gdb_bps[i].addr == addr) {
            uint32_t w = s->gdb_bps[i].original_word;
            uint8_t bytes[4] = {(uint8_t)w, (uint8_t)(w >> 8), (uint8_t)(w >> 16),
                                (uint8_t)(w >> 24)};
            bool compressed = (w & 0x3) != 0x3;
            gdb_write_mem(addr, bytes, compressed ? 2 : 4);
            /* Remove entry by shifting. */
            for (int j = i; j + 1 < s->gdb_nbp; j++)
                s->gdb_bps[j] = s->gdb_bps[j + 1];
            s->gdb_nbp--;
            pthread_mutex_unlock(&g_bp_mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_bp_mutex);
    return -1;
}

static const fc_gdb_cpu_ops_t gdb_cpu_ops = {
    .read_regs = gdb_read_regs,
    .write_regs = gdb_write_regs,
    .read_mem = gdb_read_mem,
    .write_mem = gdb_write_mem,
    .set_breakpoint = gdb_set_bp,
    .clear_breakpoint = gdb_clear_bp,
};
#endif /* BLYT_GDB */

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

    case BLYT_ECALL_FRAME_DONE: {
        rv->PC += 4;
        /* Enforce FP determinism (ADR-0007): frm must be RNE (0) at every
         * frame boundary.  A non-zero frm means cart/library code called
         * fesetround() or modified frm and did not restore it, which would
         * cause FP results to diverge across emulator runs and native. */
        uint32_t frm = (rv->csr_fcsr >> 5) & 0x7u;
        if (frm != 0u) {
#ifndef NDEBUG
            fprintf(stderr,
                    "blyt: WARNING: cart set non-default FP rounding mode "
                    "(frm=%u); results may be non-deterministic\n",
                    (unsigned)frm);
#else
            fprintf(stderr, "blyt: cart set non-default FP rounding mode; "
                            "aborting for determinism\n");
            if (g_run_ctx)
                g_run_ctx->ecall_aborted = true;
            rv_halt(rv);
            return;
#endif
        }
        /* Reset frm to RNE and clear accumulated fflags for the next frame. */
        rv->csr_fcsr = 0;
        if (g_run_ctx)
            g_run_ctx->frame_done = true;
        /* Halt the emulator so rv_step() returns immediately.  Without this,
         * rv_step() continues running the next frame's instructions within the
         * same cycle budget (BLYT_CYCLE_PER_STEP), causing multiple game frames
         * to execute per outer-loop iteration while only one testcard draw
         * occurs.  blyt_session_run_frame() clears halt at the start of each
         * new frame. */
        rv_halt(rv);
        return;
    }

    case BLYT_ECALL_DAP_HOOK: {
        /* Probe (src=0, line<0) or line event from libblyt32lua master hook.
         * Always handled — even without BLYT_DAP — so probe returns 0 (inactive)
         * rather than trapping when the host was not built with DAP support. */
        if (!g_run_ctx || !g_run_ctx->dap_enabled) {
            rv_set_reg(rv, rv_reg_a0, 0);
            rv->PC += 4;
            return;
        }
#ifdef BLYT_DAP
        {
            uint32_t src_vaddr = rv_get_reg(rv, rv_reg_a0);
            uint32_t src_len = rv_get_reg(rv, rv_reg_a1);
            int32_t line = (int32_t)rv_get_reg(rv, rv_reg_a2);
            int32_t depth = (int32_t)rv_get_reg(rv, rv_reg_a3);

            /* Probe call from blyt_dap_active(): src=0 or line<0 */
            if (src_vaddr == 0 || line < 0) {
                rv_set_reg(rv, rv_reg_a0, 1);
                rv->PC += 4;
                return;
            }

            vm_attr_t *attr = PRIV(rv);
            memory_t *mem = attr->mem;
            char src_buf[4096];
            if (src_len >= sizeof(src_buf))
                src_len = (uint32_t)(sizeof(src_buf) - 1);
            uint32_t ri;
            for (ri = 0; ri < src_len; ri++) {
                if (!GUEST_RAM_CONTAINS(mem, src_vaddr + ri, 1))
                    break;
                uint8_t c;
                memory_read(mem, &c, src_vaddr + ri, 1);
                src_buf[ri] = (char)c;
            }
            src_buf[ri] = '\0';

            int cmd = fc_dap_check_hook_line(src_buf, line, depth);
            rv_set_reg(rv, rv_reg_a0, (uint32_t)cmd);
        }
#else
        rv_set_reg(rv, rv_reg_a0, 0);
#endif
        rv->PC += 4;
        return;
    }

#ifdef BLYT_DAP
    case BLYT_ECALL_DAP_SEND: {
        /* Guest sends a DAP JSON message; forward to connected TCP client. */
        if (!g_run_ctx || !g_run_ctx->dap_enabled) {
            rv->PC += 4;
            return;
        }
        uint32_t vaddr = rv_get_reg(rv, rv_reg_a0);
        uint32_t vlen = rv_get_reg(rv, rv_reg_a1);
        vm_attr_t *attr = PRIV(rv);
        memory_t *mem = attr->mem;
        if (vlen > 65535u)
            vlen = 65535u;
        char *json = malloc((size_t)vlen + 1);
        if (json) {
            uint32_t i;
            for (i = 0; i < vlen; i++) {
                if (!GUEST_RAM_CONTAINS(mem, vaddr + i, 1))
                    break;
                uint8_t c;
                memory_read(mem, &c, vaddr + i, 1);
                json[i] = (char)c;
            }
            json[i] = '\0';
            fc_dap_host_send(json, i);
            free(json);
        }
        rv->PC += 4;
        return;
    }

    case BLYT_ECALL_DAP_RECV: {
        /* Block until VS Code sends an inspection command; write JSON to guest. */
        if (!g_run_ctx || !g_run_ctx->dap_enabled) {
            rv_set_reg(rv, rv_reg_a0, 0);
            rv->PC += 4;
            return;
        }
        uint32_t buf_vaddr = rv_get_reg(rv, rv_reg_a0);
        uint32_t max_len = rv_get_reg(rv, rv_reg_a1);
        vm_attr_t *attr = PRIV(rv);
        memory_t *mem = attr->mem;
        if (max_len > 65535u)
            max_len = 65535u;
        char *buf = malloc((size_t)max_len + 1);
        int len = 0;
        if (buf) {
            len = fc_dap_host_recv(buf, (size_t)max_len);
            if (len > 0) {
                uint32_t wr = (uint32_t)len < max_len ? (uint32_t)len : max_len;
                memory_write(mem, buf_vaddr, (const uint8_t *)buf, wr);
            }
            free(buf);
        }
        rv_set_reg(rv, rv_reg_a0, (uint32_t)len);
        rv->PC += 4;
        return;
    }

    case 6: { /* BLYT_ECALL_DAP_EXCEPTION: guest reports a caught Lua exception. */
        if (!g_run_ctx || !g_run_ctx->dap_enabled) {
            rv_set_reg(rv, rv_reg_a0, 0);
            rv->PC += 4;
            return;
        }
        uint32_t msg_vaddr = rv_get_reg(rv, rv_reg_a0);
        uint32_t msg_len = rv_get_reg(rv, rv_reg_a1);
        int is_uncaught = (int)rv_get_reg(rv, rv_reg_a2);
        vm_attr_t *attr6 = PRIV(rv);
        memory_t *mem6 = attr6->mem;
        if (msg_len > 255u)
            msg_len = 255u;
        char msg_buf[256];
        uint32_t mi;
        for (mi = 0; mi < msg_len; mi++) {
            if (!GUEST_RAM_CONTAINS(mem6, msg_vaddr + mi, 1))
                break;
            uint8_t c6;
            memory_read(mem6, &c6, msg_vaddr + mi, 1);
            msg_buf[mi] = (char)c6;
        }
        msg_buf[mi] = '\0';
        int r6 = fc_dap_on_exception(msg_buf, is_uncaught);
        rv_set_reg(rv, rv_reg_a0, (uint32_t)r6);
        rv->PC += 4;
        return;
    }

    case 7: { /* BLYT_ECALL_DAP_GET_CONDITION: copy pending condition to guest. */
        if (!g_run_ctx || !g_run_ctx->dap_enabled) {
            rv_set_reg(rv, rv_reg_a0, 0);
            rv->PC += 4;
            return;
        }
        uint32_t buf7_vaddr = rv_get_reg(rv, rv_reg_a0);
        uint32_t buf7_len = rv_get_reg(rv, rv_reg_a1);
        if (buf7_len > 255u)
            buf7_len = 255u;
        char cond_buf[256];
        int clen = fc_dap_get_condition(cond_buf, (size_t)buf7_len);
        if (clen > 0) {
            vm_attr_t *attr7 = PRIV(rv);
            memory_t *mem7 = attr7->mem;
            memory_write(mem7, buf7_vaddr, (const uint8_t *)cond_buf, (uint32_t)clen);
        }
        rv_set_reg(rv, rv_reg_a0, (uint32_t)clen);
        rv->PC += 4;
        return;
    }

    case 8: { /* BLYT_ECALL_DAP_CONDITION_RESULT: guest reports condition eval. */
        if (!g_run_ctx || !g_run_ctx->dap_enabled) {
            rv_set_reg(rv, rv_reg_a0, 0);
            rv->PC += 4;
            return;
        }
        int result8 = (int)rv_get_reg(rv, rv_reg_a0);
        int r8 = fc_dap_on_condition_result(result8);
        rv_set_reg(rv, rv_reg_a0, (uint32_t)r8);
        rv->PC += 4;
        return;
    }
#endif /* BLYT_DAP */

    case 93: /* SYS_exit (Linux NR 93) — blyt_exit on emulated path */
    case 94: /* SYS_exit_group (Linux NR 94) — blyt_exit on emulated path */
        rv_halt(rv);
        if (rv_get_reg(rv, rv_reg_a0) != 0 && g_run_ctx)
            g_run_ctx->ecall_aborted = true;
        return;

    default:
        rv_halt(rv);
        if (g_run_ctx)
            g_run_ctx->ecall_trapped = true;
        return;
    }
}

#ifdef BLYT_GDB
/* Called by rv32emu when an ebreak instruction executes.  If the PC matches a
 * registered GDB software breakpoint, flag that the breakpoint fired so the
 * outer run loop can notify the client after rv_step() returns.  Leave PC at
 * the ebreak address so the outer loop can restore the original instruction and
 * execute it as a step-over (same as the check_break path).  Otherwise advance
 * PC past the ebreak (same as the rv32emu default). */
static void blyt_ebreak_handler(riscv_t *rv) {
    if (g_run_ctx && g_run_ctx->gdb_enabled && fc_gdb_stub_check_break(rv->PC)) {
        g_run_ctx->gdb_ebreak_pending = true;
        /* Keep PC at the ebreak address so the outer loop can restore the
         * original instruction and perform a proper step-over. */
        rv->halt = true; /* force rv_step() to exit so outer loop runs promptly */
        return;
    }
    /* Not a GDB breakpoint — skip the ebreak (same as rv32emu default). */
    rv->PC += rv->compressed ? 2 : 4;
}
#endif /* BLYT_GDB */

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
            if (type != R_RISCV_JUMP_SLOT && type != R_RISCV_GLOB_DAT && type != R_RISCV_32)
                continue;

            uint32_t sym_idx = ELF32_R_SYM(r->r_info);
            if (sym_idx * dynsym->sh_entsize + sizeof(Elf32_Sym) > dynsym->sh_size)
                continue;

            const Elf32_Sym *sym =
                (const Elf32_Sym *)(map + dynsym->sh_offset + sym_idx * dynsym->sh_entsize);
            if (sym->st_name >= sym_strsz)
                continue;

            const char *sym_name = sym_strtab + sym->st_name;
            /* R_RISCV_32 for locally-defined symbols is handled by apply_lib_rela;
             * only cross-module UNDEF references need resolution here. */
            if (type == R_RISCV_32 && sym->st_shndx != SHN_UNDEF)
                continue;
            uint32_t resolved = symtab_lookup(syms, sym_name);
            if (resolved == 0) {
                if (ELF32_ST_BIND(sym->st_info) == STB_WEAK) {
                    uint8_t zero[4] = {0};
                    memory_write(mem, elf_bias + r->r_offset, zero, 4);
                } else {
                    fprintf(stderr, "[dynlink] UNRESOLVED STRONG: %s\n", sym_name);
                }
                continue;
            }

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

static blyt_cart_run_err_t dynlink(blyt_session_t *s, const blyt_cart_t *cart) {
    riscv_t *rv = s->rv;
    const char *lib_dir = getenv("BLYT_LIB_DIR");

    vm_attr_t *attr = PRIV(rv);
    memory_t *mem = attr->mem;

    blyt_lib_t libs[MAX_RUNTIME_LIBS];
    int nlibs = 0;

    /* blyt_symtab_t holds 2048 × 132-byte entries (~264 KiB).  Heap-allocate
     * so it does not overflow small stacks (e.g. Emscripten's 64 KiB default).
     * Lua carts add ~450 exported symbols from libblytcommonlua.so; 512 was
     * too small once the Lua VM library is included in the symbol table. */
    blyt_symtab_t *all_syms = calloc(1, sizeof(*all_syms));
    if (!all_syms)
        return BLYT_RUN_ERR_EMU;

    /* Seed cart symbols first so cart's strong definitions win over library stubs. */
    {
        blyt_lib_t cart_syms = {
            .map = (const uint8_t *)cart->map,
            .size = cart->map_size,
            .bias = 0,
            .mmapped = false,
        };
        build_symtab(&cart_syms, all_syms);
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
        build_symtab(&lib, all_syms);
        libs[nlibs++] = lib;

#ifdef BLYT_GDB
        /* Capture library layout for GDB qXfer:libraries-svr4:read. */
        if (s->gdb_nlibs < MAX_GDB_LIBS) {
            int idx = s->gdb_nlibs++;
            blyt_gdb_lib_t *gl = &s->gdb_libs[idx];
            strncpy(gl->path, lib_name, sizeof(gl->path) - 1);
            gl->path[sizeof(gl->path) - 1] = '\0';
            gl->l_addr = bias;
            /* l_ld = bias + PT_DYNAMIC.p_vaddr */
            gl->l_ld = 0;
            for (uint16_t pi = 0; pi < leh->e_phnum; pi++) {
                size_t poff = (size_t)leh->e_phoff + (size_t)pi * leh->e_phentsize;
                if (poff + sizeof(Elf32_Phdr) > lsz)
                    break;
                const Elf32_Phdr *ph = (const Elf32_Phdr *)(lmap + poff);
                if (ph->p_type == PT_DYNAMIC) {
                    gl->l_ld = bias + ph->p_vaddr;
                    break;
                }
            }
            /* Mirror into the stable FFI array (path pointer stays valid for
             * the session's lifetime, avoiding a dangling pointer in layout). */
            s->gdb_libs_ffi[idx].path = gl->path;
            s->gdb_libs_ffi[idx].l_addr = gl->l_addr;
            s->gdb_libs_ffi[idx].l_ld = gl->l_ld;
        }
#endif /* BLYT_GDB */

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
        /* Apply cart's own R_RISCV_RELATIVE relocations (e.g. static arrays of
         * function pointers in src/lib/ C libraries, like luaL_Reg tables).
         * Libraries get this in the BFS loop above; the cart needs it too. */
        blyt_lib_t cart_lib = {
            .map = (const uint8_t *)cart->map,
            .size = cart->map_size,
            .bias = 0,
            .mmapped = false,
        };
        apply_lib_rela(&cart_lib, mem);

        for (int i = 0; i < nlibs; i++) {
            resolve_elf_plt(libs[i].map, libs[i].size, mem, libs[i].bias, all_syms);
        }
        resolve_elf_plt(cart->map, cart->map_size, mem, 0, all_syms);

        /* Initialise the libblytc.so arena (ADR-0120). */
        uint32_t sym_base = symtab_lookup(all_syms, "blytc_arena_base");
        uint32_t sym_size = symtab_lookup(all_syms, "blytc_arena_size");
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

    free(all_syms);
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

    blyt_cart_run_err_t load_err = dynlink(s, cart);
    if (load_err != BLYT_RUN_OK) {
        rv_delete(s->rv);
        free(s);
        return NULL;
    }

    s->rv->io.on_ecall = blyt_ecall_handler;
#ifdef BLYT_GDB
    s->rv->io.on_ebreak = blyt_ebreak_handler;
#endif
    rv_set_reg(s->rv, rv_reg_ra, BLYT_TRAMPOLINE_EXIT_ADDR);

    blyt_testcard_init_palette(s->palette);

#ifdef BLYT_GDB
    if (cart->path) {
        strncpy(s->gdb_exec_path, cart->path, sizeof(s->gdb_exec_path) - 1);
        s->gdb_exec_path[sizeof(s->gdb_exec_path) - 1] = '\0';
    }
    /* Register cpu_ops with the stub so it can read/write registers/memory
     * and set/clear software breakpoints via ebreak patches. */
    fc_gdb_stub_set_cpu_ops(&gdb_cpu_ops);
#endif

    return s;
}

blyt_cart_run_err_t blyt_session_run_frame(blyt_session_t *session) {
    g_run_ctx = &session->ctx;
    session->ctx.frame_done = false;
    /* Clear any halt set by the previous BLYT_ECALL_FRAME_DONE so the while
     * loop below can execute the new frame.  A halt from blyt_exit/abort would
     * have caused the caller to stop resubmitting frames entirely. */
    session->rv->halt = false;

#ifdef BLYT_DAP
    if (session->ctx.dap_enabled && fc_dap_is_restart_pending()) {
        g_run_ctx = NULL;
        return BLYT_RUN_RESTART;
    }
#endif

#ifdef BLYT_GDB
    if (session->ctx.gdb_enabled && session->ctx.debug_state == BLYT_DEBUG_RUNNING) {
#ifdef __EMSCRIPTEN__
        /* On WASM, drain the GDB WebSocket queue once per frame. */
        fc_gdb_stub_poll();
#endif
        /* If the stub was halted externally (WASM poll or TCP \x03 interrupt),
         * T02 has already been sent.  Stop executing until vCont arrives. */
        if (fc_gdb_stub_is_halted()) {
#ifdef __EMSCRIPTEN__
            session->ctx.debug_state = BLYT_DEBUG_PAUSED_GDB;
            g_run_ctx = NULL;
            return BLYT_RUN_GDB_PAUSED;
#else
            /* Native (TCP): block in the transport thread's poll loop. */
            fc_gdb_stub_block_until_resume();
            int ext_action = fc_gdb_stub_pending_action();
            if (ext_action == 2) {
                rv_halt(session->rv);
                g_run_ctx = NULL;
                return BLYT_RUN_OK;
            }
            session->ctx.gdb_single_step = (ext_action == 1);
#endif
        }
    }
#endif

    while (!rv_has_halted(session->rv) && !session->ctx.ecall_trapped &&
           !session->ctx.ecall_aborted) {
#ifdef BLYT_GDB
        uint32_t gdb_bp_step_addr = 0;
        if (session->ctx.gdb_enabled) {
            /* If already paused waiting for vCont, poll for a resume packet. */
            if (session->ctx.debug_state == BLYT_DEBUG_PAUSED_GDB) {
#ifdef __EMSCRIPTEN__
                /* WASM: non-blocking poll — process one queued packet if any.
                 * T05 is NOT sent here proactively; LLDB will query the stop
                 * reason with '?' and the packet handler responds with T05 then.
                 * Sending T05 before the qSupported ACK corrupts the protocol. */
                fc_gdb_stub_poll();
                int _reentry_action = fc_gdb_stub_pending_action();
                if (_reentry_action < 0) {
                    g_run_ctx = NULL;
                    return BLYT_RUN_GDB_PAUSED; /* still paused, try again next tick */
                }
                session->ctx.debug_state = BLYT_DEBUG_RUNNING;
                if (_reentry_action == 2) {
                    rv_halt(session->rv);
                    break;
                }
                session->ctx.gdb_single_step = (_reentry_action == 1);
                /* Step-over: if we were paused at a software breakpoint, temporarily
                 * restore the original instruction so rv_step executes it (not the
                 * ebreak).  gdb_bp_step_addr != 0 will suppress check_break below
                 * (preventing an immediate re-stop) and trigger repatch after rv_step. */
                if (session->ctx.gdb_bp_resume_addr != 0) {
                    fc_gdb_stub_restore_bp_temp(session->ctx.gdb_bp_resume_addr);
                    gdb_bp_step_addr = session->ctx.gdb_bp_resume_addr;
                    session->ctx.gdb_bp_resume_addr = 0;
                }
                /* fall through to execute next instruction */
#else
                /* Native: this path should not be reached — TCP path blocks. */
                g_run_ctx = NULL;
                return BLYT_RUN_GDB_PAUSED;
#endif
            }
            /* Check for software breakpoint at current PC.
             * Skip if gdb_bp_step_addr is set — that means we just restored the
             * original instruction for a step-over and must not re-stop here.
             * On the native path, gdb_bp_resume_addr carries the same signal
             * across loop iterations (for mid-block ebreak step-overs). */
            uint32_t gdb_pc = rv_get_pc(session->rv);
#ifndef __EMSCRIPTEN__
            if (session->ctx.gdb_bp_resume_addr != 0) {
                /* Mid-block ebreak step-over: original instruction was already
                 * restored in the ebreak_pending handler.  Set gdb_bp_step_addr
                 * so repatch fires after rv_step, and suppress check_break. */
                gdb_bp_step_addr = session->ctx.gdb_bp_resume_addr;
                session->ctx.gdb_bp_resume_addr = 0;
            } else
#endif
                if (gdb_bp_step_addr == 0 && fc_gdb_stub_check_break(gdb_pc)) {
                session->ctx.debug_state = BLYT_DEBUG_PAUSED_GDB;
#ifdef __EMSCRIPTEN__
                session->ctx.gdb_bp_resume_addr = gdb_pc;
                if (fc_gdb_transport_wasm_is_connected())
                    fc_gdb_stub_notify_stopped();
                g_run_ctx = NULL;
                return BLYT_RUN_GDB_PAUSED;
#else
                fc_gdb_stub_notify_stopped();
                fc_gdb_stub_block_until_resume();
                int gdb_action = fc_gdb_stub_pending_action();
                session->ctx.debug_state = BLYT_DEBUG_RUNNING;
                if (gdb_action == 2) {
                    rv_halt(session->rv);
                    break;
                }
                session->ctx.gdb_single_step = (gdb_action == 1);
                /* Restore original instruction so rv_step executes it, not the
                 * ebreak.  Re-patch after rv_step via gdb_bp_step_addr. */
                fc_gdb_stub_restore_bp_temp(gdb_pc);
                gdb_bp_step_addr = gdb_pc;
#endif
            }
        }
#endif /* BLYT_GDB */

#if defined(BLYT_GDB) && defined(__EMSCRIPTEN__)
        /* On WASM, always step one instruction at a time when GDB is enabled
         * so check_break fires at every PC (rv_step is block-based). */
        if (session->ctx.gdb_enabled)
            rv_step_debug(session->rv);
        else
            rv_step(session->rv);
#elif defined(BLYT_GDB)
        /* When stepping over a software breakpoint, use rv_step_debug to
         * bypass rv32emu's block-IR cache: restore_bp_temp wrote the original
         * instruction to guest memory but the cached block still holds the
         * decoded EBREAK.  rv_step_debug reads via mem_ifetch (guest RAM
         * directly) so the restored instruction always executes. */
        if (session->ctx.gdb_enabled && gdb_bp_step_addr != 0)
            rv_step_debug(session->rv);
        else
            rv_step(session->rv);
#else
        rv_step(session->rv);
#endif

#ifdef BLYT_GDB
        /* Re-patch a SW BP that was temporarily removed to allow stepping over it. */
        if (session->ctx.gdb_enabled && gdb_bp_step_addr != 0)
            fc_gdb_stub_repatch_bp(gdb_bp_step_addr);
        /* Breakpoint fired inside rv_step() via on_ebreak: rv->halt was set to
         * force rv_step to return.  PC is at the ebreak address (the callback
         * does not advance it) so we can perform the same step-over as the
         * check_break path: restore the original instruction, execute it, then
         * re-patch the breakpoint. */
        if (session->ctx.gdb_enabled && session->ctx.gdb_ebreak_pending) {
            session->ctx.gdb_ebreak_pending = false;
            session->rv->halt = false; /* restore — cart has not actually halted */
            session->ctx.debug_state = BLYT_DEBUG_PAUSED_GDB;
            uint32_t gdb_ebreak_pc = rv_get_pc(session->rv);
#ifdef __EMSCRIPTEN__
            session->ctx.gdb_bp_resume_addr = gdb_ebreak_pc;
            if (fc_gdb_transport_wasm_is_connected())
                fc_gdb_stub_notify_stopped();
            if (session->ctx.frame_done) {
                if (!session->cart_has_drawn)
                    blyt_testcard_draw(session->frame_count++, session->pixels);
            }
            g_run_ctx = NULL;
            return BLYT_RUN_GDB_PAUSED;
#else
            fc_gdb_stub_notify_stopped();
            fc_gdb_stub_block_until_resume();
            int gdb_action = fc_gdb_stub_pending_action();
            session->ctx.debug_state = BLYT_DEBUG_RUNNING;
            if (gdb_action == 2) {
                rv_halt(session->rv);
                break;
            }
            session->ctx.gdb_single_step = (gdb_action == 1);
            /* Step-over: restore original instruction so the next rv_step call
             * executes it instead of the ebreak.  Use gdb_bp_resume_addr (a
             * session-level field) rather than the local gdb_bp_step_addr
             * because the local is reset at the top of each loop iteration;
             * the gdb_bp_resume_addr → gdb_bp_step_addr transfer in the
             * check_break section will then trigger repatch after rv_step. */
            fc_gdb_stub_restore_bp_temp(gdb_ebreak_pc);
            session->ctx.gdb_bp_resume_addr = gdb_ebreak_pc;
#endif
        }
        /* Post-instruction single-step pause. */
        if (session->ctx.gdb_enabled && session->ctx.gdb_single_step) {
            session->ctx.gdb_single_step = false;
            session->ctx.debug_state = BLYT_DEBUG_PAUSED_GDB;
            fc_gdb_stub_notify_stopped();
#ifdef __EMSCRIPTEN__
            if (session->ctx.frame_done) {
                if (!session->cart_has_drawn)
                    blyt_testcard_draw(session->frame_count++, session->pixels);
            }
            g_run_ctx = NULL;
            return BLYT_RUN_GDB_PAUSED;
#else
            fc_gdb_stub_block_until_resume();
            int gdb_action = fc_gdb_stub_pending_action();
            session->ctx.debug_state = BLYT_DEBUG_RUNNING;
            if (gdb_action == 2) {
                rv_halt(session->rv);
                break;
            }
            session->ctx.gdb_single_step = (gdb_action == 1);
#endif
        }
#endif /* BLYT_GDB */

        if (session->ctx.frame_done) {
            if (!session->cart_has_drawn)
                blyt_testcard_draw(session->frame_count++, session->pixels);
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
#ifdef BLYT_GDB
    blyt_session_gdb_shutdown(session);
#endif
    rv_delete(session->rv);
    free(session);
}

const uint8_t *blyt_session_get_pixels(const blyt_session_t *session) {
    return session->pixels;
}

const uint32_t *blyt_session_get_palette(const blyt_session_t *session) {
    return session->palette;
}

void blyt_session_expand_frame(const blyt_session_t *session, uint32_t *xrgb_out) {
    const uint8_t *px = session->pixels;
    const uint32_t *pal = session->palette;
    for (int i = 0; i < BLYT_FRAME_W * BLYT_FRAME_H; i++)
        xrgb_out[i] = pal[px[i]];
}

/* -------------------------------------------------------------------------
 * DAP session helpers
 * ------------------------------------------------------------------------- */

int blyt_session_dap_listen(blyt_session_t *s, int *port_out) {
#ifdef BLYT_DAP
    int port = fc_consolelua_dap_listen(0);
    if (port < 0)
        return -1;
    s->ctx.dap_enabled = true;
    if (port_out)
        *port_out = port;
    return port;
#else
    (void)s;
    (void)port_out;
    return -1;
#endif
}

void blyt_session_dap_shutdown(blyt_session_t *s) {
#ifdef BLYT_DAP
    fc_consolelua_dap_shutdown();
    s->ctx.dap_enabled = false;
#else
    (void)s;
#endif
}

void blyt_session_dap_reattach(blyt_session_t *s) {
#ifdef BLYT_DAP
    if (s)
        s->ctx.dap_enabled = true;
#else
    (void)s;
#endif
}

int blyt_session_dap_wait_ready(blyt_session_t *s) {
#ifdef BLYT_DAP
    if (!s || !s->ctx.dap_enabled)
        return 0;
    return fc_dap_wait_configuration_done();
#else
    (void)s;
    return 0;
#endif
}

/* -------------------------------------------------------------------------
 * GDB session helpers
 * ------------------------------------------------------------------------- */

int blyt_session_gdb_listen(blyt_session_t *s, int *port_out) {
#ifdef BLYT_GDB
    if (!s)
        return -1;

    /* Register layout with the stub.  Use the stable gdb_libs_ffi array whose
     * path pointers remain valid for the session's lifetime. */
    fc_gdb_layout_t layout = {
        .exec_path = s->gdb_exec_path,
        .libraries = s->gdb_libs_ffi,
        .n_libraries = s->gdb_nlibs,
    };
    fc_gdb_stub_set_layout(&layout);

    g_gdb_session = s;
    s->ctx.gdb_enabled = true;

    int actual_port = port_out ? *port_out : 0;
#ifdef __EMSCRIPTEN__
    /* Start in paused state so the cart waits for vCont;c before running. */
    s->ctx.debug_state = BLYT_DEBUG_PAUSED_GDB;
    fc_gdb_transport_wasm_open(actual_port);
    if (port_out)
        *port_out = actual_port;
    return actual_port;
#else
    if (fc_gdb_transport_tcp_listen(actual_port, &actual_port) != 0) {
        s->ctx.gdb_enabled = false;
        g_gdb_session = NULL;
        return -1;
    }
    if (port_out)
        *port_out = actual_port;
    return actual_port;
#endif
#else
    (void)s;
    (void)port_out;
    return -1;
#endif
}

void blyt_session_gdb_shutdown(blyt_session_t *s) {
#ifdef BLYT_GDB
    if (!s || !s->ctx.gdb_enabled)
        return;
#ifdef __EMSCRIPTEN__
    fc_gdb_transport_wasm_shutdown();
#else
    fc_gdb_transport_tcp_shutdown();
#endif
    s->ctx.gdb_enabled = false;
    if (g_gdb_session == s)
        g_gdb_session = NULL;
#else
    (void)s;
#endif
}

int blyt_session_gdb_wait_attached(blyt_session_t *s) {
#ifdef BLYT_GDB
    if (!s || !s->ctx.gdb_enabled)
        return 0;
#ifdef __EMSCRIPTEN__
    /* On WASM, connection is async; caller cannot block. */
    return fc_gdb_transport_wasm_is_connected();
#else
    /* Block until a GDB client connects (has_client set by transport thread). */
    while (!fc_gdb_stub_has_client()) {
        usleep(10000); /* 10 ms */
    }
    return 1;
#endif
#else
    (void)s;
    return 0;
#endif
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
