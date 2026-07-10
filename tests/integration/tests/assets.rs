//! Asset pipeline integration tests (issue #91).
//!
//! Covers the text-asset thin slice end to end:
//!   - packed `.blyt`: resource bytes embedded as `.cart.resource.<id>` sections,
//!     loaded via the `blyt_resource_text_get` ECALL — asserted identical across
//!     all three legs (native / WASM / libretro).
//!   - dev mode (project-dir): the runtime reads resource bytes from the
//!     content-addressed staging directory (no embedding), and a `update_assets`
//!     dev-control command hot-swaps an edited asset without a VM restart.

mod common;

use common::{
    CartProject, blyt_bin, blytplay, build_cart, build_cart_expect_failure, build_dev_elf,
    build_lua_cart, find_wasm_dir, libretro_so, repo_root, require_libretro_core, require_lua_sdk,
    require_playwright, require_rust_riscv_target, require_sdk, require_wasm, run_cart_all_legs,
    run_cart_all_legs_evict_every_frame, sdk_dir, test_libretro_core,
};
use std::io::{Read, Write};
use std::net::TcpStream;
use std::time::Duration;
use tempfile::TempDir;

/// C cart that re-reads and prints R_GREETING every frame, so a test can observe
/// a hot-swap take effect on the frame after the swap.  Never quits — the driver
/// stops it.
const PER_FRAME_LOADER_C: &str = r#"
#include "blyt.h"
#include "cart_resources.h"
#include <string.h>

static void print_greeting(void) {
    size_t len = 0;
    const char *t = blyt_resource_text_get(R_GREETING, &len);
    if (t) {
        char buf[256];
        size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
        memcpy(buf, t, n);
        buf[n] = '\0';
        blyt_console_debug(buf);
    }
}

void blyt_cart_init(void) { print_greeting(); }
void blyt_cart_update(void) { print_greeting(); }
void blyt_cart_draw(void) {}
"#;

/// Pure-Lua counterpart of PER_FRAME_LOADER_C: re-reads and prints R.GREETING
/// every frame through the constant-direct accessor (ADR-0134), so the WASM
/// host-Lua fast path (g_session == NULL) can observe a hot-swap on the frame
/// after update_assets. Never quits — the driver stops it.
const PER_FRAME_LOADER_LUA: &str = r#"
local R = require("cart_resources")
local function print_greeting()
    local t = R.GREETING:text()
    if t then
        blyt.debug.print(t)
    end
end
function init() print_greeting() end
function update() print_greeting() end
function draw() end
"#;

/// Minimal C cart that loads R_GREETING and prints its bytes verbatim.
const LOADER_C: &str = r#"
#include "blyt.h"
#include "cart_resources.h"
#include <string.h>

void blyt_cart_init(void) {
    size_t len = 0;
    const char *t = blyt_resource_text_get(R_GREETING, &len);
    if (t) {
        char buf[256];
        size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
        memcpy(buf, t, n);
        buf[n] = '\0';
        blyt_console_debug(buf);
    } else {
        blyt_console_debug("NO RESOURCE");
    }
}

/* Self-terminate after the first frame: the WASM and libretro legs of
 * run_cart_all_legs pass no frame cap, so the cart must call blyt_quit(). */
void blyt_cart_update(void) { blyt_quit(); }
void blyt_cart_draw(void) {}
"#;

/// C cart that pins all 100 distinct resource ids and verifies each pin delivers
/// the *exact* embedded bytes (not merely that the call returned OK — the #98
/// anti-pattern), emitting one deterministic summary line. The native bare-metal
/// path historically capped its resource table at 64 entries (NATIVE_MAX_RES), so
/// the 65th+ distinct id silently returned NOT_FOUND on metal while every emulated
/// leg (host table grows via realloc) served it (#141). Resource ids are 1-based,
/// assigned in sorted-name order, so the zero-padded `res_<k>.dat` assets map id
/// (k+1) -> "payload-<k>". They are raw `.dat` (not text) so the pinned size is
/// exactly the payload length — a text resource's build-appended trailing NUL
/// (#166) would make size == elen+1. The cart references resources by their baked
/// console-wide constant (ADR-0134); a real cart names R_<NAME>, but this test
/// builds the constant from the loop index, so it encodes the id inline
/// (kind RESOURCE = bit 29, cart provenance) — equivalent to BLYT_RESOURCE_ENCODE.
const MANY_RESOURCES_C: &str = r#"
#include "blyt.h"
#include <stdio.h>
#include <string.h>

#define N 100
#define R_ENCODE(id) (0x20000000u | (blyt_resource_id_t)(id)) /* kind RESOURCE, prov cart */

void blyt_cart_init(void) {
    int pin_ok = 0, match = 0, first_bad = -1;
    for (int k = 0; k < N; k++) {
        blyt_resource_id_t id = R_ENCODE(k + 1);
        char expect[32];
        int elen = snprintf(expect, sizeof(expect), "payload-%03d", k);

        const void *ptr = NULL;
        size_t size = 0;
        blyt_result_t pr = blyt_resource_pin(id, &ptr, &size);
        if (pr == BLYT_OK && ptr) {
            pin_ok++;
            if ((int)size == elen && memcmp(ptr, expect, (size_t)elen) == 0)
                match++;
            else if (first_bad < 0)
                first_bad = k + 1;
        } else if (first_bad < 0) {
            first_bad = k + 1;
        }
        blyt_resource_unpin(id);
    }

    char line[128];
    snprintf(line, sizeof(line), "RES_MANY pin_ok=%d match=%d first_bad=%d", pin_ok, match,
             first_bad);
    blyt_console_debug(line);
}

void blyt_cart_update(void) { blyt_quit(); }
void blyt_cart_draw(void) {}
"#;

/// Packed cart with **100** distinct resources: every id pins/loads back its
/// exact embedded bytes, identically across native / WASM / libretro. Pins the
/// host==native parity contract well past the old native 64-entry cap (#141);
/// the native bare-metal cap itself (a distinct artifact) is covered by the
/// QEMU gate.
#[test]
fn many_resources_round_trip_all_legs() {
    require_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("many_resources");
    let mut p = CartProject::new().c(MANY_RESOURCES_C);
    for k in 0..100 {
        p = p.asset_bytes(
            &format!("res_{k:03}.dat"),
            format!("payload-{k:03}").as_bytes(),
        );
    }
    p.write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    run_cart_all_legs(&cart, "RES_MANY pin_ok=100 match=100 first_bad=-1");
}

/// Packed cart: the greeting asset is embedded as an ELF section and reaches the
/// cart through the resource handle API — identically on native, WASM, libretro.
#[test]
fn text_asset_round_trips_all_legs() {
    require_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("assets_packed");
    CartProject::new()
        .c(LOADER_C)
        .asset("greeting.txt", "Hello from assets!")
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    run_cart_all_legs(&cart, "Hello from assets!");
}

/// Pure-Lua cart resolving the resource id through the packer-generated
/// `require("cart_resources")` module (ADR-0040), then reading it via the
/// handle API.  Exercises the bundled-Lua-module path AND the resource binding;
/// asserted identical across native / WASM (host-Lua) / libretro — the WASM leg
/// in particular proves the host-side reimplementation matches the guest one.
const LUA_RESOURCE_REQUIRE: &str = r#"
local R = require("cart_resources")
function init()
    blyt.debug.print(string.format("R[%d]=%s", R.GREETING:id(), R.GREETING:text()))
end
function update() blyt.quit() end
function draw() end
"#;

#[test]
fn lua_resource_require_round_trips_all_legs() {
    require_lua_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("lua_resource_require");
    CartProject::new()
        .lua(LUA_RESOURCE_REQUIRE)
        .asset("greeting.txt", "Hello from assets!")
        .write(&project);

    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    // :id() returns the baked console-wide constant (ADR-0134): R_GREETING is the
    // first resource, id 1 -> 0x20000001 = 536870913.
    run_cart_all_legs(&cart, "R[536870913]=Hello from assets!");
}

