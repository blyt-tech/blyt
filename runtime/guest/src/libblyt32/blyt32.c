/*
 * libblyt32 — Blyt32 variant shared library, emulated path.
 *
 * Provides ECALL stubs for all Blyt32 API functions.  These shadow
 * libblytcommon.so's definitions via ELF symbol resolution order (carts
 * DT_NEED libblyt32.so directly, so it loads before libblytcommon.so).
 *
 * The native-path implementation lives in frontends/native/src/libblyt32/
 * and uses real Linux syscalls instead of ECALL stubs.
 */

#include "blyt.h"

/* -------------------------------------------------------------------------
 * ECALL numbers (ADR-0085, range 1–49: lifecycle)
 * ------------------------------------------------------------------------- */

#define ECALL_CONSOLE_DEBUG 1

/* -------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

static unsigned int blyt32_strlen(const char *s) {
    const char *p = s;
    while (*p)
        p++;
    return (unsigned int)(p - s);
}

/* -------------------------------------------------------------------------
 * blyt_console_debug — ADR-0085 ECALL stub (a0=ptr, a1=len)
 * ------------------------------------------------------------------------- */

void blyt_console_debug(const char *s) {
    register const char *a0 __asm__("a0") = s;
    register unsigned int a1 __asm__("a1") = blyt32_strlen(s);
    register long a7 __asm__("a7") = ECALL_CONSOLE_DEBUG;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
}
