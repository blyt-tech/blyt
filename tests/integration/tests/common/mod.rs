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

/// The release player — uses the SDK bin/ copy so BLYT_LIB_DIR auto-inference
/// from the binary path resolves to sdk/lib/ (the release guest libs).
pub fn blytplay() -> PathBuf {
    sdk_dir().join("bin/blytplay")
}

/// The debug player (ADR-0129): GDB/DAP on, loads the debug guest libs.
/// Uses the SDK bin/ copy so BLYT_LIB_DIR auto-inference resolves to
/// sdk/lib/debug/ when --debug or --gdb flags are present.
pub fn blytdebug() -> PathBuf {
    sdk_dir().join("bin/blytdebug")
}

/// Path to the `blyt` devtool binary — always the SDK copy.
pub fn blyt_bin() -> PathBuf {
    sdk_dir().join("bin/blyt")
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
/// `blyt.build.yaml` is generated automatically from the declared languages.
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
    /// Contents of blyt.config.yaml (state buffers, fps, etc.)
    config_yaml: Option<String>,
}

impl CartProject {
    pub fn new() -> Self {
        CartProject {
            c_files: Vec::new(),
            cpp_files: Vec::new(),
            rust_lib_rs: None,
            config_yaml: None,
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

    /// Set the contents of `blyt.config.yaml` (state buffers, fps, etc.).
    pub fn config(mut self, yaml: &str) -> Self {
        self.config_yaml = Some(yaml.into());
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

        // blyt.info.yaml — mandatory for all blyt cart projects.  The title
        // contains a space on purpose (it is not filename-constrained) and
        // `version:` is omitted to exercise the 0.0.1-dev default.
        let project_name = dir.file_name().unwrap_or_default().to_string_lossy();
        fs::write(
            dir.join("blyt.info.yaml"),
            format!("id: {project_name}\ntitle: {project_name} Title\n"),
        )
        .unwrap();

        // blyt.build.yaml — language declaration (ADR-0073).
        // Pure Lua carts omit the file; all other combinations require explicit declaration.
        let native_count = [has_c, has_cpp, has_rust].iter().filter(|&&b| b).count();
        if has_lua && native_count == 0 {
            // pure Lua: no blyt.build.yaml needed
        } else if !has_lua && native_count == 1 {
            // pure native: singular `language:` form
            let manifest = if has_c {
                "language: c\n"
            } else if has_cpp {
                "language: \"c++\"\n"
            } else {
                "language: rust\n"
            };
            fs::write(dir.join("blyt.build.yaml"), manifest).unwrap();
        } else {
            // hybrid Lua + native: `languages:` map
            let mut manifest = String::from("languages:\n");
            if has_lua {
                manifest.push_str("  lua:\n");
            }
            if has_c {
                manifest.push_str("  c:\n");
            }
            if has_cpp {
                manifest.push_str("  \"c++\":\n");
            }
            if has_rust {
                manifest.push_str("  rust:\n");
            }
            fs::write(dir.join("blyt.build.yaml"), manifest).unwrap();
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

        if let Some(ref yaml) = self.config_yaml {
            fs::write(dir.join("blyt.config.yaml"), yaml).unwrap();
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

/// Returns true if Rust cart builds are possible: the pinned nightly toolchain
/// (or $BLYT_RUST_TOOLCHAIN override) is installed. Cart builds use a custom
/// JSON target (riscv32imafdc-blyt-none-elf) via -Z build-std, so the target
/// never appears in `rustup target list`; the nightly toolchain is the real gate.
pub fn has_rust_riscv_target() -> bool {
    let toolchain =
        std::env::var("BLYT_RUST_TOOLCHAIN").unwrap_or_else(|_| "nightly-2026-06-01".to_string());
    std::process::Command::new("rustup")
        .args(["toolchain", "list"])
        .output()
        .map(|o| String::from_utf8_lossy(&o.stdout).contains(&toolchain))
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
    let toolchain =
        std::env::var("BLYT_RUST_TOOLCHAIN").unwrap_or_else(|_| "nightly-2026-06-01".to_string());
    assert!(
        has_rust_riscv_target(),
        "Rust cart toolchain '{toolchain}' not installed — \
         run `rustup toolchain install {toolchain} --profile minimal --component rust-src`"
    );
}

pub fn require_test_session_api() {
    assert!(
        test_session_api().exists(),
        "test_session_api not built — run `cmake --build build` first"
    );
}

pub fn require_wasm() {
    assert!(
        find_wasm_dir().join("blytplay.js").exists(),
        "WASM runtime not built — install emscripten and run \
         `cmake --build build --target sdk` (incremental rebuild: \
         `cmake --build build/build-wasm`)"
    );
}

/// Require the debug WASM runtime (blytdebug.*, DAP/GDB enabled) for the WASM
/// DAP/GDB tests (ADR-0129).
pub fn require_wasm_debug() {
    assert!(
        find_wasm_debug_dir().join("blytdebug.js").exists(),
        "debug WASM runtime not built — run `cmake --build build --target sdk` \
         (builds share/wasm-debug/blytdebug.* with BLYT_DAP/BLYT_GDB; \
         incremental rebuild: `cmake --build build/build-wasm-debug`)"
    );
}

pub fn require_playwright() {
    let pkg = repo_root().join("tests/wasm/node_modules/playwright");
    assert!(
        pkg.exists(),
        "playwright not installed — run:\n  \
         cd tests/wasm && npm install && npx playwright install chromium"
    );
}

pub fn libretro_so() -> PathBuf {
    build_dir().join("blyt_libretro.so")
}

pub fn test_libretro_core() -> PathBuf {
    build_dir().join("test_libretro_core")
}

pub fn require_libretro_core() {
    assert!(
        libretro_so().exists(),
        "blyt_libretro.so not built — run `cmake --build build` first"
    );
    assert!(
        test_libretro_core().exists(),
        "test_libretro_core not built — run `cmake --build build` first \
         (requires RV32 toolchain for LIBBLYTC_OUT)"
    );
}

// -------------------------------------------------------------------------
// Cart build helpers
// -------------------------------------------------------------------------

/// Run `blyt build <project_dir>` and return the expected cart output path.
pub fn build_cart(project_dir: &std::path::Path) -> PathBuf {
    use assert_cmd::Command;
    let sdk = sdk_dir();
    let mut cmd = Command::new(blyt_bin());
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

    project_dir.join("build").join(format!(
        "{}.blyt",
        project_dir.file_name().unwrap().to_str().unwrap()
    ))
}

/// Build a dev ELF (`build/.elf`) by running `blyt run <project_dir>` and
/// killing it once the ELF has been produced.  Requires the SDK (for `blyt`
/// and `blyt-luac`); does not require the WASM runtime.
///
/// `blyt run` builds the dev ELF as its first step before starting the HTTP
/// server, so this helper works whether or not the WASM runtime is present.
pub fn build_dev_elf(project_dir: &std::path::Path) -> PathBuf {
    use std::io::{BufRead, BufReader};
    let sdk = sdk_dir();
    let mut cmd = std::process::Command::new(blyt_bin());
    cmd.args(["run", project_dir.to_str().unwrap()])
        .env("BLYT_SDK_DIR", &sdk)
        .env("BLYT_OBJCOPY", sdk.join("bin/blyt-objcopy"));
    let sdk_clang = sdk.join("bin/blyt-clang");
    if sdk_clang.exists() {
        cmd.env("BLYT_CLANG", &sdk_clang);
    }
    let sdk_luac = sdk.join("bin/blyt-luac");
    if sdk_luac.exists() {
        cmd.env("BLYT_LUAC", &sdk_luac);
    }
    // Capture stdout so we can detect when the ELF is ready; suppress stderr
    // (WASM-missing errors are expected and not failures here).
    cmd.stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::null());
    let mut child = cmd.spawn().expect("blyt run spawn");
    let mut reader = BufReader::new(child.stdout.take().unwrap());
    let mut line = String::new();
    loop {
        line.clear();
        match reader.read_line(&mut line) {
            // EOF: process exited (WASM absent but ELF already built).
            Ok(0) | Err(_) => break,
            // "built: /path/to/build/.elf" — ELF ready; process may still serve.
            Ok(_) if line.starts_with("built: ") => break,
            _ => {}
        }
    }
    let _ = child.kill();
    let _ = child.wait();
    let elf_path = project_dir.join("build/.elf");
    assert!(
        elf_path.exists(),
        "build/.elf not created by blyt run: {}",
        elf_path.display()
    );
    elf_path
}

/// Run `blyt build <project_dir>` with Lua-specific env vars and return the cart path.
pub fn build_lua_cart(project_dir: &std::path::Path) -> PathBuf {
    use assert_cmd::Command;
    let sdk = sdk_dir();
    let mut cmd = Command::new(blyt_bin());
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
    let sdk_luac = sdk.join("bin/blyt-luac");
    if sdk_luac.exists() {
        cmd.env("BLYT_LUAC", &sdk_luac);
    }
    cmd.assert().success();

    project_dir.join("build").join(format!(
        "{}.blyt",
        project_dir.file_name().unwrap().to_str().unwrap()
    ))
}

/// Run a cart with blytplay --headless; assert `expected` appears in stdout.
pub fn run_cart_native(cart: &std::path::Path, expected: &str) {
    run_cart_native_with_env(cart, &[], expected)
}

/// Run a cart with blytplay --headless; assert the process exits with a non-zero status.
pub fn run_cart_native_expect_fail(cart: &std::path::Path) {
    use assert_cmd::Command;
    Command::new(blytplay())
        .args(["--headless", cart.to_str().unwrap()])
        .assert()
        .failure();
}

/// Run a cart with blytplay --headless {extra_flags...} {cart}; assert `expected`
/// appears in stdout.
pub fn run_cart_native_with_flags(cart: &std::path::Path, extra_flags: &[&str], expected: &str) {
    use assert_cmd::Command;
    let mut cmd = Command::new(blytplay());
    cmd.arg("--headless");
    for f in extra_flags {
        cmd.arg(f);
    }
    cmd.arg(cart.to_str().unwrap());
    let output = cmd.assert().success().get_output().stdout.clone();
    assert!(
        String::from_utf8_lossy(&output).contains(expected),
        "expected {:?} in native output, got: {}",
        expected,
        String::from_utf8_lossy(&output)
    );
}

/// Run a cart with blytplay --headless plus extra environment variables; assert
/// `expected` appears in stdout.
pub fn run_cart_native_with_env(
    cart: &std::path::Path,
    extra_env: &[(&str, &str)],
    expected: &str,
) {
    use assert_cmd::Command;
    let mut cmd = Command::new(blytplay());
    cmd.args(["--headless", cart.to_str().unwrap()]);
    for (k, v) in extra_env {
        cmd.env(k, v);
    }
    let output = cmd.assert().success().get_output().stdout.clone();
    assert!(
        String::from_utf8_lossy(&output).contains(expected),
        "expected {:?} in native output, got: {}",
        expected,
        String::from_utf8_lossy(&output)
    );
}

/// Run a cart with the WASM runner; assert `expected` appears in stdout.
pub fn run_cart_wasm(cart: &std::path::Path, expected: &str) {
    run_cart_wasm_with_env(cart, &[], expected)
}

/// Run a cart with the WASM runner plus extra C environment variables; assert
/// `expected` appears in stdout.
///
/// Variables are injected via the 5th argument to `run_cart.js` as a JSON
/// object, read by `module_pre.js` into Emscripten's `ENV` table before C
/// startup.  This is needed because Emscripten does not inherit Node.js
/// `process.env` for the multi-environment (web+node) build.
pub fn run_cart_wasm_with_env(cart: &std::path::Path, extra_env: &[(&str, &str)], expected: &str) {
    use assert_cmd::Command;
    let driver = repo_root().join("tests/wasm/run_cart.js");
    let wasm_dir = find_wasm_dir();
    let mut cmd = Command::new("node");
    cmd.args([
        driver.to_str().unwrap(),
        wasm_dir.to_str().unwrap(),
        cart.to_str().unwrap(),
    ]);
    if !extra_env.is_empty() {
        // Build a minimal JSON object {"KEY":"VALUE",...} without serde_json.
        let pairs: Vec<String> = extra_env
            .iter()
            .map(|(k, v)| {
                format!(
                    "\"{}\":\"{}\"",
                    k,
                    v.replace('\\', "\\\\").replace('"', "\\\"")
                )
            })
            .collect();
        let env_json = format!("{{{}}}", pairs.join(","));
        // 4th arg = frame0OutPath (empty); 5th arg = env JSON
        cmd.args(["", &env_json]);
    }
    let output = cmd.assert().success().get_output().stdout.clone();
    assert!(
        String::from_utf8_lossy(&output).contains(expected),
        "expected {:?} in wasm output, got: {}",
        expected,
        String::from_utf8_lossy(&output)
    );
}

/// Run a cart through the embedded libretro core (test_libretro_core dlopens
/// blyt_libretro.so); assert `expected` appears in the output.  This is the
/// third leg of the native/wasm test pairs: it exercises the core's EMBEDDED
/// guest lib blobs, which are a separate artifact from the sdk/lib files the
/// blytplay path loads.  Cart debug output arrives via the libretro log
/// callback, which the driver writes to stderr.
pub fn run_cart_libretro(cart: &std::path::Path, expected: &str) {
    run_cart_libretro_with_flags(cart, &[], expected)
}

/// Run a cart through the embedded libretro core with driver flags (e.g.
/// `--reset-every-frame` to drive retro_reset_every_frame_cycle() after
/// every frame — the same save-state stress cycle as blytplay
/// --reset-every-frame); assert `expected` appears in the output.
pub fn run_cart_libretro_with_flags(cart: &std::path::Path, flags: &[&str], expected: &str) {
    use assert_cmd::Command;
    let mut cmd = Command::new(test_libretro_core());
    for f in flags {
        cmd.arg(f);
    }
    cmd.args([libretro_so().to_str().unwrap(), cart.to_str().unwrap()]);
    let output = cmd.assert().success().get_output().stderr.clone();
    assert!(
        String::from_utf8_lossy(&output).contains(expected),
        "expected {:?} in libretro core output, got: {}",
        expected,
        String::from_utf8_lossy(&output)
    );
}

/// Run a cart through the embedded libretro core; assert the driver exits
/// with a non-zero status (load failure or runtime error).
pub fn run_cart_libretro_expect_fail(cart: &std::path::Path) {
    use assert_cmd::Command;
    Command::new(test_libretro_core())
        .args([libretro_so().to_str().unwrap(), cart.to_str().unwrap()])
        .assert()
        .failure();
}

/// Path to the test_session_api binary produced by the CMake build.
pub fn test_session_api() -> PathBuf {
    build_dir().join("test_session_api")
}

/// Locate the WASM runtime directory (SDK layout).  The emcmake trees at
/// build/build-wasm[-debug] emit their artifacts directly here via
/// BLYT_WASM_OUT_DIR, so this is always the authoritative copy.
pub fn find_wasm_dir() -> PathBuf {
    sdk_dir().join("share/wasm")
}

/// Locate the DEBUG WASM runtime dir (blytdebug.*, built with BLYT_DAP/BLYT_GDB).
/// The release blytplay has DAP/GDB compiled out (ADR-0129), so the WASM DAP/GDB
/// tests must use this variant.
pub fn find_wasm_debug_dir() -> PathBuf {
    sdk_dir().join("share/wasm-debug")
}

// -------------------------------------------------------------------------
// GDB helpers
// -------------------------------------------------------------------------

pub fn require_gdb() {
    // ADR-0129: GDB/DAP debugging lives in blytdebug, not the release blytplay.
    assert!(
        blytdebug().exists(),
        "blytdebug not built — run `cmake --build build` first"
    );
    let out = std::process::Command::new(blytdebug())
        .arg("--help")
        .output()
        .unwrap_or_else(|_| {
            // blytdebug exits non-zero for --help; capture output anyway
            std::process::Command::new(blytdebug())
                .args(["--gdb", "0"])
                .output()
                .unwrap()
        });
    let stdout = String::from_utf8_lossy(&out.stdout);
    let stderr = String::from_utf8_lossy(&out.stderr);
    let _ = (stdout, stderr); // just ensure blytplay accepts --gdb
}

/// Parse "blyt: GDB listening on port N" from combined stdout/stderr output.
pub fn blytplay_gdb_port(output: &str) -> Option<u16> {
    let m = output.find("GDB listening on port ")?;
    let rest = &output[m + "GDB listening on port ".len()..];
    let end = rest
        .find(|c: char| !c.is_ascii_digit())
        .unwrap_or(rest.len());
    rest[..end].parse().ok()
}

/// Run `blyt build <project_dir>` with Lua-specific env vars and the `--debug`
/// flag, returning the cart path.
pub fn build_debug_lua_cart(project_dir: &std::path::Path) -> PathBuf {
    use assert_cmd::Command;
    let sdk = sdk_dir();
    let mut cmd = Command::new(blyt_bin());
    cmd.args(["build", "--debug", project_dir.to_str().unwrap()])
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

    // ADR-0129: debug builds are named <name>.dbg.blyt.
    project_dir.join("build").join(format!(
        "{}.dbg.blyt",
        project_dir.file_name().unwrap().to_str().unwrap()
    ))
}

/// Build a cart with debug information.
///
/// Runs `blyt build --debug <project_dir>` and returns the expected cart path.
pub fn build_debug_cart(project_dir: &std::path::Path) -> PathBuf {
    use assert_cmd::Command;
    let sdk = sdk_dir();
    let mut cmd = Command::new(blyt_bin());
    cmd.args(["build", "--debug", project_dir.to_str().unwrap()])
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

    // ADR-0129: debug builds are named <name>.dbg.blyt.
    project_dir.join("build").join(format!(
        "{}.dbg.blyt",
        project_dir.file_name().unwrap().to_str().unwrap()
    ))
}

/// Find the lldb-dap binary — prefers the SDK-bundled copy, falls back to PATH.
pub fn lldb_dap_bin() -> Option<PathBuf> {
    let sdk_candidate = sdk_dir().join("bin/blyt-lldb-dap");
    if sdk_candidate.exists() {
        return Some(sdk_candidate);
    }
    let out = std::process::Command::new("which")
        .arg("lldb-dap")
        .output()
        .ok()?;
    if out.status.success() {
        let path = String::from_utf8_lossy(&out.stdout).trim().to_string();
        if !path.is_empty() {
            return Some(PathBuf::from(path));
        }
    }
    None
}

/// Skip test if lldb-dap is not available.
pub fn require_lldb_dap() {
    assert!(
        lldb_dap_bin().is_some(),
        "lldb-dap not found — install LLVM or build SDK with blyt-lldb-dap"
    );
}

/// Find the virtual address of a symbol in a cart ELF using `readelf -s`.
///
/// Returns `None` if `readelf` is not available or the symbol is not found.
/// find_symbol_addr or panic.  Missing symbol lookup must be a test failure,
/// not a silent skip or a degraded handshake-only fallback: vacuous passes on
/// macOS (which lacks GNU readelf) hid a Linux-only GDB regression (65d8341).
pub fn require_symbol_addr(cart: &std::path::Path, symbol: &str) -> u64 {
    find_symbol_addr(cart, symbol).unwrap_or_else(|| {
        panic!(
            "symbol {symbol} not found in {} — install readelf or llvm-readelf \
             (brew install llvm); symbol lookup must not be skipped",
            cart.display()
        )
    })
}

pub fn find_symbol_addr(cart: &std::path::Path, symbol: &str) -> Option<u64> {
    // GNU readelf on Linux; llvm-readelf elsewhere (macOS has no readelf —
    // falling through silently here used to skip the GDB native-breakpoint
    // test sections on macOS entirely).
    let candidates = [
        "readelf",
        "llvm-readelf",
        "/opt/homebrew/opt/llvm/bin/llvm-readelf",
    ];
    for tool in candidates {
        let Ok(out) = std::process::Command::new(tool)
            .args(["-s", "--wide", cart.to_str()?])
            .output()
        else {
            continue;
        };
        let stdout = String::from_utf8_lossy(&out.stdout);
        for line in stdout.lines() {
            let parts: Vec<&str> = line.split_whitespace().collect();
            // readelf -s format: Num: Value Size Type Bind Vis Ndx Name
            if parts.len() >= 8 && parts[7] == symbol {
                return u64::from_str_radix(parts[1], 16).ok();
            }
        }
    }
    None
}
