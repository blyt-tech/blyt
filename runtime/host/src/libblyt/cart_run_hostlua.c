/*
 * cart_run_hostlua.c — native host-Lua fast path runner (#238, epic #230).
 *
 * Sibling to cart_run.c's blyt_session_*: runs a pure-Lua cart's bytecode in a
 * Lua VM compiled natively for the host (the deterministic seam VM,
 * cmake/blyt_hostlua_vm.cmake) instead of the RV32 Lua VM under rv32emu.  The
 * native port of frontends/wasm/wasm_main.c's run_lua_cart — same Lua fork
 * (BLYT_LUA_I32_F64), same cart bytecode, same fixed hash seed, same blyt_fpm
 * transcendental seam, same restricted stdlib subset — so its cart-visible
 * output is byte-identical to every other leg (determinism is the core
 * contract, ADR-0007).
 *
 * The whole execution body compiles only when the seam VM is available
 * (BLYT_HOSTLUA_EXEC, set on libblyt by CMake); otherwise the entry points below
 * degrade to no-ops so the frontend falls back to the rv32 session transparently.
 *
 * S2 scope (#238): VM create + restricted stdlib + the minimal blyt/blyt32 API a
 * pure-Lua cart reaches for output and termination (debug.print, quit,
 * should_quit) + sandboxed require + BLMC/raw bytecode loader + direct-call
 * init/update/draw/on_quit/cleanup lifecycle.  State buffers (S-proxy) land in S3,
 * save/restore + reset-every-frame in S4.  Unimplemented cart APIs error LOUDLY
 * rather than silently no-op (anti-#98).
 */

#include <stdbool.h>

#include "blyt_hostlua.h"

#ifdef BLYT_HOSTLUA_EXEC

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#ifdef BLYT_DAP
#include "dap_transport_tcp_lua.h" /* native host-Lua DAP server (#234) */
#include "master_hook.h" /* fc_consolelua_master_hook_install + fc_master_hook_cfg */
#endif

#include "blyt_arena.h" /* runtime/shared: single-sourced cart-heap arena (#158) */
#include "blyt_frame_hash.h" /* runtime/shared: cross-leg framebuffer hash (#188) */
#include "blyt_handle.h" /* runtime/shared: console-wide handle encoding (ADR-0134) */
#include "blyt_hostlua_heap.h" /* runtime/shared: rv32 heap-seam handoff (#231) */
#include "blyt_mem_budget.h" /* runtime/shared: unified 16 MB budget (ADR-0008 #158) */
#include "blyt_palettes.h" /* runtime/shared: built-in palette resolver (#201) */
#include "blyt_phase.h" /* runtime/shared: lifecycle phase (draw()-only gate, #205) */
#include "blyt_raster.h" /* runtime/shared: integer rasterizer core (#188) */
#include "blyt_resource_codec.h" /* runtime/shared: BLYT_RES_ALGO_NONE (#157) */
#include "resource.h" /* blyt_resource_table_t + lifecycle (#93/#158) */
#include "save.h" /* blyt_save_write / blyt_save_read */
#include "state_buffer.h" /* blyt_state_ctx_t + typed accessors */
#include "testcard.h" /* PM5544 testcard (drawn until the cart draws, #204) */

/* Host-Lua surface pool (blyt32.surface.*, #205/#208, #231).  A pure-Lua cart on
 * this path has no session, so its surfaces live in the runner (the session-less
 * mirror of the WASM leg's g_lua_surf pool — none of that leg's hybrid/session
 * routing applies here).  Slot 0 is the screen: its pixels resolve to the
 * runner's fb on each access.  Off-screen slots hold malloc'd buffers, freed at
 * the frame boundary (draw-scoped, #205). */
#define HL_SURFACE_MAX 64
typedef struct {
    uint8_t *pixels;
    int32_t w, h;
    uint16_t gen;
    bool in_use;
    bool is_screen;
    bool locked; /* exclusive tier-2 lock held (#207/#208) */
} hl_surface_t;

struct blyt_hostlua {
    lua_State *L;
    blyt_cart_t *cart; /* for save_version + cart id (save subdir) */
    blyt_log_fn log_fn;
    /* .cart.lua bytecode (into the cart mmap) — kept so the reset-every-frame
     * cycle can rebuild the VM from the same chunk. */
    const unsigned char *bytecode;
    size_t bytecode_size;
    /* State buffers (ADR-0009/0010): a standalone ctx — the host-Lua path has no
     * blyt_session, so it owns the ctx directly instead of borrowing the
     * session's (the WASM pure-Lua path's g_lua_state_ctx equivalent).  NULL when
     * the cart declares no .cart.layouts. */
    blyt_state_ctx_t *state_ctx;
    char *save_dir; /* $BLYT_SAVE_DIR (strdup'd), or NULL */
    char cart_name[64]; /* manifest id — the save subdirectory name */
    int quit; /* blyt.quit() latch (mirrors g_quit_requested in blyt_main) */
    bool done; /* on_quit() + cleanup() already run */
    /* Gfx fast path (#188 / Spike X, #231): a runner-owned paletted framebuffer
     * the blyt32.gfx.* bindings rasterize into (the host-Lua equivalent of the
     * emulated path's session->pixels[]) plus the active palette.  The host-Lua
     * path has no session, so — like the state ctx — the runner owns these
     * directly (the WASM leg's standalone g_lua_pixels / g_lua_gfx_palette). */
    uint8_t fb[BLYT_FRAME_W * BLYT_FRAME_H]; /* paletted back buffer (1 byte/px) */
    uint32_t palette[256]; /* active 256-entry XRGB8888 palette */
    bool drawn; /* a blyt32.gfx.* op ran ⇒ displace the testcard (sticky) */
    uint32_t frame_count; /* testcard animation counter (== session->frame_count) */
    /* Surface pool (#205/#208, #231) + the per-frame tier-2 lock epoch (bumped
     * each frame so a lock never outlives the draw it was taken in). */
    hl_surface_t surf[HL_SURFACE_MAX];
    bool surf_init;
    uint32_t lock_epoch;
    /* Cart heap (#158, #231): on this 64-bit host the Lua VM's objects carry
     * 8-byte pointers, so physical bytes come from plain host malloc (unbounded —
     * a 16 MB-budget cart may use ~2× host RAM, fine on desktop).  `arena` is a
     * separate rv32-sized SHADOW allocator: the fork (BLYT_HOSTLUA_HEAP_SEAM)
     * publishes each allocation's rv32-equivalent size (blyt_hostlua_heap_rv_pending),
     * and hl_lua_alloc drives this arena at those sizes so guest_heap_used and the
     * 16 MB fail-point are byte-identical to the wasm32 leg (DIRECTION 1).  Each
     * physical block carries a small prefix holding its shadow-arena offset. */
    blyt_mem_accounting_t mem_acct;
    blyt_arena_t arena;
    /* Resource table (#93/#158/#231): a pure-Lua cart has no session, so its
     * blyt.resource.* table lives in the runner (the session-less mirror of the
     * WASM leg's g_lua_resources).  non_evictable_footprint in mem_acct above is
     * published from it, feeding the same unified 16 MB budget predicate. */
    blyt_resource_table_t resources;
    bool resources_loaded;
    /* Hybrid carts (#232, ADR-0130): a Lua+native cart runs its Lua half on this
     * host VM while the native C/Rust half stays EMULATED in an rv32 session.
     * `session` is non-NULL only for a hybrid (a .lua_exports cart); the exchange
     * thread `lua_exch` (a Lua thread off hl->L, anchored via lua_exch_ref) is
     * where the ADR-0130 bridge executes the native half's Lua C API calls.  Both
     * are recreated on every VM rebuild (reset/reload) inside build_vm; the
     * session itself persists across rebuilds.  The native counterpart of the
     * WASM leg's g_session / g_lua_exch. */
    blyt_session_t *session;
    lua_State *lua_exch;
    int lua_exch_ref; /* LUA_NOREF when no exchange thread is anchored */
    /* Source-level debugging (#234): when set, build_vm arms the DAP master hook
     * on hl->L (re-armed on every reset/reload rebuild), init() is deferred to
     * blyt_hostlua_dap_wait_ready(), and destroy shuts the DAP server down. */
    bool dap_enabled;
    bool booted; /* init()/on_new_state() have run (the deferred debug boot) */
};

/* The runner is stashed in the lua_State's extra space so the C API callbacks
 * can reach its log channel + quit latch without a file-scoped global (unlike
 * the WASM leg's g_lua — a native player could in principle host more than one). */
static blyt_hostlua_t *hl_from(lua_State *L) {
    return *(blyt_hostlua_t **)lua_getextraspace(L);
}

/* Per-physical-block prefix holding the block's rv32 shadow-arena offset. 16
 * bytes keeps the object pointer (raw + prefix) 16-byte aligned — the max
 * natural alignment of any Lua value (the f64 in the Value union), matching the
 * arena's own guarantee. */
#define HL_HEAP_PREFIX 16u

/* lua_Alloc for the native host-Lua fast path (#231).  Physical bytes come from
 * plain host malloc (unbounded); a separate rv32-sized SHADOW arena (hl->arena)
 * produces the canonical guest_heap_used and the 16 MB fail-point.  The fork
 * (BLYT_HOSTLUA_HEAP_SEAM) publishes each allocation's rv32-equivalent size in
 * blyt_hostlua_heap_rv_pending just before calling here; we size the shadow block
 * from that so the count is byte-identical to the wasm32 host-Lua leg (which runs
 * the identical runner but at 32-bit object sizes).  Each physical block carries
 * a prefix storing its shadow-arena offset so free/realloc can locate the twin.
 *
 * Shadow region is 16 MB — the exact rv32 budget — so blyt_arena_malloc returns
 * NULL (→ Lua ENOMEM) at precisely the point rv32/wasm32 hit the cap; the
 * physical host allocation is never the limiting factor.  The unified budget's
 * non-evictable resource footprint feeds the same predicate via mem_acct. */
static void *hl_lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)osize;
    blyt_hostlua_t *hl = (blyt_hostlua_t *)ud;

    if (nsize == 0) { /* free */
        if (ptr) {
            char *raw = (char *)ptr - HL_HEAP_PREFIX;
            uint32_t soff = *(uint32_t *)(void *)raw;
            blyt_arena_free(&hl->arena, (char *)hl->arena.base + soff);
            free(raw);
        }
        return NULL;
    }

    if (!hl->arena.base) { /* lazily create the rv32 shadow region */
        void *region = malloc(BLYT_MEM_BUDGET_BYTES);
        if (!region)
            return NULL;
        hl->arena.base = region;
        hl->arena.size = BLYT_MEM_BUDGET_BYTES;
        hl->arena.acct = &hl->mem_acct;
    }

    /* rv32-equivalent size the fork just published for this request, consumed
     * atomically (single-threaded VM).  A request the fork routed through its
     * luaM typed layer carries a real published size; one that reached the raw
     * lua_Alloc callback directly (the auxlib buffer's heap box / external-string
     * body — lauxlib.c resizebox) leaves the pending size UNSET, and a raw byte
     * buffer holds no pointers, so its rv32 size is just the host `nsize`.  Reset
     * so the NEXT allocation is only credited a published size if the fork
     * actually published one for it (else a stale value would mis-size a raw
     * buffer — the cause of multi-KB external strings under-counting to nothing). */
    size_t rv = blyt_hostlua_heap_rv_pending;
    blyt_hostlua_heap_rv_pending = BLYT_HOSTLUA_HEAP_RV_UNSET;
    if (rv == BLYT_HOSTLUA_HEAP_RV_UNSET)
        rv = nsize;

    /* VM execution scratch (a thread's data stack / CallInfo, #231): allocate the
     * shadow block no-acct so it is excluded from guest_heap_used and the 16 MB
     * budget — the cart-attributable heap must not depend on how the runtime
     * drives the cart (this leg calls lifecycle fns from C; the wasm leg resumes
     * them in a driver coroutine).  Consumed atomically, identical logic to the
     * wasm runner so any residual is cross-leg-identical.  free() reads the
     * marker from the shadow block itself, so the free path is unchanged. */
    int noacct = blyt_hostlua_heap_stack_pending;
    blyt_hostlua_heap_stack_pending = 0;

    if (!ptr) { /* malloc */
        void *sp =
            noacct ? blyt_arena_malloc_noacct(&hl->arena, rv) : blyt_arena_malloc(&hl->arena, rv);
        if (!sp)
            return NULL; /* rv32 budget exceeded → Lua sees ENOMEM */
        char *raw = (char *)malloc(nsize + HL_HEAP_PREFIX);
        if (!raw) {
            blyt_arena_free(&hl->arena, sp);
            return NULL;
        }
        *(uint32_t *)(void *)raw = (uint32_t)((char *)sp - (char *)hl->arena.base);
        return raw + HL_HEAP_PREFIX;
    }

    /* realloc */
    char *raw = (char *)ptr - HL_HEAP_PREFIX;
    uint32_t old_soff = *(uint32_t *)(void *)raw;
    void *base_sp = (char *)hl->arena.base + old_soff;
    void *new_sp = noacct ? blyt_arena_realloc_noacct(&hl->arena, base_sp, rv)
                          : blyt_arena_realloc(&hl->arena, base_sp, rv);
    if (!new_sp)
        return NULL; /* rv32 budget exceeded */
    char *new_raw = (char *)realloc(raw, nsize + HL_HEAP_PREFIX);
    if (!new_raw)
        return NULL; /* true host OOM (shadow already resized; process is dying) */
    *(uint32_t *)(void *)new_raw = (uint32_t)((char *)new_sp - (char *)hl->arena.base);
    return new_raw + HL_HEAP_PREFIX;
}

/* blyt.debug.print(s) / blyt32.debug.print(s): the cart's cross-leg output
 * channel.  Routed through the runner's log_fn — the SAME callback the emulated
 * path drives from blyt_console_debug — so a line printed here is byte-identical
 * to the emulated leg (the frontend's log sink appends the newline). */
static int l_debug_print(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    blyt_hostlua_t *hl = hl_from(L);
    if (hl && hl->log_fn)
        hl->log_fn(s);
    return 0;
}

static int l_quit(lua_State *L) {
    blyt_hostlua_t *hl = hl_from(L);
    if (hl)
        hl->quit = 1;
    return 0;
}

