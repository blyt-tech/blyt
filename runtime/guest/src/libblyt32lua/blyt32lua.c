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

/* Graphics primitives (blyt32.gfx.*, ADR-0086; issue #188 / Spike X).  The
 * paletted 2D surface is variant-specific, so these live under blyt32, not blyt.
 * Each forwards to the libblyt32.so primitive (an ECALL into the host runtime on
 * the emulated path); the host-Lua fast path (wasm_main.c) registers its own
 * same-named binding over the shared rasterizer.  Colours are palette indices. */
static int lua_blyt_gfx_clear(lua_State *L) {
    blyt_gfx_clear((uint8_t)luaL_checkinteger(L, 1));
    return 0;
}
static int lua_blyt_gfx_pixel(lua_State *L) {
    blyt_gfx_pixel((int32_t)luaL_checkinteger(L, 1), (int32_t)luaL_checkinteger(L, 2),
                   (uint8_t)luaL_checkinteger(L, 3));
    return 0;
}
static int lua_blyt_gfx_rect_fill(lua_State *L) {
    blyt_gfx_rect_fill((int32_t)luaL_checkinteger(L, 1), (int32_t)luaL_checkinteger(L, 2),
                       (int32_t)luaL_checkinteger(L, 3), (int32_t)luaL_checkinteger(L, 4),
                       (uint8_t)luaL_checkinteger(L, 5));
    return 0;
}
static int lua_blyt_gfx_line(lua_State *L) {
    blyt_gfx_line((int32_t)luaL_checkinteger(L, 1), (int32_t)luaL_checkinteger(L, 2),
                  (int32_t)luaL_checkinteger(L, 3), (int32_t)luaL_checkinteger(L, 4),
                  (uint8_t)luaL_checkinteger(L, 5));
    return 0;
}

/* Palette load (#201/#214): lua_blyt_gfx_palette_set is defined with the
 * resource-constant machinery below (it recognises the typed palette constant
 * R.<NAME>) and forward-declared here for the gfx method table. */
static int lua_blyt_gfx_palette_set(lua_State *L);

/* Surface tier-1 bindings (blyt32.surface.*, #205).  Lua is tier-1 only: the
 * canvas is the explicit first argument (blyt32.surface.SCREEN or a handle from
 * create), source images/surfaces are arguments.  Each forwards to the
 * libblyt32.so surface primitive (an ECALL into the host on the emulated path).
 * There is no acquire/release binding — the per-pixel tier-2 lock is C/Rust and
 * native-only (the Lua per-pixel fast path is deferred).  Handles are u32 but
 * fit a positive lua_Integer (i32): SURFACE/LOCKVIEW kinds keep bit 31 clear. */
