#pragma once

#include "blyt_runtime.h"
#include "cart_load.h"

/*
 * Execute cart code in the rv32emu RISC-V emulator.
 *
 * This is the internal implementation of blyt_cart_run (declared in
 * blyt_runtime.h). The cart must have already passed blyt_cart_open.
 *
 * Steps:
 *   1. Write the cart to a temp file (rv32emu needs a path to open).
 *   2. Set up rv32emu with the cart ELF.
 *   3. Inject a trampoline page (contains ecall stubs for each blyt API fn).
 *   4. Patch the cart's GOT entries to point to the trampolines.
 *   5. Override rv32emu's on_ecall with blyt_ecall_handler.
 *   6. Set RA = EXIT trampoline address (so blyt_main's return halts cleanly).
 *   7. Run rv32emu until halt.
 */
blyt_cart_run_err_t blyt_cart_run_impl(blyt_cart_t *cart, blyt_log_fn log_fn);
