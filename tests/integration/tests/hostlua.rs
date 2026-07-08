//! Native host-Lua fast path parity (#238, epic #230, ADR-0136).
//!
//! A pure-Lua cart routed through blytplay's native host-Lua VM (opt-in
//! `--host-lua` / `BLYT_HOSTLUA=1`) must produce the SAME cart-visible output as
//! the emulated RV32 Lua VM and the WASM host-Lua fast path — determinism across
//! every leg is the core contract (ADR-0007). These tests assert the same
//! `expected` line on:
//!   * the emulated leg   (`run_cart_native` — blytplay, rv32emu),
//!   * the host-Lua leg   (`run_cart_native_with_env` + `BLYT_HOSTLUA=1`),
//!   * the WASM leg        (`run_cart_wasm` — host-Lua fast path on wasm32),
//! so a divergence between the native host-Lua VM and the reference fails here by
//! construction rather than relying on per-leg smoke.
//!
//! S2 scope: the minimal surface a pure-Lua cart reaches for output and
//! termination — `blyt.debug.print` in `init()` and `blyt.quit()`. State buffers
//! (S3), full `hello` save/restore (S4) and the libretro `.so` under opt-in (S5)
//! extend this file as those slices land.

mod common;

use common::{
    CartProject, build_lua_cart, require_libretro_core, require_lua_sdk, require_sdk, require_wasm,
    run_cart_libretro, run_cart_libretro_with_env, run_cart_libretro_with_flags, run_cart_native,
    run_cart_native_with_env, run_cart_wasm, run_cart_wasm_with_env,
};
use tempfile::TempDir;

