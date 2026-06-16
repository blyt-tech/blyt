/*
 * Swift Embedded runtime glue for the blyt sysroot.
 *
 * __lshrdi3: Swift Embedded emits a call to this compiler_rt helper when a
 * 64-bit right shift is used in Swift code targeting a 32-bit platform.
 * libblyt32.so does not export compiler_rt integer helpers (only float-soft
 * ABI helpers like __floatsidf). This is part of FV-5.
 */
typedef unsigned long long uint64_impl_t;
uint64_impl_t __lshrdi3(uint64_impl_t a, int b) {
    return a >> (unsigned)b;
}

/*
 *
 * FV-5 (Spike V): Embedded Swift's allocator emits references to
 * `posix_memalign` and the GCC stack-protector symbols
 * (`__stack_chk_guard`, `__stack_chk_fail`) that are not in libblyt32.so's
 * dynamic export set.  musl implements all three, but libblyt32.so's build
 * currently does not re-export them.
 *
 * For the spike this file provides minimal implementations bridged to what
 * libblyt32.so *does* export.  A production integration would either:
 *   a) add these three symbols to libblyt32.so's musl-derived export set, or
 *   b) ship a per-language libswift32.so sidecar (analogous to libc++ for C++).
 *
 * Classification: leaked assumption — blyt32 musl symbol set assumed to cover
 * all libc needs; Swift Embedded's allocator pattern differs from C/C++/Rust.
 */

#include <stddef.h>

/* Stack-protector canary.  musl seeds this from /dev/urandom at startup;
 * for embedded use a constant is sufficient (no secrets in the cart process). */
long __stack_chk_guard = (long)0xDEADBEEFCAFEBABELL;

/* Stack-protector failure: call the platform abort equivalent. */
__attribute__((noreturn)) void __stack_chk_fail(void) {
    __builtin_trap();
}

/* Swift's allocator uses posix_memalign for over-aligned allocations.
 * Redirect to aligned_alloc which musl exports through libblyt32.so.
 * Returns 0 on success, ENOMEM (12) on allocation failure, EINVAL (22) for
 * bad alignment.  The aligned_alloc interface requires size to be a multiple
 * of alignment; posix_memalign does not, so pad size up. */
extern void *aligned_alloc(size_t alignment, size_t size);

int posix_memalign(void **memptr, size_t alignment, size_t size) {
    if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0)
        return 22; /* EINVAL */
    /* Round size up to a multiple of alignment (posix_memalign requirement
     * vs aligned_alloc which requires size % alignment == 0). */
    size_t padded = (size + alignment - 1) & ~(alignment - 1);
    void *p = aligned_alloc(alignment, padded);
    if (!p)
        return 12; /* ENOMEM */
    *memptr = p;
    return 0;
}
