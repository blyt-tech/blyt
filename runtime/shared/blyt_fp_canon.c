/* blyt_fp_canon.c — see blyt_fp_canon.h.  Freestanding (runtime/shared). */

#include "blyt_fp_canon.h"

uint32_t blyt_canon_f32(uint32_t bits) {
    /* NaN iff exponent (bits 30:23) is all ones and the mantissa is nonzero. */
    if ((bits & UINT32_C(0x7F800000)) == UINT32_C(0x7F800000) &&
        (bits & UINT32_C(0x007FFFFF)) != 0u)
        return BLYT_F32_CANONICAL_NAN;
    return bits;
}

uint64_t blyt_canon_f64(uint64_t bits) {
    /* NaN iff exponent (bits 62:52) is all ones and the mantissa is nonzero. */
    if ((bits & UINT64_C(0x7FF0000000000000)) == UINT64_C(0x7FF0000000000000) &&
        (bits & UINT64_C(0x000FFFFFFFFFFFFF)) != 0u)
        return BLYT_F64_CANONICAL_NAN;
    return bits;
}
