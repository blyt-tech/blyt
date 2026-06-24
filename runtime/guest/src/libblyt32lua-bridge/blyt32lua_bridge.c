/* runtime/guest/src/libblyt32lua-bridge/blyt32lua_bridge.c
 *
 * ECALL-bridged Lua C API stubs (ADR-0130) — the WASM-target variant of
 * libblyt32lua.so.  Contains NO Lua VM: every lua_* entry point traps to the
 * host via BLYT_ECALL_LUA_OP (a7=10) and the host executes the real
 * operation against the host-side exchange thread.  The lua_State* received
 * by wrappers is an opaque call token minted by the host per bridged call;
 * it is never dereferenced.
 *
 * Embedded by the WASM frontend in place of the real libblyt32lua.so
 * (same DT_NEEDED name), so one cart binary serves all targets.
 *
 * Error model: lua_error / luaL_error issue an ERROR/ERRMSG op.  The host
 * halts emulation WITHOUT advancing the PC, so the ecall never "returns";
 * the trap below is defence in depth.  The host restores a register
 * snapshot and raises the error inside the calling coroutine (ADR-0084).
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "blyt.h"
#include "blyt_lua_internal.h"

/* Provided by libblyt32.so (absorbed libblytc). */
extern void *malloc(size_t n);
extern void free(void *p);
extern size_t strlen(const char *s);
extern int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);

/* ECALL numbers / opcodes / status — keep in sync with
 * runtime/host/src/libblyt/ecall.h (ADR-0130). */
#define ECALL_LUA_OP 10

#define OP_GETTOP 1
#define OP_SETTOP 2
#define OP_PUSHVALUE 3
#define OP_TYPE 4
#define OP_PUSHNIL 5
#define OP_PUSHBOOLEAN 6
#define OP_PUSHINTEGER 7
#define OP_PUSHNUMBER 8
#define OP_PUSHLSTRING 9
#define OP_TOINTEGERX 10
#define OP_TONUMBERX 11
#define OP_TOBOOLEAN 12
#define OP_TOLSTRING 13
#define OP_CREATETABLE 14
#define OP_GETFIELD 15
#define OP_SETFIELD 16
#define OP_GETI 17
#define OP_SETI 18
#define OP_RAWLEN 19
#define OP_NEXT 20
#define OP_GETGLOBAL 21
#define OP_SETGLOBAL 22
#define OP_ERROR 23
#define OP_ERRMSG 24

#define ST_OK 0
#define ST_RETRY 1
#define ST_NIL 2

/* One bridge ECALL.  Returns status (a0); *val and *aux receive a1/a2. */
static uint32_t bridge_op(uint32_t opcode, lua_State *L, uint32_t b2, uint32_t b3, uint32_t b4,
                          uint32_t *val, uint32_t *aux) {
    register uint32_t a0 __asm__("a0") = opcode;
    register uint32_t a1 __asm__("a1") = (uint32_t)(uintptr_t)L; /* token */
    register uint32_t a2 __asm__("a2") = b2;
    register uint32_t a3 __asm__("a3") = b3;
    register uint32_t a4 __asm__("a4") = b4;
    register uint32_t a7 __asm__("a7") = ECALL_LUA_OP;
    __asm__ volatile("ecall" : "+r"(a0), "+r"(a1), "+r"(a2) : "r"(a3), "r"(a4), "r"(a7) : "memory");
    if (val)
        *val = a1;
    if (aux)
        *aux = a2;
    return a0;
}

/* -------------------------------------------------------------------------
 * Per-call string arena.
 *
 * lua_tolstring results are copied into guest memory and stay valid until
 * the wrapper returns (ADR-0130 lifetime rule — a strict subset of the real
 * API's guarantee).  The arena resets lazily when the call token changes:
 * tokens are unique per bridged call, so a change marks a new wrapper
 * invocation.
 * ------------------------------------------------------------------------- */

#define ARENA_STATIC_CAP 4096

