//! Asset pipeline — Phase 1 (transform to content-addressed staging) and the
//! resource-id-index + generated constant headers (issue #91).
//!
//! Text (`.txt`) is the only asset type for now; every future asset type adds
//! a transform here (extension → resource type) and a runtime decoder. The
//! resource model follows ADR-0040 (names in source, integer handles at
//! runtime) and ADR-0088 (two-phase build; dev mode runs Phase 1 only and the
//! runtime reads the staging directory).
//!
//! Staging is content-addressed (`resources/<name>-<fp>.data`) so files are
//! never overwritten in place — a changed asset gets a new file alongside the
//! old, which the dev loop GCs only after the runtime acks the swap.

use std::ffi::OsStr;
use std::fs;
use std::path::{Path, PathBuf};

use xxhash_rust::xxh3::xxh3_64;

use crate::engine::{BuildError, Task, TaskInput, build_err};

/// A scanned asset resolved to its resource name, content fingerprint, and
/// content-addressed staging paths. `id` is assigned by `scan_assets` after the
/// full set is sorted by name (ADR-0040: IDs stable while the name set is).
#[derive(Debug, Clone, PartialEq)]
pub(super) struct AssetInfo {
    pub id: u32,
    pub resource_name: String,
    pub source: PathBuf,
    pub fingerprint: String,
    /// Relative staging path recorded in the index, e.g.
    /// `resources/greeting-<fp>.data`.
    pub rel_data: String,
    pub data_output: PathBuf,
    pub meta_output: PathBuf,
}

/// Map a path relative to `assets/` to a resource name per ADR-0040:
/// strip the extension, replace separators and any non-identifier character
/// with `_`, collapse runs of `_`, trim leading/trailing `_`, lowercase.
/// Errors if nothing remains (e.g. `assets/--.txt`).
pub(super) fn resource_name_from_rel(rel: &Path) -> Result<String, BuildError> {
    let without_ext = rel.with_extension("");
    let raw = without_ext.to_string_lossy();

    let mut subbed = String::with_capacity(raw.len());
    for ch in raw.chars() {
        if ch.is_ascii_alphanumeric() || ch == '_' {
            subbed.push(ch);
        } else {
            // Directory separators, dashes, spaces, dots, and anything else
            // that is not a C/Lua identifier character collapse to `_`.
            subbed.push('_');
        }
    }

    let mut collapsed = String::with_capacity(subbed.len());
    let mut prev_underscore = false;
    for ch in subbed.chars() {
        if ch == '_' {
            if !prev_underscore {
                collapsed.push('_');
            }
            prev_underscore = true;
        } else {
            collapsed.push(ch);
            prev_underscore = false;
        }
    }

    let trimmed = collapsed.trim_matches('_');
    if trimmed.is_empty() {
        return Err(build_err(format!(
            "asset {} has no usable resource name after sanitisation",
            rel.display()
        )));
    }
    Ok(trimmed.to_ascii_lowercase())
}

/// Uppercase resource name with the `R_` prefix, e.g. `greeting` → `R_GREETING`.
fn c_constant(name: &str) -> String {
    format!("R_{}", name.to_ascii_uppercase())
}

fn is_text_extension(ext: Option<&str>) -> bool {
    matches!(ext, Some("txt"))
}

fn collect_asset_sources(dir: &Path, out: &mut Vec<PathBuf>) -> Result<(), BuildError> {
    for entry in fs::read_dir(dir)? {
        let entry = entry?;
        let path = entry.path();
        if path.is_dir() {
            collect_asset_sources(&path, out)?;
        } else if is_text_extension(path.extension().and_then(OsStr::to_str)) {
            out.push(path);
        }
    }
    Ok(())
}

