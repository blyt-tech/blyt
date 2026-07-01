#pragma once

#include <stdint.h>

/*
 * blyt_phase — cart lifecycle phase, the guest→host signal that makes surface
 * access draw()-only (issue #195 / #205; extends ADR-0076/0122).
 *
 * The host cannot bracket draw() on the emulated path: the guest's blyt_main
 * (runtime/guest/src/libblytcommon/blyt_common.c) drives the update→draw loop
 * and the host only sees ECALLs.  So blyt_main tells the runtime which
 * lifecycle phase it is entering via blyt_phase_enter(); every surface access
 * op is then permitted only while the phase is DRAW.  Outside draw():
 *
 *   - a debug build hard-errors (dev trap — the misuse is a bug to fix), and
 *   - a release build applies a defined, leg-identical no-op: writes are
 *     dropped, a tier-2 acquire reads as cleared (never UB).
 *
 * The mechanism differs per execution variant but the phase values are one
 * shared vocabulary, so this header lives in runtime/shared:
 *   - emulated: blyt_phase_enter is an ECALL (BLYT_ECALL_PHASE) the host
 *     services, storing the phase on the run-ctx (runtime/host/.../cart_run.c).
 *   - native bare-metal: blyt_phase_enter stores a process global that the
 *     native surface ops read back through blyt_phase_current()
 *     (frontends/native/src/libblytcommon + libblyt32).
 */
typedef enum {
    BLYT_PHASE_NONE = 0, /* outside any cart callback */
    BLYT_PHASE_INIT = 1, /* inside blyt_cart_init / on_new_state */
    BLYT_PHASE_UPDATE = 2, /* inside blyt_cart_update */
    BLYT_PHASE_DRAW = 3, /* inside blyt_cart_draw — the only phase surfaces allow */
} blyt_phase_t;

/* Emitted by blyt_main (blyt_common.c) around each lifecycle callback.  Variant
 * impls: blytcommon_emu.c (ECALL) and native blytcommon.c (process global). */
void blyt_phase_enter(int32_t phase);

/* The current phase.  Only the native path needs this (its surface ops run
 * in-process and read it directly); on the emulated path the host owns the
 * phase and the guest never reads it back. */
int32_t blyt_phase_current(void);
