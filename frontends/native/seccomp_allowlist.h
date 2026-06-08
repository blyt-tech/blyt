/* seccomp_allowlist.h — Arch-dispatch seccomp filter for the LP64 launcher.
 *
 * Provides blyt_build_launcher_filter() which constructs a BPF program that applies
 * different rules to the LP64 launcher (AUDIT_ARCH_RISCV64) and to the ILP32
 * cart process after exec (AUDIT_ARCH_RISCV32).
 *
 * Empirically validated allowlists (Spike S, Spike R, 2026-05-11/13):
 *
 *   RISCV64 (LP64 launcher, post-filter-install):
 *     221  execve       launcher → cart binary
 *      64  write        error messages before exec
 *      94  exit_group   launcher exits on error
 *
 *   RISCV32 (ILP32 process: ld.so startup + libblyt32.so constructor):
 *     Confirmed by strace (Stage 2, adversary_s_dynamic on c-sky 6.5-rc1):
 *      56  openat       open DT_NEEDED libraries
 *      57  close        close fd after ELF load
 *      63  read         read ELF headers
 *      25  fcntl        F_SETFD(FD_CLOEXEC) after openat
 *     291  statx        musl uses statx (not fstat) for file info
 *     222  mmap         ELF PT_LOAD mapping
 *     226  mprotect     segment permissions after mmap
 *     214  brk          heap init during ld.so startup
 *      96  set_tid_address  musl TLS self-pointer init
 *      66  writev       musl stdio (constructor fprintf)
 *     277  seccomp      install restricted filter
 *     Conservative extras (not seen with minimal binary; expected for larger):
 *      80  fstat        older musl versions
 *     215  munmap       unmap temporaries
 *     278  getrandom    stack-canary entropy
 *     261  prlimit64    libc stack-size probe
 *      29  ioctl        isatty probe
 *      64  write        cart output
 *      72  fsync        flush save files
 *      93  exit         single-thread exit
 *      94  exit_group   cart exits
 *     mkdirat intentionally absent: launcher pre-creates directories (ADR-0131).
 *
 * All constants are defined inline; no linux/filter.h dependency.
 */

#ifndef BLYT_SECCOMP_ALLOWLIST_H
#define BLYT_SECCOMP_ALLOWLIST_H

#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

/* ── Stable kernel ABI constants ── */

#define BLYTAL_BPF_LD 0x00u
#define BLYTAL_BPF_W 0x00u
#define BLYTAL_BPF_ABS 0x20u
#define BLYTAL_BPF_JMP 0x05u
#define BLYTAL_BPF_JEQ 0x10u
#define BLYTAL_BPF_K 0x00u
#define BLYTAL_BPF_RET 0x06u

#define BLYTAL_SDAT_NR 0u
#define BLYTAL_SDAT_ARCH 4u

#define BLYTAL_SECCOMP_RET_KILL_PROCESS 0x80000000u
#define BLYTAL_SECCOMP_RET_ALLOW 0x7fff0000u

/* AUDIT_ARCH_RISCV64 = EM_RISCV(0xF3) | LE(0x40000000) | 64BIT(0x80000000) */
#define BLYTAL_AUDIT_ARCH_RISCV64 0xC00000F3u
/* AUDIT_ARCH_RISCV32 = EM_RISCV(0xF3) | LE(0x40000000) */
#define BLYTAL_AUDIT_ARCH_RISCV32 0x400000F3u

#define BLYTAL_SECCOMP_SET_MODE_FILTER 1u

struct blytal_sock_filter {
    unsigned short code;
    unsigned char jt;
    unsigned char jf;
    unsigned int k;
};

struct blytal_sock_fprog {
    unsigned short len;
    struct blytal_sock_filter *filter;
};

/* ── Allowlists ── */

static const unsigned int blytal_rv64_nrs[] = {
    221u, /* execve      launcher → cart binary      */
    64u, /* write       error output before exec    */
    94u, /* exit_group  launcher exits on error     */
};
#define BLYTAL_RV64_N ((int)(sizeof(blytal_rv64_nrs) / sizeof(blytal_rv64_nrs[0])))

static const unsigned int blytal_rv32_nrs[] = {
    /* ── ld.so library loading (Stage 2 strace confirmed) ── */
    56u, /* openat           */
    57u, /* close            */
    63u, /* read             */
    25u, /* fcntl            */
    291u, /* statx            */
    222u, /* mmap             */
    226u, /* mprotect         */
    214u, /* brk              */
    96u, /* set_tid_address  */
    /* ── musl stdio (constructor fprintf) ── */
    66u, /* writev           */
    /* ── seccomp install ── */
    277u, /* seccomp          */
    /* ── conservative extras ── */
    80u, /* fstat            */
    215u, /* munmap           */
    278u, /* getrandom        */
    261u, /* prlimit64        */
    29u, /* ioctl            */
    64u, /* write            */
    72u, /* fsync            — flush save files */
    93u, /* exit             */
    94u, /* exit_group       */
};
#define BLYTAL_RV32_N ((int)(sizeof(blytal_rv32_nrs) / sizeof(blytal_rv32_nrs[0])))