/// Scan `<project_dir>/assets/` for known asset types and resolve each to an
/// `AssetInfo` with a content fingerprint and content-addressed staging paths
/// under `<top_build>/resources/`. The set is sorted by resource name and
/// assigned 1-based integer IDs. Errors on a name collision (two files that
/// derive the same resource name). Returns an empty vec when there is no
/// `assets/` directory.
pub(super) fn scan_assets(
    project_dir: &Path,
    top_build: &Path,
) -> Result<Vec<AssetInfo>, BuildError> {
    let assets_dir = project_dir.join("assets");
    if !assets_dir.exists() {
        return Ok(Vec::new());
    }

    let mut sources = Vec::new();
    collect_asset_sources(&assets_dir, &mut sources)?;
    sources.sort();

    let resources_dir = top_build.join("resources");
    let mut assets: Vec<AssetInfo> = Vec::new();
    for source in sources {
        let rel = source.strip_prefix(&assets_dir).unwrap_or(&source);
        let resource_name = resource_name_from_rel(rel)?;
        let bytes = fs::read(&source)?;
        let fingerprint = format!("{:016x}", xxh3_64(&bytes));
        let rel_data = format!("resources/{resource_name}-{fingerprint}.data");
        let data_output = resources_dir.join(format!("{resource_name}-{fingerprint}.data"));
        let meta_output = resources_dir.join(format!("{resource_name}-{fingerprint}.meta"));
        assets.push(AssetInfo {
            id: 0, // assigned below
            resource_name,
            source,
            fingerprint,
            rel_data,
            data_output,
            meta_output,
        });
    }

    // Deterministic IDs: sort by resource name, assign 1..=N.
    assets.sort_by(|a, b| a.resource_name.cmp(&b.resource_name));
    for (i, a) in assets.iter().enumerate() {
        if i > 0 && a.resource_name == assets[i - 1].resource_name {
            return Err(build_err(format!(
                "asset name collision: two files derive resource name `{}`",
                a.resource_name
            )));
        }
    }
    for (i, a) in assets.iter_mut().enumerate() {
        a.id = (i + 1) as u32;
    }
    Ok(assets)
}

/// Minimal `.meta` for a text asset. Establishes the per-resource metadata
/// pattern future asset types fill out (dimensions, format, …); the runtime
/// uses the `.data` length for text so this is descriptive only.
fn text_meta_contents(a: &AssetInfo) -> String {
    format!("type=text\nname={}\n", a.resource_name)
}

/// `resource-id-index` body: one `<id> <rel_data>` line per resource, ordered
/// by id. Maps integer resource IDs to content-addressed staging paths so the
/// runtime can build its ID→path table at session start (ADR-0088).
pub(super) fn resource_index_contents(assets: &[AssetInfo]) -> String {
    let mut s = String::new();
    for a in assets {
        s.push_str(&format!("{} {}\n", a.id, a.rel_data));
    }
    s
}

/// Generated C header (`cart_resources.h`): one `R_<NAME>` constant per
/// resource (ADR-0040). Always includes `<blyt.h>` for `blyt_resource_h`.
pub(super) fn generate_c_header(assets: &[AssetInfo]) -> String {
    let mut s = String::new();
    s.push_str("/* Generated by `blyt build` — do not edit. */\n");
    s.push_str("#pragma once\n");
    s.push_str("#include <blyt.h>\n\n");
    for a in assets {
        s.push_str(&format!(
            "#define {} ((blyt_resource_h){})\n",
            c_constant(&a.resource_name),
            a.id
        ));
    }
    s
}

/// Generated Lua module (`cart_resources.lua`): `require`-able table mapping
/// upper-cased resource names to integer IDs (ADR-0040). Consumed by the Lua
/// resource API (#93); emitted now to establish the pattern.
pub(super) fn generate_lua_module(assets: &[AssetInfo]) -> String {
    let mut s = String::new();
    s.push_str("-- Generated by `blyt build` — do not edit.\n");
    s.push_str("return {\n");
    for a in assets {
        s.push_str(&format!(
            "    {} = {},\n",
            a.resource_name.to_ascii_uppercase(),
            a.id
        ));
    }
    s.push_str("}\n");
    s
}

/// Generated Rust module (`cart_resources.rs`): one `R_<NAME>` `ResourceHandle`
/// constant per resource (ADR-0040), mirroring the C header (#94). The cart
/// pulls it in with `include!(env!("BLYT_CART_RESOURCES_RS"))`, the same
/// mechanism `cart_state.rs` uses. `blyt::ResourceHandle::new` is a const fn,
/// so each constant is usable in const context.
pub(super) fn generate_rust_module(assets: &[AssetInfo]) -> String {
    let mut s = String::new();
    s.push_str("// Generated by `blyt build` — do not edit.\n");
    for a in assets {
        s.push_str(&format!(
            "pub const {}: blyt::ResourceHandle = blyt::ResourceHandle::new({});\n",
            c_constant(&a.resource_name),
            a.id
        ));
    }
    s
}

/* -------------------------------------------------------------------------
 * Tasks
 * ------------------------------------------------------------------------- */

