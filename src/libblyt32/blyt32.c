/*
 * libblyt32 — guest-side RV32IMAFC shared library for the Blyt32 console.
 *
 * On emulated platforms this library is mapped into the rv32emu guest address
 * space by the runtime before the cart entry point is called.  Each function
 * body is a thin ECALL stub that follows the ADR-0085 calling convention:
 *   a7  = ECALL number
 *   a0  = first argument (or return value after the call)
 *   a1  = second argument; for string arguments, the byte length
 *
 * On native RISC-V hardware, libblyt32.so contains real implementations that
 * call into the hardware subsystems directly.  This file is the emulated-
 * platform version only.
 *
 * Cart code never issues ecall instructions directly.  It calls these
 * functions through the PLT, which the runtime resolves to the addresses
 * inside this shared library before execution begins.
 */

#include "blyt.h"

/* -------------------------------------------------------------------------
 * ECALL numbers (ADR-0085, range 1–49: lifecycle)
 * ------------------------------------------------------------------------- */

#define ECALL_CONSOLE_DEBUG 1

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static unsigned int blyt32_strlen(const char *s) {
    const char *p = s;
    while (*p)
        p++;
    return (unsigned int)(p - s);
}

/* -------------------------------------------------------------------------
 * blyt_console_debug — ADR-0085 ECALL stub
 *
 * Passes (a0 = pointer, a1 = byte length) to the runtime, which reads
 * exactly a1 bytes from guest memory.  The runtime's handler writes a
 * NUL-terminated copy to the frontend's log callback.
 * ------------------------------------------------------------------------- */

void blyt_console_debug(const char *s) {
    register const char *a0 __asm__("a0") = s;
    register unsigned int a1 __asm__("a1") = blyt32_strlen(s);
    register long a7 __asm__("a7") = ECALL_CONSOLE_DEBUG;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
}
