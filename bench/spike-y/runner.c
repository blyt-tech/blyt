/* Native Lua per-pixel bench: the patched Lua VM (fork) compiled native (aarch64),
 * with the blyt32.surface per-pixel API bound to native C writing a buffer.
 * Measures the Lua-per-pixel loop with NO rv32 emulation. */
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint8_t screen[320 * 240];

/* Lock userdata layout MUST match struct blyt_fp_lock in the lvm.c patches. */
typedef struct {
    unsigned char *pixels;
    int stride, w, h;
    unsigned int token, epoch;
    int released;
} lock_t;

/* Symbols the VM fast paths (BLYT_LUA_FASTPIXEL) resolve. */
unsigned int blyt_lua_lock_epoch = 1;
void *blyt_lua_lock_mt = NULL;

static lock_t g_screen_lock;
static int g_screen_active = 0;

static int l_set(lua_State *L) {
    lock_t *u = (lock_t *)luaL_checkudata(L, 1, "blyt.lock");
    int x = (int)luaL_checkinteger(L, 2), y = (int)luaL_checkinteger(L, 3),
        c = (int)luaL_checkinteger(L, 4);
    if (!u->released && u->epoch == blyt_lua_lock_epoch && x >= 0 && x < u->w && y >= 0 && y < u->h)
        u->pixels[(unsigned)y * (unsigned)u->stride + (unsigned)x] = (unsigned char)c;
    return 0;
}
static int l_get(lua_State *L) {
    lock_t *u = (lock_t *)luaL_checkudata(L, 1, "blyt.lock");
    int x = (int)luaL_checkinteger(L, 2), y = (int)luaL_checkinteger(L, 3);
    if (!u->released && u->epoch == blyt_lua_lock_epoch && x >= 0 && x < u->w && y >= 0 && y < u->h)
        lua_pushinteger(L, u->pixels[(unsigned)y * (unsigned)u->stride + (unsigned)x]);
    else
        lua_pushinteger(L, 0);
    return 1;
}
static int l_release(lua_State *L) {
    lock_t *u = (lock_t *)luaL_checkudata(L, 1, "blyt.lock");
    u->released = 1;
    return 0;
}
int (*blyt_lua_fp_set)(lua_State *) = l_set;
int (*blyt_lua_fp_get)(lua_State *) = l_get;

void blyt_lua_fast_set_pixel(int x, int y, int c) {
    if (g_screen_active && g_screen_lock.epoch == blyt_lua_lock_epoch && !g_screen_lock.released &&
        x >= 0 && x < g_screen_lock.w && y >= 0 && y < g_screen_lock.h)
        g_screen_lock.pixels[(unsigned)y * (unsigned)g_screen_lock.stride + (unsigned)x] =
            (unsigned char)c;
}
static int l_set_pixel(lua_State *L) {
    blyt_lua_fast_set_pixel((int)luaL_checkinteger(L, 1), (int)luaL_checkinteger(L, 2),
                            (int)luaL_checkinteger(L, 3));
    return 0;
}
int (*blyt_lua_fp_set_pixel)(lua_State *) = l_set_pixel;

static int l_acquire(lua_State *L) {
    (void)L;
    lock_t *u = (lock_t *)lua_newuserdatauv(L, sizeof(lock_t), 0);
    u->pixels = screen;
    u->stride = 320;
    u->w = 320;
    u->h = 240;
    u->token = 1;
    u->epoch = blyt_lua_lock_epoch;
    u->released = 0;
    luaL_setmetatable(L, "blyt.lock");
    g_screen_lock = *u;
    g_screen_active = 1;
    return 1;
}
static int l_noop(lua_State *L) {
    (void)L;
    return 0;
}

static void setup(lua_State *L) {
    /* lock metatable */
    luaL_newmetatable(L, "blyt.lock");
    blyt_lua_lock_mt = (void *)lua_topointer(L, -1);
    lua_newtable(L);
    lua_pushcfunction(L, l_set);
    lua_setfield(L, -2, "set");
    lua_pushcfunction(L, l_get);
    lua_setfield(L, -2, "get");
    lua_pushcfunction(L, l_release);
    lua_setfield(L, -2, "release");
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
    /* blyt = { quit } */
    lua_newtable(L);
    lua_pushcfunction(L, l_noop);
    lua_setfield(L, -2, "quit");
    lua_setglobal(L, "blyt");
    /* blyt32.surface = { acquire, SCREEN, set_pixel, pset, create, clear, blit } */
    lua_newtable(L); /* blyt32 */
    lua_newtable(L); /* surface */
    lua_pushcfunction(L, l_acquire);
    lua_setfield(L, -2, "acquire");
    lua_pushinteger(L, 0x40000000);
    lua_setfield(L, -2, "SCREEN");
    lua_pushcfunction(L, l_set_pixel);
    lua_setfield(L, -2, "set_pixel");
    lua_pushcfunction(L, l_set);
    lua_setfield(L, -2, "pset");
    lua_pushcfunction(L, l_noop);
    lua_setfield(L, -2, "clear");
    lua_pushcfunction(L, l_noop);
    lua_setfield(L, -2, "blit");
    lua_setfield(L, -2, "surface");
    lua_setglobal(L, "blyt32");
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s cart.lua frames\n", argv[0]);
        return 2;
    }
    int frames = atoi(argv[2]);
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    setup(L);
    if (luaL_dofile(L, argv[1]) != LUA_OK) {
        fprintf(stderr, "load error: %s\n", lua_tostring(L, -1));
        return 1;
    }
    lua_getglobal(L, "init");
    if (lua_isfunction(L, -1)) lua_call(L, 0, 0);
    else lua_pop(L, 1);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int f = 0; f < frames; f++) {
        blyt_lua_lock_epoch++; /* per-frame epoch bump (matches the binding) */
        lua_getglobal(L, "update");
        if (lua_isfunction(L, -1)) lua_call(L, 0, 0); else lua_pop(L, 1);
        lua_getglobal(L, "draw");
        if (lua_isfunction(L, -1)) lua_call(L, 0, 0); else lua_pop(L, 1);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = ((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / frames;
    printf("%.3f ms/frame  (checksum %u)\n", ms, (unsigned)screen[320 * 100 + 50]);
    lua_close(L);
    return 0;
}
