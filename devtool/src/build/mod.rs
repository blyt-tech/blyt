mod assets;
mod c;
mod colors;
mod cpp;
mod external;
mod handle;
mod lua;
mod palette;
mod persistent;
mod resource_pack;
mod rust;

pub(crate) use rust::{discover_rust_libs, find_rust_sdk};

use std::collections::{BTreeMap, BTreeSet};
use std::ffi::OsStr;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

use crate::engine::{BuildError, Task, TaskInput, build_err, run_tasks};

use crate::cart_config_generated::blyt::{CartConfig as FbCartConfig, CartConfigArgs};
use crate::cart_info_generated::blyt::{CartInfo, CartInfoArgs};
use crate::cart_layouts_generated::blyt::{
    BufferDecl, BufferDeclArgs, CartLayouts, CartLayoutsArgs, FieldDecl, FieldDeclArgs, RecordDecl,
    RecordDeclArgs,
};
use crate::config::{CartConfig, FlatField, flatten_record};
use flatbuffers::FlatBufferBuilder;

/* -------------------------------------------------------------------------
 * Shared helpers
 * ------------------------------------------------------------------------- */

fn err(msg: impl Into<String>) -> BuildError {
    build_err(msg)
}

fn variant_str(debug: bool) -> &'static str {
    if debug { "debug" } else { "release" }
}

fn task_state_dir(project_dir: &Path, variant: &str) -> PathBuf {
    project_dir.join("build/.blyt-tasks").join(variant)
}

fn config_file_input(project_dir: &Path) -> Vec<TaskInput> {
    let p = project_dir.join("blyt.config.yaml");
    if p.exists() {
        vec![TaskInput::File(p)]
    } else {
        vec![]
    }
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

fn path_str(p: &Path) -> String {
    p.display().to_string()
}

pub(crate) fn sdk_root_from_exe() -> Option<PathBuf> {
    std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().and_then(|b| b.parent().map(PathBuf::from)))
}

fn sdk_candidates() -> impl Iterator<Item = PathBuf> {
    let from_env = std::env::var("BLYT_SDK_DIR").ok().map(PathBuf::from);
    let from_exe = sdk_root_from_exe();
    from_env.into_iter().chain(from_exe)
}

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

    if let Some(sdk) = sdk_root_from_exe() {
        let p = sdk.join("include/blyt.h");
        if p.exists() {
            return Ok(sdk.join("include"));
        }
    }

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

pub(crate) fn json_escape(s: &str) -> String {
    s.replace('\\', "\\\\").replace('"', "\\\"")
}

pub(crate) struct CompileEntry {
    pub(crate) file: PathBuf,
    pub(crate) arguments: Vec<String>,
}

pub(crate) struct SourceMapEntry {
    pub local: PathBuf,
    pub canonical: &'static str,
}

/* -------------------------------------------------------------------------
 * .cart.info section data (ADR-0073, ADR-0129)
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
 * .cart.config section data (ADR-0073, ADR-0125)
 *
 * Runtime-consumed cart configuration. Emitted for every cart so the host can
 * read save_version (and, in future, fps) from a single well-known section.
 * ------------------------------------------------------------------------- */

/// Resolve `palettes.default` to the encoded resource handle stored in
/// `.cart.config` (issues #201/#214). The name resolves against two namespaces,
/// built-in-first (so `default: vga` is unambiguous forever):
///   - a built-in name (aurora/vga/ega/cga) → a `PROV_RUNTIME` handle;
///   - a palette-file asset's canonical name → a `PROV_CART` handle.
/// `default:` unset yields 0 (the runtime default, aurora). This also enforces
/// the reserved-name rule — a palette asset may not canonicalize to a built-in
/// name — over *all* palette assets, independent of the `default:` selection.
/// Requires the asset scan to have run, so callers invoke it *after*
/// `scan_assets` (the reason `.cart.config` emission moved past the scan).
fn resolve_default_palette(
    cfg: &CartConfig,
    assets: &[assets::AssetInfo],
) -> Result<u32, BuildError> {
    // Reserved-name check: no palette asset may shadow a built-in name.
    for a in assets {
        if a.resource_type == assets::ResourceType::Palette
            && crate::config::builtin_palette_id(&a.resource_name).is_some()
        {
            return Err(build_err(format!(
                "palette asset canonical name {:?} collides with a reserved built-in \
                 palette name (aurora/vga/ega/cga); rename the file",
                a.resource_name
            )));
        }
    }

    let Some(name) = cfg.palettes.default.as_deref() else {
        return Ok(0);
    };
    // Built-in first (reserved), so an asset can never shadow `default: vga`.
    if let Some(id) = crate::config::builtin_palette_id(name) {
        return Ok(handle::resource_encode_runtime(id));
    }
    // Otherwise it must name a palette-file asset.
    match assets.iter().find(|a| a.resource_name == name) {
        Some(a) if a.resource_type == assets::ResourceType::Palette => {
            Ok(handle::resource_encode_cart(a.id))
        }
        Some(_) => Err(build_err(format!(
            "blyt.config.yaml: palettes.default {name:?} names a non-palette asset; \
             it must be a built-in (aurora/vga/ega/cga) or a palette-file asset name"
        ))),
        None => Err(build_err(format!(
            "blyt.config.yaml: unknown palette {name:?} (expected a built-in \
             aurora/vga/ega/cga, or a palette-file asset name)"
        ))),
    }
}

fn cart_config_bytes(cfg: &CartConfig, default_palette: u32) -> Vec<u8> {
    // `default_palette` is the pre-resolved encoded handle (issues #201/#214)
    // from `resolve_default_palette`; 0 (unset) means the runtime default
    // (aurora).
    let mut fbb = FlatBufferBuilder::new();
    let config = FbCartConfig::create(
        &mut fbb,
        &CartConfigArgs {
            fps: cfg.fps,
            save_version: cfg.save_version,
            default_palette,
        },
    );
    fbb.finish(config, None);
    let body = fbb.finished_data();

    let mut out = Vec::with_capacity(8 + body.len());
    out.extend_from_slice(b"CCFG");
    out.extend_from_slice(&0u16.to_le_bytes()); // format_major
    out.extend_from_slice(&0u16.to_le_bytes()); // format_minor
    out.extend_from_slice(body);
    out
}

/* -------------------------------------------------------------------------
 * .cart.layouts section data (ADR-0009, ADR-0073)
 * ------------------------------------------------------------------------- */

