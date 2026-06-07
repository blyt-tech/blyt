#include "blyt.h"

BLYT_LUA_MODULE_EXPORT_RAW(greeting, log) {
    const char *s = luaL_checkstring(L, 1);
    blyt_console_debug(s);
    return 0;
}
