/*
 * Spike A — interpreter-throughput measurement runner.
 *
 * Loads a bare RV32IMAFDC ELF (CoreMark / Embench, cross-compiled with the blyt
 * clang toolchain + static musl) and runs it to exit through the SAME rv32emu
 * interpreter core the blyt runtime ships (interpreter-only: JIT/T2C/SYSTEM off,
 * Berkeley SoftFloat for F/D). It reports the effective guest MIPS —
 *
 *     effective MIPS = retired guest instructions / host wall-clock seconds / 1e6
 *
 * — where the retired-instruction count is rv32emu's per-instruction cycle
 * counter (`rv->csr_cycle`, riscv_private.h). Run on a Pi Zero 2 W this yields
 * the ADR-0082 emulator MIPS cap; run on the dev Mac it is the comparison
 * baseline. This is measurement infrastructure only: it links the emulator core
 * as a library and changes nothing under runtime/ — no cart-visible or
 * determinism surface is touched.
 *
 * Unlike the blyt cart path, the benchmark ELFs are ordinary main()-style static
 * musl executables, so this runner installs its own on_ecall handler that
 * services the small Linux/RISC-V syscall surface musl needs (write, brk, mmap,
 * clock_gettime, set_tid_address, ...) and builds a complete SysV initial stack
 * (argc/argv/envp/auxv). rv32emu's vendored source is used unmodified.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* rv32emu headers — common.h first (defines RV32_HAS, FORCE_INLINE, ...). The
 * emulator source dir is on the include path, so use bare filenames. */
#include "common.h"
#include "io.h"
#include "log.h"
#include "riscv.h"
#include "riscv_private.h"

/* ------------------------------------------------------------------------- */
/* Linux/RISC-V generic (32-bit, time64) syscall numbers that static musl and a
 * typical CoreMark/Embench port emit. Anything not listed hits the default
 * branch, which logs the number and returns -ENOSYS. */
#define SYS_getcwd 17
#define SYS_ioctl 29
#define SYS_faccessat 48
#define SYS_close 57
#define SYS_lseek 62
#define SYS_read 63
#define SYS_write 64
#define SYS_writev 66
#define SYS_readv 65
#define SYS_fstat 80
#define SYS_exit 93
#define SYS_exit_group 94
#define SYS_set_tid_address 96
#define SYS_rt_sigaction 134
#define SYS_rt_sigprocmask 135
#define SYS_gettimeofday 169
#define SYS_brk 214
#define SYS_munmap 215
#define SYS_mmap 222
#define SYS_madvise 233
#define SYS_getrandom 278
/* time64 variants some musl builds use on rv32 */
#define SYS_clock_gettime64 403
#define SYS_gettimeofday64 169

/* Harness-private "frame marker" syscall (not a Linux number). A guest calls it
 * once per simulated frame; the runner records the retired-instruction delta
 * since the previous marker, yielding a per-frame instruction distribution that
 * captures GC/allocation spikes. Chosen well outside the Linux syscall range. */
#define SYS_BLYT_FRAME 0xB1700001u

/* Guest address-space budget. CoreMark/Embench have tiny footprints; give them
 * a comfortable 256 MiB with well-separated heap / mmap / stack windows so the
 * three allocators never collide. */
#define GUEST_MEM_SIZE (256u * 1024u * 1024u)
#define GUEST_STACK_SIZE (8u * 1024u * 1024u)
#define GUEST_MMAP_BASE 0x08000000u /* 128 MiB — bump-allocated upward */
#define GUEST_PAGE 4096u

/* ------------------------------------------------------------------------- */
/* Per-run mutable state threaded through the ecall handler. rv32emu's on_ecall
 * takes only (riscv_t *), so we keep this in a file-scope singleton. */
typedef struct {
    uint32_t brk_cur; /* current program break (grows up from _end)   */
    uint32_t mmap_cur; /* next anonymous mmap address (grows up)        */
    int unhandled; /* count of -ENOSYS fallbacks (diagnostic)       */
    /* Per-frame instruction accounting (SYS_BLYT_FRAME). */
    uint64_t *frame_insns; /* retired instructions per marked frame      */
    size_t n_frames;
    size_t cap_frames;
    uint64_t last_frame_cycle;
    int frames_started;
} runner_state_t;

static runner_state_t g_state;

