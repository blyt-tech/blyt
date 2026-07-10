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

/// Native/hybrid live reload in WASM-dev run mode (issue #124): a cart with
/// native code must hot-swap via the in-VM cart-as-library module swap
/// (blyt_session_swap_cart, issue #127) — not refuse with "reload not supported
/// for carts with native code yet".  This is the WASM analog of the player's
/// session-swap reload (blyt_libretro_reload); the player/native legs already
/// reload native carts, the WASM leg is the one this issue fixes.
///
/// cart_v1 init sets score=7; cart_v2 init sets score=100.  A working reload
/// preserves v1's score=7 across the code swap and fires v2's
/// on_load_state(HOT_RELOAD), so the v2 code reports score=7 reason=3 — a fresh
/// boot would report 100, and no reload at all would keep running v1.
const NATIVE_RELOAD_V1: &str = r#"
#include "blyt.h"
#include "cart_state.h"
#include <stdio.h>

void blyt_cart_init(void) {
    int32_t slot = -1;
    blyt_buffer_alloc_slot(S_GAME, &slot);
    blyt_buffer_set_i32(S_GAME, slot, S_GAME_SCORE, 7);
}
void blyt_cart_update(void) {}
void blyt_cart_draw(void) {}
void blyt_cart_on_load_state(blyt_load_info_t info) {
    char buf[64];
    snprintf(buf, sizeof(buf), "v1 load score=%d reason=%d",
             blyt_buffer_get_i32(S_GAME, 0, S_GAME_SCORE), (int)info.reason);
    blyt_console_debug(buf);
}
"#;

const NATIVE_RELOAD_V2: &str = r#"
#include "blyt.h"
#include "cart_state.h"
#include <stdio.h>

void blyt_cart_init(void) {
    int32_t slot = -1;
    blyt_buffer_alloc_slot(S_GAME, &slot);
    blyt_buffer_set_i32(S_GAME, slot, S_GAME_SCORE, 100);
}
void blyt_cart_update(void) {}
void blyt_cart_draw(void) {}
void blyt_cart_on_load_state(blyt_load_info_t info) {
    char buf[64];
    snprintf(buf, sizeof(buf), "v2 load score=%d reason=%d",
             blyt_buffer_get_i32(S_GAME, 0, S_GAME_SCORE), (int)info.reason);
    blyt_console_debug(buf);
}
"#;

