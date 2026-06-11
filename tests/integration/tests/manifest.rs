//! blyt.info.yaml manifest tests: id/title/version validation, output
//! filename derivation, the cart-identity startup log line on all three
//! runtime legs, and save-directory naming from the manifest id.

mod common;

use assert_cmd::Command;
use common::*;
use predicates::prelude::*;
use std::fs;
use tempfile::TempDir;

/// Write a bare project dir containing only blyt.info.yaml and run
/// `blyt build` on it, asserting failure with `expected` on stderr.
/// Manifest validation runs before any source checks, so no sources needed.
fn assert_build_fails(info_yaml: &str, expected: &str) {
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("badinfo");
    fs::create_dir_all(&project).unwrap();
    fs::write(project.join("blyt.info.yaml"), info_yaml).unwrap();

    Command::new(blyt_bin())
        .args(["build", project.to_str().unwrap()])
        .env("BLYT_SDK_DIR", sdk_dir())
        .env("BLYT_OBJCOPY", sdk_dir().join("bin/blyt-objcopy"))
        .assert()
        .failure()
        .stderr(predicate::str::contains(expected));
}

/// The retired `name:` field is rejected like any other unknown field.
#[test]
fn build_rejects_legacy_name_field() {
    assert_build_fails("name: hello\n", "unknown field");
}

#[test]
fn build_rejects_missing_id() {
    assert_build_fails("title: Hello\n", "missing required field `id`");
}

#[test]
fn build_rejects_missing_title() {
    assert_build_fails("id: hello\n", "missing required field `title`");
}

#[test]
fn build_rejects_invalid_id() {
    assert_build_fails("id: Hello\ntitle: Hello\n", "invalid `id`");
    assert_build_fails("id: \"hello world\"\ntitle: Hello\n", "invalid `id`");
}

#[test]
fn build_rejects_control_chars_in_title() {
    assert_build_fails("id: hello\ntitle: \"a\\nb\"\n", "control characters");
}

#[test]
fn build_rejects_invalid_semver_version() {
    assert_build_fails(
        "id: hello\ntitle: Hello\nversion: not-semver\n",
        "not a valid semver",
    );
}

/// The default output filename comes from the manifest id, not the project
/// directory name.
#[test]
fn output_filename_derives_from_id() {
    require_sdk();
    require_lua_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("dirname_project");
    CartProject::new()
        .lua("function init() end\nfunction update() blyt.quit() end\nfunction draw() end\n")
        .write(&project);
    fs::write(
        project.join("blyt.info.yaml"),
        "id: different_id\ntitle: Different\n",
    )
    .unwrap();

    let sdk = sdk_dir();
    Command::new(blyt_bin())
        .args(["build", project.to_str().unwrap()])
        .env("BLYT_SDK_DIR", &sdk)
        .env("BLYT_OBJCOPY", sdk.join("bin/blyt-objcopy"))
        .env("BLYT_CLANG", sdk.join("bin/blyt-clang"))
        .env("BLYT_AR", sdk.join("bin/blyt-llvm-ar"))
        .env("BLYT_LUAC", sdk.join("bin/blyt-luac"))
        .assert()
        .success();

    assert!(
        project.join("build/different_id.blyt").exists(),
        "expected build/different_id.blyt from manifest id"
    );
    assert!(
        !project.join("build/dirname_project.blyt").exists(),
        "output must not be named after the project directory"
    );
}

/// Expected startup line for a CartProject-generated cart named `id`
/// (title = "<id> Title", version = default 0.0.1-dev).
fn expected_startup_line(id: &str) -> String {
    let version = fs::read_to_string(build_dir().join("version.txt"))
        .expect("build/version.txt")
        .trim()
        .to_string();
    format!("Blyt {version} - {id} Title ({id} 0.0.1-dev)")
}

const QUIT_LUA: &str =
    "function init() end\nfunction update() blyt.quit() end\nfunction draw() end\n";