static int l_should_quit(lua_State *L) {
    blyt_hostlua_t *hl = hl_from(L);
    lua_pushboolean(L, hl ? hl->quit : 0);
    return 1;
}

/* Sandboxed require(): the host-Lua fast path replaces the default searcher with
 * a hard error so a cart cannot reach the host filesystem — only modules already
 * registered in package.loaded (native exports) resolve.  Mirrors the WASM leg. */
static int l_require(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
    lua_getfield(L, -1, name);
    if (!lua_isnil(L, -1))
        return 1;
    return luaL_error(L, "module '%s' not found (blyt sandbox: only native exports available)",
                      name);
}

/* Derive a require()-able module name from a loaded chunk's embedded source
 * (basename minus ".lua"); the chunk function must be on the stack top and is
 * left untouched.  Mirrors chunk_module_name in libblyt32lua / the WASM leg. */
static void hl_chunk_module_name(lua_State *L, char *out, size_t outsz) {
    out[0] = '\0';
    lua_Debug ar;
    lua_pushvalue(L, -1);
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

/* Load the cart's .cart.lua: a single raw bytecode chunk, or the BLMC
 * multi-chunk container (issue #54).  A chunk returning a non-nil table is
 * registered as a require()-able module (cart_resources, ADR-0040), keyed by
 * source basename — byte-for-byte the WASM leg's loader.  Returns 0 on success;
 * on failure leaves an error string on the stack top. */
static int load_lua_bytecode(lua_State *L, const unsigned char *data, size_t size) {
    if (size >= 8 && data[0] == 'B' && data[1] == 'L' && data[2] == 'M' && data[3] == 'C') {
        unsigned int nchunks = (unsigned int)data[4] | ((unsigned int)data[5] << 8) |
                               ((unsigned int)data[6] << 16) | ((unsigned int)data[7] << 24);
        data += 8;
        size -= 8;
        for (unsigned int ci = 0; ci < nchunks; ci++) {
            if (size < 4) {
                lua_pushstring(L, "BLMC truncated");
                return 1;
            }
            unsigned int csz = (unsigned int)data[0] | ((unsigned int)data[1] << 8) |
                               ((unsigned int)data[2] << 16) | ((unsigned int)data[3] << 24);
            data += 4;
            size -= 4;
            if (csz > size) {
                lua_pushstring(L, "BLMC chunk size overflow");
                return 1;
            }
            if (luaL_loadbuffer(L, (const char *)data, csz, "@chunk") != LUA_OK)
                return 1;
            char modname[64];
            hl_chunk_module_name(L, modname, sizeof(modname));
            if (lua_pcall(L, 0, 1, 0) != LUA_OK)
                return 1;
            if (modname[0] != '\0' && !lua_isnil(L, -1)) {
                luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
                lua_pushvalue(L, -2);
                lua_setfield(L, -2, modname);
                lua_pop(L, 1);
            }
            lua_pop(L, 1); /* chunk return value */
            data += csz;
            size -= csz;
        }
        return 0;
    }
    if (luaL_loadbuffer(L, (const char *)data, size, "@cart") != LUA_OK)
        return 1;
    if (lua_pcall(L, 0, 0, 0) != LUA_OK)
        return 1;
    return 0;
}

/* Register the blyt/blyt32 API surface a pure-Lua cart reaches for output and
 * termination: blyt.debug.print, blyt.quit, blyt.should_quit, blyt_quit,
 * blyt32.debug.print.  Mirrors the WASM leg's core registration in run_lua_cart
 * (the fuller surface — state buffers, gfx — lands in S3/#231). */
static void register_blyt_api(lua_State *L) {
    /* blyt32 = { debug = { print } } */
    lua_newtable(L);
    lua_newtable(L);
    lua_pushcfunction(L, l_debug_print);
    lua_setfield(L, -2, "print");
    lua_setfield(L, -2, "debug");
    lua_setglobal(L, "blyt32");

    /* blyt = { debug = { print }, quit, should_quit } */
    lua_newtable(L);
    lua_newtable(L);
    lua_pushcfunction(L, l_debug_print);
    lua_setfield(L, -2, "print");
    lua_setfield(L, -2, "debug");
    lua_pushcfunction(L, l_quit);
    lua_setfield(L, -2, "quit");
    lua_pushcfunction(L, l_should_quit);
    lua_setfield(L, -2, "should_quit");
    lua_setglobal(L, "blyt");

    lua_pushcfunction(L, l_quit);
    lua_setglobal(L, "blyt_quit");

    lua_pushcfunction(L, l_require);
    lua_setglobal(L, "require");
}

/* Open the sandboxed standard-library subset the host-Lua fast path exposes
 * (base/math/string/table/coroutine/utf8) — the SAME set, opened the SAME way
 * (luaL_requiref, not luaL_openlibs, so io/os stay out). */
static void open_libs(lua_State *L) {
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
}

/* ── State buffers (ADR-0009/0010) + S proxy + save/load ─────────────────────
 *
 * Ported from the WASM host-Lua fast path (wasm_main.c: buf_* helpers, the S
 * proxy generator, wasm_register_state_api).  The only structural change: the
 * active state ctx comes from the runner (hl->state_ctx) instead of a
 * file-scoped g_lua_state_ctx / session, since a native player could host more
 * than one runner.  Behaviour — the typed accessors, the generated S proxy, the
 * save/load hooks — is byte-for-byte the WASM leg's, so a state-buffer cart's
 * output is identical across every leg. */

/* The active state ctx: a hybrid cart (#232) shares its rv32 session's ctx so the
 * Lua and native halves see the SAME buffers (the native half reaches them via
 * BLYT_ECALL_BUF_OP); a pure-Lua cart uses the runner's standalone ctx.  The
 * native mirror of the WASM leg's active_state_ctx(). */
static blyt_state_ctx_t *hl_active_ctx(blyt_hostlua_t *hl) {
    if (!hl)
        return NULL;
    if (hl->session)
        return blyt_session_state_ctx(hl->session);
    return hl->state_ctx;
}

/* Save-slot location: a hybrid persists to its session's save dir / cart id so a
 * save round-trips through the same files as the emulated leg. */
static const char *hl_active_save_dir(blyt_hostlua_t *hl) {
    return hl->session ? blyt_session_save_dir(hl->session) : hl->save_dir;
}
static const char *hl_active_cart_name(blyt_hostlua_t *hl) {
    return hl->session ? blyt_session_cart_name(hl->session) : hl->cart_name;
}

static blyt_state_ctx_t *hl_ctx(lua_State *L) {
    return hl_active_ctx(hl_from(L));
}

static uint32_t buf_get_bits(lua_State *L) {
    uint32_t bits = 0;
    blyt_state_ctx_t *ctx = hl_ctx(L);
    if (ctx)
        blyt_state_get(ctx, (uint32_t)luaL_checkinteger(L, 1), (int32_t)luaL_checkinteger(L, 2),
                       (uint32_t)luaL_checkinteger(L, 3) & 0xFFFF, &bits);
    return bits;
}
static void buf_set_bits(lua_State *L, uint32_t bits, uint8_t type_tag) {
    blyt_state_ctx_t *ctx = hl_ctx(L);
    if (ctx)
        blyt_state_set(ctx, (uint32_t)luaL_checkinteger(L, 1), (int32_t)luaL_checkinteger(L, 2),
                       (uint32_t)luaL_checkinteger(L, 3) & 0xFFFF, bits, type_tag);
}

/* Type tags: i8=0, u8=1, i16=2, u16=3, i32=4, u32=5, f32=6, bool=7, f64=8 */
static int l_buf_get_f32(lua_State *L) {
    uint32_t bits = buf_get_bits(L);
    float f;
    memcpy(&f, &bits, 4);
    lua_pushnumber(L, (lua_Number)f);
    return 1;
}
static int l_buf_set_f32(lua_State *L) {
    float f = (float)luaL_checknumber(L, 4);
    uint32_t bits;
    memcpy(&bits, &f, 4);
    buf_set_bits(L, bits, 6);
    return 0;
}
static int l_buf_get_f64(lua_State *L) {
    uint64_t bits = 0;
    blyt_state_ctx_t *ctx = hl_ctx(L);
    if (ctx)
        blyt_state_get64(ctx, (uint32_t)luaL_checkinteger(L, 1), (int32_t)luaL_checkinteger(L, 2),
                         (uint32_t)luaL_checkinteger(L, 3) & 0xFFFF, &bits);
    double d;
    memcpy(&d, &bits, 8);
    lua_pushnumber(L, (lua_Number)d);
    return 1;
}
static int l_buf_set_f64(lua_State *L) {
    double d = (double)luaL_checknumber(L, 4);
    uint64_t bits;
    memcpy(&bits, &d, 8);
    blyt_state_ctx_t *ctx = hl_ctx(L);
    if (ctx)
        blyt_state_set64(ctx, (uint32_t)luaL_checkinteger(L, 1), (int32_t)luaL_checkinteger(L, 2),
                         (uint32_t)luaL_checkinteger(L, 3) & 0xFFFF, bits);
    return 0;
}
static int l_buf_get_i32(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)(int32_t)buf_get_bits(L));
    return 1;
}
static int l_buf_set_i32(lua_State *L) {
    buf_set_bits(L, (uint32_t)(int32_t)luaL_checkinteger(L, 4), 4);
    return 0;
}
static int l_buf_get_u32(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)buf_get_bits(L));
    return 1;
}
static int l_buf_set_u32(lua_State *L) {
    buf_set_bits(L, (uint32_t)luaL_checkinteger(L, 4), 5);
    return 0;
}
static int l_buf_get_i16(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)(int16_t)(uint16_t)buf_get_bits(L));
    return 1;
}
static int l_buf_set_i16(lua_State *L) {
    buf_set_bits(L, (uint32_t)(uint16_t)(int16_t)luaL_checkinteger(L, 4), 2);
    return 0;
}
static int l_buf_get_u16(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)(uint16_t)buf_get_bits(L));
    return 1;
}
static int l_buf_set_u16(lua_State *L) {
    buf_set_bits(L, (uint32_t)(uint16_t)luaL_checkinteger(L, 4), 3);
    return 0;
}
static int l_buf_get_i8(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)(int8_t)(uint8_t)buf_get_bits(L));
    return 1;
}
static int l_buf_set_i8(lua_State *L) {
    buf_set_bits(L, (uint32_t)(uint8_t)(int8_t)luaL_checkinteger(L, 4), 0);
    return 0;
}
static int l_buf_get_u8(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)(uint8_t)buf_get_bits(L));
    return 1;
}
static int l_buf_set_u8(lua_State *L) {
    buf_set_bits(L, (uint32_t)(uint8_t)luaL_checkinteger(L, 4), 1);
    return 0;
}
static int l_buf_get_bool(lua_State *L) {
    lua_pushboolean(L, buf_get_bits(L) ? 1 : 0);
    return 1;
}
static int l_buf_set_bool(lua_State *L) {
    buf_set_bits(L, lua_toboolean(L, 4) ? 1u : 0u, 7);
    return 0;
}
static int l_buf_alloc_slot(lua_State *L) {
    int32_t slot = -1;
    blyt_state_ctx_t *ctx = hl_ctx(L);
    if (ctx)
        blyt_state_alloc_slot(ctx, (uint32_t)luaL_checkinteger(L, 1), &slot);
    lua_pushinteger(L, slot);
    return 1;
}
static int l_buf_free_slot(lua_State *L) {
    blyt_state_ctx_t *ctx = hl_ctx(L);
    if (ctx)
        blyt_state_free_slot(ctx, (uint32_t)luaL_checkinteger(L, 1),
                             (int32_t)luaL_checkinteger(L, 2));
    return 0;
}
/* Packed entity refs (ADR-0096) — host-Lua equivalents of the blyt.buf.ref*
 * bindings in libblyt32lua. */
static int l_buf_ref(lua_State *L) {
    uint32_t ref = 0;
    blyt_state_ctx_t *ctx = hl_ctx(L);
    if (ctx)
        ref = blyt_state_ref(ctx, (uint32_t)luaL_checkinteger(L, 1),
                             (int32_t)luaL_checkinteger(L, 2));
    lua_pushinteger(L, (lua_Integer)ref);
    return 1;
}
static int l_buf_ref_valid(lua_State *L) {
    int v = 0;
    blyt_state_ctx_t *ctx = hl_ctx(L);
    if (ctx)
        v = blyt_state_ref_valid(ctx, (uint32_t)luaL_checkinteger(L, 1),
                                 (uint32_t)luaL_checkinteger(L, 2));
    lua_pushboolean(L, v);
    return 1;
}
static int l_buf_ref_slot(lua_State *L) {
    /* Pure bit math — must match blyt_buffer_ref_slot in blyt.h. */
    lua_pushinteger(L, (lua_Integer)((uint32_t)luaL_checkinteger(L, 1) & 0xFFFFu));
    return 1;
}

static int l_save_write(lua_State *L) {
    uint32_t slot = (uint32_t)luaL_checkinteger(L, 1);
    blyt_hostlua_t *hl = hl_from(L);
    /* Ask the cart to flush transient state into buffers before persisting. */
    lua_getglobal(L, "on_save_state");
    if (lua_isfunction(L, -1))
        lua_pcall(L, 0, 0, 0);
    else
        lua_pop(L, 1);
    int r = -1;
    blyt_state_ctx_t *ctx = hl_active_ctx(hl);
    if (hl && ctx)
        r = blyt_save_write(ctx, hl_active_save_dir(hl), hl_active_cart_name(hl), slot,
                            blyt_cart_save_version(hl->cart));
    lua_pushinteger(L, r);
    return 1;
}

static int l_save_read(lua_State *L) {
    uint32_t slot = (uint32_t)luaL_checkinteger(L, 1);
    blyt_hostlua_t *hl = hl_from(L);
    int r = -1;
    uint32_t saved_version = 0;
    blyt_state_ctx_t *ctx = hl_active_ctx(hl);
    if (hl && ctx)
        r = blyt_save_read(ctx, hl_active_save_dir(hl), hl_active_cart_name(hl), slot,
                           &saved_version);
    lua_pushinteger(L, r);
    if (r == BLYT_RUN_OK) {
        lua_getglobal(L, "on_load_state");
        if (lua_isfunction(L, -1)) {
            lua_newtable(L);
            lua_pushinteger(L, 0); /* reason=BLYT_LOAD_SAVE_GAME */
            lua_setfield(L, -2, "reason");
            lua_pushinteger(L, (lua_Integer)saved_version);
            lua_setfield(L, -2, "saved_cart_version");
            lua_pcall(L, 1, 0, 0);
        } else {
            lua_pop(L, 1);
        }
    }
    return 1;
}

