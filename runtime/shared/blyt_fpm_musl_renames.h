/*
 * runtime/shared/blyt_fpm_musl_renames.h — Phase B symbol renames (ADR-0135,
 * blyt#225).
 *
 * Force-included (`-include`) on each vendored blyt-tech musl stdio/stdlib TU in
 * the host-Lua FP seam library (strtod.c / floatscan.c / shgetc.c / vfprintf.c /
 * vsnprintf.c / snprintf.c). It rewrites the public and musl-internal entry
 * points those TUs define and cross-reference into a `blyt_fpm_` namespace, so
 * the objects can be linked into the host-Lua VM WITHOUT their strong
 * `strtod`/`snprintf`/`__floatscan`/… definitions overriding the surrounding
 * module's libc — critical on WASM, where the whole module IS Emscripten's musl
 * and a second `strtod`/`__floatscan` would collide, and on the native leg,
 * where they would shadow the system libc.
 *
 * The renames must be applied UNIFORMLY across all of the vendored TUs so their
 * cross-references stay consistent (e.g. strtod.c's call to `__floatscan`
 * resolves to floatscan.c's renamed definition). A rename for a symbol a given
 * TU neither defines nor references is harmless.
 *
 * The byte-neutral plumbing leaves these TUs reach for (`__towrite`,
 * `__fwritex`, the never-taken `__uflow`/`wctomb`, and `__errno_location`) are
 * renamed here too and re-provided in blyt_fpm_conv.c, which severs musl's
 * `__uflow`→read-stdio and `__towrite`→`__stdio_exit_needed` closures and the
 * `weak_alias` idioms that are unsupported on Darwin — keeping the vendored set
 * to just the conversion + float-format logic that actually needs to be pinned.
 *
 * NOT force-included on the src/math kernels (they keep their real
 * `sin`/`scalbn`/… names, which the seam deliberately overrides — see
 * blyt_fpm_soft.c) nor on blyt_fpm_conv.c (which defines the `blyt_fpm_`
 * plumbing directly).
 */
#ifndef BLYT_FPM_MUSL_RENAMES_H
#define BLYT_FPM_MUSL_RENAMES_H

/* Public conversion entry points (the seam surface luaconf.h routes to). */
#define strtod blyt_fpm_strtod
#define strtof blyt_fpm_strtof
#define strtold blyt_fpm_strtold
#define snprintf blyt_fpm_snprintf
#define vsnprintf blyt_fpm_vsnprintf
#define vfprintf blyt_fpm_vfprintf

/* musl-internal helpers the vendored TUs define + cross-reference. */
#define __floatscan blyt_fpm_floatscan
#define __shlim blyt_fpm_shlim
#define __shgetc blyt_fpm_shgetc

/* Byte-neutral plumbing leaves re-provided in blyt_fpm_conv.c. __lockfile /
 * __unlockfile back vfprintf's FLOCK/FUNLOCK (never taken: the string-cookie
 * FILE has lock == -1). errno has both a public (__errno_location) and a
 * musl-internal (___errno_location, from src/include/errno.h) spelling; both
 * alias one private backing int. */
#define __towrite blyt_fpm_towrite
#define __fwritex blyt_fpm_fwritex
#define __uflow blyt_fpm_uflow
#define wctomb blyt_fpm_wctomb
#define __lockfile blyt_fpm_lockfile
#define __unlockfile blyt_fpm_unlockfile
#define __errno_location blyt_fpm_errno_location
#define ___errno_location blyt_fpm_errno_location

#endif /* BLYT_FPM_MUSL_RENAMES_H */
