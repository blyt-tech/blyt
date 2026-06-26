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
    CartProject, blyt_bin, blytplay, build_cart, build_dev_elf, build_lua_cart, find_wasm_dir,
    libretro_so, repo_root, require_libretro_core, require_lua_sdk, require_playwright,
    require_rust_riscv_target, require_sdk, require_wasm, run_cart_all_legs, sdk_dir,
    test_libretro_core,
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

/// C cart exercising the full resource lifecycle surface (#123): load->handle,
/// pin->bytes->unpin, then a valid release followed by a stale release. It emits
/// one deterministic line encoding every observable result so a single substring
/// match pins identical behaviour across all three legs — proving pin delivers
/// the exact bytes+length, load returns OK with a non-zero handle, the first
/// release succeeds, and the stale second release is rejected (INVALID_ARG=1).
const LIFECYCLE_C: &str = r#"
#include "blyt.h"
#include "cart_resources.h"
#include <stdio.h>
#include <string.h>

void blyt_cart_init(void) {
    blyt_resource_h h = BLYT_RESOURCE_INVALID;
    blyt_result_t lr = blyt_resource_load(R_GREETING, &h);

    const void *ptr = NULL;
    size_t size = 0;
    blyt_result_t pr = blyt_resource_pin(R_GREETING, &ptr, &size);
    char g[64];
    size_t n = size < sizeof(g) - 1 ? size : sizeof(g) - 1;
    if (ptr) memcpy(g, ptr, n);
    g[ptr ? n : 0] = '\0';
    blyt_resource_unpin(R_GREETING);

    blyt_result_t r1 = blyt_resource_release(h);       /* valid */
    blyt_result_t r2 = blyt_resource_release(h);       /* stale -> rejected */

    char line[256];
    snprintf(line, sizeof(line),
             "R[%s] load=%d h=%d pin=%d bytes=%d rel1=%d rel2=%d",
             g, (int)lr, (int)(h != BLYT_RESOURCE_INVALID), (int)pr,
             (int)size, (int)r1, (int)r2);
    blyt_console_debug(line);
}

void blyt_cart_update(void) { blyt_quit(); }
void blyt_cart_draw(void) {}
"#;

/// Packed cart: the full lifecycle surface (load/pin/unpin/release + stale
/// rejection) produces identical observable output on native, WASM, libretro.
#[test]
fn resource_lifecycle_round_trips_all_legs() {
    require_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("resource_lifecycle");
    CartProject::new()
        .c(LIFECYCLE_C)
        .asset("greeting.txt", "Hello from assets!")
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    run_cart_all_legs(
        &cart,
        "R[Hello from assets!] load=0 h=1 pin=0 bytes=18 rel1=0 rel2=1",
    );
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
    local res = blyt.resource.load(R.GREETING)
    blyt.debug.print(string.format("R[%d]=%s", R.GREETING, res:text()))
    res:release()
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

    run_cart_all_legs(&cart, "R[1]=Hello from assets!");
}

/// Pure-Lua cart exercising the full handle surface: idempotent load (`==`
/// holds), module-level `pin`/`unpin` returning a frame-scoped pointer + size,
/// `:text()` owned copy, `__tostring`.  One deterministic line pins identical
/// behaviour across native / WASM / libretro.
const LUA_RESOURCE_LIFECYCLE: &str = r#"
function init()
    local a = blyt.resource.load(1)
    local b = blyt.resource.load(1)
    local ptr, size = blyt.resource.pin(1)
    blyt.resource.unpin(1)
    local txt = a:text()
    a:release()
    blyt.debug.print(string.format(
        "L[%s] size=%d eq=%s ptr=%s ts=%s",
        txt, size, tostring(a == b), tostring(ptr ~= nil), tostring(a)))
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
        "L[Hello from assets!] size=18 eq=true ptr=true ts=resource<1>",
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

    let sdk = sdk_dir();
    use std::process::{Command as StdCommand, Stdio};
    let mut child = StdCommand::new(blyt_bin())
        .args(["run", project.to_str().unwrap()])
        .env("BLYT_SDK_DIR", &sdk)
        .env("BLYT_OBJCOPY", sdk.join("bin/blyt-objcopy"))
        .env("BLYT_CLANG", sdk.join("bin/blyt-clang"))
        .env("BLYT_WASM_DIR", find_wasm_dir())
        .stdout(Stdio::piped())
        .spawn()
        .expect("spawn blyt run");

    // Read the server's stdout on a background thread so we can find its port.
    let mut stdout = child.stdout.take().unwrap();
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

    let url = wait_for_serving_url(&buf, Duration::from_secs(30));

    let driver = repo_root().join("tests/wasm/dev_asset_hotswap_test.mjs");
    let asset = project.join("assets/greeting.txt");
    let status = std::process::Command::new("node")
        .args([
            driver.to_str().unwrap(),
            &url,
            asset.to_str().unwrap(),
            "VERSION_ONE",
            "VERSION_TWO",
        ])
        .status()
        .expect("run dev_asset_hotswap_test.mjs");

    let _ = child.kill();
    let _ = reader.join();
    assert!(
        status.success(),
        "dev_asset_hotswap_test.mjs failed; blyt run output:\n{}",
        buf.lock().unwrap()
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

    let sdk = sdk_dir();
    use std::process::{Command as StdCommand, Stdio};
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

    let url = wait_for_serving_url(&buf, Duration::from_secs(30));
    let driver = repo_root().join("tests/wasm/dev_asset_hotswap_test.mjs");
    let asset = project.join("assets/greeting.txt");
    let status = std::process::Command::new("node")
        .args([
            driver.to_str().unwrap(),
            &url,
            asset.to_str().unwrap(),
            "VERSION_ONE",
            "VERSION_TWO",
        ])
        .status()
        .expect("run dev_asset_hotswap_test.mjs");

    let _ = child.kill();
    let _ = reader.join();
    assert!(
        status.success(),
        "cache-at-init cart did not observe swap on WASM; blyt run output:\n{}",
        buf.lock().unwrap()
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
local function cache_greeting()
    local res = blyt.resource.load(1)
    if res then
        cached = res:text()
        res:release()
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
