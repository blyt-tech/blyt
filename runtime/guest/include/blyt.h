#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Cart lifecycle entry points (ADR-0087)
 *
 * Required — the runtime verifies all three are present at cart load time.
 * The cart defines these; libblytcommon.so's blyt_main calls them in order:
 *   init → on_new_state → [update → draw] loop → on_quit → cleanup
 * ------------------------------------------------------------------------- */
void blyt_cart_init(void);
void blyt_cart_update(void);
void blyt_cart_draw(void);

/* Optional — libblytcommon.so provides weak no-op defaults for these. */
void blyt_cart_on_new_state(void);
void blyt_cart_on_save_state(void);
void blyt_cart_on_quit(void);
void blyt_cart_cleanup(void);

/* -------------------------------------------------------------------------
 * Cart signals to the runtime (ADR-0087)
 * ------------------------------------------------------------------------- */

/* Signal that the cart is ready to exit the update/draw loop.
 * Call from blyt_cart_on_quit, or directly from blyt_cart_update when the
 * cart decides it is finished (e.g. after showing a credits sequence). */
void blyt_quit(void);

/* Signal the end of one update+draw frame to the host runtime.
 * Called automatically by blyt_main after each blyt_cart_draw(); cart code
 * does not need to call this directly. */
void blyt_frame_done(void);

/* -------------------------------------------------------------------------
 * Debug output (ADR-0085, ECALL 1)
 * ------------------------------------------------------------------------- */
void blyt_console_debug(const char *s);

/* -------------------------------------------------------------------------
 * Lua export macros (ADR-0111) — hybrid Lua+C carts
 *
 * Usage:
 *   BLYT_LUA_EXPORT_VOID(my_fn) { ... }
 *   BLYT_LUA_EXPORT_I32(my_fn, int32_t x) { return x + 1; }
 *
 * Requires <lua.h> to be included before these macros are used.
 * Each macro:
 *   1. Emits a Lua C wrapper (__lua_export_NAME) for SDL2/libretro.
 *   2. Emits a registration helper (__lua_reg_NAME) in .lua_regtab, iterated
 *      by the generated cart_lua_modules glue on SDL2/libretro.
 *   3. Emits a blyt_lua_export_entry_t in .lua_exports, parsed by the WASM
 *      host to build trampolines that call the function via rv32emu.
 *   4. Defines the underlying C function (user writes the body).
 * ------------------------------------------------------------------------- */

/* Type codes for BLYT_LUA_EXPORT primitive types. */
#define BLYT_LUA_TYPE_VOID 0
#define BLYT_LUA_TYPE_I32 1
#define BLYT_LUA_TYPE_U32 2
#define BLYT_LUA_TYPE_F32 3
#define BLYT_LUA_TYPE_BOOL 4

/* One entry per exported function, placed in .lua_exports by the macros below.
 * The host reads this section to resolve guest addresses without raw pointers
 * (which would require relocation processing). */
typedef struct {
    char lua_name[32]; /* Lua global name */
    char fn_sym[64]; /* underlying C function symbol name */
    char wrap_sym[64]; /* Lua C wrapper symbol: __lua_export_<fn_sym> */
    uint8_t nargs;
    uint8_t arg_types[4]; /* BLYT_LUA_TYPE_* for each arg */
    uint8_t ret_type; /* BLYT_LUA_TYPE_* */
    uint8_t _pad[2];
} blyt_lua_export_entry_t; /* 168 bytes */

/* 0 args, void return */
/* clang-format off */
#define BLYT_LUA_EXPORT_VOID(name) \
    void name(void); \
    static int __lua_export_##name(lua_State *L) { \
        (void)L; name(); return 0; \
    } \
    static void __lua_reg_##name(lua_State *L) { \
        lua_pushcfunction(L, __lua_export_##name); \
        lua_setglobal(L, #name); \
    } \
    static void (*__lua_regptr_##name)(lua_State *) \
        __attribute__((used, retain, section(".lua_regtab"))) = __lua_reg_##name; \
    static const blyt_lua_export_entry_t __export_##name \
        __attribute__((used, retain, section(".lua_exports"))) = { \
        #name, #name, "__lua_export_" #name, \
        0, {0, 0, 0, 0}, BLYT_LUA_TYPE_VOID, {0, 0} \
    }; \
    void name(void)

/* 1 I32 arg (...), I32 return.  The variadic arg is the C parameter declaration
 * (e.g. "int32_t x"); the wrapper reads one integer from the Lua stack. */
#define BLYT_LUA_EXPORT_I32(name, ...) \
    int32_t name(__VA_ARGS__); \
    static int __lua_export_##name(lua_State *L) { \
        int32_t _a0 = (int32_t)lua_tointeger(L, 1); \
        int32_t _r = name(_a0); \
        lua_pushinteger(L, (lua_Integer)_r); \
        return 1; \
    } \
    static void __lua_reg_##name(lua_State *L) { \
        lua_pushcfunction(L, __lua_export_##name); \
        lua_setglobal(L, #name); \
    } \
    static void (*__lua_regptr_##name)(lua_State *) \
        __attribute__((used, retain, section(".lua_regtab"))) = __lua_reg_##name; \
    static const blyt_lua_export_entry_t __export_##name \
        __attribute__((used, retain, section(".lua_exports"))) = { \
        #name, #name, "__lua_export_" #name, \
        1, {BLYT_LUA_TYPE_I32, 0, 0, 0}, BLYT_LUA_TYPE_I32, {0, 0} \
    }; \
    int32_t name(__VA_ARGS__)
/* clang-format on */

#ifdef __cplusplus
}
#endif