static int lua_blyt_surface_create(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)blyt_surface_create((int32_t)luaL_checkinteger(L, 1),
                                                        (int32_t)luaL_checkinteger(L, 2)));
    return 1;
}
static int lua_blyt_surface_destroy(lua_State *L) {
    blyt_surface_destroy((blyt_surface_h)luaL_checkinteger(L, 1));
    return 0;
}
static int lua_blyt_surface_clear(lua_State *L) {
    blyt_surface_clear((blyt_surface_h)luaL_checkinteger(L, 1), (uint8_t)luaL_checkinteger(L, 2));
    return 0;
}
static int lua_blyt_surface_pixel(lua_State *L) {
    blyt_surface_pixel((blyt_surface_h)luaL_checkinteger(L, 1), (int32_t)luaL_checkinteger(L, 2),
                       (int32_t)luaL_checkinteger(L, 3), (uint8_t)luaL_checkinteger(L, 4));
    return 0;
}
static int lua_blyt_surface_rect_fill(lua_State *L) {
    blyt_surface_rect_fill((blyt_surface_h)luaL_checkinteger(L, 1),
                           (int32_t)luaL_checkinteger(L, 2), (int32_t)luaL_checkinteger(L, 3),
                           (int32_t)luaL_checkinteger(L, 4), (int32_t)luaL_checkinteger(L, 5),
                           (uint8_t)luaL_checkinteger(L, 6));
    return 0;
}
static int lua_blyt_surface_line(lua_State *L) {
    blyt_surface_line((blyt_surface_h)luaL_checkinteger(L, 1), (int32_t)luaL_checkinteger(L, 2),
                      (int32_t)luaL_checkinteger(L, 3), (int32_t)luaL_checkinteger(L, 4),
                      (int32_t)luaL_checkinteger(L, 5), (uint8_t)luaL_checkinteger(L, 6));
    return 0;
}
static int lua_blyt_surface_blit(lua_State *L) {
    blyt_surface_blit((blyt_surface_h)luaL_checkinteger(L, 1),
                      (blyt_surface_h)luaL_checkinteger(L, 2), (int32_t)luaL_checkinteger(L, 3),
                      (int32_t)luaL_checkinteger(L, 4));
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
 * blyt.resource.* — typed resource API (#93/#166/#196; ADR-0027, ADR-0068,
 * ADR-0086, ADR-0120, ADR-0134).  Thin Lua layer over the resource pin ECALLs:
 *   blyt.resource.text_resource(id)  -> text constant object
 *   blyt.resource.bytes_resource(id) -> bytes constant object
 *   blyt.resource.pin(id)            -> ptr, size | nil  (frame-scoped raw bytes,
 *                                       kind-agnostic, takes the constant)
 *   blyt.resource.unpin(id)
 *   const:id()    -> integer (the baked constant value)
 *   text_const:text()   -> owned Lua string, trailing storage NUL stripped (#166)
 *   bytes_const:bytes() -> owned Lua string of the exact bytes (verbatim)
 * Resources are referenced by their constant directly (ADR-0134, #196): there is
 * no load handle to track or release — :text()/:bytes() pin/unpin under the hood.
 * The typed-ness (ADR-0068 amendment 2026-06-27) is the metatable: :text() is
 * absent from a bytes constant and vice-versa, so the wrong accessor raises
 * "attempt to call a nil value".  Constant metamethods: __eq, __tostring.
 * Mirrored identically in the WASM host-Lua fast path (wasm_main.c).
 * ------------------------------------------------------------------------- */

/* Kind-specific constant metatables (ADR-0068 / #166): the typed-ness is the
 * metatable, so the wrong accessor (`:bytes()` on a text constant, or vice-versa)
 * raises "attempt to call a nil value".  ADR-0134 / #196 collapsed the accessors
 * onto the constant — there is no longer a separate loaded handle. */
#define BLYT_RESOURCE_TEXT_CONST_MT "blyt.resource.text_const"
#define BLYT_RESOURCE_BYTES_CONST_MT "blyt.resource.bytes_const"
/* palette constant (#214): a cart .hex/.gpl/.pal asset's R.<NAME>, passed to
 * gfx.palette_set.  No :text()/:bytes() accessor — the runtime consumes it. */
#define BLYT_RESOURCE_PALETTE_CONST_MT "blyt.resource.palette_const"

/* A typed resource constant (the generated R.<NAME>): the baked console-wide
 * constant value (ADR-0134) plus its kind. */
typedef struct {
    blyt_resource_id_t id;
    int is_text;
} lua_resource_const_t;

/* Fetch a resource constant of any kind (for kind-agnostic metamethods :id/__eq). */
static lua_resource_const_t *lua_opt_const(lua_State *L, int idx) {
    void *p = luaL_testudata(L, idx, BLYT_RESOURCE_TEXT_CONST_MT);
    if (!p)
        p = luaL_testudata(L, idx, BLYT_RESOURCE_BYTES_CONST_MT);
    if (!p)
        p = luaL_testudata(L, idx, BLYT_RESOURCE_PALETTE_CONST_MT);
    return (lua_resource_const_t *)p;
}

static int lua_text_resource(lua_State *L) {
    blyt_resource_id_t id = (blyt_resource_id_t)luaL_checkinteger(L, 1);
    lua_resource_const_t *c = (lua_resource_const_t *)lua_newuserdatauv(L, sizeof(*c), 0);
    c->id = id;
    c->is_text = 1;
    luaL_setmetatable(L, BLYT_RESOURCE_TEXT_CONST_MT);
    return 1;
}

static int lua_bytes_resource(lua_State *L) {
    blyt_resource_id_t id = (blyt_resource_id_t)luaL_checkinteger(L, 1);
    lua_resource_const_t *c = (lua_resource_const_t *)lua_newuserdatauv(L, sizeof(*c), 0);
    c->id = id;
    c->is_text = 0;
    luaL_setmetatable(L, BLYT_RESOURCE_BYTES_CONST_MT);
    return 1;
}

static int lua_palette_resource(lua_State *L) {
    blyt_resource_id_t id = (blyt_resource_id_t)luaL_checkinteger(L, 1);
    lua_resource_const_t *c = (lua_resource_const_t *)lua_newuserdatauv(L, sizeof(*c), 0);
    c->id = id;
    c->is_text = 0; /* unused for a palette constant */
    luaL_setmetatable(L, BLYT_RESOURCE_PALETTE_CONST_MT);
    return 1;
}

/* Palette load (#201/#214): accepts an integer built-in handle (blyt32.PALETTE_*)
 * or a palette constant userdata (R.<NAME>); both carry the same u32 handle. */
static int lua_blyt_gfx_palette_set(lua_State *L) {
    lua_resource_const_t *c =
        (lua_resource_const_t *)luaL_testudata(L, 1, BLYT_RESOURCE_PALETTE_CONST_MT);
    blyt_palette_t handle = c ? (blyt_palette_t)c->id : (blyt_palette_t)luaL_checkinteger(L, 1);
    blyt_gfx_palette_set(handle);
    return 0;
}

static int lua_const_id(lua_State *L) {
    lua_resource_const_t *c = lua_opt_const(L, 1);
    luaL_argcheck(L, c != NULL, 1, "resource constant expected");
    lua_pushinteger(L, (lua_Integer)c->id);
    return 1;
}

static int lua_const_eq(lua_State *L) {
    lua_resource_const_t *a = lua_opt_const(L, 1);
    lua_resource_const_t *b = lua_opt_const(L, 2);
    lua_pushboolean(L, a && b && a->id == b->id && a->is_text == b->is_text);
    return 1;
}

static int lua_const_tostring(lua_State *L) {
    lua_resource_const_t *c = lua_opt_const(L, 1);
    luaL_argcheck(L, c != NULL, 1, "resource constant expected");
    lua_pushfstring(L, c->is_text ? "text_resource<%d>" : "bytes_resource<%d>", (int)c->id);
    return 1;
}

static int lua_palette_tostring(lua_State *L) {
    lua_resource_const_t *c = luaL_checkudata(L, 1, BLYT_RESOURCE_PALETTE_CONST_MT);
    lua_pushfstring(L, "palette<%d>", (int)c->id);
    return 1;
}

/* text constant :text() — owned copy with the trailing storage NUL stripped (#166)
 * so #s == content length.  The build guarantees the only NUL is the trailing
 * one, so reporting size-1 is exact; this is also the "is it really text" check.
 * pin/unpin the constant directly under the hood (ADR-0134). */
static int lua_const_text(lua_State *L) {
    lua_resource_const_t *c = luaL_checkudata(L, 1, BLYT_RESOURCE_TEXT_CONST_MT);
    const void *ptr = NULL;
    size_t size = 0;
    if (blyt_resource_pin(c->id, &ptr, &size) != BLYT_OK || !ptr) {
        lua_pushnil(L);
        return 1;
    }
    size_t content = (size >= 1 && ((const char *)ptr)[size - 1] == '\0') ? size - 1 : size;
    lua_pushlstring(L, (const char *)ptr, content);
    blyt_resource_unpin(c->id);
    return 1;
}

/* bytes constant :bytes() — owned copy of the exact bytes (verbatim).  A Lua
 * string is an 8-bit-clean byte buffer, so it round-trips opaque bytes (embedded
 * NULs, high bytes) faithfully (#162). */
static int lua_const_bytes(lua_State *L) {
    lua_resource_const_t *c = luaL_checkudata(L, 1, BLYT_RESOURCE_BYTES_CONST_MT);
    const void *ptr = NULL;
    size_t size = 0;
    if (blyt_resource_pin(c->id, &ptr, &size) != BLYT_OK || !ptr) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, (const char *)ptr, size);
    blyt_resource_unpin(c->id);
    return 1;
}

/* Module-level pin/unpin take the resource constant directly (the raw escape
 * hatch for hybrid carts, #166): pin returns a lightuserdata pointer + size,
 * valid only within the current frame. */
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

/* Build a kind-specific constant metatable carrying its accessor (`text` or
 * `bytes`) plus the shared :id()/__eq/__tostring.  Leaves nothing on the stack. */
static void register_const_mt(lua_State *L, const char *mt_name, lua_CFunction accessor,
                              const char *accessor_name) {
    luaL_newmetatable(L, mt_name);
    lua_pushcfunction(L, lua_const_eq);
    lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, lua_const_tostring);
    lua_setfield(L, -2, "__tostring");
    lua_newtable(L); /* __index method table */
    lua_pushcfunction(L, accessor);
    lua_setfield(L, -2, accessor_name);
    lua_pushcfunction(L, lua_const_id);
    lua_setfield(L, -2, "id");
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1); /* pop mt */
}

