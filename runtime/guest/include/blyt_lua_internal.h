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

/* Error reporting. */
extern int luaL_error(lua_State *L, const char *fmt, ...);

#ifdef __cplusplus
}
#endif