static void record_frame(uint64_t cycle_now)
{
    if (!g_state.frames_started) {
        g_state.frames_started = 1;
        g_state.last_frame_cycle = cycle_now;
        return; /* first marker: baseline only */
    }
    if (g_state.n_frames == g_state.cap_frames) {
        size_t nc = g_state.cap_frames ? g_state.cap_frames * 2 : 1024;
        uint64_t *p = realloc(g_state.frame_insns, nc * sizeof(uint64_t));
        if (!p)
            return; /* drop silently if out of memory */
        g_state.frame_insns = p;
        g_state.cap_frames = nc;
    }
    g_state.frame_insns[g_state.n_frames++] = cycle_now - g_state.last_frame_cycle;
    g_state.last_frame_cycle = cycle_now;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *) a, y = *(const uint64_t *) b;
    return (x > y) - (x < y);
}

/* AUXV entry types we populate. */
#define AT_NULL 0
#define AT_PAGESZ 6
#define AT_RANDOM 25

/* ------------------------------------------------------------------------- */
/* Guest-memory helpers (thin wrappers over rv32emu's memory_t). */

static inline uint32_t guest_arg(riscv_t *rv, int i) {
    static const uint8_t regs[] = {rv_reg_a0, rv_reg_a1, rv_reg_a2,
                                   rv_reg_a3, rv_reg_a4, rv_reg_a5};
    return rv_get_reg(rv, regs[i]);
}

static inline void guest_ret(riscv_t *rv, uint32_t val) {
    rv_set_reg(rv, rv_reg_a0, val);
}

/* Copy `len` bytes out of guest memory into host `dst`. */
static void read_guest(riscv_t *rv, void *dst, uint32_t addr, uint32_t len) {
    memory_read(PRIV(rv)->mem, (uint8_t *)dst, addr, len);
}

/* Copy `len` bytes from host `src` into guest memory. */
static void write_guest(riscv_t *rv, uint32_t addr, const void *src, uint32_t len) {
    memory_write(PRIV(rv)->mem, addr, (const uint8_t *)src, len);
}

/* ------------------------------------------------------------------------- */
/* Individual syscall services. Each returns the value to place in a0. */

static uint32_t do_write(riscv_t *rv) {
    uint32_t fd = guest_arg(rv, 0), buf = guest_arg(rv, 1), n = guest_arg(rv, 2);
    if (n == 0)
        return 0;
    /* Only stdout/stderr are meaningful; map anything else onto the host fd. */
    int hostfd = (fd == 1) ? STDOUT_FILENO : (fd == 2) ? STDERR_FILENO : (int)fd;
    uint8_t *tmp = malloc(n);
    if (!tmp)
        return (uint32_t)-ENOMEM;
    read_guest(rv, tmp, buf, n);
    ssize_t w = write(hostfd, tmp, n);
    free(tmp);
    return (uint32_t)(w < 0 ? -errno : w);
}

static uint32_t do_writev(riscv_t *rv) {
    uint32_t iov = guest_arg(rv, 1), cnt = guest_arg(rv, 2);
    uint32_t fd = guest_arg(rv, 0);
    int hostfd = (fd == 1) ? STDOUT_FILENO : (fd == 2) ? STDERR_FILENO : (int)fd;
    uint32_t total = 0;
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t base, len;
        read_guest(rv, &base, iov + i * 8, 4);
        read_guest(rv, &len, iov + i * 8 + 4, 4);
        if (len == 0)
            continue;
        uint8_t *tmp = malloc(len);
        if (!tmp)
            return (uint32_t)-ENOMEM;
        read_guest(rv, tmp, base, len);
        ssize_t w = write(hostfd, tmp, len);
        free(tmp);
        if (w < 0)
            return (uint32_t)-errno;
        total += (uint32_t)w;
    }
    return total;
}

static uint32_t do_brk(riscv_t *rv) {
    uint32_t req = guest_arg(rv, 0);
    if (req == 0)
        return g_state.brk_cur;
    /* Grow only; keep the break below the mmap window. */
    if (req >= PRIV(rv)->break_addr && req < GUEST_MMAP_BASE)
        g_state.brk_cur = req;
    return g_state.brk_cur;
}

static uint32_t do_mmap(riscv_t *rv) {
    uint32_t len = guest_arg(rv, 1);
    /* Anonymous only. File-backed mappings are not needed by these benchmarks. */
    uint32_t aligned = (len + GUEST_PAGE - 1) & ~(GUEST_PAGE - 1);
    uint32_t addr = g_state.mmap_cur;
    if ((uint64_t)addr + aligned >= GUEST_MEM_SIZE - GUEST_STACK_SIZE)
        return (uint32_t)-ENOMEM;
    g_state.mmap_cur += aligned;
    /* Guest RAM is not guaranteed zero (macOS malloc path), and MAP_ANONYMOUS
     * promises zeroed pages — zero the region explicitly. */
    memory_fill(PRIV(rv)->mem, addr, aligned, 0);
    return addr;
}

