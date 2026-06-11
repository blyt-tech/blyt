use std::collections::{BTreeMap, BTreeSet};
use std::ffi::OsStr;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

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
 * api_version_major/minor stay 0/0 (validated at load); title/author/console are
 * left unset for now — wire them from blyt.info.yaml when that file grows fields.
 * ------------------------------------------------------------------------- */
fn cart_info_bytes(debug: bool) -> Vec<u8> {
    let mut fbb = FlatBufferBuilder::new();
    let info = CartInfo::create(
        &mut fbb,
        &CartInfoArgs {
            api_version_major: 0,
            api_version_minor: 0,
            title: None,
            author: None,
            console: None,
            debug,
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

struct CartStateFiles {
    rust_module: PathBuf,   /* build/blyt/rust/cart_state.rs */
    layouts_bin: PathBuf,   /* build/cart.layouts.bin */
    c_include_dir: PathBuf, /* build/blyt/c/ */
    /// C function body for register_cart_state_S(); empty when no buffers.
    lua_c_snippet: String,
    buffers_present: bool,
}

fn generate_cart_state(
    project_dir: &Path,
    build_dir: &Path,
    cfg: &CartConfig,
) -> Result<CartStateFiles, BuildError> {
    use crate::config::{type_tag_buf_get_suffix, type_tag_c_type, type_tag_rust_type};

    let blyt_dir = build_dir.join("blyt");
    let c_dir = blyt_dir.join("c");
    let rust_dir = blyt_dir.join("rust");
    fs::create_dir_all(&c_dir)?;
    fs::create_dir_all(&rust_dir)?;

    let c_path = c_dir.join("cart_state.h");
    let rs_path = rust_dir.join("cart_state.rs");
    let layouts_bin = build_dir.join("cart.layouts.bin");

    let _ = project_dir; /* reserved for future use */
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

            c_out.push_str(&format!(
                "#define {c_field} ((blyt_field_h)0x{field_h:08X}u) /* {c_type} */\n"
            ));
            rs_out.push_str(&format!(
                "#[allow(dead_code)] pub const {c_field}: BlytFieldH = 0x{field_h:08X}; /* {rs_type} */\n"
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
                crate::config::TYPE_F32 => format!(
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

    let lua_c = format!("{lua_c_proxy_fns}{lua_c_fn}");

    c_out.push_str("#endif /* BLYT_CART_STATE_H */\n");
    rs_out.push_str("/* end of generated constants */\n");

    fs::write(&c_path, &c_out)?;
    fs::write(&rs_path, &rs_out)?;

    /* ---- .cart.layouts FlatBuffer binary ---- */
    let layouts_bytes = cart_layouts_bytes(cfg);
    if !layouts_bytes.is_empty() {
        fs::write(&layouts_bin, &layouts_bytes)?;
    }

    Ok(CartStateFiles {
        rust_module: rs_path,
        layouts_bin,
        c_include_dir: c_dir,
        lua_c_snippet: if buffers_present {
            lua_c
        } else {
            String::new()
        },
        buffers_present,
    })
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

/* -------------------------------------------------------------------------
 * Error type
 * ------------------------------------------------------------------------- */

#[derive(Debug)]
pub struct BuildError(String);

impl std::fmt::Display for BuildError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}", self.0)
    }
}

impl From<std::io::Error> for BuildError {
    fn from(e: std::io::Error) -> Self {
        BuildError(e.to_string())
    }
}

fn err(msg: impl Into<String>) -> BuildError {
    BuildError(msg.into())
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

fn read_cart_languages(project_dir: &Path) -> Result<BTreeSet<CartLanguage>, BuildError> {
    let manifest_path = project_dir.join("blyt.build.yaml");
    if !manifest_path.exists() {
        return Ok(BTreeSet::from([CartLanguage::Lua]));
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
        (Some(lang), None) => Ok(BTreeSet::from([parse_language_str(&lang)?])),
        (None, Some(map)) => {
            if map.is_empty() {
                return Err(err("blyt.build.yaml: `languages:` map is empty"));
            }
            map.keys().map(|k| parse_language_str(k)).collect()
        }
    }
}

/* -------------------------------------------------------------------------
 * Public entry point
 * ------------------------------------------------------------------------- */

pub fn run(project_dir: &Path, output: Option<&Path>, debug: bool) -> Result<PathBuf, BuildError> {
    let info_path = project_dir.join("blyt.info.yaml");
    if !info_path.exists() {
        return Err(err(format!(
            "blyt.info.yaml not found in {} — \
             every blyt cart project must have this file",
            project_dir.display()
        )));
    }

    let clang = find_clang();
    let clangpp = find_clangpp();
    let objcopy = find_objcopy();
    let ar = find_ar();

    let sdk_include = find_sdk_include()?;
    let lib_dir = find_lib_dir(&sdk_include)?;

    let languages = read_cart_languages(project_dir)?;
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
        // When Lua is the only declared language, check that no undeclared
        // native game code exists in the conventional directories.
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
        if collect_c_files(&c_src_dir)?.is_empty() {
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

    // Per-variant optimisation/debug flags, computed once and passed to every
    // compile_c/compile_cpp invocation for user source files (generated stubs
    // like _blyt_entry.c are excluded).  Debug: -O0 -g + path remap for clean
    // stepping.  Release: -O2 (ADR-0129).  Determinism flags (ADR-0007) live in
    // compile_c/compile_cpp and apply to both variants, so -O2 must not drop them.
    let opt_c_flags: Vec<String> = if debug {
        vec![
            "-g".to_string(),
            "-O0".to_string(),
            format!("-ffile-prefix-map={}=/blyt/src", project_dir.display()),
        ]
    } else {
        vec!["-O2".to_string()]
    };
    // -C opt-level=0 mirrors the C path's -O0 so cart Rust *and* the std crates
    // rebuilt by build-std step cleanly line-by-line (no inlining / reordering /
    // <optimized out> locals).  RUSTFLAGS are appended after cargo's release
    // profile flags, so this opt-level wins over the profile's.
    let debug_rust_flags: String = if debug {
        format!(
            " -g -C opt-level=0 --remap-path-prefix={}=/blyt/src",
            project_dir.display()
        )
    } else {
        String::new()
    };

    // Build all libraries before game code.
    let lib_names = discover_libraries(project_dir)?;
    let mut built_libs: Vec<BuiltLib> = Vec::new();
    // Lua carts: pass LUA_32BITS=1 and LUA_USE_LONGJMP=1 so that src/lib/ C
    // code calling the Lua C API uses the same numeric types and error-handling
    // path as the blyt Lua VM compiled with these flags.
    let lua_lib_defines: Vec<String> = if is_lua {
        vec![
            "-DLUA_32BITS=1".to_string(),
            "-DLUA_USE_LONGJMP=1".to_string(),
        ]
    } else {
        vec![]
    };
    for name in &lib_names {
        built_libs.push(build_library(
            &clang,
            &clangpp,
            &ar,
            project_dir,
            name,
            &sdk_include,
            &lua_lib_defines,
            &opt_c_flags,
        )?);
    }
    let lib_include_paths: Vec<PathBuf> =
        built_libs.iter().map(|l| l.include_path.clone()).collect();
    let mut lib_archives: Vec<PathBuf> = built_libs.into_iter().map(|l| l.archive).collect();

    // Lua carts may have standalone Rust libs in src/lib/ (Cargo.toml present).
    // Build each independently; the resulting archives are appended to lib_archives
    // and linked before -lblyt32lua so cart_lua_modules overrides the weak symbol.
    if is_lua {
        let cargo = find_cargo();
        for (lib_name, lib_path) in discover_rust_libs(project_dir)? {
            let lib_build_dir = project_dir.join("build/lib").join(&lib_name);
            fs::create_dir_all(&lib_build_dir)?;
            let status = cargo_cart_cmd(&cargo, &lib_path.join("Cargo.toml"), &lib_build_dir)
                .env("RUSTFLAGS", cart_rustflags(&debug_rust_flags))
                .status()
                .map_err(|e| err(format!("failed to run cargo for Rust lib {lib_name}: {e}")))?;
            if !status.success() {
                return Err(err(format!("cargo build failed for Rust lib {lib_name}")));
            }
            let archive_dir = lib_build_dir.join(RUST_TARGET).join("release");
            let archive = find_rust_staticlib(&archive_dir)?;
            lib_archives.push(archive);
        }
    }

    let build_dir = if is_lua {
        project_dir.join("build/game/lua")
    } else {
        project_dir.join("build/game/c")
    };
    fs::create_dir_all(&build_dir)?;

    let ld_script = build_dir.join("blyt_cart.ld");
    let ld_content = if is_lua {
        HYBRID_LUA_LINKER_SCRIPT
    } else {
        LINKER_SCRIPT
    };
    fs::write(&ld_script, ld_content)?;

    let cart_info_file = build_dir.join("cart.info.bin");
    fs::write(&cart_info_file, cart_info_bytes(debug))?;

    /* Parse blyt.config.yaml and generate state buffer codegen artifacts. */
    let cart_config = crate::config::read_cart_config(project_dir).map_err(|e| err(e))?;
    let cart_state = generate_cart_state(project_dir, &build_dir, &cart_config)?;

    /* Generate the cart entry point stub.  _blyt_entry is the ELF e_entry:
     *   - On native RISC-V hardware: ld.so jumps here after loading
     *     libblyt32.so and libblytcommon.so; it calls blyt_main via PLT.
     *   - In the emulator: rv_create sets PC = e_entry = _blyt_entry;
     *     the PLT entry for blyt_main is resolved by dynlink before rv_run.
     * The load-time security check requires e_entry to be within an
     * executable PT_LOAD segment of the cart, which _blyt_entry satisfies. */
    let entry_stub_src = build_dir.join("_blyt_entry.c");
    fs::write(
        &entry_stub_src,
        "/* Generated by blyt build — do not edit. */\n\
         void blyt_main(void);\n\
         /* blyt_exit is provided by the native libblyt32.so (calls exit_group).\n\
          * On the emulated path the dynamic linker never calls ELF constructors,\n\
          * so blyt_exit is never reached; the emulator halts on ECALL_QUIT. */\n\
         __attribute__((noreturn)) void blyt_exit(int code);\n\
         void _blyt_entry(void) {\n\
             blyt_main();\n\
             blyt_exit(0);\n\
         }\n",
    )?;

    /* Generate the PT_INTERP section.  lld does not populate the .interp
     * phdr from a custom PHDRS linker script via --dynamic-linker; an
     * explicit input section is required. */
    let interp_src = build_dir.join("_blyt_interp.c");
    fs::write(
        &interp_src,
        // External (non-static) symbol so the link can pin it with -Wl,-u to
        // survive --gc-sections; `used` alone (SHF_GNU_RETAIN) is not honoured
        // for the .interp section here, and losing it drops PT_INTERP (ADR-0112).
        "/* Generated by blyt build — do not edit. */\n\
         __attribute__((section(\".interp\"), used))\n\
         const char blyt_interp[] = \"/lib/ld-blyt.so.1\";\n",
    )?;

    // Generated stubs need no lib includes — they only call blyt_main/blyt_exit.
    // No debug flags: these are auto-generated files, not user source.
    let mut obj_files = Vec::new();
    obj_files.push(compile_c(
        &clang,
        &entry_stub_src,
        &build_dir,
        &sdk_include,
        &[],
        &[],
        &[],
    )?);
    obj_files.push(compile_c(
        &clang,
        &interp_src,
        &build_dir,
        &sdk_include,
        &[],
        &[],
        &[],
    )?);

    // Step 1: Lua bytecode — shared for all carts that include Lua.
    if is_lua {
        let luac = find_luac();
        let lua_files = collect_lua_files(&project_dir.join("src/game/lua"))?;

        let bytecode_path = build_dir.join("bytecode.luac");
        let mut luac_cmd = Command::new(&luac);
        luac_cmd.arg("-o").arg(&bytecode_path);
        for f in &lua_files {
            luac_cmd.arg(f);
        }
        let status = luac_cmd
            .status()
            .map_err(|e| err(format!("failed to run {luac}: {e}")))?;
        if !status.success() {
            return Err(err("luac compilation failed"));
        }

        let data_c = build_dir.join("cart_lua_data.c");
        generate_lua_data_c(&bytecode_path, &data_c)?;
        obj_files.push(compile_c(
            &clang,
            &data_c,
            &build_dir,
            &sdk_include,
            &[],
            &[],
            &[],
        )?);
    }

    // Hybrid Lua cart: generate __blyt_lua_glue.c that provides cart_lua_modules.
    // Iterates .lua_regtab (blyt_lua_regtab_entry_t structs placed there by
    // BLYT_LUA_EXPORT_* macros and Rust #[lua_export]).  Generated for all Lua
    // carts regardless of language mix; Rust carts no longer define their own
    // cart_lua_modules.  Module grouping (require("mod").fn()) is implemented here.
    // If state buffers are declared, also registers the "S" constants module
    // (ADR-0059) before the regtab loop.
    if is_lua {
        let glue_src = build_dir.join("__blyt_lua_glue.c");
        let mut glue = String::new();
        glue.push_str("/* Generated by blyt build — do not edit. */\n");
        glue.push_str("#include <blyt.h>\n");
        if cart_state.buffers_present {
            glue.push_str("#include <string.h> /* strcmp for proxy field dispatch */\n");
            glue.push_str(&cart_state.lua_c_snippet);
        }
        glue.push_str(
            "/* Defined by the linker script via PROVIDE; not synthesised at link time. */\n",
        );
        glue.push_str("extern blyt_lua_regtab_entry_t __start_lua_regtab[];\n");
        glue.push_str("extern blyt_lua_regtab_entry_t __stop_lua_regtab[];\n");
        glue.push_str("void cart_lua_modules(lua_State *L) {\n");
        if cart_state.buffers_present {
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
        fs::write(&glue_src, &glue)?;
        obj_files.push(compile_c(
            &clang,
            &glue_src,
            &build_dir,
            &sdk_include,
            &[],
            &lua_lib_defines,
            &[],
        )?);
    }

    // Step 2: Native game code.  Each declared native language is compiled;
    // all communicate at link time via extern "C" ABI.
    // For Lua+Rust: the archive goes into lib_archives so the linker emits
    // -u,cart_lua_modules (not -u,blyt_cart_init — those come from libblyt32lua.so).
    // For a pure Rust cart (no Lua): rust_archive carries lifecycle symbols.
    /* Include dirs for C/C++: lib includes + generated cart_state.h dir */
    let mut c_include_paths: Vec<PathBuf> = lib_include_paths.clone();
    if cart_state.buffers_present {
        c_include_paths.push(cart_state.c_include_dir.clone());
    }
    let c_include_refs: Vec<&Path> = c_include_paths.iter().map(PathBuf::as_path).collect();

    if languages.contains(&CartLanguage::C) {
        let extra_defines: &[String] = if is_lua { &lua_lib_defines } else { &[] };
        for src in collect_c_files(&project_dir.join("src/game/c"))? {
            obj_files.push(compile_c(
                &clang,
                &src,
                &build_dir,
                &sdk_include,
                &c_include_refs,
                extra_defines,
                &opt_c_flags,
            )?);
        }
    }
    if languages.contains(&CartLanguage::Cpp) {
        // libc++ headers live at <sdk>/include/c++/v1/ (put there by cmake sdk step)
        let libcxx_include = sdk_include.join("c++/v1");
        if !libcxx_include.exists() {
            return Err(err(
                "libc++ headers not found — run `cmake --build build --target sdk` \
                 to build C++ support",
            ));
        }
        // Add libc++ static archives to the link step (before -lblyt32).
        // libc++abi is optional — link it only when present.
        let libc_a = lib_dir.join("libc++.a");
        if !libc_a.exists() {
            return Err(err(
                "libc++.a not found — run `cmake --build build --target sdk` \
                 to build C++ support",
            ));
        }
        lib_archives.push(libc_a);
        let libcxxabi_a = lib_dir.join("libc++abi.a");
        if libcxxabi_a.exists() {
            lib_archives.push(libcxxabi_a);
        }
        for src in collect_cpp_files(&project_dir.join("src/game/c++"))? {
            obj_files.push(compile_cpp(
                &clangpp,
                &src,
                &build_dir,
                &sdk_include,
                &libcxx_include,
                &c_include_refs,
                &opt_c_flags,
            )?);
        }
    }
    let rust_archive = if languages.contains(&CartLanguage::Rust) {
        let cargo = find_cargo();
        let rust_sdk = find_rust_sdk(&sdk_include)?;
        let rust_manifest = project_dir.join("src/game/rust/Cargo.toml");
        let rust_build_dir = project_dir.join("build/game/rust");
        fs::create_dir_all(&rust_build_dir)?;
        let rust_libs = discover_rust_libs(project_dir)?;
        let cart_state_rs = if cart_state.buffers_present {
            Some(cart_state.rust_module.as_path())
        } else {
            None
        };
        let archive = build_rust_archive(
            &cargo,
            &rust_manifest,
            &rust_build_dir,
            &rust_sdk,
            &rust_libs,
            &debug_rust_flags,
            is_lua,
            cart_state_rs,
        )?;
        Some(archive)
    } else {
        None
    };

    let raw_elf = build_dir.join("cart.elf");
    link_cart(
        &clang,
        &obj_files,
        rust_archive.as_deref(),
        &lib_archives,
        &ld_script,
        &lib_dir,
        &raw_elf,
        is_lua,
    )?;

    let output_path = output
        .map(PathBuf::from)
        .unwrap_or_else(|| default_output(project_dir, debug));

    let lua_bytecode_path = if is_lua {
        Some(build_dir.join("bytecode.luac"))
    } else {
        None
    };

    /* Build extra_sections list: .cart.lua (Lua only) + .cart.layouts (if present). */
    let mut extra_sections: Vec<(&str, &std::path::Path)> = Vec::new();
    if let Some(ref bytecode) = lua_bytecode_path {
        extra_sections.push((".cart.lua", bytecode.as_path()));
    }
    if cart_state.buffers_present {
        extra_sections.push((".cart.layouts", cart_state.layouts_bin.as_path()));
    }
    finalise_cart(
        &objcopy,
        &raw_elf,
        &cart_info_file,
        &output_path,
        &extra_sections,
        debug,
    )?;

    println!("built: {}", output_path.display());
    Ok(output_path)
}

/// Build a single library from `src/lib/<lib_name>/` in isolation.
///
/// For Rust libs (Cargo.toml present): runs `cargo build --release` and
/// returns the cargo target directory.  For C/C++ libs: compiles source files
/// and produces `build/lib/<lib_name>/lib.a`.
///
/// Useful for checking a library compiles or forcing a specific build order
/// without building the full cart.
pub fn build_single_lib(project_dir: &Path, lib_name: &str) -> Result<PathBuf, BuildError> {
    let src_dir = project_dir.join("src/lib").join(lib_name);
    if !src_dir.exists() {
        return Err(err(format!(
            "library {lib_name}: src/lib/{lib_name}/ not found under {}",
            project_dir.display()
        )));
    }

    // Rust lib: Cargo.toml present → let cargo handle it.
    let cargo_toml = src_dir.join("Cargo.toml");
    if cargo_toml.exists() {
        let cargo = find_cargo();
        let build_dir = project_dir.join("build/lib").join(lib_name);
        fs::create_dir_all(&build_dir)?;
        let status = cargo_cart_cmd(&cargo, &cargo_toml, &build_dir)
            .env("RUSTFLAGS", cart_rustflags(""))
            .status()
            .map_err(|e| err(format!("failed to run {cargo}: {e}")))?;
        if !status.success() {
            return Err(err(format!("cargo build failed for lib {lib_name}")));
        }
        println!("built: {}", build_dir.display());
        return Ok(build_dir);
    }

    // C/C++ lib.
    let clang = find_clang();
    let clangpp = find_clangpp();
    let ar = find_ar();
    let sdk_include = find_sdk_include()?;
    let built = build_library(
        &clang,
        &clangpp,
        &ar,
        project_dir,
        lib_name,
        &sdk_include,
        &[],
        &[],
    )?;
    println!("built: {}", built.archive.display());
    Ok(built.archive)
}

fn default_output(project_dir: &Path, debug: bool) -> PathBuf {
    let name = project_dir
        .file_name()
        .and_then(OsStr::to_str)
        .unwrap_or("cart");
    // ADR-0129: debug carts get a `.dbg.blyt` suffix so they never collide with
    // a release `.blyt` and so `blyt debug` / tooling can tell them apart.
    let file = if debug {
        format!("{name}.dbg.blyt")
    } else {
        format!("{name}.blyt")
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
    // SDK layout: use the blyt-prefixed clang symlink from bin/
    if let Some(sdk) = sdk_root_from_exe() {
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
    // SDK layout: use the blyt-prefixed objcopy symlink from bin/
    if let Some(sdk) = sdk_root_from_exe() {
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
    if let Some(sdk) = sdk_root_from_exe() {
        let p = sdk.join("bin/blyt-llvm-ar");
        if p.exists() {
            return p.to_string_lossy().into_owned();
        }
    }
    "llvm-ar".to_string()
}

fn find_clangpp() -> String {
    if let Ok(c) = std::env::var("BLYT_CLANGPP") {
        return c;
    }
    if let Some(sdk) = sdk_root_from_exe() {
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
    if let Some(sdk) = sdk_root_from_exe() {
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

fn find_sdk_include() -> Result<PathBuf, BuildError> {
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

fn find_rust_sdk(sdk_include: &Path) -> Result<PathBuf, BuildError> {
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
 * Invokes `cargo build --release` targeting riscv32imafc-unknown-none-elf
 * (ADR-0108, spike-o-results: corrected target string).  The SDK crate is
 * injected via --config so game code declares `blyt = "0.1"` and cargo
 * resolves to the SDK path at build time without hard-coding it.
 *
 * Returns the path to the produced .a file.
 * ------------------------------------------------------------------------- */

const RUST_TARGET: &str = "riscv32imafc-unknown-none-elf";

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

/// Build the RUSTFLAGS for a cart Rust build.
///
/// `relocation-model=pic`: the cart ELF is ET_DYN (PIE), so every object —
/// including `core`/`alloc` rebuilt by build-std — must be position
/// independent.  `panic=abort`: no unwinding runtime; matches the SDK crate.
fn cart_rustflags(extra: &str) -> String {
    format!("-C relocation-model=pic -C panic=abort{extra}")
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
    let mut cmd = Command::new(cargo);
    cmd.env("RUSTUP_TOOLCHAIN", rust_toolchain())
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
    let status = cargo_cmd
        .env("RUSTFLAGS", cart_rustflags(extra_rustflags))
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

struct BuiltLib {
    archive: PathBuf,
    include_path: PathBuf,
}

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
fn discover_rust_libs(project_dir: &Path) -> Result<Vec<(String, PathBuf)>, BuildError> {
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

fn build_library(
    clang: &str,
    clangpp: &str,
    ar: &str,
    project_dir: &Path,
    lib_name: &str,
    sdk_include: &Path,
    extra_defines: &[String],
    debug_flags: &[String],
) -> Result<BuiltLib, BuildError> {
    let src_dir = project_dir.join("src/lib").join(lib_name);
    let build_dir = project_dir.join("build/lib").join(lib_name);
    fs::create_dir_all(&build_dir)?;

    let include_path = {
        let with_include = src_dir.join("include");
        if with_include.is_dir() {
            with_include
        } else {
            src_dir.clone()
        }
    };

    let c_files = collect_c_files(&src_dir)?;
    let cpp_files = collect_cpp_files(&src_dir)?;

    if c_files.is_empty() && cpp_files.is_empty() {
        return Err(err(format!(
            "library {lib_name}: no source files found under {}",
            src_dir.display()
        )));
    }

    let mut obj_files = Vec::new();
    for src in &c_files {
        obj_files.push(compile_c(
            clang,
            src,
            &build_dir,
            sdk_include,
            &[include_path.as_path()],
            extra_defines,
            debug_flags,
        )?);
    }

    if !cpp_files.is_empty() {
        // libc++ headers are used as -isystem when available; the library may
        // or may not use them depending on whether it includes standard headers.
        let libcxx_include = sdk_include.join("c++/v1");
        for src in &cpp_files {
            obj_files.push(compile_cpp(
                clangpp,
                src,
                &build_dir,
                sdk_include,
                &libcxx_include,
                &[include_path.as_path()],
                debug_flags,
            )?);
        }
    }

    let archive = build_dir.join("lib.a");
    run_archive(ar, &obj_files, &archive)?;

    Ok(BuiltLib {
        archive,
        include_path,
    })
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
    cmd.args([
        "--target=riscv32",
        "-march=rv32imafc",
        "-mabi=ilp32f",
        "-nostdlib",
        "-fno-exceptions",
        "-fpie",
        // Per-function/data sections so the link's --gc-sections can drop unused
        // code (notably dead libc++ std-lib code pulled in transitively).
        "-ffunction-sections",
        "-fdata-sections",
        // Determinism flags (ADR-0007): IEEE 754 strict mode.
        // -ffp-contract=off  — no implicit FMA fusion (changes rounding)
        // -fno-fast-math     — no IEEE-breaking optimisations
        // -fwrapv            — signed integer overflow wraps (no UB)
        // -frounding-math    — compiler must not assume RNE is fixed
        // -fsignaling-nans   — NaN operations may signal; no elision
        "-ffp-contract=off",
        "-fno-fast-math",
        "-fwrapv",
        "-frounding-math",
        "-fsignaling-nans",
        "-c",
    ])
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
    cmd.args([
        "--target=riscv32",
        "-march=rv32imafc",
        "-mabi=ilp32f",
        "-nostdlib",
        "-fno-exceptions",
        "-fno-rtti",
        "-fpie",
        // Per-function/data sections so the link's --gc-sections can drop the
        // unused parts of libc++ pulled in transitively by std::string etc.
        "-ffunction-sections",
        "-fdata-sections",
        // Determinism flags (ADR-0007)
        "-ffp-contract=off",
        "-fno-fast-math",
        "-fwrapv",
        "-frounding-math",
        "-fsignaling-nans",
        "-c",
    ])
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
        "-march=rv32imafc",
        "-mabi=ilp32f",
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
            read_debug(&cart_info_bytes(true)),
            "debug cart -> debug=true"
        );
        assert!(
            !read_debug(&cart_info_bytes(false)),
            "release cart -> debug=false"
        );
    }

    #[test]
    fn cart_info_preamble_is_well_formed() {
        let b = cart_info_bytes(false);
        assert_eq!(&b[0..4], b"CINF");
        assert_eq!(&b[4..6], &[0, 0], "format_major = 0");
        assert_eq!(&b[6..8], &[0, 0], "format_minor = 0");
    }
}
