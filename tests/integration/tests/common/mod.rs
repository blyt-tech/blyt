//! Shared test fixtures for blyt integration tests.
#![allow(dead_code)]

use std::fs;
use std::path::PathBuf;

// -------------------------------------------------------------------------
// Path helpers
// -------------------------------------------------------------------------

pub fn repo_root() -> PathBuf {
    // CARGO_MANIFEST_DIR = tests/integration/
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent() // tests/
        .unwrap()
        .parent() // repo root
        .unwrap()
        .to_path_buf()
}

/// CMake build directory.  Overridable via BLYT_BUILD_DIR for CI environments
/// where the build tree lives outside the repo (e.g. the QEMU gate job).
pub fn build_dir() -> PathBuf {
    std::env::var("BLYT_BUILD_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|_| repo_root().join("build"))
}

pub fn sdk_dir() -> PathBuf {
    build_dir().join("sdk")
}

pub fn blytrun() -> PathBuf {
    build_dir().join("blytrun")
}

// -------------------------------------------------------------------------
// Cart project fixture builder
// -------------------------------------------------------------------------

/// Generate the game crate's Cargo.toml, including any Rust lib dependencies.
/// `blyt build` injects the actual source paths via --config at build time.
fn rust_game_cargo_toml(rust_lib_names: &[String]) -> String {
    let mut s = "\
[package]\n\
name = \"cart\"\n\
version = \"0.1.0\"\n\
edition = \"2021\"\n\
publish = false\n\
\n\
[lib]\n\
crate-type = [\"staticlib\"]\n\
\n\
[dependencies]\n\
blyt = \"0.1\"\n"
        .to_string();
    for name in rust_lib_names {
        s.push_str(&format!("{name} = \"0.1\"\n"));
    }
    s
}

/// Generate a minimal Cargo.toml for a Rust library under src/lib/<name>/.
fn rust_lib_cargo_toml(name: &str) -> String {
    format!(
        "[package]\nname = \"{name}\"\nversion = \"0.1.0\"\nedition = \"2021\"\npublish = false\n\n[lib]\n"
    )
}

/// Builder for an on-disk cart project used in integration tests.
///
/// Call `.c(source)` and/or `.rust(lib_rs)` to declare languages, then
/// `.write(dir)` to materialise the project layout under `dir`.  The
/// `cart.build.yaml` is generated automatically from the declared languages.
///
/// ```
/// CartProject::new().c(r#"..."#).write(&project_dir);
/// CartProject::new().rust(r#"..."#).write(&project_dir);
/// CartProject::new().lua(r#"..."#).write(&project_dir);
/// ```
pub struct CartProject {
    /// (filename, source) pairs for src/game/c/
    c_files: Vec<(String, String)>,
    /// (filename, source) pairs for src/game/c++/
    cpp_files: Vec<(String, String)>,
    /// Contents of src/game/rust/src/lib.rs
    rust_lib_rs: Option<String>,
    /// (filename, source) pairs for src/game/lua/
    lua_files: Vec<(String, String)>,
    /// (lib_name, relative_path_within_lib, content) — files for src/lib/<name>/
    lib_files: Vec<(String, String, String)>,
    /// Names of Rust libs declared via rust_lib(); used to generate the game Cargo.toml.
    rust_lib_names: Vec<String>,
}

impl CartProject {
    pub fn new() -> Self {
        CartProject {
            c_files: Vec::new(),
            cpp_files: Vec::new(),
            rust_lib_rs: None,
            lua_files: Vec::new(),
            lib_files: Vec::new(),
            rust_lib_names: Vec::new(),
        }
    }

    /// Add `source` as `src/game/c/main.c`.
    pub fn c(self, source: &str) -> Self {
        self.c_file("main.c", source)
    }

    /// Add `source` as `src/game/c/<name>`.
    pub fn c_file(mut self, name: &str, source: &str) -> Self {
        self.c_files.push((name.into(), source.into()));
        self
    }

    /// Add `source` as `src/game/c++/main.cpp`.
    pub fn cpp(self, source: &str) -> Self {
        self.cpp_file("main.cpp", source)
    }

    /// Add `source` as `src/game/c++/<name>`.
    pub fn cpp_file(mut self, name: &str, source: &str) -> Self {
        self.cpp_files.push((name.into(), source.into()));
        self
    }

    /// Add `source` as `src/game/lua/main.lua`.
    pub fn lua(self, source: &str) -> Self {
        self.lua_file("main.lua", source)
    }

    /// Add `source` as `src/game/lua/<name>`.
    pub fn lua_file(mut self, name: &str, source: &str) -> Self {
        self.lua_files.push((name.into(), source.into()));
        self
    }

    /// Set the contents of `src/game/rust/src/lib.rs`.
    /// `blyt build` injects the SDK crate path via `--config` so
    /// the `blyt = "0.1"` dependency resolves without a published crate.
    pub fn rust(mut self, lib_rs: &str) -> Self {
        self.rust_lib_rs = Some(lib_rs.into());
        self
    }

    /// Add a Rust library crate at `src/lib/<name>/`.
    ///
    /// Creates `src/lib/<name>/Cargo.toml` (package name = `<name>`) and
    /// `src/lib/<name>/src/lib.rs` from `lib_rs`.  The game's Cargo.toml is
    /// generated to include `<name> = "0.1"` as a dependency; `blyt build`
    /// injects the source path via `--config` at build time.
    pub fn rust_lib(mut self, name: &str, lib_rs: &str) -> Self {
        self.lib_files
            .push((name.into(), "Cargo.toml".into(), rust_lib_cargo_toml(name)));
        self.lib_files
            .push((name.into(), "src/lib.rs".into(), lib_rs.into()));
        self.rust_lib_names.push(name.into());
        self
    }

    /// Add a file to a library at `src/lib/<name>/<rel_path>`.
    ///
    /// Use `include/<filename>.h` as `rel_path` for public headers; the build
    /// tool exposes `src/lib/<name>/include/` as the include root when it exists.
    /// Use a bare filename for source files and headers in flat layouts.
    pub fn lib_file(mut self, name: &str, rel_path: &str, content: &str) -> Self {
        self.lib_files
            .push((name.into(), rel_path.into(), content.into()));
        self
    }

    /// Write the project layout under `dir` and panic on any I/O error.
    pub fn write(self, dir: &std::path::Path) {
        let has_c = !self.c_files.is_empty();
        let has_cpp = !self.cpp_files.is_empty();
        let has_rust = self.rust_lib_rs.is_some();
        let has_lua = !self.lua_files.is_empty();
        assert!(
            has_c || has_cpp || has_rust || has_lua,
            "CartProject::write: no game language declared"
        );

        // Ensure the project root exists before writing any files into it.
        fs::create_dir_all(dir).unwrap();

        // cart.build.yaml — language declaration (ADR-0073).
        // Lua carts omit the file; the absent manifest signals Lua to blyt build.
        if !has_lua {
            let manifest = if has_c {
                "language: c\n"
            } else if has_cpp {
                "language: \"c++\"\n"
            } else {
                "language: rust\n"
            };
            fs::write(dir.join("cart.build.yaml"), manifest).unwrap();
        }

        if has_c {
            let c_dir = dir.join("src/game/c");
            fs::create_dir_all(&c_dir).unwrap();
            for (name, source) in &self.c_files {
                fs::write(c_dir.join(name), source).unwrap();
            }
        }

        if has_cpp {
            let cpp_dir = dir.join("src/game/c++");
            fs::create_dir_all(&cpp_dir).unwrap();
            for (name, source) in &self.cpp_files {
                fs::write(cpp_dir.join(name), source).unwrap();
            }
        }

        if let Some(lib_rs) = self.rust_lib_rs {
            let rust_src = dir.join("src/game/rust/src");
            fs::create_dir_all(&rust_src).unwrap();
            fs::write(rust_src.join("lib.rs"), lib_rs).unwrap();
            fs::write(
                dir.join("src/game/rust/Cargo.toml"),
                rust_game_cargo_toml(&self.rust_lib_names),
            )
            .unwrap();
        }

        if has_lua {
            let lua_dir = dir.join("src/game/lua");
            fs::create_dir_all(&lua_dir).unwrap();
            for (name, source) in &self.lua_files {
                fs::write(lua_dir.join(name), source).unwrap();
            }
        }

        for (lib_name, rel_path, content) in &self.lib_files {
            let dest = dir.join("src/lib").join(lib_name).join(rel_path);
            fs::create_dir_all(dest.parent().unwrap()).unwrap();
            fs::write(dest, content).unwrap();
        }
    }
}

// -------------------------------------------------------------------------
// Convenience wrappers (thin delegators to CartProject)
// -------------------------------------------------------------------------

/// Shorthand for `CartProject::new().c(source).write(dir)`.
pub fn write_c_cart_project(dir: &std::path::Path, source: &str) {
    CartProject::new().c(source).write(dir);
}

/// Shorthand for `CartProject::new().rust(lib_rs).write(dir)`.
pub fn write_rust_cart_project(dir: &std::path::Path, lib_rs: &str) {
    CartProject::new().rust(lib_rs).write(dir);
}

// -------------------------------------------------------------------------
// Rust toolchain probe
// -------------------------------------------------------------------------

/// Returns true if the riscv32imafc-unknown-none-elf Rust target is installed.
/// Install with: `rustup target add riscv32imafc-unknown-none-elf`
pub fn has_rust_riscv_target() -> bool {
    std::process::Command::new("rustup")
        .args(["target", "list", "--installed"])
        .output()
        .map(|o| String::from_utf8_lossy(&o.stdout).contains("riscv32imafc-unknown-none-elf"))
        .unwrap_or(false)
}

/// Returns true if a usable `luac` is available (SDK blyt-luac, $BLYT_LUAC, or system luac).
pub fn has_luac() -> bool {
    // Check $BLYT_LUAC override first.
    if let Ok(c) = std::env::var("BLYT_LUAC") {
        return std::path::Path::new(&c).exists();
    }
    // SDK symlink.
    if sdk_dir().join("bin/blyt-luac").exists() {
        return true;
    }
    // System luac.
    std::process::Command::new("luac")
        .arg("-v")
        .output()
        .is_ok()
}

// -------------------------------------------------------------------------
// Tooling requirement assertions
//
// Call these at the top of tests that need a given tool.  They panic with an
// actionable message instead of silently returning, so a missing tool is a
// test failure, not an invisible skip.
// -------------------------------------------------------------------------

pub fn require_sdk() {
    assert!(
        sdk_dir().join("bin/blyt-clang").exists(),
        "SDK not assembled — run `cmake --build build --target sdk` first"
    );
    assert!(
        sdk_dir().join("lib/libblyt32.so").exists(),
        "libblyt32.so not in SDK — run `cmake --build build --target sdk` first"
    );
}

pub fn require_cpp_sdk() {
    assert!(
        sdk_dir().join("bin/blyt-clang++").exists(),
        "blyt-clang++ not in SDK — run `cmake --build build --target sdk` first"
    );
    assert!(
        sdk_dir().join("lib/libc++.a").exists(),
        "libc++.a not in SDK — run `cmake --build build --target sdk` first"
    );
}

pub fn require_lua_sdk() {
    assert!(
        sdk_dir().join("lib/libblyt32lua.so").exists(),
        "libblyt32lua.so not in SDK — run `cmake --build build --target sdk` first"
    );
    assert!(
        has_luac(),
        "luac not available — install luac or set BLYT_LUAC"
    );
}

pub fn require_rust_riscv_target() {
    assert!(
        has_rust_riscv_target(),
        "riscv32imafc-unknown-none-elf Rust target not installed — \
         run `rustup target add riscv32imafc-unknown-none-elf`"
    );
}

pub fn require_test_session_api() {
    assert!(
        test_session_api().exists(),
        "test_session_api not built — run `cmake --build build` first"
    );
}

// -------------------------------------------------------------------------
// Cart build helpers
// -------------------------------------------------------------------------

/// Run `blyt build <project_dir>` and return the expected cart output path.
pub fn build_cart(project_dir: &std::path::Path) -> PathBuf {
    use assert_cmd::Command;
    let sdk = sdk_dir();
    let mut cmd = Command::cargo_bin("blyt").unwrap();
    cmd.args(["build", project_dir.to_str().unwrap()])
        .env("BLYT_SDK_DIR", &sdk)
        .env("BLYT_OBJCOPY", sdk.join("bin/blyt-objcopy"))
        .env("BLYT_RUST_SDK", repo_root().join("sdk/rust/blyt"));
    let sdk_clang = sdk.join("bin/blyt-clang");
    if sdk_clang.exists() {
        cmd.env("BLYT_CLANG", &sdk_clang);
    }
    let sdk_clangpp = sdk.join("bin/blyt-clang++");
    if sdk_clangpp.exists() {
        cmd.env("BLYT_CLANGPP", &sdk_clangpp);
    }
    let sdk_ar = sdk.join("bin/blyt-llvm-ar");
    if sdk_ar.exists() {
        cmd.env("BLYT_AR", &sdk_ar);
    }
    cmd.assert().success();

    project_dir.parent().unwrap().join(format!(
        "{}.blyt",
        project_dir.file_name().unwrap().to_str().unwrap()
    ))
}

/// Run `blyt build <project_dir>` with Lua-specific env vars and return the cart path.
pub fn build_lua_cart(project_dir: &std::path::Path) -> PathBuf {
    use assert_cmd::Command;
    let sdk = sdk_dir();
    let mut cmd = Command::cargo_bin("blyt").unwrap();
    cmd.args(["build", project_dir.to_str().unwrap()])
        .env("BLYT_SDK_DIR", &sdk)
        .env("BLYT_OBJCOPY", sdk.join("bin/blyt-objcopy"));
    let sdk_clang = sdk.join("bin/blyt-clang");
    if sdk_clang.exists() {
        cmd.env("BLYT_CLANG", &sdk_clang);
    }
    let sdk_clangpp = sdk.join("bin/blyt-clang++");
    if sdk_clangpp.exists() {
        cmd.env("BLYT_CLANGPP", &sdk_clangpp);
    }
    let sdk_ar = sdk.join("bin/blyt-llvm-ar");
    if sdk_ar.exists() {
        cmd.env("BLYT_AR", &sdk_ar);
    }
    let sdk_luac = sdk.join("bin/blyt-luac");
    if sdk_luac.exists() {
        cmd.env("BLYT_LUAC", &sdk_luac);
    }
    cmd.assert().success();

    project_dir.parent().unwrap().join(format!(
        "{}.blyt",
        project_dir.file_name().unwrap().to_str().unwrap()
    ))
}

/// Path to the test_session_api binary produced by the CMake build.
pub fn test_session_api() -> PathBuf {
    build_dir().join("test_session_api")
}

/// Locate the WASM runtime directory.
///
/// Prefers a direct emcmake build output; falls back to the SDK's share/wasm/.
pub fn find_wasm_dir() -> PathBuf {
    let direct = repo_root().join("build-wasm");
    if direct.join("blytrun.js").exists() {
        return direct;
    }
    build_dir().join("sdk/share/wasm")
}
