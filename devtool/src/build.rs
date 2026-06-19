use std::collections::{BTreeMap, BTreeSet};
use std::ffi::OsStr;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

use crate::engine::{BuildError, Task, TaskInput, build_err, parse_depfile, run_tasks};

use crate::cart_info_generated::blyt::{CartInfo, CartInfoArgs};
use crate::cart_layouts_generated::blyt::{
    BufferDecl, BufferDeclArgs, CartLayouts, CartLayoutsArgs, FieldDecl, FieldDeclArgs, RecordDecl,
    RecordDeclArgs,
};
use crate::config::{CartConfig, FlatField, flatten_record};
use flatbuffers::FlatBufferBuilder;

/* -------------------------------------------------------------------------
 * .cart.info section data (ADR-0073, ADR-0129)
 *
 * 8-byte preamble: "CINF" + format_major(u16le=0) + format_minor(u16le=0),
 * followed by a FlatBuffers CartInfo table.  The body is written with the
 * `flatbuffers` crate from schemas/cart_info.fbs; the runtime reads it with the
 * flatcc-generated reader — the wire format is identical for both codegens.
 *
 * `debug` records whether this is a `blyt build --debug` cart (DWARF, unstripped).
 * api_version_major/minor stay 0/0 (validated at load); id/title/version come
 * from blyt.info.yaml (see read_cart_info); author/console are left unset for
 * now — wire them from blyt.info.yaml when that file grows fields.
 * ------------------------------------------------------------------------- */
fn cart_info_bytes(debug: bool, info: &InfoFields) -> Vec<u8> {
    let mut fbb = FlatBufferBuilder::new();
    let id = fbb.create_string(&info.id);
    let title = fbb.create_string(&info.title);
    let version = fbb.create_string(&info.version);
    let info = CartInfo::create(
        &mut fbb,
        &CartInfoArgs {
            api_version_major: 0,
            api_version_minor: 0,
            title: Some(title),
            author: None,
            console: None,
            debug,
            id: Some(id),
            version: Some(version),
        },
    );
    fbb.finish(info, None);
    let body = fbb.finished_data();

    let mut out = Vec::with_capacity(8 + body.len());
    out.extend_from_slice(b"CINF");
    out.extend_from_slice(&0u16.to_le_bytes()); // format_major
    out.extend_from_slice(&0u16.to_le_bytes()); // format_minor
    out.extend_from_slice(body);
    out
}

/* -------------------------------------------------------------------------
 * .cart.layouts section data (ADR-0009, ADR-0073)
 *
 * Serialises CartLayouts FlatBuffer with CLAY preamble.
 * Returns empty Vec when cfg has no state_buffers (no section emitted).
 * ------------------------------------------------------------------------- */
fn cart_layouts_bytes(cfg: &CartConfig) -> Vec<u8> {
    use crate::config::compute_schema_hash;

    if cfg.state_buffers.is_empty() {
        return Vec::new();
    }

    let schema_hash = compute_schema_hash(cfg);
    let mut fbb = FlatBufferBuilder::new();

    /* Build RecordDecl vector (only records referenced by a buffer). */
    let mut record_fbs: Vec<flatbuffers::WIPOffset<RecordDecl<'_>>> = Vec::new();
    for (rec_name, rec) in &cfg.records {
        let name_off = fbb.create_string(rec_name);
        /* Resolve to flat fields */
        let mut visiting = Vec::new();
        let flat = match flatten_record(rec_name, &cfg.records, &mut visiting) {
            Ok(f) => f,
            Err(_) => continue,
        };
        let _ = rec; /* rec is used indirectly via flatten_record */
        let fields_fbs: Vec<flatbuffers::WIPOffset<FieldDecl<'_>>> = flat
            .iter()
            .map(|f| {
                let fn_off = fbb.create_string(&f.flat_name);
                FieldDecl::create(
                    &mut fbb,
                    &FieldDeclArgs {
                        name: Some(fn_off),
                        type_tag: f.type_tag,
                    },
                )
            })
            .collect();
        let fields_vec = fbb.create_vector(&fields_fbs);
        record_fbs.push(RecordDecl::create(
            &mut fbb,
            &RecordDeclArgs {
                name: Some(name_off),
                fields: Some(fields_vec),
            },
        ));
    }
    let records_vec = fbb.create_vector(&record_fbs);

    /* Build BufferDecl vector. */
    let mut buffer_fbs: Vec<flatbuffers::WIPOffset<BufferDecl<'_>>> = Vec::new();
    for (buf_name, buf) in &cfg.state_buffers {
        let name_off = fbb.create_string(buf_name);
        let rec_off = fbb.create_string(&buf.record);
        buffer_fbs.push(BufferDecl::create(
            &mut fbb,
            &BufferDeclArgs {
                name: Some(name_off),
                record_name: Some(rec_off),
                count: buf.count,
            },
        ));
    }
    let buffers_vec = fbb.create_vector(&buffer_fbs);

    let root = CartLayouts::create(
        &mut fbb,
        &CartLayoutsArgs {
            records: Some(records_vec),
            buffers: Some(buffers_vec),
            schema_hash,
        },
    );
    fbb.finish(root, None);
    let body = fbb.finished_data();

    let mut out = Vec::with_capacity(8 + body.len());
    out.extend_from_slice(b"CLAY");
    out.extend_from_slice(&0u16.to_le_bytes()); // format_major
    out.extend_from_slice(&0u16.to_le_bytes()); // format_minor
    out.extend_from_slice(body);
    out
}

/* -------------------------------------------------------------------------
 * Codegen: generate cart_state.h / cart_state.rs and the Lua C snippet.
 *
 * Returns the paths written and metadata about the output.
 * buffers_present is false when no state_buffers are declared.
 * ------------------------------------------------------------------------- */

#[allow(dead_code)]
/// Computed outputs from blyt.config.yaml; no files are written.
struct CartStateData {
    c_header: String,
    rust_state: String,
    layouts_bytes: Vec<u8>,
    /// C function body for register_cart_state_S(); empty when no buffers.
    lua_c_snippet: String,
    buffers_present: bool,
}

fn compute_cart_state(cfg: &CartConfig) -> Result<CartStateData, BuildError> {
    use crate::config::{type_tag_buf_get_suffix, type_tag_c_type, type_tag_rust_type};

    let buffers_present = !cfg.state_buffers.is_empty();

    /* ---- C header ---- */
    let mut c_out = String::new();
    c_out.push_str("/* Auto-generated by blyt build — do not edit. */\n");
    c_out.push_str("#ifndef BLYT_CART_STATE_H\n");
    c_out.push_str("#define BLYT_CART_STATE_H\n");
    c_out.push_str("#include <blyt.h>\n\n");

    /* ---- Rust module ---- */
    let mut rs_out = String::new();
    rs_out.push_str("/* Auto-generated by blyt build — do not edit. */\n");
    rs_out.push_str("use blyt::{BlytBufferH, BlytFieldH};\n\n");

    /* ---- Lua C snippet: register_cart_state_S(L) (ADR-0059 + ADR-0011) ----
     * Part A: integer constants table "S" registered as global + require("S").
     * Part B: proxy metatables built entirely via the Lua C API (no lua_pcall /
     * luaL_dostring — blyt_lua_internal.h deliberately omits those).
     *
     * Design (ADR-0011):
     *   S.game[slot].score  reads  blyt_buffer_get_i32(S_GAME, slot, S_GAME_SCORE)
     *   S.game[slot].score = v  writes blyt_buffer_set_i32(...)
     *   S.game.count  returns the declared slot count
     *
     * Row proxies are pre-allocated (one per declared slot index) to avoid GC
     * pressure in gameplay loops.  The slot is stored at raw integer key 1 in
     * each row table; lua_rawgeti(row, 1) retrieves it without metamethods.
     *
     * One static C function trio is generated per buffer:
     *   _blyt_proxy_{buf}_row_idx    — row __index
     *   _blyt_proxy_{buf}_row_newidx — row __newindex
     *   _blyt_proxy_{buf}_idx        — buffer proxy __index (rows + count) */

    /* lua_c_proxy_fns: static C functions emitted before register_cart_state_S */
    let mut lua_c_proxy_fns = String::new();
    /* lua_c_fn: body of register_cart_state_S */
    let mut lua_c_fn = String::new();
    lua_c_fn.push_str("static void register_cart_state_S(lua_State *L) {\n");
    lua_c_fn.push_str("    lua_newtable(L);\n");
    /* lua_c_proxy_setup: proxy construction appended at end of lua_c_fn body */
    let mut lua_c_proxy_setup = String::new();

    let mut buf_index: u32 = 1;
    for (buf_name, buf_decl) in &cfg.state_buffers {
        let buf_upper = buf_name.to_uppercase();
        let c_prefix = format!("S_{buf_upper}");

        /* Buffer handle constant — C/Rust include S_ prefix, Lua key omits it */
        c_out.push_str(&format!(
            "#define {c_prefix} ((blyt_buffer_h){buf_index}u)\n"
        ));
        rs_out.push_str(&format!(
            "#[allow(dead_code)] pub const {c_prefix}: BlytBufferH = {buf_index};\n"
        ));
        lua_c_fn.push_str(&format!(
            "    lua_pushinteger(L, {buf_index}); lua_setfield(L, -2, \"{buf_upper}\");\n"
        ));

        /* Resolve flat fields for this buffer's record */
        let mut visiting = Vec::new();
        let fields: Vec<FlatField> =
            flatten_record(&buf_decl.record, &cfg.records, &mut visiting).map_err(|e| err(e))?;

        for f in &fields {
            /* C/Rust name: S_BUFNAME_FIELDNAME; Lua key: BUFNAME_FIELDNAME */
            let c_field = format!("{c_prefix}_{}", f.flat_name.to_uppercase());
            let lua_key = format!("{buf_upper}_{}", f.flat_name.to_uppercase());
            /* blyt_field_h: upper 16 bits = buf_id, lower 16 bits = field index */
            let field_h: u32 = (buf_index << 16) | (f.index & 0xFFFF);

            let c_type = type_tag_c_type(f.type_tag);
            let rs_type = type_tag_rust_type(f.type_tag);
            let _suffix = type_tag_buf_get_suffix(f.type_tag);

            /* ref: fields are u32 on the wire; annotate the generated
             * constant with the target buffer (ADR-0096). */
            let (c_note, rs_note) = match &f.ref_target {
                Some(target) => (
                    format!("blyt_entity_ref_t -> buffer \"{target}\""),
                    format!("entity ref -> buffer \"{target}\""),
                ),
                None => (c_type.to_string(), rs_type.to_string()),
            };

            c_out.push_str(&format!(
                "#define {c_field} ((blyt_field_h)0x{field_h:08X}u) /* {c_note} */\n"
            ));
            rs_out.push_str(&format!(
                "#[allow(dead_code)] pub const {c_field}: BlytFieldH = 0x{field_h:08X}; /* {rs_note} */\n"
            ));
            lua_c_fn.push_str(&format!(
                "    lua_pushinteger(L, 0x{field_h:08X}); lua_setfield(L, -2, \"{lua_key}\");\n"
            ));
        }

        /* ---- C proxy functions for this buffer ---- */
        let n_upvals = 1 + fields.len(); /* buf_h + one field_h per field */
        let fn_row_idx = format!("_blyt_proxy_{buf_name}_row_idx");
        let fn_row_newidx = format!("_blyt_proxy_{buf_name}_row_newidx");
        let fn_buf_idx = format!("_blyt_proxy_{buf_name}_idx");

        /* row __index: S.game[slot].field -> blyt_buffer_get_*(buf_h, slot, fh) */
        lua_c_proxy_fns.push_str(&format!("static int {fn_row_idx}(lua_State *L) {{\n"));
        lua_c_proxy_fns.push_str("    lua_rawgeti(L, 1, 1); /* slot stored at raw key 1 */\n");
        lua_c_proxy_fns.push_str("    lua_Integer _s = lua_tointeger(L, -1); lua_pop(L, 1);\n");
        lua_c_proxy_fns.push_str(
            "    blyt_buffer_h _bh = (blyt_buffer_h)lua_tointeger(L, lua_upvalueindex(1));\n",
        );
        lua_c_proxy_fns.push_str("    const char *_k = lua_tostring(L, 2);\n");
        lua_c_proxy_fns.push_str("    if (_k) {\n");
        for (fi, f) in fields.iter().enumerate() {
            let upval = fi + 2; /* upvalue 1=buf_h, 2+=field_h */
            let suffix = type_tag_buf_get_suffix(f.type_tag);
            let push = match f.type_tag {
                crate::config::TYPE_F32 | crate::config::TYPE_F64 => format!(
                    "lua_pushnumber(L, blyt_buffer_get_{suffix}(_bh, (int32_t)_s, (blyt_field_h)lua_tointeger(L, lua_upvalueindex({upval}))))"
                ),
                crate::config::TYPE_BOOL => format!(
                    "lua_pushboolean(L, blyt_buffer_get_{suffix}(_bh, (int32_t)_s, (blyt_field_h)lua_tointeger(L, lua_upvalueindex({upval}))) ? 1 : 0)"
                ),
                _ => format!(
                    "lua_pushinteger(L, blyt_buffer_get_{suffix}(_bh, (int32_t)_s, (blyt_field_h)lua_tointeger(L, lua_upvalueindex({upval}))))"
                ),
            };
            lua_c_proxy_fns.push_str(&format!(
                "        if (strcmp(_k, \"{}\") == 0) {{ {push}; return 1; }}\n",
                f.flat_name
            ));
        }
        lua_c_proxy_fns.push_str("    }\n");
        lua_c_proxy_fns.push_str("    lua_pushnil(L); return 1;\n");
        lua_c_proxy_fns.push_str("}\n");

        /* row __newindex: S.game[slot].field = v -> blyt_buffer_set_*(buf_h, slot, fh, v) */
        lua_c_proxy_fns.push_str(&format!("static int {fn_row_newidx}(lua_State *L) {{\n"));
        lua_c_proxy_fns.push_str("    lua_rawgeti(L, 1, 1);\n");
        lua_c_proxy_fns.push_str("    lua_Integer _s = lua_tointeger(L, -1); lua_pop(L, 1);\n");
        lua_c_proxy_fns.push_str(
            "    blyt_buffer_h _bh = (blyt_buffer_h)lua_tointeger(L, lua_upvalueindex(1));\n",
        );
        lua_c_proxy_fns.push_str("    const char *_k = lua_tostring(L, 2);\n");
        lua_c_proxy_fns.push_str("    if (_k) {\n");
        for (fi, f) in fields.iter().enumerate() {
            let upval = fi + 2;
            let suffix = type_tag_buf_get_suffix(f.type_tag);
            let set = match f.type_tag {
                crate::config::TYPE_F32 => format!(
                    "blyt_buffer_set_{suffix}(_bh, (int32_t)_s, (blyt_field_h)lua_tointeger(L, lua_upvalueindex({upval})), (float)lua_tonumber(L, 3))"
                ),
                crate::config::TYPE_F64 => format!(
                    "blyt_buffer_set_{suffix}(_bh, (int32_t)_s, (blyt_field_h)lua_tointeger(L, lua_upvalueindex({upval})), (double)lua_tonumber(L, 3))"
                ),
                crate::config::TYPE_BOOL => format!(
                    "blyt_buffer_set_{suffix}(_bh, (int32_t)_s, (blyt_field_h)lua_tointeger(L, lua_upvalueindex({upval})), (bool)(lua_toboolean(L, 3) != 0))"
                ),
                crate::config::TYPE_U32 => format!(
                    "blyt_buffer_set_{suffix}(_bh, (int32_t)_s, (blyt_field_h)lua_tointeger(L, lua_upvalueindex({upval})), (uint32_t)lua_tointeger(L, 3))"
                ),
                crate::config::TYPE_U16 => format!(
                    "blyt_buffer_set_{suffix}(_bh, (int32_t)_s, (blyt_field_h)lua_tointeger(L, lua_upvalueindex({upval})), (uint16_t)lua_tointeger(L, 3))"
                ),
                crate::config::TYPE_U8 => format!(
                    "blyt_buffer_set_{suffix}(_bh, (int32_t)_s, (blyt_field_h)lua_tointeger(L, lua_upvalueindex({upval})), (uint8_t)lua_tointeger(L, 3))"
                ),
                crate::config::TYPE_I16 => format!(
                    "blyt_buffer_set_{suffix}(_bh, (int32_t)_s, (blyt_field_h)lua_tointeger(L, lua_upvalueindex({upval})), (int16_t)lua_tointeger(L, 3))"
                ),
                crate::config::TYPE_I8 => format!(
                    "blyt_buffer_set_{suffix}(_bh, (int32_t)_s, (blyt_field_h)lua_tointeger(L, lua_upvalueindex({upval})), (int8_t)lua_tointeger(L, 3))"
                ),
                _ => format!(
                    "blyt_buffer_set_{suffix}(_bh, (int32_t)_s, (blyt_field_h)lua_tointeger(L, lua_upvalueindex({upval})), (int32_t)lua_tointeger(L, 3))"
                ),
            };
            lua_c_proxy_fns.push_str(&format!(
                "        if (strcmp(_k, \"{}\") == 0) {{ {set}; }}\n",
                f.flat_name
            ));
        }
        lua_c_proxy_fns.push_str("    }\n");
        lua_c_proxy_fns.push_str("    return 0;\n");
        lua_c_proxy_fns.push_str("}\n");

        /* buffer proxy __index: S.game[slot]->row proxy, S.game.count->n */
        lua_c_proxy_fns.push_str(&format!("static int {fn_buf_idx}(lua_State *L) {{\n"));
        lua_c_proxy_fns.push_str("    if (lua_type(L, 2) == LUA_TSTRING) {\n");
        lua_c_proxy_fns.push_str("        const char *_k = lua_tostring(L, 2);\n");
        lua_c_proxy_fns.push_str("        if (_k && strcmp(_k, \"count\") == 0) {\n");
        lua_c_proxy_fns.push_str(&format!(
            "            lua_pushinteger(L, {}); return 1;\n",
            buf_decl.count
        ));
        lua_c_proxy_fns.push_str("        }\n");
        lua_c_proxy_fns.push_str("        lua_pushnil(L); return 1;\n");
        lua_c_proxy_fns.push_str("    }\n");
        /* Integer key: return pre-allocated row proxy from upvalue 1 (_rows). */
        lua_c_proxy_fns.push_str("    lua_rawgeti(L, lua_upvalueindex(1), lua_tointeger(L, 2));\n");
        lua_c_proxy_fns.push_str("    return 1;\n");
        lua_c_proxy_fns.push_str("}\n");

        /* ---- Proxy setup code inside register_cart_state_S ---- */
        /* All absolute stack indices are tracked via lua_gettop snapshots.
         * lua_settop(L, _init) at the end restores the stack cleanly. */
        lua_c_proxy_setup.push_str("    {\n");
        lua_c_proxy_setup.push_str("        int _init = lua_gettop(L);\n");
        /* Create row metatable */
        lua_c_proxy_setup.push_str("        lua_newtable(L);\n");
        lua_c_proxy_setup.push_str("        int _row_mt = lua_gettop(L);\n");
        /* row __index closure: upvalue 1=buf_h, 2..=field_h per field */
        lua_c_proxy_setup.push_str(&format!("        lua_pushinteger(L, {buf_index}u);\n"));
        for f in &fields {
            let field_h: u32 = (buf_index << 16) | (f.index & 0xFFFF);
            lua_c_proxy_setup.push_str(&format!("        lua_pushinteger(L, 0x{field_h:08X}u);\n"));
        }
        lua_c_proxy_setup.push_str(&format!(
            "        lua_pushcclosure(L, {fn_row_idx}, {n_upvals});\n"
        ));
        lua_c_proxy_setup.push_str("        lua_setfield(L, _row_mt, \"__index\");\n");
        /* row __newindex closure */
        lua_c_proxy_setup.push_str(&format!("        lua_pushinteger(L, {buf_index}u);\n"));
        for f in &fields {
            let field_h: u32 = (buf_index << 16) | (f.index & 0xFFFF);
            lua_c_proxy_setup.push_str(&format!("        lua_pushinteger(L, 0x{field_h:08X}u);\n"));
        }
        lua_c_proxy_setup.push_str(&format!(
            "        lua_pushcclosure(L, {fn_row_newidx}, {n_upvals});\n"
        ));
        lua_c_proxy_setup.push_str("        lua_setfield(L, _row_mt, \"__newindex\");\n");
        /* Create _rows table */
        lua_c_proxy_setup.push_str("        lua_newtable(L);\n");
        lua_c_proxy_setup.push_str("        int _rows = lua_gettop(L);\n");
        /* Pre-allocate row proxies */
        lua_c_proxy_setup.push_str(&format!(
            "        for (lua_Integer _i = 0; _i < {}; _i++) {{\n",
            buf_decl.count
        ));
        lua_c_proxy_setup.push_str("            lua_newtable(L);\n");
        lua_c_proxy_setup.push_str("            int _row = lua_gettop(L);\n");
        lua_c_proxy_setup.push_str("            lua_pushinteger(L, _i);\n");
        lua_c_proxy_setup.push_str("            lua_rawseti(L, _row, 1); /* row[1] = slot */\n");
        lua_c_proxy_setup.push_str("            lua_pushvalue(L, _row_mt);\n");
        lua_c_proxy_setup.push_str("            lua_setmetatable(L, _row);\n");
        lua_c_proxy_setup.push_str("            lua_rawseti(L, _rows, _i); /* _rows[i] = row */\n");
        lua_c_proxy_setup.push_str("        }\n");
        /* Create buffer proxy + its metatable with __index closure */
        lua_c_proxy_setup.push_str("        lua_newtable(L);\n");
        lua_c_proxy_setup.push_str("        int _proxy = lua_gettop(L);\n");
        lua_c_proxy_setup.push_str("        lua_newtable(L);\n");
        lua_c_proxy_setup.push_str("        int _proxy_mt = lua_gettop(L);\n");
        lua_c_proxy_setup.push_str("        lua_pushvalue(L, _rows);\n");
        lua_c_proxy_setup.push_str(&format!(
            "        lua_pushinteger(L, {});\n",
            buf_decl.count
        ));
        lua_c_proxy_setup.push_str(&format!("        lua_pushcclosure(L, {fn_buf_idx}, 2);\n"));
        lua_c_proxy_setup.push_str("        lua_setfield(L, _proxy_mt, \"__index\");\n");
        lua_c_proxy_setup.push_str("        lua_pushvalue(L, _proxy_mt);\n");
        lua_c_proxy_setup.push_str("        lua_setmetatable(L, _proxy);\n");
        /* S.game = proxy */
        lua_c_proxy_setup.push_str("        lua_getglobal(L, \"S\");\n");
        lua_c_proxy_setup.push_str("        int _S = lua_gettop(L);\n");
        lua_c_proxy_setup.push_str("        lua_pushvalue(L, _proxy);\n");
        lua_c_proxy_setup.push_str(&format!("        lua_setfield(L, _S, \"{buf_name}\");\n"));
        lua_c_proxy_setup.push_str("        lua_settop(L, _init);\n");
        lua_c_proxy_setup.push_str("    }\n");

        c_out.push('\n');
        rs_out.push('\n');
        buf_index += 1;
    }

    /* Register in _LOADED["S"] so require("S") works; also set as global S. */
    lua_c_fn.push_str("    luaL_getsubtable(L, LUA_REGISTRYINDEX, \"_LOADED\");\n");
    lua_c_fn.push_str("    lua_pushvalue(L, -2);\n");
    lua_c_fn.push_str("    lua_setfield(L, -2, \"S\");\n");
    lua_c_fn.push_str("    lua_pop(L, 1); /* pop _LOADED */\n");
    lua_c_fn.push_str("    lua_setglobal(L, \"S\"); /* pops table */\n");
    lua_c_fn.push_str(&lua_c_proxy_setup);
    lua_c_fn.push_str("}\n");

    c_out.push_str("#endif /* BLYT_CART_STATE_H */\n");
    rs_out.push_str("/* end of generated constants */\n");

    let layouts_bytes = cart_layouts_bytes(cfg);
    let lua_c = format!("{lua_c_proxy_fns}{lua_c_fn}");

    Ok(CartStateData {
        c_header: c_out,
        rust_state: rs_out,
        layouts_bytes,
        lua_c_snippet: if buffers_present {
            lua_c
        } else {
            String::new()
        },
        buffers_present,
    })
}

