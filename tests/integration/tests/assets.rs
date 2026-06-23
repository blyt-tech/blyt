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
    CartProject, blytplay, build_cart, build_dev_elf, require_libretro_core, require_sdk,
    require_wasm, run_cart_all_legs,
};
use std::io::{Read, Write};
use std::net::TcpStream;
use std::time::Duration;
use tempfile::TempDir;

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