/* ── Arch-dispatch filter builder ──────────────────────────────────────────
 *
 * Layout (N64 = BLYTAL_RV64_N, N32 = BLYTAL_RV32_N):
 *
 *   [0]              LD arch
 *   [1]              JEQ RISCV64, jt=0, jf=(2+2*N64)  → RV64 block or skip
 *   [2]              LD nr                              ┐
 *   [3..2+2*N64]     N64 × (JEQ nr, jt=0, jf=1 / ALLOW) │ RV64 block
 *   [3+2*N64]        RET KILL                           ┘
 *   [4+2*N64]        JEQ RISCV32, jt=1, jf=0
 *   [5+2*N64]        RET KILL  (unknown arch)
 *   [6+2*N64]        LD nr                              ┐
 *   [7+2*N64..]      N32 × (JEQ nr, jt=0, jf=1 / ALLOW) │ RV32 block
 *   [7+2*(N64+N32)]  RET KILL                           ┘
 *
 * Total: 8 + 2*(N64 + N32) instructions.
 */

#define BLYTAL_FILTER_BUF 256

static int blyt_build_launcher_filter(struct blytal_sock_filter *buf, int bufsz,
                                      const unsigned int *rv64, int n64, const unsigned int *rv32,
                                      int n32) {
    int total = 8 + 2 * (n64 + n32);
    if (total > bufsz) {
        fprintf(stderr, "blyt_launcher: launcher filter too large: %d > %d\n", total, bufsz);
        return -1;
    }
    if ((2 + 2 * n64) > 255) {
        fprintf(stderr, "blyt_launcher: RV64 block too large for jf offset\n");
        return -1;
    }

    int i = 0;

#define FS(c_, k_)                                                                                 \
    do {                                                                                           \
        buf[i].code = (unsigned short)(c_);                                                        \
        buf[i].jt = 0;                                                                             \
        buf[i].jf = 0;                                                                             \
        buf[i].k = (k_);                                                                           \
        i++;                                                                                       \
    } while (0)
#define FJ(c_, k_, jt_, jf_)                                                                       \
    do {                                                                                           \
        buf[i].code = (unsigned short)(c_);                                                        \
        buf[i].jt = (unsigned char)(jt_);                                                          \
        buf[i].jf = (unsigned char)(jf_);                                                          \
        buf[i].k = (k_);                                                                           \
        i++;                                                                                       \
    } while (0)

    /* Arch dispatch */
    FS(BLYTAL_BPF_LD | BLYTAL_BPF_W | BLYTAL_BPF_ABS, BLYTAL_SDAT_ARCH);
    FJ(BLYTAL_BPF_JMP | BLYTAL_BPF_JEQ | BLYTAL_BPF_K, BLYTAL_AUDIT_ARCH_RISCV64, 0,
       (unsigned char)(2 + 2 * n64));

    /* RV64 block */
    FS(BLYTAL_BPF_LD | BLYTAL_BPF_W | BLYTAL_BPF_ABS, BLYTAL_SDAT_NR);
    for (int j = 0; j < n64; j++) {
        FJ(BLYTAL_BPF_JMP | BLYTAL_BPF_JEQ | BLYTAL_BPF_K, rv64[j], 0, 1);
        FS(BLYTAL_BPF_RET | BLYTAL_BPF_K, BLYTAL_SECCOMP_RET_ALLOW);
    }
    FS(BLYTAL_BPF_RET | BLYTAL_BPF_K, BLYTAL_SECCOMP_RET_KILL_PROCESS);

    /* RV32 check */
    FJ(BLYTAL_BPF_JMP | BLYTAL_BPF_JEQ | BLYTAL_BPF_K, BLYTAL_AUDIT_ARCH_RISCV32, 1, 0);
    FS(BLYTAL_BPF_RET | BLYTAL_BPF_K, BLYTAL_SECCOMP_RET_KILL_PROCESS);

    /* RV32 block */
    FS(BLYTAL_BPF_LD | BLYTAL_BPF_W | BLYTAL_BPF_ABS, BLYTAL_SDAT_NR);
    for (int j = 0; j < n32; j++) {
        FJ(BLYTAL_BPF_JMP | BLYTAL_BPF_JEQ | BLYTAL_BPF_K, rv32[j], 0, 1);
        FS(BLYTAL_BPF_RET | BLYTAL_BPF_K, BLYTAL_SECCOMP_RET_ALLOW);
    }
    FS(BLYTAL_BPF_RET | BLYTAL_BPF_K, BLYTAL_SECCOMP_RET_KILL_PROCESS);

#undef FS
#undef FJ

    return i;
}

static int blyt_install_launcher_filter(void) {
    struct blytal_sock_filter prog[BLYTAL_FILTER_BUF];
    int len = blyt_build_launcher_filter(prog, BLYTAL_FILTER_BUF, blytal_rv64_nrs, BLYTAL_RV64_N,
                                         blytal_rv32_nrs, BLYTAL_RV32_N);
    if (len < 0)
        return -1;
    fprintf(stderr, "blyt_native: launcher filter: %d insns (rv64=%d rv32=%d)\n", len,
            BLYTAL_RV64_N, BLYTAL_RV32_N);
    struct blytal_sock_fprog fp;
    fp.len = (unsigned short)len;
    fp.filter = prog;
    if (syscall(SYS_seccomp, BLYTAL_SECCOMP_SET_MODE_FILTER, 0, &fp) != 0) {
        perror("seccomp");
        return -1;
    }
    return 0;
}

#endif /* BLYT_SECCOMP_ALLOWLIST_H */
