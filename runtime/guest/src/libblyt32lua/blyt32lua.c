/*
 * runtime/guest/src/libblyt32lua/blyt32lua.c
 *
 * Guest-side Lua cart library.
 *
 * Provides blyt_cart_init/update/draw as strong symbols so the cart validator
 * accepts them from this library (via DT_NEEDED: libblyt32lua.so).
 *
 * Registers:
 *   blyt.quit()      → blyt_quit()
 *   blyt.debug.print(s)    → blyt_console_debug(s)
 *   blyt32.quit()    → blyt_quit()
 *   blyt32.debug.print(s)  → blyt_console_debug(s)
 *
 * cart_lua_modules(L) is a weak symbol; C/Rust src/lib/ libraries define it
 * to register additional Lua modules before the cart bytecode is loaded.
 *
 * cart_lua_bytecode / cart_lua_bytecode_size are weak; the per-cart generated
 * cart_lua_data.c object provides the strong definitions.
 */

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include <string.h>

#include "blyt.h"
#ifdef BLYT_DAP
#include "master_hook.h"
/* Weak default: 0 (hook disabled).  master_hook_ecall.c provides the strong
 * definition that probes the host via BLYT_ECALL_DAP_HOOK. */
int blyt_dap_active(void) __attribute__((weak));
int blyt_dap_active(void) {
    return 0;
}
/* Weak default no-op; strong definitions in master_hook_ecall.c (ECALL path)
 * and dap_transport_wasm.c (WASM path) call the actual transport. */
int blyt_dap_report_exception(lua_State *L, int is_uncaught) __attribute__((weak));
int blyt_dap_report_exception(lua_State *L, int is_uncaught) {
    (void)L;
    (void)is_uncaught;
    return 0;
}
static int dap_error_handler(lua_State *L) {
    if (blyt_dap_report_exception(L, 1))
        fc_dap_pause_loop(L, NULL);
    return 1;
}
#endif

/* Per-cart bytecode — defined in the generated cart_lua_data.c object. */
extern const unsigned char cart_lua_bytecode[] __attribute__((weak));
extern const unsigned int cart_lua_bytecode_size __attribute__((weak));

/* Per-cart Lua module registration — defined by src/lib/ libraries. */
void cart_lua_modules(lua_State *L) __attribute__((weak));

static lua_State *g_L;

/*
 * Minimal require() for sandboxed carts.
 *
 * Only pre-registered modules (placed in LUA_LOADED_TABLE by luaL_requiref
 * from cart_lua_modules) are visible.  Filesystem loading is not supported.
 */
static int lua_blyt_require(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
    lua_getfield(L, -1, name);
    if (!lua_isnil(L, -1)) {
        lua_remove(L, -2); /* remove _LOADED table */
        return 1;
    }
    lua_pop(L, 2);
    return luaL_error(L, "module '%s' not found (only pre-registered modules available)", name);
}

static int lua_blyt_debug_print(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    blyt_console_debug(s);
    return 0;
}

static int lua_blyt_quit(lua_State *L) {
    (void)L;
    blyt_quit();
    return 0;
}

static int lua_blyt_save_write(lua_State *L) {
    lua_pushinteger(L, blyt_save_write((uint32_t)luaL_checkinteger(L, 1)));
    return 1;
}
static int lua_blyt_save_read(lua_State *L) {
    lua_pushinteger(L, blyt_save_read((uint32_t)luaL_checkinteger(L, 1)));
    return 1;
}

/*
 * register_blyt32 — create the blyt and blyt32 globals.
 *
 * Per ADR-0086:
 *   blyt.*   — canonical home for shared modules (audio, state, input, quit …)
 *   blyt32.* — variant-specific (gfx, color, spatial) PLUS aliases to every
 *              shared module so that blyt32.foo == blyt.foo is always true.
 *
 * Pattern: build each shared subtable/function once, then copy the reference
 * into both tables so the Lua equality check holds.
 */
/* -------------------------------------------------------------------------
 * blyt32.buf.* — state buffer Lua API (ADR-0009, ADR-0010, ADR-0057, ADR-0058)
 * ------------------------------------------------------------------------- */

