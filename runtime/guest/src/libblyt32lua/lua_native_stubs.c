/* runtime/guest/src/libblyt32lua/lua_native_stubs.c
 *
 * Minimal stubs for the native libblyt32lua.so build.
 *
 * linit.c references Lua standard-library openers that are excluded from
 * LUA_ALL_SRCS (liolib/loslib/loadlib/ldblib/lutf8lib).  On the emulated path
 * these are stubbed out in lua_runtime_stubs.c; that file is NOT included in
 * the native build because it redefines errno_location, stderr, fprintf, etc.
 * which conflict with the native musl libc.
 *
 * Only the Lua opener stubs are needed here; all other libc symbols come from
 * the native musl ld.so at runtime.
 */

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
