/*
 * lua_native_malloc.c — mmap-based heap for the native libblyt32lua.so build.
 *
 * The native ld.so (/lib/ld-blyt.so.1) is a minimal ILP32F interpreter: it
 * exposes __libc_realloc / __libc_free etc. under hidden-visibility internal
 * names only.  Standard "malloc" / "free" / "realloc" are not present in its
 * .dynsym.  This file provides them without conflicting with the ld.so's own
 * heap: each allocation is an independent anonymous mmap; the ld.so's heap
 * (brk-based) is never touched, so free() here never corrupts the ld.so.
 *
 * All four functions (malloc / free / realloc / calloc) are consistent within
 * this translation unit.  Lua's VM drives all allocations through a single
 * realloc-based allocator function; no cross-heap mixing occurs.
 *
 * Layout:  [ 16-byte header (stores total mmap size) | user data ... ]
 *
 * Syscalls used (both are in the seccomp allowlist blytal_rv32_nrs[]):
 *   SYS_mmap   = 222
 *   SYS_munmap = 215
 */

#include <stddef.h>
#include <stdint.h>

#define LUA_ALLOC_HDR 16u /* keeps user data 16-byte aligned on rv32 */

/* rv32 Linux syscall helpers — must not call any PLT symbol. */

static void *blyt_mmap(size_t len) {
    register long a0 __asm__("a0") = 0; /* addr   = NULL            */
    register long a1 __asm__("a1") = (long)len;
    register long a2 __asm__("a2") = 3; /* PROT_READ | PROT_WRITE   */
    register long a3 __asm__("a3") = 34; /* MAP_PRIVATE | MAP_ANON   */
    register long a4 __asm__("a4") = -1; /* fd = -1 (anonymous)      */
    register long a5 __asm__("a5") = 0; /* offset = 0               */
    register long a7 __asm__("a7") = 222; /* SYS_mmap                 */
    __asm__ volatile("ecall"
                     : "+r"(a0)
                     : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a7)
                     : "memory");
    /* kernel returns -errno on failure (large unsigned value) */
    if ((unsigned long)(long)a0 > (unsigned long)-4096UL)
        return (void *)0;
    return (void *)(long)a0;
}

static void blyt_munmap(void *ptr, size_t len) {
    register long a0 __asm__("a0") = (long)ptr;
    register long a1 __asm__("a1") = (long)len;
    register long a7 __asm__("a7") = 215; /* SYS_munmap               */
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
}

void *malloc(size_t n) {
    if (!n)
        n = 1;
    size_t total = n + LUA_ALLOC_HDR;
    void *m = blyt_mmap(total);
    if (!m)
        return (void *)0;
    *(size_t *)m = total;
    return (char *)m + LUA_ALLOC_HDR;
}

void free(void *p) {
    if (!p)
        return;
    char *m = (char *)p - LUA_ALLOC_HDR;
    size_t total = *(size_t *)m;
    blyt_munmap(m, total);
}

void *realloc(void *p, size_t n) {
    if (!p)
        return malloc(n);
    if (!n) {
        free(p);
        return (void *)0;
    }

    char *m = (char *)p - LUA_ALLOC_HDR;
    size_t old_total = *(size_t *)m;
    size_t old_cap = old_total - LUA_ALLOC_HDR;

    if (n <= old_cap)
        return p; /* already fits */

    void *q = malloc(n);
    if (!q)
        return (void *)0;

    /* manual copy — avoid PLT call to memcpy which may be unresolved here */
    {
        char *dst = (char *)q;
        const char *src = (const char *)p;
        size_t i;
        for (i = 0; i < old_cap; i++)
            dst[i] = src[i];
    }
    free(p);
    return q;
}

void *calloc(size_t nmemb, size_t sz) {
    if (sz && nmemb > (size_t)-1 / sz)
        return (void *)0;
    size_t total = nmemb * sz;
    void *p = malloc(total);
    if (p) {
        char *c = (char *)p;
        size_t i;
        for (i = 0; i < total; i++)
            c[i] = 0;
    }
    return p;
}

void *aligned_alloc(size_t alignment, size_t size) {
    /* All our allocations are already LUA_ALLOC_HDR (16-byte) aligned.
     * Larger alignment is unsupported; return NULL for those requests. */
    if (alignment > LUA_ALLOC_HDR)
        return (void *)0;
    return malloc(size);
}