/// blytplay logs `Blyt <version> - <title> (<id> <cart-version>)` on stderr
/// when a cart starts.
#[test]
fn startup_log_line_native() {
    require_sdk();
    require_lua_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("startup_log");
    CartProject::new().lua(QUIT_LUA).write(&project);
    let cart = build_lua_cart(&project);

    let output = Command::new(blytplay())
        .args(["--headless", cart.to_str().unwrap()])
        .assert()
        .success()
        .get_output()
        .stderr
        .clone();
    let stderr = String::from_utf8_lossy(&output);
    let expected = expected_startup_line("startup_log");
    assert!(
        stderr.contains(&expected),
        "expected {expected:?} in blytplay stderr, got: {stderr}"
    );
}

/// Same startup line on the WASM leg (both the host-Lua fast path — this cart
/// is pure Lua — and the emulated path share the log site in wasm_main.c).
#[test]
fn startup_log_line_wasm() {
    require_sdk();
    require_lua_sdk();
    require_wasm();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("startup_log_wasm");
    CartProject::new().lua(QUIT_LUA).write(&project);
    let cart = build_lua_cart(&project);

    let driver = repo_root().join("tests/wasm/run_cart.js");
    let output = Command::new("node")
        .args([
            driver.to_str().unwrap(),
            find_wasm_dir().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .assert()
        .success()
        .get_output()
        .stderr
        .clone();
    let stderr = String::from_utf8_lossy(&output);
    let expected = expected_startup_line("startup_log_wasm");
    assert!(
        stderr.contains(&expected),
        "expected {expected:?} in wasm driver stderr, got: {stderr}"
    );
}

/// Same startup line through the embedded libretro core (third leg).
#[test]
fn startup_log_line_libretro() {
    require_sdk();
    require_lua_sdk();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("startup_log_retro");
    CartProject::new().lua(QUIT_LUA).write(&project);
    let cart = build_lua_cart(&project);

    run_cart_libretro(&cart, &expected_startup_line("startup_log_retro"));
}

/// The save-file subdirectory is named after the manifest id, not the .blyt
/// filename: build with -o to a different filename and check where the save
/// slot lands.
#[test]
fn save_dir_derives_from_manifest_id() {
    require_sdk();

    let tmp = TempDir::new().unwrap();
    let save_dir = TempDir::new().unwrap();
    let project = tmp.path().join("save_id_cart");

    CartProject::new()
        .config(
            "\
records:
  Game:
    fields:
      - { name: score, type: i32 }
state_buffers:
  game:
    record: Game
    count: 1
",
        )
        .c(r#"
#include "blyt.h"
#include "cart_state.h"

void blyt_cart_init(void) {
    int32_t slot = -1;
    blyt_buffer_alloc_slot(S_GAME, &slot);
    blyt_buffer_set_i32(S_GAME, slot, S_GAME_SCORE, 7);
    blyt_save_write(0);
}

void blyt_cart_update(void) {
    blyt_quit();
}

void blyt_cart_draw(void) {}
"#)
        .write(&project);

    // Build to a filename that does NOT match the manifest id.
    let sdk = sdk_dir();
    let out = project.join("build/renamed_output.blyt");
    let mut cmd = Command::new(blyt_bin());
    cmd.args([
        "build",
        project.to_str().unwrap(),
        "-o",
        out.to_str().unwrap(),
    ])
    .env("BLYT_SDK_DIR", &sdk)
    .env("BLYT_OBJCOPY", sdk.join("bin/blyt-objcopy"));
    let sdk_clang = sdk.join("bin/blyt-clang");
    if sdk_clang.exists() {
        cmd.env("BLYT_CLANG", &sdk_clang);
    }
    let sdk_ar = sdk.join("bin/blyt-llvm-ar");
    if sdk_ar.exists() {
        cmd.env("BLYT_AR", &sdk_ar);
    }
    cmd.assert().success();

    Command::new(blytplay())
        .args(["--headless", out.to_str().unwrap()])
        .env("BLYT_SAVE_DIR", save_dir.path())
        .assert()
        .success();

    assert!(
        save_dir.path().join("save_id_cart/slot_0.blys").exists(),
        "save slot must land under the manifest id, not the .blyt filename"
    );
    assert!(
        !save_dir.path().join("renamed_output").exists(),
        "no save dir named after the .blyt filename"
    );
}