/// Pure-Lua cart exercising the constant-direct surface (ADR-0134): a typed
/// text-resource constant, the kind-agnostic module-level `pin`/`unpin` returning
/// a frame-scoped pointer + size, `:text()` owned copy, `:id()`, `__tostring`.
/// One deterministic line pins identical behaviour across native / WASM /
/// libretro.  `tlen` (#txt) is the NUL-stripped content length (18) while `size`
/// is the raw stored length the byte-blind pin reports (19, incl. the
/// build-appended trailing NUL, #166).
const LUA_RESOURCE_LIFECYCLE: &str = r#"
local R = require("cart_resources")
function init()
    -- Constant-direct surface (ADR-0134): the module-level pin/unpin raw escape
    -- hatch takes the constant, and :text() reads the typed string.  tostring on
    -- the constant prints its baked value (R_GREETING = 0x20000001 = 536870913).
    local id = R.GREETING:id()
    local ptr, size = blyt.resource.pin(id)
    blyt.resource.unpin(id)
    local txt = R.GREETING:text()
    blyt.debug.print(string.format(
        "L[%s] tlen=%d size=%d id=%d ptr=%s ts=%s",
        txt, #txt, size, id, tostring(ptr ~= nil), tostring(R.GREETING)))
end
function update() blyt.quit() end
function draw() end
"#;

#[test]
fn lua_resource_lifecycle_round_trips_all_legs() {
    require_lua_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("lua_resource_lifecycle");
    CartProject::new()
        .lua(LUA_RESOURCE_LIFECYCLE)
        .asset("greeting.txt", "Hello from assets!")
        .write(&project);

    let cart = build_lua_cart(&project);
    run_cart_all_legs(
        &cart,
        "L[Hello from assets!] tlen=18 size=19 id=536870913 ptr=true ts=text_resource<536870913>",
    );
}

/// Rust cart exercising the constant-direct SDK resource surface (#94/#166/#196):
/// the packer-generated `R_GREETING: blyt::TextResource` constant (pulled in via
/// `include!(env!("BLYT_CART_RESOURCES_RS"))`), `R_GREETING.text_string()` (owned
/// copy) and `R_GREETING.pin()` → `PinnedText::as_str` (frame-scoped borrow, NUL
/// stripped).  One deterministic line encodes every observable result — the
/// greeting text, the owned-copy length, the borrow length, and that the owned
/// copy equals the borrow — so a single substring match pins identical behaviour
/// across all three legs (and proves the bytes are real, not a green-but-ignored
/// stub).  Both lengths are the NUL-stripped content length (18).
const LIFECYCLE_RUST: &str = r#"#![no_std]

extern crate alloc;
use alloc::format;

include!(env!("BLYT_CART_RESOURCES_RS"));

#[no_mangle]
pub extern "C" fn blyt_cart_init() {
    let owned = R_GREETING.text_string();

    let pinned = R_GREETING.pin();
    let borrowed = pinned.as_str().unwrap();

    blyt::console_debug(&format!(
        "R[{}] owned_len={} pin_len={} eq={}",
        borrowed,
        owned.len(),
        borrowed.len(),
        owned == borrowed
    ));

    drop(pinned);
}

#[no_mangle]
pub extern "C" fn blyt_cart_update() {
    blyt::quit();
}

#[no_mangle]
pub extern "C" fn blyt_cart_draw() {}
"#;

/// Packed Rust cart: the full SDK resource surface produces identical observable
/// output on native, WASM, libretro.
#[test]
fn rust_resource_round_trips_all_legs() {
    require_sdk();
    require_rust_riscv_target();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("rust_resource");
    CartProject::new()
        .rust(LIFECYCLE_RUST)
        .asset("greeting.txt", "Hello from assets!")
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    run_cart_all_legs(
        &cart,
        "R[Hello from assets!] owned_len=18 pin_len=18 eq=true",
    );
}

/// Dev mode: a project-dir dev ELF carries no embedded resources; the runtime
/// reads the bytes from the content-addressed staging directory via the
/// resource-id-index (BLYT_RESOURCE_DIR).
#[test]
fn text_asset_dev_mode_reads_from_staging() {
    require_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("assets_dev");
    CartProject::new()
        .c(LOADER_C)
        .asset("greeting.txt", "Hello from staging!")
        .write(&project);

    let elf = build_dev_elf(&project);
    assert!(elf.exists(), "dev ELF not found at {}", elf.display());

    // blytplay in project-dir mode resolves build/.elf and, finding no embedded
    // resources, reads them from <project>/build via BLYT_RESOURCE_DIR (set
    // automatically by the player in project-dir mode).
    use assert_cmd::Command;
    let out = Command::new(blytplay())
        .args(["--headless", "--quit-after", "3", project.to_str().unwrap()])
        .assert()
        .success()
        .get_output()
        .stdout
        .clone();
    let out = String::from_utf8_lossy(&out);
    assert!(
        out.contains("Hello from staging!"),
        "expected staged asset text in dev-mode output, got: {out}"
    );
}

/// Dev-mode hot-swap: edit the asset, rebuild Phase 1, send `update_assets`, and
/// the next frame must observe the new bytes — no VM restart.
#[test]
fn text_asset_update_assets_hot_swap() {
    require_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("assets_hotswap");
    // A cart that re-reads and prints the greeting every frame, so we can observe
    // the swap take effect on the frame after update_assets.
    CartProject::new()
        .c(r#"
#include "blyt.h"
#include "cart_resources.h"
#include <string.h>

static void print_greeting(void) {
    size_t len = 0;
    const char *t = blyt_resource_text_get(R_GREETING, &len);
    if (t) {
        char buf[256];
        size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
        memcpy(buf, t, n);
        buf[n] = '\0';
        blyt_console_debug(buf);
    }
}

void blyt_cart_init(void) { print_greeting(); }
void blyt_cart_update(void) { print_greeting(); }
void blyt_cart_draw(void) {}
"#)
        .asset("greeting.txt", "VERSION_ONE")
        .write(&project);

    build_dev_elf(&project);

    // Launch the player in project-dir mode with a dev-control port; capture its
    // stdout so we can scan for the swapped text.
    use std::process::{Command as StdCommand, Stdio};
    let mut child = StdCommand::new(blytplay())
        .args([
            "--headless",
            "--dev-ctrl-port",
            "0",
            "--quit-after",
            "100000",
            project.to_str().unwrap(),
        ])
        .stdout(Stdio::piped())
        .spawn()
        .expect("spawn blytplay");

    let mut stdout = child.stdout.take().unwrap();

    // The player prints "Dev control: listening on 127.0.0.1:<port>"; parse it.
    // Read stdout on a background thread, accumulating into a shared buffer.
    use std::sync::{Arc, Mutex};
    let buf = Arc::new(Mutex::new(String::new()));
    let buf_t = buf.clone();
    let reader = std::thread::spawn(move || {
        let mut chunk = [0u8; 1024];
        loop {
            match stdout.read(&mut chunk) {
                Ok(0) => break,
                Ok(n) => buf_t
                    .lock()
                    .unwrap()
                    .push_str(&String::from_utf8_lossy(&chunk[..n])),
                Err(_) => break,
            }
        }
    });

    let port = wait_for_dev_ctrl_port(&buf, Duration::from_secs(10));

    // Edit the asset and rebuild Phase 1 so a new content-addressed staging file
    // + updated resource-id-index exist.
    std::fs::write(project.join("assets/greeting.txt"), "VERSION_TWO").unwrap();
    build_dev_elf(&project);

    // Send update_assets for resource id 1 and wait for the ack.
    let mut sock = TcpStream::connect(("127.0.0.1", port)).expect("connect dev-ctrl");
    sock.set_read_timeout(Some(Duration::from_secs(10)))
        .unwrap();
    sock.write_all(b"{\"id\":1,\"cmd\":\"update_assets\",\"assets\":[1]}\n")
        .unwrap();
    let ack = read_line(&mut sock, Duration::from_secs(10));
    assert!(
        ack.contains("\"status\":\"ok\"") && ack.contains("update_assets"),
        "expected ok ack for update_assets, got: {ack}"
    );

    // Give the cart several frames to print the new content, then stop it.
    std::thread::sleep(Duration::from_millis(500));
    let _ = child.kill();
    let _ = reader.join();

    let out = buf.lock().unwrap().clone();
    assert!(
        out.contains("VERSION_ONE"),
        "expected pre-swap text, got: {out}"
    );
    assert!(
        out.contains("VERSION_TWO"),
        "expected post-swap text after update_assets, got: {out}"
    );
}

fn wait_for_dev_ctrl_port(
    buf: &std::sync::Arc<std::sync::Mutex<String>>,
    timeout: Duration,
) -> u16 {
    let start = std::time::Instant::now();
    loop {
        {
            let s = buf.lock().unwrap();
            if let Some(idx) = s.find("Dev control: listening on 127.0.0.1:") {
                let tail = &s[idx + "Dev control: listening on 127.0.0.1:".len()..];
                let digits: String = tail.chars().take_while(|c| c.is_ascii_digit()).collect();
                if !digits.is_empty() && tail.len() > digits.len() {
                    return digits.parse().unwrap();
                }
            }
        }
        if start.elapsed() > timeout {
            panic!(
                "timed out waiting for dev-control port; output: {}",
                buf.lock().unwrap()
            );
        }
        std::thread::sleep(Duration::from_millis(20));
    }
}

fn read_line(sock: &mut TcpStream, timeout: Duration) -> String {
    let start = std::time::Instant::now();
    let mut acc = String::new();
    let mut byte = [0u8; 1];
    while start.elapsed() < timeout {
        match sock.read(&mut byte) {
            Ok(0) => break,
            Ok(_) => {
                if byte[0] == b'\n' {
                    break;
                }
                acc.push(byte[0] as char);
            }
            Err(_) => break,
        }
    }
    acc
}

/// WASM dev-mode hot-swap (issue #118): the full browser path.  `blyt run` serves
/// the project; headless Chromium loads the real shell.html, which preloads the
/// content-addressed resource staging dir into MEMFS over HTTP and points the
/// host at it via BLYT_RESOURCE_DIR.  Editing the asset drives the *live* watcher
/// → update_assets broadcast → shell.html refetch → reload_resources chain; the
/// swapped bytes must reach the cart with no VM restart.
#[test]
fn text_asset_wasm_dev_mode_hot_swap() {
    require_sdk();
    require_wasm();
    require_playwright();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("assets_wasm_hotswap");
    CartProject::new()
        .c(PER_FRAME_LOADER_C)
        .asset("greeting.txt", "VERSION_ONE")
        .write(&project);

    // Build once up front for a clean failure if the project doesn't compile
    // (the server rebuilds internally, but this surfaces build errors directly).
    build_dev_elf(&project);

    run_wasm_dev_hotswap(
        &project,
        "VERSION_ONE",
        "VERSION_TWO",
        "C per-frame hot-swap",
    );
}

/// Shared WASM hot-swap browser harness: serve `project` via `blyt run`, drive it
/// in headless Chromium through dev_asset_hotswap_test.mjs (edit the greeting
/// asset from `v1` to `v2` and assert the swap reaches the cart with no restart).
/// The caller must have already written + built the project (build_dev_elf).
/// `context` labels the failure so callers can distinguish C vs pure-Lua legs.
fn run_wasm_dev_hotswap(project: &std::path::Path, v1: &str, v2: &str, context: &str) {
    run_wasm_dev_hotswap_asset(project, "greeting.txt", v1, v2, context);
}

/// As `run_wasm_dev_hotswap`, but edits `assets/<asset_rel>` instead of the
/// default `greeting.txt` — used by the raw-resource leg, which edits a `.dat`.
fn run_wasm_dev_hotswap_asset(
    project: &std::path::Path,
    asset_rel: &str,
    v1: &str,
    v2: &str,
    context: &str,
) {
    use std::process::{Command as StdCommand, Stdio};
    use std::sync::{Arc, Mutex};

    let sdk = sdk_dir();
    let mut child = StdCommand::new(blyt_bin())
        .args(["run", project.to_str().unwrap()])
        .env("BLYT_SDK_DIR", &sdk)
        .env("BLYT_OBJCOPY", sdk.join("bin/blyt-objcopy"))
        .env("BLYT_CLANG", sdk.join("bin/blyt-clang"))
        .env("BLYT_WASM_DIR", find_wasm_dir())
        .stdout(Stdio::piped())
        .spawn()
        .expect("spawn blyt run");

    let mut stdout = child.stdout.take().unwrap();
    let buf = Arc::new(Mutex::new(String::new()));
    let buf_t = buf.clone();
    let reader = std::thread::spawn(move || {
        let mut chunk = [0u8; 1024];
        loop {
            match stdout.read(&mut chunk) {
                Ok(0) => break,
                Ok(n) => buf_t
                    .lock()
                    .unwrap()
                    .push_str(&String::from_utf8_lossy(&chunk[..n])),
                Err(_) => break,
            }
        }
    });

    let url = wait_for_serving_url(&buf, Duration::from_secs(30));
    let driver = repo_root().join("tests/wasm/dev_asset_hotswap_test.mjs");
    let asset = project.join("assets").join(asset_rel);
    let status = std::process::Command::new("node")
        .args([
            driver.to_str().unwrap(),
            &url,
            asset.to_str().unwrap(),
            v1,
            v2,
        ])
        .status()
        .expect("run dev_asset_hotswap_test.mjs");

    let _ = child.kill();
    let _ = reader.join();
    assert!(
        status.success(),
        "{context}: dev_asset_hotswap_test.mjs failed; blyt run output:\n{}",
        buf.lock().unwrap()
    );
}

/// Like `run_wasm_dev_hotswap_asset`, but drives the ASSET-SET-CHANGE → `reload`
/// path (issue #246): the browser driver adds `new_asset_rel` (a resource-id-set
/// change → devtool dispatches a bare `reload`, no `update_assets`) while editing
/// `assets/greeting.txt` from `v1` to `v2` in the same coalesced rebuild.  A
/// working reload reloads the host-Lua resource table from the swapped-in cart, so
/// the per-frame reader sees `v2`; pre-fix it aliases the freed old cart map (#246).
fn run_wasm_dev_add_reload(
    project: &std::path::Path,
    new_asset_rel: &str,
    v1: &str,
    v2: &str,
    context: &str,
) {
    use std::process::{Command as StdCommand, Stdio};
    use std::sync::{Arc, Mutex};

    let sdk = sdk_dir();
    let mut child = StdCommand::new(blyt_bin())
        .args(["run", project.to_str().unwrap()])
        .env("BLYT_SDK_DIR", &sdk)
        .env("BLYT_OBJCOPY", sdk.join("bin/blyt-objcopy"))
        .env("BLYT_CLANG", sdk.join("bin/blyt-clang"))
        .env("BLYT_WASM_DIR", find_wasm_dir())
        .stdout(Stdio::piped())
        .spawn()
        .expect("spawn blyt run");

    let mut stdout = child.stdout.take().unwrap();
    let buf = Arc::new(Mutex::new(String::new()));
    let buf_t = buf.clone();
    let reader = std::thread::spawn(move || {
        let mut chunk = [0u8; 1024];
        loop {
            match stdout.read(&mut chunk) {
                Ok(0) => break,
                Ok(n) => buf_t
                    .lock()
                    .unwrap()
                    .push_str(&String::from_utf8_lossy(&chunk[..n])),
                Err(_) => break,
            }
        }
    });

    let url = wait_for_serving_url(&buf, Duration::from_secs(30));
    let driver = repo_root().join("tests/wasm/dev_asset_add_reload_test.mjs");
    let greeting = project.join("assets/greeting.txt");
    let new_asset = project.join("assets").join(new_asset_rel);
    let status = std::process::Command::new("node")
        .args([
            driver.to_str().unwrap(),
            &url,
            greeting.to_str().unwrap(),
            new_asset.to_str().unwrap(),
            v1,
            v2,
        ])
        .status()
        .expect("run dev_asset_add_reload_test.mjs");

    let _ = child.kill();
    let _ = reader.join();
    assert!(
        status.success(),
        "{context}: dev_asset_add_reload_test.mjs failed; blyt run output:\n{}",
        buf.lock().unwrap()
    );
}

/// End-to-end reachability for #246: adding an asset to a running pure-Lua
/// WASM-dev cart forces devtool's dispatch_signals to emit a bare `reload` (no
/// `update_assets` — the resource-id set changed), which is the real flow that
/// makes the host-Lua resource-table use-after-free reachable in normal dev use.
/// The greeting the cart reads every frame is edited to v2 in the same rebuild, so
/// the post-reload read reveals whether the table reloaded from the swapped-in
/// cart (v2) or still aliases the freed old cart (stale v1).  Pre-fix this times
/// out on v2; post-fix v2 appears.  (The dispatch decision itself is unit-tested
/// in devtool: dispatch_resource_set_change_forces_reload.)
#[test]
fn asset_add_forces_reload_reloads_resource_lua_wasm() {
    require_lua_sdk();
    require_wasm();
    require_playwright();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("assets_wasm_add_reload_lua");
    CartProject::new()
        .lua(PER_FRAME_LOADER_LUA)
        .asset("greeting.txt", "VERSION_ONE")
        .write(&project);

    build_dev_elf(&project);
    run_wasm_dev_add_reload(
        &project,
        "extra.txt",
        "VERSION_ONE",
        "VERSION_TWO",
        "pure-Lua asset-add forces reload",
    );
}

/// WASM dev-mode hot-swap, pure-Lua fast path (issue #120): same browser path as
/// the C test above, but the cart is pure-Lua so it runs host-side in g_lua with
/// g_session == NULL.  The swap must reach it through the host-Lua resource table
/// reload — proving the #118 update_assets gate is gone and the fast-path table is
/// reloaded between frames with no VM restart.
#[test]
fn text_asset_wasm_dev_mode_hot_swap_lua() {
    require_lua_sdk();
    require_wasm();
    require_playwright();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("assets_wasm_hotswap_lua");
    CartProject::new()
        .lua(PER_FRAME_LOADER_LUA)
        .asset("greeting.txt", "VERSION_ONE")
        .write(&project);

    build_dev_elf(&project);
    run_wasm_dev_hotswap(
        &project,
        "VERSION_ONE",
        "VERSION_TWO",
        "pure-Lua per-frame hot-swap",
    );
}

/// Parse the `blyt run: serving on http://127.0.0.1:<port>/` banner into a URL.
fn wait_for_serving_url(
    buf: &std::sync::Arc<std::sync::Mutex<String>>,
    timeout: Duration,
) -> String {
    const MARKER: &str = "serving on http://";
    let start = std::time::Instant::now();
    loop {
        {
            let s = buf.lock().unwrap();
            if let Some(idx) = s.find(MARKER) {
                let tail = &s[idx + MARKER.len()..];
                let end = tail.find(char::is_whitespace).unwrap_or(tail.len());
                let hostport = tail[..end].trim_end_matches('/');
                if hostport.contains(':') {
                    return format!("http://{hostport}/");
                }
            }
        }
        if start.elapsed() > timeout {
            panic!(
                "timed out waiting for blyt run serving URL; output: {}",
                buf.lock().unwrap()
            );
        }
        std::thread::sleep(Duration::from_millis(20));
    }
}

// ---------------------------------------------------------------------------
// on_assets_reloaded — dev-only asset hot-swap callback (issue #122)
//
// The asset hot-swap (#91/#118) only helps carts that re-read a resource every
// frame.  A realistic cart reads once and *derives* something it caches; a pure
// table swap then changes nothing it can observe.  on_assets_reloaded(ids) fires
// after the swap so such a cart can re-derive only the affected resources.
//
// These carts deliberately cache at init and never re-read in update(), so the
// swapped bytes can surface only if the callback fired — closing the same
// test-coverage hole (#98 `(void)info`) the existing per-frame-reader tests miss.
// ---------------------------------------------------------------------------

/// C cart: caches R_GREETING at init and re-emits the cached copy every frame.
/// It re-derives ONLY in on_assets_reloaded — update() never re-reads — so a
/// post-swap VERSION_TWO proves the callback fired, not a per-frame re-read.
const CACHE_AT_INIT_C: &str = r#"
#include "blyt.h"
#include "cart_resources.h"
#include <string.h>

static char g_cached[256];

static void cache_greeting(void) {
    size_t len = 0;
    const char *t = blyt_resource_text_get(R_GREETING, &len);
    if (t) {
        size_t n = len < sizeof(g_cached) - 1 ? len : sizeof(g_cached) - 1;
        memcpy(g_cached, t, n);
        g_cached[n] = '\0';
    }
}

void blyt_cart_init(void) { cache_greeting(); }

void blyt_cart_on_assets_reloaded(const uint32_t *ids, size_t n) {
    (void)ids;
    (void)n;
    cache_greeting();
}

void blyt_cart_update(void) { blyt_console_debug(g_cached); }
void blyt_cart_draw(void) {}
"#;

/// Lua cart: echoes the ids passed to on_assets_reloaded(ids) every frame. Lua
/// has no resource-read API yet (#93/#120), so this asserts the *payload* — the
/// changed-id set the callback receives — not byte re-derivation.
const IDS_ECHO_LUA: &str = r#"
local got
function init() end
function on_assets_reloaded(ids)
    local parts = {}
    for i = 1, #ids do
        parts[i] = tostring(ids[i])
    end
    got = "reloaded:" .. table.concat(parts, ",")
end
function update()
    if got then
        blyt32.debug.print(got)
    end
end
function draw() end
"#;

/// Rust cart: lowers a `#[no_mangle] extern "C"` on_assets_reloaded to the same C
/// symbol the host dispatches generically.  Emits its marker only when it
/// received exactly ids==[7], proving the (ptr, count) marshalling and values.
const IDS_ECHO_RUST: &str = r#"
#![no_std]

use core::sync::atomic::{AtomicBool, Ordering};

static GOT: AtomicBool = AtomicBool::new(false);

#[no_mangle]
pub extern "C" fn blyt_cart_init() {}

#[no_mangle]
pub extern "C" fn blyt_cart_on_assets_reloaded(ids: *const u32, n: usize) {
    if !ids.is_null() && n == 1 && unsafe { *ids } == 7 {
        GOT.store(true, Ordering::Relaxed);
    }
}

#[no_mangle]
pub extern "C" fn blyt_cart_update() {
    if GOT.load(Ordering::Relaxed) {
        blyt::console_debug("reloaded:7");
    }
}

#[no_mangle]
pub extern "C" fn blyt_cart_draw() {}
"#;

/// Recursively copy `src` into `dst` (used to snapshot a v1 resource staging dir
/// before rebuilding v2 in place).
fn copy_dir(src: &std::path::Path, dst: &std::path::Path) {
    std::fs::create_dir_all(dst).unwrap();
    for entry in std::fs::read_dir(src).unwrap() {
        let entry = entry.unwrap();
        let from = entry.path();
        let to = dst.join(entry.file_name());
        if entry.file_type().unwrap().is_dir() {
            copy_dir(&from, &to);
        } else {
            std::fs::copy(&from, &to).unwrap();
        }
    }
}

/// Launch blytplay on a project-dir dev build with a dev-control port, optionally
/// edit+rebuild an asset, send one `update_assets` carrying `assets_json` (e.g.
/// "[1]"), then let it run briefly and return everything it printed to stdout.
/// The caller must have already built the v1 dev ELF.
fn native_update_assets_capture(
    project: &std::path::Path,
    edit_asset: Option<(&str, &str)>,
    assets_json: &str,
) -> String {
    use std::process::{Command as StdCommand, Stdio};
    use std::sync::{Arc, Mutex};

    let mut child = StdCommand::new(blytplay())
        .args([
            "--headless",
            "--dev-ctrl-port",
            "0",
            "--quit-after",
            "100000",
            project.to_str().unwrap(),
        ])
        .stdout(Stdio::piped())
        .spawn()
        .expect("spawn blytplay");

    let mut stdout = child.stdout.take().unwrap();
    let buf = Arc::new(Mutex::new(String::new()));
    let buf_t = buf.clone();
    let reader = std::thread::spawn(move || {
        let mut chunk = [0u8; 1024];
        loop {
            match stdout.read(&mut chunk) {
                Ok(0) => break,
                Ok(n) => buf_t
                    .lock()
                    .unwrap()
                    .push_str(&String::from_utf8_lossy(&chunk[..n])),
                Err(_) => break,
            }
        }
    });

    let port = wait_for_dev_ctrl_port(&buf, Duration::from_secs(10));

    if let Some((rel, content)) = edit_asset {
        std::fs::write(project.join("assets").join(rel), content).unwrap();
        build_dev_elf(project);
    }

    let mut sock = TcpStream::connect(("127.0.0.1", port)).expect("connect dev-ctrl");
    sock.set_read_timeout(Some(Duration::from_secs(10)))
        .unwrap();
    let cmd = format!("{{\"id\":1,\"cmd\":\"update_assets\",\"assets\":{assets_json}}}\n");
    sock.write_all(cmd.as_bytes()).unwrap();
    let ack = read_line(&mut sock, Duration::from_secs(10));
    assert!(
        ack.contains("\"status\":\"ok\"") && ack.contains("update_assets"),
        "expected ok ack for update_assets, got: {ack}"
    );

    std::thread::sleep(Duration::from_millis(500));
    let _ = child.kill();
    let _ = reader.join();
    buf.lock().unwrap().clone()
}

/// Drive the libretro core over `cart`, firing update_assets at frame `after`
/// with `asset_ids` (a comma list), optionally repointing BLYT_RESOURCE_DIR at
/// `res_dir_v2` first; run exactly `frames` frames. `res_dir_v1` seeds the
/// initial resource dir. Returns the core's stderr (where cart debug output goes).
fn libretro_update_assets_capture(
    cart: &std::path::Path,
    res_dir_v1: Option<&std::path::Path>,
    res_dir_v2: Option<&std::path::Path>,
    asset_ids: &str,
    after: u32,
    frames: u32,
) -> String {
    use assert_cmd::Command;
    let mut cmd = Command::new(test_libretro_core());
    cmd.arg("--run-frames")
        .arg(frames.to_string())
        .arg("--update-assets-after")
        .arg(after.to_string())
        .arg("--asset-ids")
        .arg(asset_ids);
    if let Some(v2) = res_dir_v2 {
        cmd.arg("--resource-dir-v2").arg(v2);
    }
    cmd.arg(libretro_so()).arg(cart);
    if let Some(v1) = res_dir_v1 {
        cmd.env("BLYT_RESOURCE_DIR", v1);
    }
    let out = cmd.assert().success().get_output().stderr.clone();
    String::from_utf8_lossy(&out).into_owned()
}

/// Native leg: a cache-at-init C cart sees swapped bytes only because
/// on_assets_reloaded fired (update() never re-reads).
#[test]
fn on_assets_reloaded_c_native() {
    require_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("reload_cb_c_native");
    CartProject::new()
        .c(CACHE_AT_INIT_C)
        .asset("greeting.txt", "VERSION_ONE")
        .write(&project);
    build_dev_elf(&project);

    let out = native_update_assets_capture(&project, Some(("greeting.txt", "VERSION_TWO")), "[1]");
    assert!(
        out.contains("VERSION_ONE"),
        "expected pre-swap cached text, got: {out}"
    );
    assert!(
        out.contains("VERSION_TWO"),
        "expected re-derived text after on_assets_reloaded, got: {out}"
    );
}

/// WASM leg: same cache-at-init C cart, driven through the real browser path
/// (live watcher → update_assets broadcast → refetch → reload → callback).
#[test]
fn on_assets_reloaded_c_wasm() {
    require_sdk();
    require_wasm();
    require_playwright();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("reload_cb_c_wasm");
    CartProject::new()
        .c(CACHE_AT_INIT_C)
        .asset("greeting.txt", "VERSION_ONE")
        .write(&project);
    build_dev_elf(&project);

    run_wasm_dev_hotswap(
        &project,
        "VERSION_ONE",
        "VERSION_TWO",
        "C cache-at-init callback",
    );
}

/// Libretro leg: load the (identical-code) dev ELF reading v1 resources, then at
/// frame 2 repoint BLYT_RESOURCE_DIR at the v2 staging dir and fire update_assets.
/// The cache-at-init cart prints VERSION_ONE until the callback re-derives v2.
#[test]
fn on_assets_reloaded_c_libretro() {
    require_sdk();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("reload_cb_c_libretro");
    CartProject::new()
        .c(CACHE_AT_INIT_C)
        .asset("greeting.txt", "VERSION_ONE")
        .write(&project);

    // Build v1, snapshot its staging dir, then rebuild v2 in place. The cart code
    // is identical across both builds; only the resource bytes differ.
    build_dev_elf(&project);
    let build_dir = project.join("build");
    let v1_dir = tmp.path().join("res_v1");
    copy_dir(&build_dir, &v1_dir);

    std::fs::write(project.join("assets/greeting.txt"), "VERSION_TWO").unwrap();
    let elf = build_dev_elf(&project); // build_dir now holds v2 staging

    let out = libretro_update_assets_capture(&elf, Some(&v1_dir), Some(&build_dir), "1", 2, 6);
    assert!(
        out.contains("VERSION_ONE"),
        "expected pre-swap cached text on libretro, got: {out}"
    );
    assert!(
        out.contains("VERSION_TWO"),
        "expected re-derived text after on_assets_reloaded on libretro, got: {out}"
    );
}

/// Lua leg (native, emulated): the global on_assets_reloaded(ids) receives the
/// changed-id set as a 1-based table. Payload-only (Lua has no resource read).
#[test]
fn on_assets_reloaded_lua_native_ids_payload() {
    require_sdk();
    require_lua_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("reload_cb_lua_native");
    CartProject::new().lua(IDS_ECHO_LUA).write(&project);
    build_dev_elf(&project);

    let out = native_update_assets_capture(&project, None, "[1,2]");
    assert!(
        out.contains("reloaded:1,2"),
        "expected Lua on_assets_reloaded to receive ids [1,2], got: {out}"
    );
}

/// Lua leg (libretro, emulated): same payload assertion via a packed cart driven
/// by the libretro core's update_assets trigger.
#[test]
fn on_assets_reloaded_lua_libretro_ids_payload() {
    require_sdk();
    require_lua_sdk();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("reload_cb_lua_libretro");
    CartProject::new().lua(IDS_ECHO_LUA).write(&project);
    let cart = build_lua_cart(&project);

    let out = libretro_update_assets_capture(&cart, None, None, "7", 2, 6);
    assert!(
        out.contains("reloaded:7"),
        "expected Lua on_assets_reloaded to receive id [7] on libretro, got: {out}"
    );
}

/// Lua cart: caches the greeting at init via the resource API and re-emits the
/// cached copy every frame.  It re-reads ONLY in on_assets_reloaded — update()
/// never re-reads — so a post-swap VERSION_TWO proves the callback fired AND the
/// Lua resource binding observed the swapped bytes.  This is the bytes-level
/// re-derivation coverage the payload-only Lua tests above cannot give, now that
/// #93 lands the read API on the emulated legs (#122 follow-up).
const CACHE_AT_INIT_LUA: &str = r#"
local cached
-- 0x20000001 = R_GREETING baked constant (kind RESOURCE, id 1; ADR-0134).
local function cache_greeting()
    local t = blyt.resource.text_resource(0x20000001):text()
    if t then
        cached = t
    end
end
function init() cache_greeting() end
function on_assets_reloaded(_ids) cache_greeting() end
function update()
    if cached then
        blyt.debug.print(cached)
    end
end
function draw() end
"#;

/// Lua leg (native, emulated): a cache-at-init Lua cart re-derives swapped bytes
/// only because on_assets_reloaded fired and the Lua resource read returned the
/// new content (update() never re-reads).
#[test]
fn on_assets_reloaded_lua_native_bytes() {
    require_sdk();
    require_lua_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("reload_bytes_lua_native");
    CartProject::new()
        .lua(CACHE_AT_INIT_LUA)
        .asset("greeting.txt", "VERSION_ONE")
        .write(&project);
    build_dev_elf(&project);

    let out = native_update_assets_capture(&project, Some(("greeting.txt", "VERSION_TWO")), "[1]");
    assert!(
        out.contains("VERSION_ONE"),
        "expected pre-swap cached text, got: {out}"
    );
    assert!(
        out.contains("VERSION_TWO"),
        "expected Lua-re-derived text after on_assets_reloaded, got: {out}"
    );
}

/// Lua leg (WASM host-Lua fast path, issue #120): the same cache-at-init Lua cart
/// driven through the real browser path.  With g_session == NULL there is no rv32
/// session to reload, so a post-swap VERSION_TWO proves the host-Lua fast path
/// reloaded its own g_lua_resources table AND fired on_assets_reloaded in g_lua —
/// the wiring this issue adds.  update() never re-reads, so a per-frame re-read
/// cannot mask a missing callback.
#[test]
fn on_assets_reloaded_lua_wasm_bytes() {
    require_lua_sdk();
    require_wasm();
    require_playwright();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("reload_bytes_lua_wasm");
    CartProject::new()
        .lua(CACHE_AT_INIT_LUA)
        .asset("greeting.txt", "VERSION_ONE")
        .write(&project);
    build_dev_elf(&project);

    run_wasm_dev_hotswap(
        &project,
        "VERSION_ONE",
        "VERSION_TWO",
        "pure-Lua cache-at-init callback",
    );
}

/// Lua leg (libretro, emulated): same cache-at-init Lua cart; at frame 2 repoint
/// BLYT_RESOURCE_DIR at the v2 staging dir and fire update_assets.  The cart
/// prints VERSION_ONE until the callback re-reads v2 through the Lua binding.
#[test]
fn on_assets_reloaded_lua_libretro_bytes() {
    require_sdk();
    require_lua_sdk();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("reload_bytes_lua_libretro");
    CartProject::new()
        .lua(CACHE_AT_INIT_LUA)
        .asset("greeting.txt", "VERSION_ONE")
        .write(&project);

    build_dev_elf(&project);
    let build_dir = project.join("build");
    let v1_dir = tmp.path().join("res_v1");
    copy_dir(&build_dir, &v1_dir);

    std::fs::write(project.join("assets/greeting.txt"), "VERSION_TWO").unwrap();
    let elf = build_dev_elf(&project); // build_dir now holds v2 staging

    let out = libretro_update_assets_capture(&elf, Some(&v1_dir), Some(&build_dir), "1", 2, 6);
    assert!(
        out.contains("VERSION_ONE"),
        "expected pre-swap cached text on libretro, got: {out}"
    );
    assert!(
        out.contains("VERSION_TWO"),
        "expected Lua-re-derived text after on_assets_reloaded on libretro, got: {out}"
    );
}

/// Rust leg (libretro, emulated): a Rust cart's `extern "C"` on_assets_reloaded
/// receives the ids through the same C symbol, proving the Rust surface + ABI.
#[test]
fn on_assets_reloaded_rust_libretro_ids_payload() {
    require_sdk();
    require_rust_riscv_target();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("reload_cb_rust_libretro");
    CartProject::new().rust(IDS_ECHO_RUST).write(&project);
    let cart = build_cart(&project);

    let out = libretro_update_assets_capture(&cart, None, None, "7", 2, 6);
    assert!(
        out.contains("reloaded:7"),
        "expected Rust on_assets_reloaded to receive id [7] on libretro, got: {out}"
    );
}

// ---------------------------------------------------------------------------
// raw / opaque resource type (issue #162)
//
// A `.dat` (any non-.txt extension declared via an `include:` glob) is a `raw`
// resource: the cart receives the exact, uninterpreted bytes.  These mirror the
// text-asset tests above but (a) use the resource *pin* surface (size is
// authoritative — `blyt_resource_text_get` would mishandle an embedded NUL) and
// (b) assert the **value** across all three legs.
// ---------------------------------------------------------------------------

/// C cart that pins R_BLOB and emits its exact bytes as a deterministic hex
/// line, so a single substring match pins identical opaque bytes across legs.
/// The payload deliberately contains an embedded NUL and a high byte (0xFF),
/// which a NUL-terminated text read could not round-trip.
const RAW_HEX_LOADER_C: &str = r#"
#include "blyt.h"
#include "cart_resources.h"
#include <stdio.h>

void blyt_cart_init(void) {
    const void *ptr = NULL;
    size_t size = 0;
    blyt_resource_pin(R_BLOB, &ptr, &size);
    const unsigned char *p = (const unsigned char *)ptr;
    char line[256];
    int off = snprintf(line, sizeof(line), "BLOB len=%d bytes=", (int)size);
    for (size_t i = 0; i < size && off + 2 < (int)sizeof(line); i++)
        off += snprintf(line + off, (size_t)((int)sizeof(line) - off), "%02x", p[i]);
    blyt_resource_unpin(R_BLOB);
    blyt_console_debug(line);
}

void blyt_cart_update(void) { blyt_quit(); }
void blyt_cart_draw(void) {}
"#;

/// C cart that caches R_BLOB's bytes via pin at init and re-emits the cached
/// copy every frame, re-deriving ONLY in on_assets_reloaded — so a post-swap
/// value proves the callback fired for a raw resource (not a per-frame re-read).
/// Used with printable payloads so the swap is easy to read.
const CACHE_AT_INIT_RAW_C: &str = r#"
#include "blyt.h"
#include "cart_resources.h"
#include <string.h>

static char g_cached[256];

static void cache_blob(void) {
    const void *ptr = NULL;
    size_t size = 0;
    if (blyt_resource_pin(R_BLOB, &ptr, &size) == BLYT_OK && ptr) {
        size_t n = size < sizeof(g_cached) - 1 ? size : sizeof(g_cached) - 1;
        memcpy(g_cached, ptr, n);
        g_cached[n] = '\0';
    }
    blyt_resource_unpin(R_BLOB);
}

void blyt_cart_init(void) { cache_blob(); }
void blyt_cart_on_assets_reloaded(const uint32_t *ids, size_t n) {
    (void)ids;
    (void)n;
    cache_blob();
}
void blyt_cart_update(void) { blyt_console_debug(g_cached); }
void blyt_cart_draw(void) {}
"#;

/// C cart that re-pins R_BLOB every frame and prints it — the per-frame reader
/// the WASM browser hot-swap harness needs (drives the live watcher path).
const PER_FRAME_RAW_C: &str = r#"
#include "blyt.h"
#include "cart_resources.h"
#include <string.h>

void blyt_cart_init(void) {}
void blyt_cart_update(void) {
    const void *ptr = NULL;
    size_t size = 0;
    if (blyt_resource_pin(R_BLOB, &ptr, &size) == BLYT_OK && ptr) {
        char buf[256];
        size_t n = size < sizeof(buf) - 1 ? size : sizeof(buf) - 1;
        memcpy(buf, ptr, n);
        buf[n] = '\0';
        blyt_console_debug(buf);
    }
    blyt_resource_unpin(R_BLOB);
}
void blyt_cart_draw(void) {}
"#;

/// Packed cart: a raw `.dat` resource reaches the cart as identical opaque bytes
/// (embedded NUL + high byte) on native, WASM, libretro.
#[test]
fn raw_asset_round_trips_all_legs() {
    require_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("raw_packed");
    let payload: &[u8] = &[0x00, 0xFF, 0x10, b'h', b'i', 0x00];
    CartProject::new()
        .c(RAW_HEX_LOADER_C)
        .asset_bytes("blob.dat", payload)
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    run_cart_all_legs(&cart, "BLOB len=6 bytes=00ff10686900");
}

/// Native dev leg: a raw `.dat` hot-swaps and the cache-at-init cart sees the new
/// bytes only because on_assets_reloaded fired (update() never re-reads).
#[test]
fn on_assets_reloaded_raw_c_native() {
    require_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("raw_reload_native");
    CartProject::new()
        .c(CACHE_AT_INIT_RAW_C)
        .asset("blob.dat", "RAW_ONE")
        .write(&project);
    build_dev_elf(&project);

    let out = native_update_assets_capture(&project, Some(("blob.dat", "RAW_TWO")), "[1]");
    assert!(
        out.contains("RAW_ONE"),
        "expected pre-swap cached raw bytes, got: {out}"
    );
    assert!(
        out.contains("RAW_TWO"),
        "expected re-derived raw bytes after on_assets_reloaded, got: {out}"
    );
}

/// WASM dev leg: the same raw hot-swap through the real browser path (live
/// watcher → update_assets → refetch → reload), a per-frame raw reader observing
/// the swapped bytes with no VM restart.
#[test]
fn on_assets_reloaded_raw_c_wasm() {
    require_sdk();
    require_wasm();
    require_playwright();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("raw_reload_wasm");
    CartProject::new()
        .c(PER_FRAME_RAW_C)
        .asset("blob.dat", "VERSION_ONE")
        .write(&project);
    build_dev_elf(&project);

    run_wasm_dev_hotswap_asset(
        &project,
        "blob.dat",
        "VERSION_ONE",
        "VERSION_TWO",
        "C per-frame raw hot-swap",
    );
}

/// Libretro dev leg: load the dev ELF reading v1 raw bytes, then at frame 2
/// repoint BLYT_RESOURCE_DIR at the v2 staging dir and fire update_assets; the
/// cache-at-init raw cart prints RAW_ONE until the callback re-derives RAW_TWO.
#[test]
fn on_assets_reloaded_raw_c_libretro() {
    require_sdk();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("raw_reload_libretro");
    CartProject::new()
        .c(CACHE_AT_INIT_RAW_C)
        .asset("blob.dat", "RAW_ONE")
        .write(&project);

    build_dev_elf(&project);
    let build_dir = project.join("build");
    let v1_dir = tmp.path().join("res_v1");
    copy_dir(&build_dir, &v1_dir);

    std::fs::write(project.join("assets/blob.dat"), "RAW_TWO").unwrap();
    let elf = build_dev_elf(&project); // build_dir now holds v2 staging

    let out = libretro_update_assets_capture(&elf, Some(&v1_dir), Some(&build_dir), "1", 2, 6);
    assert!(
        out.contains("RAW_ONE"),
        "expected pre-swap cached raw bytes on libretro, got: {out}"
    );
    assert!(
        out.contains("RAW_TWO"),
        "expected re-derived raw bytes after on_assets_reloaded on libretro, got: {out}"
    );
}

// ---------------------------------------------------------------------------
// get_bytes — owned opaque-bytes copy (issue #162)
//
// The companion to text_get/:text()/text_string for binary resources: copies the
// resource's exact bytes into guest-owned memory that outlives the frame, with no
// NUL terminator and no UTF-8 assumption.  Each cart reads the same binary blob
// (embedded NUL + 0xFF) and emits a deterministic hex line; asserted identical
// across native / WASM / libretro.
// ---------------------------------------------------------------------------

const BLOB_HEX: &str = "BYTES len=6 hex=00ff10686900";

/// The opaque payload all three get_bytes carts read: embedded NUL and a high
/// byte, which a NUL-terminated text read could not round-trip.
fn blob_payload() -> Vec<u8> {
    vec![0x00, 0xFF, 0x10, b'h', b'i', 0x00]
}

/// C cart: blyt_resource_bytes_get → owned copy (survives unpin), hex-encode the
/// exact bytes, free.
const BYTES_GET_C: &str = r#"
#include "blyt.h"
#include "cart_resources.h"
#include <stdio.h>
#include <stdlib.h>

void blyt_cart_init(void) {
    size_t len = 0;
    unsigned char *b = (unsigned char *)blyt_resource_bytes_get(R_BLOB, &len);
    char line[256];
    int off = snprintf(line, sizeof(line), "BYTES len=%d hex=", (int)len);
    for (size_t i = 0; i < len && off + 2 < (int)sizeof(line); i++)
        off += snprintf(line + off, (size_t)((int)sizeof(line) - off), "%02x", b[i]);
    free(b);
    blyt_console_debug(line);
}

void blyt_cart_update(void) { blyt_quit(); }
void blyt_cart_draw(void) {}
"#;

/// Rust cart: BytesResource::bytes_vec → owned Vec<u8>, hex-encode.
const BYTES_GET_RUST: &str = r#"#![no_std]

extern crate alloc;
use alloc::format;
use alloc::string::String;

include!(env!("BLYT_CART_RESOURCES_RS"));

#[no_mangle]
pub extern "C" fn blyt_cart_init() {
    let bytes = R_BLOB.bytes_vec();
    let mut hex = String::new();
    for b in &bytes {
        hex.push_str(&format!("{:02x}", b));
    }
    blyt::console_debug(&format!("BYTES len={} hex={}", bytes.len(), hex));
}

#[no_mangle]
pub extern "C" fn blyt_cart_update() {
    blyt::quit();
}

#[no_mangle]
pub extern "C" fn blyt_cart_draw() {}
"#;

/// Lua cart: res:bytes() → Lua string of the exact bytes (binary-safe), hex-encode
/// via string.byte.
const BYTES_GET_LUA: &str = r#"
local R = require("cart_resources")
function init()
    local s = R.BLOB:bytes()
    local hex = {}
    for i = 1, #s do
        hex[i] = string.format("%02x", string.byte(s, i))
    end
    blyt.debug.print(string.format("BYTES len=%d hex=%s", #s, table.concat(hex)))
end
function update() blyt.quit() end
function draw() end
"#;

/// Packed C cart: blyt_resource_bytes_get returns the exact opaque bytes,
/// identically across native / WASM / libretro.
#[test]
fn c_bytes_get_round_trips_all_legs() {
    require_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("bytes_get_c");
    CartProject::new()
        .c(BYTES_GET_C)
        .asset_bytes("blob.dat", &blob_payload())
        .write(&project);

    let cart = build_cart(&project);
    run_cart_all_legs(&cart, BLOB_HEX);
}

/// Packed Rust cart: BytesResource::bytes_vec returns the exact opaque bytes,
/// identically across native / WASM / libretro.
#[test]
fn rust_bytes_vec_round_trips_all_legs() {
    require_sdk();
    require_rust_riscv_target();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("bytes_get_rust");
    CartProject::new()
        .rust(BYTES_GET_RUST)
        .asset_bytes("blob.dat", &blob_payload())
        .write(&project);

    let cart = build_cart(&project);
    run_cart_all_legs(&cart, BLOB_HEX);
}

/// Packed Lua cart: res:bytes() returns the exact opaque bytes as a binary-safe
/// Lua string, identically across native / WASM (host-Lua) / libretro.
#[test]
fn lua_resource_bytes_round_trips_all_legs() {
    require_lua_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("bytes_get_lua");
    CartProject::new()
        .lua(BYTES_GET_LUA)
        .asset_bytes("blob.dat", &blob_payload())
        .write(&project);

    let cart = build_lua_cart(&project);
    run_cart_all_legs(&cart, BLOB_HEX);
}

// ---------------------------------------------------------------------------
// Typed text/bytes handles + text NUL-termination contract (issue #166)
//
// Text resources are stored with a build-appended trailing NUL; the runtime is
// byte-blind, so the low-level pin reports the stored length INCLUDING the NUL
// (asserted by resource_lifecycle/lua_resource_lifecycle above: bytes/size=19),
// while the text accessors strip it and report the content length. These pin the
// stripped length identically across legs, and the C error path (text_get on a
// raw resource → NULL via the missing trailing NUL). Build-time text validation
// and the Rust typed-handle compile barrier are single-build (compile) tests.
// ---------------------------------------------------------------------------

/// C cart: blyt_resource_text_get reports the NUL-stripped content length (18 for
/// the 18-byte greeting, not the stored 19) and the buffer is the content with no
/// trailing junk. Pins the text accessor's length contract across all legs (#166).
const TEXT_LEN_C: &str = r#"
#include "blyt.h"
#include "cart_resources.h"
#include <stdio.h>
#include <stdlib.h>

void blyt_cart_init(void) {
    size_t len = 0;
    char *t = blyt_resource_text_get(R_GREETING, &len);
    char line[64];
    snprintf(line, sizeof(line), "TXTLEN=%d:%s", (int)len, t ? t : "NULL");
    blyt_console_debug(line);
    free(t);
}

void blyt_cart_update(void) { blyt_quit(); }
void blyt_cart_draw(void) {}
"#;

/// Packed C cart: the text accessor reports the NUL-stripped content length,
/// identically across native / WASM / libretro (#166).
#[test]
fn c_text_get_content_length_all_legs() {
    require_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("text_len_c");
    CartProject::new()
        .c(TEXT_LEN_C)
        .asset("greeting.txt", "Hello from assets!")
        .write(&project);

    let cart = build_cart(&project);
    run_cart_all_legs(&cart, "TXTLEN=18:Hello from assets!");
}

/// C cart: blyt_resource_text_get on a *raw* resource returns NULL — the bytes
/// lack the build-appended trailing NUL, which is the C error path for feeding a
/// raw resource to the text accessor (ADR-0068/0088 amendments, #166). The blob
/// deliberately does not end in 0x00 (which would be the accepted residual hole).
const TEXT_GET_ON_RAW_C: &str = r#"
#include "blyt.h"
#include "cart_resources.h"
#include <stdlib.h>

void blyt_cart_init(void) {
    size_t len = 7;
    char *t = blyt_resource_text_get((blyt_text_resource_t)R_BLOB, &len);
    blyt_console_debug(t ? "TEXT_OK" : "TEXT_NULL");
    free(t);
}

void blyt_cart_update(void) { blyt_quit(); }
void blyt_cart_draw(void) {}
"#;

/// Packed C cart: text_get on a raw resource returns NULL, identically across
/// native / WASM / libretro (#166).
#[test]
fn c_text_get_on_raw_returns_null_all_legs() {
    require_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("text_on_raw_c");
    CartProject::new()
        .c(TEXT_GET_ON_RAW_C)
        .asset_bytes("blob.dat", b"rawdata")
        .write(&project);

    let cart = build_cart(&project);
    run_cart_all_legs(&cart, "TEXT_NULL");
}

/// A `text` resource that is not valid UTF-8 is a build error naming the file
/// (ADR-0088 amendment 2026-06-27, #166).
#[test]
fn text_asset_invalid_utf8_is_build_error() {
    require_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("bad_utf8");
    CartProject::new()
        .c(LOADER_C)
        .asset_bytes("greeting.txt", &[0xFF, 0xFE, 0xFD])
        .write(&project);

    let out = build_cart_expect_failure(&project);
    assert!(
        out.contains("UTF-8"),
        "expected a UTF-8 build error, got: {out}"
    );
    assert!(
        out.contains("greeting.txt"),
        "expected the offending file path, got: {out}"
    );
}

/// A `text` resource with an embedded NUL is a build error naming the file
/// (ADR-0088 amendment 2026-06-27, #166).
#[test]
fn text_asset_embedded_nul_is_build_error() {
    require_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("embedded_nul");
    CartProject::new()
        .c(LOADER_C)
        .asset_bytes("greeting.txt", b"hi\x00there")
        .write(&project);

    let out = build_cart_expect_failure(&project);
    assert!(
        out.contains("NUL"),
        "expected an embedded-NUL build error, got: {out}"
    );
    assert!(
        out.contains("greeting.txt"),
        "expected the offending file path, got: {out}"
    );
}

/// Rust compile barrier (#166): the text accessor exists only on the text types,
/// so calling `text_string()` on a `BytesResource` constant fails to compile —
/// the Rust analogue of ADR-0068's typed handles.
const RUST_TEXT_ON_BYTES: &str = r#"#![no_std]

include!(env!("BLYT_CART_RESOURCES_RS"));

#[no_mangle]
pub extern "C" fn blyt_cart_init() {
    // R_BLOB is a BytesResource, which has no text_string().
    let _ = R_BLOB.text_string();
}

#[no_mangle]
pub extern "C" fn blyt_cart_update() {}

#[no_mangle]
pub extern "C" fn blyt_cart_draw() {}
"#;

#[test]
fn rust_text_accessor_on_bytes_is_compile_error() {
    require_sdk();
    require_rust_riscv_target();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("rust_text_on_bytes");
    CartProject::new()
        .rust(RUST_TEXT_ON_BYTES)
        .asset_bytes("blob.dat", b"rawdata")
        .write(&project);

    let out = build_cart_expect_failure(&project);
    assert!(
        out.contains("text_string"),
        "expected a compile error about the missing text_string method, got: {out}"
    );
}

/* -------------------------------------------------------------------------
 * Per-resource zstd compression (#157, ADR-0026)
 *
 * The packer compresses a resource body (zstd) only when it saves >5%, writing
 * an 8-byte uncompressed header [algo|0|0|0|dsize_le] ahead of the body in the
 * `.cart.resource.<id>` section; the runtime decodes on first access into an
 * owned buffer (uncompressed bodies stay zero-copy). A compressed resource must
 * yield byte-identical bytes on every leg — the determinism contract.
 * ------------------------------------------------------------------------- */

/// A short phrase repeated — highly compressible, so it packs zstd. ~20 KiB,
/// well under the 16 MiB resource scratch the pin path copies through.
fn compressible_blob() -> Vec<u8> {
    "blyt-resource-compression-test-payload\n"
        .repeat(512)
        .into_bytes()
}

/// Deterministic high-entropy bytes (splitmix64) — uniformly random, so zstd
/// cannot beat the 5% threshold and the packer must ship `none`.
fn random_blob(n: usize) -> Vec<u8> {
    let mut state: u64 = 0x9E37_79B9_7F4A_7C15;
    (0..n)
        .map(|_| {
            state = state.wrapping_add(0x9E37_79B9_7F4A_7C15);
            let mut z = state;
            z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
            z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
            (z ^ (z >> 31)) as u8
        })
        .collect()
}

/// FNV-1a (32-bit) over `bytes`, matching the C cart's loop exactly — a
/// full-content fingerprint, so the cross-leg assertion proves every decoded
/// byte is correct (not merely the length: the #98 anti-pattern).
fn fnv1a(bytes: &[u8]) -> u32 {
    let mut h: u32 = 2166136261;
    for &b in bytes {
        h ^= b as u32;
        h = h.wrapping_mul(16777619);
    }
    h
}

/// Dump a `.cart.resource.<id>` section (header + body) from a packed cart via
/// the SDK's objcopy, leaving the cart untouched (writes a throwaway output).
fn dump_resource_section(cart: &std::path::Path, id: u32) -> Vec<u8> {
    let dir = cart.parent().unwrap();
    let sec_out = dir.join(format!("dump-section-{id}.bin"));
    let throwaway = dir.join(format!("dump-throwaway-{id}.elf"));
    let status = std::process::Command::new(sdk_dir().join("bin/blyt-objcopy"))
        .arg("--dump-section")
        .arg(format!(".cart.resource.{id}={}", sec_out.display()))
        .arg(cart)
        .arg(&throwaway)
        .status()
        .expect("run blyt-objcopy --dump-section");
    assert!(
        status.success(),
        "objcopy --dump-section failed for id {id}"
    );
    std::fs::read(&sec_out).expect("read dumped section")
}

/// C cart that pins R_BLOB and prints a deterministic line: the pin result, the
/// byte length, and an FNV-1a over every byte — so a single substring match
/// pins identical *content* across all three legs.
const C_BLOB_FNV: &str = r#"
#include "blyt.h"
#include "cart_resources.h"
#include <stdio.h>

void blyt_cart_init(void) {
    const void *ptr = NULL;
    size_t size = 0;
    blyt_result_t pr = blyt_resource_pin(R_BLOB, &ptr, &size);
    unsigned int h = 2166136261u;
    const unsigned char *b = (const unsigned char *)ptr;
    for (size_t i = 0; i < size; i++) {
        h ^= b[i];
        h *= 16777619u;
    }
    char line[96];
    snprintf(line, sizeof(line), "BLOB pin=%d size=%d fnv=%08x", (int)pr, (int)size, h);
    blyt_console_debug(line);
}
void blyt_cart_update(void) { blyt_quit(); }
void blyt_cart_draw(void) {}
"#;

/// A compressed resource decodes to the exact original bytes — asserted
/// **identical across native / WASM / libretro** (the determinism contract),
/// proven by an FNV over the full decoded content, not per-leg smoke.
#[test]
fn compressed_resource_round_trips_all_legs() {
    require_sdk();
    require_wasm();
    require_libretro_core();

    let blob = compressible_blob();
    let expected = format!("BLOB pin=0 size={} fnv={:08x}", blob.len(), fnv1a(&blob));

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("compressed_resource");
    CartProject::new()
        .c(C_BLOB_FNV)
        .asset_bytes("blob.dat", &blob)
        .write(&project);

    let cart = build_cart(&project);
    // Guard: the resource genuinely compressed (the test would be vacuous on the
    // `none` path), so this exercises the zstd decode on every leg.
    assert_eq!(
        dump_resource_section(&cart, 1)[0],
        1,
        "blob.dat should have packed zstd"
    );

    run_cart_all_legs(&cart, &expected);
}

/// C cart that pins R_BLOB and prints its FNV-1a every frame (init + each
/// update), quitting after a few frames. Under `--evict-every-frame` the owned
/// decompressed bytes are freed after every frame, so each subsequent read
/// rehydrates from the cart section — the line must stay byte-identical, proving
/// rehydration yields the original bytes and eviction is cart-invisible (#137).
const EVICT_REHYDRATE_FNV_C: &str = r#"
#include "blyt.h"
#include "cart_resources.h"
#include <stdio.h>

static int g_frames = 0;

static void read_and_print(void) {
    const void *ptr = NULL;
    size_t size = 0;
    blyt_result_t pr = blyt_resource_pin(R_BLOB, &ptr, &size);
    unsigned int h = 2166136261u;
    const unsigned char *b = (const unsigned char *)ptr;
    for (size_t i = 0; i < size; i++) {
        h ^= b[i];
        h *= 16777619u;
    }
    blyt_resource_unpin(R_BLOB);
    char line[96];
    snprintf(line, sizeof(line), "BLOB pin=%d size=%d fnv=%08x", (int)pr, (int)size, h);
    blyt_console_debug(line);
}

void blyt_cart_init(void) { read_and_print(); }
void blyt_cart_update(void) {
    read_and_print(); /* frames after the first re-read a rehydrated resource */
    if (++g_frames >= 3)
        blyt_quit();
}
void blyt_cart_draw(void) {}
"#;

/// AC #1/#4: load → release a compressed resource, force eviction every frame,
/// re-access → bytes are identical to the first access, across all three legs;
/// and the cart-visible output is identical whether or not eviction occurred.
#[test]
fn evicted_resource_rehydrates_byte_identical_all_legs() {
    require_sdk();
    require_wasm();
    require_libretro_core();

    let blob = compressible_blob();
    let expected = format!("BLOB pin=0 size={} fnv={:08x}", blob.len(), fnv1a(&blob));

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("evict_rehydrate");
    CartProject::new()
        .c(EVICT_REHYDRATE_FNV_C)
        .asset_bytes("blob.dat", &blob)
        .write(&project);

    let cart = build_cart(&project);
    // Guard: the resource genuinely compressed, so eviction has owned bytes to
    // reclaim and rehydration exercises the zstd re-decode on every leg.
    assert_eq!(
        dump_resource_section(&cart, 1)[0],
        1,
        "blob.dat should have packed zstd"
    );

    // Forced eviction after every frame: each re-read rehydrates from scratch.
    run_cart_all_legs_evict_every_frame(&cart, &expected);
    // No eviction: identical cart-visible output — eviction never changes it.
    run_cart_all_legs(&cart, &expected);
}

/// The packed section of a compressible resource is zstd-flagged, smaller than
/// the raw bytes, records the true decompressed size, and repacks
/// byte-identically (acceptance criteria: compress, dsize-in-index, reproducible).
#[test]
fn compressed_resource_section_is_zstd_with_dsize_and_reproducible() {
    require_sdk();

    let blob = compressible_blob();
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("zstd_section");
    CartProject::new()
        .c(C_BLOB_FNV)
        .asset_bytes("blob.dat", &blob)
        .write(&project);

    let cart = build_cart(&project);
    let section = dump_resource_section(&cart, 1);

    assert_eq!(section[0], 1, "algo byte should be zstd");
    assert!(
        section.len() < blob.len(),
        "on-disk section {} not smaller than raw {}",
        section.len(),
        blob.len()
    );
    let dsize = u32::from_le_bytes(section[4..8].try_into().unwrap()) as usize;
    assert_eq!(
        dsize,
        blob.len(),
        "header dsize must equal the decompressed length"
    );

    // Reproducibility: an independent repack of the same input yields
    // byte-identical section bytes.
    let project2 = tmp.path().join("zstd_section_repeat");
    CartProject::new()
        .c(C_BLOB_FNV)
        .asset_bytes("blob.dat", &blob)
        .write(&project2);
    let section2 = dump_resource_section(&build_cart(&project2), 1);
    assert_eq!(
        section, section2,
        "repacking must produce byte-identical section bytes"
    );
}

/// An incompressible resource (or one below the 5% threshold) packs `none`: the
/// algo flag is none and the body is byte-exact (no growth beyond the header).
#[test]
fn incompressible_resource_packs_none() {
    require_sdk();

    let raw = random_blob(4096);
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("none_section");
    CartProject::new()
        .c(C_BLOB_FNV)
        .asset_bytes("blob.dat", &raw)
        .write(&project);

    let cart = build_cart(&project);
    let section = dump_resource_section(&cart, 1);

    assert_eq!(section[0], 0, "incompressible data should pack none");
    assert_eq!(
        &section[8..],
        &raw[..],
        "none body must equal the raw bytes (no growth)"
    );
    let dsize = u32::from_le_bytes(section[4..8].try_into().unwrap()) as usize;
    assert_eq!(dsize, raw.len());
}
