#include "blyt.h"
#include "lauxlib.h"
#include "lua.h"

static int l_log(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    blyt_console_debug(s);
    return 0;
}

static const luaL_Reg greeting_funcs[] = {
    {"log", l_log},
    {NULL, NULL},
};

static int luaopen_greeting(lua_State *L) {
    luaL_newlib(L, greeting_funcs);
    return 1;
}

void cart_lua_modules(lua_State *L) {
    luaL_requiref(L, "greeting", luaopen_greeting, 1);
    lua_pop(L, 1);
}