/* Build the constant metatables and the blyt.resource module table, aliasing
 * blyt32.resource == blyt.resource (ADR-0086 shared module).  Call after the
 * blyt and blyt32 globals exist. */
static void register_resource_module(lua_State *L) {
    register_const_mt(L, BLYT_RESOURCE_TEXT_CONST_MT, lua_const_text, "text");
    register_const_mt(L, BLYT_RESOURCE_BYTES_CONST_MT, lua_const_bytes, "bytes");

    /* Palette constant metatable (#214): :id()/__eq shared with the others, but
     * no bytes accessor and a palette-specific __tostring. */
    luaL_newmetatable(L, BLYT_RESOURCE_PALETTE_CONST_MT);
    lua_pushcfunction(L, lua_const_eq);
    lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, lua_palette_tostring);
    lua_setfield(L, -2, "__tostring");
    lua_newtable(L); /* __index method table */
    lua_pushcfunction(L, lua_const_id);
    lua_setfield(L, -2, "id");
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1); /* pop mt */

    lua_newtable(L); /* resource module */
    lua_pushcfunction(L, lua_text_resource);
    lua_setfield(L, -2, "text_resource");
    lua_pushcfunction(L, lua_bytes_resource);
    lua_setfield(L, -2, "bytes_resource");
    lua_pushcfunction(L, lua_palette_resource);
    lua_setfield(L, -2, "palette");
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

