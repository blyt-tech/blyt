#pragma once

/* SDK-internal Lua declarations for BLYT_LUA_EXPORT_* macro-generated wrappers
 * and devtool-generated cart_lua_modules glue.  Not for direct use in cart code.
 *
 * Deliberately omits: lua_call, lua_pcall, lua_load, luaL_loadbuffer,
 * lua_gettable, lua_settable, luaL_requiref, luaL_openlibs — cart code must
 * communicate with Lua exclusively through the BLYT_LUA_EXPORT_* boundary.
 *
 * Matches the Lua 5.4 ABI compiled with LUA_32BITS=1 (lua_Integer=int,
 * lua_Number=float, as used by all blyt Lua VMs). */

#ifdef __cplusplus
extern "C" {
#endif

/* Matches lua.h: -(INT_MAX/2 + 1000) for 32-bit int (RV32 ABI). */
#define LUA_REGISTRYINDEX (-1073742823)

typedef struct lua_State lua_State; /* opaque */
typedef int (*lua_CFunction)(lua_State *L);

/* LUA_32BITS=1: lua_Integer=int, lua_Number=float on RV32. */
typedef int lua_Integer;
typedef float lua_Number;

/* Argument extraction — used by BLYT_LUA_EXPORT_* wrapper bodies. */
extern lua_Integer lua_tointegerx(lua_State *L, int idx, int *isnum);
#define lua_tointeger(L, i) lua_tointegerx((L), (i), (int *)0)
extern lua_Number lua_tonumberx(lua_State *L, int idx, int *isnum);
#define lua_tonumber(L, i) lua_tonumberx((L), (i), (int *)0)
extern int lua_toboolean(lua_State *L, int idx);

/* Return value push — used by BLYT_LUA_EXPORT_* wrapper bodies. */
extern void lua_pushinteger(lua_State *L, lua_Integer n);
extern void lua_pushnumber(lua_State *L, lua_Number n);
extern void lua_pushboolean(lua_State *L, int b);

/* Stack manipulation — underlying functions for macros used by cart_lua_modules. */
extern void lua_settop(lua_State *L, int idx);
#define lua_pop(L, n) lua_settop((L), -(n) - 1)

/* Table operations — underlying functions; lua_newtable/lua_istable are macros. */
extern void lua_createtable(lua_State *L, int narr, int nrec);
#define lua_newtable(L) lua_createtable((L), 0, 0)
extern int lua_type(lua_State *L, int idx);
#define LUA_TTABLE 5
#define lua_istable(L, n) (lua_type((L), (n)) == LUA_TTABLE)

/* Global and field access — used by cart_lua_modules. */
extern void lua_getglobal(lua_State *L, const char *name);
extern void lua_setglobal(lua_State *L, const char *name);
extern void lua_getfield(lua_State *L, int idx, const char *k);
extern void lua_setfield(lua_State *L, int idx, const char *k);
extern void lua_pushvalue(lua_State *L, int idx);

/* Closure registration — lua_pushcfunction is a macro for lua_pushcclosure(f,0). */
extern void lua_pushcclosure(lua_State *L, lua_CFunction f, int n);
#define lua_pushcfunction(L, f) lua_pushcclosure((L), (f), 0)
/* Upvalue access index for closures created with lua_pushcclosure. */
#define lua_upvalueindex(i) (LUA_REGISTRYINDEX - (i))

/* Metatable and raw table operations — used by devtool-generated proxy setup. */
extern int lua_setmetatable(lua_State *L, int objindex);
extern int lua_rawgeti(lua_State *L, int idx, lua_Integer n);
extern void lua_rawseti(lua_State *L, int idx, lua_Integer n);

/* Error reporting. */
extern int luaL_error(lua_State *L, const char *fmt, ...);

/* Module table helper — gets or creates registry[fname]; used by cart_lua_modules. */
extern int luaL_getsubtable(lua_State *L, int idx, const char *fname);

/* --- ADR-0130 additions: string/table surface for raw wrappers ------------
 * Same restricted-API spirit (no code loading); exported by libblyt32lua.so
 * on rv32 and implemented as ECALL bridge stubs in the WASM-target variant. */

typedef __SIZE_TYPE__ size_t_blyt; /* avoid <stddef.h> dependency here */

extern int lua_gettop(lua_State *L);
extern void lua_pushnil(lua_State *L);
extern const char *lua_pushstring(lua_State *L, const char *s);
extern const char *lua_pushlstring(lua_State *L, const char *s, size_t_blyt len);
extern const char *lua_tolstring(lua_State *L, int idx, size_t_blyt *len);
#define lua_tostring(L, i) lua_tolstring((L), (i), (size_t_blyt *)0)
extern int lua_error(lua_State *L); /* never returns */
extern int lua_geti(lua_State *L, int idx, lua_Integer i);
extern void lua_seti(lua_State *L, int idx, lua_Integer i);
extern unsigned int lua_rawlen(lua_State *L, int idx);
extern int lua_next(lua_State *L, int idx);
extern const char *lua_typename(lua_State *L, int tp);

/* Lua type tags (match lua.h). */
#define LUA_TNONE (-1)
#define LUA_TNIL 0
#define LUA_TBOOLEAN 1
#define LUA_TLIGHTUSERDATA 2
#define LUA_TNUMBER 3
#define LUA_TSTRING 4
/* LUA_TTABLE 5 defined above */
#define LUA_TFUNCTION 6
#define LUA_TUSERDATA 7
#define LUA_TTHREAD 8

/* Argument helpers (composed from the surface above in the bridge variant). */
extern lua_Integer luaL_checkinteger(lua_State *L, int arg);
extern lua_Number luaL_checknumber(lua_State *L, int arg);
extern const char *luaL_checklstring(lua_State *L, int arg, size_t_blyt *len);
#define luaL_checkstring(L, a) luaL_checklstring((L), (a), (size_t_blyt *)0)

#ifdef __cplusplus
}
#endif
