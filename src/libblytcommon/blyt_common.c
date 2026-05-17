/*
 * libblytcommon — shared RV32IMAFC library for variant-portable blyt_* API.
 *
 * Symbols here are declared in blyt.h and work identically on every console
 * variant (blyt32, blytty, blyt3d, …).  Variant libraries link against this
 * library and declare it as DT_NEEDED; carts see the symbols through the
 * variant library's DT_NEEDED chain without listing libblytcommon.so directly.
 *
 * On emulated platforms each function body is a thin ECALL stub (ADR-0085).
 * On native RISC-V hardware this file contains real implementations.
 */

#include "blyt.h"

/* ECALL numbers (ADR-0085, range 1–49: lifecycle) */
#define ECALL_CONSOLE_DEBUG 1

static unsigned int blytcommon_strlen(const char *s) {
    const char *p = s;
    while (*p)
        p++;
    return (unsigned int)(p - s);
}

void blyt_console_debug(const char *s) {
    register const char *a0 __asm__("a0") = s;
    register unsigned int a1 __asm__("a1") = blytcommon_strlen(s);
    register long a7 __asm__("a7") = ECALL_CONSOLE_DEBUG;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
}
