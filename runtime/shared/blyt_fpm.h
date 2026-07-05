/*
 * runtime/shared/blyt_fpm.h — host-Lua floating-point math seam (ADR-0135).
 *
 * The emulated / native-metal Lua path computes transcendentals with blyt-tech
 * musl's generic-C src/math kernels over Berkeley SoftFloat (RISC-V NaN
 * specialization + SOFTFLOAT_ROUND_ODD, ADR-0132) — a bit-exact deterministic
 * reference (ADR-0007). The host-Lua fast path (Lua compiled natively for the
 * host; WASM today) must reproduce that reference bit-for-bit; otherwise its
 * transcendentals resolve to whatever libm the host toolchain ships (Emscripten's
 * bundled musl today — a coincidental, unpinned agreement, blyt#223).
 *
 * This seam is the single choke point. Zone-2 composite ops (transcendentals and
 * `pow`, i.e. Lua `^`) route through blyt_fpm_* to the SAME blyt-tech musl
 * generic-C kernels, linked into the host-Lua VM instead of the host libm. Zone-1
 * basic ops (+ - * / sqrt, compares, round-to-int, fmod) are IEEE-mandated
 * correctly-rounded, so native == softfloat bit-for-bit given `-ffp-contract=off`
 * (WASM MVP has no scalar FMA); they stay native and are NOT part of this seam.
 *
 * Doubles cross the seam as uint64_t bit patterns: a uint64_t passes in integer
 * registers under every ABI, so a future native host (x86-64/arm64 with hardware
 * FMA) can soft-float-lower the Zone-2 kernels internally without colliding with
 * the caller's native-f64 ABI (the d2u/u2d pattern from softfloat_builtins.c,
 * promoted to a module boundary). On WASM the bitcast is a no-op and the kernels
 * use native f64 — which the #223 parity gate proves equals the softfloat
 * reference on WASM MVP (see ADR-0135: WASM realizes Mode A via pinned kernels +
 * native f64; true `-msoft-float` lowering is deferred to the native host-Lua
 * leg, where it is both feasible and necessary).
 *
 * Scope note (blyt#223 Phase A): the transcendental + `pow` surface routes here.
 * strtod / number-format (`lua_str2number` / `lua_number2str`) is the other
 * Zone-2 surface (ADR-0135); pinning it to musl on WASM requires a renamed musl
 * stdio/stdlib subset (musl's strtod/printf would otherwise override the whole
 * module's libc), tracked as Phase B.
 */
#ifndef BLYT_FPM_H
#define BLYT_FPM_H

#include <stdint.h>
#include <string.h>

/* Bit-pattern <-> double bitcasts. Pure moves (no FP op), so they are ABI-safe
 * even when the caller is compiled native and the kernel is soft-float-lowered. */
static inline uint64_t blyt_fpm_d2u(double d) {
    uint64_t u;
    memcpy(&u, &d, sizeof u);
    return u;
}
static inline double blyt_fpm_u2d(uint64_t u) {
    double d;
    memcpy(&d, &u, sizeof d);
    return d;
}

/* ── Zone-2 seam: uint64_t bit-pattern interface (implemented in
 * blyt_fpm_soft.c over the in-house blyt-tech musl kernels). ────────────────── */
uint64_t blyt_fpm_sin(uint64_t x);
uint64_t blyt_fpm_cos(uint64_t x);
uint64_t blyt_fpm_tan(uint64_t x);
uint64_t blyt_fpm_asin(uint64_t x);
uint64_t blyt_fpm_acos(uint64_t x);
uint64_t blyt_fpm_atan(uint64_t x);
uint64_t blyt_fpm_exp(uint64_t x);
uint64_t blyt_fpm_log(uint64_t x);
uint64_t blyt_fpm_log2(uint64_t x);
uint64_t blyt_fpm_log10(uint64_t x);
uint64_t blyt_fpm_atan2(uint64_t y, uint64_t x);
uint64_t blyt_fpm_pow(uint64_t x, uint64_t y); /* Lua `^` */

/* ── Convenience double wrappers for the Lua VM call sites. Pure bitcasts around
 * the seam; kept inline so the VM sources read naturally. ───────────────────── */
static inline double blyt_fpm_sind(double x) {
    return blyt_fpm_u2d(blyt_fpm_sin(blyt_fpm_d2u(x)));
}
static inline double blyt_fpm_cosd(double x) {
    return blyt_fpm_u2d(blyt_fpm_cos(blyt_fpm_d2u(x)));
}
static inline double blyt_fpm_tand(double x) {
    return blyt_fpm_u2d(blyt_fpm_tan(blyt_fpm_d2u(x)));
}
static inline double blyt_fpm_asind(double x) {
    return blyt_fpm_u2d(blyt_fpm_asin(blyt_fpm_d2u(x)));
}
static inline double blyt_fpm_acosd(double x) {
    return blyt_fpm_u2d(blyt_fpm_acos(blyt_fpm_d2u(x)));
}
static inline double blyt_fpm_atand(double x) {
    return blyt_fpm_u2d(blyt_fpm_atan(blyt_fpm_d2u(x)));
}
static inline double blyt_fpm_expd(double x) {
    return blyt_fpm_u2d(blyt_fpm_exp(blyt_fpm_d2u(x)));
}
static inline double blyt_fpm_logd(double x) {
    return blyt_fpm_u2d(blyt_fpm_log(blyt_fpm_d2u(x)));
}
static inline double blyt_fpm_log2d(double x) {
    return blyt_fpm_u2d(blyt_fpm_log2(blyt_fpm_d2u(x)));
}
static inline double blyt_fpm_log10d(double x) {
    return blyt_fpm_u2d(blyt_fpm_log10(blyt_fpm_d2u(x)));
}
static inline double blyt_fpm_atan2d(double y, double x) {
    return blyt_fpm_u2d(blyt_fpm_atan2(blyt_fpm_d2u(y), blyt_fpm_d2u(x)));
}
static inline double blyt_fpm_powd(double x, double y) {
    return blyt_fpm_u2d(blyt_fpm_pow(blyt_fpm_d2u(x), blyt_fpm_d2u(y)));
}

#endif /* BLYT_FPM_H */
