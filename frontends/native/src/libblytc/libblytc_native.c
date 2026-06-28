/* libblytc_native.c — Native-path libblytc.so.
 *
 * On the native RISC-V execution path most of the C library comes from the
 * system musl interpreter (ld-blyt.so.1) via a DT_NEEDED entry: memcpy, snprintf,
 * strtol, getenv, ... all resolve there at runtime.  The DT_NEEDED entry for
 * ld-blyt.so.1 is injected at link time via -Wl,--no-as-needed applied to the
 * ld-blyt.so.1 linker stub.
 *
 * The one exception is the cart heap (#158, ADR-0008). The cart-visible
 * allocator (malloc/free/realloc/calloc/aligned_alloc) is defined HERE — backed
 * by the single runtime/shared arena hosted in libblytcommon.so — instead of
 * delegating to musl, so a native cart and a desktop (emulated) cart account
 * guest_heap_used identically and fail an allocation at the same logical point
 * against the unified 16 MB budget (required for save-state / replay / netplay
 * parity). Because libblytc.so is earlier than ld-blyt.so.1 in the cart's
 * DT_NEEDED search order, these definitions interpose musl's malloc family for
 * the whole cart process — the same single-allocator arrangement the emulated
 * path has. The arena state + accounting live in libblytcommon (resolved over
 * the DT_NEEDED entry added at link time); the resource cache there uses a
 * separate raw allocator, so its bytes never enter guest_heap_used.
 */

#include <stddef.h>

/* Cart-heap arena entry points, exported by libblytcommon.so (#158). */
extern void *blyt_cart_heap_malloc(size_t n);
extern void blyt_cart_heap_free(void *p);
extern void *blyt_cart_heap_realloc(void *p, size_t n);
extern void *blyt_cart_heap_calloc(size_t nmemb, size_t sz);
extern void *blyt_cart_heap_aligned_alloc(size_t alignment, size_t size);

const int blytc_native_version = 1;

void *malloc(size_t n) {
    return blyt_cart_heap_malloc(n);
}

void free(void *p) {
    blyt_cart_heap_free(p);
}

void *realloc(void *p, size_t n) {
    return blyt_cart_heap_realloc(p, n);
}

void *calloc(size_t nmemb, size_t sz) {
    return blyt_cart_heap_calloc(nmemb, sz);
}

void *aligned_alloc(size_t alignment, size_t size) {
    return blyt_cart_heap_aligned_alloc(alignment, size);
}
