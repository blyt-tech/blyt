/*
 * blyt_hostlua_api.h — the ONE host-Lua API registration for every leg (#267,
 * epic #230, ADR-0029/0008/0136).
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * #242 unified how the legs *drive* a cart (blyt_hostlua_driver.h). It did not
 * unify how they *set one up*: `cart_run_hostlua.c` (native/libretro) and
 * `frontends/wasm/wasm_main.c` each hand-registered the same blyt/blyt32 surface
 * in their own order, with their own decomposition (native split gfx and surface
 * into two functions; wasm folded surface, color and the lock metatable into
 * one). Same API set, two independently-authored sequences.
 *
 * That is not a tidiness problem. Registering these names interns short strings,
 * so it allocates, and it lands in the scaffolding that `guest_heap_used`
 * subtracts as its baseline. Subtracting the baseline removes the scaffolding's
 * BYTES but not its LAYOUT: the cart heap is a first-fit arena that charges a
 * recycled block whole when the remainder is too small to split (blyt_arena.c),
 * so a different registration order leaves a different free list behind, and the
 * cart's own later allocations get charged against it differently. Measured, that
 * refracted into a constant ±16 B cross-leg divergence in `cart_allocations`
 * which survived #242 and broke ADR-0029's promise that the deterministic tier is
 * byte-identical on every leg.
 *
 * So the ORDER is part of the contract, and a contract that lives in two files
 * is not a contract. This header owns the whole sequence — which registrations
 * run, in which order, under which names, with which constants — from one
 * definition. A leg supplies only what is irreducibly its own: the `lua_CFunction`
 * bodies behind each name (native calls libblyt directly, wasm calls its session)
 * and the two registrations whose backends genuinely differ (state API and the S
 * proxy), which run through hooks at their canonical position in the sequence.
 *
 * The pattern deliberately mirrors blyt_hostlua_driver.h: a leg cannot drift out
 * of step by editing its own copy, because it does not have one.
 *
 * Header-only (static inline), like the driver: nothing to add to
 * frontends/wasm/CMakeLists.txt, so the WASM leg cannot silently link-fail and
 * ship a stale .wasm the way a new runtime/shared/*.c would.
 */

#ifndef BLYT_SHARED_HOSTLUA_API_H
#define BLYT_SHARED_HOSTLUA_API_H

#include "lauxlib.h"
#include "lua.h"

#include "blyt_handle.h" /* BLYT_RESOURCE_ENCODE / BLYT_SCREEN (ADR-0134) */
#include "blyt_hostlua_driver.h" /* the driver's own globals + their order */
#include "blyt_palettes.h" /* BLYT_PAL_ID_* (#201) */

/* Metatable registry names. Shared so the two legs cannot disagree on a string
 * that Lua uses as an identity key — they previously carried these as separate
 * per-leg #defines that happened to hold equal text. */
#define BLYT_HOSTLUA_LOCK_MT "blyt.surface.lock"
#define BLYT_HOSTLUA_RESOURCE_TEXT_CONST_MT "blyt.resource.text_const"
#define BLYT_HOSTLUA_RESOURCE_BYTES_CONST_MT "blyt.resource.bytes_const"
#define BLYT_HOSTLUA_RESOURCE_PALETTE_CONST_MT "blyt.resource.palette_const"

/*
 * The leg's half of the API: the C bodies behind each registered name, plus
 * hooks for the two registrations that resist sharing. Every field is required
 * unless its comment says otherwise — a NULL where one is expected would register
 * a nil and fail at cart runtime rather than here.
 */