/// Phase 1 transform for a single asset: write the content-addressed `.data`
/// (raw bytes for text) and its `.meta`. A no-op when the content-addressed
/// outputs already exist (handled by the engine via the content fingerprint).
pub(super) struct AssetTask {
    pub resource_name: String,
    pub source: PathBuf,
    pub data_output: PathBuf,
    pub meta_output: PathBuf,
    pub meta: String,
    pub key_str: String,
}

impl Task for AssetTask {
    fn key(&self) -> &str {
        &self.key_str
    }
    fn label(&self) -> String {
        format!("asset    {}", self.resource_name)
    }
    fn inputs(&self) -> Vec<TaskInput> {
        vec![TaskInput::File(self.source.clone())]
    }
    fn outputs(&self) -> Vec<PathBuf> {
        vec![self.data_output.clone(), self.meta_output.clone()]
    }
    fn run(&self) -> Result<(), BuildError> {
        if let Some(parent) = self.data_output.parent() {
            fs::create_dir_all(parent)?;
        }
        // Text passes through verbatim; the transform is the identity copy.
        fs::copy(&self.source, &self.data_output)?;
        fs::write(&self.meta_output, &self.meta)?;
        Ok(())
    }
}

/// Write `resource-id-index` atomically (temp file → rename) so the runtime
/// never sees a half-written index. Failed builds leave the previous index in
/// place (ADR-0088).
pub(super) struct WriteResourceIndexTask {
    pub output: PathBuf,
    pub contents: String,
}

impl Task for WriteResourceIndexTask {
    fn key(&self) -> &str {
        "write_resource_index"
    }
    fn label(&self) -> String {
        "write    resource-id-index".to_string()
    }
    fn inputs(&self) -> Vec<TaskInput> {
        vec![TaskInput::Value(self.contents.clone())]
    }
    fn outputs(&self) -> Vec<PathBuf> {
        vec![self.output.clone()]
    }
    fn run(&self) -> Result<(), BuildError> {
        if let Some(parent) = self.output.parent() {
            fs::create_dir_all(parent)?;
        }
        let tmp = self.output.with_extension("tmp");
        fs::write(&tmp, &self.contents)?;
        fs::rename(&tmp, &self.output)?;
        Ok(())
    }
}

/// Emit the generated constant files: `blyt/c/cart_resources.h` (native carts),
/// `blyt/lua/cart_resources.lua` (Lua carts), and `blyt/rust/cart_resources.rs`
/// (Rust carts).
pub(super) struct GenerateResourceHeadersTask {
    pub c_output: PathBuf,
    pub lua_output: PathBuf,
    pub rust_output: PathBuf,
    pub c_header: String,
    pub lua_module: String,
    pub rust_module: String,
}

impl Task for GenerateResourceHeadersTask {
    fn key(&self) -> &str {
        "generate_resource_headers"
    }
    fn label(&self) -> String {
        "generate cart_resources.{h,lua,rs}".to_string()
    }
    fn inputs(&self) -> Vec<TaskInput> {
        vec![
            TaskInput::Value(self.c_header.clone()),
            TaskInput::Value(self.lua_module.clone()),
            TaskInput::Value(self.rust_module.clone()),
        ]
    }
    fn outputs(&self) -> Vec<PathBuf> {
        vec![
            self.c_output.clone(),
            self.lua_output.clone(),
            self.rust_output.clone(),
        ]
    }
    fn run(&self) -> Result<(), BuildError> {
        for out in [&self.c_output, &self.lua_output, &self.rust_output] {
            if let Some(parent) = out.parent() {
                fs::create_dir_all(parent)?;
            }
        }
        fs::write(&self.c_output, &self.c_header)?;
        fs::write(&self.lua_output, &self.lua_module)?;
        fs::write(&self.rust_output, &self.rust_module)?;
        Ok(())
    }
}

/// Build the Phase 1 task set plus the `.cart.resource.<id>` ELF sections used
/// by the packed (release) build. `c_header_dir` / `lua_header_dir` are the
/// variant include dirs; `top_build` holds the staging dir and index.
pub(super) struct AssetBuild {
    pub tasks: Vec<Box<dyn Task>>,
    /// `(section_name, staged_data_path)` for each resource — embedded into the
    /// packed `.blyt` only (the dev ELF reads the staging directory instead).
    pub resource_sections: Vec<(String, PathBuf)>,
    /// Whether any resources were found (callers add the generated header dir to
    /// the include path).
    pub any: bool,
}

