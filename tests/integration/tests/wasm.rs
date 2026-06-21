mod common;

use assert_cmd::Command;
use common::{
    CartProject, blyt_bin, build_cart, build_lua_cart, find_wasm_dir, repo_root, require_lua_sdk,
    require_playwright, require_wasm, sdk_dir, write_c_cart_project,
};
use std::fs;
use tempfile::TempDir;

/// WASM gate test: run an idle cart inside blytplay.js under Node.js and
/// compare the first rendered XRGB8888 frame byte-for-byte against the same
/// golden file used by testcard_frame0_matches_golden.
///
/// Requires the WASM runtime to have been built:
///   cmake --build build --target sdk   (requires emcc)
/// (incremental rebuild: cmake --build build/build-wasm).
///
/// Silently skipped when blytplay.js is not found.
#[test]
fn wasm_testcard_frame0_matches_golden() {
    require_wasm();
    let wasm_dir = find_wasm_dir();
    assert!(
        sdk_dir().join("bin/blyt-clang").exists(),
        "SDK not assembled — run `cmake --build build --target sdk` first"
    );

    let golden_path = repo_root().join("tests/testcard_frame0.bin");
    assert!(
        golden_path.exists(),
        "golden file missing: {}",
        golden_path.display()
    );

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("wasm_testcard");
    write_c_cart_project(
        &project,
        r#"
#include "blyt.h"
void blyt_cart_init(void)   {}
void blyt_cart_update(void) {}
void blyt_cart_draw(void)   {}
"#,
    );
    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let frame_path = tmp.path().join("wasm_frame0.bin");
    let driver = repo_root().join("tests/wasm/run_cart.js");

    Command::new("node")
        .args([
            driver.to_str().unwrap(),
            wasm_dir.to_str().unwrap(),
            cart.to_str().unwrap(),
            frame_path.to_str().unwrap(),
        ])
        .assert()
        .success();

    assert!(frame_path.exists(), "wasm_frame0.bin was not written");
    let got = fs::read(&frame_path).expect("reading wasm_frame0.bin");
    let want = fs::read(&golden_path).expect("reading golden");
    assert_eq!(got.len(), want.len(), "WASM frame size mismatch");
    assert_eq!(
        got, want,
        "WASM frame 0 does not match golden testcard_frame0.bin"
    );
}

/// blyt build wasm: exporting a pre-built cart produces a self-contained HTML
/// page with the WASM runtime and cart embedded as base64.
///
/// Silently skipped when blytplay.js is not found.
#[test]
fn build_wasm_produces_html() {
    require_wasm();
    let wasm_dir = find_wasm_dir();
    assert!(
        sdk_dir().join("bin/blyt-clang").exists(),
        "SDK not assembled — run `cmake --build build --target sdk` first"
    );

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("wasm_export");
    write_c_cart_project(
        &project,
        r#"
#include "blyt.h"
void blyt_cart_init(void)   { blyt_console_debug("wasm-export"); }
void blyt_cart_update(void) { blyt_quit(); }
void blyt_cart_draw(void)   {}
"#,
    );
    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let html_path = tmp.path().join("wasm_export.html");
    Command::new(blyt_bin())
        .args([
            "build",
            "wasm",
            cart.to_str().unwrap(),
            "--output",
            html_path.to_str().unwrap(),
        ])
        .env("BLYT_SDK_DIR", sdk_dir())
        .env("BLYT_OBJCOPY", sdk_dir().join("bin/blyt-objcopy"))
        .env("BLYT_WASM_DIR", &wasm_dir)
        .assert()
        .success();

    assert!(
        html_path.exists(),
        "HTML not written to {}",
        html_path.display()
    );
    let html = fs::read_to_string(&html_path).unwrap();
    assert!(
        html.contains("<canvas id=\"canvas\""),
        "HTML missing canvas element"
    );
    assert!(html.contains("wasmBinary"), "HTML missing wasmBinary setup");
    assert!(
        html.contains("FS.writeFile(\"/cart.blyt\""),
        "HTML missing cart preRun hook"
    );
    assert!(
        html.contains("<title>wasm_export</title>"),
        "HTML has wrong or missing title"
    );
    // Both the WASM runtime and cart are base64-embedded; the file must be large.
    assert!(
        html.len() > 512 * 1024,
        "HTML is suspiciously small ({} bytes) — WASM may not be embedded",
        html.len()
    );
}

/// blyt build all: compiling and exporting in one step produces both a .blyt
/// cart and a .html page, with the HTML containing the embedded runtime.
///
/// Silently skipped when blytplay.js is not found.
#[test]
fn build_all_produces_cart_and_html() {
    require_wasm();
    let wasm_dir = find_wasm_dir();
    assert!(
        sdk_dir().join("bin/blyt-clang").exists(),
        "SDK not assembled — run `cmake --build build --target sdk` first"
    );

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("all_cart");
    write_c_cart_project(
        &project,
        r#"
#include "blyt.h"
void blyt_cart_init(void)   { blyt_console_debug("build-all"); }
void blyt_cart_update(void) { blyt_quit(); }
void blyt_cart_draw(void)   {}
"#,
    );

    let sdk = sdk_dir();
    let mut cmd = Command::new(blyt_bin());
    cmd.args(["build", "all", project.to_str().unwrap()])
        .env("BLYT_SDK_DIR", &sdk)
        .env("BLYT_OBJCOPY", sdk.join("bin/blyt-objcopy"))
        .env("BLYT_WASM_DIR", &wasm_dir);
    if sdk.join("bin/blyt-clang").exists() {
        cmd.env("BLYT_CLANG", sdk.join("bin/blyt-clang"));
    }
    cmd.assert().success();

    let cart = project.join("build").join("all_cart.blyt");
    let html_path = project.join("build").join("all_cart.html");

    assert!(cart.exists(), "cart not found at {}", cart.display());
    assert!(
        html_path.exists(),
        "HTML not found at {}",
        html_path.display()
    );

    let html = fs::read_to_string(&html_path).unwrap();
    assert!(
        html.contains("<canvas id=\"canvas\""),
        "HTML missing canvas element"
    );
    assert!(html.contains("wasmBinary"), "HTML missing wasmBinary setup");
    assert!(
        html.len() > 512 * 1024,
        "HTML is suspiciously small ({} bytes) — WASM may not be embedded",
        html.len()
    );
}