static int lua_buf_get_f32(lua_State *L) {
    lua_pushinteger(L, (int)blyt_buffer_get_f32((uint32_t)luaL_checkinteger(L, 1),
                                                (int32_t)luaL_checkinteger(L, 2),
                                                (uint32_t)luaL_checkinteger(L, 3)));
    return 1;
}
static int lua_buf_set_f32(lua_State *L) {
    blyt_buffer_set_f32((uint32_t)luaL_checkinteger(L, 1), (int32_t)luaL_checkinteger(L, 2),
                        (uint32_t)luaL_checkinteger(L, 3), (float)luaL_checknumber(L, 4));
    return 0;
}
/* f64 (Spike U): lua_Number is double, so the value round-trips losslessly. */
static int lua_buf_get_f64(lua_State *L) {
    lua_pushnumber(L, blyt_buffer_get_f64((uint32_t)luaL_checkinteger(L, 1),
                                          (int32_t)luaL_checkinteger(L, 2),
                                          (uint32_t)luaL_checkinteger(L, 3)));
    return 1;
}
static int lua_buf_set_f64(lua_State *L) {
    blyt_buffer_set_f64((uint32_t)luaL_checkinteger(L, 1), (int32_t)luaL_checkinteger(L, 2),
                        (uint32_t)luaL_checkinteger(L, 3), (double)luaL_checknumber(L, 4));
    return 0;
}
static int lua_buf_get_i32(lua_State *L) {
    lua_pushinteger(L, blyt_buffer_get_i32((uint32_t)luaL_checkinteger(L, 1),
                                           (int32_t)luaL_checkinteger(L, 2),
                                           (uint32_t)luaL_checkinteger(L, 3)));
    return 1;
}
static int lua_buf_set_i32(lua_State *L) {
    blyt_buffer_set_i32((uint32_t)luaL_checkinteger(L, 1), (int32_t)luaL_checkinteger(L, 2),
                        (uint32_t)luaL_checkinteger(L, 3), (int32_t)luaL_checkinteger(L, 4));
    return 0;
}
static int lua_buf_get_u32(lua_State *L) {
    lua_pushinteger(L, (int32_t)blyt_buffer_get_u32((uint32_t)luaL_checkinteger(L, 1),
                                                    (int32_t)luaL_checkinteger(L, 2),
                                                    (uint32_t)luaL_checkinteger(L, 3)));
    return 1;
}
static int lua_buf_set_u32(lua_State *L) {
    blyt_buffer_set_u32((uint32_t)luaL_checkinteger(L, 1), (int32_t)luaL_checkinteger(L, 2),
                        (uint32_t)luaL_checkinteger(L, 3), (uint32_t)luaL_checkinteger(L, 4));
    return 0;
}
static int lua_buf_get_i16(lua_State *L) {
    lua_pushinteger(L, blyt_buffer_get_i16((uint32_t)luaL_checkinteger(L, 1),
                                           (int32_t)luaL_checkinteger(L, 2),
                                           (uint32_t)luaL_checkinteger(L, 3)));
    return 1;
}
static int lua_buf_set_i16(lua_State *L) {
    blyt_buffer_set_i16((uint32_t)luaL_checkinteger(L, 1), (int32_t)luaL_checkinteger(L, 2),
                        (uint32_t)luaL_checkinteger(L, 3), (int16_t)luaL_checkinteger(L, 4));
    return 0;
}
static int lua_buf_get_u16(lua_State *L) {
    lua_pushinteger(L, blyt_buffer_get_u16((uint32_t)luaL_checkinteger(L, 1),
                                           (int32_t)luaL_checkinteger(L, 2),
                                           (uint32_t)luaL_checkinteger(L, 3)));
    return 1;
}
static int lua_buf_set_u16(lua_State *L) {
    blyt_buffer_set_u16((uint32_t)luaL_checkinteger(L, 1), (int32_t)luaL_checkinteger(L, 2),
                        (uint32_t)luaL_checkinteger(L, 3), (uint16_t)luaL_checkinteger(L, 4));
    return 0;
}
static int lua_buf_get_i8(lua_State *L) {
    lua_pushinteger(L, blyt_buffer_get_i8((uint32_t)luaL_checkinteger(L, 1),
                                          (int32_t)luaL_checkinteger(L, 2),
                                          (uint32_t)luaL_checkinteger(L, 3)));
    return 1;
}
static int lua_buf_set_i8(lua_State *L) {
    blyt_buffer_set_i8((uint32_t)luaL_checkinteger(L, 1), (int32_t)luaL_checkinteger(L, 2),
                       (uint32_t)luaL_checkinteger(L, 3), (int8_t)luaL_checkinteger(L, 4));
    return 0;
}
static int lua_buf_get_u8(lua_State *L) {
    lua_pushinteger(L, blyt_buffer_get_u8((uint32_t)luaL_checkinteger(L, 1),
                                          (int32_t)luaL_checkinteger(L, 2),
                                          (uint32_t)luaL_checkinteger(L, 3)));
    return 1;
}
static int lua_buf_set_u8(lua_State *L) {
    blyt_buffer_set_u8((uint32_t)luaL_checkinteger(L, 1), (int32_t)luaL_checkinteger(L, 2),
                       (uint32_t)luaL_checkinteger(L, 3), (uint8_t)luaL_checkinteger(L, 4));
    return 0;
}
static int lua_buf_get_bool(lua_State *L) {
    lua_pushboolean(L, blyt_buffer_get_bool((uint32_t)luaL_checkinteger(L, 1),
                                            (int32_t)luaL_checkinteger(L, 2),
                                            (uint32_t)luaL_checkinteger(L, 3)));
    return 1;
}
static int lua_buf_set_bool(lua_State *L) {
    blyt_buffer_set_bool((uint32_t)luaL_checkinteger(L, 1), (int32_t)luaL_checkinteger(L, 2),
                         (uint32_t)luaL_checkinteger(L, 3), (bool)lua_toboolean(L, 4));
    return 0;
}
static int lua_buf_alloc_slot(lua_State *L) {
    int32_t slot = -1;
    blyt_buffer_alloc_slot((uint32_t)luaL_checkinteger(L, 1), &slot);
    lua_pushinteger(L, slot);
    return 1;
}
static int lua_buf_free_slot(lua_State *L) {
    blyt_buffer_free_slot((uint32_t)luaL_checkinteger(L, 1), (int32_t)luaL_checkinteger(L, 2));
    return 0;
}
/* Packed entity refs (ADR-0096) */
static int lua_buf_ref(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)blyt_buffer_ref((uint32_t)luaL_checkinteger(L, 1),
                                                    (int32_t)luaL_checkinteger(L, 2)));
    return 1;
}
static int lua_buf_ref_valid(lua_State *L) {
    lua_pushboolean(L, blyt_buffer_ref_valid((uint32_t)luaL_checkinteger(L, 1),
                                             (uint32_t)luaL_checkinteger(L, 2)));
    return 1;
}
static int lua_buf_ref_slot(lua_State *L) {
    /* Pure bit math — no ECALL (mirrors the static inline in blyt.h). */
    lua_pushinteger(L, (lua_Integer)((uint32_t)luaL_checkinteger(L, 1) & 0xFFFFu));
    return 1;
}

