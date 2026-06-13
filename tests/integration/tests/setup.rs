mod common;

use assert_cmd::Command;
use common::{CartProject, blyt_bin, sdk_dir};
use std::fs;
use tempfile::TempDir;

/// `blyt setup vscode` on a C cart writes a launch.json with the single
/// auto-detecting `blyt` configuration.
#[test]
fn setup_vscode_native_cart() {
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("c_cart");
    CartProject::new()
        .c("#include \"blyt.h\"\nvoid blyt_cart_init(void){}\nvoid blyt_cart_update(void){blyt_quit();}\nvoid blyt_cart_draw(void){}\n")
        .write(&project);

    Command::new(blyt_bin())
        .args(["setup", "vscode", project.to_str().unwrap()])
        .assert()
        .success();

    let launch_path = project.join(".vscode/launch.json");
    assert!(launch_path.exists(), "launch.json not written");
    let content = fs::read_to_string(&launch_path).unwrap();

    assert!(
        content.contains("\"type\": \"blyt\""),
        "launch.json missing blyt config:\n{content}"
    );
    assert!(
        !content.contains("blyt-run"),
        "launch.json should not contain the removed blyt-run type:\n{content}"
    );
}

/// `blyt setup vscode` on a Lua cart writes the same single auto-detecting
/// `blyt` configuration as any other cart.
#[test]
fn setup_vscode_lua_cart() {
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("lua_cart");
    CartProject::new()
        .lua("function blyt_cart_init() end\nfunction blyt_cart_update() blyt.quit() end\nfunction blyt_cart_draw() end\n")
        .write(&project);

    Command::new(blyt_bin())
        .args(["setup", "vscode", project.to_str().unwrap()])
        .assert()
        .success();

    let launch_path = project.join(".vscode/launch.json");
    assert!(launch_path.exists(), "launch.json not written");
    let content = fs::read_to_string(&launch_path).unwrap();

    assert!(
        content.contains("\"type\": \"blyt\""),
        "launch.json missing blyt config:\n{content}"
    );
}

/// `blyt setup vscode` on a Rust cart also writes a `.vscode/settings.json`
/// telling rust-analyzer the `--config patch` that resolves the SDK `blyt`
/// crate (issue #48 item 1a).  A non-Rust cart gets no settings.json.
#[test]
fn setup_vscode_rust_cart_writes_ra_config() {
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("rust_cart");
    CartProject::new()
        .rust("#![no_std]\n#[no_mangle] pub extern \"C\" fn blyt_cart_init() {}\n#[no_mangle] pub extern \"C\" fn blyt_cart_update() {}\n#[no_mangle] pub extern \"C\" fn blyt_cart_draw() {}\n")
        .write(&project);

    Command::new(blyt_bin())
        .args(["setup", "vscode", project.to_str().unwrap()])
        .env("BLYT_SDK_DIR", sdk_dir())
        .assert()
        .success();

    let settings = project.join(".vscode/settings.json");
    assert!(settings.exists(), "settings.json not written for rust cart");
    let content = fs::read_to_string(&settings).unwrap();
    assert!(
        content.contains("rust-analyzer.cargo.extraArgs"),
        "settings.json missing extraArgs:\n{content}"
    );
    assert!(
        content.contains("patch.crates-io.blyt.path"),
        "settings.json missing blyt patch:\n{content}"
    );
}

/// `blyt setup vscode` on a non-project directory exits with non-zero status.
#[test]
fn setup_vscode_rejects_non_project() {
    let tmp = TempDir::new().unwrap();
    let not_a_project = tmp.path().join("empty");
    fs::create_dir_all(&not_a_project).unwrap();

    Command::new(blyt_bin())
        .args(["setup", "vscode", not_a_project.to_str().unwrap()])
        .assert()
        .failure();
}

/// `blyt setup vscode` does not overwrite an existing launch.json without
/// `--force`.
#[test]
fn setup_vscode_does_not_clobber() {
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("no_clobber");
    CartProject::new()
        .c("#include \"blyt.h\"\nvoid blyt_cart_init(void){}\nvoid blyt_cart_update(void){blyt_quit();}\nvoid blyt_cart_draw(void){}\n")
        .write(&project);

    // Write a sentinel file first.
    let vscode_dir = project.join(".vscode");
    fs::create_dir_all(&vscode_dir).unwrap();
    let launch_path = vscode_dir.join("launch.json");
    fs::write(&launch_path, "// sentinel\n").unwrap();

    // setup without --force should succeed but NOT overwrite.
    Command::new(blyt_bin())
        .args(["setup", "vscode", project.to_str().unwrap()])
        .assert()
        .success();

    let content = fs::read_to_string(&launch_path).unwrap();
    assert_eq!(
        content, "// sentinel\n",
        "setup vscode should not overwrite without --force"
    );

    // With --force it should overwrite.
    Command::new(blyt_bin())
        .args(["setup", "vscode", "--force", project.to_str().unwrap()])
        .assert()
        .success();

    let content = fs::read_to_string(&launch_path).unwrap();
    assert!(
        content.contains("\"type\": \"blyt\""),
        "expected overwrite with --force"
    );
}