/// Browser canvas gate: run the testcard cart in headless Chromium via Playwright,
/// read the canvas via getImageData, and pixel-compare against the golden frame.
/// This is the only test that exercises blyt_js_present's canvas-drawing path.
///
/// Requires playwright + Chromium:
///   cd tests/wasm && npm install && npx playwright install chromium
#[test]
fn wasm_canvas_renders_in_browser() {
    require_wasm();
    require_playwright();
    assert!(
        sdk_dir().join("bin/blyt-clang").exists(),
        "SDK not assembled — run `cmake --build build --target sdk` first"
    );

    let golden_path = repo_root().join("tests/testcard_frame0.bin");
    assert!(
        golden_path.exists(),
        "golden file missing: {}",
        golden_path.display()
    );

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("browser_canvas_cart");
    write_c_cart_project(
        &project,
        r#"
#include "blyt.h"
void blyt_cart_init(void)   {}
void blyt_cart_update(void) {}
void blyt_cart_draw(void)   {}
"#,
    );
    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    /* Build a standalone HTML with the WASM runtime and cart embedded as base64,
     * so the page can be loaded via file:// without a server. */
    let html_path = tmp.path().join("browser_canvas_cart.html");
    Command::new(blyt_bin())
        .args([
            "build",
            "wasm",
            cart.to_str().unwrap(),
            "--output",
            html_path.to_str().unwrap(),
        ])
        .env("BLYT_SDK_DIR", sdk_dir())
        .env("BLYT_OBJCOPY", sdk_dir().join("bin/blyt-objcopy"))
        .env("BLYT_WASM_DIR", find_wasm_dir())
        .assert()
        .success();
    assert!(html_path.exists(), "standalone HTML not written");

    let script = repo_root().join("tests/wasm/browser_canvas_test.mjs");
    Command::new("node")
        .args([
            script.to_str().unwrap(),
            html_path.to_str().unwrap(),
            golden_path.to_str().unwrap(),
        ])
        .assert()
        .success();
}

const DEV_CTRL_CONFIG: &str = "\
records:
  Game:
    fields:
      - { name: score, type: i32 }
state_buffers:
  game:
    record: Game
    count: 1
";

/// Dev control channel C handler (issue #87): drive reset / save_state /
/// load_state / reload straight into blyt_dev_ctrl_command under Node.js and
/// check the JSON responses.  Reload swaps to a second cart build (cart_v2) and
/// must preserve the state buffer across the code swap.  The relay transport is
/// unit-tested in devtool/src/run.rs; this covers the runtime handler.
#[test]
fn wasm_dev_control_lifecycle_commands() {
    require_wasm();
    require_lua_sdk();
    let wasm_dir = find_wasm_dir();
    assert!(
        sdk_dir().join("bin/blyt-luac").exists(),
        "SDK not assembled — run `cmake --build build --target sdk` first"
    );

    let tmp = TempDir::new().unwrap();

    // cart_v1: fresh init sets score=7; on_new_state / on_load_state report it.
    let project_v1 = tmp.path().join("dev_ctrl_v1");
    CartProject::new()
        .config(DEV_CTRL_CONFIG)
        .lua(
            r#"
local slot = -1
function init()
    slot = blyt.buf.alloc_slot(S.GAME)
    S.game[slot].score = 7
end
function update() end
function draw() end
function on_new_state()
    blyt.debug.print("v1 new score=" .. tostring(S.game[slot].score))
end
function on_load_state(info)
    blyt.debug.print("v1 load score=" .. tostring(S.game[slot].score)
        .. " reason=" .. tostring(info.reason))
end
"#,
        )
        .write(&project_v1);
    let cart_v1 = build_lua_cart(&project_v1);

    // cart_v2: same layout, fresh init sets score=100 — so a working reload
    // (which preserves v1's score=7) is distinguishable from a fresh boot.
    let project_v2 = tmp.path().join("dev_ctrl_v2");
    CartProject::new()
        .config(DEV_CTRL_CONFIG)
        .lua(
            r#"
local slot = -1
function init()
    slot = blyt.buf.alloc_slot(S.GAME)
    S.game[slot].score = 100
end
function update() end
function draw() end
function on_load_state(info)
    blyt.debug.print("v2 load score=" .. tostring(S.game[slot].score)
        .. " reason=" .. tostring(info.reason))
end
"#,
        )
        .write(&project_v2);
    let cart_v2 = build_lua_cart(&project_v2);

    let driver = repo_root().join("tests/wasm/dev_ctrl_test.js");
    Command::new("node")
        .args([
            driver.to_str().unwrap(),
            wasm_dir.to_str().unwrap(),
            cart_v1.to_str().unwrap(),
            cart_v2.to_str().unwrap(),
        ])
        .assert()
        .success();
}