/// A minimal pure-Lua cart: print one line in `init()`, then quit. The native
/// host-Lua VM must emit the same line as the emulated and WASM legs.
#[test]
fn hostlua_debug_print_parity() {
    require_sdk();
    require_lua_sdk();
    require_wasm();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hostlua_hello");

    CartProject::new()
        .lua(
            r#"
function init()
    blyt.debug.print("hello from host-lua")
end

function update()
    blyt.quit()
end

function draw() end
"#,
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    let expected = "hello from host-lua";

    // Emulated (rv32emu) — the reference.
    run_cart_native(&cart, expected);
    // Native host-Lua fast path (#238) — the new leg under test.
    run_cart_native_with_env(&cart, &[("BLYT_HOSTLUA", "1")], expected);
    // WASM host-Lua fast path — the existing native-VM leg.
    run_cart_wasm(&cart, expected);
}

/// `blyt.quit()` called from `init()` terminates before any `update()`/`draw()`
/// runs — a marker printed in `init()` appears exactly once, and one printed in
/// `update()` must NOT appear, on the host-Lua leg just as on the emulated leg.
#[test]
fn hostlua_quit_in_init_skips_update() {
    require_sdk();
    require_lua_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hostlua_quit_init");

    CartProject::new()
        .lua(
            r#"
function init()
    blyt.debug.print("init-ran")
    blyt.quit()
end

function update()
    blyt.debug.print("update-ran")
end

function draw() end
"#,
        )
        .write(&project);

    let cart = build_lua_cart(&project);

    for env in [&[][..], &[("BLYT_HOSTLUA", "1")][..]] {
        use assert_cmd::Command;
        let mut cmd = Command::new(common::blytplay());
        cmd.args(["--headless", "--quit-after", "5", cart.to_str().unwrap()]);
        for (k, v) in env {
            cmd.env(k, v);
        }
        let out = cmd.assert().success().get_output().stdout.clone();
        let s = String::from_utf8_lossy(&out);
        assert!(
            s.contains("init-ran"),
            "init did not run (env={env:?}): {s}"
        );
        assert!(
            !s.contains("update-ran"),
            "update ran despite quit() in init (env={env:?}): {s}"
        );
    }
}

/// State-buffer schema for the S3 round-trip cart.
const SB_CONFIG: &str = "\
records:
  Game:
    fields:
      - { name: score, type: i32 }
state_buffers:
  game:
    record: Game
    count: 1
";

/// S3: state buffers + `S` proxy + `blyt.buf.*` + `save_write`/`save_read`. A cart
/// allocates a slot, writes a field through the generated `S` proxy, persists via
/// `save_write`, overwrites the field, then `save_read` restores it — the printed
/// value must be the *saved* one, identically on the emulated, native host-Lua,
/// and WASM legs. This exercises the whole state-buffer surface the host-Lua path
/// ported in S3 (typed accessor, S proxy, standalone `blyt_state_ctx`, save I/O).
#[test]
fn hostlua_state_buffer_save_round_trip_parity() {
    require_sdk();
    require_lua_sdk();
    require_wasm();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hostlua_sb");

    CartProject::new()
        .config(SB_CONFIG)
        .lua(
            r#"
local slot = -1

function init()
    slot = blyt.buf.alloc_slot(S.GAME)
    S.game[slot].score = 42
    blyt.save_write(0)
    S.game[slot].score = 99
end

function update()
    blyt.save_read(0)
    blyt.debug.print("score=" .. tostring(S.game[slot].score))
    blyt.quit()
end

function draw() end
"#,
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    let expected = "score=42";

    // native + host-Lua share a real host save dir; WASM uses its MEMFS /tmp.
    let save_dir = TempDir::new().unwrap();
    let sd = save_dir.path().to_str().unwrap();

    run_cart_native_with_env(&cart, &[("BLYT_SAVE_DIR", sd)], expected);
    run_cart_native_with_env(
        &cart,
        &[("BLYT_SAVE_DIR", sd), ("BLYT_HOSTLUA", "1")],
        expected,
    );
    run_cart_wasm_with_env(&cart, &[("BLYT_SAVE_DIR", "/tmp")], expected);
}

/// S3: entity-ref + multi-buffer surface without save I/O — mirrors `hello`'s
/// core (alloc across two buffers, store a packed ref, validate it, mutate a
/// field through the ref's slot). Same printed trajectory on all three legs plus
/// the native host-Lua leg.
#[test]
fn hostlua_entity_ref_parity() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hostlua_ref");

    CartProject::new()
        .config(
            "\
records:
  Globals:
    fields:
      - { name: player, ref: character }
  Character:
    fields:
      - { name: x, type: i32 }
state_buffers:
  globals:
    record: Globals
    count: 1
  character:
    record: Character
    count: 4
",
        )
        .lua(
            r#"
function init()
    blyt.buf.alloc_slot(S.GLOBALS)
    local slot = blyt.buf.alloc_slot(S.CHARACTER)
    S.globals[0].player = blyt.buf.ref(S.CHARACTER, slot)
    S.character[slot].x = 10
end

function update()
    local player = S.globals[0].player
    if blyt.buf.ref_valid(S.CHARACTER, player) then
        local slot = blyt.buf.ref_slot(player)
        S.character[slot].x = S.character[slot].x + 5
        blyt.debug.print("x=" .. S.character[slot].x)
    end
    blyt.quit()
end

function draw() end
"#,
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    let expected = "x=15";

    run_cart_native(&cart, expected);
    run_cart_native_with_env(&cart, &[("BLYT_HOSTLUA", "1")], expected);
    run_cart_wasm(&cart, expected);
    // S5: the release libretro .so links the same libblyt.a, so it honors the
    // opt-in dispatch too ("let it flip", #233 owns the .so parity wiring).
    run_cart_libretro(&cart, expected);
    run_cart_libretro_with_env(&cart, &[("BLYT_HOSTLUA", "1")], expected);
}

/// Run blytplay `--headless {flags} {cart}` with extra env; assert `expected` in
/// stdout. Combines the flag + env knobs so the host-Lua leg can be driven under
/// `--reset-every-frame` (BLYT_HOSTLUA=1 + the flag together).
fn run_native_env_flags(
    cart: &std::path::Path,
    env: &[(&str, &str)],
    flags: &[&str],
    expected: &str,
) {
    use assert_cmd::Command;
    let mut cmd = Command::new(common::blytplay());
    cmd.arg("--headless");
    for f in flags {
        cmd.arg(f);
    }
    cmd.arg(cart.to_str().unwrap());
    for (k, v) in env {
        cmd.env(k, v);
    }
    let out = cmd.assert().success().get_output().stdout.clone();
    let s = String::from_utf8_lossy(&out);
    assert!(
        s.contains(expected),
        "expected {expected:?} (env={env:?} flags={flags:?}): {s}"
    );
}

/// Run the embedded libretro core `{flags} {so} {cart}` with extra env; assert
/// `expected` in its stderr (the core's log stream). Combines the flag + env
/// knobs so the `.so` host-Lua leg can be driven under `--reset-every-frame`.
fn run_libretro_env_flags(
    cart: &std::path::Path,
    env: &[(&str, &str)],
    flags: &[&str],
    expected: &str,
) {
    use assert_cmd::Command;
    let mut cmd = Command::new(common::test_libretro_core());
    for f in flags {
        cmd.arg(f);
    }
    cmd.args([
        common::libretro_so().to_str().unwrap(),
        cart.to_str().unwrap(),
    ]);
    for (k, v) in env {
        cmd.env(k, v);
    }
    let out = cmd.assert().success().get_output().stderr.clone();
    let s = String::from_utf8_lossy(&out);
    assert!(
        s.contains(expected),
        "expected {expected:?} (env={env:?} flags={flags:?}): {s}"
    );
}

/// S4: the full `hello` acceptance surface — state buffers + entity ref + a plain
/// Lua `frame` counter serialised through `on_save_state`/`on_load_state`, run
/// across every leg AND under `--reset-every-frame` (the save/clear/restore
/// determinism stress). The host-Lua leg must produce the same trajectory as the
/// emulated and WASM legs, and be invariant to reset-every-frame — proving the
/// VM-rebuild reset cycle round-trips state byte-identically (the emulated path
/// reaches the same state by zeroing guest BSS).
#[test]
fn hostlua_hello_all_legs_reset_every_frame_parity() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hostlua_hello_full");

    // hello's schema: a Globals record with a `frame` i32 + a `player` ref, and a
    // Character record. `frame` is deliberately a plain Lua local serialised via
    // the save hooks — the exact reset-every-frame determinism case.
    CartProject::new()
        .config(
            "\
records:
  Globals:
    fields:
      - { name: frame, type: i32 }
      - { name: player, ref: character }
  Character:
    fields:
      - { name: x, type: i32 }
      - { name: y, type: i32 }
state_buffers:
  globals:
    record: Globals
    count: 1
  character:
    record: Character
    count: 1
",
        )
        .lua(
            r#"
local frame

function init()
    frame = 0
end

function on_new_state()
    blyt.buf.alloc_slot(S.GLOBALS)
    local slot = blyt.buf.alloc_slot(S.CHARACTER)
    S.globals[0].player = blyt.buf.ref(S.CHARACTER, slot)
    S.character[slot].x = 160
    S.character[slot].y = 120
    blyt.debug.print("init player pos: 160, 120")
end

function update()
    frame = frame + 1
    if frame % 10 == 0 then
        local player = S.globals[0].player
        if blyt.buf.ref_valid(S.CHARACTER, player) then
            local slot = blyt.buf.ref_slot(player)
            local x = (S.character[slot].x + 1) % 320
            local y = (S.character[slot].y + 1) % 240
            S.character[slot].x = x
            S.character[slot].y = y
            blyt.debug.print("update frame " .. frame .. " player pos: " .. x .. ", " .. y)
        end
    end
    if frame >= 20 then
        blyt.quit()
    end
end

function draw() end

function on_save_state()
    S.globals[0].frame = frame
end

function on_load_state(_info)
    frame = S.globals[0].frame
end
"#,
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    let expected = "update frame 20 player pos: 162, 122";

    // Plain run — every leg agrees (incl. the release libretro .so, S5).
    run_cart_native(&cart, expected);
    run_cart_native_with_env(&cart, &[("BLYT_HOSTLUA", "1")], expected);
    run_cart_wasm(&cart, expected);
    run_cart_libretro(&cart, expected);
    run_cart_libretro_with_env(&cart, &[("BLYT_HOSTLUA", "1")], expected);

    // Reset-every-frame — the host-Lua VM-rebuild cycle must reach the SAME
    // trajectory as the emulated (BSS-zero) and WASM (VM-rebuild) legs, on both
    // blytplay and the libretro .so.
    run_native_env_flags(&cart, &[], &["--reset-every-frame"], expected);
    run_native_env_flags(
        &cart,
        &[("BLYT_HOSTLUA", "1")],
        &["--reset-every-frame"],
        expected,
    );
    run_cart_wasm_with_env(&cart, &[("BLYT_RESET_EVERY_FRAME", "1")], expected);
    run_cart_libretro_with_flags(&cart, &["--reset-every-frame"], expected);
    run_libretro_env_flags(
        &cart,
        &[("BLYT_HOSTLUA", "1")],
        &["--reset-every-frame"],
        expected,
    );
}