/* -------------------------------------------------------------------------
 * blyt.resource.* — typed resource API (#93/#166; ADR-0027, ADR-0068, ADR-0086,
 * ADR-0120).  Thin Lua layer over the resource lifecycle ECALLs (#123):
 *   blyt.resource.text_resource(id)  -> text constant object
 *   blyt.resource.bytes_resource(id) -> bytes constant object
 *   blyt.resource.pin(id)            -> ptr, size | nil  (frame-scoped raw bytes,
 *                                       kind-agnostic id-based escape hatch)
 *   blyt.resource.unpin(id)
 *   const:load() -> kind-specific handle object | nil
 *   const:id()   -> integer id
 *   text_handle:text()   -> owned Lua string, trailing storage NUL stripped (#166)
 *   bytes_handle:bytes() -> owned Lua string of the exact bytes (verbatim)
 *   handle:release()     -> advisory release (also run by __gc)
 * The typed-ness (ADR-0068 amendment 2026-06-27) is the metatable: :text() is
 * absent from the bytes handle and vice-versa, so the wrong accessor raises
 * "attempt to call a nil value".  Handle metamethods: __gc, __eq, __tostring.
 * Mirrored identically in the WASM host-Lua fast path (wasm_main.c).
 * ------------------------------------------------------------------------- */

#define BLYT_RESOURCE_CONST_MT "blyt.resource.const"
#define BLYT_RESOURCE_TEXT_HANDLE_MT "blyt.resource.text_handle"
#define BLYT_RESOURCE_BYTES_HANDLE_MT "blyt.resource.bytes_handle"

/* A typed resource constant (the generated R.<NAME>): an id plus its kind. */
typedef struct {
    blyt_resource_id_t id;
    int is_text;
} lua_resource_const_t;

typedef struct {
    blyt_resource_id_t id;
    blyt_resource_h handle;
    int released; /* 1 once release has run (explicit or __gc) — avoids double release */
} lua_resource_handle_t;

/* A loaded handle is either metatable; release/__gc/__eq/__tostring are shared. */
static lua_resource_handle_t *lua_opt_handle(lua_State *L, int idx) {
    void *p = luaL_testudata(L, idx, BLYT_RESOURCE_TEXT_HANDLE_MT);
    if (!p)
        p = luaL_testudata(L, idx, BLYT_RESOURCE_BYTES_HANDLE_MT);
    return (lua_resource_handle_t *)p;
}

