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

/* blyt_frame_done — no-op on the native path.
 * Shadows libblytcommon.so's ECALL stub (a7=2 = io_submit on Linux, blocked
 * by the restricted seccomp filter).  Frame pacing via clock_nanosleep is
 * deferred. */
void blyt_frame_done(void) {
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