/* Register blyt.buf.* + blyt.save_write/read (and the blyt32.* aliases) into the
 * Lua state.  Byte-for-byte the WASM leg's wasm_register_state_api. */
static void register_state_api(lua_State *L) {
    static const struct {
        const char *name;
        lua_CFunction fn;
    } buf_fns[] = {
        {"get_f32", l_buf_get_f32},
        {"set_f32", l_buf_set_f32},
        {"get_f64", l_buf_get_f64},
        {"set_f64", l_buf_set_f64},
        {"get_i32", l_buf_get_i32},
        {"set_i32", l_buf_set_i32},
        {"get_u32", l_buf_get_u32},
        {"set_u32", l_buf_set_u32},
        {"get_i16", l_buf_get_i16},
        {"set_i16", l_buf_set_i16},
        {"get_u16", l_buf_get_u16},
        {"set_u16", l_buf_set_u16},
        {"get_i8", l_buf_get_i8},
        {"set_i8", l_buf_set_i8},
        {"get_u8", l_buf_get_u8},
        {"set_u8", l_buf_set_u8},
        {"get_bool", l_buf_get_bool},
        {"set_bool", l_buf_set_bool},
        {"alloc_slot", l_buf_alloc_slot},
        {"free_slot", l_buf_free_slot},
        {"ref", l_buf_ref},
        {"ref_valid", l_buf_ref_valid},
        {"ref_slot", l_buf_ref_slot},
        {NULL, NULL},
    };
    lua_newtable(L); /* buf subtable */
    for (int i = 0; buf_fns[i].name; i++) {
        lua_pushcfunction(L, buf_fns[i].fn);
        lua_setfield(L, -2, buf_fns[i].name);
    }
    /* blyt.buf */
    lua_getglobal(L, "blyt");
    lua_pushvalue(L, -2);
    lua_setfield(L, -2, "buf");
    /* blyt32.buf = blyt.buf */
    lua_getglobal(L, "blyt32");
    if (lua_istable(L, -1)) {
        lua_pushvalue(L, -3);
        lua_setfield(L, -2, "buf");
    }
    lua_pop(L, 2); /* pop blyt32 (or nil) + blyt */
    lua_pop(L, 1); /* pop buf table */

    /* blyt.save_write / blyt.save_read */
    lua_getglobal(L, "blyt");
    lua_pushcfunction(L, l_save_write);
    lua_setfield(L, -2, "save_write");
    lua_pushcfunction(L, l_save_read);
    lua_setfield(L, -2, "save_read");
    lua_getglobal(L, "blyt32");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -2, "save_write");
        lua_setfield(L, -2, "save_write");
        lua_getfield(L, -2, "save_read");
        lua_setfield(L, -2, "save_read");
    }
    lua_pop(L, 2); /* pop blyt32 (or nil) + blyt */
}

/* Build + eval the Lua chunk that creates the `S` proxy global, mirroring the
 * packer-generated register_cart_state_S() but using blyt.buf.get_T/set_T (no
 * ECALL).  Byte-for-byte the WASM leg's wasm_register_s_proxy, reading the
 * runner's own ctx. */