/* Cap on the resources_loaded list the Lua/host helpers enumerate (advisory; a
 * cart with more loaded resources than this sees the list truncated, never the
 * scalar totals or count). 256 covers any realistic working set. */
#define BLYT_LUA_MEM_RES_MAX 256

/* blyt32.mem.stats() — memory introspection (ADR-0029, #159). Returns a table:
 *   { resource_cache_used, cart_allocations, total_used, budget_cap,
 *     resources_loaded = { {id=, size=}, ... } }
 * The deterministic-vs-advisory contract is documented on blyt_mem_stats in
 * blyt.h: only budget_cap (and allocation outcomes) are safe to branch game
 * logic on; the cache figures and residency list are advisory. */
static int lua_mem_stats(lua_State *L) {
    blyt_mem_stats_t s = {0};
    blyt_mem_stats(&s); /* scalars from the accounting block (no ECALL) */
    blyt_mem_resource_t res[BLYT_LUA_MEM_RES_MAX];
    uint32_t n = blyt_mem_resources(res, BLYT_LUA_MEM_RES_MAX); /* list on demand */

    lua_createtable(L, 0, 5);
    lua_pushinteger(L, (lua_Integer)s.resource_cache_used);
    lua_setfield(L, -2, "resource_cache_used");
    lua_pushinteger(L, (lua_Integer)s.cart_allocations);
    lua_setfield(L, -2, "cart_allocations");
    lua_pushinteger(L, (lua_Integer)s.total_used);
    lua_setfield(L, -2, "total_used");
    lua_pushinteger(L, (lua_Integer)s.budget_cap);
    lua_setfield(L, -2, "budget_cap");

    uint32_t shown = n < BLYT_LUA_MEM_RES_MAX ? n : BLYT_LUA_MEM_RES_MAX;
    lua_createtable(L, (int)shown, 0); /* resources_loaded array */
    for (uint32_t i = 0; i < shown; i++) {
        lua_createtable(L, 0, 2);
        lua_pushinteger(L, (lua_Integer)res[i].id);
        lua_setfield(L, -2, "id");
        lua_pushinteger(L, (lua_Integer)res[i].size);
        lua_setfield(L, -2, "size");
        lua_rawseti(L, -2, (lua_Integer)(i + 1));
    }
    lua_setfield(L, -2, "resources_loaded");
    return 1;
}