fn cart_layouts_bytes(cfg: &CartConfig) -> Vec<u8> {
    use crate::config::compute_schema_hash;

    if cfg.state_buffers.is_empty() {
        return Vec::new();
    }

    let schema_hash = compute_schema_hash(cfg);
    let mut fbb = FlatBufferBuilder::new();

    let mut record_fbs: Vec<flatbuffers::WIPOffset<RecordDecl<'_>>> = Vec::new();
    for (rec_name, rec) in &cfg.records {
        let name_off = fbb.create_string(rec_name);
        let mut visiting = Vec::new();
        let flat = match flatten_record(rec_name, &cfg.records, &mut visiting) {
            Ok(f) => f,
            Err(_) => continue,
        };
        let _ = rec;
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

    let mut buf_fbs: Vec<flatbuffers::WIPOffset<BufferDecl<'_>>> = Vec::new();
    for (buf_name, buf_decl) in &cfg.state_buffers {
        let name_off = fbb.create_string(buf_name);
        let rec_off = fbb.create_string(&buf_decl.record);
        buf_fbs.push(BufferDecl::create(
            &mut fbb,
            &BufferDeclArgs {
                name: Some(name_off),
                record_name: Some(rec_off),
                count: buf_decl.count,
            },
        ));
    }
    let bufs_vec = fbb.create_vector(&buf_fbs);

    let layouts = CartLayouts::create(
        &mut fbb,
        &CartLayoutsArgs {
            schema_hash,
            records: Some(records_vec),
            buffers: Some(bufs_vec),
        },
    );
    fbb.finish(layouts, None);
    let body = fbb.finished_data();

    let mut out = Vec::with_capacity(8 + body.len());
    out.extend_from_slice(b"CLAY");
    out.extend_from_slice(&0u16.to_le_bytes());
    out.extend_from_slice(&0u16.to_le_bytes());
    out.extend_from_slice(body);
    out
}

/* -------------------------------------------------------------------------
 * Linker scripts
 * ------------------------------------------------------------------------- */

const LINKER_SCRIPT: &str = "ENTRY(_blyt_entry)
";

const HYBRID_LUA_LINKER_SCRIPT: &str = "ENTRY(_blyt_entry)

SECTIONS {
    . = SIZEOF_HEADERS;
    .interp : { *(.interp) }
    .hash : { *(.hash) }
    .gnu.hash : { *(.gnu.hash) }
    .dynsym : { *(.dynsym) }
    .dynstr : { *(.dynstr) }
    .rela.dyn : { *(.rela.dyn) }
    .rela.plt : { *(.rela.plt) }
    .rodata : { *(.rodata .rodata.*) }
    .lua_exports : { KEEP(*(.lua_exports)) }
    . = ALIGN(0x1000);
    .plt : { *(.plt) }
    .text : { *(.text .text.*) }
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

const ENTRY_STUB_C: &str = "/* Generated by blyt build — do not edit. */\n\
     void blyt_main(void);\n\
     /* blyt_exit: resolved over the cart's DT_NEEDED chain (libblytcommon on\n\
      * the native path) and calls exit_group; on the emulated path ECALL_QUIT\n\
      * halts the emulator first so blyt_exit is never reached. */\n\
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

/* -------------------------------------------------------------------------
 * Codegen: cart state header / Rust module / layouts / Lua glue
 * ------------------------------------------------------------------------- */

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

    let mut c_out = String::new();
    c_out.push_str("/* Auto-generated by blyt build — do not edit. */\n");
    c_out.push_str("#ifndef BLYT_CART_STATE_H\n");
    c_out.push_str("#define BLYT_CART_STATE_H\n");
    c_out.push_str("#include <blyt.h>\n\n");

    let mut rs_out = String::new();
    rs_out.push_str("/* Auto-generated by blyt build — do not edit. */\n");
    rs_out.push_str("use blyt::{BlytBufferH, BlytFieldH};\n\n");

    let mut lua_c_proxy_fns = String::new();
    let mut lua_c_fn = String::new();
    lua_c_fn.push_str("static void register_cart_state_S(lua_State *L) {\n");
    lua_c_fn.push_str("    lua_newtable(L);\n");
    let mut lua_c_proxy_setup = String::new();

    let mut buf_index: u32 = 1;
    for (buf_name, buf_decl) in &cfg.state_buffers {
        let buf_upper = buf_name.to_uppercase();
        let c_prefix = format!("S_{buf_upper}");

        c_out.push_str(&format!(
            "#define {c_prefix} ((blyt_buffer_h){buf_index}u)\n"
        ));
        rs_out.push_str(&format!(
            "#[allow(dead_code)] pub const {c_prefix}: BlytBufferH = {buf_index};\n"
        ));
        lua_c_fn.push_str(&format!(
            "    lua_pushinteger(L, {buf_index}); lua_setfield(L, -2, \"{buf_upper}\");\n"
        ));

        let mut visiting = Vec::new();
        let fields: Vec<FlatField> =
            flatten_record(&buf_decl.record, &cfg.records, &mut visiting).map_err(|e| err(e))?;

        for f in &fields {
            let c_field = format!("{c_prefix}_{}", f.flat_name.to_uppercase());
            let lua_key = format!("{buf_upper}_{}", f.flat_name.to_uppercase());
            let field_h: u32 = (buf_index << 16) | (f.index & 0xFFFF);

            let c_type = type_tag_c_type(f.type_tag);
            let rs_type = type_tag_rust_type(f.type_tag);
            let _suffix = type_tag_buf_get_suffix(f.type_tag);

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

        let n_upvals = 1 + fields.len();
        let fn_row_idx = format!("_blyt_proxy_{buf_name}_row_idx");
        let fn_row_newidx = format!("_blyt_proxy_{buf_name}_row_newidx");
        let fn_buf_idx = format!("_blyt_proxy_{buf_name}_idx");

        lua_c_proxy_fns.push_str(&format!("static int {fn_row_idx}(lua_State *L) {{\n"));
        lua_c_proxy_fns.push_str("    lua_rawgeti(L, 1, 1); /* slot stored at raw key 1 */\n");
        lua_c_proxy_fns.push_str("    lua_Integer _s = lua_tointeger(L, -1); lua_pop(L, 1);\n");
        lua_c_proxy_fns.push_str(
            "    blyt_buffer_h _bh = (blyt_buffer_h)lua_tointeger(L, lua_upvalueindex(1));\n",
        );
        lua_c_proxy_fns.push_str("    const char *_k = lua_tostring(L, 2);\n");
        lua_c_proxy_fns.push_str("    if (_k) {\n");
        for (fi, f) in fields.iter().enumerate() {
            let upval = fi + 2;
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
        lua_c_proxy_fns.push_str("    lua_rawgeti(L, lua_upvalueindex(1), lua_tointeger(L, 2));\n");
        lua_c_proxy_fns.push_str("    return 1;\n");
        lua_c_proxy_fns.push_str("}\n");

        lua_c_proxy_setup.push_str("    {\n");
        lua_c_proxy_setup.push_str("        int _init = lua_gettop(L);\n");
        lua_c_proxy_setup.push_str("        lua_newtable(L);\n");
        lua_c_proxy_setup.push_str("        int _row_mt = lua_gettop(L);\n");
        lua_c_proxy_setup.push_str(&format!("        lua_pushinteger(L, {buf_index}u);\n"));
        for f in &fields {
            let field_h: u32 = (buf_index << 16) | (f.index & 0xFFFF);
            lua_c_proxy_setup.push_str(&format!("        lua_pushinteger(L, 0x{field_h:08X}u);\n"));
        }
        lua_c_proxy_setup.push_str(&format!(
            "        lua_pushcclosure(L, {fn_row_idx}, {n_upvals});\n"
        ));
        lua_c_proxy_setup.push_str("        lua_setfield(L, _row_mt, \"__index\");\n");
        lua_c_proxy_setup.push_str(&format!("        lua_pushinteger(L, {buf_index}u);\n"));
        for f in &fields {
            let field_h: u32 = (buf_index << 16) | (f.index & 0xFFFF);
            lua_c_proxy_setup.push_str(&format!("        lua_pushinteger(L, 0x{field_h:08X}u);\n"));
        }
        lua_c_proxy_setup.push_str(&format!(
            "        lua_pushcclosure(L, {fn_row_newidx}, {n_upvals});\n"
        ));
        lua_c_proxy_setup.push_str("        lua_setfield(L, _row_mt, \"__newindex\");\n");
        lua_c_proxy_setup.push_str("        lua_newtable(L);\n");
        lua_c_proxy_setup.push_str("        int _rows = lua_gettop(L);\n");
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

pub(crate) fn generate_lua_state_decls(cfg: &CartConfig) -> Result<String, BuildError> {
    use crate::config::{FlatField, type_tag_lua_type};

    let mut out = String::new();
    out.push_str("---@meta\n");
    out.push_str("-- Auto-generated by blyt build — do not edit.\n");
    out.push_str("-- LuaLS annotations for the `S` state proxy (issue #48).\n\n");

    let mut s_fields = String::from("---@class blyt.S\n");

    for (buf_name, buf_decl) in &cfg.state_buffers {
        let buf_upper = buf_name.to_uppercase();
        let mut visiting = Vec::new();
        let fields: Vec<FlatField> =
            flatten_record(&buf_decl.record, &cfg.records, &mut visiting).map_err(err)?;

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

        let buf_class = format!("blyt.S.{buf_name}");
        out.push_str(&format!("---@class {buf_class}\n"));
        out.push_str(&format!(
            "---@field count integer @ {} slots\n",
            buf_decl.count
        ));
        out.push_str(&format!("---@field [integer] {row_class}\n\n"));

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
    out.push_str("\n---@type blyt.S\n");
    out.push_str("S = {}\n");
    Ok(out)
}

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
 * Codegen tasks
 * ------------------------------------------------------------------------- */

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

/* -------------------------------------------------------------------------
 * Archive task
 * ------------------------------------------------------------------------- */

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

/* -------------------------------------------------------------------------
 * Link task
 * ------------------------------------------------------------------------- */

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

fn find_lib_dir(sdk_include: &Path) -> Result<PathBuf, BuildError> {
    if let Ok(d) = std::env::var("BLYT_LIB_DIR") {
        let p = PathBuf::from(d);
        if p.join("libblyt32.so").exists() {
            return Ok(p);
        }
    }

    if let Ok(sdk) = std::env::var("BLYT_SDK_DIR") {
        let p = PathBuf::from(sdk).join("lib");
        if p.join("libblyt32.so").exists() {
            return Ok(p);
        }
    }

    if let Some(sdk) = sdk_root_from_exe() {
        let p = sdk.join("lib");
        if p.join("libblyt32.so").exists() {
            return Ok(p);
        }
    }

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
        "-Wl,-z,relro",
        "-Wl,-z,now",
        "-Wl,--build-id=none",
        "-Wl,--gc-sections",
    ])
    .arg(format!("-T{}", ld_script.display()))
    .arg("-Wl,-u,blyt_interp")
    .arg("-o")
    .arg(output);

    let has_native = !objs.is_empty() || rust_archive.is_some() || !lib_archives.is_empty();
    if lua_cart && has_native {
        cmd.arg("-Wl,--export-dynamic");
    }

    for obj in objs {
        cmd.arg(obj);
    }

    if let Some(archive) = rust_archive {
        if lua_cart {
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
        cmd.arg("-Wl,-u,blyt_cart_init")
            .arg("-Wl,-u,blyt_cart_update")
            .arg("-Wl,-u,blyt_cart_draw");
    }

    for archive in lib_archives {
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

    if lua_cart {
        cmd.arg("-Wl,--no-as-needed")
            .arg("-L")
            .arg(lib_dir)
            .arg("-lblyt32lua")
            .arg("-Wl,--as-needed");
    }
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
 * Assemble cart task
 * ------------------------------------------------------------------------- */

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
        .arg("--remove-section=.lua_regtab");

    if !debug {
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

/* -------------------------------------------------------------------------
 * Dev ELF task — like AssembleCartTask but never strips (dev artifact)
 * Output: build/.elf (release) / build/.dbg.elf (debug)
 * Key "finalise_dev" keeps its fingerprint separate from "finalise" so the
 * two tasks coexist in the same state directory without invalidating each other.
 * ------------------------------------------------------------------------- */

struct DevElfTask {
    objcopy: String,
    raw_elf: PathBuf,
    cart_info_file: PathBuf,
    output: PathBuf,
    extra_sections: Vec<(String, PathBuf)>,
}

impl Task for DevElfTask {
    fn key(&self) -> &str {
        "finalise_dev"
    }
    fn label(&self) -> String {
        "finalise dev ELF".to_string()
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
        // always pass debug=true: never strip the dev ELF regardless of variant
        finalise_cart(
            &self.objcopy,
            &self.raw_elf,
            &self.cart_info_file,
            &self.output,
            &extra,
            true,
        )
    }
}

fn default_output(project_dir: &Path, id: &str, debug: bool) -> PathBuf {
    let file = if debug {
        format!("{id}.dbg.blyt")
    } else {
        format!("{id}.blyt")
    };
    project_dir.join("build").join(file)
}

fn dev_elf_output(project_dir: &Path, debug: bool) -> PathBuf {
    let file = if debug { ".dbg.elf" } else { ".elf" };
    project_dir.join("build").join(file)
}

/* -------------------------------------------------------------------------
 * Source map
 * ------------------------------------------------------------------------- */

/// Absolute paths of every declared asset directory that exists — dev-mode
/// watch targets (#162). Returns empty on a malformed `assets:` block; the
/// build itself surfaces the config error.
pub(crate) fn asset_watch_dirs(project_dir: &Path) -> Vec<PathBuf> {
    assets::watch_dirs(project_dir).unwrap_or_default()
}

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
    if let Some(rust_src) = rust::rust_src_dir() {
        v.push(SourceMapEntry {
            local: rust_src,
            canonical: "/blyt/rust",
        });
    }
    if let Some(reg) = rust::cargo_registry_src() {
        v.push(SourceMapEntry {
            local: reg,
            canonical: "/blyt/cargo",
        });
    }
    v
}

fn write_source_map_manifest(
    build_root: &Path,
    entries: &[SourceMapEntry],
    project_dir: &Path,
) -> Result<(), BuildError> {
    let mut pairs: Vec<(String, String)> = entries
        .iter()
        .map(|e| (e.canonical.to_string(), e.local.display().to_string()))
        .collect();
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

/* -------------------------------------------------------------------------
 * Editor codegen (compile_commands.json + LuaLS annotations)
 * ------------------------------------------------------------------------- */

pub(crate) fn compile_commands_for(project_dir: &Path) -> Result<Vec<CompileEntry>, BuildError> {
    let c_srcs = c::collect_c_files(&project_dir.join("src/game/c"))?;
    let cpp_srcs = cpp::collect_cpp_files(&project_dir.join("src/game/c++"))?;
    if c_srcs.is_empty() && cpp_srcs.is_empty() {
        return Ok(Vec::new());
    }

    let sdk_include = find_sdk_include()?;
    let is_lua = project_dir.join("src/game/lua").is_dir();
    let build_dir = project_dir.join(if is_lua {
        "build/game/lua/release"
    } else {
        "build/game/c/release"
    });

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

    let defines: Vec<String> = if is_lua {
        vec!["-DBLYT_LUA_I32_F64=1".into(), "-DLUA_USE_LONGJMP=1".into()]
    } else {
        vec![]
    };

    let mut entries = Vec::new();
    if !c_srcs.is_empty() {
        let clang = c::find_clang();
        for src in c_srcs {
            entries.push(c::c_compile_arguments(
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
        let clangpp = cpp::find_clangpp();
        let libcxx_include = sdk_include.join("c++/v1");
        for src in cpp_srcs {
            entries.push(cpp::cpp_compile_arguments(
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

pub(crate) fn write_editor_codegen(project_dir: &Path) -> Result<(), BuildError> {
    let cfg = crate::config::read_cart_config(project_dir).map_err(err)?;
    if project_dir.join("src/game/lua").is_dir() && !cfg.state_buffers.is_empty() {
        let path = project_dir.join("build/blyt/lua/cart_state.lua");
        if write_if_changed(&path, &generate_lua_state_decls(&cfg)?)? {
            println!("wrote:  {}", path.display());
        }
    }

    match compile_commands_for(project_dir) {
        Ok(entries) if !entries.is_empty() => {
            let path = project_dir.join("build/compile_commands.json");
            let json = compile_commands_json(project_dir, &entries);
            if write_if_changed(&path, &json)? {
                println!("wrote:  {}", path.display());
            }
        }
        Ok(_) => {}
        Err(e) => println!("note: skipped compile_commands.json ({e})"),
    }
    Ok(())
}

/* -------------------------------------------------------------------------
 * Build manifest (blyt.build.yaml) and cart info (blyt.info.yaml)
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
    /// Asset-declaration block (#162). Parsed and consumed separately by
    /// `assets::read_assets_config`; declared here so `deny_unknown_fields`
    /// accepts the `assets:` key when a language declaration is also present.
    #[serde(default)]
    #[allow(dead_code)]
    assets: Option<assets::AssetsConfig>,
    /// Persistent-resource declaration (#160). Parsed and consumed separately by
    /// `persistent::read_persistent_resources`; declared here so
    /// `deny_unknown_fields` accepts the `persistent_resources:` key.
    #[serde(default)]
    #[allow(dead_code)]
    persistent_resources: Option<Vec<String>>,
}

#[derive(serde::Deserialize, Default)]
#[serde(deny_unknown_fields)]
#[allow(dead_code)]
struct LanguageConfig {
    codegen: Option<bool>,
    sources: Option<Vec<String>>,
    compile_command: Option<String>,
    source_extension: Option<String>,
    strip_sections: Option<Vec<String>>,
}

pub(super) enum ExternalOutputType {
    Object,
    Archive,
}

pub(super) struct ExternalCompileConfig {
    pub language: String,
    pub extension: String,
    pub command_template: String,
    pub strip_sections: Vec<String>,
    pub output_type: ExternalOutputType,
}

enum BuildLanguages {
    Builtin(BTreeSet<CartLanguage>),
    External {
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
                    external::validate_compile_command_template(&cmd)?;
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
 * Library discovery
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
        if !path.is_dir() || path.join("Cargo.toml").exists() {
            continue;
        }
        let has_sources =
            !c::collect_c_files(&path)?.is_empty() || !cpp::collect_cpp_files(&path)?.is_empty();
        if has_sources {
            if let Some(name) = path.file_name().and_then(|n| n.to_str()) {
                names.push(name.to_string());
            }
        }
    }
    names.sort();
    Ok(names)
}

/* -------------------------------------------------------------------------
 * Pre-build: compile + link tasks, shared by `run` and `build_for_dev`
 * ------------------------------------------------------------------------- */

struct PreBuild {
    tasks: Vec<Box<dyn Task>>,
    state_dir: PathBuf,
    raw_elf: PathBuf,
    cart_info_file: PathBuf,
    extra_sections: Vec<(String, PathBuf)>,
    /// `.cart.resource.<id>` sections embedded only into the packed `.blyt`
    /// (release Phase 2). The dev ELF omits these and the runtime reads the
    /// staging directory instead (ADR-0088).
    resource_sections: Vec<(String, PathBuf)>,
    objcopy: String,
    cart_id: String,
    project_dir: PathBuf,
}

fn pre_build(project_dir_arg: &Path, debug: bool) -> Result<PreBuild, BuildError> {
    let project_dir = fs::canonicalize(project_dir_arg)?;
    let project_dir = &project_dir;
    let cart_info = read_cart_info(project_dir)?;

    let clang = c::find_clang();
    let clangpp = cpp::find_clangpp();
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

    if is_lua {
        let lua_src_dir = project_dir.join("src/game/lua");
        if lua::collect_lua_files(&lua_src_dir)?.is_empty() {
            return Err(err(format!(
                "no .lua files found under {} — \
                 for a C or Rust cart add `language: c` or `language: rust` \
                 to blyt.build.yaml",
                lua_src_dir.display()
            )));
        }
        if native_count == 0 {
            if !c::collect_c_files(&project_dir.join("src/game/c"))?.is_empty() {
                return Err(err(
                    "src/game/c/ contains .c files but no native language is declared — \
                     add a `languages:` block to blyt.build.yaml, e.g.:\n\
                     \x20 languages:\n\
                     \x20   lua:\n\
                     \x20   c:",
                ));
            }
            if !cpp::collect_cpp_files(&project_dir.join("src/game/c++"))?.is_empty() {
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
        if external_config.is_none() && c::collect_c_files(&c_src_dir)?.is_empty() {
            return Err(err(format!(
                "no .c files found under {}",
                c_src_dir.display()
            )));
        }
    }
    if languages.contains(&CartLanguage::Cpp) {
        let cpp_src_dir = project_dir.join("src/game/c++");
        if cpp::collect_cpp_files(&cpp_src_dir)?.is_empty() {
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
        if external::collect_external_source_files(project_dir, cfg)?.is_empty() {
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
    let state_dir = task_state_dir(project_dir, variant);

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
    // `.cart.config` is emitted *after* the asset scan below: its `default_palette`
    // may reference a palette-file asset, so the name→id map must exist first (#214).
    let entry_stub_src = build_dir.join("_blyt_entry.c");
    write_if_changed(&entry_stub_src, ENTRY_STUB_C)?;
    let interp_src = build_dir.join("_blyt_interp.c");
    write_if_changed(&interp_src, INTERP_STUB_C)?;

    let mut tasks: Vec<Box<dyn Task>> = Vec::new();

    // Asset pipeline Phase 1 (issue #91): scan assets/, stage content-addressed
    // resource files, write the resource-id-index, and emit cart_resources.{h,lua}.
    let top_build = project_dir.join("build");
    let scanned_assets = assets::scan_assets(project_dir, &top_build)?;

    // `.cart.config` (deferred from above): resolve `palettes.default` against
    // both the built-in and palette-asset namespaces now the scan is done (#214).
    let default_palette = resolve_default_palette(&cart_config, &scanned_assets)?;
    let cart_config_file = build_dir.join("cart.config.bin");
    write_bytes_if_changed(
        &cart_config_file,
        &cart_config_bytes(&cart_config, default_palette),
    )?;

    let assets::AssetBuild {
        tasks: asset_tasks,
        resource_sections,
        any: has_assets,
    } = assets::plan_assets(
        &scanned_assets,
        &top_build,
        &build_dir.join("blyt/c"),
        &build_dir.join("blyt/lua"),
        &build_dir.join("blyt/rust"),
    );
    tasks.extend(asset_tasks);

    // Packer-generated color constants (#203, ADR-0059): emit cart_colors.{h,
    // lua,rs} from the manifest's `colors:` swatch map. Independent of the asset
    // pipeline (colors are a config map, not files); gated on its own presence.
    let has_colors = !cart_config.colors.is_empty();
    if has_colors {
        tasks.push(Box::new(colors::GenerateColorHeadersTask {
            c_output: build_dir.join("blyt/c/cart_colors.h"),
            lua_output: build_dir.join("blyt/lua/cart_colors.lua"),
            rust_output: build_dir.join("blyt/rust/cart_colors.rs"),
            c_header: colors::generate_c_header(&cart_config.colors),
            lua_module: colors::generate_lua_module(&cart_config.colors),
            rust_module: colors::generate_rust_module(&cart_config.colors),
        }));
    }

    // Persistent resources (#160, ADR-0028): resolve the manifest's
    // `persistent_resources` names to ids against the scanned assets. This also
    // enforces the build-time budget guard (Layer 1) — an over-budget or
    // unknown-name set fails the build here, before any cart is produced.
    let persistent_names = persistent::read_persistent_resources(project_dir)?;
    let persistent_ids = persistent::resolve_persistent_ids(&persistent_names, &scanned_assets)?;

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

    tasks.push(c::make_c_task(
        project_dir,
        entry_stub_src.clone(),
        build_dir.clone(),
        &clang,
        &sdk_include,
        vec![],
        vec![],
        opt_c_flags.clone(),
        "compile_c",
    ));
    tasks.push(c::make_c_task(
        project_dir,
        interp_src.clone(),
        build_dir.clone(),
        &clang,
        &sdk_include,
        vec![],
        vec![],
        opt_c_flags.clone(),
        "compile_c",
    ));

    if is_lua {
        let luac = lua::find_luac();
        let mut lua_files = lua::collect_lua_files(&project_dir.join("src/game/lua"))?;
        // Bundle the packer-generated `cart_resources` module (ADR-0040) as the
        // FIRST chunk so `require("cart_resources")` resolves before any cart
        // code runs it at module scope (#93).  CompileLuaTask lists each file as
        // an input, so the engine sequences the generating task ahead of it.
        if has_assets {
            lua_files.insert(0, build_dir.join("blyt/lua/cart_resources.lua"));
        }
        // Same for the packer-generated color module (#203): bundle it first so
        // `require("cart_colors")` resolves before cart code runs at module
        // scope.
        if has_colors {
            lua_files.insert(0, build_dir.join("blyt/lua/cart_colors.lua"));
        }
        let bytecode_path = build_dir.join("bytecode.luac");
        let data_c = build_dir.join("cart_lua_data.c");
        let glue_src = build_dir.join("__blyt_lua_glue.c");

        tasks.push(Box::new(lua::CompileLuaTask {
            luac: luac.clone(),
            lua_files,
            project_dir: project_dir.to_path_buf(),
            output: bytecode_path.clone(),
        }));
        tasks.push(Box::new(lua::GenerateLuaDataTask {
            bytecode_path,
            output_c: data_c.clone(),
        }));
        tasks.push(c::make_c_task(
            project_dir,
            data_c,
            build_dir.clone(),
            &clang,
            &sdk_include,
            vec![],
            vec![],
            vec![],
            "compile_c",
        ));
        tasks.push(c::make_c_task(
            project_dir,
            glue_src,
            build_dir.clone(),
            &clang,
            &sdk_include,
            vec![],
            lua_lib_defines.clone(),
            vec![],
            "compile_c",
        ));
    }

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

        let c_files = c::collect_c_files(&src_dir)?;
        let cpp_files = cpp::collect_cpp_files(&src_dir)?;
        if c_files.is_empty() && cpp_files.is_empty() {
            return Err(err(format!(
                "library {name}: no source files found under {}",
                src_dir.display()
            )));
        }
        let mut lib_obj_files: Vec<PathBuf> = Vec::new();
        for src in c_files {
            let task = c::make_c_task(
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
            tasks.push(task);
        }
        for src in cpp_files {
            let libcxx_include = sdk_include.join("c++/v1");
            let task = cpp::make_cpp_task(
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
            tasks.push(task);
        }
        tasks.push(Box::new(AssembleLibArchiveTask {
            key_str: format!("archive/{name}"),
            ar: ar.clone(),
            obj_files: lib_obj_files,
            output: archive,
        }));
    }

    if is_lua {
        let cargo = rust::find_cargo();
        let rust_sdk = rust::find_rust_sdk(&sdk_include)?;
        let rust_lib_patches: Vec<(String, PathBuf)> = Vec::new();
        for (lib_name, lib_path) in rust::discover_rust_libs(project_dir)? {
            let lib_build_dir = project_dir.join("build/lib").join(&lib_name).join(variant);
            fs::create_dir_all(&lib_build_dir)?;
            let output = lib_build_dir.join("lib.a");
            lib_archives.push(output.clone());
            tasks.push(Box::new(rust::CompileRustTask {
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
                cart_resources_rs: None,
                cart_colors_rs: None,
                output,
                source_dir: lib_path,
            }));
        }
    }

    let mut c_include_paths: Vec<PathBuf> = lib_include_paths;
    // build/blyt/c holds cart_state.h (state buffers), cart_resources.h
    // (assets), and cart_colors.h (#203); add it to the include path if any is
    // generated.
    if buffers_present || has_assets || has_colors {
        c_include_paths.push(build_dir.join("blyt/c"));
    }

    if languages.contains(&CartLanguage::C) {
        let extra_defines = if is_lua {
            lua_lib_defines.clone()
        } else {
            vec![]
        };
        for src in c::collect_c_files(&project_dir.join("src/game/c"))? {
            tasks.push(c::make_c_task(
                project_dir,
                src,
                build_dir.clone(),
                &clang,
                &sdk_include,
                c_include_paths.clone(),
                extra_defines.clone(),
                opt_c_flags.clone(),
                "compile_c",
            ));
        }
    }

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
        for src in cpp::collect_cpp_files(&project_dir.join("src/game/c++"))? {
            tasks.push(cpp::make_cpp_task(
                project_dir,
                src,
                build_dir.clone(),
                &clangpp,
                &sdk_include,
                libcxx_include.clone(),
                c_include_paths.clone(),
                opt_c_flags.clone(),
                "compile_cpp",
            ));
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

    let rust_archive_path = if languages.contains(&CartLanguage::Rust) {
        let cargo = rust::find_cargo();
        let rust_sdk = rust::find_rust_sdk(&sdk_include)?;
        let rust_manifest = project_dir.join("src/game/rust/Cargo.toml");
        let rust_build_dir = project_dir.join("build/game/rust").join(variant);
        fs::create_dir_all(&rust_build_dir)?;
        let rust_libs = rust::discover_rust_libs(project_dir)?;
        let cart_state_rs = if buffers_present {
            Some(build_dir.join("blyt/rust/cart_state.rs"))
        } else {
            None
        };
        // Resource constants (#94): generated only when the cart bundles assets,
        // matching the C/Lua header gating.  The cart pulls them in with
        // `include!(env!("BLYT_CART_RESOURCES_RS"))`.
        let cart_resources_rs = if has_assets {
            Some(build_dir.join("blyt/rust/cart_resources.rs"))
        } else {
            None
        };
        // Color constants (#203): generated only when the cart declares colors,
        // matching the C/Lua gating.  Pulled in via
        // `include!(env!("BLYT_CART_COLORS_RS"))`.
        let cart_colors_rs = if has_colors {
            Some(build_dir.join("blyt/rust/cart_colors.rs"))
        } else {
            None
        };
        let output = rust_build_dir.join("cart.a");
        tasks.push(Box::new(rust::CompileRustTask {
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
            cart_resources_rs,
            cart_colors_rs,
            output: output.clone(),
            source_dir: project_dir.join("src/game/rust"),
        }));
        Some(output)
    } else {
        None
    };

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
        let src_files = external::collect_external_source_files(project_dir, cfg)?;
        let is_obj = matches!(cfg.output_type, ExternalOutputType::Object);
        tasks.push(Box::new(external::CompileExternalTask {
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

    let mut obj_files: Vec<PathBuf> = Vec::new();
    let mut final_lib_archives = lib_archives;

    obj_files.push(build_dir.join("_blyt_entry.o"));
    obj_files.push(build_dir.join("_blyt_interp.o"));

    if is_lua {
        obj_files.push(build_dir.join("cart_lua_data.o"));
        obj_files.push(build_dir.join("__blyt_lua_glue.o"));
    }

    if languages.contains(&CartLanguage::C) {
        for src in c::collect_c_files(&project_dir.join("src/game/c"))? {
            let stem = src.file_stem().and_then(OsStr::to_str).unwrap_or("unknown");
            obj_files.push(build_dir.join(format!("{stem}.o")));
        }
    }

    if languages.contains(&CartLanguage::Cpp) {
        for src in cpp::collect_cpp_files(&project_dir.join("src/game/c++"))? {
            let stem = src.file_stem().and_then(OsStr::to_str).unwrap_or("unknown");
            obj_files.push(build_dir.join(format!("{stem}.o")));
        }
    }

    if let Some((path, is_obj)) = external_output {
        if is_obj {
            obj_files.push(path);
        } else {
            final_lib_archives.push(path);
        }
    }

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

    let mut extra_sections: Vec<(String, PathBuf)> = Vec::new();
    // .cart.config is emitted for every cart (carries save_version; ADR-0125).
    extra_sections.push((".cart.config".to_string(), cart_config_file.clone()));
    if is_lua {
        extra_sections.push((".cart.lua".to_string(), build_dir.join("bytecode.luac")));
    }
    if buffers_present {
        extra_sections.push((
            ".cart.layouts".to_string(),
            build_dir.join("cart.layouts.bin"),
        ));
    }
    // .cart.persistent (#160): the sorted persistent resource-id list, emitted
    // into both the packed `.blyt` and the dev ELF (so the emulated dev path
    // pre-loads the same set). Omitted entirely when the cart declares none.
    if !persistent_ids.is_empty() {
        let persistent_file = build_dir.join("cart.persistent.bin");
        tasks.push(Box::new(persistent::WritePersistentSectionTask {
            output: persistent_file.clone(),
            ids: persistent_ids,
        }));
        extra_sections.push((".cart.persistent".to_string(), persistent_file));
    }

    Ok(PreBuild {
        tasks,
        state_dir,
        raw_elf,
        cart_info_file,
        extra_sections,
        resource_sections,
        objcopy,
        cart_id: cart_info.id,
        project_dir: project_dir.to_path_buf(),
    })
}

/* -------------------------------------------------------------------------
 * Public entry point: build a full cart
 * ------------------------------------------------------------------------- */

pub fn run(
    project_dir: &Path,
    output: Option<&Path>,
    debug: bool,
    force: bool,
) -> Result<PathBuf, BuildError> {
    let mut pre = pre_build(project_dir, debug)?;
    let output_path = output
        .map(PathBuf::from)
        .unwrap_or_else(|| default_output(&pre.project_dir, &pre.cart_id, debug));
    // Phase 2: embed assets as .cart.resource.<id> sections into the packed cart.
    let mut packed_sections = pre.extra_sections;
    packed_sections.extend(pre.resource_sections);
    pre.tasks.push(Box::new(AssembleCartTask {
        objcopy: pre.objcopy.clone(),
        raw_elf: pre.raw_elf.clone(),
        cart_info_file: pre.cart_info_file.clone(),
        output: output_path.clone(),
        extra_sections: packed_sections,
        debug,
    }));
    run_tasks(&pre.tasks, &pre.state_dir, force)?;
    write_editor_codegen(&pre.project_dir)?;
    println!("built: {}", output_path.display());
    Ok(output_path)
}

/* -------------------------------------------------------------------------
 * Public entry point: build a dev ELF for `blyt run ./dir` / `blyt debug
 * ./dir` (issue #84).  Same compile+link tasks as `run()` but produces
 * build/.elf (release) or build/.dbg.elf (debug) via DevElfTask instead of
 * the distributable .blyt cart.  Never strips — dev artifact.
 * ------------------------------------------------------------------------- */

pub fn build_for_dev(project_dir: &Path, debug: bool, force: bool) -> Result<PathBuf, BuildError> {
    let mut pre = pre_build(project_dir, debug)?;
    let dev_elf = dev_elf_output(&pre.project_dir, debug);
    pre.tasks.push(Box::new(DevElfTask {
        objcopy: pre.objcopy.clone(),
        raw_elf: pre.raw_elf.clone(),
        cart_info_file: pre.cart_info_file.clone(),
        output: dev_elf.clone(),
        extra_sections: pre.extra_sections,
    }));
    run_tasks(&pre.tasks, &pre.state_dir, force)?;
    println!("built: {}", dev_elf.display());
    Ok(dev_elf)
}

/* -------------------------------------------------------------------------
 * Public entry point: build a single library
 * ------------------------------------------------------------------------- */

pub fn build_single_lib(
    project_dir: &Path,
    lib_name: &str,
    _debug: bool,
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

    let cargo_toml = src_dir.join("Cargo.toml");
    if cargo_toml.exists() {
        let cargo = rust::find_cargo();
        let sdk_include = find_sdk_include()?;
        let rust_sdk = rust::find_rust_sdk(&sdk_include)?;
        let lib_build_dir = project_dir.join("build/lib").join(lib_name);
        fs::create_dir_all(&lib_build_dir)?;
        let state_dir = task_state_dir(project_dir, &format!("lib/{lib_name}"));
        let output = lib_build_dir.join("lib.a");
        let tasks: Vec<Box<dyn Task>> = vec![Box::new(rust::CompileRustTask {
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
            cart_resources_rs: None,
            cart_colors_rs: None,
            output: output.clone(),
            source_dir: src_dir,
        })];
        run_tasks(&tasks, &state_dir, force)?;
        println!("built: {}", output.display());
        return Ok(output);
    }

    let clang = c::find_clang();
    let clangpp = cpp::find_clangpp();
    let ar = find_ar();
    let sdk_include = find_sdk_include()?;
    let lib_build_dir = project_dir.join("build/lib").join(lib_name);
    let state_dir = task_state_dir(project_dir, &format!("lib/{lib_name}"));

    let c_files = c::collect_c_files(&src_dir)?;
    let cpp_files = cpp::collect_cpp_files(&src_dir)?;
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
        let task = c::make_c_task(
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
        tasks.push(task);
    }
    for src in cpp_files {
        let libcxx_include = sdk_include.join("c++/v1");
        let task = cpp::make_cpp_task(
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
        tasks.push(task);
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

#[cfg(test)]
mod tests {
    use super::external::{tokenize_command, validate_compile_command_template};
    use super::*;
    use crate::cart_config_generated::blyt::root_as_cart_config;
    use crate::cart_info_generated::blyt::root_as_cart_info;
    use tempfile::tempdir;

    fn test_fields() -> InfoFields {
        InfoFields {
            id: "hello".to_string(),
            title: "Hello World".to_string(),
            version: "0.0.1-dev".to_string(),
        }
    }

    fn read_debug(bytes: &[u8]) -> bool {
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
    fn cart_config_save_version_round_trips() {
        let cfg = CartConfig {
            fps: 60,
            save_version: 7,
            ..Default::default()
        };
        let b = cart_config_bytes(&cfg, 0);
        assert_eq!(&b[0..4], b"CCFG");
        assert_eq!(&b[4..6], &[0, 0], "format_major = 0");
        assert_eq!(&b[6..8], &[0, 0], "format_minor = 0");
        let config = root_as_cart_config(&b[8..]).expect("valid CartConfig");
        assert_eq!(config.save_version(), 7);
        assert_eq!(config.fps(), 60);
    }

    #[test]
    fn cart_config_save_version_defaults_zero() {
        let b = cart_config_bytes(&CartConfig::default(), 0);
        let config = root_as_cart_config(&b[8..]).expect("valid CartConfig");
        assert_eq!(config.save_version(), 0);
    }

    #[test]
    fn cart_config_default_palette_stores_resolved_handle() {
        // `cart_config_bytes` now just stores the pre-resolved handle verbatim.
        let b = cart_config_bytes(&CartConfig::default(), 0);
        assert_eq!(
            root_as_cart_config(&b[8..])
                .expect("valid CartConfig")
                .default_palette(),
            0
        );
        let b = cart_config_bytes(&CartConfig::default(), 0x2100_0002);
        assert_eq!(
            root_as_cart_config(&b[8..])
                .expect("valid CartConfig")
                .default_palette(),
            0x2100_0002
        );
    }

    /// A palette `AssetInfo` with the given canonical name and id (other fields
    /// are placeholders `resolve_default_palette` never reads).
    fn palette_asset(id: u32, name: &str) -> assets::AssetInfo {
        assets::AssetInfo {
            id,
            resource_name: name.to_string(),
            source: PathBuf::from(format!("assets/{name}.hex")),
            fingerprint: "0".repeat(16),
            rel_data: format!("resources/{name}-0.data"),
            data_output: PathBuf::from("x"),
            meta_output: PathBuf::from("x"),
            resource_type: assets::ResourceType::Palette,
            staged_len: 1024,
        }
    }

    fn cfg_with_default(name: Option<&str>) -> CartConfig {
        let mut cfg = CartConfig::default();
        cfg.palettes.default = name.map(str::to_string);
        cfg
    }

    #[test]
    fn resolve_default_palette_unset_is_zero() {
        assert_eq!(
            resolve_default_palette(&cfg_with_default(None), &[]).unwrap(),
            0
        );
    }

    #[test]
    fn resolve_default_palette_builtin_is_runtime_handle() {
        // vga = built-in id 2, runtime provenance -> 0x2100_0002.
        assert_eq!(
            resolve_default_palette(&cfg_with_default(Some("vga")), &[]).unwrap(),
            0x2100_0002
        );
    }

    #[test]
    fn resolve_default_palette_asset_is_cart_handle() {
        // A palette asset named `main` at id 3 -> PROV_CART handle 0x2000_0003.
        let assets = [palette_asset(3, "main")];
        assert_eq!(
            resolve_default_palette(&cfg_with_default(Some("main")), &assets).unwrap(),
            0x2000_0003
        );
    }

    #[test]
    fn resolve_default_palette_builtin_wins_over_asset() {
        // Built-in-first: even were an asset somehow named `vga`, `default: vga`
        // resolves to the built-in. (Such an asset is itself rejected below.)
        let assets = [palette_asset(3, "main")];
        assert_eq!(
            resolve_default_palette(&cfg_with_default(Some("vga")), &assets).unwrap(),
            0x2100_0002
        );
    }

    #[test]
    fn resolve_default_palette_reserved_asset_name_errors() {
        // A palette asset canonicalizing to a built-in name is a build error,
        // even when it is not the selected default.
        let assets = [palette_asset(1, "ega")];
        let err = resolve_default_palette(&cfg_with_default(None), &assets)
            .err()
            .expect("reserved name should be rejected");
        assert!(err.to_string().contains("reserved built-in"), "{err}");
    }

    #[test]
    fn resolve_default_palette_non_palette_asset_errors() {
        let mut text = palette_asset(2, "readme");
        text.resource_type = assets::ResourceType::Text;
        let err = resolve_default_palette(&cfg_with_default(Some("readme")), &[text])
            .err()
            .expect("non-palette asset should be rejected");
        assert!(err.to_string().contains("non-palette asset"), "{err}");
    }

    #[test]
    fn resolve_default_palette_unknown_name_errors() {
        let err = resolve_default_palette(&cfg_with_default(Some("nope")), &[])
            .err()
            .expect("unknown name should be rejected");
        assert!(err.to_string().contains("unknown palette"), "{err}");
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
        expect_err("name: hello\n", "unknown field");

        fs::remove_file(dir.path().join("blyt.info.yaml")).unwrap();
        let e = read_cart_info(dir.path()).unwrap_err().to_string();
        assert!(e.contains("blyt.info.yaml not found"));
    }

    // --- codegen task inputs/outputs ---

    #[test]
    fn codegen_tasks_input_config_when_present() {
        let d = tempdir().unwrap();
        let cfg = d.path().join("blyt.config.yaml");
        fs::write(&cfg, "").unwrap();
        let build = d.path().join("build");

        for task in [
            &GenerateCHeaderTask {
                project_dir: d.path().to_path_buf(),
                build_dir: build.clone(),
            } as &dyn Task,
            &GenerateRustStateTask {
                project_dir: d.path().to_path_buf(),
                build_dir: build.clone(),
            },
            &GenerateLayoutsTask {
                project_dir: d.path().to_path_buf(),
                build_dir: build.clone(),
            },
            &GenerateLuaGlueTask {
                project_dir: d.path().to_path_buf(),
                build_dir: build.clone(),
            },
        ] {
            assert!(
                task.inputs().contains(&TaskInput::File(cfg.clone())),
                "{} must include blyt.config.yaml in inputs",
                task.key()
            );
        }
    }

    #[test]
    fn codegen_tasks_empty_inputs_without_config() {
        let d = tempdir().unwrap();
        let build = d.path().join("build");
        for task in [
            &GenerateCHeaderTask {
                project_dir: d.path().to_path_buf(),
                build_dir: build.clone(),
            } as &dyn Task,
            &GenerateRustStateTask {
                project_dir: d.path().to_path_buf(),
                build_dir: build.clone(),
            },
        ] {
            assert!(
                task.inputs().is_empty(),
                "{} must have no inputs when blyt.config.yaml absent",
                task.key()
            );
        }
    }

    #[test]
    fn codegen_task_outputs() {
        let build = PathBuf::from("/build");
        let project = PathBuf::from("/proj");
        assert_eq!(
            GenerateCHeaderTask {
                project_dir: project.clone(),
                build_dir: build.clone()
            }
            .outputs(),
            vec![build.join("blyt/c/cart_state.h")]
        );
        assert_eq!(
            GenerateRustStateTask {
                project_dir: project.clone(),
                build_dir: build.clone()
            }
            .outputs(),
            vec![build.join("blyt/rust/cart_state.rs")]
        );
        assert_eq!(
            GenerateLayoutsTask {
                project_dir: project.clone(),
                build_dir: build.clone()
            }
            .outputs(),
            vec![build.join("cart.layouts.bin")]
        );
        assert_eq!(
            GenerateLuaGlueTask {
                project_dir: project,
                build_dir: build.clone()
            }
            .outputs(),
            vec![build.join("__blyt_lua_glue.c")]
        );
    }

    // --- AssembleLibArchiveTask ---

    #[test]
    fn archive_task_inputs_and_outputs() {
        let d = tempdir().unwrap();
        let o1 = d.path().join("a.o");
        let o2 = d.path().join("b.o");
        let out = d.path().join("libcart.a");
        let task = AssembleLibArchiveTask {
            key_str: "archive/mylib".to_string(),
            ar: "llvm-ar".to_string(),
            obj_files: vec![o1.clone(), o2.clone()],
            output: out.clone(),
        };
        let inputs = task.inputs();
        assert!(inputs.contains(&TaskInput::File(o1)));
        assert!(inputs.contains(&TaskInput::File(o2)));
        assert!(inputs.contains(&TaskInput::Value("llvm-ar".to_string())));
        assert_eq!(task.outputs(), vec![out]);
    }

    // --- LinkElfTask ---

    #[test]
    fn link_task_inputs_files_and_compiler() {
        let obj = PathBuf::from("/build/foo.o");
        let ld = PathBuf::from("/sdk/ld/blyt.ld");
        let out = PathBuf::from("/build/cart.elf");
        let task = LinkElfTask {
            clang: "clang".to_string(),
            obj_files: vec![obj.clone()],
            rust_archive: None,
            lib_archives: vec![],
            ld_script: ld.clone(),
            lib_dir: PathBuf::from("/sdk/lib"),
            output: out.clone(),
            is_lua: false,
        };
        let inputs = task.inputs();
        assert!(inputs.contains(&TaskInput::File(obj)));
        assert!(inputs.contains(&TaskInput::File(ld)));
        assert!(inputs.contains(&TaskInput::Value("clang".to_string())));
        assert_eq!(task.outputs(), vec![out]);
    }

    #[test]
    fn link_task_includes_rust_archive_and_lib_archives() {
        let rust_ar = PathBuf::from("/build/libcart.a");
        let lib_ar = PathBuf::from("/build/lib/libmylib.a");
        let task = LinkElfTask {
            clang: "clang".to_string(),
            obj_files: vec![],
            rust_archive: Some(rust_ar.clone()),
            lib_archives: vec![lib_ar.clone()],
            ld_script: PathBuf::from("/sdk/ld/blyt.ld"),
            lib_dir: PathBuf::from("/sdk/lib"),
            output: PathBuf::from("/build/cart.elf"),
            is_lua: false,
        };
        let inputs = task.inputs();
        assert!(inputs.contains(&TaskInput::File(rust_ar)));
        assert!(inputs.contains(&TaskInput::File(lib_ar)));
    }

    // --- AssembleCartTask ---

    #[test]
    fn assemble_cart_inputs_and_outputs() {
        let elf = PathBuf::from("/build/cart.elf");
        let info = PathBuf::from("/build/cart.info");
        let luac = PathBuf::from("/build/cart.luac");
        let out = PathBuf::from("/build/cart.blyt");
        let task = AssembleCartTask {
            objcopy: "llvm-objcopy".to_string(),
            raw_elf: elf.clone(),
            cart_info_file: info.clone(),
            output: out.clone(),
            extra_sections: vec![(".cart.lua".to_string(), luac.clone())],
            debug: false,
        };
        let inputs = task.inputs();
        assert!(inputs.contains(&TaskInput::File(elf)));
        assert!(inputs.contains(&TaskInput::File(info)));
        assert!(inputs.contains(&TaskInput::File(luac)));
        assert!(inputs.contains(&TaskInput::Value("llvm-objcopy".to_string())));
        assert!(inputs.contains(&TaskInput::Value("debug=false".to_string())));
        assert_eq!(task.outputs(), vec![out]);
    }

    // --- DevElfTask ---

    #[test]
    fn dev_elf_task_inputs_and_outputs() {
        let elf = PathBuf::from("/build/cart.elf");
        let info = PathBuf::from("/build/cart.info");
        let luac = PathBuf::from("/build/cart.luac");
        let out = PathBuf::from("/build/.elf");
        let task = DevElfTask {
            objcopy: "llvm-objcopy".to_string(),
            raw_elf: elf.clone(),
            cart_info_file: info.clone(),
            output: out.clone(),
            extra_sections: vec![(".cart.lua".to_string(), luac.clone())],
        };
        let inputs = task.inputs();
        assert!(inputs.contains(&TaskInput::File(elf)));
        assert!(inputs.contains(&TaskInput::File(info)));
        assert!(inputs.contains(&TaskInput::File(luac)));
        assert!(inputs.contains(&TaskInput::Value("llvm-objcopy".to_string())));
        assert_eq!(task.outputs(), vec![out]);
    }

    #[test]
    fn dev_elf_task_key_is_finalise_dev() {
        let task = DevElfTask {
            objcopy: "llvm-objcopy".to_string(),
            raw_elf: PathBuf::from("/build/cart.elf"),
            cart_info_file: PathBuf::from("/build/cart.info"),
            output: PathBuf::from("/build/.elf"),
            extra_sections: vec![],
        };
        assert_eq!(task.key(), "finalise_dev");
        assert_ne!(task.key(), "finalise");
    }

    #[test]
    fn dev_elf_output_paths() {
        let p = PathBuf::from("/proj");
        assert_eq!(dev_elf_output(&p, false), p.join("build/.elf"));
        assert_eq!(dev_elf_output(&p, true), p.join("build/.dbg.elf"));
    }

    #[test]
    fn assemble_cart_debug_flag_differs_between_variants() {
        let make = |debug: bool| AssembleCartTask {
            objcopy: "llvm-objcopy".to_string(),
            raw_elf: PathBuf::from("/build/cart.elf"),
            cart_info_file: PathBuf::from("/build/cart.info"),
            output: PathBuf::from("/build/cart.blyt"),
            extra_sections: vec![],
            debug,
        };
        assert!(
            make(true)
                .inputs()
                .contains(&TaskInput::Value("debug=true".to_string()))
        );
        assert!(
            make(false)
                .inputs()
                .contains(&TaskInput::Value("debug=false".to_string()))
        );
        assert_ne!(make(true).inputs(), make(false).inputs());
    }
}
