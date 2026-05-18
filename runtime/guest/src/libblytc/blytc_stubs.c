/*
 * blytc_stubs.c — musl internal stubs for the restricted libblytc.so build.
 *
 * libblytc.so is compiled from a curated subset of musl source files.
 * The stubs below satisfy linker references to musl internals that are not
 * included in that subset, adapting musl's assumptions to the blyt cart
 * execution environment (single-threaded, no OS, no filesystem, no signals).
 */

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

/*
 * Provide the musl __libc global that libblytc.so's curated source files
 * reference via the `libc` macro (src/internal/libc.h: #define libc __libc).
 *
 * We replicate only the fields accessed by our source subset; the layout
 * must match struct __libc in third_party/musl/src/internal/libc.h.
 * Keep in sync if the musl submodule is updated.
 *
 * Fields:
 *   can_do_threads — 0: single-threaded cart, no pthread support
 *   threaded       — 0: fmemopen sets lock=-1 → no-op FILE locking
 *   secure         — 0
 *   need_locks     — 0
 *   threads_minus_1 — 0
 *   auxv           — NULL (no ELF auxiliary vector)
 *   tls_head/size/align/cnt — 0 (no TLS)
 *   page_size      — 4096
 *   global_locale  — zero-initialised (C locale)
 */

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

/* ---- errno (no thread-local storage; single-threaded cart) ---- */

static int g_errno;

int *__errno_location(void) {
    return &g_errno;
}

/* musl provides weak_alias(__errno_location, ___errno_location) in its
 * errno/__errno_location.c; replicate the relationship here. */
int *___errno_location(void) {
    return __errno_location();
}

/* ---- locale translation (C locale: return string as-is) ---- */

/* __lctrans_cur translates a string for the current locale.  In C locale
 * there is no translation; return the input unchanged. */
const char *__lctrans_cur(const char *msg) {
    return msg;
}

/* ---- FILE locking (no-ops: all our FILE objects use lock=-1) ---- */

int __lockfile(FILE *f) {
    (void)f;
    return 0;
}

void __unlockfile(FILE *f) {
    (void)f;
}

/* ---- open file list (fmemopen needs __ofl_add; we don't track open files) ---- */

FILE *__ofl_add(FILE *f) {
    return f;
}

/* ---- stdio exit hook (referenced by __towrite; no atexit in carts) ---- */

void __stdio_exit_needed(void) {
}

/* ---- syscall safety net ----
 *
 * Any musl code path that reaches __syscall has escaped our curated subset.
 * Issue ECALL 0 (BLYT_ECALL_EXIT) to halt the emulator cleanly rather than
 * hanging or corrupting state.  This should never fire in practice.
 */

static void blytc_abort(void) {
#ifdef __riscv
    register long a7 __asm__("a7") = 0; /* BLYT_ECALL_EXIT — halt emulator */
    __asm__ volatile("ecall" : : "r"(a7) : "memory");
#endif
    __builtin_unreachable();
}

long __syscall_ret(unsigned long r) {
    (void)r;
    blytc_abort();
    return -1; /* unreachable */
}

/* ---- wide character stubs for vfprintf %ls / %lc (C locale, ASCII only) ---- */

size_t mbrtowc(wchar_t *restrict pwc, const char *restrict s, size_t n, mbstate_t *restrict ps) {
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
            *pwc = (wchar_t)c;
        return 1;
    }
    return (size_t)-1; /* non-ASCII: invalid in C locale */
}

size_t wcrtomb(char *restrict s, wchar_t wc, mbstate_t *restrict ps) {
    (void)ps;
    if (!s)
        return 1;
    if ((uint32_t)wc < 0x80u) {
        *s = (char)wc;
        return 1;
    }
    /* Non-ASCII wchar in C locale: emit '?' as a safe replacement. */
    *s = '?';
    return 1;
}

int mbsinit(const mbstate_t *ps) {
    (void)ps;
    return 1; /* always in initial shift state */
}

/* ---- localeconv stub (C locale decimal point for strtod / vfprintf) ---- */

#include <locale.h>

static char decimal_point[] = ".";
static char empty[] = "";

static struct lconv c_locale_conv = {
    .decimal_point = decimal_point,
    .thousands_sep = empty,
    .grouping = empty,
    .int_curr_symbol = empty,
    .currency_symbol = empty,
    .mon_decimal_point = empty,
    .mon_thousands_sep = empty,
    .mon_grouping = empty,
    .positive_sign = empty,
    .negative_sign = empty,
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
    return &c_locale_conv;
}