/// Generate LuaLS/EmmyLua annotations for the `S` state-proxy so editing Lua
/// gameplay code gets completion and type-checking on `S.<buffer>[slot].<field>`
/// (issue #48 item 1b).  Mirrors the proxy that `generate_cart_state` builds:
/// `S.game[slot].score` (typed field), `S.game.count` (slot count), plus the
/// uppercase handle/field-id integer constants (`S.GAME`, `S.GAME_SCORE`).
pub(crate) fn generate_lua_state_decls(cfg: &CartConfig) -> Result<String, BuildError> {
    use crate::config::{FlatField, type_tag_lua_type};

    let mut out = String::new();
    out.push_str("---@meta\n");
    out.push_str("-- Auto-generated by blyt build — do not edit.\n");
    out.push_str("-- LuaLS annotations for the `S` state proxy (issue #48).\n\n");

    // S class: one field per buffer proxy, plus the integer handle constants.
    let mut s_fields = String::from("---@class blyt.S\n");

    for (buf_name, buf_decl) in &cfg.state_buffers {
        let buf_upper = buf_name.to_uppercase();
        let mut visiting = Vec::new();
        let fields: Vec<FlatField> =
            flatten_record(&buf_decl.record, &cfg.records, &mut visiting).map_err(err)?;

        // Row class: one typed field per flattened field.
        let row_class = format!("blyt.S.{buf_name}.row");
        out.push_str(&format!("---@class {row_class}\n"));
        for f in &fields {
            let lua_ty = type_tag_lua_type(f.type_tag);
            let note = match &f.ref_target {
                Some(t) => format!(" @ entity ref -> buffer \"{t}\""),
                None => String::new(),
            };
            out.push_str(&format!("---@field {} {lua_ty}{note}\n", f.flat_name));
        }
        out.push('\n');

        // Buffer proxy class: count + integer-indexed rows.
        let buf_class = format!("blyt.S.{buf_name}");
        out.push_str(&format!("---@class {buf_class}\n"));
        out.push_str(&format!(
            "---@field count integer @ {} slots\n",
            buf_decl.count
        ));
        out.push_str(&format!("---@field [integer] {row_class}\n\n"));

        // Register the proxy + handle constants on S.
        s_fields.push_str(&format!("---@field {buf_name} {buf_class}\n"));
        s_fields.push_str(&format!("---@field {buf_upper} integer @ buffer handle\n"));
        for f in &fields {
            s_fields.push_str(&format!(
                "---@field {buf_upper}_{} integer @ field id\n",
                f.flat_name.to_uppercase()
            ));
        }
    }

    out.push_str(&s_fields);
    // Declare the injected `S` global with the class type (runtime provides it).
    out.push_str("\n---@type blyt.S\n");
    out.push_str("S = {}\n");
    Ok(out)
}

/* -------------------------------------------------------------------------
 * Linker script (ADR-0024, ADR-0112)
 *
 * Produces an ET_DYN (PIE) RV32 ELF with PT_INTERP = /lib/ld-blyt.so.1 and
 * DT_NEEDED: libblyt32.so.
 *
 * PT_INTERP makes the cart a valid native executable on any system that has
 * /lib/ld-blyt.so.1 (a symlink to the platform's ILP32 dynamic linker).
 * On the emulated path, blyt's custom dynlinker ignores PT_INTERP and
 * handles DT_NEEDED resolution directly.
 *
 * PIE (ET_DYN) is required on the native path: the c-sky ILP32 kernel patch
 * computes AT_PHDR incorrectly for ET_EXEC carts (-no-pie), causing a segfault
 * in musl's startup. ET_DYN carts work correctly.
 *
 * ELF congruence (p_vaddr % p_align == p_offset % p_align):
 *   FILEHDR PHDRS in the text PT_LOAD anchors p_offset=0.  With p_vaddr=0
 *   (PIE, sections start at 0), both sides are 0 mod 0x1000. The ld.so maps
 *   the binary at a runtime base B chosen by the OS (≥ mmap_min_addr), so
 *   all virtual addresses are B + script_vaddr — no fixed-address constraint.
 *
 * Security requirements (ADR-0112):
 *   - PT_INTERP = /lib/ld-blyt.so.1 (required; validated at cart load time)
 *   - PT_GNU_RELRO + BIND_NOW: explicit relro PHDR + -z,relro -z,now
 *   - No ecall in cart code: cart calls blyt API via PLT only
 *   - Entry point in PF_X PT_LOAD segment: _blyt_entry in cart .text
 * ------------------------------------------------------------------------- */
const LINKER_SCRIPT: &str = "ENTRY(_blyt_entry)
";

/* Lua cart linker script (pure-Lua and hybrid Lua+C).
 *
 * Small Lua carts (< 4KB of code) pack all PT_LOAD segments into the same page.
 * musl's map_library initial-mmap is PROT_READ only (from the first PT_LOAD
 * flags) and only calls mmap_fixed for segments whose page base differs from
 * addr_min=0.  With everything on page 0 the RW GOT is never remapped, so
 * do_relocs cannot write PLT/GOT entries → SIGSEGV.  The first ALIGN(0x1000)
 * forces .got onto a separate page regardless of cart code size.
 *
 * C/C++/Rust carts are never this small and use the minimal LINKER_SCRIPT.
 *
 * PROVIDE'd start/stop symbols for .lua_regtab let cart_lua_modules iterate
 * registrars without GOT-via-hidden-visibility tricks; KEEP prevents
 * --gc-sections from discarding them before the symbols are resolved.
 *
 * .lua_regtab entries contain pointer fields that need RELATIVE relocations.
 * They are merged into the .data.rel.ro output section (placed between
 * .got.plt and .dynamic) so that lld's -z,relro algorithm includes them in
 * the single RELRO PT_LOAD on page 2.  If they were in a separate custom
 * output section, lld would create a second RW PT_LOAD on the same page.
 * The rv64ilp32 compat kernel cannot handle two RW PT_LOAD segments sharing
 * a page and one of the mappings ends up unmapped, causing SIGSEGV during
 * reloc_all in musl ld.so. */
const HYBRID_LUA_LINKER_SCRIPT: &str = "ENTRY(_blyt_entry)

SECTIONS {
    /* Page 0 (addr_min): R-only metadata + read-only data.
     * musl's map_library skips mmap_fixed for segments whose page base equals
     * addr_min; the initial PROT_READ mmap covers them correctly.
     *
     * SIZEOF_HEADERS reserves space for the ELF file header and program
     * headers at vaddr 0, so lld produces p_offset=0 for the first PT_LOAD.
     * Without this, lld places sections at vaddr=0 but file-offset=0x1000
     * (p_offset=0x1000), which causes the kernel to set AT_PHDR pointing to
     * file[0x1034] instead of the real program headers at file[0x34], and
     * ld.so crashes reading garbage as PHDRs before any constructor runs. */
    . = SIZEOF_HEADERS;
    .interp : { *(.interp) }
    .hash : { *(.hash) }
    .gnu.hash : { *(.gnu.hash) }
    .dynsym : { *(.dynsym) }
    .dynstr : { *(.dynstr) }
    .rela.dyn : { *(.rela.dyn) }
    .rela.plt : { *(.rela.plt) }
    .rodata : { *(.rodata .rodata.*) }
    /* .lua_exports contains only strings and byte fields (no pointer fields);
     * it needs no RELATIVE relocation and is safe in the read-only LOAD0. */
    .lua_exports : { KEEP(*(.lua_exports)) }
    /* Page 1+: executable code.  Page base != addr_min so musl remaps this
     * segment with PROT_READ|PROT_EXEC via mmap_fixed. */
    . = ALIGN(0x1000);
    .plt : { *(.plt) }
    .text : { *(.text .text.*) }
    /* Page 2+: RELRO region (.got/.got.plt/.data.rel.ro/.dynamic) plus
     * non-RELRO data.  Page base != addr_min so musl remaps with
     * PROT_READ|PROT_WRITE; RELRO then mprotects it read-only.
     *
     * .lua_regtab input sections are placed inside the .data.rel.ro output
     * section so that lld treats them as RELRO and does NOT create a second
     * RW PT_LOAD for them on this same page. */
    . = ALIGN(0x1000);
    .got : { *(.got) }
    .got.plt : { *(.got.plt) }
    .data.rel.ro : {
        PROVIDE(__start_lua_regtab = .);
        KEEP(*(.lua_regtab))
        PROVIDE(__stop_lua_regtab = .);
    }
    .dynamic : { *(.dynamic) }
    .data : { *(.data .data.*) }
    .sdata : { *(.sdata .sdata.*) }
    .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) }
    .sbss (NOLOAD) : { *(.sbss .sbss.*) }
}
";

fn err(msg: impl Into<String>) -> BuildError {
    build_err(msg)
}

fn config_file_input(project_dir: &Path) -> Vec<TaskInput> {
    let p = project_dir.join("blyt.config.yaml");
    if p.exists() {
        vec![TaskInput::File(p)]
    } else {
        vec![]
    }
}

fn variant_str(debug: bool) -> &'static str {
    if debug { "debug" } else { "release" }
}

fn write_bytes_if_changed(path: &Path, content: &[u8]) -> Result<bool, BuildError> {
    if fs::read(path).map(|e| e == content).unwrap_or(false) {
        return Ok(false);
    }
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    fs::write(path, content)?;
    Ok(true)
}

const ENTRY_STUB_C: &str = "/* Generated by blyt build — do not edit. */\n\
     void blyt_main(void);\n\
     /* blyt_exit: native libblyt32.so calls exit_group; on the emulated path\n\
      * ECALL_QUIT halts the emulator first so blyt_exit is never reached. */\n\
     __attribute__((noreturn)) void blyt_exit(int code);\n\
     /* blyt_runtime_startup: installs the restricted seccomp filter and resets\n\
      * FCSR before cart code runs on native hardware; no-op on emulated targets.\n\
      * Called here rather than via a .init_array constructor — musl ILP32 ld.so\n\
      * does not invoke constructors on this custom entry path (issue #43). */\n\
     void blyt_runtime_startup(void);\n\
     void _blyt_entry(void) {\n\
         blyt_runtime_startup();\n\
         blyt_main();\n\
         blyt_exit(0);\n\
     }\n";

const INTERP_STUB_C: &str = "/* Generated by blyt build — do not edit. */\n\
     __attribute__((section(\".interp\"), used))\n\
     const char blyt_interp[] = \"/lib/ld-blyt.so.1\";\n";

fn generate_lua_glue_c(
    out_path: &Path,
    buffers_present: bool,
    lua_c_snippet: &str,
) -> Result<(), BuildError> {
    let mut glue = String::new();
    glue.push_str("/* Generated by blyt build — do not edit. */\n");
    glue.push_str("#include <blyt.h>\n");
    if buffers_present {
        glue.push_str("#include <string.h> /* strcmp for proxy field dispatch */\n");
        glue.push_str(lua_c_snippet);
    }
    glue.push_str(
        "/* Defined by the linker script via PROVIDE; not synthesised at link time. */\n",
    );
    glue.push_str("extern blyt_lua_regtab_entry_t __start_lua_regtab[];\n");
    glue.push_str("extern blyt_lua_regtab_entry_t __stop_lua_regtab[];\n");
    glue.push_str("void cart_lua_modules(lua_State *L) {\n");
    if buffers_present {
        glue.push_str("    register_cart_state_S(L);\n");
    }
    glue.push_str("    blyt_lua_regtab_entry_t *e;\n");
    glue.push_str("    for (e = __start_lua_regtab; e < __stop_lua_regtab; e++) {\n");
    glue.push_str("        if (!e->module_name) {\n");
    glue.push_str("            lua_pushcfunction(L, e->wrapper);\n");
    glue.push_str("            lua_setglobal(L, e->lua_fn_name);\n");
    glue.push_str("        } else {\n");
    glue.push_str("            lua_getglobal(L, e->module_name);\n");
    glue.push_str("            if (!lua_istable(L, -1)) {\n");
    glue.push_str("                lua_pop(L, 1);\n");
    glue.push_str("                lua_newtable(L);\n");
    glue.push_str("                lua_pushvalue(L, -1);\n");
    glue.push_str("                lua_setglobal(L, e->module_name);\n");
    glue.push_str("                luaL_getsubtable(L, LUA_REGISTRYINDEX, \"_LOADED\");\n");
    glue.push_str("                lua_pushvalue(L, -2);\n");
    glue.push_str("                lua_setfield(L, -2, e->module_name);\n");
    glue.push_str("                lua_pop(L, 1);\n");
    glue.push_str("            }\n");
    glue.push_str("            lua_pushcfunction(L, e->wrapper);\n");
    glue.push_str("            lua_setfield(L, -2, e->lua_fn_name);\n");
    glue.push_str("            lua_pop(L, 1);\n");
    glue.push_str("        }\n");
    glue.push_str("    }\n");
    glue.push_str("}\n");
    fs::write(out_path, glue)?;
    Ok(())
}

/* -------------------------------------------------------------------------
 * Task implementations
 * ------------------------------------------------------------------------- */

struct CompileCTask {
    key_str: String,
    clang: String,
    src: PathBuf,
    build_dir: PathBuf,
    sdk_include: PathBuf,
    extra_includes: Vec<PathBuf>,
    extra_defines: Vec<String>,
    debug_flags: Vec<String>,
    project_dir: PathBuf,
}

impl Task for CompileCTask {
    fn key(&self) -> &str {
        &self.key_str
    }
    fn label(&self) -> String {
        let rel = self
            .src
            .strip_prefix(&self.project_dir)
            .unwrap_or(&self.src);
        format!("compile  {}", rel.display())
    }
    fn inputs(&self) -> Vec<TaskInput> {
        let stem = self
            .src
            .file_stem()
            .and_then(OsStr::to_str)
            .unwrap_or("unknown");
        let depfile = self.build_dir.join(format!("{stem}.d"));
        let mut v = vec![TaskInput::File(self.src.clone())];
        for dep in parse_depfile(&depfile) {
            v.push(TaskInput::File(dep));
        }
        v.push(TaskInput::Value(self.clang.clone()));
        for f in &self.debug_flags {
            v.push(TaskInput::Value(f.clone()));
        }
        for d in &self.extra_defines {
            v.push(TaskInput::Value(d.clone()));
        }
        v
    }
    fn outputs(&self) -> Vec<PathBuf> {
        let stem = self
            .src
            .file_stem()
            .and_then(OsStr::to_str)
            .unwrap_or("unknown");
        vec![self.build_dir.join(format!("{stem}.o"))]
    }
    fn run(&self) -> Result<(), BuildError> {
        let inc_refs: Vec<&Path> = self.extra_includes.iter().map(PathBuf::as_path).collect();
        compile_c(
            &self.clang,
            &self.src,
            &self.build_dir,
            &self.sdk_include,
            &inc_refs,
            &self.extra_defines,
            &self.debug_flags,
        )?;
        Ok(())
    }
}