static char g_arena_static[ARENA_STATIC_CAP];
static char *g_arena_heap = NULL; /* grown via malloc when needed */
static size_t g_arena_heap_cap = 0;
static size_t g_arena_used = 0;
static lua_State *g_arena_token = NULL;

static void arena_reset(lua_State *L) {
    if (g_arena_token != L) {
        g_arena_token = L;
        g_arena_used = 0;
    }
}

static char *arena_alloc(lua_State *L, size_t n) {
    arena_reset(L);
    if (g_arena_used + n <= ARENA_STATIC_CAP) {
        char *p = g_arena_static + g_arena_used;
        g_arena_used += n;
        return p;
    }
    /* Large strings get their own heap block, kept until the next call:
     * simple and bounded for spike purposes (one growable block). */
    if (n > g_arena_heap_cap) {
        free(g_arena_heap);
        g_arena_heap = malloc(n);
        g_arena_heap_cap = g_arena_heap ? n : 0;
    }
    return g_arena_heap;
}

/* -------------------------------------------------------------------------
 * Lua C API stubs
 * ------------------------------------------------------------------------- */

int lua_gettop(lua_State *L) {
    uint32_t v;
    bridge_op(OP_GETTOP, L, 0, 0, 0, &v, NULL);
    return (int)v;
}

void lua_settop(lua_State *L, int idx) {
    bridge_op(OP_SETTOP, L, (uint32_t)idx, 0, 0, NULL, NULL);
}

void lua_pushvalue(lua_State *L, int idx) {
    bridge_op(OP_PUSHVALUE, L, (uint32_t)idx, 0, 0, NULL, NULL);
}

int lua_type(lua_State *L, int idx) {
    uint32_t v;
    bridge_op(OP_TYPE, L, (uint32_t)idx, 0, 0, &v, NULL);
    return (int)v;
}

void lua_pushnil(lua_State *L) {
    bridge_op(OP_PUSHNIL, L, 0, 0, 0, NULL, NULL);
}

void lua_pushboolean(lua_State *L, int b) {
    bridge_op(OP_PUSHBOOLEAN, L, (uint32_t)(b ? 1 : 0), 0, 0, NULL, NULL);
}

void lua_pushinteger(lua_State *L, lua_Integer n) {
    bridge_op(OP_PUSHINTEGER, L, (uint32_t)n, 0, 0, NULL, NULL);
}

void lua_pushnumber(lua_State *L, lua_Number n) {
    uint64_t bits;
    __builtin_memcpy(&bits, &n, 8);
    bridge_op(OP_PUSHNUMBER, L, (uint32_t)bits, (uint32_t)(bits >> 32), 0, NULL, NULL);
}

const char *lua_pushlstring(lua_State *L, const char *s, size_t_blyt len) {
    bridge_op(OP_PUSHLSTRING, L, (uint32_t)(uintptr_t)s, (uint32_t)len, 0, NULL, NULL);
    return s; /* real API returns the interned copy; bridged code must not
               * rely on interning, only on the contents (ADR-0130) */
}

const char *lua_pushstring(lua_State *L, const char *s) {
    if (!s) {
        lua_pushnil(L);
        return NULL;
    }
    return lua_pushlstring(L, s, strlen(s));
}

lua_Integer lua_tointegerx(lua_State *L, int idx, int *isnum) {
    uint32_t v, aux;
    bridge_op(OP_TOINTEGERX, L, (uint32_t)idx, 0, 0, &v, &aux);
    if (isnum)
        *isnum = (int)aux;
    return (lua_Integer)(int32_t)v;
}

lua_Number lua_tonumberx(lua_State *L, int idx, int *isnum) {
    uint32_t lo, hi;
    uint32_t st = bridge_op(OP_TONUMBERX, L, (uint32_t)idx, 0, 0, &lo, &hi);
    if (isnum)
        *isnum = (st == ST_OK) ? 1 : 0;
    if (st != ST_OK)
        return (lua_Number)0;
    uint64_t bits = (uint64_t)hi << 32 | (uint64_t)lo;
    lua_Number d;
    __builtin_memcpy(&d, &bits, 8);
    return d;
}