static void register_s_proxy(lua_State *L, blyt_state_ctx_t *ctx) {
    static const char *type_names[] = {"i8", "u8", "i16", "u16", "i32", "u32", "f32", "bool"};

    if (!ctx || ctx->n_buffers == 0)
        return;

    size_t cap = 4096 + ctx->n_buffers * 600;
    for (uint32_t bi = 0; bi < ctx->n_buffers; bi++)
        cap += ctx->buffers[bi].n_fields * 150;

    char *buf = malloc(cap);
    if (!buf)
        return;
    size_t pos = 0;

#define APPEND(s)                                                                                  \
    do {                                                                                           \
        size_t _n = strlen(s);                                                                     \
        if (pos + _n + 1 <= cap) {                                                                 \
            memcpy(buf + pos, s, _n);                                                              \
            pos += _n;                                                                             \
            buf[pos] = '\0';                                                                       \
        }                                                                                          \
    } while (0)
#define APPENDF(...)                                                                               \
    do {                                                                                           \
        int _n = snprintf(buf + pos, cap - pos, __VA_ARGS__);                                      \
        if (_n > 0 && (size_t)_n < cap - pos)                                                      \
            pos += (size_t)_n;                                                                     \
    } while (0)

    APPEND("do\nlocal _buf=blyt.buf\nS={}\n");

    /* Integer constants: S.BUFNAME = buf_id, S.BUFNAME_FIELDNAME = field_h */
    for (uint32_t bi = 0; bi < ctx->n_buffers; bi++) {
        blyt_buffer_ctx_t *bc = &ctx->buffers[bi];
        uint32_t buf_id = bc->buf_id;

        APPENDF("S.");
        for (const char *p = bc->name; *p; p++)
            buf[pos++] = (char)toupper((unsigned char)*p);
        buf[pos] = '\0';
        APPENDF("=%u\n", buf_id);

        for (uint32_t fi = 0; fi < bc->n_fields; fi++) {
            uint32_t field_h = (buf_id << 16) | (fi + 1);
            APPENDF("S.");
            for (const char *p = bc->name; *p; p++)
                buf[pos++] = (char)toupper((unsigned char)*p);
            buf[pos++] = '_';
            buf[pos] = '\0';
            for (const char *p = bc->field_names[fi]; *p; p++)
                buf[pos++] = (char)toupper((unsigned char)*p);
            buf[pos] = '\0';
            APPENDF("=%u\n", field_h);
        }
    }

    /* Proxy tables: one per buffer */
    for (uint32_t bi = 0; bi < ctx->n_buffers; bi++) {
        blyt_buffer_ctx_t *bc = &ctx->buffers[bi];
        uint32_t buf_id = bc->buf_id;

        APPENDF("local _b%u_rmt={}\n", buf_id);

        APPENDF("_b%u_rmt.__index=function(t,k)\nlocal s=rawget(t,1)\n", buf_id);
        for (uint32_t fi = 0; fi < bc->n_fields; fi++) {
            uint8_t tag = bc->field_types[fi];
            const char *tname = (tag < 8) ? type_names[tag] : "i32";
            APPENDF("%s k==\"%s\" then return _buf.get_%s(%u,s,%u)\n", fi == 0 ? "if" : "elseif",
                    bc->field_names[fi], tname, buf_id, fi + 1);
        }
        APPEND("end\nend\n");

        APPENDF("_b%u_rmt.__newindex=function(t,k,v)\nlocal s=rawget(t,1)\n", buf_id);
        for (uint32_t fi = 0; fi < bc->n_fields; fi++) {
            uint8_t tag = bc->field_types[fi];
            const char *tname = (tag < 8) ? type_names[tag] : "i32";
            APPENDF("%s k==\"%s\" then _buf.set_%s(%u,s,%u,v)\n", fi == 0 ? "if" : "elseif",
                    bc->field_names[fi], tname, buf_id, fi + 1);
        }
        APPEND("end\nend\n");

        APPENDF("local _b%u_rows={}\n", buf_id);
        APPENDF("for i=0,%u do local r={i};setmetatable(r,_b%u_rmt);_b%u_rows[i]=r end\n",
                bc->count > 0 ? bc->count - 1 : 0, buf_id, buf_id);

        APPENDF("S.%s=setmetatable({},{__index=function(t,k) if k==\"count\" then return %u end "
                "return _b%u_rows[k] end})\n",
                bc->name, bc->count, buf_id);
    }

    APPEND("end\n");

#undef APPEND
#undef APPENDF

    if (luaL_loadbuffer(L, buf, pos, "@s_proxy") != LUA_OK) {
        fprintf(stderr, "[blyt] register_s_proxy load error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    } else if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        fprintf(stderr, "[blyt] register_s_proxy eval error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }

    free(buf);
}

/* Typed resource-constant userdata (ADR-0068/0134, #166/#214): typed-ness is the
 * metatable.  Declared up here so the gfx palette_set (below) can accept a palette
 * constant (R.<NAME>); the accessors/registration live in the resource section. */
#define HL_RESOURCE_TEXT_CONST_MT "blyt.resource.text_const"
#define HL_RESOURCE_BYTES_CONST_MT "blyt.resource.bytes_const"
#define HL_RESOURCE_PALETTE_CONST_MT "blyt.resource.palette_const"

typedef struct {
    uint32_t id; /* the baked console-wide constant (ADR-0134) */
    int is_text;
} hl_resource_const_t;

/* Resolve a palette handle to 256-entry XRGB8888 bytes (built-in or cart asset);
 * defined in the resource section. */
static const uint8_t *hl_resolve_palette(blyt_hostlua_t *hl, uint32_t handle);

/* ── Gfx fast path (#188 / Spike X, #231) ────────────────────────────────────
 *
 * blyt32.gfx.* rasterize into the runner's own paletted framebuffer via the
 * shared integer rasterizer (blyt_raster.c) — the SAME core the emulated gfx
 * ECALL handlers (cart_run.c) and the WASM host-Lua bindings (wasm_main.c) run,
 * so the pixels are bit-identical to every other leg.  A pure-Lua cart on this
 * path has no session (blyt_hostlua_should_use rejects hybrids), so the draw
 * target is always the runner's standalone fb — none of the WASM leg's
 * "session buffer when hybrid" routing is needed here. */

/* Seed the palette before init, matching the pre-init auto-load the emulated
 * legs perform into session->palette (#201/#204, ADR-0088): the cart's declared
 * `palettes: default:` handle when it names a built-in, else the runtime default
 * (aurora).  A cart-provenance custom default palette (#214) needs the resource
 * table (deferred to #231's heap half); until then it falls back to aurora.  So
 * a cart that never calls palette_set renders — and its testcard remaps —
 * against the same palette on every leg. */
static void hl_palette_ensure_default(blyt_hostlua_t *hl) {
    uint32_t handle = blyt_cart_default_palette(hl->cart);
    if (handle == 0)
        handle = BLYT_RESOURCE_ENCODE(BLYT_PAL_ID_AURORA, BLYT_RESOURCE_PROV_RUNTIME);
    const uint8_t *pal = hl_resolve_palette(hl, handle); /* built-in OR cart asset (#214) */
    if (!pal)
        pal = (const uint8_t *)blyt_builtin_palette(
            BLYT_RESOURCE_ENCODE(BLYT_PAL_ID_AURORA, BLYT_RESOURCE_PROV_RUNTIME));
    /* Explicit little-endian decode: cart-resource bytes may be unaligned, and an
     * explicit LE reconstruction stays bit-identical to every other leg. */
    for (int i = 0; i < 256; i++)
        hl->palette[i] = (uint32_t)pal[i * 4] | ((uint32_t)pal[i * 4 + 1] << 8) |
                         ((uint32_t)pal[i * 4 + 2] << 16) | ((uint32_t)pal[i * 4 + 3] << 24);
}

static int l_gfx_clear(lua_State *L) {
    blyt_hostlua_t *hl = hl_from(L);
    blyt_raster_clear(hl->fb, BLYT_FRAME_W, BLYT_FRAME_W, BLYT_FRAME_H,
                      (uint8_t)luaL_checkinteger(L, 1));
    hl->drawn = true;
    return 0;
}
static int l_gfx_pixel(lua_State *L) {
    blyt_hostlua_t *hl = hl_from(L);
    blyt_raster_pixel(hl->fb, BLYT_FRAME_W, BLYT_FRAME_W, BLYT_FRAME_H,
                      (int)luaL_checkinteger(L, 1), (int)luaL_checkinteger(L, 2),
                      (uint8_t)luaL_checkinteger(L, 3));
    hl->drawn = true;
    return 0;
}
static int l_gfx_rect_fill(lua_State *L) {
    blyt_hostlua_t *hl = hl_from(L);
    blyt_raster_rect_fill(hl->fb, BLYT_FRAME_W, BLYT_FRAME_W, BLYT_FRAME_H,
                          (int)luaL_checkinteger(L, 1), (int)luaL_checkinteger(L, 2),
                          (int)luaL_checkinteger(L, 3), (int)luaL_checkinteger(L, 4),
                          (uint8_t)luaL_checkinteger(L, 5));
    hl->drawn = true;
    return 0;
}
static int l_gfx_line(lua_State *L) {
    blyt_hostlua_t *hl = hl_from(L);
    blyt_raster_line(hl->fb, BLYT_FRAME_W, BLYT_FRAME_W, BLYT_FRAME_H, (int)luaL_checkinteger(L, 1),
                     (int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3),
                     (int)luaL_checkinteger(L, 4), (uint8_t)luaL_checkinteger(L, 5));
    hl->drawn = true;
    return 0;
}

/* blyt32.gfx.palette_set(handle|R.<NAME>) (#201/#214): load a built-in OR a
 * cart-asset palette into the active palette.  Accepts an integer built-in handle
 * or a palette-constant userdata; a no-op on a handle that does not resolve to a
 * 256-entry palette (the SAME defined outcome as the WASM leg). */
static int l_gfx_palette_set(lua_State *L) {
    blyt_hostlua_t *hl = hl_from(L);
    hl_resource_const_t *c =
        (hl_resource_const_t *)luaL_testudata(L, 1, HL_RESOURCE_PALETTE_CONST_MT);
    uint32_t handle = c ? c->id : (uint32_t)luaL_checkinteger(L, 1);
    const uint8_t *pal = hl_resolve_palette(hl, handle);
    if (pal) {
        for (int i = 0; i < 256; i++)
            hl->palette[i] = (uint32_t)pal[i * 4] | ((uint32_t)pal[i * 4 + 1] << 8) |
                             ((uint32_t)pal[i * 4 + 2] << 16) | ((uint32_t)pal[i * 4 + 3] << 24);
    }
    return 0;
}

/* Register blyt32.gfx.* onto the existing blyt32 global.  Mirrors the WASM leg's
 * wasm_register_gfx_api (the session-less subset); called from build_vm so the
 * fresh VM each reset cycle re-registers it. */
static void register_gfx_api(lua_State *L) {
    lua_getglobal(L, "blyt32");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_newtable(L); /* blyt32.gfx */
    static const struct {
        const char *name;
        lua_CFunction fn;
    } gfx_fns[] = {
        {"clear", l_gfx_clear},
        {"pixel", l_gfx_pixel},
        {"rect_fill", l_gfx_rect_fill},
        {"line", l_gfx_line},
        {"palette_set", l_gfx_palette_set},
        {NULL, NULL},
    };
    for (int i = 0; gfx_fns[i].name; i++) {
        lua_pushcfunction(L, gfx_fns[i].fn);
        lua_setfield(L, -2, gfx_fns[i].name);
    }
    /* Built-in palette constants (#201), mirroring the WASM leg + blyt32lua.c. */
    lua_pushinteger(
        L, (lua_Integer)BLYT_RESOURCE_ENCODE(BLYT_PAL_ID_AURORA, BLYT_RESOURCE_PROV_RUNTIME));
    lua_setfield(L, -2, "PALETTE_AURORA");
    lua_pushinteger(L,
                    (lua_Integer)BLYT_RESOURCE_ENCODE(BLYT_PAL_ID_VGA, BLYT_RESOURCE_PROV_RUNTIME));
    lua_setfield(L, -2, "PALETTE_VGA");
    lua_pushinteger(L,
                    (lua_Integer)BLYT_RESOURCE_ENCODE(BLYT_PAL_ID_EGA, BLYT_RESOURCE_PROV_RUNTIME));
    lua_setfield(L, -2, "PALETTE_EGA");
    lua_pushinteger(L,
                    (lua_Integer)BLYT_RESOURCE_ENCODE(BLYT_PAL_ID_CGA, BLYT_RESOURCE_PROV_RUNTIME));
    lua_setfield(L, -2, "PALETTE_CGA");
    lua_pushinteger(
        L, (lua_Integer)BLYT_RESOURCE_ENCODE(BLYT_PAL_ID_AURORA, BLYT_RESOURCE_PROV_RUNTIME));
    lua_setfield(L, -2, "PALETTE_DEFAULT");
    lua_setfield(L, -2, "gfx"); /* blyt32.gfx = gfx */

    /* blyt32.color: named color-index constants (#203).  Host-side (not cart
     * code), so these raw indices MUST match blyt.h's BLYT_EGA_* / BLYT_AURORA_*;
     * the cross-leg frame-hash parity test is the guard against drift.  Byte-for-
     * byte the WASM leg's blyt32.color registration. */
    static const char *const color_names[16] = {
        "BLACK",  "BLUE",    "GREEN",    "CYAN",    "RED",    "MAGENTA",    "BROWN",     "LTGRAY",
        "DKGRAY", "BR_BLUE", "BR_GREEN", "BR_CYAN", "BR_RED", "BR_MAGENTA", "BR_YELLOW", "WHITE",
    };
    static const uint8_t ega_idx[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    static const uint8_t aurora_idx[16] = {0, 223, 185, 195, 155, 239, 165, 10,
                                           5, 219, 189, 201, 160, 236, 175, 15};
    lua_newtable(L); /* blyt32.color */
    for (int pass = 0; pass < 2; pass++) { /* color.ega, color.vga (same set) */
        lua_newtable(L);
        for (int i = 0; i < 16; i++) {
            lua_pushinteger(L, (lua_Integer)ega_idx[i]);
            lua_setfield(L, -2, color_names[i]);
        }
        lua_setfield(L, -2, pass == 0 ? "ega" : "vga");
    }
    lua_newtable(L); /* color.aurora */
    for (int i = 0; i < 16; i++) {
        lua_pushinteger(L, (lua_Integer)aurora_idx[i]);
        lua_setfield(L, -2, color_names[i]);
    }
    lua_setfield(L, -2, "aurora");
    for (int i = 0; i < 16; i++) { /* default aliases on color root -> aurora */
        lua_pushinteger(L, (lua_Integer)aurora_idx[i]);
        lua_setfield(L, -2, color_names[i]);
    }
    lua_setfield(L, -2, "color"); /* blyt32.color = color */

    lua_pop(L, 1); /* pop blyt32 */
}

/* Emit the deterministic framebuffer + palette hashes when BLYT_FRAME_HASH is
 * set — the host-Lua fast path's equivalent of the host runtime's
 * blyt_emit_frame_hash (cart_run.c), over the same paletted bytes so every leg's
 * hash matches (#188/#204).  Off by default; zero cost when unset. */
static void hl_emit_frame_hash(blyt_hostlua_t *hl) {
    static int s_on = -1;
    if (s_on < 0)
        s_on = getenv("BLYT_FRAME_HASH") != NULL ? 1 : 0;
    if (!s_on || !hl->log_fn)
        return;
    char buf[64];
    uint64_t h = blyt_frame_hash(hl->fb, (size_t)BLYT_FRAME_W * (size_t)BLYT_FRAME_H);
    snprintf(buf, sizeof(buf), "[blyt:fbhash] %016llx", (unsigned long long)h);
    hl->log_fn(buf);
    uint64_t ph = blyt_frame_hash((const uint8_t *)hl->palette, 256 * sizeof(uint32_t));
    snprintf(buf, sizeof(buf), "[blyt:palhash] %016llx", (unsigned long long)ph);
    hl->log_fn(buf);
}

/* Finalise a completed frame: while the cart has not drawn, render the testcard
 * into fb (mirroring the emulated path's !cart_has_drawn testcard in
 * blyt_session_run_frame), then emit the cross-leg frame hash. */
static void hl_frame_done(blyt_hostlua_t *hl) {
    if (!hl->drawn)
        blyt_testcard_draw(hl->frame_count++, hl->palette, hl->fb);
    hl_emit_frame_hash(hl);
}

/* ── Surface fast path (#205/#208, #231) ─────────────────────────────────────
 *
 * The session-less half of the WASM leg's blyt32.surface.* bindings.  A pure-Lua
 * cart's surfaces live in hl->surf; the SAME shared rasterizer draws them, so
 * they are pixel-identical to every leg.  Tier-1 (create/destroy/clear/pixel/
 * rect_fill/line/blit) plus the tier-2 per-pixel lock (acquire → get/set/clear/
 * rect_fill/line → release).  None of the WASM leg's hybrid/session routing is
 * needed — this path never has a session. */

#define HL_LOCK_MT "blyt.surface.lock"

static void hl_surf_ensure_init(blyt_hostlua_t *hl) {
    if (hl->surf_init)
        return;
    hl->surf[0].in_use = true;
    hl->surf[0].is_screen = true;
    hl->surf[0].w = BLYT_FRAME_W;
    hl->surf[0].h = BLYT_FRAME_H;
    hl->surf_init = true;
}

static hl_surface_t *hl_surf_resolve(blyt_hostlua_t *hl, uint32_t h) {
    hl_surf_ensure_init(hl);
    if (!blyt_handle_is_surface(h))
        return NULL;
    uint32_t idx = blyt_dyn_decode_index(h);
    if (idx >= HL_SURFACE_MAX)
        return NULL;
    hl_surface_t *s = &hl->surf[idx];
    if (!s->in_use || (uint16_t)blyt_dyn_decode_gen(h) != s->gen)
        return NULL;
    if (s->is_screen)
        s->pixels = hl->fb; /* screen pixels are always the live fb */
    return s;
}

/* #207: while a tier-2 lock is held the lock owns the surface, so tier-1 ops /
 * blit / destroy through the handle are rejected (defined no-op). */
static hl_surface_t *hl_surf_resolve_drawable(blyt_hostlua_t *hl, uint32_t h) {
    hl_surface_t *s = hl_surf_resolve(hl, h);
    return (s && s->locked) ? NULL : s;
}

typedef struct {
    uint8_t *pixels;
    int32_t w, h;
    bool is_screen;
} hl_draw_target_t;

static bool hl_resolve_target(blyt_hostlua_t *hl, uint32_t h, hl_draw_target_t *t) {
    hl_surface_t *s = hl_surf_resolve_drawable(hl, h);
    if (!s)
        return false;
    t->pixels = s->pixels;
    t->w = s->w;
    t->h = s->h;
    t->is_screen = s->is_screen;
    return true;
}

/* Reap draw-scoped off-screen surfaces at the frame boundary (#205) and
 * force-release any lock the cart forgot — including the screen (slot 0), which
 * is never freed but must not carry a lock into the next frame (#208).  Mirrors
 * the emulated path's frame-entry reap in blyt_session_run_frame. */
static void hl_surf_reap(blyt_hostlua_t *hl) {
    hl_surf_ensure_init(hl);
    hl->surf[0].locked = false;
    for (uint32_t i = 1; i < HL_SURFACE_MAX; i++) {
        if (hl->surf[i].in_use) {
            free(hl->surf[i].pixels);
            hl->surf[i].pixels = NULL;
            hl->surf[i].in_use = false;
            hl->surf[i].locked = false;
            hl->surf[i].gen++;
        }
    }
}

static int l_surface_create(lua_State *L) {
    blyt_hostlua_t *hl = hl_from(L);
    int32_t w = (int32_t)luaL_checkinteger(L, 1);
    int32_t h = (int32_t)luaL_checkinteger(L, 2);
    hl_surf_ensure_init(hl);
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) {
        lua_pushinteger(L, (lua_Integer)BLYT_HANDLE_NONE);
        return 1;
    }
    uint32_t idx = 0;
    for (uint32_t i = 1; i < HL_SURFACE_MAX; i++) {
        if (!hl->surf[i].in_use) {
            idx = i;
            break;
        }
    }
    uint8_t *buf = idx ? (uint8_t *)malloc((size_t)w * (size_t)h) : NULL;
    if (!buf) {
        lua_pushinteger(L, (lua_Integer)BLYT_HANDLE_NONE);
        return 1;
    }
    blyt_raster_clear(buf, w, w, h, 0); /* blank = palette index 0 */
    hl->surf[idx].pixels = buf;
    hl->surf[idx].w = w;
    hl->surf[idx].h = h;
    hl->surf[idx].in_use = true;
    hl->surf[idx].is_screen = false;
    lua_pushinteger(L, (lua_Integer)blyt_surface_encode(hl->surf[idx].gen, idx));
    return 1;
}

static int l_surface_destroy(lua_State *L) {
    blyt_hostlua_t *hl = hl_from(L);
    hl_surface_t *s = hl_surf_resolve_drawable(hl, (uint32_t)luaL_checkinteger(L, 1)); /* #207 */
    if (s && !s->is_screen) {
        free(s->pixels);
        s->pixels = NULL;
        s->in_use = false;
        s->gen++;
    }
    return 0;
}

static int l_surface_clear(lua_State *L) {
    blyt_hostlua_t *hl = hl_from(L);
    hl_draw_target_t t;
    if (hl_resolve_target(hl, (uint32_t)luaL_checkinteger(L, 1), &t)) {
        blyt_raster_clear(t.pixels, t.w, t.w, t.h, (uint8_t)luaL_checkinteger(L, 2));
        if (t.is_screen)
            hl->drawn = true;
    }
    return 0;
}
static int l_surface_pixel(lua_State *L) {
    blyt_hostlua_t *hl = hl_from(L);
    hl_draw_target_t t;
    if (hl_resolve_target(hl, (uint32_t)luaL_checkinteger(L, 1), &t)) {
        blyt_raster_pixel(t.pixels, t.w, t.w, t.h, (int)luaL_checkinteger(L, 2),
                          (int)luaL_checkinteger(L, 3), (uint8_t)luaL_checkinteger(L, 4));
        if (t.is_screen)
            hl->drawn = true;
    }
    return 0;
}
static int l_surface_rect_fill(lua_State *L) {
    blyt_hostlua_t *hl = hl_from(L);
    hl_draw_target_t t;
    if (hl_resolve_target(hl, (uint32_t)luaL_checkinteger(L, 1), &t)) {
        blyt_raster_rect_fill(t.pixels, t.w, t.w, t.h, (int)luaL_checkinteger(L, 2),
                              (int)luaL_checkinteger(L, 3), (int)luaL_checkinteger(L, 4),
                              (int)luaL_checkinteger(L, 5), (uint8_t)luaL_checkinteger(L, 6));
        if (t.is_screen)
            hl->drawn = true;
    }
    return 0;
}
static int l_surface_line(lua_State *L) {
    blyt_hostlua_t *hl = hl_from(L);
    hl_draw_target_t t;
    if (hl_resolve_target(hl, (uint32_t)luaL_checkinteger(L, 1), &t)) {
        blyt_raster_line(t.pixels, t.w, t.w, t.h, (int)luaL_checkinteger(L, 2),
                         (int)luaL_checkinteger(L, 3), (int)luaL_checkinteger(L, 4),
                         (int)luaL_checkinteger(L, 5), (uint8_t)luaL_checkinteger(L, 6));
        if (t.is_screen)
            hl->drawn = true;
    }
    return 0;
}
static int l_surface_blit(lua_State *L) {
    blyt_hostlua_t *hl = hl_from(L);
    /* Reject if either endpoint is unresolvable or locked (#207). */
    hl_draw_target_t d, s;
    if (hl_resolve_target(hl, (uint32_t)luaL_checkinteger(L, 1), &d) &&
        hl_resolve_target(hl, (uint32_t)luaL_checkinteger(L, 2), &s)) {
        blyt_raster_blit(d.pixels, d.w, d.w, d.h, s.pixels, s.w, s.w, s.h,
                         (int)luaL_checkinteger(L, 3), (int)luaL_checkinteger(L, 4));
        if (d.is_screen)
            hl->drawn = true;
    }
    return 0;
}

/* Tier-2 per-pixel lock (#208).  acquire() materialises a direct pointer to the
 * surface's canonical buffer and marks it locked; per-pixel get/set then touch
 * host memory with no crossing.  The per-frame epoch (hl->lock_epoch) is the
 * staleness guard.  An OOB get/set is a defined no-op (the determinism-bearing
 * behaviour, uniform across legs; the debug hard-error is not wired on the fast
 * path, exactly as on the WASM leg). */
typedef struct {
    uint8_t *pixels;
    int32_t w, h;
    uint32_t handle; /* surface handle (release re-resolve) */
    uint32_t epoch; /* hl->lock_epoch captured at acquire */
    bool released;
    bool is_screen; /* writes flip hl->drawn */
} hl_lock_t;

static int l_surface_acquire(lua_State *L) {
    blyt_hostlua_t *hl = hl_from(L);
    uint32_t h = (uint32_t)luaL_checkinteger(L, 1);
    hl_surface_t *s = hl_surf_resolve(hl, h);
    if (!s || s->locked) {
        lua_pushnil(L);
        return 1;
    }
    s->locked = true;
    hl_lock_t *u = (hl_lock_t *)lua_newuserdatauv(L, sizeof(*u), 0);
    u->pixels = s->pixels;
    u->w = s->w;
    u->h = s->h;
    u->handle = h;
    u->epoch = hl->lock_epoch;
    u->released = false;
    u->is_screen = s->is_screen;
    luaL_setmetatable(L, HL_LOCK_MT);
    return 1;
}

static hl_lock_t *hl_lock_live(lua_State *L, int idx) {
    blyt_hostlua_t *hl = hl_from(L);
    hl_lock_t *u = (hl_lock_t *)luaL_checkudata(L, idx, HL_LOCK_MT);
    if (u->released || u->epoch != hl->lock_epoch)
        return NULL;
    return u;
}

static int l_lock_get(lua_State *L) {
    hl_lock_t *u = hl_lock_live(L, 1);
    int32_t x = (int32_t)luaL_checkinteger(L, 2);
    int32_t y = (int32_t)luaL_checkinteger(L, 3);
    if (!u || x < 0 || x >= u->w || y < 0 || y >= u->h) {
        lua_pushinteger(L, 0);
        return 1;
    }
    lua_pushinteger(L, (lua_Integer)u->pixels[(uint32_t)y * (uint32_t)u->w + (uint32_t)x]);
    return 1;
}
static int l_lock_set(lua_State *L) {
    blyt_hostlua_t *hl = hl_from(L);
    hl_lock_t *u = hl_lock_live(L, 1);
    int32_t x = (int32_t)luaL_checkinteger(L, 2);
    int32_t y = (int32_t)luaL_checkinteger(L, 3);
    uint8_t c = (uint8_t)luaL_checkinteger(L, 4);
    if (!u || x < 0 || x >= u->w || y < 0 || y >= u->h)
        return 0;
    u->pixels[(uint32_t)y * (uint32_t)u->w + (uint32_t)x] = c;
    if (u->is_screen)
        hl->drawn = true;
    return 0;
}
static int l_lock_clear(lua_State *L) {
    blyt_hostlua_t *hl = hl_from(L);
    hl_lock_t *u = hl_lock_live(L, 1);
    if (u) {
        blyt_raster_clear(u->pixels, u->w, u->w, u->h, (uint8_t)luaL_checkinteger(L, 2));
        if (u->is_screen)
            hl->drawn = true;
    }
    return 0;
}
static int l_lock_rect_fill(lua_State *L) {
    blyt_hostlua_t *hl = hl_from(L);
    hl_lock_t *u = hl_lock_live(L, 1);
    if (u) {
        blyt_raster_rect_fill(u->pixels, u->w, u->w, u->h, (int)luaL_checkinteger(L, 2),
                              (int)luaL_checkinteger(L, 3), (int)luaL_checkinteger(L, 4),
                              (int)luaL_checkinteger(L, 5), (uint8_t)luaL_checkinteger(L, 6));
        if (u->is_screen)
            hl->drawn = true;
    }
    return 0;
}
static int l_lock_line(lua_State *L) {
    blyt_hostlua_t *hl = hl_from(L);
    hl_lock_t *u = hl_lock_live(L, 1);
    if (u) {
        blyt_raster_line(u->pixels, u->w, u->w, u->h, (int)luaL_checkinteger(L, 2),
                         (int)luaL_checkinteger(L, 3), (int)luaL_checkinteger(L, 4),
                         (int)luaL_checkinteger(L, 5), (uint8_t)luaL_checkinteger(L, 6));
        if (u->is_screen)
            hl->drawn = true;
    }
    return 0;
}
static int l_lock_release(lua_State *L) {
    blyt_hostlua_t *hl = hl_from(L);
    hl_lock_t *u = (hl_lock_t *)luaL_checkudata(L, 1, HL_LOCK_MT);
    if (!u->released && u->epoch == hl->lock_epoch) {
        hl_surface_t *s = hl_surf_resolve(hl, u->handle);
        if (s)
            s->locked = false;
    }
    u->released = true;
    return 0;
}

static void register_surface_lock_mt(lua_State *L) {
    luaL_newmetatable(L, HL_LOCK_MT);
    lua_newtable(L); /* __index */
    static const luaL_Reg lock_methods[] = {
        {"get", l_lock_get},
        {"set", l_lock_set},
        {"clear", l_lock_clear},
        {"rect_fill", l_lock_rect_fill},
        {"line", l_lock_line},
        {"release", l_lock_release},
        {NULL, NULL},
    };
    luaL_setfuncs(L, lock_methods, 0);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1); /* pop mt */
}

