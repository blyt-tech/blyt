/* seccomp_restricted.h — Restricted RISC-V ILP32 seccomp filter for libblyt32.so.
 *
 * Installed by the libblyt32_install_seccomp constructor in the ILP32 cart
 * process before any cart code runs.  The launcher's arch-dispatch filter
 * (installed before execve) permits seccomp(2) for the RISCV32 process so
 * this constructor can run.  PR_SET_NO_NEW_PRIVS is inherited from the
 * launcher across exec.
 *
 * All kernel ABI constants are defined inline — no linux/filter.h or
 * linux/seccomp.h dependency — so this header compiles for the bare-metal
 * RV32IMAFC target used to cross-compile the guest libraries.
 *
 * Allowlist (empirically validated, Spike S 2026-05-13):
 *   29  ioctl       isatty probe in stdio
 *   63  read        cart input
 *   64  write       SYS_write — blyt_console_debug and other output
 *   93  exit        single-thread exit
 *   94  exit_group  cart exits
 */

#ifndef BLYT_SECCOMP_RESTRICTED_H
#define BLYT_SECCOMP_RESTRICTED_H

/* ── Stable kernel ABI constants (inline — no linux/filter.h dependency) ── */

#define BLYT_BPF_LD 0x00u
#define BLYT_BPF_W 0x00u
#define BLYT_BPF_ABS 0x20u
#define BLYT_BPF_JMP 0x05u
#define BLYT_BPF_JEQ 0x10u
#define BLYT_BPF_K 0x00u
#define BLYT_BPF_RET 0x06u

#define BLYT_SDAT_NR 0u /* offsetof(seccomp_data, nr)   */
#define BLYT_SDAT_ARCH 4u /* offsetof(seccomp_data, arch) */

#define BLYT_SECCOMP_RET_KILL_PROCESS 0x80000000u
#define BLYT_SECCOMP_RET_ALLOW 0x7fff0000u

/* AUDIT_ARCH_RISCV32 = EM_RISCV(0xF3) | LE(0x40000000) */
#define BLYT_AUDIT_ARCH_RISCV32 0x400000F3u

#define BLYT_SECCOMP_SET_MODE_FILTER 1u

/* matches struct sock_filter in linux/filter.h (stable kernel ABI) */
struct blyt_sock_filter {
    unsigned short code;
    unsigned char jt;
    unsigned char jf;
    unsigned int k;
};

/* matches struct sock_fprog in linux/filter.h (stable kernel ABI) */
struct blyt_sock_fprog {
    unsigned short len;
    struct blyt_sock_filter *filter;
};

/* ── Restricted allowlist ── */

static const unsigned int blyt_restricted_nrs[] = {
    29u, /* ioctl      */
    63u, /* read       */
    64u, /* write      */
    93u, /* exit       */
    94u, /* exit_group */
};
#define BLYT_RESTRICTED_N ((int)(sizeof(blyt_restricted_nrs) / sizeof(blyt_restricted_nrs[0])))

/* ── Raw RV32 syscall helpers ── */

static inline void blyt_rs_write(int fd, const char *buf, unsigned int len)
{
    register long a0 __asm__("a0") = fd;
    register const char *a1 __asm__("a1") = buf;
    register long a2 __asm__("a2") = len;
    register long a7 __asm__("a7") = 64; /* SYS_write */
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
}

static __attribute__((noreturn)) void blyt_rs_exit_group(int code)
{
    register long a0 __asm__("a0") = code;
    register long a7 __asm__("a7") = 94; /* SYS_exit_group */
    __asm__ volatile("ecall" : : "r"(a0), "r"(a7) : "memory");
    __builtin_unreachable();
}

static inline int blyt_rs_seccomp(unsigned int mode, unsigned int flags, void *prog)
{
    register long a0 __asm__("a0") = mode;
    register long a1 __asm__("a1") = flags;
    register long a2 __asm__("a2") = (long)prog;
    register long a7 __asm__("a7") = 277; /* SYS_seccomp */
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return (int)a0;
}

/* ── Restricted filter builder ──
 *
 * RISCV32-only filter.  Layout (N = BLYT_RESTRICTED_N, total = 4 + 2*N insns):
 *   [0]        LD arch
 *   [1]        JEQ RISCV32, jt=1, jf=0  →  skip KILL; else KILL
 *   [2]        RET KILL_PROCESS  (wrong arch — should never fire)
 *   [3]        LD nr
 *   [4..4+2N)  N × (JEQ nr, jt=0, jf=1  /  RET ALLOW)
 *   [4+2N]     RET KILL_PROCESS  (default deny)
 */
#define BLYT_RESTRICTED_BUF 32

static int blyt_build_restricted_filter(struct blyt_sock_filter *buf, int bufsz)
{
    int n = BLYT_RESTRICTED_N;
    int total = 4 + 2 * n;
    if (total > bufsz)
        return -1;

    int i = 0;

#define FS(c_, k_)                          \
    do {                                    \
        buf[i].code = (unsigned short)(c_); \
        buf[i].jt = 0;                      \
        buf[i].jf = 0;                      \
        buf[i].k = (k_);                    \
        i++;                                \
    } while (0)
#define FJ(c_, k_, jt_, jf_)               \
    do {                                    \
        buf[i].code = (unsigned short)(c_); \
        buf[i].jt = (unsigned char)(jt_);   \
        buf[i].jf = (unsigned char)(jf_);   \
        buf[i].k = (k_);                    \
        i++;                                \
    } while (0)

    FS(BLYT_BPF_LD | BLYT_BPF_W | BLYT_BPF_ABS, BLYT_SDAT_ARCH);
    FJ(BLYT_BPF_JMP | BLYT_BPF_JEQ | BLYT_BPF_K, BLYT_AUDIT_ARCH_RISCV32, 1, 0);
    FS(BLYT_BPF_RET | BLYT_BPF_K, BLYT_SECCOMP_RET_KILL_PROCESS);

    FS(BLYT_BPF_LD | BLYT_BPF_W | BLYT_BPF_ABS, BLYT_SDAT_NR);
    for (int j = 0; j < n; j++) {
        FJ(BLYT_BPF_JMP | BLYT_BPF_JEQ | BLYT_BPF_K, blyt_restricted_nrs[j], 0, 1);
        FS(BLYT_BPF_RET | BLYT_BPF_K, BLYT_SECCOMP_RET_ALLOW);
    }
    FS(BLYT_BPF_RET | BLYT_BPF_K, BLYT_SECCOMP_RET_KILL_PROCESS);

#undef FS
#undef FJ

    return i;
}

static int blyt_install_restricted_filter(void)
{
    struct blyt_sock_filter prog[BLYT_RESTRICTED_BUF];
    int len = blyt_build_restricted_filter(prog, BLYT_RESTRICTED_BUF);
    if (len < 0)
        return -1;
    struct blyt_sock_fprog fp;
    fp.len = (unsigned short)len;
    fp.filter = prog;
    return blyt_rs_seccomp(BLYT_SECCOMP_SET_MODE_FILTER, 0, &fp);
}

#endif /* BLYT_SECCOMP_RESTRICTED_H */