int lua_toboolean(lua_State *L, int idx) {
    uint32_t v;
    bridge_op(OP_TOBOOLEAN, L, (uint32_t)idx, 0, 0, &v, NULL);
    return (int)v;
}

const char *lua_tolstring(lua_State *L, int idx, size_t_blyt *len) {
    char *buf = arena_alloc(L, 64); /* first try: small inline buffer */
    uint32_t wrote, full;
    uint32_t st =
        bridge_op(OP_TOLSTRING, L, (uint32_t)idx, (uint32_t)(uintptr_t)buf, 64, &wrote, &full);
    if (st == ST_NIL) {
        if (len)
            *len = 0;
        return NULL;
    }
    if (st == ST_RETRY) {
        buf = arena_alloc(L, (size_t)full + 1);
        if (!buf)
            return NULL; /* OOM: degrade like lua_tolstring on non-string */
        st = bridge_op(OP_TOLSTRING, L, (uint32_t)idx, (uint32_t)(uintptr_t)buf, full + 1, &wrote,
                       &full);
        if (st != ST_OK)
            return NULL;
    }
    if (len)
        *len = (size_t_blyt)full;
    return buf;
}

void lua_createtable(lua_State *L, int narr, int nrec) {
    bridge_op(OP_CREATETABLE, L, (uint32_t)narr, (uint32_t)nrec, 0, NULL, NULL);
}

void lua_getfield(lua_State *L, int idx, const char *k) {
    bridge_op(OP_GETFIELD, L, (uint32_t)idx, (uint32_t)(uintptr_t)k, (uint32_t)strlen(k), NULL,
              NULL);
}

void lua_setfield(lua_State *L, int idx, const char *k) {
    bridge_op(OP_SETFIELD, L, (uint32_t)idx, (uint32_t)(uintptr_t)k, (uint32_t)strlen(k), NULL,
              NULL);
}

int lua_geti(lua_State *L, int idx, lua_Integer i) {
    uint32_t v;
    bridge_op(OP_GETI, L, (uint32_t)idx, (uint32_t)i, 0, &v, NULL);
    return (int)v;
}

void lua_seti(lua_State *L, int idx, lua_Integer i) {
    bridge_op(OP_SETI, L, (uint32_t)idx, (uint32_t)i, 0, NULL, NULL);
}

unsigned int lua_rawlen(lua_State *L, int idx) {
    uint32_t v;
    bridge_op(OP_RAWLEN, L, (uint32_t)idx, 0, 0, &v, NULL);
    return v;
}

int lua_next(lua_State *L, int idx) {
    uint32_t v;
    bridge_op(OP_NEXT, L, (uint32_t)idx, 0, 0, &v, NULL);
    return (int)v;
}

void lua_getglobal(lua_State *L, const char *name) {
    bridge_op(OP_GETGLOBAL, L, (uint32_t)(uintptr_t)name, (uint32_t)strlen(name), 0, NULL, NULL);
}

void lua_setglobal(lua_State *L, const char *name) {
    bridge_op(OP_SETGLOBAL, L, (uint32_t)(uintptr_t)name, (uint32_t)strlen(name), 0, NULL, NULL);
}

const char *lua_typename(lua_State *L, int tp) {
    (void)L;
    switch (tp) {
    case LUA_TNIL:
        return "nil";
    case LUA_TBOOLEAN:
        return "boolean";
    case LUA_TLIGHTUSERDATA:
    case LUA_TUSERDATA:
        return "userdata";
    case LUA_TNUMBER:
        return "number";
    case LUA_TSTRING:
        return "string";
    case LUA_TTABLE:
        return "table";
    case LUA_TFUNCTION:
        return "function";
    case LUA_TTHREAD:
        return "thread";
    default:
        return "no value";
    }
}