/* Register blyt32.surface.* onto the existing blyt32 global + the lock
 * metatable.  Mirrors the WASM leg's surface registration (session-less half). */
static void register_surface_api(lua_State *L) {
    lua_getglobal(L, "blyt32");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_newtable(L); /* blyt32.surface */
    static const struct {
        const char *name;
        lua_CFunction fn;
    } surface_fns[] = {
        {"create", l_surface_create},
        {"destroy", l_surface_destroy},
        {"clear", l_surface_clear},
        {"pixel", l_surface_pixel},
        {"rect_fill", l_surface_rect_fill},
        {"line", l_surface_line},
        {"blit", l_surface_blit},
        {"acquire", l_surface_acquire},
        {NULL, NULL},
    };
    for (int i = 0; surface_fns[i].name; i++) {
        lua_pushcfunction(L, surface_fns[i].fn);
        lua_setfield(L, -2, surface_fns[i].name);
    }
    lua_pushinteger(L, (lua_Integer)BLYT_SCREEN); /* blyt32.surface.SCREEN */
    lua_setfield(L, -2, "SCREEN");
    lua_setfield(L, -2, "surface"); /* blyt32.surface = surface */
    lua_pop(L, 1); /* pop blyt32 */

    register_surface_lock_mt(L);
}

/* ── Resource table + mem.stats (#93/#158/#159, #231) ────────────────────────
 *
 * The session-less half of the WASM leg's blyt.resource.* / blyt32.mem.stats
 * bindings.  A pure-Lua cart reads the runner's own resource table directly (no
 * ECALL, no session); the pin/refcount/footprint semantics replicate the rv32
 * RESOURCE_* ECALL handlers in cart_run.c, so the host-Lua path is behaviourally
 * identical to every emulated leg.  Footprint is published into mem_acct's
 * non_evictable_footprint, feeding the SAME unified budget predicate the cart
 * heap uses. */

static blyt_resource_table_t *hl_active_resources(blyt_hostlua_t *hl) {
    return hl->resources_loaded ? &hl->resources : NULL;
}

/* Republish the non-evictable footprint from the table and bound the resident
 * evictable cache to the room it leaves — the host-Lua mirror of the host
 * mem_acct_publish_footprint (#158).  Call after any pin/unpin. */
static void hl_publish_footprint(blyt_hostlua_t *hl, blyt_resource_table_t *t) {
    if (!t)
        return;
    uint32_t footprint = blyt_resource_table_footprint(t);
    hl->mem_acct.non_evictable_footprint = footprint;
    blyt_resource_table_evict_to_fit(
        t, blyt_mem_cache_room(blyt_mem_cart_heap(&hl->mem_acct), footprint));
}

/* Would newly pinning `e` (adding e->len to the footprint when it is so far
 * evictable) still fit the unified budget?  Mirrors mem_acct_reference_fits. */
static int hl_reference_fits(blyt_hostlua_t *hl, blyt_resource_table_t *t,
                             const blyt_resource_entry_t *e, int was_evictable) {
    uint32_t incoming = was_evictable ? (uint32_t)e->len : 0u;
    return blyt_mem_alloc_fits(blyt_mem_cart_heap(&hl->mem_acct), blyt_resource_table_footprint(t),
                               incoming);
}

/* Resolve a baked resource constant to its table entry (ADR-0134): a cart-bundled
 * RESOURCE only.  NULL for a non-resource kind, runtime provenance, or absent id. */
static blyt_resource_entry_t *hl_resolve(blyt_resource_table_t *t, uint32_t handle) {
    if (!t || !blyt_handle_is_resource(handle) ||
        blyt_resource_decode_provenance(handle) != BLYT_RESOURCE_PROV_CART)
        return NULL;
    return blyt_resource_table_find_mut(t, blyt_resource_decode_id(handle));
}

/* Pin exactly as the rv32 RESOURCE_PIN handler does: budget-gate, materialize,
 * bump pin, touch for LRU, republish footprint.  *out = resident bytes (NULL/0
 * is a valid success), false on absent / over budget / decode failure. */
static bool hl_pin_entry(blyt_hostlua_t *hl, blyt_resource_table_t *t, blyt_resource_entry_t *e,
                         const uint8_t **out) {
    *out = NULL;
    if (!e || !hl_reference_fits(hl, t, e, blyt_rl_is_evictable(&e->rl)))
        return false;
    const uint8_t *bytes = blyt_resource_entry_data(e);
    if (!bytes && e->len)
        return false; /* decode failed */
    blyt_rl_pin(&e->rl);
    blyt_resource_table_touch(t, e); /* recency for LRU (#158) */
    hl_publish_footprint(hl, t); /* footprint grew; bound cache */
    *out = bytes;
    return true;
}

static void hl_unpin_entry(blyt_hostlua_t *hl, blyt_resource_table_t *t, blyt_resource_entry_t *e) {
    blyt_rl_unpin(&e->rl);
    hl_publish_footprint(hl, t); /* footprint may have shrunk (#158) */
}

/* Resolve a palette handle to its 256-entry XRGB8888 bytes (#201/#214): RUNTIME
 * -> built-in table; CART -> the resource table (must hold exactly 1024 bytes). */
static const uint8_t *hl_resolve_palette(blyt_hostlua_t *hl, uint32_t handle) {
    if (blyt_resource_decode_provenance(handle) == BLYT_RESOURCE_PROV_RUNTIME)
        return (const uint8_t *)blyt_builtin_palette(handle);
    blyt_resource_entry_t *e = hl_resolve(hl_active_resources(hl), handle);
    if (!e || e->len != 256u * sizeof(uint32_t))
        return NULL;
    return blyt_resource_entry_data(e);
}

static hl_resource_const_t *hl_opt_const(lua_State *L, int idx) {
    void *p = luaL_testudata(L, idx, HL_RESOURCE_TEXT_CONST_MT);
    if (!p)
        p = luaL_testudata(L, idx, HL_RESOURCE_BYTES_CONST_MT);
    if (!p)
        p = luaL_testudata(L, idx, HL_RESOURCE_PALETTE_CONST_MT);
    return (hl_resource_const_t *)p;
}

static int l_res_text_resource(lua_State *L) {
    hl_resource_const_t *c = (hl_resource_const_t *)lua_newuserdatauv(L, sizeof(*c), 0);
    c->id = (uint32_t)luaL_checkinteger(L, 1);
    c->is_text = 1;
    luaL_setmetatable(L, HL_RESOURCE_TEXT_CONST_MT);
    return 1;
}
static int l_res_bytes_resource(lua_State *L) {
    hl_resource_const_t *c = (hl_resource_const_t *)lua_newuserdatauv(L, sizeof(*c), 0);
    c->id = (uint32_t)luaL_checkinteger(L, 1);
    c->is_text = 0;
    luaL_setmetatable(L, HL_RESOURCE_BYTES_CONST_MT);
    return 1;
}
static int l_res_palette_resource(lua_State *L) {
    hl_resource_const_t *c = (hl_resource_const_t *)lua_newuserdatauv(L, sizeof(*c), 0);
    c->id = (uint32_t)luaL_checkinteger(L, 1);
    c->is_text = 0;
    luaL_setmetatable(L, HL_RESOURCE_PALETTE_CONST_MT);
    return 1;
}
static int l_res_palette_tostring(lua_State *L) {
    hl_resource_const_t *c = luaL_checkudata(L, 1, HL_RESOURCE_PALETTE_CONST_MT);
    lua_pushfstring(L, "palette<%d>", (int)c->id);
    return 1;
}
static int l_res_const_id(lua_State *L) {
    hl_resource_const_t *c = hl_opt_const(L, 1);
    luaL_argcheck(L, c != NULL, 1, "resource constant expected");
    lua_pushinteger(L, (lua_Integer)c->id);
    return 1;
}
static int l_res_const_eq(lua_State *L) {
    hl_resource_const_t *a = hl_opt_const(L, 1);
    hl_resource_const_t *b = hl_opt_const(L, 2);
    lua_pushboolean(L, a && b && a->id == b->id && a->is_text == b->is_text);
    return 1;
}
static int l_res_const_tostring(lua_State *L) {
    hl_resource_const_t *c = hl_opt_const(L, 1);
    luaL_argcheck(L, c != NULL, 1, "resource constant expected");
    lua_pushfstring(L, c->is_text ? "text_resource<%d>" : "bytes_resource<%d>", (int)c->id);
    return 1;
}
/* text constant :text() — owned copy, trailing storage NUL stripped (#166). */
static int l_res_const_text(lua_State *L) {
    hl_resource_const_t *c = luaL_checkudata(L, 1, HL_RESOURCE_TEXT_CONST_MT);
    blyt_hostlua_t *hl = hl_from(L);
    blyt_resource_table_t *t = hl_active_resources(hl);
    blyt_resource_entry_t *e = hl_resolve(t, c->id);
    const uint8_t *bytes = NULL;
    if (!hl_pin_entry(hl, t, e, &bytes)) {
        lua_pushnil(L);
        return 1;
    }
    size_t content = (e->len >= 1 && bytes[e->len - 1] == '\0') ? e->len - 1 : e->len;
    lua_pushlstring(L, (const char *)bytes, content);
    hl_unpin_entry(hl, t, e);
    return 1;
}
/* bytes constant :bytes() — owned copy of the exact bytes, verbatim (#162). */
static int l_res_const_bytes(lua_State *L) {
    hl_resource_const_t *c = luaL_checkudata(L, 1, HL_RESOURCE_BYTES_CONST_MT);
    blyt_hostlua_t *hl = hl_from(L);
    blyt_resource_table_t *t = hl_active_resources(hl);
    blyt_resource_entry_t *e = hl_resolve(t, c->id);
    const uint8_t *bytes = NULL;
    if (!hl_pin_entry(hl, t, e, &bytes)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, (const char *)bytes, e->len);
    hl_unpin_entry(hl, t, e);
    return 1;
}
/* Module-level pin/unpin: kind-agnostic raw escape hatch, takes the constant. */
static int l_res_pin(lua_State *L) {
    uint32_t id = (uint32_t)luaL_checkinteger(L, 1);
    blyt_hostlua_t *hl = hl_from(L);
    blyt_resource_table_t *t = hl_active_resources(hl);
    blyt_resource_entry_t *e = hl_resolve(t, id);
    const uint8_t *bytes = NULL;
    if (!hl_pin_entry(hl, t, e, &bytes)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlightuserdata(L, (void *)(uintptr_t)bytes);
    lua_pushinteger(L, (lua_Integer)e->len);
    return 2;
}
static int l_res_unpin(lua_State *L) {
    uint32_t id = (uint32_t)luaL_checkinteger(L, 1);
    blyt_hostlua_t *hl = hl_from(L);
    blyt_resource_table_t *t = hl_active_resources(hl);
    blyt_resource_entry_t *e = hl_resolve(t, id);
    if (e)
        hl_unpin_entry(hl, t, e);
    return 0;
}

static void hl_register_const_mt(lua_State *L, const char *mt_name, lua_CFunction accessor,
                                 const char *accessor_name) {
    luaL_newmetatable(L, mt_name);
    lua_pushcfunction(L, l_res_const_eq);
    lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, l_res_const_tostring);
    lua_setfield(L, -2, "__tostring");
    lua_newtable(L);
    lua_pushcfunction(L, accessor);
    lua_setfield(L, -2, accessor_name);
    lua_pushcfunction(L, l_res_const_id);
    lua_setfield(L, -2, "id");
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1); /* pop mt */
}