/* Build the blyt.mem module table and alias blyt32.mem == blyt.mem (ADR-0086
 * shared module, #93). Call after the blyt and blyt32 globals exist. */
static void register_mem_module(lua_State *L) {
    lua_newtable(L); /* mem module */
    lua_pushcfunction(L, lua_mem_stats);
    lua_setfield(L, -2, "stats");

    lua_getglobal(L, "blyt");
    lua_pushvalue(L, -2); /* mem */
    lua_setfield(L, -2, "mem");
    lua_pop(L, 1); /* pop blyt */
    lua_getglobal(L, "blyt32");
    lua_pushvalue(L, -2); /* mem */
    lua_setfield(L, -2, "mem");
    lua_pop(L, 1); /* pop blyt32 */
    lua_pop(L, 1); /* pop mem module */
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

    /* --- blyt32.gfx subtable (variant-specific paletted 2D, #188) --- */
    lua_newtable(L); /* blyt32.gfx */
    static const struct {
        const char *name;
        lua_CFunction fn;
    } gfx_fns[] = {
        {"clear", lua_blyt_gfx_clear},
        {"pixel", lua_blyt_gfx_pixel},
        {"rect_fill", lua_blyt_gfx_rect_fill},
        {"line", lua_blyt_gfx_line},
        {"palette_set", lua_blyt_gfx_palette_set},
        {NULL, NULL},
    };
    for (int i = 0; gfx_fns[i].name; i++) {
        lua_pushcfunction(L, gfx_fns[i].fn);
        lua_setfield(L, -2, gfx_fns[i].name);
    }
    /* Built-in palette constants (#201), mirroring how BLYT_SCREEN is exposed
     * (a plain constant field, not the packer-generated R module -- these are
     * runtime built-ins, not cart-declared resources). */
    lua_pushinteger(L, (lua_Integer)BLYT_PALETTE_AURORA);
    lua_setfield(L, -2, "PALETTE_AURORA");
    lua_pushinteger(L, (lua_Integer)BLYT_PALETTE_VGA);
    lua_setfield(L, -2, "PALETTE_VGA");
    lua_pushinteger(L, (lua_Integer)BLYT_PALETTE_EGA);
    lua_setfield(L, -2, "PALETTE_EGA");
    lua_pushinteger(L, (lua_Integer)BLYT_PALETTE_CGA);
    lua_setfield(L, -2, "PALETTE_CGA");
    lua_pushinteger(L, (lua_Integer)BLYT_PALETTE_DEFAULT);
    lua_setfield(L, -2, "PALETTE_DEFAULT");
    lua_setfield(L, -2, "gfx"); /* blyt32.gfx = gfx; pops gfx */

    /* --- blyt32.surface subtable (tier-1 surface API, #205) --- */
    lua_newtable(L); /* blyt32.surface */
    static const struct {
        const char *name;
        lua_CFunction fn;
    } surface_fns[] = {
        {"create", lua_blyt_surface_create},       {"destroy", lua_blyt_surface_destroy},
        {"clear", lua_blyt_surface_clear},         {"pixel", lua_blyt_surface_pixel},
        {"rect_fill", lua_blyt_surface_rect_fill}, {"line", lua_blyt_surface_line},
        {"blit", lua_blyt_surface_blit},           {NULL, NULL},
    };
    for (int i = 0; surface_fns[i].name; i++) {
        lua_pushcfunction(L, surface_fns[i].fn);
        lua_setfield(L, -2, surface_fns[i].name);
    }
    lua_pushinteger(L, (lua_Integer)BLYT_SCREEN); /* blyt32.surface.SCREEN */
    lua_setfield(L, -2, "SCREEN");
    lua_setfield(L, -2, "surface"); /* blyt32.surface = surface; pops surface */

    /* --- blyt32.color subtable: named color-index constants (#203) --- */
    /* The EGA-16 naming vocabulary; each bundled palette its own index set.
     * color.ega / .vga / .aurora carry the per-palette indices (names shared,
     * values differ; vga's low 16 == ega); the color root mirrors aurora as the
     * zero-config default (blyt.h's BLYT_<NAME> aliases).  Values come straight
     * from blyt.h -- this is guest code, so no duplication. */
    static const char *const color_names[16] = {
        "BLACK",  "BLUE",    "GREEN",    "CYAN",    "RED",    "MAGENTA",    "BROWN",     "LTGRAY",
        "DKGRAY", "BR_BLUE", "BR_GREEN", "BR_CYAN", "BR_RED", "BR_MAGENTA", "BR_YELLOW", "WHITE",
    };
    static const uint8_t ega_idx[16] = {
        BLYT_EGA_BLACK,  BLYT_EGA_BLUE,       BLYT_EGA_GREEN,     BLYT_EGA_CYAN,
        BLYT_EGA_RED,    BLYT_EGA_MAGENTA,    BLYT_EGA_BROWN,     BLYT_EGA_LTGRAY,
        BLYT_EGA_DKGRAY, BLYT_EGA_BR_BLUE,    BLYT_EGA_BR_GREEN,  BLYT_EGA_BR_CYAN,
        BLYT_EGA_BR_RED, BLYT_EGA_BR_MAGENTA, BLYT_EGA_BR_YELLOW, BLYT_EGA_WHITE,
    };
    static const uint8_t aurora_idx[16] = {
        BLYT_AURORA_BLACK,  BLYT_AURORA_BLUE,       BLYT_AURORA_GREEN,     BLYT_AURORA_CYAN,
        BLYT_AURORA_RED,    BLYT_AURORA_MAGENTA,    BLYT_AURORA_BROWN,     BLYT_AURORA_LTGRAY,
        BLYT_AURORA_DKGRAY, BLYT_AURORA_BR_BLUE,    BLYT_AURORA_BR_GREEN,  BLYT_AURORA_BR_CYAN,
        BLYT_AURORA_BR_RED, BLYT_AURORA_BR_MAGENTA, BLYT_AURORA_BR_YELLOW, BLYT_AURORA_WHITE,
    };
    lua_newtable(L); /* blyt32.color */
    /* color.ega and color.vga (identical index set) */
    for (int pass = 0; pass < 2; pass++) {
        lua_newtable(L);
        for (int i = 0; i < 16; i++) {
            lua_pushinteger(L, (lua_Integer)ega_idx[i]);
            lua_setfield(L, -2, color_names[i]);
        }
        lua_setfield(L, -2, pass == 0 ? "ega" : "vga");
    }
    /* color.aurora */
    lua_newtable(L);
    for (int i = 0; i < 16; i++) {
        lua_pushinteger(L, (lua_Integer)aurora_idx[i]);
        lua_setfield(L, -2, color_names[i]);
    }
    lua_setfield(L, -2, "aurora");
    /* Default aliases on the color root -> aurora (the console default). */
    for (int i = 0; i < 16; i++) {
        lua_pushinteger(L, (lua_Integer)aurora_idx[i]);
        lua_setfield(L, -2, color_names[i]);
    }
    lua_setfield(L, -2, "color"); /* blyt32.color = color; pops color */

    lua_setglobal(L, "blyt32"); /* pops blyt32 */

    lua_pop(L, 1); /* pop blyt.buf (idx B) */

    lua_pop(L, 1); /* pop blyt.debug (idx A) */

    register_resource_module(L); /* blyt.resource.* + blyt32.resource.* (#93) */
    register_mem_module(L); /* blyt.mem.* + blyt32.mem.* (ADR-0029, #159) */

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