/* Monotonic host time relative to the runner's first clock query. Returning
 * time-since-boot would overflow a benchmark that casts it into a 32-bit tick
 * (e.g. CoreMark's ee_u32 microseconds), so we rebase to ~0. The authoritative
 * MIPS number comes from csr_cycle, not from this. */
static uint64_t g_clock_base_ns;

static uint64_t rebased_now_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t ns = (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
    if (!g_clock_base_ns)
        g_clock_base_ns = ns;
    return ns - g_clock_base_ns;
}

static uint32_t do_clock_gettime(riscv_t *rv) {
    /* clk_id in a0 ignored — always monotonic. */
    uint32_t ts_addr = guest_arg(rv, 1);
    uint64_t ns = rebased_now_ns();
    /* Kernel __kernel_timespec: s64 tv_sec, s64 tv_nsec (16 bytes). */
    int64_t sec = (int64_t)(ns / 1000000000ull);
    int64_t nsec = (int64_t)(ns % 1000000000ull);
    write_guest(rv, ts_addr, &sec, 8);
    write_guest(rv, ts_addr + 8, &nsec, 8);
    return 0;
}

static uint32_t do_gettimeofday(riscv_t *rv) {
    uint32_t tv_addr = guest_arg(rv, 0);
    uint64_t ns = rebased_now_ns();
    int64_t sec = (int64_t)(ns / 1000000000ull);
    int64_t usec = (int64_t)((ns % 1000000000ull) / 1000);
    write_guest(rv, tv_addr, &sec, 8);
    write_guest(rv, tv_addr + 8, &usec, 8);
    return 0;
}

static uint32_t do_getrandom(riscv_t *rv) {
    uint32_t buf = guest_arg(rv, 0), n = guest_arg(rv, 1);
    /* Determinism is irrelevant to a throughput measurement; feed zeros. */
    memory_fill(PRIV(rv)->mem, buf, n, 0);
    return n;
}

/* ------------------------------------------------------------------------- */
/* ecall dispatch. rv32emu leaves PC at the ecall on entry; we advance it by 4
 * on every serviced call (exit halts instead). */

static void runner_ecall(riscv_t *rv) {
    uint32_t num = rv_get_reg(rv, rv_reg_a7);
    bool halted = false;
    uint32_t ret = 0;

    switch (num) {
    case SYS_write:
        ret = do_write(rv);
        break;
    case SYS_writev:
        ret = do_writev(rv);
        break;
    case SYS_read:
    case SYS_readv:
        ret = 0; /* benchmarks read no input */
        break;
    case SYS_brk:
        ret = do_brk(rv);
        break;
    case SYS_mmap:
        ret = do_mmap(rv);
        break;
    case SYS_munmap:
    case SYS_madvise:
        ret = 0;
        break;
    case SYS_clock_gettime64:
        ret = do_clock_gettime(rv);
        break;
    case SYS_gettimeofday:
        ret = do_gettimeofday(rv);
        break;
    case SYS_getrandom:
        ret = do_getrandom(rv);
        break;
    case SYS_set_tid_address:
        ret = 1; /* fake tid */
        break;
    case SYS_rt_sigprocmask:
    case SYS_rt_sigaction:
        ret = 0;
        break;
    case SYS_ioctl:
        ret = (uint32_t)-ENOTTY; /* isatty() -> false -> full buffering */
        break;
    case SYS_fstat:
        /* zeroed stat: st_mode==0 -> treated as a regular file (buffered). */
        memory_fill(PRIV(rv)->mem, guest_arg(rv, 1), 128, 0);
        ret = 0;
        break;
    case SYS_close:
    case SYS_faccessat:
        ret = 0;
        break;
    case SYS_BLYT_FRAME:
        record_frame(rv->csr_cycle);
        ret = 0;
        break;
    case SYS_exit:
    case SYS_exit_group:
        PRIV(rv)->exit_code = (int)guest_arg(rv, 0);
        rv_halt(rv);
        halted = true;
        break;
    default:
        fprintf(stderr, "[spike-a] unhandled syscall %u (a0=%#x) -> ENOSYS\n", num,
                guest_arg(rv, 0));
        g_state.unhandled++;
        ret = (uint32_t)-ENOSYS;
        break;
    }

    if (!halted) {
        guest_ret(rv, ret);
        rv->PC += 4;
    }
}

/* ------------------------------------------------------------------------- */
/* Build a complete SysV initial stack (argc/argv/envp/auxv) so static musl's
 * _start finds argc, an empty envp, and an auxv carrying AT_PAGESZ + AT_RANDOM.
 * We do not rely on rv32emu's partial stack (argc/argv only, no auxv). */

