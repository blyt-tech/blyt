//! Asset pipeline — Phase 1 (transform to content-addressed staging) and the
//! resource-id-index + generated constant headers (issue #91, #162).
//!
//! Membership is explicit (ADR-0088 amendment, #162): nothing is auto-scanned —
//! every asset a cart ships is declared via an `include:` glob in the
//! `assets:` block of `blyt.build.yaml`. A resource's *type* is decided by its
//! extension independent of how it was included: an extension in the global
//! `assets.text_extensions` list (default `["txt"]`) → text, everything else →
//! `raw` (opaque bytes) (ADR-0088 amendment 2026-06-27, #166). The two share a
//! near-identity transform, differing only in text's C-string hardening: a text
//! resource is validated (valid UTF-8, no embedded NUL) and a single trailing
//! `\0` is appended to its staged bytes, while raw stays byte-exact. The type
//! flows codegen-only into the generated constants (typed by kind); the runtime
//! stays byte-blind. The resource model follows ADR-0040 (names in source,
//! integer handles at runtime) and ADR-0088 (two-phase build; dev mode runs
//! Phase 1 only and the runtime reads the staging directory).
//!
//! Staging is content-addressed (`resources/<name>-<fp>.data`) so files are
//! never overwritten in place — a changed asset gets a new file alongside the
//! old, which the dev loop GCs only after the runtime acks the swap.

use std::ffi::OsStr;
use std::fs;
use std::path::{Path, PathBuf};

use globset::{Glob, GlobSet, GlobSetBuilder};
use unicode_normalization::UnicodeNormalization;
use xxhash_rust::xxh3::xxh3_64;

use crate::engine::{BuildError, Task, TaskInput, build_err};

/// A resource's type, decided by its file extension (ADR-0088 amendment). The
/// distinction is recorded in the `.meta` `type=` field (descriptive only — the
/// runtime serves `.data` bytes and never reads `.meta`) and, since #166, drives
/// the typed generated constants and text-only NUL termination. `text` resources
/// are validated (UTF-8, no embedded NUL) and gain a trailing `\0`; `raw` stays
/// byte-exact.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum ResourceType {
    Text,
    Raw,
    /// A cart-authored palette file (`.hex`/`.gpl`/`.pal`) — the first
    /// *processed* type (ADR-0088 amendment 2026-07-02, #214). Parsed and
    /// validated at build time into a fixed 1024-byte XRGB8888 table; at
    /// runtime it is just resource bytes (the runtime stays byte-blind, #166).
    Palette,
}

impl ResourceType {
    /// Classify an extension. Reserved palette-file extensions
    /// (`.hex`/`.gpl`/`.pal`) are the processed `Palette` type and take
    /// precedence over `text_extensions` (they are auto-scanned, #214); an
    /// extension listed in `text_extensions` (default `["txt"]`) is text;
    /// every other extension is opaque `raw` bytes (ADR-0088 amendment
    /// 2026-06-27). When a further transform lands (sprite/audio/...), it gets
    /// its own variant here.
    fn from_extension(ext: Option<&str>, text_extensions: &[String]) -> Self {
        match ext {
            Some(e) if super::palette::PaletteFormat::from_extension(e).is_some() => {
                ResourceType::Palette
            }
            Some(e) if text_extensions.iter().any(|t| t == e) => ResourceType::Text,
            _ => ResourceType::Raw,
        }
    }

    fn meta_name(self) -> &'static str {
        match self {
            ResourceType::Text => "text",
            ResourceType::Raw => "raw",
            ResourceType::Palette => "palette",
        }
    }

    fn is_text(self) -> bool {
        matches!(self, ResourceType::Text)
    }

    /// A *processed* type — parsed/transformed at build time rather than staged
    /// as opaque bytes. Processed types are auto-scanned by extension (a member
    /// even without a matching `include:` glob, #214).
    fn is_processed(self) -> bool {
        matches!(self, ResourceType::Palette)
    }

    /// C typedef for the generated `R_<NAME>` constant (typed-by-kind, #166).
    fn c_type(self) -> &'static str {
        match self {
            ResourceType::Text => "blyt_text_resource_t",
            ResourceType::Raw => "blyt_bytes_resource_t",
            ResourceType::Palette => "blyt_palette_t",
        }
    }

    /// Rust newtype for the generated `R_<NAME>` constant (typed-by-kind, #166).
    fn rust_type(self) -> &'static str {
        match self {
            ResourceType::Text => "TextResource",
            ResourceType::Raw => "BytesResource",
            ResourceType::Palette => "Palette",
        }
    }

    /// Lua constructor for the generated typed constant object (typed-by-kind,
    /// #166).
    fn lua_ctor(self) -> &'static str {
        match self {
            ResourceType::Text => "blyt.resource.text_resource",
            ResourceType::Raw => "blyt.resource.bytes_resource",
            ResourceType::Palette => "blyt.resource.palette",
        }
    }
}

/// Default `assets.text_extensions` when the manifest omits it (ADR-0088
/// amendment 2026-06-27): `.txt` is the only built-in text extension.
fn default_text_extensions() -> Vec<String> {
    vec!["txt".to_string()]
}

/// Validate a `text` resource's source bytes (ADR-0088 amendment 2026-06-27,
/// #166): must be valid UTF-8 with no embedded NUL (`0x00`). `0x00` is itself
/// valid UTF-8 (U+0000) but never appears inside a multi-byte sequence, so the
/// two checks are orthogonal. Honest-cart correctness, not a security boundary
/// (the runtime is byte-blind; defense lives at string-consuming API
/// boundaries). Returns a build error naming the file on failure.
fn validate_text_bytes(source: &Path, bytes: &[u8]) -> Result<(), BuildError> {
    if let Some(pos) = bytes.iter().position(|&b| b == 0) {
        return Err(build_err(format!(
            "text asset {} contains an embedded NUL (0x00) byte at offset {pos}; \
             text resources must be NUL-free (declare it as a non-text extension \
             for opaque bytes)",
            source.display()
        )));
    }
    if std::str::from_utf8(bytes).is_err() {
        return Err(build_err(format!(
            "text asset {} is not valid UTF-8; text resources must be valid UTF-8 \
             (declare it as a non-text extension for opaque bytes)",
            source.display()
        )));
    }
    Ok(())
}

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
    /// Resource type, derived from the source extension (`.txt` → text, else
    /// raw). Drives the `.meta` `type=` field only.
    pub resource_type: ResourceType,
    /// Staged (decompressed) byte length — the source size, plus one for the
    /// text NUL terminator. Equals the runtime entry's `e->len` (footprint unit),
    /// so the persistent-budget guard (#160) sums this field.
    pub staged_len: usize,
}

