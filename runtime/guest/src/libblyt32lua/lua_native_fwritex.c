/*
 * lua_native_fwritex.c — minimal __fwritex for the vsnprintf / vfprintf path.
 *
 * musl's vfprintf.c calls the hidden symbol __fwritex (defined in fwrite.c).
 * We provide our own version here instead of including fwrite.c, because
 * fwrite.c's public fwrite() would crash when called with our fake FILE stubs
 * (blyt_FILE = { int fd; }) — those stubs don't have the full musl FILE layout
 * that fwrite() accesses.
 *
 * This file is compiled into libblytc_native.o with LIBBLYTC_INCLUDES, so it
 * can safely include stdio_impl.h.  The public fwrite() stub lives in
 * lua_native_stubs.c and is compiled with LUA_MUSL_INCLUDES (no musl/src/).
 */

#include "stdio_impl.h"
#include <string.h>

size_t __fwritex(const unsigned char *restrict s, size_t l, FILE *restrict f) {
    size_t i = 0;

    if (!f->wend && __towrite(f))
        return 0;

    if (l > (size_t)(f->wend - f->wpos))
        return f->write(f, s, l);

    if (f->lbf >= 0) {
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
