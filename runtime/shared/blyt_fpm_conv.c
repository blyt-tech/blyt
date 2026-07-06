/*
 * runtime/shared/blyt_fpm_conv.c — Phase B plumbing leaves for the host-Lua FP
 * number-format seam (ADR-0135, blyt#225).
 *
 * The vendored blyt-tech musl `strtod`/`vfprintf` subset (see
 * blyt_fpm_musl_renames.h) reaches for a handful of musl-internal helpers whose
 * real definitions would drag in machinery we neither want nor can build
 * cleanly here:
 *   • __uflow  — the read-refill path shgetc.c falls back to. It is UNREACHABLE
 *     for a string pseudo-FILE (sh_fromstring sets rend = (void*)-1 and __shlim
 *     with lim 0 sets shend = rend, so the shgetc fast path walks the
 *     NUL-terminated string and never hits the __shgetc branch), but the symbol
 *     must still resolve. Pulling real __uflow would drag in the full read stdio.
 *   • __towrite / __fwritex — the write-buffer plumbing vfprintf emits bytes
 *     through. musl's real __towrite.c pulls __stdio_exit_needed and fwrite.c
 *     carries a `weak_alias` (unsupported on Darwin). Both are byte-neutral: for
 *     the string-cookie FILE vsnprintf builds, every write goes straight to the
 *     cookie's sn_write, so any correct implementation yields identical output
 *     (the FP-sensitive logic lives entirely in fmt_fp / decfloat / hexfloat,
 *     which are compiled verbatim from musl).
 *   • wctomb — referenced by printf_core for %lc/%ls, which Lua's number/string
 *     formatting never emits. Reimplemented as musl's UTF-8 encoder (musl's
 *     default MB_CUR_MAX == 4 locale) so it stays faithful if ever reached.
 *   • __errno_location — errno is set by floatscan/vfprintf on overflow etc.;
 *     Lua ignores it (l_str2d checks the end pointer, not errno). A private
 *     backing int keeps the seam self-contained instead of binding the module's
 *     libc errno.
 *
 * These are re-provided here under their `blyt_fpm_` names (matching
 * blyt_fpm_musl_renames.h) so the vendored objects link against this file, not
 * the surrounding module's libc. This file is NOT compiled with the rename
 * header's effect mattering: it defines the target names directly.
 */
#include "stdio_impl.h" /* musl-internal: FILE layout, F_NOWR/F_ERR, EOF */

#include <string.h>
#include <wchar.h>

/* musl __towrite, minus the __stdio_exit_needed helper (never needed for the
 * unbuffered string-cookie FILE — buf_size is 0, so wend == wpos and every
 * write goes straight through f->write). */
int blyt_fpm_towrite(FILE *f) {
    f->mode |= f->mode - 1;
    if (f->flags & F_NOWR) {
        f->flags |= F_ERR;
        return EOF;
    }
    /* Clear read buffer, activate write through the buffer. */
    f->rpos = f->rend = 0;
    f->wpos = f->wbase = f->buf;
    f->wend = f->buf + f->buf_size;
    return 0;
}

/* musl __fwritex, verbatim except calling blyt_fpm_towrite. Byte-neutral: it
 * only moves bytes into the FILE buffer or hands them to f->write. */
size_t blyt_fpm_fwritex(const unsigned char *restrict s, size_t l, FILE *restrict f) {
    size_t i = 0;

    if (!f->wend && blyt_fpm_towrite(f))
        return 0;

    if (l > (size_t)(f->wend - f->wpos))
        return f->write(f, s, l);

    if (f->lbf >= 0) {
        /* Match /^(.*\n|)/ */
        for (i = l; i && s[i - 1] != '\n'; i--)
            ;
        if (i) {
            size_t n = f->write(f, s, i);
            if (n < i)
                return n;
            s += i;
            l -= i;
        }
    }

    memcpy(f->wpos, s, l);
    f->wpos += l;
    return l + i;
}

/* Unreachable for string pseudo-FILEs (see file header); return EOF if ever hit
 * so callers terminate the scan cleanly rather than spin. */
int blyt_fpm_uflow(FILE *f) {
    (void)f;
    return EOF;
}

/* musl's UTF-8 wcrtomb encoding (default MB_CUR_MAX == 4), inlined without the
 * locale/mbstate plumbing. Never reached from Lua's formatting surface. */
int blyt_fpm_wctomb(char *s, wchar_t wc) {
    unsigned c = (unsigned)wc;
    if (!s)
        return 0;
    if (c < 0x80u) {
        s[0] = (char)c;
        return 1;
    } else if (c < 0x800u) {
        s[0] = (char)(0xc0u | (c >> 6));
        s[1] = (char)(0x80u | (c & 0x3fu));
        return 2;
    } else if (c < 0xd800u || c - 0xe000u < 0x2000u) {
        s[0] = (char)(0xe0u | (c >> 12));
        s[1] = (char)(0x80u | ((c >> 6) & 0x3fu));
        s[2] = (char)(0x80u | (c & 0x3fu));
        return 3;
    } else if (c - 0x10000u < 0x100000u) {
        s[0] = (char)(0xf0u | (c >> 18));
        s[1] = (char)(0x80u | ((c >> 12) & 0x3fu));
        s[2] = (char)(0x80u | ((c >> 6) & 0x3fu));
        s[3] = (char)(0x80u | (c & 0x3fu));
        return 4;
    }
    return -1;
}

/* vfprintf's FLOCK/FUNLOCK on the string-cookie FILE (lock == -1) never take the
 * lock branch, but the symbols must resolve. No-op. */
int blyt_fpm_lockfile(FILE *f) {
    (void)f;
    return 0;
}
void blyt_fpm_unlockfile(FILE *f) {
    (void)f;
}

/* Private errno backing: the seam's floatscan/vfprintf set errno on overflow;
 * Lua ignores it, so a self-contained int keeps the module's libc errno out of
 * the picture. */
int *blyt_fpm_errno_location(void) {
    static int e;
    return &e;
}
