mod common;

use assert_cmd::Command;
use common::{
    CartProject, blytplay, build_cart, find_wasm_dir, libretro_so, repo_root,
    require_libretro_core, require_sdk, require_wasm, test_libretro_core,
};
use tempfile::TempDir;

/* ── Test cart ───────────────────────────────────────────────────────────────
 *
 * Exercises the user-facing trace channels: console_debug + typed state
 * buffer ops (api), frame boundaries (frame), and a clean multi-frame run so
 * the frame counter advances.  Runs three frames then quits. */

const TRACE_CONFIG: &str = "\
records:
  Game:
    fields:
      - { name: score,  type: i32 }
      - { name: frames, type: i32 }
state_buffers:
  game:
    record: Game
    count: 1
";

/* The frame counter lives in a state buffer, not a static: the
 * --reset-every-frame cycle zeroes guest BSS each frame (a static counter
 * would never advance and the cart would never quit), while state buffer
 * contents are snapshot-restored. */
const TRACE_TEST_C: &str = r#"
#include "blyt.h"
#include "cart_state.h"

void blyt_cart_init(void) {
    int32_t slot = -1;
    blyt_buffer_alloc_slot(S_GAME, &slot);
    blyt_buffer_set_i32(S_GAME, slot, S_GAME_SCORE, 42);
    blyt_console_debug("trace test marker");
}

void blyt_cart_update(void) {
    (void)blyt_buffer_get_i32(S_GAME, 0, S_GAME_SCORE);
    int32_t f = blyt_buffer_get_i32(S_GAME, 0, S_GAME_FRAMES) + 1;
    blyt_buffer_set_i32(S_GAME, 0, S_GAME_FRAMES, f);
    if (f >= 3)
        blyt_quit();
}

void blyt_cart_draw(void) {}
"#;

fn build_trace_cart(tmp: &TempDir) -> std::path::PathBuf {
    let project = tmp.path().join("trace_cart");
    CartProject::new()
        .config(TRACE_CONFIG)
        .c(TRACE_TEST_C)
        .write(&project);
    let cart = build_cart(&project);
    assert!(cart.exists());
    cart
}

/// Run blytplay on `cart` with the given env/flags; return (stdout, stderr).
fn run_native(cart: &std::path::Path, env: &[(&str, &str)], flags: &[&str]) -> (String, String) {
    let mut cmd = Command::new(blytplay());
    cmd.arg("--headless");
    // Make the test hermetic: an ambient BLYT_TRACE must not leak in.
    cmd.env_remove("BLYT_TRACE");
    for f in flags {
        cmd.arg(f);
    }
    for (k, v) in env {
        cmd.env(k, v);
    }
    cmd.arg(cart.to_str().unwrap());
    let output = cmd.assert().success().get_output().clone();
    (
        String::from_utf8_lossy(&output.stdout).into_owned(),
        String::from_utf8_lossy(&output.stderr).into_owned(),
    )
}

fn assert_api_frame_trace(stderr: &str, context: &str) {
    for needle in [
        "[blyt:frame] f=0",                          // frame channel active
        "start",                                     // frame open
        "end",                                       // frame close
        "[blyt:frame] f=2",                          // counter advances across frames
        "console_debug(\"trace test marker\")",      // api: string dereferenced
        "buf_set_i32(buf=1, slot=0, field=1, v=42)", // api: typed set
        "buf_get_i32(buf=1, slot=0, field=1) -> 42", // api: typed get
        "buf_alloc_slot(buf=1) -> slot=0",           // api: slot management
        "frame_done()",                              // api: frame boundary ecall
    ] {
        assert!(
            stderr.contains(needle),
            "expected {needle:?} in {context} trace stderr, got:\n{stderr}"
        );
    }
}

/* ── Native leg ──────────────────────────────────────────────────────────── */

/// BLYT_TRACE=api,frame on blytplay emits typed api lines and frame
/// boundaries on stderr, with the frame counter advancing.
#[test]
fn trace_api_frame_channels_native() {
    require_sdk();
    let tmp = TempDir::new().unwrap();
    let cart = build_trace_cart(&tmp);

    let (stdout, stderr) = run_native(&cart, &[("BLYT_TRACE", "api,frame")], &[]);
    assert!(stdout.contains("trace test marker"));
    assert_api_frame_trace(&stderr, "native");
}

/// The --trace flag is equivalent to setting BLYT_TRACE (both '--trace=list'
/// and '--trace list' forms).
#[test]
fn trace_flag_form_native() {
    require_sdk();
    let tmp = TempDir::new().unwrap();
    let cart = build_trace_cart(&tmp);

    let (_, stderr) = run_native(&cart, &[], &["--trace=api,frame"]);
    assert_api_frame_trace(&stderr, "--trace=");

    let (_, stderr) = run_native(&cart, &[], &["--trace", "api,frame"]);
    assert_api_frame_trace(&stderr, "--trace ");
}

