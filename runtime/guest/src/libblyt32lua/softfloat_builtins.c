/*
 * runtime/guest/src/libblyt32lua/softfloat_builtins.c
 *
 * Compiler-rt ABI wrappers for Berkeley SoftFloat.
 *
 * On riscv32-linux-gnu with clang, `long double` is 128-bit quad precision
 * (f128) and there is no hardware support.  The compiler generates calls to
 * these __tf3/__df3 routines whenever `long double` or `double` arithmetic
 * appears in cross-compiled code — including musl's strtod/floatscan and
 * Lua's number↔string conversion (LUAI_UACNUMBER=double with LUA_32BITS=1).
 *
 * Calling conventions on RISC-V ilp32 (verified from compiler output):
 *
 *   double (__df3):
 *     Arguments passed by value in register pairs: a in a0:a1, b in a2:a3.
 *     Return value in a0:a1.
 *
 *   long double (__tf3):
 *     Large aggregate return: caller passes hidden result pointer in a0.
 *     'a' pointer in a1, 'b' pointer in a2.
 *     Function stores result at *a0, returns void.
 *     → Declare as: void __addtf3(f128*, const f128*, const f128*)
 *
 * This file is compiled alongside the SoftFloat source (cross-compiled for
 * RV32 in cmake Step 2c) and linked into libblyt32lua.so.
 */

#include <stdint.h>
#include <string.h>

/* SoftFloat public API (included via -I during build) */
#include "softfloat.h"

/* -------------------------------------------------------------------------
 * Helpers: bitcast double ↔ float64_t
 * float64_t is typedef uint64_t in SoftFloat; double is 64-bit IEEE 754.
 * On RISC-V ilp32, both are passed in a0:a1.  The union avoids UB.
 * ------------------------------------------------------------------------- */

typedef union {
    double d;
    float64_t u;
} f64_u;
typedef union {
    float f;
    float32_t u;
} f32_u;

static inline float64_t d2u(double d) {
    f64_u u;
    u.d = d;
    return u.u;
}
static inline double u2d(float64_t u) {
    f64_u v;
    v.u = u;
    return v.d;
}
static inline float32_t f2u(float f) {
    f32_u u;
    u.f = f;
    return u.u;
}
static inline float u2f(float32_t u) {
    f32_u v;
    v.u = u;
    return v.f;
}

/* float128_t = struct{uint64_t v[2]} — same ABI as the compiler's long double
 * when passed via pointer (verified from generated assembly). */

/* -------------------------------------------------------------------------
 * Double (64-bit) arithmetic
 * ------------------------------------------------------------------------- */

double __adddf3(double a, double b) {
    return u2d(f64_add(d2u(a), d2u(b)));
}
double __subdf3(double a, double b) {
    return u2d(f64_sub(d2u(a), d2u(b)));
}
double __muldf3(double a, double b) {
    return u2d(f64_mul(d2u(a), d2u(b)));
}
double __divdf3(double a, double b) {
    return u2d(f64_div(d2u(a), d2u(b)));
}

/* -------------------------------------------------------------------------
 * Double comparisons — return negative/zero/positive
 * ------------------------------------------------------------------------- */

int __ltdf2(double a, double b) {
    float64_t ua = d2u(a), ub = d2u(b);
    if (f64_lt(ua, ub))
        return -1;
    if (f64_eq(ua, ub))
        return 0;
    return 1;
}
int __ledf2(double a, double b) {
    float64_t ua = d2u(a), ub = d2u(b);
    return f64_le(ua, ub) ? -1 : 1;
}
/* The compiler tests `__gtdf2(a,b) > 0` for `a > b` and `__gedf2(a,b) >= 0`
 * for `a >= b`, so these must return POSITIVE on the high side — not the
 * negative that __ltdf2(b,a)/__ledf2(b,a) yield. NaN (unordered) → negative so
 * the comparison reports false. */
