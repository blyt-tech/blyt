/*
 * runtime/shared/blyt_fpm_soft.c — Zone-2 kernels for the host-Lua FP seam
 * (ADR-0135). Each entry bitcasts the uint64_t argument to double, calls the
 * in-house blyt-tech musl generic-C kernel, and bitcasts the result back.
 *
 * `sin`/`cos`/… here resolve to the blyt-tech musl `src/math/*.c` objects that
 * the host-Lua build compiles in (see frontends/wasm/CMakeLists.txt), NOT the
 * host toolchain's libm: those object definitions are strong symbols that
 * override the lazily-linked host libm archive, so the whole host-Lua VM uses the
 * pinned musl kernels. The argument is always a runtime value (a Lua stack
 * number bitcast through u2d), never a compile-time constant, so the compiler
 * cannot constant-fold these calls with its own libm; `-fno-builtin` on this TU
 * keeps that guarantee explicit.
 *
 * On WASM the double ops inside the musl kernels are native f64 — equal to the
 * Berkeley-SoftFloat reference on WASM MVP (no scalar FMA), proven by the #223
 * parity gate. A future native host-Lua leg would compile the musl kernels
 * `-msoft-float` so the same uint64_t boundary hides softfloat lowering; this TU
 * does not change.
 */
#include "blyt_fpm.h"

#include <math.h>

uint64_t blyt_fpm_sin(uint64_t x) {
    return blyt_fpm_d2u(sin(blyt_fpm_u2d(x)));
}
uint64_t blyt_fpm_cos(uint64_t x) {
    return blyt_fpm_d2u(cos(blyt_fpm_u2d(x)));
}
uint64_t blyt_fpm_tan(uint64_t x) {
    return blyt_fpm_d2u(tan(blyt_fpm_u2d(x)));
}
uint64_t blyt_fpm_asin(uint64_t x) {
    return blyt_fpm_d2u(asin(blyt_fpm_u2d(x)));
}
uint64_t blyt_fpm_acos(uint64_t x) {
    return blyt_fpm_d2u(acos(blyt_fpm_u2d(x)));
}
uint64_t blyt_fpm_atan(uint64_t x) {
    return blyt_fpm_d2u(atan(blyt_fpm_u2d(x)));
}
uint64_t blyt_fpm_exp(uint64_t x) {
    return blyt_fpm_d2u(exp(blyt_fpm_u2d(x)));
}
uint64_t blyt_fpm_log(uint64_t x) {
    return blyt_fpm_d2u(log(blyt_fpm_u2d(x)));
}
uint64_t blyt_fpm_log2(uint64_t x) {
    return blyt_fpm_d2u(log2(blyt_fpm_u2d(x)));
}
uint64_t blyt_fpm_log10(uint64_t x) {
    return blyt_fpm_d2u(log10(blyt_fpm_u2d(x)));
}
uint64_t blyt_fpm_atan2(uint64_t y, uint64_t x) {
    return blyt_fpm_d2u(atan2(blyt_fpm_u2d(y), blyt_fpm_u2d(x)));
}
uint64_t blyt_fpm_pow(uint64_t x, uint64_t y) {
    return blyt_fpm_d2u(pow(blyt_fpm_u2d(x), blyt_fpm_u2d(y)));
}
