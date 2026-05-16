#pragma once

#include <stdint.h>

/*
 * Blyt ECALL numbers (a7 register before the ecall instruction, ADR-0085).
 *
 * Cart code never issues ecall instructions.  ECALLs are issued by
 * libblyt32.so's stub functions inside the rv32emu guest address space.
 * When rv32emu's on_ecall fires, the runtime dispatches on a7.
 *
 * Number space (ADR-0085):
 *   0        reserved/invalid — used internally for the EXIT trampoline only
 *   1–49     lifecycle (quit_ready, log, …)
 *   100–199  graphics
 *   …
 */
#define BLYT_ECALL_EXIT 0 /* internal: halt emulation cleanly */
#define BLYT_ECALL_CONSOLE_DEBUG 1 /* blyt_console_debug: a0=ptr, a1=len */

/*
 * EXIT trampoline — injected into rv32emu guest memory by the runtime.
 *
 * When blyt_main returns, the CPU jumps to BLYT_TRAMPOLINE_EXIT_ADDR
 * (which the runtime places in RA before calling blyt_main).  The
 * trampoline issues ECALL 0 so the host's on_ecall handler can halt the
 * emulator cleanly.
 *
 * CONSOLE_DEBUG no longer needs a trampoline: the ecall fires from inside
 * libblyt32.so's stub code, which is mapped into guest memory by the
 * dynamic loader.
 *
 * Layout at BLYT_TRAMPOLINE_BASE (12 bytes):
 *   addi x17, x0, 0   ; li a7, BLYT_ECALL_EXIT
 *   ecall
 *   unimp              ; 0x00000000 — illegal, should never be reached
 */
#define BLYT_TRAMPOLINE_BASE 0x00003000u
#define BLYT_TRAMPOLINE_EXIT_ADDR BLYT_TRAMPOLINE_BASE

#define RV32_LI_A7_0 UINT32_C(0x00000893) /* addi x17, x0, 0 */
#define RV32_ECALL UINT32_C(0x00000073)
#define RV32_UNIMP UINT32_C(0x00000000)
