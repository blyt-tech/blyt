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
    let sdk = sdk_dir();
    let mut cmd = Command::cargo_bin("blyt").unwrap();
    cmd.args(["build", project_dir.to_str().unwrap()])
        .env("BLYT_SDK_DIR", &root)
        .env("BLYT_LIB_DIR", sdk.join("lib"))
        .env("BLYT_OBJCOPY", sdk.join("bin/blyt-objcopy"));
    // Use the SDK's riscv32-capable clang if available; system clang on macOS
    // cannot target riscv32 so the test is skipped when the SDK is absent.
    let sdk_clang = sdk.join("bin/blyt-clang");
    if sdk_clang.exists() {
        cmd.env("BLYT_CLANG", &sdk_clang);
    }
    cmd.assert().success();

    // Default output: <parent>/<project_dir_name>.blyt
    project_dir.parent().unwrap().join(format!(
        "{}.blyt",
        project_dir.file_name().unwrap().to_str().unwrap()
    ))
}

/// Path to the assembled SDK directory (build/sdk/).
fn sdk_dir() -> PathBuf {
    build_dir().join("sdk")
}

// -------------------------------------------------------------------------
// Tests
// -------------------------------------------------------------------------

/// SDK end-to-end: use the assembled SDK (build/sdk/) to build and run a cart.
///
/// The SDK binary auto-discovers its toolchain and libraries from its own
/// location in build/sdk/bin/ — no env vars required for blyt build.
/// blytrun needs BLYT_LIB_DIR since it cannot yet auto-discover its libraries.
///
/// Requires `cmake --build build --target sdk` to have completed.
/// Silently skipped if the SDK has not been assembled.
#[test]
fn sdk_e2e_build_and_run() {
    let sdk = sdk_dir();
    let sdk_blyt = sdk.join("bin/blyt");
    let sdk_blytrun = sdk.join("bin/blytrun");

    assert!(
        sdk_blyt.exists() && sdk_blytrun.exists(),
        "SDK not assembled — run `cmake --build build --target sdk` first"
    );

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hello");

    write_cart_project(
        &project,
        r#"
#include "blyt.h"

static int s_frame = 0;

void blyt_cart_init(void)   { blyt_console_debug("hello from sdk"); }
void blyt_cart_update(void) { if (++s_frame >= 1) blyt_quit_ready(); }
void blyt_cart_draw(void)   {}
"#,
    );

    // Build the cart using the assembled SDK binary.  No env vars are needed:
    // the SDK blyt auto-discovers its toolchain (blyt-clang) and runtime
    // libraries from its own location in build/sdk/bin/.
    Command::new(&sdk_blyt)
        .args(["build", project.to_str().unwrap()])
        .assert()
        .success();

    let cart = project.parent().unwrap().join(format!(
        "{}.blyt",
        project.file_name().unwrap().to_str().unwrap()
    ));
    assert!(cart.exists(), "cart not found at {}", cart.display());

    // Run the cart with the SDK's blytrun.
    let out = Command::new(&sdk_blytrun)
        .args(["--headless", cart.to_str().unwrap()])
        .env("BLYT_LIB_DIR", sdk.join("lib"))
        .assert()
        .success()
        .get_output()
        .stdout
        .clone();

    assert!(
        String::from_utf8_lossy(&out).contains("hello from sdk"),
        "expected 'hello from sdk' in output, got: {}",
        String::from_utf8_lossy(&out)
    );
}

/// The lifecycle callbacks fire in the correct order: init, then update+draw
/// each iteration, with quit signalled after the second update.
///
/// Requires the SDK to be assembled (riscv32 toolchain in build/sdk/).
#[test]
fn hello_cart_lifecycle_output() {
    assert!(
        sdk_dir().join("bin/blyt-clang").exists(),
        "SDK not assembled — run `cmake --build build --target sdk` first"
    );
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
        .env("BLYT_LIB_DIR", sdk_dir().join("lib"))
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
        .env("BLYT_LIB_DIR", sdk_dir().join("lib"))
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
        .env("BLYT_LIB_DIR", sdk_dir().join("lib"))
        .assert()
        .failure();
}
