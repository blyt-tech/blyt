mod common;

use assert_cmd::Command;
use common::{
    blyt_bin, build_cart, find_wasm_dir, repo_root, require_playwright, require_wasm, sdk_dir,
    write_c_cart_project,
};
use std::fs;
use tempfile::TempDir;

/// WASM gate test: run an idle cart inside blytplay.js under Node.js and
/// compare the first rendered XRGB8888 frame byte-for-byte against the same
/// golden file used by testcard_frame0_matches_golden.
///
/// Requires the WASM runtime to have been built:
///   emcmake cmake -B build-wasm -S frontends/wasm && cmake --build build-wasm
/// (or cmake --build build --target sdk when emcc is present).
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
