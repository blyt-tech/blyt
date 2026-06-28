/*
 * lua_native_malloc.c — cart-heap allocator for the native libblyt32lua.so build.
 *
 * The native ld.so (/lib/ld-blyt.so.1) does not export "malloc"/"free"/... under
 * standard names in its .dynsym, so libblyt32lua.so must define them itself for
 * the embedded Lua VM and the curated musl subset (libblytc_native.o) linked
 * beside it.
 *
 * #158 (ADR-0008): these route to the single runtime/shared cart-heap arena
 * hosted in libblytcommon.so (the same arena the native libblytc and the
 * emulated libblytc use), so a Lua cart's heap is accounted in guest_heap_used
 * byte-identically across wasm32/rv32 and bounded by the unified 16 MB budget —
 * not on a private mmap heap as before. The arena entry points resolve from
 * libblytcommon over the DT_NEEDED chain (libblyt32lua.so → libblyt32.so →
 * libblytcommon.so). The resource cache there uses a separate raw allocator, so
 * resource bytes never enter the cart heap total.
 */

#include <stddef.h>

/* Cart-heap arena entry points, exported by libblytcommon.so (#158). */
extern void *blyt_cart_heap_malloc(size_t n);
extern void blyt_cart_heap_free(void *p);
extern void *blyt_cart_heap_realloc(void *p, size_t n);
extern void *blyt_cart_heap_calloc(size_t nmemb, size_t sz);
extern void *blyt_cart_heap_aligned_alloc(size_t alignment, size_t size);

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