static int lua_text_resource(lua_State *L) {
    blyt_resource_id_t id = (blyt_resource_id_t)luaL_checkinteger(L, 1);
    lua_resource_const_t *c = (lua_resource_const_t *)lua_newuserdatauv(L, sizeof(*c), 0);
    c->id = id;
    c->is_text = 1;
    luaL_setmetatable(L, BLYT_RESOURCE_CONST_MT);
    return 1;
}

static int lua_bytes_resource(lua_State *L) {
    blyt_resource_id_t id = (blyt_resource_id_t)luaL_checkinteger(L, 1);
    lua_resource_const_t *c = (lua_resource_const_t *)lua_newuserdatauv(L, sizeof(*c), 0);
    c->id = id;
    c->is_text = 0;
    luaL_setmetatable(L, BLYT_RESOURCE_CONST_MT);
    return 1;
}

static int lua_const_load(lua_State *L) {
    lua_resource_const_t *c = luaL_checkudata(L, 1, BLYT_RESOURCE_CONST_MT);
    blyt_resource_h h = BLYT_RESOURCE_INVALID;
    if (blyt_resource_load(c->id, &h) != BLYT_OK || h == BLYT_RESOURCE_INVALID) {
        lua_pushnil(L);
        return 1;
    }
    lua_resource_handle_t *uh = (lua_resource_handle_t *)lua_newuserdatauv(L, sizeof(*uh), 0);
    uh->id = c->id;
    uh->handle = h;
    uh->released = 0;
    luaL_setmetatable(L, c->is_text ? BLYT_RESOURCE_TEXT_HANDLE_MT : BLYT_RESOURCE_BYTES_HANDLE_MT);
    return 1;
}

static int lua_const_id(lua_State *L) {
    lua_resource_const_t *c = luaL_checkudata(L, 1, BLYT_RESOURCE_CONST_MT);
    lua_pushinteger(L, (lua_Integer)c->id);
    return 1;
}

static int lua_const_eq(lua_State *L) {
    lua_resource_const_t *a = luaL_checkudata(L, 1, BLYT_RESOURCE_CONST_MT);
    lua_resource_const_t *b = luaL_checkudata(L, 2, BLYT_RESOURCE_CONST_MT);
    lua_pushboolean(L, a->id == b->id && a->is_text == b->is_text);
    return 1;
}

static int lua_const_tostring(lua_State *L) {
    lua_resource_const_t *c = luaL_checkudata(L, 1, BLYT_RESOURCE_CONST_MT);
    lua_pushfstring(L, c->is_text ? "text_resource<%d>" : "bytes_resource<%d>", (int)c->id);
    return 1;
}

/* text handle :text() — owned copy with the trailing storage NUL stripped (#166)
 * so #s == content length.  The build guarantees the only NUL is the trailing
 * one, so reporting size-1 is exact; this is also the "is it really text" check. */
static int lua_handle_text(lua_State *L) {
    lua_resource_handle_t *uh = luaL_checkudata(L, 1, BLYT_RESOURCE_TEXT_HANDLE_MT);
    const void *ptr = NULL;
    size_t size = 0;
    if (blyt_resource_pin(uh->id, &ptr, &size) != BLYT_OK || !ptr) {
        lua_pushnil(L);
        return 1;
    }
    size_t content = (size >= 1 && ((const char *)ptr)[size - 1] == '\0') ? size - 1 : size;
    lua_pushlstring(L, (const char *)ptr, content);
    blyt_resource_unpin(uh->id);
    return 1;
}

/* bytes handle :bytes() — owned copy of the exact bytes (verbatim).  A Lua string
 * is an 8-bit-clean byte buffer, so it round-trips opaque bytes (embedded NULs,
 * high bytes) faithfully (#162). */
static int lua_handle_bytes(lua_State *L) {
    lua_resource_handle_t *uh = luaL_checkudata(L, 1, BLYT_RESOURCE_BYTES_HANDLE_MT);
    const void *ptr = NULL;
    size_t size = 0;
    if (blyt_resource_pin(uh->id, &ptr, &size) != BLYT_OK || !ptr) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, (const char *)ptr, size);
    blyt_resource_unpin(uh->id);
    return 1;
}

static int lua_handle_release(lua_State *L) {
    lua_resource_handle_t *uh = lua_opt_handle(L, 1);
    luaL_argcheck(L, uh != NULL, 1, "resource handle expected");
    if (!uh->released) {
        blyt_resource_release(uh->handle);
        uh->released = 1;
    }
    return 0;
}

