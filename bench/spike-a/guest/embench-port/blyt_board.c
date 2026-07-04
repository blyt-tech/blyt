/*
 * Embench-IoT board-support layer for the blyt Spike A harness.
 *
 * Target: bare RV32IMAFDC/ilp32d static-musl ELF run through the blyt rv32emu
 * interpreter core (see ../../host/runner.c). Embench is normally built with a
 * per-board support file (examples/<arch>/<board>/boardsupport.c); this is the
 * minimal "blyt" board.
 *
 * The three seams (initialise_board / start_trigger / stop_trigger) are no-ops:
 * the authoritative effective-MIPS figure comes from the runner's retired-
 * instruction counter (rv->csr_cycle) measured against host wall-clock, so the
 * board does not need to read any clock. The trigger functions keep the
 * noinline/externally_visible attributes the upstream boards use so the
 * measured region is not reordered or elided.
 *
 * We replace Embench's support/board.c (which #includes a per-config
 * boardsupport.c) rather than patch it, keeping the upstream tree pristine.
 * CPU_MHZ is only consumed by Embench's Python post-processing, never by the C
 * sources, so it is not defined here. WARMUP_HEAT and GLOBAL_SCALE_FACTOR are
 * supplied on the compiler command line by build-embench.sh.
 */

#include "support.h"

void initialise_board(void) {
}

void __attribute__((noinline)) __attribute__((externally_visible)) start_trigger(void) {
}

void __attribute__((noinline)) __attribute__((externally_visible)) stop_trigger(void) {
}