int __gtdf2(double a, double b) {
    float64_t ua = d2u(a), ub = d2u(b);
    if (f64_lt(ub, ua))
        return 1; /* a > b */
    if (f64_eq(ua, ub))
        return 0; /* a == b → not > 0 */
    return -1; /* a < b or unordered */
}
int __gedf2(double a, double b) {
    float64_t ua = d2u(a), ub = d2u(b);
    if (f64_lt(ua, ub))
        return -1; /* a < b */
    if (f64_eq(ua, ub))
        return 0; /* a == b → >= 0 */
    if (f64_lt(ub, ua))
        return 1; /* a > b → > 0 */
    return -1; /* unordered (NaN) → not >= */
}
int __eqdf2(double a, double b) {
    return f64_eq(d2u(a), d2u(b)) ? 0 : 1;
}
int __nedf2(double a, double b) {
    return f64_eq(d2u(a), d2u(b)) ? 0 : 1;
}

/* -------------------------------------------------------------------------
 * Double ↔ integer conversions
 * ------------------------------------------------------------------------- */

int __fixdfsi(double a) {
    return (int)f64_to_i32_r_minMag(d2u(a), 0);
}
long long __fixdfdi(double a) {
    return f64_to_i64_r_minMag(d2u(a), 0);
}
float __truncdfsf2(double a) {
    return u2f(f64_to_f32(d2u(a)));
}

double __floatsidf(int a) {
    return u2d(i32_to_f64(a));
}
double __floatdidf(long long a) {
    return u2d(i64_to_f64(a));
}
double __floatunsidf(unsigned int a) {
    return u2d(ui32_to_f64(a));
}
/* uint64 → double — emitted by the Lua VM under LUA_FLOAT_DOUBLE (Spike U)
 * for unsigned-64-to-double conversions (e.g. size_t/hash → lua_Number). */
double __floatundidf(unsigned long long a) {
    return u2d(ui64_to_f64(a));
}

/* float → double */
double __extendsfdf2(float a) {
    return u2d(f32_to_f64(f2u(a)));
}

/* int64 → float (single precision) */
float __floatdisf(long long a) {
    return u2f(f64_to_f32(i64_to_f64(a)));
}

/* uint64 → float (single precision) — emitted by clang at -O2 for some
 * unsigned-64-to-float conversions in the Lua VM. */
float __floatundisf(unsigned long long a) {
    return u2f(f64_to_f32(ui64_to_f64(a)));
}

/* -------------------------------------------------------------------------
 * Float → int64
 * ------------------------------------------------------------------------- */

long long __fixsfdi(float a) {
    return f64_to_i64_r_minMag(f32_to_f64(f2u(a)), 0);
}

/* -------------------------------------------------------------------------
 * Quad (128-bit) arithmetic — hidden-pointer calling convention.
 * Declared as taking float128_t* so the compiler generates pointer loads.
 * ------------------------------------------------------------------------- */

void __addtf3(float128_t *r, const float128_t *a, const float128_t *b) {
    *r = f128_add(*a, *b);
}
void __subtf3(float128_t *r, const float128_t *a, const float128_t *b) {
    *r = f128_sub(*a, *b);
}
void __multf3(float128_t *r, const float128_t *a, const float128_t *b) {
    *r = f128_mul(*a, *b);
}
void __divtf3(float128_t *r, const float128_t *a, const float128_t *b) {
    *r = f128_div(*a, *b);
}

/* -------------------------------------------------------------------------
 * Quad comparisons — a and b passed by pointer
 * ------------------------------------------------------------------------- */