static int lua_handle_eq(lua_State *L) {
    lua_resource_handle_t *a = lua_opt_handle(L, 1);
    lua_resource_handle_t *b = lua_opt_handle(L, 2);
    lua_pushboolean(L, a && b && a->handle == b->handle && a->id == b->id);
    return 1;
}

static int lua_handle_tostring(lua_State *L) {
    lua_resource_handle_t *uh = lua_opt_handle(L, 1);
    luaL_argcheck(L, uh != NULL, 1, "resource handle expected");
    lua_pushfstring(L, "resource<%d>", (int)uh->id);
    return 1;
}

/* Module-level pin/unpin take the integer id directly (ADR-0120): pin returns a
 * lightuserdata pointer + size, valid only within the current frame.  These stay
 * kind-agnostic — the raw escape hatch for hybrid carts (#166). */
static int lua_resource_pin(lua_State *L) {
    blyt_resource_id_t id = (blyt_resource_id_t)luaL_checkinteger(L, 1);
    const void *ptr = NULL;
    size_t size = 0;
    if (blyt_resource_pin(id, &ptr, &size) != BLYT_OK || !ptr) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlightuserdata(L, (void *)ptr);
    lua_pushinteger(L, (lua_Integer)size);
    return 2;
}

static int lua_resource_unpin(lua_State *L) {
    blyt_resource_unpin((blyt_resource_id_t)luaL_checkinteger(L, 1));
    return 0;
}

/* Build a loaded-handle metatable carrying the kind-specific accessor (`text` or
 * `bytes`) plus the shared release/__gc/__eq/__tostring.  Leaves nothing on the
 * stack. */
static void register_handle_mt(lua_State *L, const char *mt_name, lua_CFunction accessor,
                               const char *accessor_name) {
    luaL_newmetatable(L, mt_name);
    lua_pushcfunction(L, lua_handle_release); /* __gc: idempotent release */
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, lua_handle_eq);
    lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, lua_handle_tostring);
    lua_setfield(L, -2, "__tostring");
    lua_newtable(L); /* __index method table */
    lua_pushcfunction(L, accessor);
    lua_setfield(L, -2, accessor_name);
    lua_pushcfunction(L, lua_handle_release);
    lua_setfield(L, -2, "release");
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1); /* pop mt */
}

/* Build the constant + handle metatables and the blyt.resource module table,
 * aliasing blyt32.resource == blyt.resource (ADR-0086 shared module).  Call after
 * the blyt and blyt32 globals exist. */
static void register_resource_module(lua_State *L) {
    /* Resource-constant metatable: :load(), :id(), __eq, __tostring. */
    luaL_newmetatable(L, BLYT_RESOURCE_CONST_MT);
    lua_pushcfunction(L, lua_const_eq);
    lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, lua_const_tostring);
    lua_setfield(L, -2, "__tostring");
    lua_newtable(L); /* __index */
    lua_pushcfunction(L, lua_const_load);
    lua_setfield(L, -2, "load");
    lua_pushcfunction(L, lua_const_id);
    lua_setfield(L, -2, "id");
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1); /* pop const mt */

    register_handle_mt(L, BLYT_RESOURCE_TEXT_HANDLE_MT, lua_handle_text, "text");
    register_handle_mt(L, BLYT_RESOURCE_BYTES_HANDLE_MT, lua_handle_bytes, "bytes");

    lua_newtable(L); /* resource module */
    lua_pushcfunction(L, lua_text_resource);
    lua_setfield(L, -2, "text_resource");
    lua_pushcfunction(L, lua_bytes_resource);
    lua_setfield(L, -2, "bytes_resource");
    lua_pushcfunction(L, lua_resource_pin);
    lua_setfield(L, -2, "pin");
    lua_pushcfunction(L, lua_resource_unpin);
    lua_setfield(L, -2, "unpin");

    lua_getglobal(L, "blyt");
    lua_pushvalue(L, -2); /* resource */
    lua_setfield(L, -2, "resource");
    lua_pop(L, 1); /* pop blyt */
    lua_getglobal(L, "blyt32");
    lua_pushvalue(L, -2); /* resource */
    lua_setfield(L, -2, "resource");
    lua_pop(L, 1); /* pop blyt32 */
    lua_pop(L, 1); /* pop resource module */
}

