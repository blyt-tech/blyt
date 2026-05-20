/*
 * native_hello — minimal Phase-6 gate cart for the native RISC-V target.
 *
 * Calls blyt_console_debug (resolved from native libblyt32.so → write(2,...))
 * then exits cleanly via SYS_exit_group.  Used to verify that the LP64
 * launcher, phase-1 and phase-2 seccomp filters, and the native API path all
 * work end-to-end on the QEMU ILP32 target.
 *
 * Building on the QEMU host (requires riscv32-linux-musl toolchain):
 *
 *   CC=riscv32-linux-musl-gcc   # or: clang --target=riscv32-unknown-linux-musl
 *   LIBS=<path-to-build/native>
 *   SDK_INC=<path-to-build/sdk/include>
 *
 *   $CC -march=rv32imafc -mabi=ilp32f \
 *       -I $SDK_INC -L $LIBS -lblyt32 \
 *       -Wl,-rpath-link,$LIBS \
 *       -T <path-to-hello/build/game/c/blyt_cart.ld> \
 *       -o native_hello.elf native_hello.c _blyt_entry.c
 *
 * Running on QEMU:
 *   ./blyt_native --lib-dir $LIBS -- ./native_hello.elf
 *
 * Expected: "hello from native RISC-V" on stderr, exit code 0.
 */

#include "blyt.h"

/* Entry point: called directly by the ELF loader (no _start / main). */
void _blyt_entry(void) {
    blyt_console_debug("hello from native RISC-V");

    /* SYS_exit_group(0) — in phase-2 allowlist (NR 94) */
    register long a0 __asm__("a0") = 0;
    register long a7 __asm__("a7") = 94;
    __asm__ volatile("ecall" : : "r"(a0), "r"(a7));
    __builtin_unreachable();
}
