/*
 * runtime/shared/blyt_fpm_musl_shim/syscall_arch.h — Phase B header shim
 * (ADR-0135, blyt#225).
 *
 * The vendored blyt-tech musl stdio subset (vfprintf/vsnprintf/strtod) pulls
 * <stdio_impl.h> → src/internal/syscall.h → "syscall_arch.h", the per-arch inline
 * syscall stubs. musl's real per-arch header defines __syscall0..__syscall6 as
 * register-variable asm (`register long a0 __asm__("a0")`, `svc`, …) that only
 * assemble for that arch — and the seam compiles the reference's riscv32 musl
 * tree even when the actual target is wasm32 (or a non-matching native arch),
 * where clang/wasm-ld reject the RISC-V register names at *parse* time even
 * though the stubs are never called.
 *
 * The seam issues no syscalls (its FILEs are string cookies and stubbed
 * plumbing), so shadow syscall_arch.h with these portable, asm-free stubs — same
 * __syscall0..6 names src/internal/syscall.h dispatches to, but returning -1
 * instead of trapping. Placed ahead of musl's arch dir on the vendored TUs'
 * include path. Never called; present only so the generic __syscall machinery in
 * syscall.h has declarations to expand to.
 */
#ifndef _BLYT_FPM_SYSCALL_ARCH_H
#define _BLYT_FPM_SYSCALL_ARCH_H

static inline long __syscall0(long n) {
    (void)n;
    return -1;
}
static inline long __syscall1(long n, long a) {
    (void)n;
    (void)a;
    return -1;
}
static inline long __syscall2(long n, long a, long b) {
    (void)n;
    (void)a;
    (void)b;
    return -1;
}
static inline long __syscall3(long n, long a, long b, long c) {
    (void)n;
    (void)a;
    (void)b;
    (void)c;
    return -1;
}
static inline long __syscall4(long n, long a, long b, long c, long d) {
    (void)n;
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    return -1;
}
static inline long __syscall5(long n, long a, long b, long c, long d, long e) {
    (void)n;
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    (void)e;
    return -1;
}
static inline long __syscall6(long n, long a, long b, long c, long d, long e, long f) {
    (void)n;
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    (void)e;
    (void)f;
    return -1;
}

#endif