static void register_blyt32(lua_State *L) {
    /* --- shared: blyt.debug subtable --- */
    lua_newtable(L); /* idx A: blyt.debug */
    lua_pushcfunction(L, lua_blyt_debug_print);
    lua_setfield(L, -2, "print");

    /* --- blyt table --- */
    lua_newtable(L); /* blyt */
    lua_pushvalue(L, -2); /* copy ref to blyt.debug */
    lua_setfield(L, -2, "debug");
    lua_pushcfunction(L, lua_blyt_quit); /* shared: blyt.quit */
    lua_setfield(L, -2, "quit");
    lua_pushcfunction(L, lua_blyt_save_write);
    lua_setfield(L, -2, "save_write");
    lua_pushcfunction(L, lua_blyt_save_read);
    lua_setfield(L, -2, "save_read");
    lua_setglobal(L, "blyt"); /* pops blyt */

    /* --- shared: blyt.buf subtable (state buffer API, ADR-0009) --- */
    lua_newtable(L); /* idx B: blyt.buf */
    static const struct {
        const char *name;
        lua_CFunction fn;
    } buf_fns[] = {
        {"get_f32", lua_buf_get_f32},
        {"set_f32", lua_buf_set_f32},
        {"get_f64", lua_buf_get_f64},
        {"set_f64", lua_buf_set_f64},
        {"get_i32", lua_buf_get_i32},
        {"set_i32", lua_buf_set_i32},
        {"get_u32", lua_buf_get_u32},
        {"set_u32", lua_buf_set_u32},
        {"get_i16", lua_buf_get_i16},
        {"set_i16", lua_buf_set_i16},
        {"get_u16", lua_buf_get_u16},
        {"set_u16", lua_buf_set_u16},
        {"get_i8", lua_buf_get_i8},
        {"set_i8", lua_buf_set_i8},
        {"get_u8", lua_buf_get_u8},
        {"set_u8", lua_buf_set_u8},
        {"get_bool", lua_buf_get_bool},
        {"set_bool", lua_buf_set_bool},
        {"alloc_slot", lua_buf_alloc_slot},
        {"free_slot", lua_buf_free_slot},
        {"ref", lua_buf_ref},
        {"ref_valid", lua_buf_ref_valid},
        {"ref_slot", lua_buf_ref_slot},
        {NULL, NULL},
    };
    for (int i = 0; buf_fns[i].name; i++) {
        lua_pushcfunction(L, buf_fns[i].fn);
        lua_setfield(L, -2, buf_fns[i].name);
    }
    /* Register as blyt.buf */
    lua_getglobal(L, "blyt");
    lua_pushvalue(L, -2); /* copy ref to blyt.buf */
    lua_setfield(L, -2, "buf");
    lua_pop(L, 1); /* pop blyt */

    /* --- blyt32 table — distinct table; shared entries alias blyt.* --- */
    lua_newtable(L); /* blyt32 */
    lua_pushvalue(L, -3); /* copy ref to blyt.debug (idx A still on stack) */
    lua_setfield(L, -2, "debug");
    lua_getglobal(L, "blyt");
    lua_getfield(L, -1, "quit"); /* blyt.quit */
    lua_setfield(L, -3, "quit"); /* blyt32.quit = blyt.quit */
    lua_getfield(L, -1, "save_write"); /* blyt.save_write */
    lua_setfield(L, -3, "save_write"); /* blyt32.save_write = blyt.save_write */
    lua_getfield(L, -1, "save_read"); /* blyt.save_read */
    lua_setfield(L, -3, "save_read"); /* blyt32.save_read = blyt.save_read */
    lua_getfield(L, -1, "buf"); /* blyt.buf */
    lua_setfield(L, -3, "buf"); /* blyt32.buf = blyt.buf */
    lua_pop(L, 1); /* pop blyt */
    lua_setglobal(L, "blyt32"); /* pops blyt32 */

    lua_pop(L, 1); /* pop blyt.buf (idx B) */

    lua_pop(L, 1); /* pop blyt.debug (idx A) */

    register_resource_module(L); /* blyt.resource.* + blyt32.resource.* (#93) */

    lua_pushcfunction(L, lua_blyt_require);
    lua_setglobal(L, "require");
}

/*
 * Derive a require()-able module name from a loaded chunk's embedded source
 * name (the basename, minus ".lua").  The chunk function must be on the stack
 * top; it is left untouched.  Writes "" if the source is unavailable.
 *
 * This lets a bundled chunk that returns a table (the packer-generated
 * `cart_resources`, ADR-0040) be registered into LUA_LOADED_TABLE so the
 * sandboxed require (lua_blyt_require) resolves it — the same path the WASM
 * host-Lua loader uses, so the two stay byte-for-byte behaviourally identical.
 */