/// Without BLYT_TRACE, no trace lines appear at all.
#[test]
fn trace_off_by_default() {
    require_sdk();
    let tmp = TempDir::new().unwrap();
    let cart = build_trace_cart(&tmp);

    let (_, stderr) = run_native(&cart, &[], &[]);
    assert!(
        !stderr.contains("[blyt:"),
        "trace lines on stderr without BLYT_TRACE:\n{stderr}"
    );
}

/// Tracing must not perturb the cart: stdout is byte-identical with
/// BLYT_TRACE=all and without (trace goes to stderr only, and the emulated
/// timestep carries no wall-clock coupling).
#[test]
fn trace_stdout_identical() {
    require_sdk();
    let tmp = TempDir::new().unwrap();
    let cart = build_trace_cart(&tmp);

    let (stdout_plain, _) = run_native(&cart, &[], &[]);
    let (stdout_traced, _) = run_native(&cart, &[("BLYT_TRACE", "all")], &[]);
    assert_eq!(
        stdout_plain, stdout_traced,
        "stdout differs between traced and untraced runs"
    );
}

/// Channel selection is real: api-only must not emit frame lines and
/// vice versa.
#[test]
fn trace_channel_selection() {
    require_sdk();
    let tmp = TempDir::new().unwrap();
    let cart = build_trace_cart(&tmp);

    let (_, stderr) = run_native(&cart, &[("BLYT_TRACE", "api")], &[]);
    assert!(stderr.contains("[blyt:api]"));
    assert!(
        !stderr.contains("[blyt:frame]"),
        "frame lines emitted with api-only trace:\n{stderr}"
    );

    let (_, stderr) = run_native(&cart, &[("BLYT_TRACE", "frame")], &[]);
    assert!(stderr.contains("[blyt:frame]"));
    assert!(
        !stderr.contains("[blyt:api]"),
        "api lines emitted with frame-only trace:\n{stderr}"
    );
}

/// The lifecycle channel names host-initiated guest calls: the
/// --reset-every-frame cycle drives on_save_state/init/on_load_state through
/// blyt_session_begin_fn_call and must produce matching call/ret pairs.
#[test]
fn trace_lifecycle_reset_every_frame() {
    require_sdk();
    let tmp = TempDir::new().unwrap();
    let cart = build_trace_cart(&tmp);

    let (_, stderr) = run_native(
        &cart,
        &[("BLYT_TRACE", "lifecycle")],
        &["--reset-every-frame"],
    );
    for needle in [
        "[blyt:lifecycle]",
        "call on_save_state",
        "ret on_save_state",
        "call init",
        "ret init",
    ] {
        assert!(
            stderr.contains(needle),
            "expected {needle:?} in lifecycle trace, got:\n{stderr}"
        );
    }
}

/* ── WASM leg ────────────────────────────────────────────────────────────── */

/// BLYT_TRACE reaches the WASM runtime via __blyt_env_vars (run_cart.js env
/// JSON → module_pre.js → Emscripten ENV) and produces the same typed api +
/// frame lines on stderr as the native leg.
#[test]
fn trace_api_frame_channels_wasm() {
    require_sdk();
    require_wasm();
    let tmp = TempDir::new().unwrap();
    let cart = build_trace_cart(&tmp);

    let driver = repo_root().join("tests/wasm/run_cart.js");
    let wasm_dir = find_wasm_dir();
    let output = Command::new("node")
        .args([
            driver.to_str().unwrap(),
            wasm_dir.to_str().unwrap(),
            cart.to_str().unwrap(),
            "", // frame0 output path (unused)
            "{\"BLYT_TRACE\":\"api,frame\"}",
        ])
        .env_remove("BLYT_TRACE")
        .assert()
        .success()
        .get_output()
        .clone();
    let stdout = String::from_utf8_lossy(&output.stdout).into_owned();
    let stderr = String::from_utf8_lossy(&output.stderr).into_owned();
    assert!(stdout.contains("trace test marker"));
    assert_api_frame_trace(&stderr, "wasm");
}

/* ── libretro leg ────────────────────────────────────────────────────────── */

/// The libretro core (with its embedded guest libs) honours BLYT_TRACE from
/// the process environment and emits the same typed api + frame lines.
#[test]
fn trace_api_frame_channels_libretro() {
    require_sdk();
    require_libretro_core();
    let tmp = TempDir::new().unwrap();
    let cart = build_trace_cart(&tmp);

    let output = Command::new(test_libretro_core())
        .args([libretro_so().to_str().unwrap(), cart.to_str().unwrap()])
        .env_remove("BLYT_TRACE")
        .env("BLYT_TRACE", "api,frame")
        .assert()
        .success()
        .get_output()
        .clone();
    let stderr = String::from_utf8_lossy(&output.stderr).into_owned();
    assert_api_frame_trace(&stderr, "libretro");
}