/* -------------------------------------------------------------------------
 * Errors — never return.
 * ------------------------------------------------------------------------- */

int lua_error(lua_State *L) {
    bridge_op(OP_ERROR, L, 0, 0, 0, NULL, NULL);
    __builtin_trap(); /* host halted without advancing PC; unreachable */
}

int luaL_error(lua_State *L, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0)
        n = 0;
    if ((size_t)n >= sizeof(buf))
        n = (int)sizeof(buf) - 1;
    bridge_op(OP_ERRMSG, L, (uint32_t)(uintptr_t)buf, (uint32_t)n, 0, NULL, NULL);
    __builtin_trap();
}

/* -------------------------------------------------------------------------
 * Composed helpers (no dedicated opcodes)
 * ------------------------------------------------------------------------- */

lua_Integer luaL_checkinteger(lua_State *L, int arg) {
    int isnum = 0;
    lua_Integer n = lua_tointegerx(L, arg, &isnum);
    if (!isnum)
        luaL_error(L, "bad argument #%d (number expected, got %s)", arg,
                   lua_typename(L, lua_type(L, arg)));
    return n;
}

lua_Number luaL_checknumber(lua_State *L, int arg) {
    int isnum = 0;
    lua_Number n = lua_tonumberx(L, arg, &isnum);
    if (!isnum)
        luaL_error(L, "bad argument #%d (number expected, got %s)", arg,
                   lua_typename(L, lua_type(L, arg)));
    return n;
}

const char *luaL_checklstring(lua_State *L, int arg, size_t_blyt *len) {
    const char *s = lua_tolstring(L, arg, len);
    if (!s)
        luaL_error(L, "bad argument #%d (string expected, got %s)", arg,
                   lua_typename(L, lua_type(L, arg)));
    return s;
}

int luaL_getsubtable(lua_State *L, int idx, const char *fname) {
    lua_getfield(L, idx, fname);
    if (lua_istable(L, -1))
        return 1;
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setfield(L, idx, fname); /* note: idx must be absolute for this path */
    return 0;
}

/* Runtime registration of guest closures is host-controlled on WASM
 * (ADR-0130 deferred list); cart_lua_modules is never invoked here. */
void lua_pushcclosure(lua_State *L, lua_CFunction f, int n) {
    (void)f;
    (void)n;
    luaL_error(L, "blyt bridge: lua_pushcclosure is not available on this target");
}

/* ADR-0011: proxy metatable construction.  The WASM host drives Lua directly so
 * these are never called (lua_pushcclosure errors first); stubs satisfy the ABI. */
int lua_setmetatable(lua_State *L, int objindex) {
    (void)objindex;
    luaL_error(L, "blyt bridge: lua_setmetatable is not available on this target");
    return 0;
}

int lua_rawgeti(lua_State *L, int idx, lua_Integer n) {
    (void)idx;
    (void)n;
    luaL_error(L, "blyt bridge: lua_rawgeti is not available on this target");
    return 0;
}

void lua_rawseti(lua_State *L, int idx, lua_Integer n) {
    (void)idx;
    (void)n;
    luaL_error(L, "blyt bridge: lua_rawseti is not available on this target");
}

/* -------------------------------------------------------------------------
 * Cart lifecycle no-ops.
 *
 * On the WASM hybrid path the host-side Lua drives the cart; these exist
 * only to satisfy libblytcommon.so's strong-symbol check at load time.
 * ------------------------------------------------------------------------- */

void blyt_cart_init(void) {
}
void blyt_cart_update(void) {
}
void blyt_cart_draw(void) {
}
void blyt_cart_on_new_state(void) {
}
void blyt_cart_on_save_state(void) {
}
void blyt_cart_on_load_state(blyt_load_info_t info) {
    (void)info;
}
void blyt_cart_on_assets_reloaded(const uint32_t *ids, size_t n) {
    (void)ids;
    (void)n;
}