static void chunk_module_name(lua_State *L, char *out, size_t outsz) {
    out[0] = '\0';
    lua_Debug ar;
    lua_pushvalue(L, -1); /* dup the chunk function for getinfo to consume */
    if (lua_getinfo(L, ">S", &ar) && ar.source) {
        const char *src = ar.source;
        if (*src == '@' || *src == '=')
            src++;
        const char *base = src;
        for (const char *p = src; *p; p++)
            if (*p == '/' || *p == '\\')
                base = p + 1;
        size_t len = strlen(base);
        if (len > 4 && strcmp(base + len - 4, ".lua") == 0)
            len -= 4;
        if (len >= outsz)
            len = outsz - 1;
        memcpy(out, base, len);
        out[len] = '\0';
    }
}

static lua_State *open_state(void) {
    blyt_console_debug("open_state: before luaL_newstate");
    lua_State *L = luaL_newstate();
    blyt_console_debug(L ? "open_state: newstate ok" : "open_state: newstate NULL");
    if (!L)
        return NULL;

    /* Open the same library set as the WASM host-side state (wasm_main.c
     * run_lua_cart): base, math, string, table, coroutine, utf8.  The two
     * paths must expose identical globals or carts behave differently per
     * target (Spike T stage 1 found pairs() present on WASM, absent here).
     * utf8 is allowed by ADR-0079 (read-only, deterministic; issue #167). */
    luaL_requiref(L, "_G", luaopen_base, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "math", luaopen_math, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "string", luaopen_string, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "table", luaopen_table, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "coroutine", luaopen_coroutine, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    lua_pop(L, 1);
    blyt_console_debug("open_state: stdlib subset opened");

    blyt_console_debug("open_state: before register_blyt32");
    register_blyt32(L);
    blyt_console_debug("open_state: register_blyt32 done");

    if (cart_lua_modules)
        cart_lua_modules(L);

    if (cart_lua_bytecode && cart_lua_bytecode_size) {
        blyt_console_debug("open_state: before load lua bytecode");
        const unsigned char *data = cart_lua_bytecode;
        unsigned int remaining = cart_lua_bytecode_size;

        if (remaining >= 8 && data[0] == 'B' && data[1] == 'L' && data[2] == 'M' &&
            data[3] == 'C') {
            /* BLMC multi-chunk format: each file compiled as its own chunk
             * with its own source name and line numbers (issue #54). */
            unsigned int nchunks = (unsigned int)data[4] | ((unsigned int)data[5] << 8) |
                                   ((unsigned int)data[6] << 16) | ((unsigned int)data[7] << 24);
            data += 8;
            remaining -= 8;
            for (unsigned int ci = 0; ci < nchunks; ci++) {
                if (remaining < 4) {
                    blyt_console_debug("open_state: BLMC truncated");
                    lua_close(L);
                    return NULL;
                }
                unsigned int csz = (unsigned int)data[0] | ((unsigned int)data[1] << 8) |
                                   ((unsigned int)data[2] << 16) | ((unsigned int)data[3] << 24);
                data += 4;
                remaining -= 4;
                if (csz > remaining) {
                    blyt_console_debug("open_state: BLMC chunk size overflow");
                    lua_close(L);
                    return NULL;
                }
                if (luaL_loadbuffer(L, (const char *)data, csz, "@chunk") != LUA_OK) {
                    blyt_console_debug(lua_tostring(L, -1));
                    lua_close(L);
                    return NULL;
                }
                char modname[64];
                chunk_module_name(L, modname, sizeof(modname));
                if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
                    blyt_console_debug(lua_tostring(L, -1));
                    lua_close(L);
                    return NULL;
                }
                /* Register a non-nil table return as a require()-able module
                 * (e.g. the packer-generated cart_resources, ADR-0040). */
                if (modname[0] != '\0' && !lua_isnil(L, -1)) {
                    luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
                    lua_pushvalue(L, -2); /* the chunk's return value */
                    lua_setfield(L, -2, modname);
                    lua_pop(L, 1); /* pop _LOADED */
                }
                lua_pop(L, 1); /* pop the chunk return value */
                data += csz;
                remaining -= csz;
            }
        } else {
            int load_result = luaL_loadbuffer(L, (const char *)cart_lua_bytecode,
                                              cart_lua_bytecode_size, "@cart");
            blyt_console_debug(load_result == LUA_OK ? "open_state: loadbuffer OK"
                                                     : "open_state: loadbuffer FAILED");
            if (load_result != LUA_OK) {
                blyt_console_debug(lua_tostring(L, -1));
                lua_close(L);
                return NULL;
            }
            blyt_console_debug("open_state: before lua_pcall");
            if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
                blyt_console_debug(lua_tostring(L, -1));
                lua_close(L);
                return NULL;
            }
            blyt_console_debug("open_state: lua_pcall done");
        }
        blyt_console_debug("open_state: lua bytecode loaded");
    }

    return L;
}