/* Register blyt.resource.* + blyt32.resource.* — mirror of the guest
 * register_resource_module / the WASM leg.  Call after blyt/blyt32 exist. */
static void register_resource_api(lua_State *L) {
    hl_register_const_mt(L, HL_RESOURCE_TEXT_CONST_MT, l_res_const_text, "text");
    hl_register_const_mt(L, HL_RESOURCE_BYTES_CONST_MT, l_res_const_bytes, "bytes");

    luaL_newmetatable(L, HL_RESOURCE_PALETTE_CONST_MT); /* :id()/__eq shared, no bytes (#214) */
    lua_pushcfunction(L, l_res_const_eq);
    lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, l_res_palette_tostring);
    lua_setfield(L, -2, "__tostring");
    lua_newtable(L);
    lua_pushcfunction(L, l_res_const_id);
    lua_setfield(L, -2, "id");
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    lua_newtable(L); /* resource module */
    lua_pushcfunction(L, l_res_text_resource);
    lua_setfield(L, -2, "text_resource");
    lua_pushcfunction(L, l_res_bytes_resource);
    lua_setfield(L, -2, "bytes_resource");
    lua_pushcfunction(L, l_res_palette_resource);
    lua_setfield(L, -2, "palette");
    lua_pushcfunction(L, l_res_pin);
    lua_setfield(L, -2, "pin");
    lua_pushcfunction(L, l_res_unpin);
    lua_setfield(L, -2, "unpin");

    lua_getglobal(L, "blyt");
    lua_pushvalue(L, -2);
    lua_setfield(L, -2, "resource");
    lua_pop(L, 1);
    lua_getglobal(L, "blyt32");
    if (lua_istable(L, -1)) {
        lua_pushvalue(L, -2);
        lua_setfield(L, -2, "resource");
    }
    lua_pop(L, 1);
    lua_pop(L, 1); /* pop resource module */
}

/* blyt32.mem.stats() (ADR-0029, #159): reads the accounting block + resource
 * table directly (no ECALL).  Byte-for-byte behaviourally the WASM/guest path;
 * see the determinism-vs-advisory contract on blyt_mem_stats in blyt.h. */
static int l_mem_stats(lua_State *L) {
    blyt_hostlua_t *hl = hl_from(L);
    blyt_resource_table_t *t = hl_active_resources(hl);
    uint32_t cache_used = t ? blyt_resource_table_resident_decompressed(t) : 0u;
    uint32_t heap_used = blyt_mem_cart_heap(&hl->mem_acct);

    lua_createtable(L, 0, 5);
    lua_pushinteger(L, (lua_Integer)cache_used);
    lua_setfield(L, -2, "resource_cache_used");
    lua_pushinteger(L, (lua_Integer)heap_used);
    lua_setfield(L, -2, "cart_allocations");
    lua_pushinteger(L, (lua_Integer)(heap_used + cache_used));
    lua_setfield(L, -2, "total_used");
    lua_pushinteger(L, (lua_Integer)BLYT_MEM_BUDGET_BYTES);
    lua_setfield(L, -2, "budget_cap");

    lua_newtable(L); /* resources_loaded */
    uint32_t shown = 0;
    if (t) {
        for (size_t i = 0; i < t->count; i++) {
            const blyt_resource_entry_t *e = &t->entries[i];
            if (!(e->persistent || e->owned != NULL ||
                  (e->algo == BLYT_RES_ALGO_NONE && e->last_access > 0)))
                continue;
            lua_createtable(L, 0, 2);
            lua_pushinteger(L, (lua_Integer)blyt_resource_encode(e->id, BLYT_RESOURCE_PROV_CART));
            lua_setfield(L, -2, "id");
            lua_pushinteger(L, (lua_Integer)(uint32_t)e->len);
            lua_setfield(L, -2, "size");
            lua_rawseti(L, -2, (lua_Integer)(++shown));
        }
    }
    lua_setfield(L, -2, "resources_loaded");
    return 1;
}

static void register_mem_api(lua_State *L) {
    lua_newtable(L); /* mem module */
    lua_pushcfunction(L, l_mem_stats);
    lua_setfield(L, -2, "stats");
    lua_getglobal(L, "blyt");
    lua_pushvalue(L, -2);
    lua_setfield(L, -2, "mem");
    lua_pop(L, 1);
    lua_getglobal(L, "blyt32");
    if (lua_istable(L, -1)) {
        lua_pushvalue(L, -2);
        lua_setfield(L, -2, "mem");
    }
    lua_pop(L, 1);
    lua_pop(L, 1); /* pop mem module */
}

/* Call a global lifecycle function `name` if it is defined.  Returns 0 when the
 * callback ran cleanly (or is undefined) and -1 when it raised a Lua error (the
 * message is logged). */
static int call_lifecycle(blyt_hostlua_t *hl, const char *name) {
    lua_State *L = hl->L;
    lua_getglobal(L, name);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return 0;
    }
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        if (hl->log_fn) {
            char buf[512];
            snprintf(buf, sizeof(buf), "blyt-hostlua: error in %s(): %s", name,
                     msg ? msg : "(no message)");
            hl->log_fn(buf);
        } else {
            fprintf(stderr, "blyt-hostlua: error in %s(): %s\n", name, msg ? msg : "(no message)");
        }
        lua_pop(L, 1);
        return -1;
    }
    return 0;
}

bool blyt_hostlua_available(void) {
    return true;
}

/* Build a fresh Lua VM into hl->L: create the state, stash the runner, open the
 * restricted stdlib + blyt/blyt32 API, re-register the state API + S proxy
 * against the (already-initialised) hl->state_ctx, and load the cart bytecode.
 * Does NOT run lifecycle callbacks — the caller drives init()/on_new_state().
 * Used both at create() and to rebuild the VM each --reset-every-frame cycle
 * (mirroring the WASM leg's wasm_lua_rebuild).  Returns 0 on success (hl->L
 * live), -1 on failure (hl->L closed and NULLed, error logged). */
/* ── Hybrid bridge: Lua half (this host VM) → native half (rv32 session) ──────
 *
 * The native counterpart of the WASM leg's trampoline machinery (wasm_main.c).
 * A .lua_exports cart's native functions are installed as Lua closures on this
 * VM; calling one drives the emulated native half via the frontend-agnostic
 * session API (begin_fn_call / begin_bridged_call / run_frame).  Two kinds:
 *   - typed (≤4 scalar args, no bridge ECALLs): marshal Lua args into rv32
 *     registers, run the native fn to completion, read back the scalar return.
 *   - bridged (ADR-0130 raw exports): move the Lua args onto the exchange thread,
 *     invoke the guest wrapper (which reads/pushes its own Lua values through
 *     BLYT_ECALL_LUA_OP against the exchange thread), move the results back.
 * Unlike the WASM leg there is no lua_yieldk/GDB-coroutine path: native debug
 * pauses by blocking a thread (#234), so the drive loop is a plain while.
 * ──────────────────────────────────────────────────────────────────────────── */

/* Lua scalar type codes in a .lua_exports entry (blyt.h BLYT_LUA_TYPE_*). */
#define HL_LUA_TYPE_VOID 0
#define HL_LUA_TYPE_I32 1
#define HL_LUA_TYPE_U32 2
#define HL_LUA_TYPE_F32 3
#define HL_LUA_TYPE_BOOL 4

static uint32_t hl_lua_to_rv32(lua_State *L, int idx, int type) {
    switch (type) {
    case HL_LUA_TYPE_I32:
        return (uint32_t)(int32_t)lua_tointeger(L, idx);
    case HL_LUA_TYPE_U32:
        return (uint32_t)lua_tointeger(L, idx);
    case HL_LUA_TYPE_F32: {
        float f = (float)lua_tonumber(L, idx);
        uint32_t bits;
        memcpy(&bits, &f, 4);
        return bits;
    }
    case HL_LUA_TYPE_BOOL:
        return lua_toboolean(L, idx) ? 1u : 0u;
    default:
        return 0u;
    }
}

static void hl_rv32_to_lua(lua_State *L, uint32_t val, int type) {
    switch (type) {
    case HL_LUA_TYPE_I32:
        lua_pushinteger(L, (lua_Integer)(int32_t)val);
        break;
    case HL_LUA_TYPE_U32:
        lua_pushinteger(L, (lua_Integer)(uint32_t)val);
        break;
    case HL_LUA_TYPE_F32: {
        float f;
        memcpy(&f, &val, 4);
        lua_pushnumber(L, (lua_Number)f);
        break;
    }
    case HL_LUA_TYPE_BOOL:
        lua_pushboolean(L, val ? 1 : 0);
        break;
    default:
        break; /* VOID: push nothing */
    }
}

/* Drive the emulated native half until the in-flight call completes, then push
 * its scalar return.  A native blyt_quit() during the call latches hl->quit
 * (the host-Lua mirror of the WASM leg's g_lua_quit propagation). */
static int hl_run_trampoline_loop(lua_State *L, blyt_hostlua_t *hl, int ret_type) {
    blyt_cart_run_err_t ferr;
    do {
        ferr = blyt_session_run_frame(hl->session);
    } while (ferr != BLYT_RUN_FN_DONE && ferr != BLYT_RUN_FN_ERROR &&
             ferr != BLYT_RUN_ERR_ECALL_TRAP && ferr != BLYT_RUN_ERR_ABORT);
    if (ferr != BLYT_RUN_FN_DONE)
        return luaL_error(L, "blyt hybrid: native call failed");
    uint32_t ret_val = blyt_session_fn_return_value(hl->session);
    if (!hl->quit && blyt_session_check_guest_quit(hl->session))
        hl->quit = 1;
    hl_rv32_to_lua(L, ret_val, ret_type);
    return (ret_type == HL_LUA_TYPE_VOID) ? 0 : 1;
}

/* Typed export closure.  Upvalues: [1]=fn_addr [2]=nargs [3..6]=arg_types [7]=ret_type. */
static int hl_typed_trampoline(lua_State *L) {
    blyt_hostlua_t *hl = hl_from(L);
    uint32_t fn_addr = (uint32_t)(uintptr_t)lua_touserdata(L, lua_upvalueindex(1));
    int nargs = (int)lua_tointeger(L, lua_upvalueindex(2));
    uint32_t args[4] = {0};
    for (int i = 0; i < nargs && i < 4; i++)
        args[i] = hl_lua_to_rv32(L, i + 1, (int)lua_tointeger(L, lua_upvalueindex(3 + i)));
    blyt_session_begin_fn_call(hl->session, fn_addr, nargs, args);
    int ret_type = (int)lua_tointeger(L, lua_upvalueindex(7));
    return hl_run_trampoline_loop(L, hl, ret_type);
}

/* Bridged (ADR-0130 raw) export closure.  Upvalue: [1]=wrap_addr. */
static int hl_bridged_trampoline(lua_State *L) {
    blyt_hostlua_t *hl = hl_from(L);
    uint32_t wrap_addr = (uint32_t)(uintptr_t)lua_touserdata(L, lua_upvalueindex(1));
    int n = lua_gettop(L);
    if (!lua_checkstack(hl->lua_exch, n + 2))
        return luaL_error(L, "blyt hybrid: exchange stack overflow");
    lua_settop(hl->lua_exch, 0); /* defensive: previous call always cleans up */
    lua_xmove(L, hl->lua_exch, n); /* wrapper sees args at exch indices 1..n */
    if (blyt_session_begin_bridged_call(hl->session, wrap_addr) != 0)
        return luaL_error(L, "blyt hybrid: bridged call setup failed");
    blyt_cart_run_err_t ferr;
    do {
        ferr = blyt_session_run_frame(hl->session);
    } while (ferr != BLYT_RUN_FN_DONE && ferr != BLYT_RUN_FN_ERROR &&
             ferr != BLYT_RUN_ERR_ECALL_TRAP && ferr != BLYT_RUN_ERR_ABORT);
    if (ferr == BLYT_RUN_FN_ERROR) {
        /* The wrapper raised a Lua error; guest registers were restored.  Re-raise
         * inside this Lua call so a script-level pcall catches it (ADR-0130). */
        if (lua_gettop(hl->lua_exch) < 1)
            lua_pushstring(hl->lua_exch, "blyt hybrid: unknown error");
        lua_xmove(hl->lua_exch, L, 1);
        lua_settop(hl->lua_exch, 0);
        return lua_error(L);
    }
    if (ferr != BLYT_RUN_FN_DONE) {
        lua_settop(hl->lua_exch, 0);
        return luaL_error(L, "blyt hybrid: bridged call failed");
    }
    int m = (int)blyt_session_fn_return_value(hl->session); /* a0 = wrapper result count */
    if (!hl->quit && blyt_session_check_guest_quit(hl->session))
        hl->quit = 1;
    int avail = lua_gettop(hl->lua_exch);
    if (m < 0 || m > avail)
        m = 0;
    luaL_checkstack(L, m + 1, "bridged results");
    lua_xmove(hl->lua_exch, L, m);
    lua_settop(hl->lua_exch, 0);
    return m;
}