pub(super) fn plan_assets(
    assets: &[AssetInfo],
    top_build: &Path,
    c_header_dir: &Path,
    lua_header_dir: &Path,
    rust_header_dir: &Path,
) -> AssetBuild {
    let mut tasks: Vec<Box<dyn Task>> = Vec::new();
    let mut resource_sections: Vec<(String, PathBuf)> = Vec::new();

    for a in assets {
        tasks.push(Box::new(AssetTask {
            resource_name: a.resource_name.clone(),
            source: a.source.clone(),
            data_output: a.data_output.clone(),
            meta_output: a.meta_output.clone(),
            meta: text_meta_contents(a),
            key_str: format!("asset/{}", a.resource_name),
        }));
        resource_sections.push((format!(".cart.resource.{}", a.id), a.data_output.clone()));
    }

    if !assets.is_empty() {
        tasks.push(Box::new(WriteResourceIndexTask {
            output: top_build.join("resource-id-index"),
            contents: resource_index_contents(assets),
        }));
        tasks.push(Box::new(GenerateResourceHeadersTask {
            c_output: c_header_dir.join("cart_resources.h"),
            lua_output: lua_header_dir.join("cart_resources.lua"),
            rust_output: rust_header_dir.join("cart_resources.rs"),
            c_header: generate_c_header(assets),
            lua_module: generate_lua_module(assets),
            rust_module: generate_rust_module(assets),
        }));
    }

    AssetBuild {
        tasks,
        resource_sections,
        any: !assets.is_empty(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::Path;

    fn name(p: &str) -> String {
        resource_name_from_rel(Path::new(p)).unwrap()
    }

    #[test]
    fn name_transform_matches_adr_0040() {
        assert_eq!(name("hero.png"), "hero");
        assert_eq!(name("sprites/hero.png"), "sprites_hero");
        assert_eq!(name("ui/buttons/play.png"), "ui_buttons_play");
        assert_eq!(name("music/theme.xm"), "music_theme");
        assert_eq!(name("hero-idle.png"), "hero_idle");
        assert_eq!(name("sfx/jump 1.wav"), "sfx_jump_1");
        assert_eq!(name("--draft--.png"), "draft");
        assert_eq!(name("ui/btn.play.png"), "ui_btn_play");
        assert_eq!(name("greeting.txt"), "greeting");
    }

    #[test]
    fn name_transform_rejects_empty() {
        assert!(resource_name_from_rel(Path::new("--.txt")).is_err());
        assert!(resource_name_from_rel(Path::new("__.txt")).is_err());
    }

    fn asset(id: u32, n: &str, fp: &str) -> AssetInfo {
        AssetInfo {
            id,
            resource_name: n.to_string(),
            source: PathBuf::from(format!("assets/{n}.txt")),
            fingerprint: fp.to_string(),
            rel_data: format!("resources/{n}-{fp}.data"),
            data_output: PathBuf::from(format!("build/resources/{n}-{fp}.data")),
            meta_output: PathBuf::from(format!("build/resources/{n}-{fp}.meta")),
        }
    }

    #[test]
    fn index_format() {
        let assets = vec![
            asset(1, "greeting", "abc123"),
            asset(2, "hero_idle", "def456"),
        ];
        assert_eq!(
            resource_index_contents(&assets),
            "1 resources/greeting-abc123.data\n2 resources/hero_idle-def456.data\n"
        );
    }

    #[test]
    fn c_header_constants() {
        let assets = vec![asset(1, "greeting", "abc123")];
        let h = generate_c_header(&assets);
        assert!(h.contains("#include <blyt.h>"));
        assert!(h.contains("#define R_GREETING ((blyt_resource_h)1)"));
    }

    #[test]
    fn lua_module_table() {
        let assets = vec![
            asset(1, "greeting", "abc123"),
            asset(2, "hero_idle", "def456"),
        ];
        let m = generate_lua_module(&assets);
        assert!(m.contains("return {"));
        assert!(m.contains("GREETING = 1,"));
        assert!(m.contains("HERO_IDLE = 2,"));
    }

    #[test]
    fn rust_module_constants() {
        let assets = vec![
            asset(1, "greeting", "abc123"),
            asset(2, "hero_idle", "def456"),
        ];
        let m = generate_rust_module(&assets);
        assert!(m.contains(
            "pub const R_GREETING: blyt::ResourceHandle = blyt::ResourceHandle::new(1);"
        ));
        assert!(m.contains(
            "pub const R_HERO_IDLE: blyt::ResourceHandle = blyt::ResourceHandle::new(2);"
        ));
    }
}