#[test]
fn wasm_dev_control_native_reload() {
    require_wasm();
    let wasm_dir = find_wasm_dir();

    let tmp = TempDir::new().unwrap();

    let project_v1 = tmp.path().join("native_reload_v1");
    CartProject::new()
        .config(DEV_CTRL_CONFIG)
        .c(NATIVE_RELOAD_V1)
        .write(&project_v1);
    let cart_v1 = build_cart(&project_v1);

    let project_v2 = tmp.path().join("native_reload_v2");
    CartProject::new()
        .config(DEV_CTRL_CONFIG)
        .c(NATIVE_RELOAD_V2)
        .write(&project_v2);
    let cart_v2 = build_cart(&project_v2);

    let driver = repo_root().join("tests/wasm/native_reload_test.js");
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

/// Hybrid (Lua + native) live reload in WASM-dev (issue #124).  A hybrid cart
/// runs its Lua host-side over a bridge to native code in the rv32 session, so a
/// reload must refresh BOTH: swap the native module (so new native code runs)
/// AND rebuild the host-Lua VM, with state preserved.  The native module exports
/// `tag()` (v1→1, v2→2) and the Lua `on_load_state` reports the buffer score,
/// the live native tag, and the reason.  A working reload preserves score=7,
/// runs the new native code (tag=2) and the new Lua (`v2 load`), and fires
/// on_load_state(HOT_RELOAD): "v2 load score=7 tag=2 reason=3".  tag=2 (not 1)
/// is what proves the native module — not just the host-Lua side — reloaded.
const HYBRID_RELOAD_C_V1: &str = r#"
#include "blyt.h"
BLYT_LUA_MODULE_EXPORT_RAW(native, tag) { lua_pushinteger(L, 1); return 1; }
"#;
const HYBRID_RELOAD_C_V2: &str = r#"
#include "blyt.h"
BLYT_LUA_MODULE_EXPORT_RAW(native, tag) { lua_pushinteger(L, 2); return 1; }
"#;
const HYBRID_RELOAD_LUA_V1: &str = r#"
local native = require("native")
function init()
    local slot = blyt.buf.alloc_slot(S.GAME)
    S.game[slot].score = 7
end
function update() end
function draw() end
function on_load_state(info)
    blyt.debug.print("v1 load score=" .. S.game[0].score
        .. " tag=" .. native.tag() .. " reason=" .. info.reason)
end
"#;
const HYBRID_RELOAD_LUA_V2: &str = r#"
local native = require("native")
function init()
    local slot = blyt.buf.alloc_slot(S.GAME)
    S.game[slot].score = 100
end
function update() end
function draw() end
function on_load_state(info)
    blyt.debug.print("v2 load score=" .. S.game[0].score
        .. " tag=" .. native.tag() .. " reason=" .. info.reason)
end
"#;

#[test]
fn wasm_dev_control_hybrid_reload() {
    require_wasm();
    require_lua_sdk();
    let wasm_dir = find_wasm_dir();

    let tmp = TempDir::new().unwrap();

    let project_v1 = tmp.path().join("hybrid_reload_v1");
    CartProject::new()
        .config(DEV_CTRL_CONFIG)
        .lua(HYBRID_RELOAD_LUA_V1)
        .c(HYBRID_RELOAD_C_V1)
        .write(&project_v1);
    let cart_v1 = build_lua_cart(&project_v1);

    let project_v2 = tmp.path().join("hybrid_reload_v2");
    CartProject::new()
        .config(DEV_CTRL_CONFIG)
        .lua(HYBRID_RELOAD_LUA_V2)
        .c(HYBRID_RELOAD_C_V2)
        .write(&project_v2);
    let cart_v2 = build_lua_cart(&project_v2);

    let driver = repo_root().join("tests/wasm/native_reload_test.js");
    Command::new("node")
        .args([
            driver.to_str().unwrap(),
            wasm_dir.to_str().unwrap(),
            cart_v1.to_str().unwrap(),
            cart_v2.to_str().unwrap(),
            "tag=2",
        ])
        .assert()
        .success();
}

/// Pure-Lua fast-path `reload` cart-swap must reload the host-Lua resource table
/// from the NEW cart (issue #246).  A pure-Lua cart (g_session == NULL) bundles an
/// uncompressed resource (`greeting.txt`); v1 ships RES_V1, v2 ships RES_V2 with
/// byte-identical Lua code, so the ONLY observable difference across the swap is
/// the bundled resource content.  The cart reads the resource in on_load_state and
/// prints it.
///
/// Resource-table entries are zero-copy aliases into the cart map (resource.c
/// `e->data = body`).  The reload closes cart_v1 and opens cart_v2, so unless
/// `blyt_dev_ctrl_reload_fetched` reloads `g_lua_resources` from cart_v2 (mirroring
/// the native `blyt_hostlua_reload` from #244), every entry dangles into the freed
/// cart_v1 map — the post-swap read returns stale RES_V1 / garbage (a
/// use-after-free), never RES_V2.  Pre-fix this fails; post-fix it re-reads RES_V2.
///
/// This is the WASM leg of the bug #246 characterises; dev_ctrl_test.js only
/// reloads `hello` (zero resources), which is why it slipped through.
const RELOAD_RESOURCE_LUA: &str = r#"
-- 0x20000001 = R_GREETING baked constant (kind RESOURCE, id 1; ADR-0134).
local function greeting()
    return blyt.resource.text_resource(0x20000001):text() or "<nil>"
end
function init()
    blyt.debug.print("init greeting=" .. greeting())
end
function update() end
function draw() end
function on_load_state(info)
    blyt.debug.print("reload greeting=" .. greeting() .. " reason=" .. tostring(info.reason))
end
"#;

#[test]
fn wasm_dev_control_reload_reloads_resource_table() {
    require_wasm();
    require_lua_sdk();
    let wasm_dir = find_wasm_dir();

    let tmp = TempDir::new().unwrap();

    let project_v1 = tmp.path().join("reload_res_v1");
    CartProject::new()
        .config(DEV_CTRL_CONFIG)
        .lua(RELOAD_RESOURCE_LUA)
        .asset("greeting.txt", "RES_V1")
        .write(&project_v1);
    let cart_v1 = build_lua_cart(&project_v1);

    let project_v2 = tmp.path().join("reload_res_v2");
    CartProject::new()
        .config(DEV_CTRL_CONFIG)
        .lua(RELOAD_RESOURCE_LUA)
        .asset("greeting.txt", "RES_V2")
        .write(&project_v2);
    let cart_v2 = build_lua_cart(&project_v2);

    let driver = repo_root().join("tests/wasm/reload_resource_test.js");
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
