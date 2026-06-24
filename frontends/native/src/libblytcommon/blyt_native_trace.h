/*
 * blyt_native_trace.h — BLYT_TRACE "api" channel emit helpers, native path.
 *
 * Mirrors the host trace module's line format ("[blyt:api] <msg>") minus the
 * frame/time counters — runtime/host trace code cannot run inside the seccomp'd
 * ILP32 cart process.  getenv() comes from ld-blyt.so.1; all formatting is
 * hand-rolled (no snprintf in these libraries) and emission is a raw SYS_write
 * to fd 2 (write(2) is in the restricted seccomp allowlist).  f32 values print
 * as raw hex bits.
 *
 * Factored out of the native libblytcommon impls (blytcommon.c) as static
 * helpers (mirroring seccomp_restricted.h); unused helpers in a TU are dropped
 * without warning (static inline).  See issue #128.
 */

#ifndef BLYT_NATIVE_TRACE_H
#define BLYT_NATIVE_TRACE_H

#include <stdint.h>

/* Provided at runtime by ld-blyt.so.1 (system musl). */
extern char *getenv(const char *name);

static inline void blyt32_trace_write(const char *buf, unsigned int len) {
    register long a0 __asm__("a0") = 2; /* STDERR_FILENO */
    register const char *a1 __asm__("a1") = buf;
    register long a2 __asm__("a2") = (long)len;
    register long a7 __asm__("a7") = 64; /* SYS_write */
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
}

/* 1 when BLYT_TRACE contains the "api" (or "all") channel token. */
static inline int blyt32_trace_api_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        cached = 0;
        const char *p = getenv("BLYT_TRACE");
        while (p && *p) {
            if (((p[0] == 'a' && p[1] == 'p' && p[2] == 'i') ||
                 (p[0] == 'a' && p[1] == 'l' && p[2] == 'l')) &&
                (p[3] == '\0' || p[3] == ',')) {
                cached = 1;
                break;
            }
            while (*p && *p != ',')
                p++;
            if (*p)
                p++;
        }
    }
    return cached;
}

static inline char *blyt32_trace_app_str(char *p, char *end, const char *s) {
    while (*s && p + 1 < end)
        *p++ = *s++;
    return p;
}

static inline char *blyt32_trace_app_dec(char *p, char *end, long v) {
    char num[16];
    int ni = 0;
    unsigned long u = (v < 0) ? (unsigned long)-v : (unsigned long)v;
    if (v < 0 && p + 1 < end)
        *p++ = '-';
    do {
        num[ni++] = (char)('0' + (int)(u % 10u));
        u /= 10u;
    } while (u && ni < (int)sizeof(num));
    for (int j = ni - 1; j >= 0 && p + 1 < end; j--)
        *p++ = num[j];
    return p;
}

static inline char *blyt32_trace_app_hex(char *p, char *end, uint32_t v) {
    p = blyt32_trace_app_str(p, end, "0x");
    int started = 0;
    for (int shift = 28; shift >= 0; shift -= 4) {
        unsigned int nyb = (v >> shift) & 0xFu;
        if (!nyb && !started && shift)
            continue;
        started = 1;
        if (p + 1 < end)
            *p++ = "0123456789abcdef"[nyb];
    }
    return p;
}

static inline void blyt32_trace_emit(char *buf, char *p, char *end) {
    if (p + 1 < end)
        *p++ = '\n';
    blyt32_trace_write(buf, (unsigned int)(p - buf));
}

/* One line per typed buffer get/set: "[blyt:api] <name>(buf=…, slot=…,
 * field=…[, v=…])[ -> …]".  f32 bits are printed as hex. */
static inline void blyt32_trace_buf_op(const char *name, uint32_t b, int32_t s, uint32_t f,
                                       uint32_t bits, int is_set, int is_f32) {
    if (!blyt32_trace_api_enabled())
        return;
    char buf[160];
    char *end = buf + sizeof(buf);
    char *p = blyt32_trace_app_str(buf, end, "[blyt:api] ");
    p = blyt32_trace_app_str(p, end, name);
    p = blyt32_trace_app_str(p, end, "(buf=");
    p = blyt32_trace_app_dec(p, end, (long)b);
    p = blyt32_trace_app_str(p, end, ", slot=");
    p = blyt32_trace_app_dec(p, end, (long)s);
    p = blyt32_trace_app_str(p, end, ", field=");
    p = blyt32_trace_app_dec(p, end, (long)(f & 0xFFFFu));
    p = blyt32_trace_app_str(p, end, is_set ? ", v=" : ") -> ");
    if (is_f32)
        p = blyt32_trace_app_hex(p, end, bits);
    else
        p = blyt32_trace_app_dec(p, end, (long)(int32_t)bits);
    if (is_set)
        p = blyt32_trace_app_str(p, end, ")");
    blyt32_trace_emit(buf, p, end);
}

/* Simple "<name>(arg) [-> ret]" line for the non-buffer APIs. */
static inline void blyt32_trace_call(const char *name, long arg, int has_ret, long ret) {
    if (!blyt32_trace_api_enabled())
        return;
    char buf[128];
    char *end = buf + sizeof(buf);
    char *p = blyt32_trace_app_str(buf, end, "[blyt:api] ");
    p = blyt32_trace_app_str(p, end, name);
    p = blyt32_trace_app_str(p, end, "(");
    p = blyt32_trace_app_dec(p, end, arg);
    p = blyt32_trace_app_str(p, end, ")");
    if (has_ret) {
        p = blyt32_trace_app_str(p, end, " -> ");
        p = blyt32_trace_app_dec(p, end, ret);
    }
    blyt32_trace_emit(buf, p, end);
}

#endif /* BLYT_NATIVE_TRACE_H */