struct CompileCppTask {
    key_str: String,
    clangpp: String,
    src: PathBuf,
    build_dir: PathBuf,
    sdk_include: PathBuf,
    libcxx_include: PathBuf,
    extra_includes: Vec<PathBuf>,
    debug_flags: Vec<String>,
    project_dir: PathBuf,
}

impl Task for CompileCppTask {
    fn key(&self) -> &str {
        &self.key_str
    }
    fn label(&self) -> String {
        let rel = self
            .src
            .strip_prefix(&self.project_dir)
            .unwrap_or(&self.src);
        format!("compile  {}", rel.display())
    }
    fn inputs(&self) -> Vec<TaskInput> {
        let stem = self
            .src
            .file_stem()
            .and_then(OsStr::to_str)
            .unwrap_or("unknown");
        let depfile = self.build_dir.join(format!("{stem}.d"));
        let mut v = vec![TaskInput::File(self.src.clone())];
        for dep in parse_depfile(&depfile) {
            v.push(TaskInput::File(dep));
        }
        v.push(TaskInput::Value(self.clangpp.clone()));
        for f in &self.debug_flags {
            v.push(TaskInput::Value(f.clone()));
        }
        v
    }
    fn outputs(&self) -> Vec<PathBuf> {
        let stem = self
            .src
            .file_stem()
            .and_then(OsStr::to_str)
            .unwrap_or("unknown");
        vec![self.build_dir.join(format!("{stem}.o"))]
    }
    fn run(&self) -> Result<(), BuildError> {
        let inc_refs: Vec<&Path> = self.extra_includes.iter().map(PathBuf::as_path).collect();
        compile_cpp(
            &self.clangpp,
            &self.src,
            &self.build_dir,
            &self.sdk_include,
            &self.libcxx_include,
            &inc_refs,
            &self.debug_flags,
        )?;
        Ok(())
    }
}

struct GenerateCHeaderTask {
    project_dir: PathBuf,
    build_dir: PathBuf,
}

impl Task for GenerateCHeaderTask {
    fn key(&self) -> &str {
        "generate_c_header"
    }
    fn label(&self) -> String {
        "generate cart_state.h".to_string()
    }
    fn inputs(&self) -> Vec<TaskInput> {
        config_file_input(&self.project_dir)
    }
    fn outputs(&self) -> Vec<PathBuf> {
        vec![self.build_dir.join("blyt/c/cart_state.h")]
    }
    fn run(&self) -> Result<(), BuildError> {
        let cfg = crate::config::read_cart_config(&self.project_dir).map_err(err)?;
        let data = compute_cart_state(&cfg)?;
        let dir = self.build_dir.join("blyt/c");
        fs::create_dir_all(&dir)?;
        fs::write(dir.join("cart_state.h"), data.c_header)?;
        Ok(())
    }
}

struct GenerateRustStateTask {
    project_dir: PathBuf,
    build_dir: PathBuf,
}

impl Task for GenerateRustStateTask {
    fn key(&self) -> &str {
        "generate_rust_state"
    }
    fn label(&self) -> String {
        "generate cart_state.rs".to_string()
    }
    fn inputs(&self) -> Vec<TaskInput> {
        config_file_input(&self.project_dir)
    }
    fn outputs(&self) -> Vec<PathBuf> {
        vec![self.build_dir.join("blyt/rust/cart_state.rs")]
    }
    fn run(&self) -> Result<(), BuildError> {
        let cfg = crate::config::read_cart_config(&self.project_dir).map_err(err)?;
        let data = compute_cart_state(&cfg)?;
        let dir = self.build_dir.join("blyt/rust");
        fs::create_dir_all(&dir)?;
        fs::write(dir.join("cart_state.rs"), data.rust_state)?;
        Ok(())
    }
}

struct GenerateLayoutsTask {
    project_dir: PathBuf,
    build_dir: PathBuf,
}

impl Task for GenerateLayoutsTask {
    fn key(&self) -> &str {
        "generate_layouts"
    }
    fn label(&self) -> String {
        "generate cart.layouts.bin".to_string()
    }
    fn inputs(&self) -> Vec<TaskInput> {
        config_file_input(&self.project_dir)
    }
    fn outputs(&self) -> Vec<PathBuf> {
        vec![self.build_dir.join("cart.layouts.bin")]
    }
    fn run(&self) -> Result<(), BuildError> {
        let cfg = crate::config::read_cart_config(&self.project_dir).map_err(err)?;
        let data = compute_cart_state(&cfg)?;
        fs::write(self.build_dir.join("cart.layouts.bin"), data.layouts_bytes)?;
        Ok(())
    }
}

struct GenerateLuaGlueTask {
    project_dir: PathBuf,
    build_dir: PathBuf,
}

impl Task for GenerateLuaGlueTask {
    fn key(&self) -> &str {
        "generate_lua_glue"
    }
    fn label(&self) -> String {
        "generate __blyt_lua_glue.c".to_string()
    }
    fn inputs(&self) -> Vec<TaskInput> {
        config_file_input(&self.project_dir)
    }
    fn outputs(&self) -> Vec<PathBuf> {
        vec![self.build_dir.join("__blyt_lua_glue.c")]
    }
    fn run(&self) -> Result<(), BuildError> {
        let cfg = crate::config::read_cart_config(&self.project_dir).map_err(err)?;
        let data = compute_cart_state(&cfg)?;
        generate_lua_glue_c(
            &self.build_dir.join("__blyt_lua_glue.c"),
            data.buffers_present,
            &data.lua_c_snippet,
        )
    }
}

struct CompileLuaTask {
    luac: String,
    lua_files: Vec<PathBuf>,
    project_dir: PathBuf,
    output: PathBuf,
}

impl Task for CompileLuaTask {
    fn key(&self) -> &str {
        "lua_bytecode"
    }
    fn label(&self) -> String {
        "luac     src/game/lua/".to_string()
    }
    fn inputs(&self) -> Vec<TaskInput> {
        let mut v: Vec<TaskInput> = self
            .lua_files
            .iter()
            .map(|f| TaskInput::File(f.clone()))
            .collect();
        v.push(TaskInput::Value(self.luac.clone()));
        v
    }
    fn outputs(&self) -> Vec<PathBuf> {
        vec![self.output.clone()]
    }
    fn run(&self) -> Result<(), BuildError> {
        let mut cmd = Command::new(&self.luac);
        cmd.arg("-o")
            .arg(&self.output)
            .arg("-P")
            .arg(&self.project_dir);
        for f in &self.lua_files {
            cmd.arg(f);
        }
        let status = cmd
            .status()
            .map_err(|e| err(format!("failed to run {}: {e}", self.luac)))?;
        if !status.success() {
            return Err(err("luac compilation failed"));
        }
        Ok(())
    }
}

struct GenerateLuaDataTask {
    bytecode_path: PathBuf,
    output_c: PathBuf,
}

impl Task for GenerateLuaDataTask {
    fn key(&self) -> &str {
        "gen_lua_data"
    }
    fn label(&self) -> String {
        "gen      cart_lua_data.c".to_string()
    }
    fn inputs(&self) -> Vec<TaskInput> {
        vec![TaskInput::File(self.bytecode_path.clone())]
    }
    fn outputs(&self) -> Vec<PathBuf> {
        vec![self.output_c.clone()]
    }
    fn run(&self) -> Result<(), BuildError> {
        generate_lua_data_c(&self.bytecode_path, &self.output_c)
    }
}

struct AssembleLibArchiveTask {
    key_str: String,
    ar: String,
    obj_files: Vec<PathBuf>,
    output: PathBuf,
}

impl Task for AssembleLibArchiveTask {
    fn key(&self) -> &str {
        &self.key_str
    }
    fn label(&self) -> String {
        format!(
            "archive  {}",
            self.output
                .file_name()
                .unwrap_or_default()
                .to_string_lossy()
        )
    }
    fn inputs(&self) -> Vec<TaskInput> {
        let mut v: Vec<TaskInput> = self
            .obj_files
            .iter()
            .map(|f| TaskInput::File(f.clone()))
            .collect();
        v.push(TaskInput::Value(self.ar.clone()));
        v
    }
    fn outputs(&self) -> Vec<PathBuf> {
        vec![self.output.clone()]
    }
    fn run(&self) -> Result<(), BuildError> {
        run_archive(&self.ar, &self.obj_files, &self.output)
    }
}

struct CompileRustTask {
    key_str: String,
    label_str: String,
    cargo: String,
    manifest: PathBuf,
    build_dir: PathBuf,
    rust_sdk_path: PathBuf,
    rust_lib_patches: Vec<(String, PathBuf)>,
    extra_rustflags: String,
    is_lua: bool,
    cart_state_rs: Option<PathBuf>,
    output: PathBuf,
    source_dir: PathBuf,
}

impl Task for CompileRustTask {
    fn key(&self) -> &str {
        &self.key_str
    }
    fn label(&self) -> String {
        self.label_str.clone()
    }
    fn inputs(&self) -> Vec<TaskInput> {
        let mut v = vec![TaskInput::File(self.manifest.clone())];
        // Track all Rust source files so a change triggers recompile.
        if self.source_dir.is_dir() {
            if let Ok(mut srcs) = collect_rust_files(&self.source_dir) {
                srcs.sort();
                for s in srcs {
                    v.push(TaskInput::File(s));
                }
            }
        }
        v.push(TaskInput::Value(self.extra_rustflags.clone()));
        if let Some(ref rs) = self.cart_state_rs {
            v.push(TaskInput::File(rs.clone()));
        }
        v
    }
    fn outputs(&self) -> Vec<PathBuf> {
        vec![self.output.clone()]
    }
    fn run(&self) -> Result<(), BuildError> {
        let archive = build_rust_archive(
            &self.cargo,
            &self.manifest,
            &self.build_dir,
            &self.rust_sdk_path,
            &self.rust_lib_patches,
            &self.extra_rustflags,
            self.is_lua,
            self.cart_state_rs.as_deref(),
        )?;
        if archive != self.output {
            fs::copy(&archive, &self.output)
                .map_err(|e| err(format!("failed to copy rust archive: {e}")))?;
        }
        Ok(())
    }
}

struct CompileExternalTask {
    key_str: String,
    config: ExternalCompileConfig,
    project_dir: PathBuf,
    build_dir: PathBuf,
    sdk_include: PathBuf,
    sdk_lib: PathBuf,
    sdk_bin: PathBuf,
    cart_state_include: PathBuf,
    objcopy: String,
    debug: bool,
    src_files: Vec<PathBuf>,
    output: PathBuf,
}

impl Task for CompileExternalTask {
    fn key(&self) -> &str {
        &self.key_str
    }
    fn label(&self) -> String {
        format!("external compile_command ({:?})", self.config.language)
    }
    fn inputs(&self) -> Vec<TaskInput> {
        let mut v: Vec<TaskInput> = self
            .src_files
            .iter()
            .map(|f| TaskInput::File(f.clone()))
            .collect();
        v.push(TaskInput::Value(self.config.command_template.clone()));
        v.push(TaskInput::Value(format!("debug={}", self.debug)));
        v
    }
    fn outputs(&self) -> Vec<PathBuf> {
        vec![self.output.clone()]
    }
    fn run(&self) -> Result<(), BuildError> {
        compile_external(
            &self.config,
            &self.project_dir,
            &self.build_dir,
            &self.sdk_include,
            &self.sdk_lib,
            &self.sdk_bin,
            &self.cart_state_include,
            &self.objcopy,
            self.debug,
        )?;
        Ok(())
    }
}

struct LinkElfTask {
    clang: String,
    obj_files: Vec<PathBuf>,
    rust_archive: Option<PathBuf>,
    lib_archives: Vec<PathBuf>,
    ld_script: PathBuf,
    lib_dir: PathBuf,
    output: PathBuf,
    is_lua: bool,
}

impl Task for LinkElfTask {
    fn key(&self) -> &str {
        "link"
    }
    fn label(&self) -> String {
        "link     cart.elf".to_string()
    }
    fn inputs(&self) -> Vec<TaskInput> {
        let mut v: Vec<TaskInput> = self
            .obj_files
            .iter()
            .map(|f| TaskInput::File(f.clone()))
            .collect();
        if let Some(ref a) = self.rust_archive {
            v.push(TaskInput::File(a.clone()));
        }
        for a in &self.lib_archives {
            v.push(TaskInput::File(a.clone()));
        }
        v.push(TaskInput::File(self.ld_script.clone()));
        v.push(TaskInput::Value(self.clang.clone()));
        v
    }
    fn outputs(&self) -> Vec<PathBuf> {
        vec![self.output.clone()]
    }
    fn run(&self) -> Result<(), BuildError> {
        link_cart(
            &self.clang,
            &self.obj_files,
            self.rust_archive.as_deref(),
            &self.lib_archives,
            &self.ld_script,
            &self.lib_dir,
            &self.output,
            self.is_lua,
        )
    }
}

struct AssembleCartTask {
    objcopy: String,
    raw_elf: PathBuf,
    cart_info_file: PathBuf,
    output: PathBuf,
    extra_sections: Vec<(String, PathBuf)>,
    debug: bool,
}

impl Task for AssembleCartTask {
    fn key(&self) -> &str {
        "finalise"
    }
    fn label(&self) -> String {
        "finalise cart".to_string()
    }
    fn inputs(&self) -> Vec<TaskInput> {
        let mut v = vec![
            TaskInput::File(self.raw_elf.clone()),
            TaskInput::File(self.cart_info_file.clone()),
        ];
        for (_, path) in &self.extra_sections {
            v.push(TaskInput::File(path.clone()));
        }
        v.push(TaskInput::Value(self.objcopy.clone()));
        v.push(TaskInput::Value(format!("debug={}", self.debug)));
        v
    }
    fn outputs(&self) -> Vec<PathBuf> {
        vec![self.output.clone()]
    }
    fn run(&self) -> Result<(), BuildError> {
        let extra: Vec<(&str, &Path)> = self
            .extra_sections
            .iter()
            .map(|(n, p)| (n.as_str(), p.as_path()))
            .collect();
        finalise_cart(
            &self.objcopy,
            &self.raw_elf,
            &self.cart_info_file,
            &self.output,
            &extra,
            self.debug,
        )
    }
}

fn collect_rust_files(dir: &Path) -> Result<Vec<PathBuf>, BuildError> {
    let mut out = Vec::new();
    collect_rust_recursive(dir, &mut out)?;
    Ok(out)
}

fn collect_rust_recursive(dir: &Path, out: &mut Vec<PathBuf>) -> Result<(), BuildError> {
    for entry in fs::read_dir(dir)? {
        let entry = entry?;
        let path = entry.path();
        if path.is_dir() {
            collect_rust_recursive(&path, out)?;
        } else if path.extension().and_then(OsStr::to_str) == Some("rs") {
            out.push(path);
        }
    }
    Ok(())
}

/* Helper to build a CompileCTask with a project-relative key. */
fn make_c_task(
    project_dir: &Path,
    src: PathBuf,
    build_dir: PathBuf,
    clang: &str,
    sdk_include: &Path,
    extra_includes: Vec<PathBuf>,
    extra_defines: Vec<String>,
    debug_flags: Vec<String>,
    key_prefix: &str,
) -> CompileCTask {
    let rel = src.strip_prefix(project_dir).unwrap_or(&src);
    let key_str = format!("{key_prefix}/{}", rel.display());
    CompileCTask {
        key_str,
        clang: clang.to_string(),
        src,
        build_dir,
        sdk_include: sdk_include.to_path_buf(),
        extra_includes,
        extra_defines,
        debug_flags,
        project_dir: project_dir.to_path_buf(),
    }
}

fn make_cpp_task(
    project_dir: &Path,
    src: PathBuf,
    build_dir: PathBuf,
    clangpp: &str,
    sdk_include: &Path,
    libcxx_include: PathBuf,
    extra_includes: Vec<PathBuf>,
    debug_flags: Vec<String>,
    key_prefix: &str,
) -> CompileCppTask {
    let rel = src.strip_prefix(project_dir).unwrap_or(&src);
    let key_str = format!("{key_prefix}/{}", rel.display());
    CompileCppTask {
        key_str,
        clangpp: clangpp.to_string(),
        src,
        build_dir,
        sdk_include: sdk_include.to_path_buf(),
        libcxx_include,
        extra_includes,
        debug_flags,
        project_dir: project_dir.to_path_buf(),
    }
}

/* -------------------------------------------------------------------------
 * Build manifest (blyt.build.yaml)
 *
 * ADR-0073: Lua is the default cart language and does not need to be declared.
 * Native languages (C, C++, Rust) require explicit declaration via `language:`
 * (singular, one language) or `languages:` (plural map, for hybrid carts).
 *
 * Hybrid Lua + native example:
 *   languages:
 *     lua:
 *     c:
 *
 * If blyt.build.yaml is present but has neither `language:` nor `languages:`,
 * the build errors rather than guessing (ADR-0073).
 * Per-language sub-keys (`codegen`, `sources`) are parsed but not yet acted on.
 * ------------------------------------------------------------------------- */

#[derive(Debug, PartialEq, Eq, PartialOrd, Ord, Clone, Copy)]
enum CartLanguage {
    C,
    Cpp,
    Lua,
    Rust,
}

#[derive(serde::Deserialize)]
#[serde(deny_unknown_fields)]
struct BuildManifest {
    language: Option<String>,
    languages: Option<BTreeMap<String, Option<LanguageConfig>>>,
}

#[derive(serde::Deserialize, Default)]
#[serde(deny_unknown_fields)]
#[allow(dead_code)] // codegen/sources parsed for validation; not yet acted on
struct LanguageConfig {
    codegen: Option<bool>,
    sources: Option<Vec<String>>,
    compile_command: Option<String>,
    source_extension: Option<String>,
    strip_sections: Option<Vec<String>>,
}

enum ExternalOutputType {
    Object,  // @OBJFILE@ → <lang>.o, pushed to obj_files
    Archive, // @LIBFILE@ → <lang>.a, pushed to lib_archives
}

struct ExternalCompileConfig {
    language: String,
    extension: String,
    command_template: String,
    strip_sections: Vec<String>,
    output_type: ExternalOutputType,
}

enum BuildLanguages {
    Builtin(BTreeSet<CartLanguage>),
    External {
        /// Additional builtin languages (C, Rust, …) compiled alongside the
        /// external compile_command.  Empty for single-language external carts.
        builtin: BTreeSet<CartLanguage>,
        cfg: ExternalCompileConfig,
    },
}

fn parse_language_str(s: &str) -> Result<CartLanguage, BuildError> {
    match s {
        "c" => Ok(CartLanguage::C),
        "c++" => Ok(CartLanguage::Cpp),
        "rust" => Ok(CartLanguage::Rust),
        "lua" => Ok(CartLanguage::Lua),
        other => Err(err(format!(
            "blyt.build.yaml: unknown language {other:?} — \
             expected `c`, `c++`, `rust`, or `lua`"
        ))),
    }
}

