/*
 * lua_native_stubs.c — symbol stubs for the native libblyt32lua.so build.
 *
 * The native build embeds curated musl string/math/ctype/snprintf sources via
 * a partial link object (libblytc_native.o) and an mmap-based allocator via
 * lua_native_malloc.c.  This file provides:
 *
 *   - musl-internal symbols referenced by those embedded sources
 *     (__libc, __errno_location, __lctrans_cur, FILE locking, etc.)
 *   - stdio stubs (fopen/fclose/fprintf/stderr/…): Lua's file-I/O paths are
 *     excluded from the sandboxed cart build, so these are dead code paths;
 *     they exist only to satisfy link-time references from lauxlib.c etc.
 *   - Excluded Lua standard-library openers (luaopen_io/os/debug/package/utf8)
 *
 * abort() and __syscall_ret() use the native rv32 SYS_exit_group syscall
 * (not BLYT_ECALL_EXIT) so the cart terminates cleanly under seccomp.
 */

#include <stddef.h>
#include <stdint.h>

/* =========================================================================
 * musl internal plumbing (mirrors blytc_stubs.c — adapted for native exec)
 * ========================================================================= */

/* musl's src/internal/libc.h uses `#define libc __libc`; all musl sources
 * that include libc.h reference this global.  Layout must match struct __libc
 * in third_party/musl/src/internal/libc.h exactly. */

struct __locale_map;
struct __locale_struct {
    const struct __locale_map *cat[6];
};
struct tls_module {
    struct tls_module *next;
    void *image;
    size_t len, size, align, offset;
};
struct __libc {
    char can_do_threads;
    char threaded;
    char secure;
    volatile signed char need_locks;
    int threads_minus_1;
    size_t *auxv;
    struct tls_module *tls_head;
    size_t tls_size, tls_align, tls_cnt;
    size_t page_size;
    struct __locale_struct global_locale;
};

struct __libc __libc = {
    .can_do_threads = 0,
    .threaded = 0,
    .page_size = 4096,
};

/* ---- errno ---- */

static int g_errno;

int *__errno_location(void) {
    return &g_errno;
}
int *___errno_location(void) {
    return __errno_location();
}

/* ---- locale translation (C locale: identity) ---- */

const char *__lctrans_cur(const char *msg) {
    return msg;
}

/* ---- FILE locking (single-threaded cart: all no-ops) ---- */

/* forward-declare FILE using the musl type so we don't need stdio_impl.h */
typedef struct _IO_FILE FILE;

int __lockfile(FILE *f) {
    (void)f;
    return 0;
}
void __unlockfile(FILE *f) {
    (void)f;
}

/* ---- open-file list (fmemopen path; we never track open files) ---- */

FILE *__ofl_add(FILE *f) {
    return f;
}

/* ---- stdio exit hook (referenced by __towrite; cart has no atexit) ---- */

void __stdio_exit_needed(void) {
}

/* ---- abort / syscall safety net ---- */

/* SYS_exit_group on rv32 Linux = 94 */
static _Noreturn void blyt_native_exit(void) {
#ifdef __riscv
    register long a7 __asm__("a7") = 94; /* SYS_exit_group */
    register long a0 __asm__("a0") = 1; /* exit code      */
    __asm__ volatile("ecall" : : "r"(a7), "r"(a0) : "memory");
#endif
    __builtin_unreachable();
}

void abort(void) {
    blyt_native_exit();
}

long __syscall_ret(unsigned long r) {
    (void)r;
    blyt_native_exit();
}

/* ---- wide-character stubs (vfprintf %ls/%lc, C locale only) ---- */

typedef unsigned int blyt_wchar_t;
typedef unsigned int blyt_mbstate_t;

size_t mbrtowc(blyt_wchar_t *restrict pwc, const char *restrict s, size_t n,
               blyt_mbstate_t *restrict ps) {
    (void)ps;
    if (!s)
        return 0;
    if (!n)
        return (size_t)-2;
    unsigned char c = (unsigned char)*s;
    if (c == 0) {
        if (pwc)
            *pwc = 0;
        return 0;
    }
    if (c < 0x80) {
        if (pwc)
            *pwc = c;
        return 1;
    }
    return (size_t)-1;
}

size_t wcrtomb(char *restrict s, blyt_wchar_t wc, blyt_mbstate_t *restrict ps) {
    (void)ps;
    if (!s)
        return 1;
    if ((unsigned int)wc < 0x80u) {
        *s = (char)wc;
        return 1;
    }
    *s = '?';
    return 1;
}

int mbsinit(const blyt_mbstate_t *ps) {
    (void)ps;
    return 1;
}