typedef struct {
    /* blyt / blyt32 core */
    lua_CFunction debug_print;
    lua_CFunction quit;
    lua_CFunction should_quit;
    lua_CFunction require_fn;

    /* Driver globals (blyt_hostlua_driver.h owns their names + order). */
    lua_CFunction blyt_call;
    lua_CFunction phase_update;
    lua_CFunction phase_draw;
    lua_CFunction phase_none;

    /* blyt32.gfx.* (#188) */
    lua_CFunction gfx_clear;
    lua_CFunction gfx_pixel;
    lua_CFunction gfx_rect_fill;
    lua_CFunction gfx_line;
    lua_CFunction gfx_palette_set;

    /* blyt32.surface.* (#205) */
    lua_CFunction surface_create;
    lua_CFunction surface_destroy;
    lua_CFunction surface_clear;
    lua_CFunction surface_pixel;
    lua_CFunction surface_rect_fill;
    lua_CFunction surface_line;
    lua_CFunction surface_blit;
    lua_CFunction surface_acquire;

    /* blyt.surface.lock methods (tier-2, #208) */
    lua_CFunction lock_get;
    lua_CFunction lock_set;
    lua_CFunction lock_clear;
    lua_CFunction lock_rect_fill;
    lua_CFunction lock_line;
    lua_CFunction lock_release;

    /* blyt.resource.* + typed constant metatables (#166/#214) */
    lua_CFunction res_const_text;
    lua_CFunction res_const_bytes;
    lua_CFunction res_const_eq;
    lua_CFunction res_const_id;
    lua_CFunction res_const_tostring;
    lua_CFunction res_palette_tostring;
    lua_CFunction res_text_resource;
    lua_CFunction res_bytes_resource;
    lua_CFunction res_palette_resource;
    lua_CFunction res_pin;
    lua_CFunction res_unpin;

    /* blyt32.mem.stats (ADR-0029, #159) */
    lua_CFunction mem_stats;

    /* Optional: run immediately before the gfx table is built. The WASM leg
     * materialises its default palette here; native has nothing to do. NULL to
     * skip. Allocation-free on every leg, so it cannot perturb the order. */
    void (*pre_gfx)(void *ctx);

    /* The two registrations whose backends genuinely differ (native drives a
     * blyt_state_ctx_t, wasm a blyt_session_t). Called at their canonical point
     * in the sequence below. Both NULL for a cart with no state context. */
    void (*register_state_api)(lua_State *L, void *ctx);
    void (*register_s_proxy)(lua_State *L, void *ctx);

    /* Opaque leg context handed back to the hooks above. */
    void *ctx;
} blyt_hostlua_api_t;

/* Named color-index constants (#203). Mirrors blyt32lua.c's pure-Lua binding;
 * these raw indices MUST match blyt.h's BLYT_EGA_* / BLYT_AURORA_* (host-side
 * code cannot include the cart-facing header). The cross-leg frame-hash parity
 * test guards against drift. */
static const char *const blyt_hostlua_color_names[16] = {
    "BLACK",  "BLUE",    "GREEN",    "CYAN",    "RED",    "MAGENTA",    "BROWN",     "LTGRAY",
    "DKGRAY", "BR_BLUE", "BR_GREEN", "BR_CYAN", "BR_RED", "BR_MAGENTA", "BR_YELLOW", "WHITE",
};
static const unsigned char blyt_hostlua_ega_idx[16] = {0, 1, 2,  3,  4,  5,  6,  7,
                                                       8, 9, 10, 11, 12, 13, 14, 15};
static const unsigned char blyt_hostlua_aurora_idx[16] = {0, 223, 185, 195, 155, 239, 165, 10,
                                                          5, 219, 189, 201, 160, 236, 175, 15};

/* Set field `name` on the table at -1 to C function `fn`. */
static inline void blyt_hl_setfn(lua_State *L, const char *name, lua_CFunction fn) {
    lua_pushcfunction(L, fn);
    lua_setfield(L, -2, name);
}

/* Set field `name` on the table at -1 to integer `v`. */
static inline void blyt_hl_setint(lua_State *L, const char *name, lua_Integer v) {
    lua_pushinteger(L, v);
    lua_setfield(L, -2, name);
}

/* Bind the module table at -1 onto BOTH `blyt.<name>` and `blyt32.<name>`, then
 * pop it — the shape every module registration below shares. */
static inline void blyt_hl_bind_module(lua_State *L, const char *name) {
    lua_getglobal(L, "blyt");
    if (lua_istable(L, -1)) {
        lua_pushvalue(L, -2);
        lua_setfield(L, -2, name);
    }
    lua_pop(L, 1);
    lua_getglobal(L, "blyt32");
    if (lua_istable(L, -1)) {
        lua_pushvalue(L, -2);
        lua_setfield(L, -2, name);
    }
    lua_pop(L, 1);
    lua_pop(L, 1); /* pop module */
}

/* A typed resource constant's metatable (#166/#214): :id()/:<accessor>() + __eq
 * + __tostring. Field order is deliberate and matches what both legs did before
 * this header existed — it is interning order, so it is part of the contract. */
static inline void blyt_hl_const_mt(lua_State *L, const blyt_hostlua_api_t *a, const char *mt_name,
                                    lua_CFunction accessor, const char *accessor_name) {
    luaL_newmetatable(L, mt_name);
    blyt_hl_setfn(L, "__eq", a->res_const_eq);
    blyt_hl_setfn(L, "__tostring", a->res_const_tostring);
    lua_newtable(L); /* __index */
    blyt_hl_setfn(L, accessor_name, accessor);
    blyt_hl_setfn(L, "id", a->res_const_id);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1); /* pop mt */
}

