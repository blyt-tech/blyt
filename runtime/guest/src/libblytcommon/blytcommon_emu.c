/*
 * blytcommon_emu.c — emulated-path impls of the variant-agnostic lifecycle/IO
 * APIs (issue #128).
 *
 * The emulated counterpart to frontends/native/src/libblytcommon/blytcommon.c:
 * the "common API, variant-specific impl" symbols whose emulated form is an
 * ECALL the host services (frame_done, console_debug, exit) or a no-op
 * (runtime_startup).  They live in libblytcommon — not the libblyt32 variant —
 * because the API is variant-agnostic; only the mechanism (ECALL vs native
 * syscall / FCSR / seccomp) differs per variant.
 *
 * The portable lifecycle driver (blyt_main) is in blyt_common.c, compiled into
 * the same library.  The host-backed data-transport stubs (state buffers,
 * save/load, resources) remain in the libblyt32 variant: those delegate to a
 * host-side implementation over the ECALL boundary, so the guest side is
 * genuinely variant transport (and #129/#123 rework them).
 */

#include "blyt.h"

/* ECALL numbers (must match runtime/host/src/libblyt/ecall.h). */
#define ECALL_CONSOLE_DEBUG 1
#define ECALL_FRAME_DONE 2

static unsigned int blytcommon_strlen(const char *s) {
    const char *p = s;
    while (*p)
        p++;
    return (unsigned int)(p - s);
}

/* blyt_frame_done — end-of-frame signal (ECALL 2).
 *
 * Called by blyt_main after each blyt_cart_draw().  The host intercepts this
 * ECALL, runs its frame callback (SDL event polling, frame-rate cap, etc.),
 * then resumes the emulator for the next frame without halting it. */
void blyt_frame_done(void) {
    register long a7 __asm__("a7") = ECALL_FRAME_DONE;
    __asm__ volatile("ecall" : : "r"(a7) : "memory");
}

/* blyt_console_debug — ADR-0085 ECALL stub (a0=ptr, a1=len). */
void blyt_console_debug(const char *s) {
    register const char *a0 __asm__("a0") = s;
    register unsigned int a1 __asm__("a1") = blytcommon_strlen(s);
    register long a7 __asm__("a7") = ECALL_CONSOLE_DEBUG;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
}

/* blyt_exit — clean process exit.
 *
 * Called by _blyt_entry after blyt_main() returns.  exit_group(2) (NR 94) is
 * the same mechanism on both paths — on the emulated path the emulator
 * intercepts the ECALL and halts the simulation; on native the kernel handles
 * it — so this is variant-agnostic.  In practice ECALL_QUIT usually halts the
 * emulator first, so blyt_exit is rarely reached on the emulated path. */
__attribute__((noreturn)) void blyt_exit(int code) {
    register long a0 __asm__("a0") = code;
    register long a7 __asm__("a7") = 94; /* SYS_exit_group */
    __asm__ volatile("ecall" : : "r"(a0), "r"(a7));
    __builtin_unreachable();
}

/* blyt_runtime_startup — no-op on emulated targets.  The native libblytcommon
 * variant installs the restricted seccomp filter and resets FCSR before cart
 * code runs; on the emulated path the host owns that setup, so there is nothing
 * to do here. */
void blyt_runtime_startup(void) {
}
