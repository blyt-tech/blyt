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
 *   0        exit/abort — halt emulation; a0=0 clean, a0≠0 abort
 *   1–49     lifecycle (console_debug, frame_done, …)
 *   100–199  graphics
 *   …
 */
#define BLYT_ECALL_EXIT 0 /* halt emulation; a0=exit code (0=clean, 1=abort) */
#define BLYT_ECALL_CONSOLE_DEBUG 1 /* blyt_console_debug: a0=ptr, a1=len */
#define BLYT_ECALL_FRAME_DONE 2 /* end of one update+draw cycle */

/*
 * EXIT trampoline — injected into rv32emu guest memory by the runtime.
 *
 * When blyt_main returns, the CPU jumps to BLYT_TRAMPOLINE_EXIT_ADDR
 * (placed in RA before calling blyt_main).  The trampoline sets a0=0
 * (clean exit) and a7=0 (BLYT_ECALL_EXIT), then issues ecall so the
 * host can halt the emulator and report BLYT_RUN_OK.
 *
 * abort() in libblytc.so uses the same ecall but with a0=1, which the
 * handler maps to BLYT_RUN_ERR_ABORT so frontends can distinguish a
 * normal return from a fatal internal error.
 *
 * Layout at BLYT_TRAMPOLINE_BASE (16 bytes):
 *   addi x10, x0, 0   ; li a0, 0  (exit code: 0 = clean)
 *   addi x17, x0, 0   ; li a7, BLYT_ECALL_EXIT
 *   ecall
 *   unimp              ; 0x00000000 — should never be reached
 */
#define BLYT_TRAMPOLINE_BASE 0x00003000u
#define BLYT_TRAMPOLINE_EXIT_ADDR BLYT_TRAMPOLINE_BASE

#define RV32_LI_A0_0 UINT32_C(0x00000513) /* addi x10, x0, 0 */
#define RV32_LI_A7_0 UINT32_C(0x00000893) /* addi x17, x0, 0 */
#define RV32_ECALL UINT32_C(0x00000073)
#define RV32_UNIMP UINT32_C(0x00000000)