/* ── The canonical sequence ──────────────────────────────────────────────────
 *
 * Step order is the contract (see the header comment). Each step leaves the Lua
 * stack as it found it.
 */

/* 1. blyt32 / blyt roots + the driver's globals. Everything downstream binds
 *    onto these, so they must exist first. */
static inline void blyt_hostlua_register_core(lua_State *L, const blyt_hostlua_api_t *a) {
    /* blyt32 = { debug = { print } } */
    lua_newtable(L);
    lua_newtable(L);
    blyt_hl_setfn(L, "print", a->debug_print);
    lua_setfield(L, -2, "debug");
    lua_setglobal(L, "blyt32");

    /* blyt = { debug = { print }, quit, should_quit } */
    lua_newtable(L);
    lua_newtable(L);
    blyt_hl_setfn(L, "print", a->debug_print);
    lua_setfield(L, -2, "debug");
    blyt_hl_setfn(L, "quit", a->quit);
    blyt_hl_setfn(L, "should_quit", a->should_quit);
    lua_setglobal(L, "blyt");

    lua_pushcfunction(L, a->quit);
    lua_setglobal(L, "blyt_quit");
    lua_pushcfunction(L, a->require_fn);
    lua_setglobal(L, "require");

    blyt_hostlua_driver_register_globals(L, a->blyt_call, a->phase_update, a->phase_draw,
                                         a->phase_none);
}

/* 2. blyt32.gfx.* + the built-in palette constants (#201). */
static inline void blyt_hostlua_register_gfx(lua_State *L, const blyt_hostlua_api_t *a) {
    if (a->pre_gfx)
        a->pre_gfx(a->ctx);
    lua_getglobal(L, "blyt32");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_newtable(L); /* blyt32.gfx */
    blyt_hl_setfn(L, "clear", a->gfx_clear);
    blyt_hl_setfn(L, "pixel", a->gfx_pixel);
    blyt_hl_setfn(L, "rect_fill", a->gfx_rect_fill);
    blyt_hl_setfn(L, "line", a->gfx_line);
    blyt_hl_setfn(L, "palette_set", a->gfx_palette_set);
    blyt_hl_setint(
        L, "PALETTE_AURORA",
        (lua_Integer)BLYT_RESOURCE_ENCODE(BLYT_PAL_ID_AURORA, BLYT_RESOURCE_PROV_RUNTIME));
    blyt_hl_setint(L, "PALETTE_VGA",
                   (lua_Integer)BLYT_RESOURCE_ENCODE(BLYT_PAL_ID_VGA, BLYT_RESOURCE_PROV_RUNTIME));
    blyt_hl_setint(L, "PALETTE_EGA",
                   (lua_Integer)BLYT_RESOURCE_ENCODE(BLYT_PAL_ID_EGA, BLYT_RESOURCE_PROV_RUNTIME));
    blyt_hl_setint(L, "PALETTE_CGA",
                   (lua_Integer)BLYT_RESOURCE_ENCODE(BLYT_PAL_ID_CGA, BLYT_RESOURCE_PROV_RUNTIME));
    blyt_hl_setint(
        L, "PALETTE_DEFAULT",
        (lua_Integer)BLYT_RESOURCE_ENCODE(BLYT_PAL_ID_AURORA, BLYT_RESOURCE_PROV_RUNTIME));
    lua_setfield(L, -2, "gfx");
    lua_pop(L, 1); /* pop blyt32 */
}

/* 3. blyt32.surface.* + the tier-2 lock metatable (#205/#208). */
static inline void blyt_hostlua_register_surface(lua_State *L, const blyt_hostlua_api_t *a) {
    lua_getglobal(L, "blyt32");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_newtable(L); /* blyt32.surface */
    blyt_hl_setfn(L, "create", a->surface_create);
    blyt_hl_setfn(L, "destroy", a->surface_destroy);
    blyt_hl_setfn(L, "clear", a->surface_clear);
    blyt_hl_setfn(L, "pixel", a->surface_pixel);
    blyt_hl_setfn(L, "rect_fill", a->surface_rect_fill);
    blyt_hl_setfn(L, "line", a->surface_line);
    blyt_hl_setfn(L, "blit", a->surface_blit);
    blyt_hl_setfn(L, "acquire", a->surface_acquire);
    blyt_hl_setint(L, "SCREEN", (lua_Integer)BLYT_SCREEN);
    lua_setfield(L, -2, "surface");
    lua_pop(L, 1); /* pop blyt32 */

    luaL_newmetatable(L, BLYT_HOSTLUA_LOCK_MT);
    lua_newtable(L); /* __index */
    blyt_hl_setfn(L, "get", a->lock_get);
    blyt_hl_setfn(L, "set", a->lock_set);
    blyt_hl_setfn(L, "clear", a->lock_clear);
    blyt_hl_setfn(L, "rect_fill", a->lock_rect_fill);
    blyt_hl_setfn(L, "line", a->lock_line);
    blyt_hl_setfn(L, "release", a->lock_release);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1); /* pop mt */
}

