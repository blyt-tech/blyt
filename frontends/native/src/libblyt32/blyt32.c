/*
 * libblyt32 — Blyt32 variant shared library, native RISC-V path.
 *
 * Built from frontends/native/src/libblyt32/ and installed alongside the
 * LP64 launcher.  The launcher sets LD_LIBRARY_PATH so the musl ILP32 ld.so
 * finds this library before the emulated-path version.
 *
 * Provides:
 *   1. Strong definitions of blyt API functions that use real Linux syscalls,
 *      shadowing libblytcommon.so's ECALL stubs via ELF load-order precedence.
 *   2. A constructor that installs the restricted seccomp filter before any
 *      cart code runs.  The constructor fires only on the native path —
 *      blyt's custom dynlinker on emulated targets does not call ELF
 *      constructors.
 *
 * Platform-specific API functions are added here as the API surface grows
 * (graphics → framebuffer writes, audio → hardware, input → /dev/input, …).
 */

#include "blyt.h"
#include "seccomp_restricted.h"

/* ── Seccomp constructor ─────────────────────────────────────────────────
 *
 * Runs after ld.so resolves all DT_NEEDED libraries, before cart code starts.
 * Installs the restricted RISCV32-only seccomp filter over the launcher's
 * arch-dispatch filter (LIFO: restricted filter evaluated first).
 *
 * PR_SET_NO_NEW_PRIVS is inherited from the launcher across execve; no
 * prctl call is needed here.
 */
__attribute__((constructor)) static void libblyt32_install_seccomp(void) {
    static const char dbg_a[] = "CTOR_ENTER\n";
    blyt_rs_write(2, dbg_a, sizeof(dbg_a) - 1);
    if (blyt_install_restricted_filter() != 0) {
        static const char msg[] = "libblyt32: FATAL: seccomp install failed\n";
        blyt_rs_write(2, msg, sizeof(msg) - 1);
        blyt_rs_exit_group(127);
    }
    static const char dbg_b[] = "CTOR_DONE\n";
    blyt_rs_write(2, dbg_b, sizeof(dbg_b) - 1);
    /* Explicitly reset FCSR to RNE+no-flags regardless of OS/ld.so state. */
    __asm__ volatile("csrw fcsr, zero" ::: "memory");
}

/* ── Native API implementations ──────────────────────────────────────────
 *
 * Strong definitions shadow libblytcommon.so's ECALL stubs: libblyt32.so is
 * in the cart's direct DT_NEEDED so it loads and wins symbol resolution
 * before libblytcommon.so.
 */

static unsigned int blyt32_native_strlen(const char *s) {
    const char *p = s;
    while (*p)
        p++;
    return (unsigned int)(p - s);
}

/* blyt_frame_done — frame-boundary housekeeping on the native path.
 *
 * Called by the cart at the end of each logical frame.  Enforces FP
 * determinism (ADR-0007) by checking and resetting the RISC-V FCSR:
 *
 *   frm (bits 7:5) — rounding mode.  Must be 0 (round-to-nearest-even,
 *   the IEEE 754 default) at every frame boundary.  A non-zero frm means
 *   the cart or one of its libraries called fesetround() or modified frm
 *   directly, which would cause FP results to diverge across runs.
 *
 *   fflags (bits 4:0) — accumulated FP exception flags (NX/UF/OF/DZ/NV).
 *   These are set by normal FP arithmetic and do not affect determinism;
 *   they are cleared here to give each frame a clean starting state.
 *
 * Debug builds emit a warning and continue; release builds abort because
 * a dirty frm means results from this frame are already non-deterministic
 * and allowing the cart to continue would compound the divergence. */
void blyt_frame_done(void) {
    unsigned int fcsr;
    __asm__ volatile("csrr %0, fcsr" : "=r"(fcsr));
    unsigned int frm = (fcsr >> 5) & 0x7u;
    if (frm != 0u) {
#ifndef NDEBUG
        static const char pfx[] = "blyt: WARNING: cart set non-default FP rounding mode (frm=";
        static const char sfx[] = "); results may be non-deterministic\n";
        char digit = (char)('0' + frm);
        blyt_rs_write(2, pfx, sizeof(pfx) - 1);
        blyt_rs_write(2, &digit, 1);
        blyt_rs_write(2, sfx, sizeof(sfx) - 1);
#else
        static const char msg[] = "blyt: cart set non-default FP rounding mode; "
                                  "aborting for determinism\n";
        blyt_rs_write(2, msg, sizeof(msg) - 1);
        blyt_rs_exit_group(1);
#endif
    }
    /* Reset frm to RNE (0) and clear accumulated fflags for the next frame.
     * The memory clobber prevents the compiler from reordering FP operations
     * across this boundary. */
    __asm__ volatile("csrw fcsr, zero" ::: "memory");
}

/* blyt_exit — clean process exit after cart main loop.
 *
 * Called by _blyt_entry (the ELF entry point stub) after blyt_main() returns.
 * Bypasses musl's exit() cleanup path, which calls munmap and other syscalls
 * blocked by the restricted seccomp filter.  exit_group(0) is in the allowlist.
 *
 * Declared __attribute__((noreturn)) so the compiler can omit the return path
 * in _blyt_entry and avoid generating a dead-code epilogue.
 */
__attribute__((noreturn)) void blyt_exit(int code) {
    blyt_rs_exit_group(code);
}

/* blyt_console_debug — SYS_write(fd=2, s, len).
 * write(2) is NR 64, in the restricted allowlist. */
void blyt_console_debug(const char *s) {
    unsigned int len = blyt32_native_strlen(s);
    register long a0 __asm__("a0") = 2; /* STDERR_FILENO */
    register const char *a1 __asm__("a1") = s;
    register long a2 __asm__("a2") = len;
    register long a7 __asm__("a7") = 64; /* SYS_write */
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
}
