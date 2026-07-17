/*
 * runtime/guest/src/libblyt32lua/lua_reverse_tramp.c
 *
 * Reverse-trampoline cart-facing entry points (#262, ADR-0130 amend).
 *
 * The restricted cart API (blyt_lua_internal.h) declares lua_pcall / lua_call as
 * plain extern functions so the SAME cart object links against either the full
 * in-machine libblyt32lua (rv32 emulated / bare-metal) or the ECALL bridge stub
 * (host-Lua, libblyt32lua-bridge.so).  In lua.h these are MACROS over
 * lua_pcallk / lua_callk, so on the full-lib side we materialise real symbols
 * here — this TU deliberately does NOT use the macros.  The bridge stub provides
 * its own ECALL versions (blyt32lua_bridge.c).
 *
 * msgh must be 0: the host-Lua bridge cannot run a message handler, so requiring
 * it here too keeps the reverse-trampoline behaviour identical across every leg
 * (determinism is the core contract — ADR-0007).
 */

#include <lauxlib.h>
#include <lua.h>

#undef lua_pcall
int lua_pcall(lua_State *L, int nargs, int nresults, int msgh) {
    if (msgh != 0)
        return luaL_error(L, "blyt bridge: lua_pcall message handler must be 0");
    return lua_pcallk(L, nargs, nresults, 0, 0, NULL);
}

#undef lua_call
void lua_call(lua_State *L, int nargs, int nresults) {
    lua_callk(L, nargs, nresults, 0, NULL);
}
