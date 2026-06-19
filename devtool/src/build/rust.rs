use std::ffi::OsStr;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::OnceLock;

use crate::engine::{BuildError, Task, TaskInput};

use super::err;

/// Custom cart Rust target (Spike U): RV32IMAFDC / ilp32d hard-double ABI.
/// There is no upstream `riscv32imafdc` target, so a target-spec JSON is shipped
/// in-tree and resolved by name via `RUST_TARGET_PATH` (set in `cargo_cart_cmd`).
/// The bare name (not a path) keeps cargo's output dir as `<name>/release/`.
pub(super) const RUST_TARGET: &str = "riscv32imafdc-blyt-none-elf";

/// The target-spec JSON, compiled into devtool and materialised into each cart's
/// `--target-dir` at build time so cargo can resolve `--target <RUST_TARGET>`.
const RUST_TARGET_SPEC: &str = include_str!("../../targets/riscv32imafdc-blyt-none-elf.json");

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

fn home_dir() -> Option<PathBuf> {
    std::env::var_os("HOME").map(PathBuf::from)
}

/// The pinned cart toolchain's rust-src tree
/// (`<sysroot>/lib/rustlib/src/rust`), whence build-std recompiles
/// core/alloc.  Returns None if the sysroot can't be resolved, in which case
/// the /blyt/rust remap is simply omitted (paths stay absolute, still debuggable
/// locally).
pub(super) fn rust_src_dir() -> Option<PathBuf> {
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
pub(super) fn cargo_registry_src() -> Option<PathBuf> {
    let cargo_home = std::env::var_os("CARGO_HOME")
        .map(PathBuf::from)
        .or_else(|| home_dir().map(|h| h.join(".cargo")))?;
    Some(cargo_home.join("registry/src"))
}

pub(super) struct CompileRustTask {
    pub key_str: String,
    pub label_str: String,
    pub cargo: String,
    pub manifest: PathBuf,
    pub build_dir: PathBuf,
    pub rust_sdk_path: PathBuf,
    pub rust_lib_patches: Vec<(String, PathBuf)>,
    pub extra_rustflags: String,
    pub is_lua: bool,
    pub cart_state_rs: Option<PathBuf>,
    pub output: PathBuf,
    pub source_dir: PathBuf,
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

pub(super) fn find_cargo() -> String {
    if let Ok(c) = std::env::var("BLYT_CARGO") {
        return c;
    }
    "cargo".to_string()
}

/// Finds the `blyt` SDK crate (sdk/rust/blyt/) that game Rust code depends on.
/// Resolution order:
///   1. $BLYT_RUST_SDK — explicit override
///   2. <sdk>/rust/blyt/ — SDK install layout (build/sdk/rust/blyt/)
///   3. Walk up from sdk_include looking for sdk/rust/blyt/ in the repo tree
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

    if let Some(sdk) = super::sdk_root_from_exe() {
        let p = sdk.join("rust/blyt");
        if p.join("Cargo.toml").exists() {
            return Ok(p);
        }
    }

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

fn cart_rustflags(extra: &str) -> String {
    format!("-Zunstable-options -C relocation-model=pic -C panic=abort{extra}")
}

fn cart_sccache() -> Option<&'static str> {
    static SCCACHE: OnceLock<Option<String>> = OnceLock::new();
    SCCACHE
        .get_or_init(|| {
            if std::env::var_os("RUSTC_WRAPPER").is_some() {
                return None;
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

fn cargo_cart_cmd(cargo: &str, manifest: &Path, target_dir: &Path) -> Command {
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

    if is_lua {
        cmd.arg("--features").arg("blyt/lua");
    }

    let mut cargo_cmd = cmd;
    if let Some(rs_path) = cart_state_rs {
        let abs = std::fs::canonicalize(rs_path).unwrap_or_else(|_| rs_path.to_path_buf());
        cargo_cmd.env("BLYT_CART_STATE_RS", abs);
    }
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

    let out_dir = build_dir.join(RUST_TARGET).join("release");
    let archive = find_rust_staticlib(&out_dir)?;
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
