#pragma once

#include <stdint.h>

/*
 * Blyt ECALL numbers (placed in a7 before the ecall instruction).
 * The cart never contains ecall instructions — only the runtime-injected
 * trampolines do. Carts call blyt API functions via PLT → GOT → trampoline.
 */
#define BLYT_ECALL_EXIT 0 /* halt emulation cleanly */
#define BLYT_ECALL_CONSOLE_DEBUG 1 /* blyt_console_debug(const char *s) */

/*
 * Trampoline page: a small region the runtime writes into emulated memory
 * before execution begins. Each entry is a short RV32 stub ending with ecall.
 *
 * Layout at BLYT_TRAMPOLINE_BASE:
 *
 *  +0   EXIT trampoline   (BLYT_TRAMPOLINE_EXIT_OFFSET)
 *  +12  CONSOLE_DEBUG trampoline (BLYT_TRAMPOLINE_CONSOLE_DEBUG_OFFSET)
 *
 * Each trampoline is at most 12 bytes (3 × 4-byte instructions).
 * Total page: 32 bytes — fits well within one aligned cache line.
 */
#define BLYT_TRAMPOLINE_BASE 0x00003000u

#define BLYT_TRAMPOLINE_EXIT_OFFSET 0u
#define BLYT_TRAMPOLINE_CONSOLE_DEBUG_OFFSET 12u

#define BLYT_TRAMPOLINE_EXIT_ADDR (BLYT_TRAMPOLINE_BASE + BLYT_TRAMPOLINE_EXIT_OFFSET)
#define BLYT_TRAMPOLINE_CONSOLE_DEBUG_ADDR                                                         \
    (BLYT_TRAMPOLINE_BASE + BLYT_TRAMPOLINE_CONSOLE_DEBUG_OFFSET)

/*
 * RV32 instruction encodings for the trampolines.
 *
 * EXIT trampoline (12 bytes):
 *   addi x17, x0, 0   ; li a7, BLYT_ECALL_EXIT
 *   ecall
 *   unimp              ; 0x00000000 — illegal insn, should never reach here
 *
 * CONSOLE_DEBUG trampoline (12 bytes):
 *   addi x17, x0, 1   ; li a7, BLYT_ECALL_CONSOLE_DEBUG
 *   ecall              ; dispatch to runtime handler
 *   jalr x0, x1, 0    ; ret — return to caller
 */
#define RV32_LI_A7_0 UINT32_C(0x00000893) /* addi x17, x0, 0 */
#define RV32_LI_A7_1 UINT32_C(0x00100893) /* addi x17, x0, 1 */
#define RV32_ECALL UINT32_C(0x00000073)
#define RV32_UNIMP UINT32_C(0x00000000) /* illegal — 0x0000 decoded twice */
#define RV32_RET UINT32_C(0x00008067) /* jalr x0, x1, 0 */
