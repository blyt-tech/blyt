mod common;

use assert_cmd::Command;
use common::{blytplay, build_cart, sdk_dir, test_session_api, write_c_cart_project};
use predicates::prelude::*;
use tempfile::TempDir;

/// SDK end-to-end: use the assembled SDK (build/sdk/) to build and run a cart.
///
/// The SDK binary auto-discovers its toolchain and libraries from its own
/// location in build/sdk/bin/ — no env vars required for blyt build or blytplay.
///
/// Requires `cmake --build build --target sdk` to have completed.
/// Silently skipped if the SDK has not been assembled.
#[test]
fn sdk_e2e_build_and_run() {
    let sdk = sdk_dir();
    let sdk_blyt = sdk.join("bin/blyt");
    let sdk_blytplay = sdk.join("bin/blytplay");

    assert!(
        sdk_blyt.exists() && sdk_blytplay.exists(),
        "SDK not assembled — run `cmake --build build --target sdk` first"
    );

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hello");

    write_c_cart_project(
        &project,
        r#"
#include "blyt.h"

static int s_frame = 0;

void blyt_cart_init(void)   { blyt_console_debug("hello from sdk"); }
void blyt_cart_update(void) { if (++s_frame >= 1) blyt_quit(); }
void blyt_cart_draw(void)   {}
"#,
    );

    // Build the cart using the assembled SDK binary.  Remove inherited override
    // env vars so that the SDK blyt fully auto-discovers its toolchain
    // (blyt-clang) and runtime libraries from its own location in
    // build/sdk/bin/.
    Command::new(&sdk_blyt)
        .args(["build", project.to_str().unwrap()])
        .env_remove("BLYT_SDK_DIR")
        .env_remove("BLYT_CLANG")
        .env_remove("BLYT_OBJCOPY")
        .assert()
        .success();

    let cart = project.join("build").join(format!(
        "{}.blyt",
        project.file_name().unwrap().to_str().unwrap()
    ));
    assert!(cart.exists(), "cart not found at {}", cart.display());

    // Run the cart with the SDK's blytplay.
    let out = Command::new(&sdk_blytplay)
        .args(["--headless", cart.to_str().unwrap()])
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

    write_c_cart_project(
        &project,
        r#"
#include "blyt.h"

static int s_frame = 0;

void blyt_cart_init(void)   { blyt_console_debug("init"); }
void blyt_cart_update(void) {
    blyt_console_debug("update");
    if (++s_frame >= 2) blyt_quit();
}
void blyt_cart_draw(void)   { blyt_console_debug("draw"); }
"#,
    );

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let output = Command::new(blytplay())
        .args(["--headless", cart.to_str().unwrap()])
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

/// A cart that calls abort() must exit with a non-zero status and the
/// frontend must report the abort (not confuse it with a clean exit).
#[test]
fn cart_abort_surfaces_as_error() {
    assert!(
        sdk_dir().join("bin/blyt-clang").exists(),
        "SDK not assembled — run `cmake --build build --target sdk` first"
    );

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("abort_cart");

    write_c_cart_project(
        &project,
        r#"
#include "blyt.h"
#include <stdlib.h>

void blyt_cart_init(void)   { abort(); }
void blyt_cart_update(void) { blyt_quit(); }
void blyt_cart_draw(void)   {}
"#,
    );

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    Command::new(blytplay())
        .args(["--headless", cart.to_str().unwrap()])
        .assert()
        .failure() // non-zero exit — abort is not a clean exit
        .stderr(predicate::str::contains("aborted"));
}

/// blytplay --headless rejects a non-existent cart path with a non-zero exit.
#[test]
fn run_missing_cart_fails() {
    Command::new(blytplay())
        .args(["--headless", "/nonexistent/path/cart.blyt"])
        .assert()
        .failure();
}

/// Session API: drives a cart via blyt_session_create/run_frame/destroy directly
/// (not via the blyt_cart_run wrapper).  Verifies BLYT_RUN_FRAME_DONE is returned
/// at least once before the cart exits cleanly.
#[test]
fn session_api_run_frame() {
    assert!(
        sdk_dir().join("bin/blyt-clang").exists(),
        "SDK not assembled — run `cmake --build build --target sdk` first"
    );
    assert!(
        test_session_api().exists(),
        "test_session_api not built — run `cmake --build build` first"
    );

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("session_cart");
    write_c_cart_project(
        &project,
        r#"
#include "blyt.h"
static int s_frame = 0;
void blyt_cart_init(void)   { blyt_console_debug("session-init"); }
void blyt_cart_update(void) { if (++s_frame >= 2) blyt_quit(); }
void blyt_cart_draw(void)   {}
"#,
    );

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let out = Command::new(test_session_api())
        .args([
            "session",
            cart.to_str().unwrap(),
            sdk_dir().join("lib").to_str().unwrap(),
        ])
        .assert()
        .success()
        .get_output()
        .stdout
        .clone();

    assert!(
        String::from_utf8_lossy(&out).contains("session-init"),
        "expected 'session-init' in output, got: {}",
        String::from_utf8_lossy(&out)
    );
}

/// Registry: loads the guest .so files into memory, registers them with
/// blyt_register_lib, then runs a cart without BLYT_LIB_DIR — exercising the
/// in-memory registry path through dynlink.
#[test]
fn registry_replaces_blyt_lib_dir() {
    assert!(
        sdk_dir().join("lib/libblyt32.so").exists(),
        "SDK not assembled — run `cmake --build build --target sdk` first"
    );
    assert!(
        sdk_dir().join("bin/blyt-clang").exists(),
        "SDK not assembled — run `cmake --build build --target sdk` first"
    );
    assert!(
        test_session_api().exists(),
        "test_session_api not built — run `cmake --build build` first"
    );

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("registry_cart");
    write_c_cart_project(
        &project,
        r#"
#include "blyt.h"
void blyt_cart_init(void)   { blyt_console_debug("registry-ok"); }
void blyt_cart_update(void) { blyt_quit(); }
void blyt_cart_draw(void)   {}
"#,
    );

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let out = Command::new(test_session_api())
        .args([
            "registry",
            cart.to_str().unwrap(),
            sdk_dir().join("lib").to_str().unwrap(),
        ])
        // Explicitly clear BLYT_LIB_DIR to ensure the registry is the only source
        .env_remove("BLYT_LIB_DIR")
        .assert()
        .success()
        .get_output()
        .stdout
        .clone();

    assert!(
        String::from_utf8_lossy(&out).contains("registry-ok"),
        "expected 'registry-ok' in output, got: {}",
        String::from_utf8_lossy(&out)
    );
}

/// In-VM cart-as-library module swap (issue #127, spike-W β / gate G1): create a
/// session on v1, run a few frames, then swap the cart code to v2 IN PLACE via
/// blyt_session_swap_cart — without recreating the VM/session.  Proves the core
/// contract a fresh-load reload cannot: the VM instance persists across the swap
/// (blyt_session_vm_id unchanged) and the v2 code boots and runs.  State
/// preservation across the swap is covered end-to-end by the player/libretro leg
/// in dev_control.rs::native_dev_control_lifecycle_commands.
#[test]
fn session_api_cart_swap() {
    assert!(
        sdk_dir().join("bin/blyt-clang").exists(),
        "SDK not assembled — run `cmake --build build --target sdk` first"
    );
    assert!(
        test_session_api().exists(),
        "test_session_api not built — run `cmake --build build` first"
    );

    let tmp = TempDir::new().unwrap();

    // v1 and v2 print distinct init markers and never quit, so the harness can
    // run a bounded number of frames and confirm v2's code went live after the
    // swap (its marker appears) while v1's was active before it.
    let project_v1 = tmp.path().join("swap_v1");
    write_c_cart_project(
        &project_v1,
        r#"
#include "blyt.h"
void blyt_cart_init(void)   { blyt_console_debug("swap-v1-init"); }
void blyt_cart_update(void) {}
void blyt_cart_draw(void)   {}
"#,
    );
    let v1 = build_cart(&project_v1);
    assert!(v1.exists(), "v1 cart not found at {}", v1.display());

    let project_v2 = tmp.path().join("swap_v2");
    write_c_cart_project(
        &project_v2,
        r#"
#include "blyt.h"
void blyt_cart_init(void)   { blyt_console_debug("swap-v2-init"); }
void blyt_cart_update(void) {}
void blyt_cart_draw(void)   {}
"#,
    );
    let v2 = build_cart(&project_v2);
    assert!(v2.exists(), "v2 cart not found at {}", v2.display());

    let out = Command::new(test_session_api())
        .args([
            "swap",
            v1.to_str().unwrap(),
            v2.to_str().unwrap(),
            sdk_dir().join("lib").to_str().unwrap(),
        ])
        .assert()
        .success()
        .get_output()
        .stdout
        .clone();
    let out = String::from_utf8_lossy(&out);

    assert!(
        out.contains("swap-v1-init"),
        "v1 init never ran before the swap; output: {out}"
    );
    assert!(
        out.contains("swap-v2-init"),
        "v2 code did not boot after the in-place swap (stale block cache or \
         unresolved entry point?); output: {out}"
    );
    assert!(
        out.contains("swap-ok"),
        "swap did not complete with the VM persisted; output: {out}"
    );

    // Issue #119: a debug reload re-maps the cart at a FRESH guest base each
    // time.  Re-run the same swap at a non-zero base (BLYT_SWAP_BASE) — this
    // exercises the runtime re-resolving the runtime libs' references back into
    // the cart (libblytcommon's blyt_main → blyt_cart_init/update/draw), which a
    // same-base swap leaves coincidentally valid.  Without that re-resolution the
    // re-booted cart jumps into freed memory at the old base.
    let out2 = Command::new(test_session_api())
        .args([
            "swap",
            v1.to_str().unwrap(),
            v2.to_str().unwrap(),
            sdk_dir().join("lib").to_str().unwrap(),
        ])
        .env("BLYT_SWAP_BASE", "0x05000000")
        .assert()
        .success()
        .get_output()
        .stdout
        .clone();
    let out2 = String::from_utf8_lossy(&out2);
    assert!(
        out2.contains("swap-v2-init") && out2.contains("swap-ok"),
        "fresh-base swap (BLYT_SWAP_BASE) did not boot v2 cleanly — runtime-lib \
         references into the cart were not re-resolved; output: {out2}"
    );
}