/* =========================================================================
 * locale / string stubs
 * ========================================================================= */

#include <locale.h>

static char s_dp[] = ".";
static char s_empty[] = "";

static struct lconv s_lconv = {
    .decimal_point = s_dp,
    .thousands_sep = s_empty,
    .grouping = s_empty,
    .int_curr_symbol = s_empty,
    .currency_symbol = s_empty,
    .mon_decimal_point = s_empty,
    .mon_thousands_sep = s_empty,
    .mon_grouping = s_empty,
    .positive_sign = s_empty,
    .negative_sign = s_empty,
    .frac_digits = 127,
    .p_cs_precedes = 127,
    .n_cs_precedes = 127,
    .p_sep_by_space = 127,
    .n_sep_by_space = 127,
    .p_sign_posn = 127,
    .n_sign_posn = 127,
    .int_frac_digits = 127,
    .int_p_cs_precedes = 127,
    .int_n_cs_precedes = 127,
    .int_p_sep_by_space = 127,
    .int_n_sep_by_space = 127,
    .int_p_sign_posn = 127,
    .int_n_sign_posn = 127,
};

struct lconv *localeconv(void) {
    return &s_lconv;
}

static char s_strerror[] = "error";
char *strerror(int e) {
    (void)e;
    return s_strerror;
}

int strcoll(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* =========================================================================
 * time stub (Lua uses time(NULL) for random seeding only)
 * ========================================================================= */

typedef long blyt_time_t;
blyt_time_t time(blyt_time_t *t) {
    if (t)
        *t = 0;
    return 0;
}

/* =========================================================================
 * stdio stubs
 *
 * Lua's file-I/O paths (lauxlib.c fopen/fclose/…) are dead code in
 * sandboxed carts.  Non-NULL stdin/stdout/stderr avoid NULL-pointer checks
 * inside Lua.  fprintf routes to blyt_console_debug so panic messages reach
 * the cart console; all other operations silently succeed or return EOF.
 *
 * NOTE: snprintf is NOT stubbed here; it comes from musl snprintf.c compiled
 * into libblytc_native.o (see cmake/blyt_sdk.cmake native build section).
 *
 * The signatures use void * in place of FILE * (no <stdio.h> here).  All
 * object pointers share one representation and calling convention on the
 * single RV32 ilp32f target, so callers built against the real prototypes
 * interoperate deterministically; silence clang's builtin-signature lints.
 * ========================================================================= */

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wincompatible-library-redeclaration"
#pragma clang diagnostic ignored "-Wbuiltin-requires-header"

void blyt_console_debug(const char *s) __attribute__((weak));

typedef struct {
    int fd;
} blyt_FILE;
static blyt_FILE s_stdin = {0};
static blyt_FILE s_stdout = {1};
static blyt_FILE s_stderr = {2};

void *stdin = &s_stdin;
void *stdout = &s_stdout;
void *stderr = &s_stderr;

void *fopen(const char *p, const char *m) {
    (void)p;
    (void)m;
    return (void *)0;
}
int fclose(void *f) {
    (void)f;
    return 0;
}
size_t fread(void *b, size_t s, size_t n, void *f) {
    (void)b;
    (void)s;
    (void)n;
    (void)f;
    return 0;
}
size_t fwrite(const void *b, size_t s, size_t n, void *f) {
    (void)b;
    (void)s;
    (void)f;
    return n;
}
int feof(void *f) {
    (void)f;
    return 1;
}
int ferror(void *f) {
    (void)f;
    return 0;
}
int fflush(void *f) {
    (void)f;
    return 0;
}
void *freopen(const char *p, const char *m, void *f) {
    (void)p;
    (void)m;
    (void)f;
    return (void *)0;
}
int getc(void *f) {
    (void)f;
    return -1;
}

int fprintf(void *f, const char *fmt, ...) {
    (void)f;
    if (blyt_console_debug)
        blyt_console_debug(fmt);
    return 0;
}

int fputs(const char *s, void *f) {
    (void)f;
    if (blyt_console_debug)
        blyt_console_debug(s);
    return 0;
}

int fputc(int c, void *f) {
    (void)f;
    return c;
}

#pragma clang diagnostic pop

/* =========================================================================
 * Excluded Lua standard-library openers
 * ========================================================================= */

typedef void lua_State;

int luaopen_io(lua_State *L) {
    (void)L;
    return 0;
}
int luaopen_os(lua_State *L) {
    (void)L;
    return 0;
}
int luaopen_debug(lua_State *L) {
    (void)L;
    return 0;
}
int luaopen_package(lua_State *L) {
    (void)L;
    return 0;
}
int luaopen_utf8(lua_State *L) {
    (void)L;
    return 0;
}
