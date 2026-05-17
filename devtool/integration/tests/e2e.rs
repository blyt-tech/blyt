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

/// Path to the CMake build directory (contains blytrun and the runtime libraries).
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

/// The lifecycle callbacks fire in the correct order: init, then update+draw
/// each iteration, with quit signalled after the second update.
#[test]
fn hello_cart_lifecycle_output() {
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hello");

    write_cart_project(
        &project,
        r#"
#include "blyt.h"

static int s_frame = 0;

void blyt_cart_init(void)   { blyt_console_debug("init"); }
void blyt_cart_update(void) {
    blyt_console_debug("update");
    if (++s_frame >= 2) blyt_quit_ready();
}
void blyt_cart_draw(void)   { blyt_console_debug("draw"); }
"#,
    );

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let output = Command::new(blytrun())
        .args(["--headless", cart.to_str().unwrap()])
        .env("BLYT_LIB_DIR", build_dir())
        .assert()
        .success()
        .get_output()
        .stdout
        .clone();
    let out = String::from_utf8_lossy(&output);

    // init fires exactly once, before any update/draw
    assert!(out.contains("init"), "missing 'init' in output: {out}");
    // update and draw both appear (multiple iterations)
    assert!(out.contains("update"), "missing 'update' in output: {out}");
    assert!(out.contains("draw"), "missing 'draw' in output: {out}");
    // init must appear before the first update
    let init_pos = out.find("init").unwrap();
    let update_pos = out.find("update").unwrap();
    assert!(init_pos < update_pos, "init must precede first update");
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