/* Write one 32-bit little-endian word into guest memory. */
static inline void stw(memory_t *mem, uint32_t addr, uint32_t val) {
    memory_write(mem, addr, (const uint8_t *)&val, 4);
}

static void setup_stack(riscv_t *rv, int argc, char **argv) {
    memory_t *mem = PRIV(rv)->mem;

    /* Place strings + the 16 AT_RANDOM bytes just below the top of RAM. */
    uint32_t p = GUEST_MEM_SIZE - 16;
    uint32_t rand_addr = p;
    uint8_t zero16[16] = {0};
    memory_write(mem, rand_addr, zero16, 16);

    uint32_t argv_addrs[8];
    for (int i = argc - 1; i >= 0; i--) {
        uint32_t len = (uint32_t)strlen(argv[i]) + 1;
        p -= len;
        memory_write(mem, p, (const uint8_t *)argv[i], len);
        argv_addrs[i] = p;
    }

    /* Vector (ascending words): argc, argv[0..argc-1], NULL, envp NULL,
     * auxv{ AT_PAGESZ, AT_RANDOM, AT_NULL } (3 pairs). */
    uint32_t nwords = 1u + (uint32_t)argc + 1u + 1u + 2u * 3u;
    uint32_t sp = (p - nwords * 4u) & ~15u;

    uint32_t w = sp;
    stw(mem, w, (uint32_t)argc), w += 4;
    for (int i = 0; i < argc; i++)
        stw(mem, w, argv_addrs[i]), w += 4;
    stw(mem, w, 0), w += 4; /* argv NULL */
    stw(mem, w, 0), w += 4; /* envp NULL */
    stw(mem, w, AT_PAGESZ), w += 4;
    stw(mem, w, GUEST_PAGE), w += 4;
    stw(mem, w, AT_RANDOM), w += 4;
    stw(mem, w, rand_addr), w += 4;
    stw(mem, w, AT_NULL), w += 4;
    stw(mem, w, 0), w += 4;

    rv->X[rv_reg_sp] = sp;
}