int __lttf2(const float128_t *a, const float128_t *b) {
    if (f128_lt(*a, *b))
        return -1;
    if (f128_eq(*a, *b))
        return 0;
    return 1;
}
int __letf2(const float128_t *a, const float128_t *b) {
    return f128_le(*a, *b) ? -1 : 1;
}
/* Same high-side-positive convention as __gtdf2/__gedf2 (see note above). */
int __gttf2(const float128_t *a, const float128_t *b) {
    if (f128_lt(*b, *a))
        return 1; /* a > b */
    if (f128_eq(*a, *b))
        return 0; /* a == b → not > 0 */
    return -1; /* a < b or unordered */
}
int __getf2(const float128_t *a, const float128_t *b) {
    if (f128_lt(*a, *b))
        return -1; /* a < b */
    if (f128_eq(*a, *b))
        return 0; /* a == b → >= 0 */
    if (f128_lt(*b, *a))
        return 1; /* a > b → > 0 */
    return -1; /* unordered (NaN) → not >= */
}
int __eqtf2(const float128_t *a, const float128_t *b) {
    return f128_eq(*a, *b) ? 0 : 1;
}
int __netf2(const float128_t *a, const float128_t *b) {
    return f128_eq(*a, *b) ? 0 : 1;
}
int __unordtf2(const float128_t *a, const float128_t *b) {
    /* Unordered iff either operand is NaN; NaN != NaN in IEEE 754 */
    return (!f128_eq(*a, *a) || !f128_eq(*b, *b)) ? 1 : 0;
}

/* -------------------------------------------------------------------------
 * Quad ↔ integer/float conversions — result by pointer, source by value/ptr
 * ------------------------------------------------------------------------- */

/* quad → int */
int __fixtfsi(const float128_t *a) {
    return (int)f128_to_i32_r_minMag(*a, 0);
}
long long __fixtfdi(const float128_t *a) {
    return f128_to_i64_r_minMag(*a, 0);
}
unsigned int __fixunstfsi(const float128_t *a) {
    return f128_to_ui32_r_minMag(*a, 0);
}

/* int → quad */
void __floatsitf(float128_t *r, int a) {
    *r = i32_to_f128(a);
}
void __floatditf(float128_t *r, long long a) {
    *r = i64_to_f128(a);
}
void __floatunsitf(float128_t *r, unsigned int a) {
    *r = ui32_to_f128(a);
}

/* float/double → quad */
void __extendsftf2(float128_t *r, float a) {
    *r = f32_to_f128(f2u(a));
}
void __extenddftf2(float128_t *r, double a) {
    *r = f64_to_f128(d2u(a));
}

/* quad → float/double */
float __trunctfsf2(const float128_t *a) {
    return u2f(f128_to_f32(*a));
}
double __trunctfdf2(const float128_t *a) {
    return u2d(f128_to_f64(*a));
}

/* -------------------------------------------------------------------------
 * 64-bit integer division (no floating point)
 * ------------------------------------------------------------------------- */

/* 64-bit unsigned division using shift-subtract — no recursion, only
 * 32-bit arithmetic which the compiler handles inline on RV32+M. */
unsigned long long __udivdi3(unsigned long long a, unsigned long long b) {
    if (b == 0)
        return 0;
    unsigned long long q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((a >> i) & 1);
        if (r >= b) {
            r -= b;
            q |= (1ULL << i);
        }
    }
    return q;
}

unsigned long long __umoddi3(unsigned long long a, unsigned long long b) {
    if (b == 0)
        return 0;
    unsigned long long r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((a >> i) & 1);
        if (r >= b)
            r -= b;
    }
    return r;
}

/* -------------------------------------------------------------------------
 * Floating-point environment stubs (no hardware FP exception support)
 * The rv32emu FCSR is managed directly by the emulator; these stubs satisfy
 * linker references from musl code that we don't call at runtime.
 * ------------------------------------------------------------------------- */

int feclearexcept(int e) {
    (void)e;
    return 0;
}
int fegetround(void) {
    return 0; /* FE_TONEAREST */
}
int feraiseexcept(int e) {
    (void)e;
    return 0;
}
int fesetround(int r) {
    (void)r;
    return 0;
}
int fetestexcept(int e) {
    (void)e;
    return 0;
}

/* -------------------------------------------------------------------------
 * Wide character stub
 * ------------------------------------------------------------------------- */

int wctomb(char *s, unsigned int wc) {
    (void)s;
    (void)wc;
    return 0;
}