static void call_global(const char *name) {
    if (!g_L)
        return;
    int msgh = 0;
#ifdef BLYT_DAP
    if (blyt_dap_active()) {
        lua_pushcfunction(g_L, dap_error_handler);
        msgh = lua_gettop(g_L);
    }
#endif
    lua_getglobal(g_L, name);
    if (lua_isfunction(g_L, -1)) {
        if (lua_pcall(g_L, 0, 0, msgh) != LUA_OK) {
            blyt_console_debug(lua_tostring(g_L, -1));
            lua_pop(g_L, 1);
        }
    } else {
        lua_pop(g_L, 1);
    }
#ifdef BLYT_DAP
    if (msgh > 0)
        lua_remove(g_L, msgh);
#endif
}

void blyt_cart_init(void) {
    blyt_console_debug("blyt_cart_init: start");
    g_L = open_state();
    blyt_console_debug(g_L ? "blyt_cart_init: open_state ok" : "blyt_cart_init: open_state FAILED");
#ifdef BLYT_DAP
    if (g_L && blyt_dap_active()) {
        fc_master_hook_cfg.dap_enabled = true;
        fc_consolelua_master_hook_install(g_L);
    }
#endif
    call_global("init");
    blyt_console_debug("blyt_cart_init: done");
}

void blyt_cart_update(void) {
    call_global("update");
}
void blyt_cart_draw(void) {
    call_global("draw");
}
void blyt_cart_on_new_state(void) {
    call_global("on_new_state");
}
void blyt_cart_on_save_state(void) {
    call_global("on_save_state");
}
void blyt_cart_on_load_state(blyt_load_info_t info) {
    if (!g_L)
        return;
    int msgh = 0;
#ifdef BLYT_DAP
    if (blyt_dap_active()) {
        lua_pushcfunction(g_L, dap_error_handler);
        msgh = lua_gettop(g_L);
    }
#endif
    lua_getglobal(g_L, "on_load_state");
    if (lua_isfunction(g_L, -1)) {
        /* Marshal blyt_load_info_t into a single `info` table argument,
         * mirroring the WASM host-Lua path (frontends/wasm/wasm_main.c).
         * `buffers` is NULL until Phase 9, so only the scalar fields are
         * exposed for now. */
        lua_newtable(g_L);
        lua_pushinteger(g_L, (lua_Integer)info.reason);
        lua_setfield(g_L, -2, "reason");
        lua_pushinteger(g_L, (lua_Integer)info.saved_cart_version);
        lua_setfield(g_L, -2, "saved_cart_version");
        if (lua_pcall(g_L, 1, 0, msgh) != LUA_OK) {
            blyt_console_debug(lua_tostring(g_L, -1));
            lua_pop(g_L, 1);
        }
    } else {
        lua_pop(g_L, 1);
    }
#ifdef BLYT_DAP
    if (msgh > 0)
        lua_remove(g_L, msgh);
#endif
}

/* Dev-only asset hot-swap hook (issue #122).  The host marshals the changed
 * resource ids into guest memory and calls this; we present them to the cart as
 * a 1-based integer array `ids` passed to the global on_assets_reloaded(ids). */
void blyt_cart_on_assets_reloaded(const uint32_t *ids, size_t n) {
    if (!g_L)
        return;
    int msgh = 0;
#ifdef BLYT_DAP
    if (blyt_dap_active()) {
        lua_pushcfunction(g_L, dap_error_handler);
        msgh = lua_gettop(g_L);
    }
#endif
    lua_getglobal(g_L, "on_assets_reloaded");
    if (lua_isfunction(g_L, -1)) {
        lua_createtable(g_L, (int)n, 0);
        for (size_t i = 0; i < n; i++) {
            lua_pushinteger(g_L, (lua_Integer)ids[i]);
            lua_seti(g_L, -2, (lua_Integer)(i + 1));
        }
        if (lua_pcall(g_L, 1, 0, msgh) != LUA_OK) {
            blyt_console_debug(lua_tostring(g_L, -1));
            lua_pop(g_L, 1);
        }
    } else {
        lua_pop(g_L, 1);
    }
#ifdef BLYT_DAP
    if (msgh > 0)
        lua_remove(g_L, msgh);
#endif
}
