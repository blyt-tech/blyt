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

#include "save.h" /* blyt_save_write / blyt_save_read */
#include "state_buffer.h" /* blyt_state_ctx_t + typed accessors */

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
};

/* The runner is stashed in the lua_State's extra space so the C API callbacks
 * can reach its log channel + quit latch without a file-scoped global (unlike
 * the WASM leg's g_lua — a native player could in principle host more than one). */
static blyt_hostlua_t *hl_from(lua_State *L) {
    return *(blyt_hostlua_t **)lua_getextraspace(L);
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

/* Load the cart's .cart.lua: a single raw bytecode chunk, or the BLMC
 * multi-chunk container (issue #54).  Byte-for-byte the WASM leg's loader.
 * Returns 0 on success; on failure leaves an error string on the stack top. */
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
            if (lua_pcall(L, 0, 0, 0) != LUA_OK)
                return 1;
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

static blyt_state_ctx_t *hl_ctx(lua_State *L) {
    blyt_hostlua_t *hl = hl_from(L);
    return hl ? hl->state_ctx : NULL;
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
    if (hl && hl->state_ctx)
        r = blyt_save_write(hl->state_ctx, hl->save_dir, hl->cart_name, slot,
                            blyt_cart_save_version(hl->cart));
    lua_pushinteger(L, r);
    return 1;
}

static int l_save_read(lua_State *L) {
    uint32_t slot = (uint32_t)luaL_checkinteger(L, 1);
    blyt_hostlua_t *hl = hl_from(L);
    int r = -1;
    uint32_t saved_version = 0;
    if (hl && hl->state_ctx)
        r = blyt_save_read(hl->state_ctx, hl->save_dir, hl->cart_name, slot, &saved_version);
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
static int build_vm(blyt_hostlua_t *hl) {
    /* luaL_newstate uses the seam VM's pinned hash seed (luai_makeseed ==
     * 0x424C5954) — the SAME VM construction every other leg uses. */
    hl->L = luaL_newstate();
    if (!hl->L)
        return -1;
    *(blyt_hostlua_t **)lua_getextraspace(hl->L) = hl;

    open_libs(hl->L);
    register_blyt_api(hl->L);

    /* State API + S proxy (before bytecode/init so on_new_state can alloc slots).
     * The ctx itself is initialised once in create() and persists across rebuilds
     * so its buffers survive the snapshot/restore cycle. */
    if (hl->state_ctx) {
        register_state_api(hl->L);
        register_s_proxy(hl->L, hl->state_ctx);
    }

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
    return 0;
}

blyt_hostlua_t *blyt_hostlua_create(blyt_cart_t *cart, blyt_log_fn log_fn) {
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

    /* State buffers: a pure-Lua cart with .cart.layouts gets a standalone ctx
     * (no session/emulator).  Initialised once here; the VM built below registers
     * the blyt.buf.* + save API and the generated S proxy against it. */
    if (blyt_cart_has_layouts(cart)) {
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
        if (hl->state_ctx) {
            blyt_state_ctx_destroy(hl->state_ctx);
            free(hl->state_ctx);
        }
        free(hl->save_dir);
        free(hl);
        return NULL;
    }

    /* Boot phase of the guest blyt_main loop: init() then on_new_state(). */
    if (call_lifecycle(hl, "init") != 0 || call_lifecycle(hl, "on_new_state") != 0) {
        blyt_hostlua_destroy(hl);
        return NULL;
    }

    return hl;
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
    if (hl->state_ctx) {
        snap = blyt_state_ctx_snapshot(hl->state_ctx);
        blyt_state_ctx_zero_data(hl->state_ctx);
    }

    /* 4. Tear down the VM and rebuild it from the same bytecode (all Lua globals
     * wiped — the host-Lua equivalent of zeroing guest BSS). */
    lua_close(hl->L);
    hl->L = NULL;
    if (build_vm(hl) != 0) {
        /* Rebuild failed: the runner is unusable; mark done so run_frame stops. */
        if (snap)
            blyt_state_snapshot_free(snap);
        hl->done = true;
        return;
    }

    /* 5. Re-run init() on the fresh VM. */
    call_lifecycle(hl, "init");

    /* 6. Restore state buffers + notify the cart (BLYT_LOAD_HOT_RELOAD = 3). */
    if (snap) {
        blyt_state_ctx_restore_snapshot(hl->state_ctx, snap);
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

    if (call_lifecycle(hl, "update") != 0 || call_lifecycle(hl, "draw") != 0) {
        hl->done = true;
        return BLYT_RUN_ERR_ABORT;
    }
    return BLYT_RUN_FRAME_DONE;
}

void blyt_hostlua_destroy(blyt_hostlua_t *hl) {
    if (!hl)
        return;
    if (hl->L)
        lua_close(hl->L);
    if (hl->state_ctx) {
        blyt_state_ctx_destroy(hl->state_ctx);
        free(hl->state_ctx);
    }
    free(hl->save_dir);
    free(hl);
}

/* Opt-in dispatch predicate.  Pure-Lua = has .cart.lua, no cart-native lifecycle
 * symbol, and no .lua_exports (typed/bridged exports ⇒ hybrid, stays on rv32). */
bool blyt_hostlua_should_use(const blyt_cart_t *cart) {
    if (!cart)
        return false;
    if (!getenv("BLYT_HOSTLUA"))
        return false;
    if (!blyt_cart_find_section(cart, ".cart.lua", NULL))
        return false;
    if (blyt_cart_has_native_lifecycle(cart))
        return false;
    if (blyt_cart_find_section(cart, ".lua_exports", NULL))
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

blyt_cart_run_err_t blyt_hostlua_run_frame(blyt_hostlua_t *hl) {
    (void)hl;
    return BLYT_RUN_ERR_EMU;
}

void blyt_hostlua_reset_every_frame_cycle(blyt_hostlua_t *hl) {
    (void)hl;
}

void blyt_hostlua_destroy(blyt_hostlua_t *hl) {
    (void)hl;
}

#endif /* BLYT_HOSTLUA_EXEC */
