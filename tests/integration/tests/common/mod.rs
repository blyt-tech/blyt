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

const RUST_CARGO_TOML: &str = "\
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
blyt = \"0.1\"\n";

/// Builder for an on-disk cart project used in integration tests.
///
/// Call `.c(source)` and/or `.rust(lib_rs)` to declare languages, then
/// `.write(dir)` to materialise the project layout under `dir`.  The
/// `cart.build.yaml` is generated automatically from the declared languages.
///
/// ```
/// CartProject::new().c(r#"..."#).write(&project_dir);
/// CartProject::new().rust(r#"..."#).write(&project_dir);
/// CartProject::new().c(r#"..."#).rust(r#"..."#).write(&project_dir); // hybrid
/// ```
pub struct CartProject {
    /// (filename, source) pairs for src/game/c/
    c_files: Vec<(String, String)>,
    /// (filename, source) pairs for src/game/c++/
    cpp_files: Vec<(String, String)>,
    /// Contents of src/game/rust/src/lib.rs
    rust_lib_rs: Option<String>,
    /// (lib_name, relative_path_within_lib, content) — files for src/lib/<name>/
    lib_files: Vec<(String, String, String)>,
}

impl CartProject {
    pub fn new() -> Self {
        CartProject {
            c_files: Vec::new(),
            cpp_files: Vec::new(),
            rust_lib_rs: None,
            lib_files: Vec::new(),
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

    /// Set the contents of `src/game/rust/src/lib.rs`.
    /// `blyt build` injects the SDK crate path via `--config` so
    /// the `blyt = "0.1"` dependency resolves without a published crate.
    pub fn rust(mut self, lib_rs: &str) -> Self {
        self.rust_lib_rs = Some(lib_rs.into());
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
        assert!(
            has_c || has_cpp || has_rust,
            "CartProject::write: no game language declared"
        );

        // cart.build.yaml — language declaration (ADR-0073).
        // Mixed-language game code is not yet supported in blyt build; for now
        // each project has exactly one game language.
        let manifest = if has_c {
            "language: c\n"
        } else if has_cpp {
            "language: \"c++\"\n"
        } else {
            "language: rust\n"
        };
        fs::write(dir.join("cart.build.yaml"), manifest).unwrap();

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
            fs::write(dir.join("src/game/rust/Cargo.toml"), RUST_CARGO_TOML).unwrap();
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