/// Map a path relative to a resource dir to a resource name per ADR-0088's
/// 8-step transform: strip the extension; NFD-decompose and drop combining
/// diacritical marks (so `café`→`cafe`); replace separators and any remaining
/// non-identifier character with `_`; collapse runs of `_`; trim
/// leading/trailing `_`; lowercase. Per ADR-0088's build-errors table, rejects
/// (rather than silently mangling): a stem empty after sanitisation, a
/// normalized name beginning with a digit, and a non-ASCII character that
/// survives diacritic removal (CJK, ß, emoji — no ASCII fold exists).
pub(super) fn resource_name_from_rel(rel: &Path) -> Result<String, BuildError> {
    let without_ext = rel.with_extension("");
    let raw = without_ext.to_string_lossy();

    // ADR-0088 step 3: NFD decomposition, then drop combining diacritical marks
    // (U+0300–U+036F) so accented letters fold to their ASCII base.
    let decomposed: String = raw
        .nfd()
        .filter(|ch| !matches!(ch, '\u{0300}'..='\u{036F}'))
        .collect();

    // ADR-0088 build error: a character with no ASCII fold (CJK, ß, emoji, …)
    // survives diacritic removal. Reject rather than underscore it away — the
    // alternative silently mangles the constant.
    if let Some(ch) = decomposed.chars().find(|ch| !ch.is_ascii()) {
        return Err(build_err(format!(
            "asset {} contains non-ASCII character {ch:?} with no ASCII \
             equivalent after diacritic removal; resource names must be ASCII",
            rel.display()
        )));
    }

    // ADR-0088 step 4: separators, dashes, spaces, dots, and anything else that
    // is not a C/Lua identifier character collapse to `_`.
    let mut subbed = String::with_capacity(decomposed.len());
    for ch in decomposed.chars() {
        if ch.is_ascii_alphanumeric() || ch == '_' {
            subbed.push(ch);
        } else {
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

    // ADR-0088 build error: a normalized identifier beginning with a digit is
    // not a valid C/Lua identifier (`R_3D` aside, the Lua `R.` key `R.3D` is
    // invalid) — reject rather than emit an uncompilable constant.
    if trimmed.starts_with(|ch: char| ch.is_ascii_digit()) {
        return Err(build_err(format!(
            "asset {} derives resource name `{trimmed}`, which begins with a \
             digit; resource constants must start with a letter or underscore",
            rel.display()
        )));
    }

    Ok(trimmed.to_ascii_lowercase())
}

/// Uppercase resource name with the `R_` prefix, e.g. `greeting` → `R_GREETING`.
fn c_constant(name: &str) -> String {
    format!("R_{}", name.to_ascii_uppercase())
}

// -------------------------------------------------------------------------
// `assets:` block of blyt.build.yaml (ADR-0088 amendment, #162)
//
//   assets:
//     dirs:
//       - dir: assets/          # default dir; entry optional
//         include: ["**/*.txt", "**/*.lvl"]
//         exclude: ["wip/**"]
//       - dir: other_assets/
//         constant_prefix: DATA # required for any non-assets/ dir
//         include: ["**/*.dat"]
//
// Membership of a dir = include-matched files minus exclude-matched files
// (auto-scan of processed types is reserved for when those transforms land).
// Globs are relative to the dir's root. `constant_prefix` is prepended to each
// derived resource name (empty default for assets/, required otherwise).
// -------------------------------------------------------------------------

#[derive(serde::Deserialize, Debug, Clone)]
#[serde(deny_unknown_fields)]
pub(super) struct AssetsConfig {
    /// Extensions whose resources are `text` (default `["txt"]`); everything
    /// else is `raw` opaque bytes (ADR-0088 amendment 2026-06-27, #166).
    #[serde(default = "default_text_extensions")]
    pub text_extensions: Vec<String>,
    #[serde(default)]
    pub dirs: Vec<AssetDir>,
}

impl Default for AssetsConfig {
    fn default() -> Self {
        // A manifest with no `assets:` block (or no file at all) still gets the
        // default text-extension set, matching a present-but-empty block.
        Self {
            text_extensions: default_text_extensions(),
            dirs: Vec::new(),
        }
    }
}

#[derive(serde::Deserialize, Debug, Clone)]
#[serde(deny_unknown_fields)]
pub(super) struct AssetDir {
    pub dir: String,
    #[serde(default)]
    pub include: Vec<String>,
    #[serde(default)]
    pub exclude: Vec<String>,
    #[serde(default)]
    pub constant_prefix: Option<String>,
}

/// Lean view over blyt.build.yaml that captures only the `assets:` block and
/// ignores the language/build fields (no `deny_unknown_fields`).
#[derive(serde::Deserialize, Default)]
struct AssetsManifest {
    #[serde(default)]
    assets: Option<AssetsConfig>,
}

/// Read the `assets:` block from `<project_dir>/blyt.build.yaml`. Returns an
/// empty config when the file is absent or has no `assets:` block.
pub(super) fn read_assets_config(project_dir: &Path) -> Result<AssetsConfig, BuildError> {
    let path = project_dir.join("blyt.build.yaml");
    if !path.exists() {
        return Ok(AssetsConfig::default());
    }
    let text = fs::read_to_string(&path)?;
    let manifest: AssetsManifest =
        serde_yaml::from_str(&text).map_err(|e| build_err(format!("blyt.build.yaml: {e}")))?;
    Ok(manifest.assets.unwrap_or_default())
}

/// Absolute paths of every declared asset directory (the default `assets/`
/// plus any `assets.dirs` entries) that currently exists. Dev-mode watch uses
/// this so edits in additional dirs hot-swap like edits in `assets/` (#162).
pub(super) fn watch_dirs(project_dir: &Path) -> Result<Vec<PathBuf>, BuildError> {
    let config = read_assets_config(project_dir)?;
    let dirs = resolve_asset_dirs(&config)?;
    let mut out = Vec::new();
    for d in &dirs {
        let abs = project_dir.join(normalize_dir(&d.dir));
        if abs.is_dir() {
            out.push(abs);
        }
    }
    Ok(out)
}

/// A declared asset directory resolved to its constant prefix and glob lists.
struct ResolvedDir {
    /// Directory path relative to the project root, e.g. `assets/`.
    dir: String,
    /// Constant prefix (empty for `assets/`), folded into each resource name.
    prefix: String,
    include: Vec<String>,
    exclude: Vec<String>,
    /// Explicitly declared in the manifest (vs the implicit default `assets/`).
    /// A missing explicit dir is an error; a missing implicit one is skipped.
    explicit: bool,
}

/// Strip a trailing slash so `assets/` and `assets` compare equal.
fn normalize_dir(dir: &str) -> &str {
    dir.strip_suffix('/').unwrap_or(dir)
}

/// `constant_prefix` must already be a valid uppercase identifier fragment
/// (`[A-Z0-9_]*`; empty permitted) — the packer prepends it verbatim and does
/// not transform it (ADR-0088).
fn validate_prefix(p: &str) -> Result<(), BuildError> {
    if p.chars()
        .all(|c| c.is_ascii_uppercase() || c.is_ascii_digit() || c == '_')
    {
        Ok(())
    } else {
        Err(build_err(format!(
            "blyt.build.yaml: constant_prefix {p:?} must contain only [A-Z0-9_]"
        )))
    }
}

/// Resolve the declared dirs, adding the implicit default `assets/` (empty
/// prefix) when it is not declared, and validating prefixes. `assets/` may omit
/// `constant_prefix`; every other dir must declare one.
fn resolve_asset_dirs(config: &AssetsConfig) -> Result<Vec<ResolvedDir>, BuildError> {
    let mut dirs: Vec<ResolvedDir> = Vec::new();
    let mut has_default = false;
    for entry in &config.dirs {
        let is_default = normalize_dir(&entry.dir) == "assets";
        has_default |= is_default;
        let prefix = match &entry.constant_prefix {
            Some(p) => {
                validate_prefix(p)?;
                p.clone()
            }
            None if is_default => String::new(),
            None => {
                return Err(build_err(format!(
                    "blyt.build.yaml: asset dir {:?} must declare a `constant_prefix` \
                     (only assets/ may omit it)",
                    entry.dir
                )));
            }
        };
        dirs.push(ResolvedDir {
            dir: entry.dir.clone(),
            prefix,
            include: entry.include.clone(),
            exclude: entry.exclude.clone(),
            explicit: true,
        });
    }
    if !has_default {
        // The default dir is always a member dir; with no includes (and no
        // auto-scan types yet) it simply contributes nothing.
        dirs.insert(
            0,
            ResolvedDir {
                dir: "assets/".to_string(),
                prefix: String::new(),
                include: Vec::new(),
                exclude: Vec::new(),
                explicit: false,
            },
        );
    }
    Ok(dirs)
}

/// Build a `GlobSet` from dir-root-relative patterns. An empty pattern list
/// yields a set that matches nothing.
fn build_globset(patterns: &[String], dir: &str) -> Result<GlobSet, BuildError> {
    let mut builder = GlobSetBuilder::new();
    for p in patterns {
        let glob = Glob::new(p).map_err(|e| {
            build_err(format!(
                "blyt.build.yaml: dir {dir:?}: invalid glob {p:?}: {e}"
            ))
        })?;
        builder.add(glob);
    }
    builder
        .build()
        .map_err(|e| build_err(format!("blyt.build.yaml: dir {dir:?}: bad globs: {e}")))
}

/// Collect every file under `dir`, returned as paths relative to `root`.
fn collect_files(root: &Path, dir: &Path, out: &mut Vec<PathBuf>) -> Result<(), BuildError> {
    for entry in fs::read_dir(dir)? {
        let entry = entry?;
        let path = entry.path();
        if path.is_dir() {
            collect_files(root, &path, out)?;
        } else {
            out.push(path.strip_prefix(root).unwrap_or(&path).to_path_buf());
        }
    }
    Ok(())
}

/// Scan the declared asset directories (ADR-0088 amendment) and resolve each
/// included file to an `AssetInfo` with a content fingerprint and
/// content-addressed staging paths under `<top_build>/resources/`. Membership
/// is include-minus-exclude per dir; the resource type is by extension. The set
/// is sorted by resource name and assigned 1-based integer IDs. Errors on a name
/// collision (two files — possibly in different dirs — deriving the same name).
pub(super) fn scan_assets(
    project_dir: &Path,
    top_build: &Path,
) -> Result<Vec<AssetInfo>, BuildError> {
    let config = read_assets_config(project_dir)?;
    let dirs = resolve_asset_dirs(&config)?;

    let resources_dir = top_build.join("resources");
    let mut assets: Vec<AssetInfo> = Vec::new();
    for d in &dirs {
        let abs = project_dir.join(normalize_dir(&d.dir));
        if !abs.exists() {
            if d.explicit {
                return Err(build_err(format!(
                    "blyt.build.yaml: asset dir {:?} does not exist",
                    d.dir
                )));
            }
            continue;
        }

        let include = build_globset(&d.include, &d.dir)?;
        let exclude = build_globset(&d.exclude, &d.dir)?;

        let mut files = Vec::new();
        collect_files(&abs, &abs, &mut files)?;
        files.sort();

        for rel in files {
            let resource_type = ResourceType::from_extension(
                rel.extension().and_then(OsStr::to_str),
                &config.text_extensions,
            );
            // Membership: an include match, OR a processed type (palette)
            // auto-scanned by extension (ADR-0088 amendment, #214). An `exclude`
            // still drops either.
            let included = include.is_match(&rel) || resource_type.is_processed();
            if !included || exclude.is_match(&rel) {
                continue;
            }
            let base = resource_name_from_rel(&rel)?;
            let resource_name = if d.prefix.is_empty() {
                base
            } else {
                format!("{}_{}", d.prefix.to_ascii_lowercase(), base)
            };
            let source = abs.join(&rel);
            let bytes = fs::read(&source)?;
            let fingerprint = format!("{:016x}", xxh3_64(&bytes));
            let rel_data = format!("resources/{resource_name}-{fingerprint}.data");
            let data_output = resources_dir.join(format!("{resource_name}-{fingerprint}.data"));
            let meta_output = resources_dir.join(format!("{resource_name}-{fingerprint}.meta"));
            // The staged `.data` length (the runtime entry's `e->len`, summed by
            // the persistent-budget guard #160): a palette is a fixed 1024-byte
            // XRGB8888 table, text is the source bytes plus one NUL, raw is the
            // source bytes verbatim.
            let staged_len = match resource_type {
                ResourceType::Palette => super::palette::PALETTE_DATA_LEN,
                ResourceType::Text => bytes.len() + 1,
                ResourceType::Raw => bytes.len(),
            };
            assets.push(AssetInfo {
                id: 0, // assigned below
                resource_name,
                source,
                fingerprint,
                rel_data,
                data_output,
                meta_output,
                resource_type,
                staged_len,
            });
        }
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

/// Minimal `.meta` for an asset. Establishes the per-resource metadata pattern
/// future asset types fill out (dimensions, format, …); the runtime serves the
/// `.data` bytes and never reads `.meta`, so the `type=` field is descriptive
/// only (`text` vs `raw`, by extension).
fn meta_contents(a: &AssetInfo) -> String {
    format!(
        "type={}\nname={}\n",
        a.resource_type.meta_name(),
        a.resource_name
    )
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
/// resource (ADR-0040), typed by kind (ADR-0068 amendment 2026-06-27, #166): a
/// text resource is a `blyt_text_resource_t`, a raw one a `blyt_bytes_resource_t`
/// (both `uint32_t` aliases — the typing steers callers to the matching
/// accessor, not a compile barrier). Always includes `<blyt.h>` for the
/// typedefs.
pub(super) fn generate_c_header(assets: &[AssetInfo]) -> String {
    let mut s = String::new();
    s.push_str("/* Generated by `blyt build` — do not edit. */\n");
    s.push_str("#pragma once\n");
    s.push_str("#include <blyt.h>\n\n");
    for a in assets {
        s.push_str(&format!(
            "#define {} (({})0x{:08X}u)\n",
            c_constant(&a.resource_name),
            a.resource_type.c_type(),
            super::handle::resource_encode_cart(a.id)
        ));
    }
    s
}

/// Generated Lua module (`cart_resources.lua`): a `require`-able table mapping
/// upper-cased resource names to typed constant objects (ADR-0040, ADR-0068
/// amendment 2026-06-27): `GREETING = blyt.resource.text_resource(0x20000001)`
/// for text, `BLOB = blyt.resource.bytes_resource(0x20000002)` for raw — the id
/// is the baked console-wide constant (ADR-0134). `R.X:text()` / `R.X:bytes()`
/// read the bytes directly (no load handle).
pub(super) fn generate_lua_module(assets: &[AssetInfo]) -> String {
    let mut s = String::new();
    s.push_str("-- Generated by `blyt build` — do not edit.\n");
    s.push_str("return {\n");
    for a in assets {
        s.push_str(&format!(
            "    {} = {}(0x{:08X}),\n",
            a.resource_name.to_ascii_uppercase(),
            a.resource_type.lua_ctor(),
            super::handle::resource_encode_cart(a.id)
        ));
    }
    s.push_str("}\n");
    s
}

/// Generated Rust module (`cart_resources.rs`): one `R_<NAME>` constant per
/// resource (ADR-0040), typed by kind (ADR-0068 amendment 2026-06-27, #166):
/// `TextResource` for text, `BytesResource` for raw. Misusing the text accessor
/// on a bytes resource is a compile error. The cart pulls it in with
/// `include!(env!("BLYT_CART_RESOURCES_RS"))`; both `::new` constructors are
/// const fns, so each constant is usable in const context.
pub(super) fn generate_rust_module(assets: &[AssetInfo]) -> String {
    let mut s = String::new();
    s.push_str("// Generated by `blyt build` — do not edit.\n");
    for a in assets {
        let ty = a.resource_type.rust_type();
        s.push_str(&format!(
            "pub const {}: blyt::{ty} = blyt::{ty}::new(0x{:08X});\n",
            c_constant(&a.resource_name),
            super::handle::resource_encode_cart(a.id)
        ));
    }
    s
}

/* -------------------------------------------------------------------------
 * Tasks
 * ------------------------------------------------------------------------- */

/// Phase 1 transform for a single asset: write the content-addressed `.data`
/// and its `.meta`. A no-op when the content-addressed outputs already exist
/// (handled by the engine via the content fingerprint). A `text` resource is
/// validated (UTF-8, no embedded NUL) and gets a single trailing `\0` appended
/// to its staged bytes (ADR-0088 amendment 2026-06-27, #166); `raw` is copied
/// byte-exact.
pub(super) struct AssetTask {
    pub resource_name: String,
    pub source: PathBuf,
    pub data_output: PathBuf,
    pub meta_output: PathBuf,
    pub meta: String,
    /// Text resources are validated and NUL-terminated; raw is byte-exact.
    pub is_text: bool,
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
        if self.is_text {
            // Text: validate (UTF-8, no embedded NUL) then append one trailing
            // `\0` for C-string safety (ADR-0088 amendment 2026-06-27, #166).
            // The reported length will include the NUL; only the guest text
            // accessors strip it. Raw stays byte-exact below.
            let mut bytes = fs::read(&self.source)?;
            validate_text_bytes(&self.source, &bytes)?;
            bytes.push(0);
            fs::write(&self.data_output, &bytes)?;
        } else {
            fs::copy(&self.source, &self.data_output)?;
        }
        fs::write(&self.meta_output, &self.meta)?;
        Ok(())
    }
}

/// Phase 1 transform for a palette asset (#214): parse the `.hex`/`.gpl`/`.pal`
/// source into a canonical 1024-byte XRGB8888 table (LE) and write it as the
/// staged `.data`. Unlike a text/raw asset this is a *processed* type — parse
/// errors (bad hex, out-of-range component, > 256 colors, wrong header) surface
/// here as build errors. A no-op when the content-addressed outputs already
/// exist (the engine keys on the source fingerprint).
pub(super) struct PaletteTask {
    pub resource_name: String,
    pub source: PathBuf,
    pub data_output: PathBuf,
    pub meta_output: PathBuf,
    pub meta: String,
    pub format: super::palette::PaletteFormat,
    pub key_str: String,
}

impl Task for PaletteTask {
    fn key(&self) -> &str {
        &self.key_str
    }
    fn label(&self) -> String {
        format!("palette  {}", self.resource_name)
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
        let text = fs::read_to_string(&self.source)
            .map_err(|e| build_err(format!("palette asset {}: {e}", self.source.display())))?;
        let bytes = super::palette::parse_to_bytes(self.format, &text)?;
        fs::write(&self.data_output, &bytes)?;
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
        if a.resource_type == ResourceType::Palette {
            // A processed type: parse+validate the palette file into its fixed
            // 1024-byte staged form (#214), rather than the identity/text copy.
            let format = a
                .source
                .extension()
                .and_then(OsStr::to_str)
                .and_then(super::palette::PaletteFormat::from_extension)
                .expect("palette resource_type implies a palette extension");
            tasks.push(Box::new(PaletteTask {
                resource_name: a.resource_name.clone(),
                source: a.source.clone(),
                data_output: a.data_output.clone(),
                meta_output: a.meta_output.clone(),
                meta: meta_contents(a),
                format,
                key_str: format!("palette/{}", a.resource_name),
            }));
        } else {
            tasks.push(Box::new(AssetTask {
                resource_name: a.resource_name.clone(),
                source: a.source.clone(),
                data_output: a.data_output.clone(),
                meta_output: a.meta_output.clone(),
                meta: meta_contents(a),
                is_text: a.resource_type.is_text(),
                key_str: format!("asset/{}", a.resource_name),
            }));
        }
        // The packed `.blyt` embeds the *compressed* form (#157, ADR-0026): a
        // PackResourceTask turns the staged `.data` into a `.res` blob (8-byte
        // header + raw|zstd body), and that blob — not the raw `.data` — becomes
        // the `.cart.resource.<id>` section. Dev mode keeps reading the staging
        // `.data` directly, so it stays uncompressed.
        let packed_output = a.data_output.with_extension("res");
        tasks.push(Box::new(super::resource_pack::PackResourceTask {
            resource_name: a.resource_name.clone(),
            staged_data: a.data_output.clone(),
            packed_output: packed_output.clone(),
            key_str: format!("pack/{}", a.resource_name),
        }));
        resource_sections.push((format!(".cart.resource.{}", a.id), packed_output));
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
    fn name_transform_strips_diacritics() {
        // ADR-0088 step 3: NFD decomposition + combining-mark removal folds
        // accented letters to their ASCII base rather than dropping them.
        assert_eq!(name("café.txt"), "cafe");
        assert_eq!(name("piñata.txt"), "pinata");
        assert_eq!(name("über/Ähnlich.txt"), "uber_ahnlich");
        assert_eq!(name("menu/Crème Brûlée.txt"), "menu_creme_brulee");
    }

    #[test]
    fn name_transform_rejects_empty() {
        assert!(resource_name_from_rel(Path::new("--.txt")).is_err());
        assert!(resource_name_from_rel(Path::new("__.txt")).is_err());
    }

    #[test]
    fn name_transform_rejects_leading_digit() {
        // ADR-0088 build error: a normalized identifier beginning with a digit
        // is not a valid C/Lua identifier — reject rather than emit `R_3D`.
        assert!(resource_name_from_rel(Path::new("3d.txt")).is_err());
        assert!(resource_name_from_rel(Path::new("3d/model.txt")).is_err());
        assert!(resource_name_from_rel(Path::new("2x-scale.txt")).is_err());
        // A digit elsewhere is fine.
        assert_eq!(name("level3.txt"), "level3");
        assert_eq!(name("sfx/jump 1.wav"), "sfx_jump_1");
    }

    #[test]
    fn name_transform_rejects_residual_non_ascii() {
        // ADR-0088 build error: characters with no ASCII fold (CJK, ß, emoji)
        // survive diacritic removal and must be rejected, not underscored.
        assert!(resource_name_from_rel(Path::new("日本.txt")).is_err());
        assert!(resource_name_from_rel(Path::new("hero_日.txt")).is_err());
        assert!(resource_name_from_rel(Path::new("party🎉.txt")).is_err());
    }

    fn asset(id: u32, n: &str, fp: &str) -> AssetInfo {
        asset_of(id, n, fp, ResourceType::Text)
    }

    fn asset_of(id: u32, n: &str, fp: &str, ty: ResourceType) -> AssetInfo {
        AssetInfo {
            id,
            resource_name: n.to_string(),
            source: PathBuf::from(format!("assets/{n}.txt")),
            fingerprint: fp.to_string(),
            rel_data: format!("resources/{n}-{fp}.data"),
            data_output: PathBuf::from(format!("build/resources/{n}-{fp}.data")),
            meta_output: PathBuf::from(format!("build/resources/{n}-{fp}.meta")),
            resource_type: ty,
            staged_len: 0,
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
    fn c_header_constants_typed_by_kind() {
        // text -> blyt_text_resource_t, raw -> blyt_bytes_resource_t (#166).
        let assets = vec![
            asset_of(1, "greeting", "abc123", ResourceType::Text),
            asset_of(2, "blob", "def456", ResourceType::Raw),
        ];
        let h = generate_c_header(&assets);
        assert!(h.contains("#include <blyt.h>"));
        // Constants are baked in the console-wide encoding (ADR-0134): kind
        // RESOURCE (1) in the top 3 bits, provenance 0, id in the low 24 bits.
        assert!(h.contains("#define R_GREETING ((blyt_text_resource_t)0x20000001u)"));
        assert!(h.contains("#define R_BLOB ((blyt_bytes_resource_t)0x20000002u)"));
    }

    #[test]
    fn lua_module_typed_constants() {
        let assets = vec![
            asset_of(1, "greeting", "abc123", ResourceType::Text),
            asset_of(2, "blob", "def456", ResourceType::Raw),
        ];
        let m = generate_lua_module(&assets);
        assert!(m.contains("return {"));
        assert!(m.contains("GREETING = blyt.resource.text_resource(0x20000001),"));
        assert!(m.contains("BLOB = blyt.resource.bytes_resource(0x20000002),"));
    }

    #[test]
    fn palette_constants_typed_by_kind() {
        // A palette asset is a PROV_CART resource (0x20…), typed `Palette` in
        // each language (#214), consistent with the #166 typed-by-kind scheme.
        let assets = vec![asset_of(1, "main", "abc123", ResourceType::Palette)];
        assert!(
            generate_c_header(&assets).contains("#define R_MAIN ((blyt_palette_t)0x20000001u)")
        );
        assert!(generate_lua_module(&assets).contains("MAIN = blyt.resource.palette(0x20000001),"));
        assert!(
            generate_rust_module(&assets)
                .contains("pub const R_MAIN: blyt::Palette = blyt::Palette::new(0x20000001);")
        );
    }

    #[test]
    fn rust_module_typed_constants() {
        let assets = vec![
            asset_of(1, "greeting", "abc123", ResourceType::Text),
            asset_of(2, "blob", "def456", ResourceType::Raw),
        ];
        let m = generate_rust_module(&assets);
        assert!(m.contains(
            "pub const R_GREETING: blyt::TextResource = blyt::TextResource::new(0x20000001);"
        ));
        assert!(m.contains(
            "pub const R_BLOB: blyt::BytesResource = blyt::BytesResource::new(0x20000002);"
        ));
    }

    /* --- include-driven membership + type-by-extension (#162) --- */

    /// Write a throwaway project with the given `blyt.build.yaml` body and asset
    /// files, then scan it. `files` are `(rel_path_under_project, bytes)`.
    fn scan(build_yaml: &str, files: &[(&str, &[u8])]) -> Result<Vec<AssetInfo>, BuildError> {
        let dir = tempfile::TempDir::new().unwrap();
        let root = dir.path();
        if !build_yaml.is_empty() {
            fs::write(root.join("blyt.build.yaml"), build_yaml).unwrap();
        }
        for (rel, bytes) in files {
            let p = root.join(rel);
            fs::create_dir_all(p.parent().unwrap()).unwrap();
            fs::write(p, bytes).unwrap();
        }
        scan_assets(root, &root.join("build"))
    }

    fn names(assets: &[AssetInfo]) -> Vec<String> {
        assets.iter().map(|a| a.resource_name.clone()).collect()
    }

    #[test]
    fn glob_double_star_matches_top_level() {
        // `**/*.txt` must match a file directly under the dir root, not only in
        // a subdirectory.
        let set = build_globset(&["**/*.txt".to_string()], "assets/").unwrap();
        assert!(set.is_match(Path::new("greeting.txt")));
        assert!(set.is_match(Path::new("sub/greeting.txt")));
        assert!(!set.is_match(Path::new("greeting.bin")));
    }

    #[test]
    fn nothing_is_auto_scanned_without_include() {
        // A bare assets/ with a .txt but no `assets:` declaration ships nothing
        // (no default passthrough types — #162).
        let assets = scan("language: lua\n", &[("assets/greeting.txt", b"hi")]).unwrap();
        assert!(
            assets.is_empty(),
            "expected no assets, got {:?}",
            names(&assets)
        );
    }

    #[test]
    fn include_selects_and_type_is_by_extension() {
        let yaml = "language: lua\n\
                    assets:\n  dirs:\n    - dir: assets/\n      include: [\"**/*.txt\", \"**/*.lvl\"]\n";
        let assets = scan(
            yaml,
            &[
                ("assets/greeting.txt", b"hi"),
                ("assets/level1.lvl", b"\x00\x01\x02"),
                ("assets/notes.md", b"ignored"),
            ],
        )
        .unwrap();
        assert_eq!(names(&assets), vec!["greeting", "level1"]);
        let txt = assets
            .iter()
            .find(|a| a.resource_name == "greeting")
            .unwrap();
        let lvl = assets.iter().find(|a| a.resource_name == "level1").unwrap();
        assert_eq!(txt.resource_type, ResourceType::Text);
        assert_eq!(lvl.resource_type, ResourceType::Raw);
        assert_eq!(meta_contents(txt), "type=text\nname=greeting\n");
        assert_eq!(meta_contents(lvl), "type=raw\nname=level1\n");
    }

    #[test]
    fn palette_files_are_auto_scanned_without_include() {
        // A processed type (palette) is a member by extension alone — no
        // `include:` glob needed (#214), unlike text/raw which need one.
        let assets = scan(
            "language: lua\n",
            &[
                ("assets/main.hex", b"FF0000\n00FF00\n"),
                ("assets/notes.txt", b"ignored"),
            ],
        )
        .unwrap();
        assert_eq!(names(&assets), vec!["main"]);
        let pal = &assets[0];
        assert_eq!(pal.resource_type, ResourceType::Palette);
        // Palette stages a fixed 1024-byte table regardless of source size.
        assert_eq!(pal.staged_len, 1024);
        assert_eq!(meta_contents(pal), "type=palette\nname=main\n");
    }

    #[test]
    fn exclude_still_drops_auto_scanned_palettes() {
        // Auto-scan does not bypass `exclude:` (#214).
        let yaml = "language: lua\n\
                    assets:\n  dirs:\n    - dir: assets/\n      exclude: [\"wip/**\"]\n";
        let assets = scan(
            yaml,
            &[
                ("assets/keep.gpl", b"GIMP Palette\n0 0 0\n"),
                ("assets/wip/draft.gpl", b"GIMP Palette\n0 0 0\n"),
            ],
        )
        .unwrap();
        assert_eq!(names(&assets), vec!["keep"]);
    }

    #[test]
    fn exclude_drops_included_files() {
        let yaml = "language: lua\n\
                    assets:\n  dirs:\n    - dir: assets/\n      include: [\"**/*.dat\"]\n      exclude: [\"wip/**\"]\n";
        let assets = scan(
            yaml,
            &[("assets/keep.dat", b"a"), ("assets/wip/draft.dat", b"b")],
        )
        .unwrap();
        assert_eq!(names(&assets), vec!["keep"]);
    }

    #[test]
    fn constant_prefix_folds_into_resource_name() {
        let yaml = "language: lua\n\
                    assets:\n  dirs:\n    - dir: other_assets/\n      constant_prefix: DATA\n      include: [\"**/*.dat\"]\n";
        let assets = scan(yaml, &[("other_assets/level.dat", b"x")]).unwrap();
        assert_eq!(names(&assets), vec!["data_level"]);
        assert!(generate_c_header(&assets).contains("#define R_DATA_LEVEL"));
    }

    #[test]
    fn non_assets_dir_requires_constant_prefix() {
        let yaml = "language: lua\n\
                    assets:\n  dirs:\n    - dir: other_assets/\n      include: [\"**/*.dat\"]\n";
        let err = scan(yaml, &[("other_assets/level.dat", b"x")])
            .err()
            .expect("missing constant_prefix should be rejected");
        assert!(err.to_string().contains("constant_prefix"), "{err}");
    }

    #[test]
    fn invalid_constant_prefix_rejected() {
        let yaml = "language: lua\n\
                    assets:\n  dirs:\n    - dir: other_assets/\n      constant_prefix: data\n      include: [\"**/*.dat\"]\n";
        let err = scan(yaml, &[("other_assets/level.dat", b"x")])
            .err()
            .expect("lowercase constant_prefix should be rejected");
        assert!(err.to_string().contains("[A-Z0-9_]"), "{err}");
    }

    #[test]
    fn cross_dir_name_collision_errors() {
        let yaml = "language: lua\n\
                    assets:\n  dirs:\n    - dir: assets/\n      include: [\"**/*.dat\"]\n    - dir: other_assets/\n      constant_prefix: \"\"\n      include: [\"**/*.dat\"]\n";
        let err = scan(
            yaml,
            &[("assets/level.dat", b"a"), ("other_assets/level.dat", b"b")],
        )
        .err()
        .expect("colliding names across dirs should be rejected");
        assert!(err.to_string().contains("collision"), "{err}");
    }

    #[test]
    fn raw_bytes_pass_through_verbatim() {
        // Opaque bytes (embedded NUL, high bytes) survive the identity copy and
        // drive the content fingerprint.
        let raw: &[u8] = &[0x00, 0xFF, 0x10, b'h', b'i', 0x00];
        let yaml = "language: lua\n\
                    assets:\n  dirs:\n    - dir: assets/\n      include: [\"**/*.bin\"]\n";
        let assets = scan(yaml, &[("assets/blob.bin", raw)]).unwrap();
        assert_eq!(names(&assets), vec!["blob"]);
        assert_eq!(assets[0].resource_type, ResourceType::Raw);
        assert_eq!(assets[0].fingerprint, format!("{:016x}", xxh3_64(raw)));
    }

    /* --- text_extensions + text validation + NUL termination (#166) --- */

    #[test]
    fn from_extension_consults_text_extensions() {
        let txt = vec!["txt".to_string()];
        assert_eq!(
            ResourceType::from_extension(Some("txt"), &txt),
            ResourceType::Text
        );
        assert_eq!(
            ResourceType::from_extension(Some("dat"), &txt),
            ResourceType::Raw
        );
        assert_eq!(ResourceType::from_extension(None, &txt), ResourceType::Raw);
        // A custom list makes `.md` text and demotes `.txt` to raw.
        let md = vec!["md".to_string()];
        assert_eq!(
            ResourceType::from_extension(Some("md"), &md),
            ResourceType::Text
        );
        assert_eq!(
            ResourceType::from_extension(Some("txt"), &md),
            ResourceType::Raw
        );
    }

    #[test]
    fn palette_extensions_dispatch_to_processed_type() {
        let txt = vec!["txt".to_string()];
        for ext in ["hex", "gpl", "pal", "GPL"] {
            assert_eq!(
                ResourceType::from_extension(Some(ext), &txt),
                ResourceType::Palette,
                "{ext} should be a palette"
            );
        }
        // Reserved palette extensions win even if listed in text_extensions.
        let hex_as_text = vec!["hex".to_string()];
        assert_eq!(
            ResourceType::from_extension(Some("hex"), &hex_as_text),
            ResourceType::Palette
        );
        assert!(ResourceType::Palette.is_processed());
        assert!(!ResourceType::Text.is_processed());
        assert!(!ResourceType::Raw.is_processed());
    }

    #[test]
    fn default_text_extensions_when_block_present_but_omits_it() {
        // An `assets:` block that omits `text_extensions` still defaults to
        // ["txt"]; `.txt` stays text.
        let yaml = "language: lua\n\
                    assets:\n  dirs:\n    - dir: assets/\n      include: [\"**/*.txt\"]\n";
        let assets = scan(yaml, &[("assets/greeting.txt", b"hi")]).unwrap();
        assert_eq!(assets[0].resource_type, ResourceType::Text);
    }

    #[test]
    fn custom_text_extensions_retypes_resources() {
        // Declaring text_extensions: [md] makes .md text and .txt raw.
        let yaml = "language: lua\n\
                    assets:\n  text_extensions: [md]\n  dirs:\n    - dir: assets/\n      include: [\"**/*.md\", \"**/*.txt\"]\n";
        let assets = scan(
            yaml,
            &[("assets/readme.md", b"# hi"), ("assets/note.txt", b"hi")],
        )
        .unwrap();
        let md = assets.iter().find(|a| a.resource_name == "readme").unwrap();
        let txt = assets.iter().find(|a| a.resource_name == "note").unwrap();
        assert_eq!(md.resource_type, ResourceType::Text);
        assert_eq!(txt.resource_type, ResourceType::Raw);
    }

    #[test]
    fn validate_text_bytes_accepts_valid_utf8() {
        assert!(validate_text_bytes(Path::new("a.txt"), "héllo 🎉".as_bytes()).is_ok());
        assert!(validate_text_bytes(Path::new("a.txt"), b"").is_ok());
    }

    #[test]
    fn validate_text_bytes_rejects_embedded_nul() {
        let err = validate_text_bytes(Path::new("a.txt"), b"hi\x00there")
            .err()
            .expect("embedded NUL must be rejected");
        assert!(err.to_string().contains("NUL"), "{err}");
        assert!(err.to_string().contains("a.txt"), "{err}");
    }

    #[test]
    fn validate_text_bytes_rejects_invalid_utf8() {
        let err = validate_text_bytes(Path::new("bad.txt"), &[0xFF, 0xFE])
            .err()
            .expect("invalid UTF-8 must be rejected");
        assert!(err.to_string().contains("UTF-8"), "{err}");
        assert!(err.to_string().contains("bad.txt"), "{err}");
    }

    /// Run an `AssetTask` over `bytes` and return the staged `.data` contents.
    fn run_asset_task(bytes: &[u8], is_text: bool) -> Result<Vec<u8>, BuildError> {
        let dir = tempfile::TempDir::new().unwrap();
        let src = dir.path().join("in.dat");
        fs::write(&src, bytes).unwrap();
        let data_output = dir.path().join("out.data");
        let meta_output = dir.path().join("out.meta");
        let task = AssetTask {
            resource_name: "r".into(),
            source: src,
            data_output: data_output.clone(),
            meta_output,
            meta: "type=x\nname=r\n".into(),
            is_text,
            key_str: "asset/r".into(),
        };
        task.run()?;
        Ok(fs::read(&data_output).unwrap())
    }

    #[test]
    fn asset_task_nul_terminates_text() {
        // Text gets exactly one trailing NUL; the reported/stored length is
        // content+1 (#166).
        let out = run_asset_task(b"hi", true).unwrap();
        assert_eq!(out, b"hi\x00");
    }

    #[test]
    fn asset_task_raw_is_byte_exact() {
        // Raw is copied verbatim — no trailing NUL, even when the bytes already
        // end in 0x00.
        let raw: &[u8] = &[0x00, 0xFF, b'h', b'i', 0x00];
        let out = run_asset_task(raw, false).unwrap();
        assert_eq!(out, raw);
    }

    #[test]
    fn asset_task_text_validation_fails_on_embedded_nul() {
        assert!(run_asset_task(b"hi\x00bye", true).is_err());
    }

    /// Run a `PaletteTask` over `src_text` (`.hex`/`.gpl`/`.pal` per `format`)
    /// and return the staged `.data` bytes.
    fn run_palette_task(
        src_text: &str,
        format: super::super::palette::PaletteFormat,
    ) -> Result<Vec<u8>, BuildError> {
        let dir = tempfile::TempDir::new().unwrap();
        let src = dir.path().join("in.hex");
        fs::write(&src, src_text).unwrap();
        let data_output = dir.path().join("out.data");
        let meta_output = dir.path().join("out.meta");
        let task = PaletteTask {
            resource_name: "main".into(),
            source: src,
            data_output: data_output.clone(),
            meta_output,
            meta: "type=palette\nname=main\n".into(),
            format,
            key_str: "palette/main".into(),
        };
        task.run()?;
        Ok(fs::read(&data_output).unwrap())
    }

    #[test]
    fn palette_task_stages_1024_le_bytes() {
        use super::super::palette::PaletteFormat;
        let out = run_palette_task("FF0000\n00FF00\n0000FF\n", PaletteFormat::Hex).unwrap();
        assert_eq!(out.len(), 1024);
        // XRGB8888 little-endian (0x00FF0000 -> 00 00 FF 00).
        assert_eq!(&out[0..4], &[0x00, 0x00, 0xFF, 0x00]);
        assert_eq!(&out[4..8], &[0x00, 0xFF, 0x00, 0x00]);
        assert_eq!(&out[8..12], &[0xFF, 0x00, 0x00, 0x00]);
        assert_eq!(&out[12..16], &[0x00, 0x00, 0x00, 0x00]); // padded black
    }

    #[test]
    fn palette_task_reports_parse_errors() {
        use super::super::palette::PaletteFormat;
        assert!(run_palette_task("not-a-color\n", PaletteFormat::Hex).is_err());
    }
}