/* Install one closure per .lua_exports entry as a Lua global, or as module.fn for
 * a dotted lua_name.  Mirrors wasm_visit_export_cb byte-for-byte. */
static void hl_visit_export_cb(const char *lua_name, uint32_t fn_guest_addr,
                               uint32_t wrap_guest_addr, uint8_t flags, uint8_t nargs,
                               const uint8_t arg_types[4], uint8_t ret_type, void *userdata) {
    lua_State *L = (lua_State *)userdata;
    if (flags & BLYT_LUA_EXPORT_FLAG_BRIDGED) {
        lua_pushlightuserdata(L, (void *)(uintptr_t)wrap_guest_addr);
        lua_pushcclosure(L, hl_bridged_trampoline, 1);
    } else {
        lua_pushlightuserdata(L, (void *)(uintptr_t)fn_guest_addr);
        lua_pushinteger(L, nargs);
        for (int j = 0; j < 4; j++)
            lua_pushinteger(L, arg_types[j]);
        lua_pushinteger(L, ret_type);
        lua_pushcclosure(L, hl_typed_trampoline, 7);
    }

    const char *dot = strchr(lua_name, '.');
    if (!dot) {
        lua_setglobal(L, lua_name);
    } else {
        char mod[32];
        int mod_len = (int)(dot - lua_name);
        if (mod_len >= (int)sizeof(mod))
            mod_len = (int)sizeof(mod) - 1;
        for (int i = 0; i < mod_len; i++)
            mod[i] = lua_name[i];
        mod[mod_len] = '\0';
        const char *fn = dot + 1;

        lua_getglobal(L, mod);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_pushvalue(L, -1);
            lua_setglobal(L, mod);
            luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
            lua_pushvalue(L, -2);
            lua_setfield(L, -2, mod);
            lua_pop(L, 1); /* pop _LOADED */
        }
        lua_pushvalue(L, -2);
        lua_setfield(L, -2, fn);
        lua_pop(L, 2); /* pop module table + original closure */
    }
}

/* Wire the hybrid bridge onto a freshly built VM (called from build_vm before the
 * bytecode runs, so require()'d native modules and called globals already exist).
 * Creates the registry-anchored exchange thread, attaches it to the session, and
 * installs the export trampolines.  No-op for a pure-Lua runner (no session). */
static void hl_wire_hybrid(blyt_hostlua_t *hl) {
    if (!hl->session)
        return;
    hl->lua_exch = lua_newthread(hl->L);
    hl->lua_exch_ref = luaL_ref(hl->L, LUA_REGISTRYINDEX);
    blyt_session_lua_bridge_attach(hl->session, hl->lua_exch);
    blyt_session_visit_lua_exports(hl->session, hl_visit_export_cb, hl->L);
}

/* Install a cart-native lifecycle fn `fn_guest_addr` as the zero-arg void Lua
 * global `name` so call_lifecycle picks it up like a Lua-defined callback.  The
 * native counterpart of wasm_main.c's maybe_inject_lifecycle_cb — a plain typed
 * trampoline with nargs=0 and VOID arg/return types. */
static void hl_inject_lifecycle_cb(lua_State *L, const char *name, uint32_t fn_guest_addr) {
    lua_pushlightuserdata(L, (void *)(uintptr_t)fn_guest_addr);
    lua_pushinteger(L, 0); /* nargs = 0 */
    lua_pushinteger(L, HL_LUA_TYPE_VOID); /* arg_types[0..3] */
    lua_pushinteger(L, HL_LUA_TYPE_VOID);
    lua_pushinteger(L, HL_LUA_TYPE_VOID);
    lua_pushinteger(L, HL_LUA_TYPE_VOID);
    lua_pushinteger(L, HL_LUA_TYPE_VOID); /* ret_type */
    lua_pushcclosure(L, hl_typed_trampoline, 7);
    lua_setglobal(L, name);
}

/* Inject a hybrid cart's native lifecycle callbacks as Lua-global trampolines
 * (#232 S4).  Called from build_vm AFTER the cart bytecode has run, so the Lua
 * halves' globals already exist and a conflict (a callback defined in BOTH the
 * native and Lua halves) can be detected.  Returns 0 on success, -1 on conflict
 * (error logged; the caller closes the VM).  No-op for a pure-Lua runner. */
static int hl_inject_native_lifecycle(blyt_hostlua_t *hl) {
    if (!hl->session)
        return 0;
    static const struct {
        const char *name;
        uint32_t (*fn)(blyt_session_t *);
    } cbs[] = {
        {"init", blyt_session_cart_fn_init},
        {"on_new_state", blyt_session_cart_fn_on_new_state},
        {"update", blyt_session_cart_fn_update},
        {"draw", blyt_session_cart_fn_draw},
        {"on_quit", blyt_session_cart_fn_on_quit},
        {"cleanup", blyt_session_cart_fn_cleanup},
    };
    for (size_t i = 0; i < sizeof(cbs) / sizeof(cbs[0]); i++) {
        uint32_t fn = cbs[i].fn(hl->session);
        if (!fn)
            continue; /* callback lives in a runtime stub, not the cart itself. */
        lua_getglobal(hl->L, cbs[i].name);
        int has_lua = lua_isfunction(hl->L, -1);
        lua_pop(hl->L, 1);
        if (has_lua) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "blyt-hostlua: lifecycle '%s' defined in both native and Lua",
                     cbs[i].name);
            if (hl->log_fn)
                hl->log_fn(buf);
            else
                fprintf(stderr, "%s\n", buf);
            return -1;
        }
        hl_inject_lifecycle_cb(hl->L, cbs[i].name, fn);
    }
    return 0;
}

static int build_vm(blyt_hostlua_t *hl) {
    /* lua_newstate with the arena-backed allocator (#158) + the seam VM's pinned
     * hash seed (luaL_makeseed(NULL) == luai_makeseed override 0x424C5954) — the
     * SAME seed luaL_newstate uses, kept for determinism (mirrors the WASM leg). */
    hl->L = lua_newstate(hl_lua_alloc, hl, luaL_makeseed(NULL));
    if (!hl->L)
        return -1;
    *(blyt_hostlua_t **)lua_getextraspace(hl->L) = hl;

    open_libs(hl->L);
    register_blyt_api(hl->L);
    register_gfx_api(hl->L); /* blyt32.gfx.* (#231) */
    register_surface_api(hl->L); /* blyt32.surface.* + lock mt (#231) */
    register_resource_api(hl->L); /* blyt.resource.* + typed consts (#231) */
    register_mem_api(hl->L); /* blyt32.mem.stats (#231) */

    /* State API + S proxy (before bytecode/init so on_new_state can alloc slots).
     * The ctx (standalone for pure-Lua, the session's for a hybrid #232) is
     * initialised once and persists across rebuilds so its buffers survive the
     * snapshot/restore cycle. */
    blyt_state_ctx_t *sctx = hl_active_ctx(hl);
    if (sctx) {
        register_state_api(hl->L);
        register_s_proxy(hl->L, sctx);
    }

    /* Hybrid (#232): attach the exchange thread and install the native-export
     * trampolines BEFORE the bytecode runs, so the cart's top-level
     * require("mod") / global calls resolve against the native exports. */
    hl_wire_hybrid(hl);

    if (load_lua_bytecode(hl->L, hl->bytecode, hl->bytecode_size) != 0) {
        const char *msg = lua_tostring(hl->L, -1);
        if (hl->log_fn) {
            char buf[512];
            snprintf(buf, sizeof(buf), "blyt-hostlua: failed to load cart bytecode: %s",
                     msg ? msg : "(no message)");
            hl->log_fn(buf);
        } else {
            fprintf(stderr, "blyt-hostlua: failed to load cart bytecode: %s\n",
                    msg ? msg : "(no message)");
        }
        lua_close(hl->L);
        hl->L = NULL;
        return -1;
    }

    /* Hybrid native-lifecycle injection (#232 S4): install the cart's own native
     * lifecycle callbacks (blyt_cart_update etc.) as Lua-global trampolines now
     * that the bytecode has run and the Lua halves' globals exist, so a callback
     * defined in both halves is caught.  Before the baseline so these scaffolding
     * closures are excluded from the cart-attributable heap. */
    if (hl_inject_native_lifecycle(hl) != 0) {
        lua_close(hl->L);
        hl->L = NULL;
        return -1;
    }

    /* Capture the runtime-scaffolding baseline (#231): everything allocated up to
     * here — the VM, stdlibs, blyt/blyt32 API, S proxy, and the loaded cart
     * bytecode — is runtime overhead, identical for a given cart but different in
     * amount from the wasm leg (which additionally builds driver coroutines) and
     * the emulated leg. Excluding it makes guest_heap_used / cart_allocations and
     * the 16 MB fail-point count only the cart's own runtime allocations, so they
     * are byte-identical across legs. Re-captured on every (re)build.
     *
     * Collect first (#231): the baseline must be the *settled* scaffolding
     * footprint, not a snapshot that still holds this leg's build-time transient
     * garbage — the amount of that garbage differs between legs (native builds the
     * VM from C, wasm additionally builds driver coroutines), and any uncollected
     * remainder biases the subtraction. A full collection here pins the baseline
     * to the reachable scaffolding on every leg, so guest_heap_used − baseline is
     * the byte-identical cart-attributable heap. */
    lua_gc(hl->L, LUA_GCCOLLECT);
    hl->mem_acct.guest_heap_baseline = hl->mem_acct.guest_heap_used;

#ifdef BLYT_DAP
    /* Arm the DAP master hook on the fresh VM so breakpoints/steps fire in the
     * lifecycle callbacks (#234).  Re-run on every rebuild (reset/reload), the
     * native equivalent of the WASM leg re-installing the hook per coroutine. */
    if (hl->dap_enabled) {
        fc_master_hook_cfg.dap_enabled = true;
        fc_consolelua_master_hook_install(hl->L);
    }
#endif
    return 0;
}

/* Allocate a runner and build its VM (resources + palette + state ctx + VM),
 * but do NOT run the init() boot — the caller decides when (immediately for a
 * plain run, or after configurationDone for a debug session).  `debug` arms the
 * DAP master hook inside build_vm.  Returns the runner (VM live, uninitialised)
 * or NULL on failure. */
static blyt_hostlua_t *hl_new(blyt_cart_t *cart, blyt_log_fn log_fn, bool debug) {
    if (!cart)
        return NULL;

    size_t lua_size = 0;
    const void *bytecode = blyt_cart_find_section(cart, ".cart.lua", &lua_size);
    if (!bytecode || !lua_size)
        return NULL;

    blyt_hostlua_t *hl = calloc(1, sizeof(*hl));
    if (!hl)
        return NULL;
    hl->log_fn = log_fn;
    hl->cart = cart;
    hl->bytecode = (const unsigned char *)bytecode;
    hl->bytecode_size = lua_size;
    hl->dap_enabled = debug; /* build_vm arms the master hook when set (#234) */
    hl->lua_exch_ref = LUA_NOREF;

    /* Resource table (#93/#158/#231): load the cart's bundled + persistent
     * resources so blyt.resource.*, mem.stats, and cart-asset palettes resolve
     * (session-less mirror of the WASM leg).  Must precede the palette seed below,
     * which may resolve a cart-provenance declared default (#214). */
    blyt_resource_table_init(&hl->resources);
    blyt_resource_table_load_for_cart(&hl->resources, cart);
    blyt_resource_table_load_persistent_from_cart(&hl->resources, cart);
    if (blyt_resource_table_preload_persistent(&hl->resources) != 0) {
        if (log_fn)
            log_fn("blyt-hostlua: persistent resource preload failed");
        blyt_resource_table_clear(&hl->resources);
        free(hl);
        return NULL;
    }
    hl->resources_loaded = true;
    hl_publish_footprint(hl, &hl->resources);

    hl_palette_ensure_default(hl); /* built-in or cart-asset declared default (#214) */

    /* Hybrid (#232): a cart has a native C/Rust half if it exports Lua-callable
     * functions (.lua_exports) OR defines native lifecycle callbacks
     * (blyt_cart_update etc. in its own code — #232 S4).  Create the rv32 session
     * that runs it — in bridge mode, so it links the bridge stub
     * (libblyt32lua-bridge.so) and its Lua C API calls trap to this host VM.  The
     * session persists across VM rebuilds; build_vm re-attaches the exchange
     * thread, re-installs the export trampolines, and re-injects the native
     * lifecycle globals each rebuild. */
    if (blyt_cart_find_section(cart, ".lua_exports", NULL) ||
        blyt_cart_has_native_lifecycle(cart)) {
        hl->session = blyt_session_create_lua_bridge(cart, log_fn);
        if (!hl->session) {
            if (log_fn)
                log_fn("blyt-hostlua: hybrid session create failed");
            if (hl->resources_loaded)
                blyt_resource_table_clear(&hl->resources);
            free(hl);
            return NULL;
        }
    }

    /* State buffers: a pure-Lua cart with .cart.layouts gets a standalone ctx
     * (no session/emulator).  Initialised once here; the VM built below registers
     * the blyt.buf.* + save API and the generated S proxy against it.  A hybrid
     * cart instead shares its session's state ctx (wired in #232 S3), so it does
     * not build a standalone one here. */
    if (!hl->session && blyt_cart_has_layouts(cart)) {
        hl->state_ctx = malloc(sizeof(*hl->state_ctx));
        if (!hl->state_ctx || blyt_state_ctx_init(cart, hl->state_ctx) < 0) {
            if (log_fn)
                log_fn("blyt-hostlua: state ctx init failed");
            else
                fprintf(stderr, "blyt-hostlua: state ctx init failed\n");
            free(hl->state_ctx);
            free(hl);
            return NULL;
        }
        const char *save_dir = getenv("BLYT_SAVE_DIR");
        if (save_dir)
            hl->save_dir = strdup(save_dir);
        /* The manifest id names the save subdirectory (validated ≤63 bytes). */
        snprintf(hl->cart_name, sizeof(hl->cart_name), "%s", blyt_cart_id(cart));
    }

    if (build_vm(hl) != 0) {
        if (hl->session)
            blyt_session_destroy(hl->session);
        if (hl->state_ctx) {
            blyt_state_ctx_destroy(hl->state_ctx);
            free(hl->state_ctx);
        }
        if (hl->resources_loaded)
            blyt_resource_table_clear(&hl->resources);
        free(hl->save_dir);
        free(hl);
        return NULL;
    }
    return hl;
}