fn read_cart_languages(project_dir: &Path) -> Result<BuildLanguages, BuildError> {
    let manifest_path = project_dir.join("blyt.build.yaml");
    if !manifest_path.exists() {
        return Ok(BuildLanguages::Builtin(BTreeSet::from([CartLanguage::Lua])));
    }
    let text = fs::read_to_string(&manifest_path)?;
    let manifest: BuildManifest =
        serde_yaml::from_str(&text).map_err(|e| err(format!("blyt.build.yaml: {e}")))?;

    match (manifest.language, manifest.languages) {
        (None, None) => Err(err("blyt.build.yaml: no language declaration — \
             add `language: lua` (or other language) or a `languages:` map")),
        (Some(_), Some(_)) => Err(err(
            "blyt.build.yaml: `language` and `languages` cannot both be set",
        )),
        (Some(lang), None) => Ok(BuildLanguages::Builtin(BTreeSet::from([
            parse_language_str(&lang)?,
        ]))),
        (None, Some(map)) => {
            if map.is_empty() {
                return Err(err("blyt.build.yaml: `languages:` map is empty"));
            }
            // Partition into external (has compile_command) and builtin languages.
            // At most one language may carry a compile_command.
            let mut external: Option<(String, LanguageConfig)> = None;
            let mut builtin: BTreeSet<CartLanguage> = BTreeSet::new();
            for (key, config) in map {
                let cfg = config.unwrap_or_default();
                if cfg.compile_command.is_some() {
                    if external.is_some() {
                        return Err(err(
                            "blyt.build.yaml: at most one language in `languages:` \
                             may have `compile_command`",
                        ));
                    }
                    external = Some((key, cfg));
                } else {
                    if cfg.source_extension.is_some() {
                        return Err(err(format!(
                            "blyt.build.yaml: `{key}`: \
                             `source_extension` requires `compile_command`"
                        )));
                    }
                    if cfg.strip_sections.is_some() {
                        return Err(err(format!(
                            "blyt.build.yaml: `{key}`: \
                             `strip_sections` requires `compile_command`"
                        )));
                    }
                    builtin.insert(parse_language_str(&key)?);
                }
            }
            match external {
                None => Ok(BuildLanguages::Builtin(builtin)),
                Some((lang, lang_cfg)) => {
                    let cmd = lang_cfg.compile_command.unwrap();
                    validate_compile_command_template(&cmd)?;
                    let output_type = if cmd.contains("@LIBFILE@") {
                        ExternalOutputType::Archive
                    } else {
                        ExternalOutputType::Object
                    };
                    let extension = lang_cfg
                        .source_extension
                        .map(|s| s.strip_prefix('.').map(str::to_string).unwrap_or(s))
                        .unwrap_or_else(|| lang.clone());
                    Ok(BuildLanguages::External {
                        builtin,
                        cfg: ExternalCompileConfig {
                            language: lang,
                            extension,
                            command_template: cmd,
                            strip_sections: lang_cfg.strip_sections.unwrap_or_default(),
                            output_type,
                        },
                    })
                }
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * blyt.info.yaml — cart identity manifest
 *
 * Required for every cart project:
 *   id: hello            # machine identifier: output filename, save dir
 *   title: Hello World   # human-readable title
 *   version: 0.1.0       # optional, semver; defaults to 0.0.1-dev
 *
 * Validation rules mirror the runtime loader (cart_id_valid in
 * runtime/host/src/libblyt/cart_load.c — keep in sync):
 *   id:    1-63 bytes, [a-z0-9_-], first char alphanumeric
 *   title: non-empty, no control characters
 * ------------------------------------------------------------------------- */

#[derive(serde::Deserialize)]
#[serde(deny_unknown_fields)]
struct InfoManifest {
    id: Option<String>,
    title: Option<String>,
    version: Option<String>,
}

#[derive(Debug)]
struct InfoFields {
    id: String,
    title: String,
    version: String,
}

fn cart_id_valid(id: &str) -> bool {
    if id.is_empty() || id.len() > 63 {
        return false;
    }
    let bytes = id.as_bytes();
    if !bytes[0].is_ascii_lowercase() && !bytes[0].is_ascii_digit() {
        return false;
    }
    bytes
        .iter()
        .all(|&b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'_' || b == b'-')
}

fn read_cart_info(project_dir: &Path) -> Result<InfoFields, BuildError> {
    let info_path = project_dir.join("blyt.info.yaml");
    if !info_path.exists() {
        return Err(err(format!(
            "blyt.info.yaml not found in {} — \
             every blyt cart project must have this file",
            project_dir.display()
        )));
    }
    let text = fs::read_to_string(&info_path)?;
    let manifest: InfoManifest =
        serde_yaml::from_str(&text).map_err(|e| err(format!("blyt.info.yaml: {e}")))?;
    let id = manifest.id.ok_or_else(|| {
        err("blyt.info.yaml: missing required field `id` — \
             the machine identifier used for the output filename and save directory")
    })?;
    if !cart_id_valid(&id) {
        return Err(err(format!(
            "blyt.info.yaml: invalid `id` {id:?} — \
             must be 1-63 characters from [a-z0-9_-], starting with a letter or digit"
        )));
    }
    let title = manifest.title.ok_or_else(|| {
        err("blyt.info.yaml: missing required field `title` — \
             the human-readable title of the cart")
    })?;
    if title.is_empty() || title.chars().any(|c| c.is_control()) {
        return Err(err(
            "blyt.info.yaml: `title` must be non-empty and contain no control characters",
        ));
    }
    let version = match manifest.version {
        None => "0.0.1-dev".to_string(),
        Some(v) => {
            semver::Version::parse(&v).map_err(|e| {
                err(format!(
                    "blyt.info.yaml: `version` {v:?} is not a valid semver string: {e}"
                ))
            })?;
            v
        }
    };
    Ok(InfoFields { id, title, version })
}

/* -------------------------------------------------------------------------
 * Public entry point
 * ------------------------------------------------------------------------- */

pub fn run(
    project_dir: &Path,
    output: Option<&Path>,
    debug: bool,
    force: bool,
) -> Result<PathBuf, BuildError> {
    // Canonicalise to an absolute path so the build is independent of how blyt
    // was invoked (relative vs absolute project arg).
    let project_dir = &fs::canonicalize(project_dir)?;
    let cart_info = read_cart_info(project_dir)?;

    let clang = find_clang();
    let clangpp = find_clangpp();
    let objcopy = find_objcopy();
    let ar = find_ar();

    let sdk_include = find_sdk_include()?;
    let lib_dir = find_lib_dir(&sdk_include)?;
    let sdk_root = sdk_include
        .parent()
        .map(Path::to_path_buf)
        .unwrap_or_else(|| sdk_include.clone());

    let (languages, external_config): (BTreeSet<CartLanguage>, Option<ExternalCompileConfig>) =
        match read_cart_languages(project_dir)? {
            BuildLanguages::Builtin(s) => (s, None),
            BuildLanguages::External { builtin, cfg } => (builtin, Some(cfg)),
        };
    let is_lua = languages.contains(&CartLanguage::Lua);
    let native_count = languages
        .iter()
        .filter(|&&l| l != CartLanguage::Lua)
        .count();

    // Early validation: check source files exist before doing any build work.
    if is_lua {
        let lua_src_dir = project_dir.join("src/game/lua");
        if collect_lua_files(&lua_src_dir)?.is_empty() {
            return Err(err(format!(
                "no .lua files found under {} — \
                 for a C or Rust cart add `language: c` or `language: rust` \
                 to blyt.build.yaml",
                lua_src_dir.display()
            )));
        }
        if native_count == 0 {
            if !collect_c_files(&project_dir.join("src/game/c"))?.is_empty() {
                return Err(err(
                    "src/game/c/ contains .c files but no native language is declared — \
                     add a `languages:` block to blyt.build.yaml, e.g.:\n\
                     \x20 languages:\n\
                     \x20   lua:\n\
                     \x20   c:",
                ));
            }
            if !collect_cpp_files(&project_dir.join("src/game/c++"))?.is_empty() {
                return Err(err(
                    "src/game/c++/ contains C++ files but no native language is declared — \
                     add a `languages:` block to blyt.build.yaml",
                ));
            }
            if project_dir.join("src/game/rust/Cargo.toml").exists() {
                return Err(err(
                    "src/game/rust/Cargo.toml exists but no native language is declared — \
                     add a `languages:` block to blyt.build.yaml, e.g.:\n\
                     \x20 languages:\n\
                     \x20   lua:\n\
                     \x20   rust:",
                ));
            }
        }
    }
    if languages.contains(&CartLanguage::C) {
        let c_src_dir = project_dir.join("src/game/c");
        if external_config.is_none() && collect_c_files(&c_src_dir)?.is_empty() {
            return Err(err(format!(
                "no .c files found under {}",
                c_src_dir.display()
            )));
        }
    }
    if languages.contains(&CartLanguage::Cpp) {
        let cpp_src_dir = project_dir.join("src/game/c++");
        if collect_cpp_files(&cpp_src_dir)?.is_empty() {
            return Err(err(format!(
                "no .cpp/.cxx/.cc files found under {}",
                cpp_src_dir.display()
            )));
        }
    }
    if languages.contains(&CartLanguage::Rust) {
        let rust_manifest = project_dir.join("src/game/rust/Cargo.toml");
        if !rust_manifest.exists() {
            return Err(err(format!(
                "language: rust but no Cargo.toml found at {}",
                rust_manifest.display()
            )));
        }
    }
    if let Some(ref cfg) = external_config {
        if collect_external_source_files(project_dir, cfg)?.is_empty() {
            return Err(err(format!(
                "no .{} files found for language {:?} — searched {} and src/lib/*/",
                cfg.extension,
                cfg.language,
                project_dir.join("src/game").join(&cfg.language).display()
            )));
        }
    }

    let src_map = source_map_entries(project_dir, &sdk_root);
    let c_prefix_flags: Vec<String> = src_map
        .iter()
        .filter(|e| e.canonical == "/blyt/cart" || e.canonical == "/blyt/sdk")
        .map(|e| format!("-ffile-prefix-map={}={}", e.local.display(), e.canonical))
        .collect();
    let opt_c_flags: Vec<String> = {
        let mut f = if debug {
            vec![
                "-g".to_string(),
                "-O0".to_string(),
                "-ffile-compilation-dir=/blyt/cart".to_string(),
            ]
        } else {
            vec!["-O2".to_string()]
        };
        f.extend(c_prefix_flags);
        f
    };
    let rust_remap_flags: String = src_map
        .iter()
        .map(|e| format!(" --remap-path-prefix={}={}", e.local.display(), e.canonical))
        .collect();
    let rust_extra_flags: String = if debug {
        format!(" -g -C opt-level=0{rust_remap_flags}")
    } else {
        rust_remap_flags
    };

    let lua_lib_defines: Vec<String> = if is_lua {
        vec![
            "-DBLYT_LUA_I32_F64=1".to_string(),
            "-DLUA_USE_LONGJMP=1".to_string(),
        ]
    } else {
        vec![]
    };

    let needs_c = languages.contains(&CartLanguage::C) || languages.contains(&CartLanguage::Cpp);
    let needs_rust = languages.contains(&CartLanguage::Rust);

    // Read cart config now for task construction (buffers_present, codegen flags).
    let cart_config = crate::config::read_cart_config(project_dir).map_err(err)?;
    let buffers_present = !cart_config.state_buffers.is_empty();

    let variant = variant_str(debug);
    let build_dir = if is_lua {
        project_dir.join("build/game/lua").join(variant)
    } else if let Some(ref cfg) = external_config {
        project_dir
            .join("build/game")
            .join(&cfg.language)
            .join(variant)
    } else {
        project_dir.join("build/game/c").join(variant)
    };
    fs::create_dir_all(&build_dir)?;
    let state_dir = project_dir.join("build/.blyt-tasks").join(variant);

    // Unconditional fast writes (cheap, content-compared so mtime stays stable).
    write_source_map_manifest(&project_dir.join("build"), &src_map, project_dir)?;
    let ld_script = build_dir.join("blyt_cart.ld");
    write_if_changed(
        &ld_script,
        if is_lua {
            HYBRID_LUA_LINKER_SCRIPT
        } else {
            LINKER_SCRIPT
        },
    )?;
    let cart_info_file = build_dir.join("cart.info.bin");
    write_bytes_if_changed(&cart_info_file, &cart_info_bytes(debug, &cart_info))?;
    let entry_stub_src = build_dir.join("_blyt_entry.c");
    write_if_changed(&entry_stub_src, ENTRY_STUB_C)?;
    let interp_src = build_dir.join("_blyt_interp.c");
    write_if_changed(&interp_src, INTERP_STUB_C)?;

    // ---- Build task list -------------------------------------------------------

    let mut tasks: Vec<Box<dyn Task>> = Vec::new();

    // Codegen: one task per output, pushed only when relevant.
    if needs_c && buffers_present {
        tasks.push(Box::new(GenerateCHeaderTask {
            project_dir: project_dir.to_path_buf(),
            build_dir: build_dir.clone(),
        }));
    }
    if needs_rust && buffers_present {
        tasks.push(Box::new(GenerateRustStateTask {
            project_dir: project_dir.to_path_buf(),
            build_dir: build_dir.clone(),
        }));
    }
    if buffers_present {
        tasks.push(Box::new(GenerateLayoutsTask {
            project_dir: project_dir.to_path_buf(),
            build_dir: build_dir.clone(),
        }));
    }
    if is_lua {
        tasks.push(Box::new(GenerateLuaGlueTask {
            project_dir: project_dir.to_path_buf(),
            build_dir: build_dir.clone(),
        }));
    }

    // Compile entry stub and interp stub.
    tasks.push(Box::new(make_c_task(
        project_dir,
        entry_stub_src.clone(),
        build_dir.clone(),
        &clang,
        &sdk_include,
        vec![],
        vec![],
        opt_c_flags.clone(),
        "compile_c",
    )));
    tasks.push(Box::new(make_c_task(
        project_dir,
        interp_src.clone(),
        build_dir.clone(),
        &clang,
        &sdk_include,
        vec![],
        vec![],
        opt_c_flags.clone(),
        "compile_c",
    )));

    // Lua bytecode → data C → compile data C → compile glue C.
    if is_lua {
        let luac = find_luac();
        let lua_files = collect_lua_files(&project_dir.join("src/game/lua"))?;
        let bytecode_path = build_dir.join("bytecode.luac");
        let data_c = build_dir.join("cart_lua_data.c");
        let glue_src = build_dir.join("__blyt_lua_glue.c");

        tasks.push(Box::new(CompileLuaTask {
            luac: luac.clone(),
            lua_files,
            project_dir: project_dir.to_path_buf(),
            output: bytecode_path.clone(),
        }));
        tasks.push(Box::new(GenerateLuaDataTask {
            bytecode_path,
            output_c: data_c.clone(),
        }));
        tasks.push(Box::new(make_c_task(
            project_dir,
            data_c,
            build_dir.clone(),
            &clang,
            &sdk_include,
            vec![],
            vec![],
            vec![],
            "compile_c",
        )));
        // Glue C is produced by GenerateLuaGlueTask; compile it here.
        tasks.push(Box::new(make_c_task(
            project_dir,
            glue_src,
            build_dir.clone(),
            &clang,
            &sdk_include,
            vec![],
            lua_lib_defines.clone(),
            vec![],
            "compile_c",
        )));
    }

    // Discover lib include paths (known from directory structure, no build needed).
    let lib_names = if external_config.is_none() {
        discover_libraries(project_dir)?
    } else {
        Vec::new()
    };
    let mut lib_include_paths: Vec<PathBuf> = Vec::new();
    let mut lib_archives: Vec<PathBuf> = Vec::new();
    for name in &lib_names {
        let src_dir = project_dir.join("src/lib").join(name);
        let lib_build_dir = project_dir.join("build/lib").join(name).join(variant);
        fs::create_dir_all(&lib_build_dir)?;
        let include_path = {
            let with_include = src_dir.join("include");
            if with_include.is_dir() {
                with_include
            } else {
                src_dir.clone()
            }
        };
        lib_include_paths.push(include_path.clone());
        let archive = lib_build_dir.join("lib.a");
        lib_archives.push(archive.clone());

        let c_files = collect_c_files(&src_dir)?;
        let cpp_files = collect_cpp_files(&src_dir)?;
        if c_files.is_empty() && cpp_files.is_empty() {
            return Err(err(format!(
                "library {name}: no source files found under {}",
                src_dir.display()
            )));
        }
        let mut lib_obj_files: Vec<PathBuf> = Vec::new();
        for src in c_files {
            let task = make_c_task(
                project_dir,
                src,
                lib_build_dir.clone(),
                &clang,
                &sdk_include,
                vec![include_path.clone()],
                lua_lib_defines.clone(),
                opt_c_flags.clone(),
                &format!("compile_c_lib_{name}"),
            );
            lib_obj_files.push(task.outputs()[0].clone());
            tasks.push(Box::new(task));
        }
        for src in cpp_files {
            let libcxx_include = sdk_include.join("c++/v1");
            let task = make_cpp_task(
                project_dir,
                src,
                lib_build_dir.clone(),
                &clangpp,
                &sdk_include,
                libcxx_include,
                vec![include_path.clone()],
                opt_c_flags.clone(),
                &format!("compile_cpp_lib_{name}"),
            );
            lib_obj_files.push(task.outputs()[0].clone());
            tasks.push(Box::new(task));
        }
        tasks.push(Box::new(AssembleLibArchiveTask {
            key_str: format!("archive/{name}"),
            ar: ar.clone(),
            obj_files: lib_obj_files,
            output: archive,
        }));
    }

    // Rust libs in src/lib/ (Cargo.toml present).
    if is_lua {
        let cargo = find_cargo();
        let rust_sdk = find_rust_sdk(&sdk_include)?;
        let rust_lib_patches: Vec<(String, PathBuf)> = Vec::new(); // no cross-lib patches for libs
        for (lib_name, lib_path) in discover_rust_libs(project_dir)? {
            let lib_build_dir = project_dir.join("build/lib").join(&lib_name).join(variant);
            fs::create_dir_all(&lib_build_dir)?;
            let output = lib_build_dir.join("lib.a");
            lib_archives.push(output.clone());
            tasks.push(Box::new(CompileRustTask {
                key_str: format!("cargo_lib/{lib_name}"),
                label_str: format!("cargo    src/lib/{lib_name}/"),
                cargo: cargo.clone(),
                manifest: lib_path.join("Cargo.toml"),
                build_dir: lib_build_dir.clone(),
                rust_sdk_path: rust_sdk.clone(),
                rust_lib_patches: rust_lib_patches.clone(),
                extra_rustflags: rust_extra_flags.clone(),
                is_lua: true,
                cart_state_rs: None,
                output,
                source_dir: lib_path,
            }));
        }
    }

    // C/C++ include paths for game code: lib includes + generated cart_state.h.
    let mut c_include_paths: Vec<PathBuf> = lib_include_paths;
    if buffers_present {
        c_include_paths.push(build_dir.join("blyt/c"));
    }

    // Game C sources.
    if languages.contains(&CartLanguage::C) {
        let extra_defines = if is_lua {
            lua_lib_defines.clone()
        } else {
            vec![]
        };
        for src in collect_c_files(&project_dir.join("src/game/c"))? {
            tasks.push(Box::new(make_c_task(
                project_dir,
                src,
                build_dir.clone(),
                &clang,
                &sdk_include,
                c_include_paths.clone(),
                extra_defines.clone(),
                opt_c_flags.clone(),
                "compile_c",
            )));
        }
    }

    // Game C++ sources.
    let libcxx_archive = if languages.contains(&CartLanguage::Cpp) {
        let libcxx_include = sdk_include.join("c++/v1");
        if !libcxx_include.exists() {
            return Err(err(
                "libc++ headers not found — run `cmake --build build --target sdk` \
                 to build C++ support",
            ));
        }
        let libc_a = lib_dir.join("libc++.a");
        if !libc_a.exists() {
            return Err(err(
                "libc++.a not found — run `cmake --build build --target sdk` \
                 to build C++ support",
            ));
        }
        for src in collect_cpp_files(&project_dir.join("src/game/c++"))? {
            tasks.push(Box::new(make_cpp_task(
                project_dir,
                src,
                build_dir.clone(),
                &clangpp,
                &sdk_include,
                libcxx_include.clone(),
                c_include_paths.clone(),
                opt_c_flags.clone(),
                "compile_cpp",
            )));
        }
        let libcxxabi_a = lib_dir.join("libc++abi.a");
        Some((
            libc_a,
            if libcxxabi_a.exists() {
                Some(libcxxabi_a)
            } else {
                None
            },
        ))
    } else {
        None
    };
    if let Some((libc_a, libcxxabi_a)) = libcxx_archive {
        lib_archives.push(libc_a);
        if let Some(a) = libcxxabi_a {
            lib_archives.push(a);
        }
    }

    // Game Rust.
    let rust_archive_path = if languages.contains(&CartLanguage::Rust) {
        let cargo = find_cargo();
        let rust_sdk = find_rust_sdk(&sdk_include)?;
        let rust_manifest = project_dir.join("src/game/rust/Cargo.toml");
        let rust_build_dir = project_dir.join("build/game/rust").join(variant);
        fs::create_dir_all(&rust_build_dir)?;
        let rust_libs = discover_rust_libs(project_dir)?;
        let cart_state_rs = if buffers_present {
            Some(build_dir.join("blyt/rust/cart_state.rs"))
        } else {
            None
        };
        let output = rust_build_dir.join("cart.a");
        tasks.push(Box::new(CompileRustTask {
            key_str: "cargo_game".to_string(),
            label_str: "cargo    src/game/rust/".to_string(),
            cargo,
            manifest: rust_manifest,
            build_dir: rust_build_dir.clone(),
            rust_sdk_path: rust_sdk,
            rust_lib_patches: rust_libs,
            extra_rustflags: rust_extra_flags.clone(),
            is_lua,
            cart_state_rs,
            output: output.clone(),
            source_dir: project_dir.join("src/game/rust"),
        }));
        Some(output)
    } else {
        None
    };

    // External compile.
    let external_output = if let Some(ref cfg) = external_config {
        let sdk_bin = lib_dir.parent().map(|p| p.join("bin")).unwrap_or_else(|| {
            sdk_include
                .parent()
                .map(|p| p.join("bin"))
                .unwrap_or_else(|| PathBuf::from("bin"))
        });
        let safe_name = cfg.language.replace(['/', '\\', ' '], "_");
        let (out_path, _) = match cfg.output_type {
            ExternalOutputType::Object => (build_dir.join(format!("{safe_name}.o")), "@OBJFILE@"),
            ExternalOutputType::Archive => (build_dir.join(format!("{safe_name}.a")), "@LIBFILE@"),
        };
        let src_files = collect_external_source_files(project_dir, cfg)?;
        let is_obj = matches!(cfg.output_type, ExternalOutputType::Object);
        tasks.push(Box::new(CompileExternalTask {
            key_str: format!("external/{}", cfg.language),
            config: ExternalCompileConfig {
                language: cfg.language.clone(),
                extension: cfg.extension.clone(),
                command_template: cfg.command_template.clone(),
                strip_sections: cfg.strip_sections.clone(),
                output_type: if is_obj {
                    ExternalOutputType::Object
                } else {
                    ExternalOutputType::Archive
                },
            },
            project_dir: project_dir.to_path_buf(),
            build_dir: build_dir.clone(),
            sdk_include: sdk_include.clone(),
            sdk_lib: lib_dir.clone(),
            sdk_bin,
            cart_state_include: build_dir.join("blyt/c"),
            objcopy: objcopy.clone(),
            debug,
            src_files,
            output: out_path.clone(),
        }));
        Some((out_path, is_obj))
    } else {
        None
    };

    // Collect final obj_files and lib_archives from tasks (known paths).
    let mut obj_files: Vec<PathBuf> = Vec::new();
    let mut final_lib_archives = lib_archives;

    // Entry/interp stubs.
    obj_files.push(build_dir.join("_blyt_entry.o"));
    obj_files.push(build_dir.join("_blyt_interp.o"));

    // Lua data + glue objects.
    if is_lua {
        obj_files.push(build_dir.join("cart_lua_data.o"));
        obj_files.push(build_dir.join("__blyt_lua_glue.o"));
    }

    // C game objects.
    if languages.contains(&CartLanguage::C) {
        for src in collect_c_files(&project_dir.join("src/game/c"))? {
            let stem = src.file_stem().and_then(OsStr::to_str).unwrap_or("unknown");
            obj_files.push(build_dir.join(format!("{stem}.o")));
        }
    }

    // C++ game objects.
    if languages.contains(&CartLanguage::Cpp) {
        for src in collect_cpp_files(&project_dir.join("src/game/c++"))? {
            let stem = src.file_stem().and_then(OsStr::to_str).unwrap_or("unknown");
            obj_files.push(build_dir.join(format!("{stem}.o")));
        }
    }

    // External output.
    if let Some((path, is_obj)) = external_output {
        if is_obj {
            obj_files.push(path);
        } else {
            final_lib_archives.push(path);
        }
    }

    // Link.
    let raw_elf = build_dir.join("cart.elf");
    tasks.push(Box::new(LinkElfTask {
        clang: clang.clone(),
        obj_files: obj_files.clone(),
        rust_archive: rust_archive_path.clone(),
        lib_archives: final_lib_archives.clone(),
        ld_script: ld_script.clone(),
        lib_dir: lib_dir.clone(),
        output: raw_elf.clone(),
        is_lua,
    }));

    // Finalise.
    let output_path = output
        .map(PathBuf::from)
        .unwrap_or_else(|| default_output(project_dir, &cart_info.id, debug));
    let lua_bytecode_path = if is_lua {
        Some(build_dir.join("bytecode.luac"))
    } else {
        None
    };
    let mut extra_sections: Vec<(String, PathBuf)> = Vec::new();
    if let Some(ref p) = lua_bytecode_path {
        extra_sections.push((".cart.lua".to_string(), p.clone()));
    }
    if buffers_present {
        extra_sections.push((
            ".cart.layouts".to_string(),
            build_dir.join("cart.layouts.bin"),
        ));
    }
    tasks.push(Box::new(AssembleCartTask {
        objcopy: objcopy.clone(),
        raw_elf,
        cart_info_file,
        output: output_path.clone(),
        extra_sections,
        debug,
    }));

    // ---- Execute ---------------------------------------------------------------

    run_tasks(&tasks, &state_dir, force)?;

    write_editor_codegen(project_dir)?;

    println!("built: {}", output_path.display());
    Ok(output_path)
}

/// Build a single library from `src/lib/<lib_name>/` in isolation.
///
/// For Rust libs (Cargo.toml present): runs `cargo build --release` and
/// returns the cargo target directory.  For C/C++ libs: compiles source files
/// and produces `build/lib/<lib_name>/<variant>/lib.a`.
pub fn build_single_lib(
    project_dir: &Path,
    lib_name: &str,
    debug: bool,
    force: bool,
) -> Result<PathBuf, BuildError> {
    let project_dir = &fs::canonicalize(project_dir)?;
    let src_dir = project_dir.join("src/lib").join(lib_name);
    if !src_dir.exists() {
        return Err(err(format!(
            "library {lib_name}: src/lib/{lib_name}/ not found under {}",
            project_dir.display()
        )));
    }

    let variant = variant_str(debug);

    // Rust lib: Cargo.toml present → task-based cargo build.
    let cargo_toml = src_dir.join("Cargo.toml");
    if cargo_toml.exists() {
        let cargo = find_cargo();
        let sdk_include = find_sdk_include()?;
        let rust_sdk = find_rust_sdk(&sdk_include)?;
        let lib_build_dir = project_dir.join("build/lib").join(lib_name).join(variant);
        fs::create_dir_all(&lib_build_dir)?;
        let state_dir = project_dir.join("build/.blyt-tasks").join(variant);
        let output = lib_build_dir.join("lib.a");
        let tasks: Vec<Box<dyn Task>> = vec![Box::new(CompileRustTask {
            key_str: format!("cargo_lib/{lib_name}"),
            label_str: format!("cargo    src/lib/{lib_name}/"),
            cargo,
            manifest: cargo_toml,
            build_dir: lib_build_dir,
            rust_sdk_path: rust_sdk,
            rust_lib_patches: vec![],
            extra_rustflags: String::new(),
            is_lua: false,
            cart_state_rs: None,
            output: output.clone(),
            source_dir: src_dir,
        })];
        run_tasks(&tasks, &state_dir, force)?;
        println!("built: {}", output.display());
        return Ok(output);
    }

    // C/C++ lib.
    let clang = find_clang();
    let clangpp = find_clangpp();
    let ar = find_ar();
    let sdk_include = find_sdk_include()?;
    let lib_build_dir = project_dir.join("build/lib").join(lib_name).join(variant);
    let state_dir = project_dir.join("build/.blyt-tasks").join(variant);

    let c_files = collect_c_files(&src_dir)?;
    let cpp_files = collect_cpp_files(&src_dir)?;
    if c_files.is_empty() && cpp_files.is_empty() {
        return Err(err(format!(
            "library {lib_name}: no source files found under {}",
            src_dir.display()
        )));
    }

    let include_path = {
        let with_include = src_dir.join("include");
        if with_include.is_dir() {
            with_include
        } else {
            src_dir.clone()
        }
    };
    fs::create_dir_all(&lib_build_dir)?;

    let mut tasks: Vec<Box<dyn Task>> = Vec::new();
    let mut obj_files: Vec<PathBuf> = Vec::new();
    for src in c_files {
        let task = make_c_task(
            project_dir,
            src,
            lib_build_dir.clone(),
            &clang,
            &sdk_include,
            vec![include_path.clone()],
            vec![],
            vec![],
            &format!("compile_c_lib_{lib_name}"),
        );
        obj_files.push(task.outputs()[0].clone());
        tasks.push(Box::new(task));
    }
    for src in cpp_files {
        let libcxx_include = sdk_include.join("c++/v1");
        let task = make_cpp_task(
            project_dir,
            src,
            lib_build_dir.clone(),
            &clangpp,
            &sdk_include,
            libcxx_include,
            vec![include_path.clone()],
            vec![],
            &format!("compile_cpp_lib_{lib_name}"),
        );
        obj_files.push(task.outputs()[0].clone());
        tasks.push(Box::new(task));
    }
    let archive = lib_build_dir.join("lib.a");
    tasks.push(Box::new(AssembleLibArchiveTask {
        key_str: format!("archive/{lib_name}"),
        ar,
        obj_files,
        output: archive.clone(),
    }));

    run_tasks(&tasks, &state_dir, force)?;
    println!("built: {}", archive.display());
    Ok(archive)
}

fn default_output(project_dir: &Path, id: &str, debug: bool) -> PathBuf {
    // ADR-0129: debug carts get a `.dbg.blyt` suffix so they never collide with
    // a release `.blyt` and so `blyt debug` / tooling can tell them apart.
    let file = if debug {
        format!("{id}.dbg.blyt")
    } else {
        format!("{id}.blyt")
    };
    project_dir.join("build").join(file)
}

/* -------------------------------------------------------------------------
 * Toolchain discovery
 *
 * Resolution order for clang and llvm-objcopy:
 *   1. $BLYT_CLANG / $BLYT_OBJCOPY environment variables
 *   2. <sdk>/toolchain/bin/  — when running from a built SDK (build/sdk/bin/)
 *   3. System PATH fallback
 * ------------------------------------------------------------------------- */

pub(crate) fn sdk_root_from_exe() -> Option<PathBuf> {
    // Binary is at <sdk>/bin/blyt; SDK root is the parent of bin/.
    std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().and_then(|b| b.parent().map(PathBuf::from)))
}

fn find_clang() -> String {
    if let Ok(c) = std::env::var("BLYT_CLANG") {
        return c;
    }
    for sdk in sdk_candidates() {
        let p = sdk.join("bin/blyt-clang");
        if p.exists() {
            return p.to_string_lossy().into_owned();
        }
    }
    "clang".to_string()
}

fn find_objcopy() -> String {
    if let Ok(o) = std::env::var("BLYT_OBJCOPY") {
        return o;
    }
    for sdk in sdk_candidates() {
        let p = sdk.join("bin/blyt-objcopy");
        if p.exists() {
            return p.to_string_lossy().into_owned();
        }
    }
    "llvm-objcopy".to_string()
}

fn find_ar() -> String {
    if let Ok(a) = std::env::var("BLYT_AR") {
        return a;
    }
    for sdk in sdk_candidates() {
        let p = sdk.join("bin/blyt-llvm-ar");
        if p.exists() {
            return p.to_string_lossy().into_owned();
        }
    }
    "llvm-ar".to_string()
}

/// Iterator over candidate SDK roots, in priority order:
/// 1. $BLYT_SDK_DIR — set by the user or CI
/// 2. sdk_root_from_exe() — when running as build/sdk/bin/blyt
fn sdk_candidates() -> impl Iterator<Item = PathBuf> {
    let from_env = std::env::var("BLYT_SDK_DIR").ok().map(PathBuf::from);
    let from_exe = sdk_root_from_exe();
    from_env.into_iter().chain(from_exe)
}

fn find_clangpp() -> String {
    if let Ok(c) = std::env::var("BLYT_CLANGPP") {
        return c;
    }
    for sdk in sdk_candidates() {
        let p = sdk.join("bin/blyt-clang++");
        if p.exists() {
            return p.to_string_lossy().into_owned();
        }
    }
    "clang++".to_string()
}

fn find_luac() -> String {
    if let Ok(c) = std::env::var("BLYT_LUAC") {
        return c;
    }
    for sdk in sdk_candidates() {
        let p = sdk.join("bin/blyt-luac");
        if p.exists() {
            return p.to_string_lossy().into_owned();
        }
    }
    "luac".to_string()
}

fn collect_lua_files(dir: &Path) -> Result<Vec<PathBuf>, BuildError> {
    if !dir.exists() {
        return Ok(Vec::new());
    }
    let mut files = Vec::new();
    for entry in fs::read_dir(dir)? {
        let entry = entry?;
        let path = entry.path();
        if path.is_file() && path.extension().and_then(OsStr::to_str) == Some("lua") {
            files.push(path);
        }
    }
    files.sort();
    Ok(files)
}

fn generate_lua_data_c(bytecode_path: &Path, output_c: &Path) -> Result<(), BuildError> {
    let bytecode = fs::read(bytecode_path)?;
    let mut src = String::with_capacity(bytecode.len() * 5 + 128);
    src.push_str("/* Generated by blyt build — do not edit. */\n");
    src.push_str("const unsigned char cart_lua_bytecode[] = {\n");
    for (i, b) in bytecode.iter().enumerate() {
        if i % 16 == 0 {
            src.push_str("    ");
        }
        src.push_str(&format!("0x{b:02x},"));
        if i % 16 == 15 {
            src.push('\n');
        }
    }
    if !bytecode.is_empty() && bytecode.len() % 16 != 0 {
        src.push('\n');
    }
    src.push_str("};\n");
    src.push_str(&format!(
        "const unsigned int cart_lua_bytecode_size = {}u;\n",
        bytecode.len()
    ));
    fs::write(output_c, src)?;
    Ok(())
}

/* -------------------------------------------------------------------------
 * SDK include directory
 *
 * Looks for the directory containing blyt.h:
 *   1. $BLYT_SDK_DIR/include (or $BLYT_SDK_DIR if blyt.h is directly inside)
 *   2. <sdk>/include/ — when running from a built SDK (build/sdk/bin/)
 *   3. Ancestors of the running binary
 * ------------------------------------------------------------------------- */

pub(crate) fn find_sdk_include() -> Result<PathBuf, BuildError> {
    if let Ok(sdk) = std::env::var("BLYT_SDK_DIR") {
        let sdk = PathBuf::from(sdk);
        let via_include = sdk.join("include");
        if via_include.join("blyt.h").exists() {
            return Ok(via_include);
        }
        return Err(err(format!(
            "BLYT_SDK_DIR={} does not contain include/blyt.h",
            sdk.display()
        )));
    }

    // SDK layout: <sdk>/include/blyt.h when running from <sdk>/bin/blyt
    if let Some(sdk) = sdk_root_from_exe() {
        let p = sdk.join("include/blyt.h");
        if p.exists() {
            return Ok(sdk.join("include"));
        }
    }

    // Repo layout: walk up from the binary looking for runtime/guest/include/blyt.h
    if let Ok(exe) = std::env::current_exe() {
        for ancestor in exe.ancestors().skip(1) {
            let candidate = ancestor.join("runtime/guest/include").join("blyt.h");
            if candidate.exists() {
                return Ok(ancestor.join("runtime/guest/include"));
            }
        }
    }

    Err(err(
        "cannot find blyt.h — run `cmake --build build --target sdk` \
         and use build/sdk/bin/blyt, or set BLYT_SDK_DIR",
    ))
}

/* -------------------------------------------------------------------------
 * Library directory
 *
 * Looks for the directory containing libblyt32.so:
 *   1. $BLYT_LIB_DIR  (explicit override)
 *   2. $BLYT_SDK_DIR/lib/  — derived from SDK dir
 *   3. <sdk>/lib/  — SDK layout (running from build/sdk/bin/blyt)
 *   4. Walk up from sdk_include looking for build/sdk/lib/ or build/
 * ------------------------------------------------------------------------- */

fn find_lib_dir(sdk_include: &Path) -> Result<PathBuf, BuildError> {
    if let Ok(d) = std::env::var("BLYT_LIB_DIR") {
        let p = PathBuf::from(d);
        if p.join("libblyt32.so").exists() {
            return Ok(p);
        }
    }

    // Derive from BLYT_SDK_DIR: lib/ is always adjacent to include/
    if let Ok(sdk) = std::env::var("BLYT_SDK_DIR") {
        let p = PathBuf::from(sdk).join("lib");
        if p.join("libblyt32.so").exists() {
            return Ok(p);
        }
    }

    // SDK layout: <sdk>/lib/libblyt32.so when running from <sdk>/bin/blyt
    if let Some(sdk) = sdk_root_from_exe() {
        let p = sdk.join("lib");
        if p.join("libblyt32.so").exists() {
            return Ok(p);
        }
    }

    // Repo layout: walk up from sdk_include looking for build/sdk/lib/ or build/
    // sdk_include may be deep inside the repo (e.g. runtime/guest/include),
    // so we walk ancestors rather than assuming a fixed depth.
    let mut dir = sdk_include.to_path_buf();
    while let Some(parent) = dir.parent() {
        dir = parent.to_path_buf();
        for subdir in &["build/sdk/lib", "build"] {
            let candidate = dir.join(subdir);
            if candidate.join("libblyt32.so").exists() {
                return Ok(candidate);
            }
        }
    }

    Err(err(
        "cannot find libblyt32.so — run `cmake --build build --target sdk` \
         to build the blyt SDK, then use build/sdk/bin/blyt",
    ))
}

/* -------------------------------------------------------------------------
 * Rust toolchain discovery
 * ------------------------------------------------------------------------- */

fn find_cargo() -> String {
    if let Ok(c) = std::env::var("BLYT_CARGO") {
        return c;
    }
    "cargo".to_string()
}

/* -------------------------------------------------------------------------
 * Rust SDK crate discovery
 *
 * Finds the `blyt` SDK crate (sdk/rust/blyt/) that game Rust code depends on.
 * Resolution order:
 *   1. $BLYT_RUST_SDK — explicit override
 *   2. <sdk>/rust/blyt/ — SDK install layout (build/sdk/rust/blyt/)
 *   3. Walk up from sdk_include looking for sdk/rust/blyt/ in the repo tree
 * ------------------------------------------------------------------------- */

pub(crate) fn find_rust_sdk(sdk_include: &Path) -> Result<PathBuf, BuildError> {
    if let Ok(p) = std::env::var("BLYT_RUST_SDK") {
        let p = PathBuf::from(p);
        if p.join("Cargo.toml").exists() {
            return Ok(p);
        }
        return Err(err(format!(
            "BLYT_RUST_SDK={} does not contain Cargo.toml",
            p.display()
        )));
    }

    // SDK install layout: <sdk>/rust/blyt/
    if let Some(sdk) = sdk_root_from_exe() {
        let p = sdk.join("rust/blyt");
        if p.join("Cargo.toml").exists() {
            return Ok(p);
        }
    }

    // Repo layout: walk up from sdk_include looking for sdk/rust/blyt/
    let mut dir = sdk_include.to_path_buf();
    while let Some(parent) = dir.parent() {
        dir = parent.to_path_buf();
        let candidate = dir.join("sdk/rust/blyt");
        if candidate.join("Cargo.toml").exists() {
            return Ok(candidate);
        }
    }

    Err(err("cannot find Rust SDK crate (sdk/rust/blyt/) — \
         set BLYT_RUST_SDK to its path, or run \
         `cmake --build build --target sdk` to assemble the SDK"))
}

/* -------------------------------------------------------------------------
 * Rust staticlib build
 *
 * Invokes `cargo build --release` targeting riscv32imafdc-blyt-none-elf
 * (custom JSON target, ADR-0108 + ADR-0132).  The SDK crate is
 * injected via --config so game code declares `blyt = "0.1"` and cargo
 * resolves to the SDK path at build time without hard-coding it.
 *
 * Returns the path to the produced .a file.
 * ------------------------------------------------------------------------- */

/// Custom cart Rust target (Spike U): RV32IMAFDC / ilp32d hard-double ABI.
/// There is no upstream `riscv32imafdc` target, so a target-spec JSON is shipped
/// in-tree and resolved by name via `RUST_TARGET_PATH` (set in `cargo_cart_cmd`).
/// The bare name (not a path) keeps cargo's output dir as `<name>/release/`.
const RUST_TARGET: &str = "riscv32imafdc-blyt-none-elf";

/// The target-spec JSON, compiled into devtool and materialised into each cart's
/// `--target-dir` at build time so cargo can resolve `--target <RUST_TARGET>`.
const RUST_TARGET_SPEC: &str = include_str!("../targets/riscv32imafdc-blyt-none-elf.json");

/// Rust toolchain used to build cart code.  `-Z build-std` is an unstable
/// cargo feature, so cart Rust builds require nightly + the `rust-src`
/// component.  The host devtool still builds on stable; only the cart cargo
/// invocation is pinned here.  Override with `$BLYT_RUST_TOOLCHAIN`.
///
/// Pinned to a dated nightly for reproducible cart builds; keep this in sync
/// with the toolchain CI installs (.github/workflows/ci.yml).
const CART_RUST_TOOLCHAIN: &str = "nightly-2026-06-01";

fn rust_toolchain() -> String {
    std::env::var("BLYT_RUST_TOOLCHAIN").unwrap_or_else(|_| CART_RUST_TOOLCHAIN.to_string())
}

/* -------------------------------------------------------------------------
 * Canonical source-path mapping (issue #46)
 *
 * Each entry maps a machine-local absolute directory to a canonical "/blyt/…"
 * prefix that is embedded in cart debug info (DWARF, Rust panic Location, C
 * __FILE__).  The same set drives the compiler prefix-map flags and the
 * build/source-map.json manifest that debuggers reverse to find sources.
 * ------------------------------------------------------------------------- */

pub(crate) struct SourceMapEntry {
    pub local: PathBuf,
    pub canonical: &'static str,
}

/// Local user home, for deriving the default cargo cache location.
fn home_dir() -> Option<PathBuf> {
    std::env::var_os("HOME").map(PathBuf::from)
}

/// The pinned cart toolchain's rust-src tree
/// (`<sysroot>/lib/rustlib/src/rust`), whence build-std recompiles
/// core/alloc.  Returns None if the sysroot can't be resolved, in which case
/// the /blyt/rust remap is simply omitted (paths stay absolute, still debuggable
/// locally).
fn rust_src_dir() -> Option<PathBuf> {
    let out = Command::new("rustc")
        .env("RUSTUP_TOOLCHAIN", rust_toolchain())
        .args(["--print", "sysroot"])
        .output()
        .ok()?;
    if !out.status.success() {
        return None;
    }
    let sysroot = String::from_utf8(out.stdout).ok()?;
    Some(PathBuf::from(sysroot.trim()).join("lib/rustlib/src/rust"))
}

/// The cargo registry source cache (`$CARGO_HOME/registry/src`, default
/// `~/.cargo/registry/src`), whence crates.io dependency sources are compiled.
fn cargo_registry_src() -> Option<PathBuf> {
    let cargo_home = std::env::var_os("CARGO_HOME")
        .map(PathBuf::from)
        .or_else(|| home_dir().map(|h| h.join(".cargo")))?;
    Some(cargo_home.join("registry/src"))
}

/// The canonical source-path mappings for a cart build, in a deterministic
/// order.  The cart and SDK roots always map; the rust-src and cargo entries
/// map when their local roots can be resolved.  None of these prefixes nest,
/// so prefix-map ordering is immaterial.
pub(crate) fn source_map_entries(project_dir: &Path, sdk_root: &Path) -> Vec<SourceMapEntry> {
    let mut v = vec![
        SourceMapEntry {
            local: project_dir.to_path_buf(),
            canonical: "/blyt/cart",
        },
        SourceMapEntry {
            local: sdk_root.to_path_buf(),
            canonical: "/blyt/sdk",
        },
    ];
    if let Some(rust_src) = rust_src_dir() {
        v.push(SourceMapEntry {
            local: rust_src,
            canonical: "/blyt/rust",
        });
    }
    if let Some(reg) = cargo_registry_src() {
        v.push(SourceMapEntry {
            local: reg,
            canonical: "/blyt/cargo",
        });
    }
    v
}

/// Minimal JSON string escaping for a filesystem path (covers `"` and `\`,
/// which suffice for POSIX paths; control chars are not expected here).
fn json_escape(s: &str) -> String {
    s.replace('\\', "\\\\").replace('"', "\\\"")
}

/// Write `<build_root>/source-map.json`: the authoritative list of
/// {prefix, local} pairs a debug client reverses to resolve canonical cart
/// paths back to local sources (issue #46 §2).  A legacy `/blyt/src → project`
/// entry is appended so carts built before the /blyt/cart rename still resolve.
fn write_source_map_manifest(
    build_root: &Path,
    entries: &[SourceMapEntry],
    project_dir: &Path,
) -> Result<(), BuildError> {
    let mut pairs: Vec<(String, String)> = entries
        .iter()
        .map(|e| (e.canonical.to_string(), e.local.display().to_string()))
        .collect();
    // Legacy alias: /blyt/src meant the project root before the /blyt/cart rename.
    pairs.push(("/blyt/src".to_string(), project_dir.display().to_string()));

    let mut json = String::from("[\n");
    for (i, (prefix, local)) in pairs.iter().enumerate() {
        json.push_str(&format!(
            "  {{ \"prefix\": \"{}\", \"local\": \"{}\" }}",
            json_escape(prefix),
            json_escape(local)
        ));
        json.push_str(if i + 1 < pairs.len() { ",\n" } else { "\n" });
    }
    json.push_str("]\n");

    fs::write(build_root.join("source-map.json"), json)?;
    Ok(())
}

/// Build the RUSTFLAGS for a cart Rust build.
///
/// `relocation-model=pic`: the cart ELF is ET_DYN (PIE), so every object —
/// including `core`/`alloc` rebuilt by build-std — must be position
/// independent.  `panic=abort`: no unwinding runtime; matches the SDK crate.
fn cart_rustflags(extra: &str) -> String {
    // `-Zunstable-options` is required to load the custom target-spec JSON
    // (riscv32imafdc / ilp32d, Spike U); rustc rejects custom targets without it
    // even on nightly.
    format!("-Zunstable-options -C relocation-model=pic -C panic=abort{extra}")
}

/// sccache wrapper for cart rustc invocations, when available.
///
/// The expensive part of a cart build is `-Z build-std` recompiling
/// core/alloc as PIC, which otherwise happens once per cart per checkout.
/// Those units' inputs are machine-global (rust-src under ~/.rustup, fixed
/// RUSTFLAGS), so sccache replays them across carts, checkouts, and git
/// worktrees even though every cart keeps its own private cargo target dir.
///
/// A shared `--target-dir` was tried instead and is unsound: cargo's unit
/// hash collides for same name+version packages at different paths, so a
/// second cart silently reuses the first cart's compiled artifact without
/// ever reading the second cart's sources.
///
/// An explicit `RUSTC_WRAPPER` in the environment wins (cargo inherits it);
/// `BLYT_SCCACHE=<path>` overrides discovery; `BLYT_SCCACHE=off` disables.
fn cart_sccache() -> Option<&'static str> {
    use std::sync::OnceLock;
    static SCCACHE: OnceLock<Option<String>> = OnceLock::new();
    SCCACHE
        .get_or_init(|| {
            if std::env::var_os("RUSTC_WRAPPER").is_some() {
                return None; // cargo inherits the caller's wrapper
            }
            match std::env::var("BLYT_SCCACHE") {
                Ok(v) if v == "off" => None,
                Ok(v) if !v.is_empty() => Some(v),
                _ => Command::new("sccache")
                    .arg("--version")
                    .output()
                    .ok()
                    .filter(|o| o.status.success())
                    .map(|_| "sccache".to_string()),
            }
        })
        .as_deref()
}

/// Configure a `cargo build --release` command for a RISC-V cart Rust crate.
///
/// Pins the nightly toolchain and passes `-Z build-std=core,alloc` so the
/// standard library is recompiled from source as PIC.  Without build-std the
/// prebuilt `core`/`alloc` rlibs carry non-PIC relocations that lld rejects
/// when linking the PIE cart — which breaks any cart that uses `alloc`
/// (`Vec`/`String`/`Box`).  This is the production approach recorded in
/// ADR-0108 and the Spike O results ("invoke cargo build with build-std").
///
/// Callers may append crate-specific args (e.g. `--config` patches) and must
/// set `RUSTFLAGS` via `cart_rustflags`.
fn cargo_cart_cmd(cargo: &str, manifest: &Path, target_dir: &Path) -> Command {
    // Materialise the custom target-spec JSON into the cart's target dir and
    // point RUST_TARGET_PATH at it, so cargo resolves `--target <RUST_TARGET>`
    // by name (Spike U: riscv32imafdc / ilp32d).  Best-effort: if the write
    // fails, cargo surfaces a clear "target not found" error.
    let _ = fs::create_dir_all(target_dir);
    let _ = fs::write(
        target_dir.join(format!("{RUST_TARGET}.json")),
        RUST_TARGET_SPEC,
    );

    let mut cmd = Command::new(cargo);
    cmd.env("RUSTUP_TOOLCHAIN", rust_toolchain())
        .env("RUST_TARGET_PATH", target_dir)
        .args(["build", "--release"])
        .arg("--target")
        .arg(RUST_TARGET)
        .arg("-Z")
        .arg("build-std=core,alloc")
        .arg("--manifest-path")
        .arg(manifest)
        .arg("--target-dir")
        .arg(target_dir);
    if let Some(wrapper) = cart_sccache() {
        cmd.env("RUSTC_WRAPPER", wrapper);
    }
    cmd
}

fn build_rust_archive(
    cargo: &str,
    rust_manifest: &Path,
    build_dir: &Path,
    rust_sdk_path: &Path,
    rust_lib_patches: &[(String, PathBuf)],
    extra_rustflags: &str,
    is_lua: bool,
    cart_state_rs: Option<&Path>,
) -> Result<PathBuf, BuildError> {
    // Inject the SDK crate and any src/lib/ Rust crates via --config patches so
    // the game's Cargo.toml needs only version constraints and cargo resolves to
    // the local source at build time.  TOML dotted-key form:
    //   patch."crates-io".<name>.path = "<abs-path>"
    let mut cmd = cargo_cart_cmd(cargo, rust_manifest, build_dir);
    cmd.arg("--config").arg(format!(
        r#"patch."crates-io".blyt.path = "{}""#,
        rust_sdk_path.display()
    ));

    for (name, path) in rust_lib_patches {
        cmd.arg("--config").arg(format!(
            r#"patch."crates-io".{name}.path = "{}""#,
            path.display()
        ));
    }

    // Enable the lua feature so cart_lua_modules and #[lua_export] are available.
    if is_lua {
        cmd.arg("--features").arg("blyt/lua");
    }

    let mut cargo_cmd = cmd;
    if let Some(rs_path) = cart_state_rs {
        let abs = std::fs::canonicalize(rs_path).unwrap_or_else(|_| rs_path.to_path_buf());
        cargo_cmd.env("BLYT_CART_STATE_RS", abs);
    }
    // Pin the SDK crate's source to a canonical /blyt/sdk/rust/blyt path.  The
    // crate is injected by --config from rust_sdk_path, which is normally under
    // the SDK root (already covered by the /blyt/sdk remap) but can be elsewhere
    // via $BLYT_RUST_SDK or the in-repo sdk/rust/blyt; this explicit remap makes
    // its embedded DWARF path identical regardless of where it physically lives.
    let rust_flags = format!(
        "{extra_rustflags} --remap-path-prefix={}=/blyt/sdk/rust/blyt",
        rust_sdk_path.display()
    );
    let status = cargo_cmd
        .env("RUSTFLAGS", cart_rustflags(&rust_flags))
        .status()
        .map_err(|e| err(format!("failed to run {cargo}: {e}")))?;

    if !status.success() {
        return Err(err("cargo build failed"));
    }

    // Locate the produced .a in <build_dir>/<target>/release/
    let out_dir = build_dir.join(RUST_TARGET).join("release");
    let archive = find_rust_staticlib(&out_dir)?;

    // No compiler_builtins stripping: with build-std those objects are rebuilt
    // from source as PIC, so they no longer carry non-PIC relocations, and they
    // supply the f64 soft-float intrinsics (__divdf3, __muldf3, …) that core's
    // float formatting needs on this hardware-single-float target.  mem* still
    // comes from libblyt32.so (build-std's compiler_builtins omits it by default).
    Ok(archive)
}

fn find_rust_staticlib(dir: &Path) -> Result<PathBuf, BuildError> {
    let entries =
        fs::read_dir(dir).map_err(|e| err(format!("cannot read {}: {e}", dir.display())))?;
    for entry in entries {
        let path = entry?.path();
        let name = path.file_name().and_then(|n| n.to_str()).unwrap_or("");
        if name.starts_with("lib") && name.ends_with(".a") {
            return Ok(path);
        }
    }
    Err(err(format!(
        "cargo build did not produce a .a file in {}",
        dir.display()
    )))
}

/* -------------------------------------------------------------------------
 * Library support (src/lib/<name>/)
 *
 * Libraries are auto-discovered: every direct subdirectory of src/lib/ that
 * contains at least one .c, .cpp, .cxx, or .cc file is treated as a library.
 * Each is compiled to build/lib/<name>/lib.a before any game code is compiled.
 * C++ library files are compiled with -fno-exceptions -fno-rtti and expose
 * their API via extern "C" (ADR-0121: no C++ types at language boundaries).
 * ------------------------------------------------------------------------- */

fn discover_libraries(project_dir: &Path) -> Result<Vec<String>, BuildError> {
    let lib_root = project_dir.join("src/lib");
    if !lib_root.exists() {
        return Ok(Vec::new());
    }
    let mut names = Vec::new();
    for entry in fs::read_dir(&lib_root)? {
        let entry = entry?;
        let path = entry.path();
        // Directories with Cargo.toml are Rust libs; handled by discover_rust_libs.
        if !path.is_dir() || path.join("Cargo.toml").exists() {
            continue;
        }
        let has_sources =
            !collect_c_files(&path)?.is_empty() || !collect_cpp_files(&path)?.is_empty();
        if has_sources {
            if let Some(name) = path.file_name().and_then(|n| n.to_str()) {
                names.push(name.to_string());
            }
        }
    }
    names.sort();
    Ok(names)
}

/// Discover Rust libraries in `src/lib/`: any subdirectory with a `Cargo.toml`
/// is treated as a Rust crate.  The directory name is used as the crate name
/// for `--config` patch injection; the `Cargo.toml` [package] name must match.
pub(crate) fn discover_rust_libs(project_dir: &Path) -> Result<Vec<(String, PathBuf)>, BuildError> {
    let lib_root = project_dir.join("src/lib");
    if !lib_root.exists() {
        return Ok(Vec::new());
    }
    let mut libs = Vec::new();
    for entry in fs::read_dir(&lib_root)? {
        let entry = entry?;
        let path = entry.path();
        if path.is_dir() && path.join("Cargo.toml").exists() {
            if let Some(name) = path.file_name().and_then(|n| n.to_str()) {
                libs.push((name.to_string(), fs::canonicalize(&path)?));
            }
        }
    }
    libs.sort_by(|a, b| a.0.cmp(&b.0));
    Ok(libs)
}

fn run_archive(ar: &str, objs: &[PathBuf], output: &Path) -> Result<(), BuildError> {
    let status = Command::new(ar)
        .arg("crs")
        .arg(output)
        .args(objs)
        .status()
        .map_err(|e| err(format!("failed to run {ar}: {e}")))?;

    if !status.success() {
        return Err(err(format!("archive failed: {}", output.display())));
    }
    Ok(())
}

/* -------------------------------------------------------------------------
 * External compile command — arbitrary language via user-supplied command
 * ------------------------------------------------------------------------- */

fn collect_files_by_extension(
    dir: &Path,
    ext: &str,
    out: &mut Vec<PathBuf>,
) -> Result<(), BuildError> {
    if !dir.exists() {
        return Ok(());
    }
    for entry in fs::read_dir(dir)? {
        let entry = entry?;
        let path = entry.path();
        if path.is_dir() {
            collect_files_by_extension(&path, ext, out)?;
        } else if path.extension().and_then(OsStr::to_str) == Some(ext) {
            out.push(path);
        }
    }
    Ok(())
}

fn collect_external_source_files(
    project_dir: &Path,
    config: &ExternalCompileConfig,
) -> Result<Vec<PathBuf>, BuildError> {
    let ext = config.extension.as_str();
    let mut files = Vec::new();
    collect_files_by_extension(
        &project_dir.join("src/game").join(&config.language),
        ext,
        &mut files,
    )?;
    let lib_root = project_dir.join("src/lib");
    if lib_root.exists() {
        for entry in fs::read_dir(&lib_root)? {
            let entry = entry?;
            let path = entry.path();
            if path.is_dir() {
                collect_files_by_extension(&path, ext, &mut files)?;
            }
        }
    }
    files.sort();
    Ok(files)
}

fn tokenize_command(cmd: &str) -> Vec<String> {
    let mut tokens = Vec::new();
    let mut current = String::new();
    let mut chars = cmd.chars().peekable();
    while let Some(c) = chars.next() {
        match c {
            ' ' | '\t' | '\n' | '\r' => {
                if !current.is_empty() {
                    tokens.push(std::mem::take(&mut current));
                }
            }
            '\'' => {
                for c in chars.by_ref() {
                    if c == '\'' {
                        break;
                    }
                    current.push(c);
                }
            }
            '"' => {
                for c in chars.by_ref() {
                    if c == '"' {
                        break;
                    }
                    current.push(c);
                }
            }
            _ => current.push(c),
        }
    }
    if !current.is_empty() {
        tokens.push(current);
    }
    tokens
}

const KNOWN_PLACEHOLDERS: &[&str] = &[
    "@SRCFILES@",
    "@OBJFILE@",
    "@LIBFILE@",
    "@SDK_INCLUDE@",
    "@SDK_LIB@",
    "@SDK_BIN@",
    "@DEBUG@",
    "@CART_GENERATED_C@",
];

fn validate_compile_command_template(cmd: &str) -> Result<(), BuildError> {
    let has_obj = cmd.contains("@OBJFILE@");
    let has_lib = cmd.contains("@LIBFILE@");
    match (has_obj, has_lib) {
        (true, true) => {
            return Err(err(
                "blyt.build.yaml: compile_command cannot contain both @OBJFILE@ and @LIBFILE@",
            ));
        }
        (false, false) => {
            return Err(err(
                "blyt.build.yaml: compile_command must contain either @OBJFILE@ or @LIBFILE@",
            ));
        }
        _ => {}
    }
    if !cmd.contains("@SRCFILES@") {
        return Err(err(
            "blyt.build.yaml: compile_command must contain @SRCFILES@",
        ));
    }
    // Scan for any @...@ patterns that aren't in the known set.
    let mut rest = cmd;
    while let Some(at) = rest.find('@') {
        rest = &rest[at + 1..];
        if let Some(end) = rest.find('@') {
            let candidate = format!("@{}@", &rest[..end]);
            if !KNOWN_PLACEHOLDERS.contains(&candidate.as_str()) {
                return Err(err(format!(
                    "blyt.build.yaml: compile_command contains unknown placeholder {candidate:?}"
                )));
            }
            rest = &rest[end + 1..];
        }
    }
    Ok(())
}

fn compile_external(
    config: &ExternalCompileConfig,
    project_dir: &Path,
    build_dir: &Path,
    sdk_include: &Path,
    sdk_lib: &Path,
    sdk_bin: &Path,
    cart_state_include: &Path,
    objcopy: &str,
    debug: bool,
) -> Result<PathBuf, BuildError> {
    let src_files = collect_external_source_files(project_dir, config)?;

    // Use the language name as the output filename, with path-unsafe chars replaced.
    let safe_name = config.language.replace(['/', '\\', ' '], "_");
    let (out_path, out_placeholder) = match config.output_type {
        ExternalOutputType::Object => (build_dir.join(format!("{safe_name}.o")), "@OBJFILE@"),
        ExternalOutputType::Archive => (build_dir.join(format!("{safe_name}.a")), "@LIBFILE@"),
    };

    let tokens = tokenize_command(&config.command_template);

    let out_str = out_path.to_string_lossy();
    let inc_str = sdk_include.to_string_lossy();
    let lib_str = sdk_lib.to_string_lossy();
    let bin_str = sdk_bin.to_string_lossy();
    let csi_str = cart_state_include.to_string_lossy();
    let debug_str = if debug { "1" } else { "0" };

    let mut argv: Vec<std::ffi::OsString> = Vec::new();
    for token in &tokens {
        if token == "@SRCFILES@" {
            for f in &src_files {
                argv.push(f.as_os_str().to_os_string());
            }
        } else {
            let expanded = token
                .replace(out_placeholder, &out_str)
                .replace("@SDK_INCLUDE@", &inc_str)
                .replace("@SDK_LIB@", &lib_str)
                .replace("@SDK_BIN@", &bin_str)
                .replace("@CART_GENERATED_C@", &csi_str)
                .replace("@DEBUG@", debug_str);
            argv.push(std::ffi::OsString::from(expanded));
        }
    }

    let status = Command::new(&argv[0])
        .args(&argv[1..])
        .current_dir(project_dir)
        .status()
        .map_err(|e| err(format!("failed to run compile_command {:?}: {e}", argv[0])))?;

    if !status.success() {
        return Err(err(format!(
            "compile_command failed for language {:?}",
            config.language
        )));
    }

    if !out_path.exists() {
        return Err(err(format!(
            "compile_command exited 0 but {out_placeholder} was not created: {}",
            out_path.display()
        )));
    }

    for section in &config.strip_sections {
        let status = Command::new(objcopy)
            .arg("--remove-section")
            .arg(section)
            .arg(&out_path)
            .status()
            .map_err(|e| err(format!("failed to run objcopy for strip_sections: {e}")))?;
        if !status.success() {
            return Err(err(format!("objcopy --remove-section {section:?} failed")));
        }
    }

    Ok(out_path)
}

/* -------------------------------------------------------------------------
 * Source file discovery — all .c files under dir, recursively
 * ------------------------------------------------------------------------- */

fn collect_c_files(dir: &Path) -> Result<Vec<PathBuf>, BuildError> {
    if !dir.exists() {
        return Ok(Vec::new());
    }
    let mut files = Vec::new();
    collect_c_recursive(dir, &mut files)?;
    files.sort();
    Ok(files)
}

fn collect_c_recursive(dir: &Path, out: &mut Vec<PathBuf>) -> Result<(), BuildError> {
    for entry in fs::read_dir(dir)? {
        let entry = entry?;
        let path = entry.path();
        if path.is_dir() {
            collect_c_recursive(&path, out)?;
        } else if path.extension().and_then(OsStr::to_str) == Some("c") {
            out.push(path);
        }
    }
    Ok(())
}

/* -------------------------------------------------------------------------
 * Compilation: one .c → one .o
 * ------------------------------------------------------------------------- */

/// Fixed codegen flags for cart C compilation (target, ABI, IEEE/determinism).
/// Shared by `compile_c` and the `compile_commands.json` emitter so clangd sees
/// exactly the flags the cart is built with.  Excludes `-c`, includes, defines,
/// and the per-variant `-g`/`-O`/prefix-map flags, which callers append.
const C_TARGET_FLAGS: &[&str] = &[
    "--target=riscv32",
    "-march=rv32imafdc",
    "-mabi=ilp32d",
    "-nostdlib",
    "-fno-exceptions",
    "-fpie",
    "-ffunction-sections",
    "-fdata-sections",
    "-ffp-contract=off",
    "-fno-fast-math",
    "-fwrapv",
    "-frounding-math",
    "-fsignaling-nans",
];

/// Fixed codegen flags for cart C++ compilation — `C_TARGET_FLAGS` plus
/// `-fno-rtti` (cart C++ is RTTI-free, ADR-0121).
const CPP_TARGET_FLAGS: &[&str] = &[
    "--target=riscv32",
    "-march=rv32imafdc",
    "-mabi=ilp32d",
    "-nostdlib",
    "-fno-exceptions",
    "-fno-rtti",
    "-fpie",
    "-ffunction-sections",
    "-fdata-sections",
    "-ffp-contract=off",
    "-fno-fast-math",
    "-fwrapv",
    "-frounding-math",
    "-fsignaling-nans",
];

fn compile_c(
    clang: &str,
    src: &Path,
    build_dir: &Path,
    sdk_include: &Path,
    extra_includes: &[&Path],
    extra_defines: &[String],
    debug_flags: &[String],
) -> Result<PathBuf, BuildError> {
    let stem = src.file_stem().and_then(OsStr::to_str).unwrap_or("unknown");
    let obj = build_dir.join(format!("{stem}.o"));

    let mut cmd = Command::new(clang);
    // Determinism flags (ADR-0007) and target/ABI live in C_TARGET_FLAGS, shared
    // with the compile_commands.json emitter.
    cmd.args(C_TARGET_FLAGS)
        .arg("-c")
        .arg("-MD")
        .arg("-MF")
        .arg(build_dir.join(format!("{stem}.d")))
        .arg("-I")
        .arg(sdk_include);

    for inc in extra_includes {
        cmd.arg("-I").arg(inc);
    }
    for def in extra_defines {
        cmd.arg(def);
    }
    for flag in debug_flags {
        cmd.arg(flag);
    }

    let status = cmd
        .arg("-o")
        .arg(&obj)
        .arg(src)
        .status()
        .map_err(|e| err(format!("failed to run {clang}: {e}")))?;

    if !status.success() {
        return Err(err(format!("compilation failed: {}", src.display())));
    }
    Ok(obj)
}

/* -------------------------------------------------------------------------
 * C++ compilation
 *
 * Source files: src/game/c++/ — .cpp, .cxx, .cc extensions.
 * Uses clang++ with -fno-exceptions -fno-rtti (ADR-0121) in addition to the
 * standard determinism flags.  Library headers are added as -isystem to
 * suppress warnings from standard library internals.
 * ------------------------------------------------------------------------- */

fn collect_cpp_files(dir: &Path) -> Result<Vec<PathBuf>, BuildError> {
    if !dir.exists() {
        return Ok(Vec::new());
    }
    let mut files = Vec::new();
    collect_cpp_recursive(dir, &mut files)?;
    files.sort();
    Ok(files)
}

fn collect_cpp_recursive(dir: &Path, out: &mut Vec<PathBuf>) -> Result<(), BuildError> {
    for entry in fs::read_dir(dir)? {
        let entry = entry?;
        let path = entry.path();
        if path.is_dir() {
            collect_cpp_recursive(&path, out)?;
        } else if matches!(
            path.extension().and_then(OsStr::to_str),
            Some("cpp" | "cxx" | "cc")
        ) {
            out.push(path);
        }
    }
    Ok(())
}

fn compile_cpp(
    clangpp: &str,
    src: &Path,
    build_dir: &Path,
    sdk_include: &Path,
    libcxx_include: &Path,
    extra_includes: &[&Path],
    debug_flags: &[String],
) -> Result<PathBuf, BuildError> {
    let stem = src.file_stem().and_then(OsStr::to_str).unwrap_or("unknown");
    let obj = build_dir.join(format!("{stem}.o"));

    let mut cmd = Command::new(clangpp);
    cmd.args(CPP_TARGET_FLAGS)
        .arg("-c")
        .arg("-MD")
        .arg("-MF")
        .arg(build_dir.join(format!("{stem}.d")))
        // Both paths must be -isystem so they are in the same search group; within
        // that group, command-line order applies.  Mixing -I (user) and -isystem
        // (system) puts all -I paths first regardless of position, so libcxx headers
        // would always lose to the musl headers if the musl path uses -I.
        .arg("-isystem")
        .arg(libcxx_include)
        .arg("-isystem")
        .arg(sdk_include);

    for inc in extra_includes {
        cmd.arg("-I").arg(inc);
    }
    for flag in debug_flags {
        cmd.arg(flag);
    }

    let status = cmd
        .arg("-o")
        .arg(&obj)
        .arg(src)
        .status()
        .map_err(|e| err(format!("failed to run {clangpp}: {e}")))?;

    if !status.success() {
        return Err(err(format!("compilation failed: {}", src.display())));
    }
    Ok(obj)
}

/* -------------------------------------------------------------------------
 * compile_commands.json — clangd database (issue #48)
 *
 * Editing cart C/C++ needs clangd to know the cross-compile flags and include
 * paths; without this it parses against the host toolchain and flags every
 * blyt/libc++ include as missing.  We emit one entry per user source mirroring
 * the exact `compile_c`/`compile_cpp` invocation, into <project>/build/, where
 * clangd discovers it automatically.
 * ------------------------------------------------------------------------- */

pub(crate) struct CompileEntry {
    file: PathBuf,
    arguments: Vec<String>,
}

fn path_str(p: &Path) -> String {
    p.display().to_string()
}

/// The exact argument vector `compile_c` runs for `src` (compiler first).
fn c_compile_arguments(
    clang: &str,
    src: &Path,
    build_dir: &Path,
    sdk_include: &Path,
    extra_includes: &[&Path],
    extra_defines: &[String],
    debug_flags: &[String],
) -> CompileEntry {
    let stem = src.file_stem().and_then(OsStr::to_str).unwrap_or("unknown");
    let mut a: Vec<String> = vec![clang.to_string()];
    a.extend(C_TARGET_FLAGS.iter().map(|f| f.to_string()));
    a.push("-c".into());
    a.push("-I".into());
    a.push(path_str(sdk_include));
    for inc in extra_includes {
        a.push("-I".into());
        a.push(path_str(inc));
    }
    a.extend(extra_defines.iter().cloned());
    a.extend(debug_flags.iter().cloned());
    a.push("-o".into());
    a.push(path_str(&build_dir.join(format!("{stem}.o"))));
    a.push(path_str(src));
    CompileEntry {
        file: src.to_path_buf(),
        arguments: a,
    }
}

/// The exact argument vector `compile_cpp` runs for `src` (compiler first).
fn cpp_compile_arguments(
    clangpp: &str,
    src: &Path,
    build_dir: &Path,
    sdk_include: &Path,
    libcxx_include: &Path,
    extra_includes: &[&Path],
    debug_flags: &[String],
) -> CompileEntry {
    let stem = src.file_stem().and_then(OsStr::to_str).unwrap_or("unknown");
    let mut a: Vec<String> = vec![clangpp.to_string()];
    a.extend(CPP_TARGET_FLAGS.iter().map(|f| f.to_string()));
    a.push("-c".into());
    a.push("-isystem".into());
    a.push(path_str(libcxx_include));
    a.push("-isystem".into());
    a.push(path_str(sdk_include));
    for inc in extra_includes {
        a.push("-I".into());
        a.push(path_str(inc));
    }
    a.extend(debug_flags.iter().cloned());
    a.push("-o".into());
    a.push(path_str(&build_dir.join(format!("{stem}.o"))));
    a.push(path_str(src));
    CompileEntry {
        file: src.to_path_buf(),
        arguments: a,
    }
}

/// Discover the clangd compile-command entries for a cart's game C/C++ sources
/// without building (issue #48 item 1: `blyt setup vscode` emits the database).
/// Mirrors `run()`'s include layout — SDK headers, libc++ (C++), the generated
/// cart_state.h dir when state buffers are declared, and src/lib/<name>/include
/// — but omits -g/-O/prefix-map flags (irrelevant to clangd parsing).  The
/// generated-header dir only exists after a build; clangd resolves cart_state.h
/// once the cart has been built once.
pub(crate) fn compile_commands_for(project_dir: &Path) -> Result<Vec<CompileEntry>, BuildError> {
    let c_srcs = collect_c_files(&project_dir.join("src/game/c"))?;
    let cpp_srcs = collect_cpp_files(&project_dir.join("src/game/c++"))?;
    if c_srcs.is_empty() && cpp_srcs.is_empty() {
        return Ok(Vec::new());
    }

    let sdk_include = find_sdk_include()?;
    let is_lua = project_dir.join("src/game/lua").is_dir();
    // Use release dir for clangd (both variants have identical include flags).
    let build_dir = project_dir.join(if is_lua {
        "build/game/lua/release"
    } else {
        "build/game/c/release"
    });

    // Include paths: src/lib/<name>/include (else the lib root) + the generated
    // cart_state.h dir when state buffers are declared.
    let mut includes: Vec<PathBuf> = Vec::new();
    for name in discover_libraries(project_dir)? {
        let lib = project_dir.join("src/lib").join(&name);
        let inc = lib.join("include");
        includes.push(if inc.is_dir() { inc } else { lib });
    }
    let cfg = crate::config::read_cart_config(project_dir).map_err(err)?;
    if !cfg.state_buffers.is_empty() {
        includes.push(build_dir.join("blyt/c"));
    }
    let include_refs: Vec<&Path> = includes.iter().map(PathBuf::as_path).collect();

    // Hybrid Lua+C carts compile the Lua C API with LUA_32BITS; mirror it so
    // clangd parses lua.h the same way.
    let defines: Vec<String> = if is_lua {
        vec!["-DBLYT_LUA_I32_F64=1".into(), "-DLUA_USE_LONGJMP=1".into()]
    } else {
        vec![]
    };

    let mut entries = Vec::new();
    if !c_srcs.is_empty() {
        let clang = find_clang();
        for src in c_srcs {
            entries.push(c_compile_arguments(
                &clang,
                &src,
                &build_dir,
                &sdk_include,
                &include_refs,
                &defines,
                &[],
            ));
        }
    }
    if !cpp_srcs.is_empty() {
        let clangpp = find_clangpp();
        let libcxx_include = sdk_include.join("c++/v1");
        for src in cpp_srcs {
            entries.push(cpp_compile_arguments(
                &clangpp,
                &src,
                &build_dir,
                &sdk_include,
                &libcxx_include,
                &include_refs,
                &[],
            ));
        }
    }
    Ok(entries)
}

/// Serialise compile-command entries to the clangd JSON database format.
fn compile_commands_json(project_dir: &Path, entries: &[CompileEntry]) -> String {
    let dir = path_str(project_dir);
    let mut json = String::from("[\n");
    for (i, e) in entries.iter().enumerate() {
        let args = e
            .arguments
            .iter()
            .map(|a| format!("\"{}\"", json_escape(a)))
            .collect::<Vec<_>>()
            .join(", ");
        json.push_str(&format!(
            "  {{ \"directory\": \"{}\", \"file\": \"{}\", \"arguments\": [{}] }}",
            json_escape(&dir),
            json_escape(&path_str(&e.file)),
            args
        ));
        json.push_str(if i + 1 < entries.len() { ",\n" } else { "\n" });
    }
    json.push_str("]\n");
    json
}

/// Write `content` to `path` only if it differs from what is already there
/// (and create parent dirs).  Returns whether a write happened — lets codegen
/// run on every build/setup without churning unchanged files or their mtimes.
fn write_if_changed(path: &Path, content: &str) -> Result<bool, BuildError> {
    if fs::read_to_string(path)
        .map(|e| e == content)
        .unwrap_or(false)
    {
        return Ok(false);
    }
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    fs::write(path, content)?;
    Ok(true)
}

/// Generate the editor-only artifacts that describe a cart to the IDE
/// (issue #48 item 1), idempotently (content-compared, so re-running on every
/// build or `setup vscode` is free when nothing changed):
///   - `build/blyt/lua/cart_state.lua` — LuaLS `S` proxy decls (config-only).
///   - `build/compile_commands.json`   — clangd database (config + sources +
///     include layout).  Best-effort: a missing SDK only warns, since these are
///     editor conveniences, never build inputs.
pub(crate) fn write_editor_codegen(project_dir: &Path) -> Result<(), BuildError> {
    // LuaLS S-proxy annotations — a pure function of the config manifest, and
    // only useful when the cart has Lua game code.
    let cfg = crate::config::read_cart_config(project_dir).map_err(err)?;
    if project_dir.join("src/game/lua").is_dir() && !cfg.state_buffers.is_empty() {
        let path = project_dir.join("build/blyt/lua/cart_state.lua");
        if write_if_changed(&path, &generate_lua_state_decls(&cfg)?)? {
            println!("wrote:  {}", path.display());
        }
    }

    // clangd compile database — depends on the source set + include layout too.
    match compile_commands_for(project_dir) {
        Ok(entries) if !entries.is_empty() => {
            let path = project_dir.join("build/compile_commands.json");
            let json = compile_commands_json(project_dir, &entries);
            if write_if_changed(&path, &json)? {
                println!("wrote:  {}", path.display());
            }
        }
        Ok(_) => {} // pure-Lua cart — nothing to index
        Err(e) => println!("note: skipped compile_commands.json ({e})"),
    }
    Ok(())
}

/* -------------------------------------------------------------------------
 * Linking: .o files → raw ELF (ET_DYN/PIE, dynamically linked against libblyt32.so)
 *
 * _blyt_interp.c       provides the .interp input section (PT_INTERP =
 *                      /lib/ld-blyt.so.1); lld needs an explicit input section
 *                      to populate PT_INTERP from a custom PHDRS script
 * -pie                 ET_DYN; required on the native path — the c-sky ILP32
 *                      kernel computes AT_PHDR incorrectly for ET_EXEC carts
 * -z,relro -z,now      BIND_NOW + RELRO required by ADR-0112
 * -Bdynamic            override clang's -Bstatic injection for bare-metal riscv
 * -lblyt32             creates DT_NEEDED: libblyt32.so and PLT/GOT entries
 * -lblytcommon         resolves symbols from libblytcommon.so; lld does not
 *                      follow DT_NEEDED chains transitively during link
 * ------------------------------------------------------------------------- */

fn link_cart(
    clang: &str,
    objs: &[PathBuf],
    rust_archive: Option<&Path>,
    lib_archives: &[PathBuf],
    ld_script: &Path,
    lib_dir: &Path,
    output: &Path,
    lua_cart: bool,
) -> Result<(), BuildError> {
    let mut cmd = Command::new(clang);

    // Use the SDK's own lld via absolute path when available.  -fuse-ld=<abs>
    // is accepted by clang unconditionally and is unambiguous regardless of
    // PATH or clang's own-directory lookup order.  The blyt-ld.lld symlink
    // name avoids clashing with any system ld.lld when sdk/bin/ is on PATH
    // or installed into a shared directory.
    //
    // Fallback to -fuse-ld=lld when blyt-ld.lld is not present: outside the
    // SDK (e.g. target/debug/ during development), or on Windows where the
    // binary would be blyt-ld.lld.exe and the install-conflict concern does
    // not apply anyway (no shared /usr/bin convention).
    let fuse_ld = sdk_root_from_exe()
        .map(|sdk| sdk.join("bin").join("blyt-ld.lld"))
        .filter(|p| p.exists())
        .map(|p| format!("-fuse-ld={}", p.display()))
        .unwrap_or_else(|| "-fuse-ld=lld".to_string());
    cmd.args([
        "--target=riscv32",
        "-march=rv32imafdc",
        "-mabi=ilp32d",
        "-nostdlib",
    ])
    .arg(&fuse_ld)
    .args([
        "-Wl,--pic-executable",
        "-Wl,-Bdynamic",
        // PT_INTERP is set via the explicit _blyt_interp.c .interp section.
        // lld does not populate PT_INTERP from --dynamic-linker when a custom
        // PHDRS linker script is in use; an input .interp section is required.
        "-Wl,-z,relro",
        "-Wl,-z,now",
        "-Wl,--build-id=none",
        // Drop unused sections (paired with -ffunction-sections/-fdata-sections
        // on the compiles). Eliminates dead libc++ code (wide-char to_wstring/
        // wcsto*, aligned operator new) that std::string pulls in but never
        // uses. Roots: ENTRY(_blyt_entry); cart_init/update/draw via -u below;
        // the .interp section is SHF_GNU_RETAIN (clang `used`) so it survives.
        "-Wl,--gc-sections",
    ])
    .arg(format!("-T{}", ld_script.display()))
    // Retain the .interp input section (from _blyt_interp.c) under --gc-sections;
    // without this GC drops it and PT_INTERP goes missing (ADR-0112 load check).
    .arg("-Wl,-u,blyt_interp")
    .arg("-o")
    .arg(output);

    // Hybrid Lua+native carts: export all global defined symbols to .dynsym so
    // the WASM host's symtab_lookup can find guest function addresses for
    // trampolines (e.g. add_one, __blyt_fnsym_double).  Pure Lua-only carts
    // have no guest C functions to look up so this flag is unnecessary there.
    let has_native = !objs.is_empty() || rust_archive.is_some() || !lib_archives.is_empty();
    if lua_cart && has_native {
        cmd.arg("-Wl,--export-dynamic");
    }

    for obj in objs {
        cmd.arg(obj);
    }

    // Rust staticlib: force-include the cart lifecycle symbols that libblytcommon.so
    // calls at runtime via PLT (not visible to the static linker as dependencies).
    // -u <sym> marks each symbol as "needed" so lld retains the archive member
    // that defines it (ADR-0073 / spike-o-results: -Wl,-u,<cart_sym>).
    if let Some(archive) = rust_archive {
        if lua_cart {
            // Lua+Rust: each #[lua_export] fn lands in its own CGU; --whole-archive
            // ensures all .lua_regtab entries are extracted even though nothing
            // outside the archive references them by name.  cart_lua_modules is now
            // always provided by the C glue object file, not the Rust archive.
            cmd.arg("-Wl,--whole-archive")
                .arg(archive)
                .arg("-Wl,--no-whole-archive");
        } else {
            cmd.arg("-Wl,-u,blyt_cart_init")
                .arg("-Wl,-u,blyt_cart_update")
                .arg("-Wl,-u,blyt_cart_draw")
                .arg("-Wl,-u,blyt_cart_on_save_state")
                .arg("-Wl,-u,blyt_cart_on_load_state")
                .arg(archive);
        }
    } else if !lua_cart {
        // C/C++ carts: under --gc-sections the lifecycle entry points must be
        // retained as GC roots. They are exported in .dynsym (default
        // visibility), but make it explicit and robust against future
        // visibility changes (mirrors the Rust handling above).
        cmd.arg("-Wl,-u,blyt_cart_init")
            .arg("-Wl,-u,blyt_cart_update")
            .arg("-Wl,-u,blyt_cart_draw");
    }

    // Library archives (src/lib/*/lib.a): linked before -lblyt32 so game code
    // symbols resolve against them first.
    // Lua hybrid carts: --whole-archive so BLYT_LUA_EXPORT_* entries in
    // .lua_regtab/.lua_exports are preserved even when no C game code calls
    // the exported function by name.  Same rationale as the Rust archive above.
    for archive in lib_archives {
        // C++ runtime archives have overlapping symbols — never wrap with
        // --whole-archive or the linker will reject duplicate definitions.
        let is_cpp_rt = archive
            .file_name()
            .map(|n| n == "libc++.a" || n == "libc++abi.a")
            .unwrap_or(false);
        if lua_cart && !is_cpp_rt {
            cmd.arg("-Wl,--whole-archive")
                .arg(archive)
                .arg("-Wl,--no-whole-archive");
        } else {
            cmd.arg(archive);
        }
    }

    // Lua carts: libblyt32lua.so provides blyt_cart_init/update/draw.
    // --no-as-needed forces DT_NEEDED: libblyt32lua.so even though nothing in
    // the cart object files directly calls those symbols.
    if lua_cart {
        cmd.arg("-Wl,--no-as-needed")
            .arg("-L")
            .arg(lib_dir)
            .arg("-lblyt32lua")
            .arg("-Wl,--as-needed");
    }
    // Link against libblyt32.so; the SDK's libblyt32.so absorbs all libblytc
    // sources so lld resolves malloc/string/math directly from libblyt32.so's
    // .dynsym.  libblytc.so is loaded at runtime via libblyt32.so's DT_NEEDED.
    cmd.arg("-L").arg(lib_dir).arg("-lblyt32");

    let status = cmd
        .status()
        .map_err(|e| err(format!("failed to run {clang}: {e}")))?;

    if !status.success() {
        return Err(err("link failed"));
    }
    Ok(())
}

/* -------------------------------------------------------------------------
 * Cart finalisation: inject .cart.info, strip toolchain metadata
 *
 * .riscv.attributes and .comment are toolchain metadata with no runtime use and
 * are stripped from both variants.
 *
 * Release carts (ADR-0129) are additionally fully stripped: DWARF (.debug_*) and
 * the symbol table (.symtab/.strtab) are removed so distributables carry zero
 * debug machinery.  Debug carts keep everything for source-level debugging.
 * ------------------------------------------------------------------------- */

fn finalise_cart(
    objcopy: &str,
    raw_elf: &Path,
    cart_info_file: &Path,
    output: &Path,
    extra_sections: &[(&str, &Path)],
    debug: bool,
) -> Result<(), BuildError> {
    let mut cmd = Command::new(objcopy);
    cmd.arg("--add-section")
        .arg(format!(".cart.info={}", cart_info_file.display()))
        .arg("--set-section-flags")
        .arg(".cart.info=alloc,readonly");

    for (name, path) in extra_sections {
        cmd.arg("--add-section")
            .arg(format!("{name}={}", path.display()))
            .arg("--set-section-flags")
            .arg(format!("{name}=alloc,readonly"));
    }

    cmd.arg("--remove-section=.riscv.attributes")
        .arg("--remove-section=.comment")
        // .lua_regtab is a link-time-only section (cart_lua_modules iterates it);
        // it is not needed at runtime and is not in the cart section allowlist.
        .arg("--remove-section=.lua_regtab");

    if !debug {
        // Full strip for release: drop DWARF + symbol table.
        cmd.arg("--strip-debug")
            .arg("--strip-unneeded")
            .arg("--remove-section=.symtab")
            .arg("--remove-section=.strtab");
    }

    let status = cmd
        .arg(raw_elf)
        .arg(output)
        .status()
        .map_err(|e| err(format!("failed to run {objcopy}: {e}")))?;

    if !status.success() {
        return Err(err("objcopy (finalise) failed"));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::cart_info_generated::blyt::root_as_cart_info;

    fn test_fields() -> InfoFields {
        InfoFields {
            id: "hello".to_string(),
            title: "Hello World".to_string(),
            version: "0.0.1-dev".to_string(),
        }
    }

    // The .cart.info writer (flatbuffers crate) and the runtime reader
    // (flatcc) share one wire format; this round-trips the writer against the
    // matching flatbuffers reader to lock the `debug` field (ADR-0129).
    fn read_debug(bytes: &[u8]) -> bool {
        // Strip the 8-byte "CINF" preamble, then read the FlatBuffer body.
        assert_eq!(&bytes[0..4], b"CINF");
        let info = root_as_cart_info(&bytes[8..]).expect("valid CartInfo");
        info.debug()
    }

    #[test]
    fn cart_info_debug_flag_round_trips() {
        assert!(
            read_debug(&cart_info_bytes(true, &test_fields())),
            "debug cart -> debug=true"
        );
        assert!(
            !read_debug(&cart_info_bytes(false, &test_fields())),
            "release cart -> debug=false"
        );
    }

    #[test]
    fn cart_info_preamble_is_well_formed() {
        let b = cart_info_bytes(false, &test_fields());
        assert_eq!(&b[0..4], b"CINF");
        assert_eq!(&b[4..6], &[0, 0], "format_major = 0");
        assert_eq!(&b[6..8], &[0, 0], "format_minor = 0");
    }

    #[test]
    fn cart_info_id_title_version_round_trip() {
        let b = cart_info_bytes(false, &test_fields());
        let info = root_as_cart_info(&b[8..]).expect("valid CartInfo");
        assert_eq!(info.id(), Some("hello"));
        assert_eq!(info.title(), Some("Hello World"));
        assert_eq!(info.version(), Some("0.0.1-dev"));
    }

    #[test]
    fn cart_id_validation() {
        for ok in ["a", "0", "hello", "hello-world_2", &"a".repeat(63)] {
            assert!(cart_id_valid(ok), "{ok:?} should be valid");
        }
        for bad in [
            "",
            "Hello",
            "hello world",
            "-hello",
            "_hello",
            "héllo",
            &"a".repeat(64),
        ] {
            assert!(!cart_id_valid(bad), "{bad:?} should be invalid");
        }
    }

    // -----------------------------------------------------------------------
    // validate_compile_command_template
    // -----------------------------------------------------------------------

    fn ok_cmd(template: &str) {
        validate_compile_command_template(template)
            .unwrap_or_else(|e| panic!("expected ok for {template:?}, got: {e}"));
    }

    fn err_cmd(template: &str, needle: &str) {
        let e = validate_compile_command_template(template)
            .expect_err(&format!("expected error for {template:?}"))
            .to_string();
        assert!(
            e.contains(needle),
            "error {e:?} should contain {needle:?} (template={template:?})"
        );
    }

    #[test]
    fn compile_command_template_objfile_valid() {
        ok_cmd("compiler -o @OBJFILE@ @SRCFILES@");
    }

    #[test]
    fn compile_command_template_libfile_valid() {
        ok_cmd("compiler -o @LIBFILE@ @SRCFILES@");
    }

    #[test]
    fn compile_command_template_both_output_placeholders_fail() {
        err_cmd(
            "compiler -o @OBJFILE@ @LIBFILE@ @SRCFILES@",
            "cannot contain both @OBJFILE@ and @LIBFILE@",
        );
    }

    #[test]
    fn compile_command_template_no_output_placeholder_fails() {
        err_cmd(
            "compiler @SRCFILES@",
            "must contain either @OBJFILE@ or @LIBFILE@",
        );
    }

    #[test]
    fn compile_command_template_missing_srcfiles_fails() {
        err_cmd("compiler -o @OBJFILE@", "@SRCFILES@");
    }

    #[test]
    fn compile_command_template_unknown_placeholder_fails() {
        err_cmd(
            "compiler -o @OBJFILE@ @SRCFILES@ @UNKNOWN@",
            "unknown placeholder",
        );
    }

    // -----------------------------------------------------------------------
    // tokenize_command
    // -----------------------------------------------------------------------

    #[test]
    fn tokenize_command_basic() {
        assert_eq!(
            tokenize_command("a b  c\td"),
            vec!["a", "b", "c", "d"],
            "whitespace splits into tokens"
        );
    }

    #[test]
    fn tokenize_command_single_quotes() {
        assert_eq!(
            tokenize_command("cmd 'hello world' end"),
            vec!["cmd", "hello world", "end"],
        );
    }

    #[test]
    fn tokenize_command_double_quotes() {
        assert_eq!(
            tokenize_command(r#"cmd "hello world" end"#),
            vec!["cmd", "hello world", "end"],
        );
    }

    #[test]
    fn tokenize_command_empty_and_newlines() {
        assert_eq!(
            tokenize_command("a\nb\r\nc"),
            vec!["a", "b", "c"],
            "newlines are whitespace"
        );
    }

    // -----------------------------------------------------------------------
    // read_cart_languages — external compile path
    // -----------------------------------------------------------------------

    fn write_build_yaml(dir: &std::path::Path, text: &str) {
        fs::write(dir.join("blyt.build.yaml"), text).unwrap();
    }

    #[test]
    fn read_cart_languages_external_objfile() {
        let dir = tempfile::tempdir().unwrap();
        write_build_yaml(
            dir.path(),
            "languages:\n  swift:\n    compile_command: \"swiftc -o @OBJFILE@ @SRCFILES@\"\n",
        );
        let langs = read_cart_languages(dir.path()).unwrap();
        let BuildLanguages::External { builtin, cfg } = langs else {
            panic!("expected External variant");
        };
        assert!(builtin.is_empty(), "expected no builtin languages");
        assert_eq!(cfg.language, "swift");
        assert!(
            matches!(cfg.output_type, ExternalOutputType::Object),
            "expected Object output type"
        );
    }

    #[test]
    fn read_cart_languages_external_libfile() {
        let dir = tempfile::tempdir().unwrap();
        write_build_yaml(
            dir.path(),
            "languages:\n  zig:\n    compile_command: \"zig build-lib -femit-bin=@LIBFILE@ @SRCFILES@\"\n",
        );
        let langs = read_cart_languages(dir.path()).unwrap();
        let BuildLanguages::External { builtin, cfg } = langs else {
            panic!("expected External variant");
        };
        assert!(builtin.is_empty(), "expected no builtin languages");
        assert_eq!(cfg.language, "zig");
        assert!(
            matches!(cfg.output_type, ExternalOutputType::Archive),
            "expected Archive output type"
        );
    }

    #[test]
    fn read_cart_languages_external_with_c() {
        let dir = tempfile::tempdir().unwrap();
        write_build_yaml(
            dir.path(),
            "languages:\n  swift:\n    compile_command: \"swiftc -o @OBJFILE@ @SRCFILES@\"\n    source_extension: .swift\n  c:\n",
        );
        let langs = read_cart_languages(dir.path()).unwrap();
        let BuildLanguages::External { builtin, cfg } = langs else {
            panic!("expected External variant");
        };
        assert_eq!(builtin, BTreeSet::from([CartLanguage::C]));
        assert_eq!(cfg.language, "swift");
        assert_eq!(cfg.extension, "swift");
        assert!(
            matches!(cfg.output_type, ExternalOutputType::Object),
            "expected Object output type"
        );
    }

    #[test]
    fn read_cart_info_rules() {
        let dir = tempfile::tempdir().unwrap();
        let write = |text: &str| fs::write(dir.path().join("blyt.info.yaml"), text).unwrap();

        write("id: hello\ntitle: Hello World\n");
        let f = read_cart_info(dir.path()).unwrap();
        assert_eq!(f.id, "hello");
        assert_eq!(f.title, "Hello World");
        assert_eq!(f.version, "0.0.1-dev", "version defaults when absent");

        write("id: hello\ntitle: Hello\nversion: 1.2.3-rc.1\n");
        assert_eq!(read_cart_info(dir.path()).unwrap().version, "1.2.3-rc.1");

        let expect_err = |text: &str, needle: &str| {
            write(text);
            let e = read_cart_info(dir.path()).unwrap_err().to_string();
            assert!(e.contains(needle), "error {e:?} should mention {needle:?}");
        };
        expect_err("title: Hello\n", "missing required field `id`");
        expect_err("id: hello\n", "missing required field `title`");
        expect_err("id: Hello\ntitle: Hello\n", "invalid `id`");
        expect_err("id: hello\ntitle: \"a\\nb\"\n", "control characters");
        expect_err("id: hello\ntitle: Hello\nversion: not-semver\n", "semver");
        // The retired `name:` field is an unknown field like any other.
        expect_err("name: hello\n", "unknown field");

        fs::remove_file(dir.path().join("blyt.info.yaml")).unwrap();
        let e = read_cart_info(dir.path()).unwrap_err().to_string();
        assert!(e.contains("blyt.info.yaml not found"));
    }
}