/* 4. blyt32.color.* — named color-index constants (#203). */
static inline void blyt_hostlua_register_color(lua_State *L) {
    lua_getglobal(L, "blyt32");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_newtable(L); /* blyt32.color */
    for (int pass = 0; pass < 2; pass++) { /* color.ega, color.vga (same set) */
        lua_newtable(L);
        for (int i = 0; i < 16; i++)
            blyt_hl_setint(L, blyt_hostlua_color_names[i], (lua_Integer)blyt_hostlua_ega_idx[i]);
        lua_setfield(L, -2, pass == 0 ? "ega" : "vga");
    }
    lua_newtable(L); /* color.aurora */
    for (int i = 0; i < 16; i++)
        blyt_hl_setint(L, blyt_hostlua_color_names[i], (lua_Integer)blyt_hostlua_aurora_idx[i]);
    lua_setfield(L, -2, "aurora");
    for (int i = 0; i < 16; i++) /* default aliases on the color root -> aurora */
        blyt_hl_setint(L, blyt_hostlua_color_names[i], (lua_Integer)blyt_hostlua_aurora_idx[i]);
    lua_setfield(L, -2, "color");
    lua_pop(L, 1); /* pop blyt32 */
}

/* 5. blyt.resource.* / blyt32.resource.* + typed constant metatables. */
static inline void blyt_hostlua_register_resource(lua_State *L, const blyt_hostlua_api_t *a) {
    blyt_hl_const_mt(L, a, BLYT_HOSTLUA_RESOURCE_TEXT_CONST_MT, a->res_const_text, "text");
    blyt_hl_const_mt(L, a, BLYT_HOSTLUA_RESOURCE_BYTES_CONST_MT, a->res_const_bytes, "bytes");

    /* Palette constants (#214): :id()/__eq shared, palette-specific __tostring,
     * no bytes accessor — so this one does not go through blyt_hl_const_mt. */
    luaL_newmetatable(L, BLYT_HOSTLUA_RESOURCE_PALETTE_CONST_MT);
    blyt_hl_setfn(L, "__eq", a->res_const_eq);
    blyt_hl_setfn(L, "__tostring", a->res_palette_tostring);
    lua_newtable(L); /* __index */
    blyt_hl_setfn(L, "id", a->res_const_id);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1); /* pop mt */

    lua_newtable(L); /* resource module */
    blyt_hl_setfn(L, "text_resource", a->res_text_resource);
    blyt_hl_setfn(L, "bytes_resource", a->res_bytes_resource);
    blyt_hl_setfn(L, "palette", a->res_palette_resource);
    blyt_hl_setfn(L, "pin", a->res_pin);
    blyt_hl_setfn(L, "unpin", a->res_unpin);
    blyt_hl_bind_module(L, "resource");
}

/* 6. blyt.mem.stats / blyt32.mem.stats. */
static inline void blyt_hostlua_register_mem(lua_State *L, const blyt_hostlua_api_t *a) {
    lua_newtable(L); /* mem module */
    blyt_hl_setfn(L, "stats", a->mem_stats);
    blyt_hl_bind_module(L, "mem");
}

/*
 * Register the whole host-Lua API onto `L`, in the canonical order. THIS
 * SEQUENCE IS THE CROSS-LEG CONTRACT — see the header comment before reordering
 * anything: the order is observable through cart_allocations, and the gate that
 * catches a change to it is
 * tests/integration/tests/hostlua_heap_parity.rs.
 */
static inline void blyt_hostlua_register_api(lua_State *L, const blyt_hostlua_api_t *a) {
    blyt_hostlua_register_core(L, a);
    blyt_hostlua_register_gfx(L, a);
    blyt_hostlua_register_surface(L, a);
    blyt_hostlua_register_color(L);
    blyt_hostlua_register_resource(L, a);
    blyt_hostlua_register_mem(L, a);
    if (a->register_state_api)
        a->register_state_api(L, a->ctx);
    if (a->register_s_proxy)
        a->register_s_proxy(L, a->ctx);
}

#endif /* BLYT_SHARED_HOSTLUA_API_H */