blyt_hostlua_t *blyt_hostlua_create(blyt_cart_t *cart, blyt_log_fn log_fn) {
    blyt_hostlua_t *hl = hl_new(cart, log_fn, false);
    if (!hl)
        return NULL;

    /* Boot phase of the guest blyt_main loop: init() then on_new_state(). */
    if (call_lifecycle(hl, "init") != 0 || call_lifecycle(hl, "on_new_state") != 0) {
        blyt_hostlua_destroy(hl);
        return NULL;
    }
    hl->booted = true;
    return hl;
}

blyt_hostlua_t *blyt_hostlua_create_debug(blyt_cart_t *cart, blyt_log_fn log_fn) {
#ifdef BLYT_DAP
    /* Build + arm the hook, but DEFER init() to blyt_hostlua_dap_wait_ready() so
     * a breakpoint set in init() fires (the native equivalent of the WASM leg
     * gating init on configurationDone). */
    return hl_new(cart, log_fn, true);
#else
    (void)cart;
    (void)log_fn;
    return NULL;
#endif
}

int blyt_hostlua_dap_listen(blyt_hostlua_t *hl, int *actual_port) {
#ifdef BLYT_DAP
    if (!hl || !hl->dap_enabled)
        return -1;
    int p = fc_hostlua_dap_listen(0); /* OS-assigned, mirroring the emulated path */
    if (p < 0)
        return -1;
    if (actual_port)
        *actual_port = p;
    return 0;
#else
    (void)hl;
    (void)actual_port;
    return -1;
#endif
}

int blyt_hostlua_dap_wait_ready(blyt_hostlua_t *hl) {
#ifdef BLYT_DAP
    if (!hl || !hl->dap_enabled)
        return 0;
    if (!fc_hostlua_dap_wait_ready()) /* blocks until configurationDone / shutdown */
        return 0;
    /* Run the deferred boot under the armed hook so an init() breakpoint pauses. */
    if (!hl->booted) {
        call_lifecycle(hl, "init");
        call_lifecycle(hl, "on_new_state");
        hl->booted = true;
    }
    return 1;
#else
    (void)hl;
    return 0;
#endif
}

/* Shared tail of the reset-every-frame cycle and the hot reload (#244): tear the
 * VM down, rebuild it from hl->bytecode (re-arming the DAP master hook), re-run
 * init(), then restore `snap` over the fresh buffers and replay
 * on_load_state(HOT_RELOAD).  Consumes `snap` (freed here).  On rebuild failure
 * the runner is marked done and false is returned; true on success. */
static bool hl_rebuild_and_restore(blyt_hostlua_t *hl, blyt_state_snapshot_t *snap) {
    /* Tear down the VM and rebuild it (all Lua globals wiped — the host-Lua
     * equivalent of zeroing guest BSS; build_vm re-arms the DAP hook, #234). */
    lua_close(hl->L);
    hl->L = NULL;
    /* Empty the cart heap so the reloaded VM's allocations are bit-identical to a
     * first load (mirrors the WASM leg's wasm_lua_arena_reset).  lua_close already
     * frees the old VM's objects through the allocator; this zeroes any residual
     * accounting so guest_heap_used restarts from 0. */
    if (hl->arena.base)
        blyt_arena_reset(&hl->arena);
    if (build_vm(hl) != 0) {
        /* Rebuild failed: the runner is unusable; mark done so run_frame stops. */
        if (snap)
            blyt_state_snapshot_free(snap);
        hl->done = true;
        return false;
    }

    /* Re-run init() on the fresh VM (under the re-armed hook, an init()
     * breakpoint pauses here — the reload-while-debug re-fire, #244). */
    call_lifecycle(hl, "init");

    /* Restore state buffers + notify the cart (BLYT_LOAD_HOT_RELOAD = 3). */
    if (snap) {
        blyt_state_ctx_restore_snapshot(hl_active_ctx(hl), snap);
        blyt_state_snapshot_free(snap);
    }
    lua_getglobal(hl->L, "on_load_state");
    if (lua_isfunction(hl->L, -1)) {
        lua_newtable(hl->L);
        lua_pushinteger(hl->L, 3); /* BLYT_LOAD_HOT_RELOAD */
        lua_setfield(hl->L, -2, "reason");
        lua_pushinteger(hl->L, 0);
        lua_setfield(hl->L, -2, "saved_cart_version");
        lua_pcall(hl->L, 1, 0, 0);
    } else {
        lua_pop(hl->L, 1);
    }
    return true;
}

void blyt_hostlua_reset_every_frame_cycle(blyt_hostlua_t *hl) {
    if (!hl || hl->done)
        return;

    /* Mirror the WASM leg's wasm_lua_reset_cycle (full VM rebuild preserving
     * state): flush transient state → snapshot buffers → zero buffers → tear the
     * VM down and rebuild it → init() → restore buffers → on_load_state(HOT_RELOAD).
     * The emulated leg reaches the same observable state by zeroing guest BSS
     * instead of recreating the VM; the cart-visible round-trip is identical, and
     * asserting the same output here as a plain run is the determinism stress. */

    /* 1. Ask the cart to flush any transient state into state buffers. */
    call_lifecycle(hl, "on_save_state");

    /* 2. Snapshot + 3. zero state buffers (no-ops when the cart has no buffers,
     * where the cycle is just a VM rebuild). */
    blyt_state_snapshot_t *snap = NULL;
    blyt_state_ctx_t *sctx = hl_active_ctx(hl);
    if (sctx) {
        snap = blyt_state_ctx_snapshot(sctx);
        blyt_state_ctx_zero_data(sctx);
    }

    /* 4–6. Rebuild from the SAME bytecode, restore, notify. */
    hl_rebuild_and_restore(hl, snap);
}

bool blyt_hostlua_reload(blyt_hostlua_t *hl, blyt_cart_t *new_cart) {
    if (!hl || hl->done || !new_cart)
        return false;

    /* Validate the new image BEFORE disturbing the live VM: a cart without a
     * .cart.lua section can't run on this path, so keep the old VM running and
     * let the caller keep the old cart (mirrors reload_impl's pre-swap open). */
    size_t lua_size = 0;
    const void *bytecode = blyt_cart_find_section(new_cart, ".cart.lua", &lua_size);
    if (!bytecode || !lua_size)
        return false;

    /* Snapshot live state from the CURRENT VM first (on_save_state runs in the
     * old VM, before the swap) — the same order as the reset cycle and the WASM
     * pure-Lua reload. */
    call_lifecycle(hl, "on_save_state");
    blyt_state_snapshot_t *snap = NULL;
    blyt_state_ctx_t *sctx = hl_active_ctx(hl);
    if (sctx) {
        snap = blyt_state_ctx_snapshot(sctx);
        blyt_state_ctx_zero_data(sctx);
    }

    /* Swap the cart image into the runner.  Resource-table entries alias the cart
     * map zero-copy (resource.c), so the table must be re-pointed at new_cart now,
     * while the old cart is still valid — the caller closes the old cart only
     * after we return.  Re-seed the default palette from the new cart's assets
     * too (the new init() may still override it). */
    if (hl->resources_loaded) {
        blyt_resource_table_clear(&hl->resources);
        blyt_resource_table_load_for_cart(&hl->resources, new_cart);
        blyt_resource_table_load_persistent_from_cart(&hl->resources, new_cart);
        if (blyt_resource_table_preload_persistent(&hl->resources) != 0 && hl->log_fn)
            hl->log_fn("blyt-hostlua: reload persistent resource preload failed");
        hl_publish_footprint(hl, &hl->resources);
    }
    hl->cart = new_cart;
    hl->bytecode = (const unsigned char *)bytecode;
    hl->bytecode_size = lua_size;
    hl_palette_ensure_default(hl);

    /* Rebuild the VM from the NEW bytecode, restore, notify (HOT_RELOAD). */
    return hl_rebuild_and_restore(hl, snap);
}

blyt_cart_run_err_t blyt_hostlua_run_frame(blyt_hostlua_t *hl) {
    if (!hl)
        return BLYT_RUN_ERR_EMU;
    if (hl->done)
        return BLYT_RUN_OK;

    /* Quit is tested at the top of the call, mirroring blyt_main's
     * `while (!g_quit_requested)`: a quit requested during a prior update() still
     * ran that frame's draw(); the exit runs on_quit() + cleanup() once. */
    if (hl->quit) {
        call_lifecycle(hl, "on_quit");
        call_lifecycle(hl, "cleanup");
        hl->done = true;
        return BLYT_RUN_OK;
    }

    /* Frame entry (mirrors blyt_session_run_frame): bump the tier-2 lock epoch so
     * any lock from the previous frame is now stale, and reap the previous
     * frame's draw-scoped off-screen surfaces. */
    hl->lock_epoch++;
    hl_surf_reap(hl);

    /* Phase brackets (#205, #232 S4): a hybrid cart's emulated native half reads
     * the phase off the session run-ctx to gate surface access to draw().  The
     * native counterpart of the WASM leg's __blyt_phase_* globals; a no-op for a
     * session-less pure-Lua runner (blyt_session_set_phase(NULL)). */
    blyt_session_set_phase(hl->session, BLYT_PHASE_UPDATE);
    if (call_lifecycle(hl, "update") != 0) {
        hl->done = true;
        return BLYT_RUN_ERR_ABORT;
    }
    blyt_session_set_phase(hl->session, BLYT_PHASE_DRAW);
    if (call_lifecycle(hl, "draw") != 0) {
        hl->done = true;
        return BLYT_RUN_ERR_ABORT;
    }
    blyt_session_set_phase(hl->session, BLYT_PHASE_NONE);

    /* A native lifecycle callback may have latched a quit through the trampoline
     * loop; also poll the session directly so a quit requested by the emulated
     * half ends the loop on the next frame (mirrors blyt_main's post-frame check). */
    if (hl->session && !hl->quit && blyt_session_check_guest_quit(hl->session))
        hl->quit = 1;

    hl_frame_done(hl);
    return BLYT_RUN_FRAME_DONE;
}

/* Read-only accessors the frontend uses to present the host-Lua framebuffer
 * (there is no session, so retro_run cannot go through blyt_session_expand_frame
 * — it expands these directly into its XRGB buffer). */
const uint8_t *blyt_hostlua_get_pixels(blyt_hostlua_t *hl) {
    return hl ? hl->fb : NULL;
}
const uint32_t *blyt_hostlua_get_palette(blyt_hostlua_t *hl) {
    return hl ? hl->palette : NULL;
}

void blyt_hostlua_destroy(blyt_hostlua_t *hl) {
    if (!hl)
        return;
#ifdef BLYT_DAP
    if (hl->dap_enabled)
        fc_hostlua_dap_shutdown();
#endif
    if (hl->L)
        lua_close(hl->L); /* frees the exchange thread with the registry */
    /* Tear down the emulated native half (#232 hybrid). */
    if (hl->session)
        blyt_session_destroy(hl->session);
    /* Free any off-screen surface buffers still resident (slot 0 = screen aliases
     * fb, never freed).  free(NULL) is a no-op for unused slots. */
    for (uint32_t i = 1; i < HL_SURFACE_MAX; i++)
        free(hl->surf[i].pixels);
    free(hl->arena.base); /* the 16 MB cart-heap region (#158) */
    if (hl->resources_loaded)
        blyt_resource_table_clear(&hl->resources);
    if (hl->state_ctx) {
        blyt_state_ctx_destroy(hl->state_ctx);
        free(hl->state_ctx);
    }
    free(hl->save_dir);
    free(hl);
}

/* Opt-in dispatch predicate.  Routes to the native host-Lua path when the cart
 * has a .cart.lua section.  A hybrid cart IS supported here via the ADR-0130
 * ECALL bridge (#232): the Lua half runs on the host VM, the native half stays
 * emulated under rv32emu — for both `.lua_exports` (typed/bridged native exports
 * the Lua half calls) and native lifecycle callbacks (`blyt_cart_update` etc.,
 * injected as zero-arg Lua-global trampolines — #232 S4). */
bool blyt_hostlua_should_use(const blyt_cart_t *cart) {
    if (!cart)
        return false;
    if (!getenv("BLYT_HOSTLUA"))
        return false;
    if (!blyt_cart_find_section(cart, ".cart.lua", NULL))
        return false;
    return true;
}

#else /* !BLYT_HOSTLUA_EXEC — seam VM absent; the frontend falls back to rv32. */

bool blyt_hostlua_available(void) {
    return false;
}

bool blyt_hostlua_should_use(const blyt_cart_t *cart) {
    (void)cart;
    return false;
}

blyt_hostlua_t *blyt_hostlua_create(blyt_cart_t *cart, blyt_log_fn log_fn) {
    (void)cart;
    (void)log_fn;
    return NULL;
}

blyt_hostlua_t *blyt_hostlua_create_debug(blyt_cart_t *cart, blyt_log_fn log_fn) {
    (void)cart;
    (void)log_fn;
    return NULL;
}

int blyt_hostlua_dap_listen(blyt_hostlua_t *hl, int *actual_port) {
    (void)hl;
    (void)actual_port;
    return -1;
}

int blyt_hostlua_dap_wait_ready(blyt_hostlua_t *hl) {
    (void)hl;
    return 0;
}

blyt_cart_run_err_t blyt_hostlua_run_frame(blyt_hostlua_t *hl) {
    (void)hl;
    return BLYT_RUN_ERR_EMU;
}

const uint8_t *blyt_hostlua_get_pixels(blyt_hostlua_t *hl) {
    (void)hl;
    return NULL;
}

const uint32_t *blyt_hostlua_get_palette(blyt_hostlua_t *hl) {
    (void)hl;
    return NULL;
}

void blyt_hostlua_reset_every_frame_cycle(blyt_hostlua_t *hl) {
    (void)hl;
}

bool blyt_hostlua_reload(blyt_hostlua_t *hl, blyt_cart_t *new_cart) {
    (void)hl;
    (void)new_cart;
    return false;
}

void blyt_hostlua_destroy(blyt_hostlua_t *hl) {
    (void)hl;
}

#endif /* BLYT_HOSTLUA_EXEC */
