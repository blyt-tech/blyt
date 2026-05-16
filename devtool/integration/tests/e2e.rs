use assert_cmd::Command;
use predicates::prelude::*;
use std::fs;
use std::path::PathBuf;
use tempfile::TempDir;

/// Returns the repository root (two levels above this crate's manifest dir).
fn repo_root() -> PathBuf {
    // CARGO_MANIFEST_DIR = <repo>/devtool/integration
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent() // devtool/
        .unwrap()
        .parent() // repo root
        .unwrap()
        .to_path_buf()
}

/// Path to the CMake build directory (contains blytrun and libblyt32.so).
fn build_dir() -> PathBuf {
    repo_root().join("build")
}

/// Path to the blytrun binary produced by the CMake build.
fn blytrun() -> PathBuf {
    build_dir().join("blytrun")
}

/// Create a minimal cart project in `dir` with a single C source file.
fn write_cart_project(dir: &std::path::Path, source: &str) {
    let c_dir = dir.join("src/game/c");
    fs::create_dir_all(&c_dir).unwrap();
    fs::write(c_dir.join("main.c"), source).unwrap();
}

/// Run `blyt build <project_dir>` and return the expected cart output path.
fn build_cart(project_dir: &std::path::Path) -> PathBuf {
    let root = repo_root();
    Command::cargo_bin("blyt")
        .unwrap()
        .args(["build", project_dir.to_str().unwrap()])
        .env("BLYT_SDK_DIR", &root)
        .env("BLYT_LIB_DIR", build_dir())
        .env("BLYT_OBJCOPY", "llvm-objcopy")
        .assert()
        .success();

    // Default output: <parent>/<project_dir_name>.blyt
    project_dir.parent().unwrap().join(format!(
        "{}.blyt",
        project_dir.file_name().unwrap().to_str().unwrap()
    ))
}

// -------------------------------------------------------------------------
// Tests
// -------------------------------------------------------------------------

/// blyt build + blytrun --headless: console debug output reaches stdout.
#[test]
fn hello_cart_produces_debug_output() {
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hello");

    write_cart_project(
        &project,
        r#"
#include "blyt.h"
void blyt_main(void) {
    blyt_console_debug("hello from cart");
}
"#,
    );

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    Command::new(blytrun())
        .args(["--headless", cart.to_str().unwrap()])
        .env("BLYT_LIB_DIR", build_dir())
        .assert()
        .success()
        .stdout(predicate::str::contains("hello from cart"));
}

/// A cart that calls blyt_console_debug multiple times outputs all messages.
#[test]
fn multiple_debug_calls_all_appear() {
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("multi");

    write_cart_project(
        &project,
        r#"
#include "blyt.h"
void blyt_main(void) {
    blyt_console_debug("first");
    blyt_console_debug("second");
    blyt_console_debug("third");
}
"#,
    );

    let cart = build_cart(&project);

    Command::new(blytrun())
        .args(["--headless", cart.to_str().unwrap()])
        .env("BLYT_LIB_DIR", build_dir())
        .assert()
        .success()
        .stdout(predicate::str::contains("first"))
        .stdout(predicate::str::contains("second"))
        .stdout(predicate::str::contains("third"));
}

/// blyt build with no source files produces a clear error and non-zero exit.
#[test]
fn build_empty_project_fails_with_error() {
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("empty");
    fs::create_dir_all(project.join("src/game/c")).unwrap();

    let root = repo_root();
    Command::cargo_bin("blyt")
        .unwrap()
        .args(["build", project.to_str().unwrap()])
        .env("BLYT_SDK_DIR", &root)
        .env("BLYT_LIB_DIR", build_dir())
        .env("BLYT_OBJCOPY", "llvm-objcopy")
        .assert()
        .failure()
        .stderr(predicate::str::contains("no .c files"));
}

/// blytrun --headless rejects a non-existent cart path with a non-zero exit.
#[test]
fn run_missing_cart_fails() {
    Command::new(blytrun())
        .args(["--headless", "/nonexistent/path/cart.blyt"])
        .env("BLYT_LIB_DIR", build_dir())
        .assert()
        .failure();
}