/* ------------------------------------------------------------------------- */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <benchmark.elf> [--json] [guest-arg ...]\n", argv[0]);
        return 2;
    }

    bool json = false;
    uint64_t max_insns = 0; /* 0 = unlimited */
    const char *elf_path = argv[1];
    int guest_argc = 0;
    char *guest_argv[8];
    guest_argv[guest_argc++] = (char *)elf_path; /* argv[0] */
    for (int i = 2; i < argc && guest_argc < 8; i++) {
        if (strcmp(argv[i], "--json") == 0)
            json = true;
        else if (strcmp(argv[i], "--max-insns") == 0 && i + 1 < argc)
            max_insns = strtoull(argv[++i], NULL, 0);
        else
            guest_argv[guest_argc++] = argv[i];
    }

    vm_attr_t attr;
    memset(&attr, 0, sizeof(attr));
    attr.mem_size = GUEST_MEM_SIZE;
    attr.stack_size = GUEST_STACK_SIZE;
    attr.log_level = LOG_FATAL; /* suppress emulator chatter */
    attr.cycle_per_step = 1024;
    attr.fd_stdin = STDIN_FILENO;
    attr.fd_stdout = STDOUT_FILENO;
    attr.fd_stderr = STDERR_FILENO;
    attr.argc = guest_argc;
    attr.argv = guest_argv;
    attr.data.user.elf_program = (char *)elf_path;

    riscv_t *rv = rv_create(&attr);
    if (!rv) {
        fprintf(stderr, "[spike-a] rv_create failed for %s\n", elf_path);
        return 1;
    }
    rv->io.on_ecall = runner_ecall;

    /* Initialise the syscall shim: heap starts at the ELF's _end (page-aligned),
     * anonymous mmaps come from a separate window. */
    g_state.brk_cur = (attr.break_addr + GUEST_PAGE - 1) & ~(GUEST_PAGE - 1);
    g_state.mmap_cur = GUEST_MMAP_BASE;
    g_state.unhandled = 0;

    setup_stack(rv, guest_argc, guest_argv);

    if (getenv("SPIKE_A_DUMP_STACK")) {
        uint32_t sp = rv->X[rv_reg_sp];
        uint32_t argc_r;
        read_guest(rv, &argc_r, sp, 4);
        fprintf(stderr, "[spike-a] sp=%#x argc=%u\n", sp, argc_r);
        for (uint32_t i = 0; i < argc_r && i < 8; i++) {
            uint32_t ptr;
            read_guest(rv, &ptr, sp + 4 + i * 4, 4);
            char s[64] = {0};
            for (int k = 0; k < 63 && ptr + k < GUEST_MEM_SIZE; k++) {
                uint8_t c;
                read_guest(rv, &c, ptr + k, 1);
                if (!c)
                    break;
                s[k] = (char)c;
            }
            fprintf(stderr, "[spike-a]   argv[%u]=%#x \"%s\"\n", i, ptr, s);
        }
    }

    uint64_t cyc0 = rv->csr_cycle;
    bool capped = false;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (!rv_has_halted(rv)) {
        rv_step(rv);
        if (max_insns && (rv->csr_cycle - cyc0) >= max_insns) {
            capped = true;
            break;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint64_t cyc1 = rv->csr_cycle;

    double secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    uint64_t insns = cyc1 - cyc0;
    double mips = secs > 0 ? (double)insns / secs / 1e6 : 0.0;

    const char *base = strrchr(elf_path, '/');
    base = base ? base + 1 : elf_path;

    /* Per-frame instruction distribution (if the guest emitted frame markers).
     * Converted to per-frame time at the measured effective MIPS: a frame of K
     * guest instructions takes K / (MIPS * 1e6) seconds on this host — the basis
     * for the 60 Hz (16.67 ms) budget / headroom judgment. GC/allocation spikes
     * show up as high-percentile frames. */
    size_t nf = g_state.n_frames;
    uint64_t f_mean = 0, f_p50 = 0, f_p95 = 0, f_p99 = 0, f_max = 0;
    if (nf > 0) {
        qsort(g_state.frame_insns, nf, sizeof(uint64_t), cmp_u64);
        uint64_t sum = 0;
        for (size_t i = 0; i < nf; i++)
            sum += g_state.frame_insns[i];
        f_mean = sum / nf;
        f_p50 = g_state.frame_insns[(size_t)(0.50 * (nf - 1))];
        f_p95 = g_state.frame_insns[(size_t)(0.95 * (nf - 1))];
        f_p99 = g_state.frame_insns[(size_t)(0.99 * (nf - 1))];
        f_max = g_state.frame_insns[nf - 1];
    }
#define FRAME_MS(k) (mips > 0 ? (double)(k) / (mips * 1e6) * 1e3 : 0.0)

    bool halted = rv_has_halted(rv);
    if (json) {
        printf("{\"benchmark\":\"%s\",\"guest_insns\":%llu,"
               "\"wall_seconds\":%.6f,\"effective_mips\":%.3f,"
               "\"exit_code\":%d,\"halted\":%s,\"capped\":%s,"
               "\"unhandled_syscalls\":%d",
               base, (unsigned long long)insns, secs, mips, attr.exit_code,
               halted ? "true" : "false", capped ? "true" : "false", g_state.unhandled);
        if (nf > 0)
            printf(",\"frames\":%zu,\"frame_insns_mean\":%llu,\"frame_insns_p50\":%llu,"
                   "\"frame_insns_p95\":%llu,\"frame_insns_p99\":%llu,\"frame_insns_max\":%llu,"
                   "\"frame_ms_mean\":%.4f,\"frame_ms_p99\":%.4f,\"frame_ms_max\":%.4f",
                   nf, (unsigned long long)f_mean, (unsigned long long)f_p50,
                   (unsigned long long)f_p95, (unsigned long long)f_p99,
                   (unsigned long long)f_max, FRAME_MS(f_mean), FRAME_MS(f_p99), FRAME_MS(f_max));
        printf("}\n");
    } else {
        fprintf(stderr, "[spike-a] %-24s insns=%llu wall=%.4fs MIPS=%.2f exit=%d%s%s%s\n", base,
                (unsigned long long)insns, secs, mips, attr.exit_code,
                halted ? "" : " (NOT halted)", capped ? " (CAPPED)" : "",
                g_state.unhandled ? " (unhandled syscalls!)" : "");
        if (nf > 0)
            fprintf(stderr,
                    "[spike-a]   %zu frames  insns/frame: mean=%llu p99=%llu max=%llu  "
                    "@%.1f MIPS -> ms/frame: mean=%.2f p99=%.2f max=%.2f (budget 16.67)\n",
                    nf, (unsigned long long)f_mean, (unsigned long long)f_p99,
                    (unsigned long long)f_max, mips, FRAME_MS(f_mean), FRAME_MS(f_p99),
                    FRAME_MS(f_max));
    }

    free(g_state.frame_insns);
    rv_delete(rv);
    return attr.exit_code;
}
