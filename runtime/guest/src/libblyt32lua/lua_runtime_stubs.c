/*
 * runtime/guest/src/libblyt32lua/lua_runtime_stubs.c
 *
 * Symbols required by the Lua 5.5 VM that are NOT present in libblytc.so.
 * libblyt32lua.so absorbs libblytc sources AND these stubs so that
 * libblytcommonlua.so's PLT entries resolve at runtime.
 *
 * --- Soft-float builtins ---
 * Compiler-rt ABI: float<->double conversions for rv32imafc (no D extension).
 * Implemented as pure integer bit manipulation — no float instructions emitted.
 *
 * --- stdio stubs ---
 * lauxlib.c references fopen/fclose/fprintf/etc for luaL_loadfile (never
 * called in sandboxed carts).  Stubs prevent unresolved PLT entries crashing
 * on the first indirect call through those GOT slots.
 *
 * --- Excluded stdlib openers ---
 * linit.c references luaopen_io/os/debug/package/utf8 which are excluded
 * from libblytcommonlua.so.  Stub openers return 0 (register nothing).
 *
 * NOT defined here (already in LIBBLYTC_SRCS / musl/src/string/*.c):
 *   strpbrk, strspn — musl/src/string/*.c
 *   fwrite           — musl/src/stdio/fwrite.c
 */

#include <stddef.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Note: soft-float compiler-rt builtins (__extendsfdf2, __adddf3, __addtf3,
 * etc.) are provided by softfloat_builtins.c (Berkeley SoftFloat wrappers).
 * ------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------
 * errno
 * ------------------------------------------------------------------------- */

static int s_errno;
int *__errno_location(void) {
    return &s_errno;
}

/* -------------------------------------------------------------------------
 * Missing string/locale functions (not in libblytc's musl subset)
 * ------------------------------------------------------------------------- */

int strcoll(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

const char *strerror(int e) {
    (void)e;
    return "error";
}

/* -------------------------------------------------------------------------
 * time (referenced by lauxlib.c for random seeding)
 * ------------------------------------------------------------------------- */

typedef long blyt_time_t;
blyt_time_t time(blyt_time_t *t) {
    if (t)
        *t = 0;
    return 0;
}

/* -------------------------------------------------------------------------
 * stdio stubs — dead paths in sandboxed carts, never called at runtime.
 *
 * stdin/stdout/stderr are non-NULL so NULL-checks in Lua pass, but the
 * underlying fd operations are no-ops.  fprintf routes to blyt_console_debug
 * so Lua's panic handler produces a visible error rather than silently dying.
 * ------------------------------------------------------------------------- */

/* blyt_console_debug is in libblyt32.so; weak so this file links standalone */
void blyt_console_debug(const char *s) __attribute__((weak));

typedef struct {
    int fd;
} blyt_FILE;
static blyt_FILE s_stdin_obj = {0};
static blyt_FILE s_stdout_obj = {1};
static blyt_FILE s_stderr_obj = {2};

void *stdin = &s_stdin_obj;
void *stdout = &s_stdout_obj;
void *stderr = &s_stderr_obj;

void *fopen(const char *p, const char *m) {
    (void)p;
    (void)m;
    return NULL;
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
    return NULL;
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

/* -------------------------------------------------------------------------
 * Excluded Lua standard library openers
 *
 * linit.c's loadedlibs table references these; excluding liolib/loslib/
 * loadlib/ldblib/lutf8lib leaves their PLT entries unresolved.
 * Return 0 (push nothing) so luaL_requiref sees an empty module.
 * ------------------------------------------------------------------------- */

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
