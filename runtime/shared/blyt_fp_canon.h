/*
 * blyt_fp_canon.h — NaN canonicalization for state-buffer FP writes (ADR-0010).
 *
 * Part of runtime/shared: freestanding portable C (stdint/stddef only, no libc,
 * no allocator, no stdio, no global ctors).  Compiled into BOTH the host runtime
 * (runtime/host/libblyt) and the native guest libs (RV32, -nostdlib, runs before
 * the restricted seccomp filter is installed).  See runtime/shared/README and
 * issue #128.
 *
 * Writing a NaN to an f32/f64 state-buffer field canonicalizes it to the RISC-V
 * / IEEE-754 default quiet NaN so NaN payload bits — architecturally meaningless
 * but not bit-identical across platforms — can never cause cross-platform
 * divergence (determinism is the core console contract, ADR-0007).  These are
 * pure bit-pattern predicates so they need no <math.h> and behave identically on
 * the host (full libc) and the freestanding native target.
 */

#ifndef BLYT_SHARED_FP_CANON_H
#define BLYT_SHARED_FP_CANON_H

#include <stdint.h>

/* Canonical quiet-NaN bit patterns (ADR-0010): positive quiet NaN, zero
 * mantissa payload.  f32 matches the RISC-V canonical NaN (also x86/ARM
 * default); f64 matches rv32emu's RV_NAN_D. */
#define BLYT_F32_CANONICAL_NAN UINT32_C(0x7FC00000)
#define BLYT_F64_CANONICAL_NAN UINT64_C(0x7FF8000000000000)

/* Return the canonical f32 NaN bits if `bits` encodes any NaN (exponent all
 * ones, nonzero mantissa); otherwise return `bits` unchanged. */
uint32_t blyt_canon_f32(uint32_t bits);

/* f64 counterpart of blyt_canon_f32. */
uint64_t blyt_canon_f64(uint64_t bits);

#endif /* BLYT_SHARED_FP_CANON_H */
