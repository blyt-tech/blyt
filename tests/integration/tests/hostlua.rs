//! Native host-Lua path scenario parity (#238, epic #230, ADR-0136).
//!
//! A pure-Lua cart runs on the native host-Lua VM by default on every non-RISC-V
//! host (ADR-0136; #236 retired the emulated RV32 Lua VM as a shipped path). These
//! tests pin the cart-visible output of the surface a pure-Lua cart reaches —
//! output/termination, state buffers + the `S` proxy + `save_write`/`save_read`,
//! entity refs, f64 fields (#253), and the reset-every-frame save/clear/restore
//! cycle — asserting the SAME `expected` on all three host-Lua legs (blytplay,
//! wasm, the libretro `.so`) via `run_cart_all_legs*`. Determinism across every
//! leg is the core contract (ADR-0007); RISC-V-Lua parity for these surfaces is
//! the QEMU native gate (`native_qemu.rs`).

mod common;

use common::{
    CartProject, build_lua_cart, require_libretro_core, require_lua_sdk, require_sdk, require_wasm,
    run_cart_all_legs, run_cart_all_legs_reset_every_frame, run_cart_all_legs_with_save_dir,
};
use tempfile::TempDir;

/// A minimal pure-Lua cart: print one line in `init()`, then quit. Every host-Lua
/// leg must emit the same line.
#[test]
fn hostlua_debug_print_parity() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    require_libretro_core();

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
    run_cart_all_legs(&cart, "hello from host-lua");
}

/// `blyt.quit()` called from `init()` terminates before any `update()`/`draw()`
/// runs — a marker printed in `init()` appears exactly once, and one printed in
/// `update()` must NOT appear. Asserts a line's *absence*, which the shared
/// `run_cart_*` helpers do not, so it drives blytplay directly.
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

    use assert_cmd::Command;
    let mut cmd = Command::new(common::blytplay());
    cmd.args(["--headless", "--quit-after", "5", cart.to_str().unwrap()]);
    let out = cmd.assert().success().get_output().stdout.clone();
    let s = String::from_utf8_lossy(&out);
    assert!(s.contains("init-ran"), "init did not run: {s}");
    assert!(
        !s.contains("update-ran"),
        "update ran despite quit() in init: {s}"
    );
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

/// Sibling of [`SB_CONFIG`] whose single field is an **f64** (tag 8), for the
/// #253 audit: the host-Lua state-buffer surface was only ever exercised with
/// i32 fields, which is exactly why the #235 `type_names[]` bug (f64 routed to
/// the i32 accessors) shipped unnoticed.
const SB_F64_CONFIG: &str = "\
records:
  Game:
    fields:
      - { name: health, type: f64 }
state_buffers:
  game:
    record: Game
    count: 1
";

/// S3: state buffers + `S` proxy + `blyt.buf.*` + `save_write`/`save_read`. A cart
/// allocates a slot, writes a field through the generated `S` proxy, persists via
/// `save_write`, overwrites the field, then `save_read` restores it — the printed
/// value must be the *saved* one, identically on every host-Lua leg. This
/// exercises the whole state-buffer surface (typed accessor, S proxy, standalone
/// `blyt_state_ctx`, save I/O).
#[test]
fn hostlua_state_buffer_save_round_trip_parity() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    require_libretro_core();

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
    run_cart_all_legs_with_save_dir(&cart, "score=42");
}

/// S3: entity-ref + multi-buffer surface without save I/O — mirrors `hello`'s
/// core (alloc across two buffers, store a packed ref, validate it, mutate a
/// field through the ref's slot). Same printed trajectory on every host-Lua leg.
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
    run_cart_all_legs(&cart, "x=15");
}

/// #253: the f64 counterpart of [`hostlua_state_buffer_save_round_trip_parity`].
/// The sibling only round-trips an i32 `score`, which is exactly the coverage
/// gap that let the #235 bug (f64 fields routed to the i32 accessors) ship. This
/// writes a **fractional** f64 (`42.5`) through the `S` proxy — a value that
/// under the pre-#235 i32 path would have *errored* on write (`checkinteger` on
/// a non-integer) rather than silently truncated — persists it via `save_write`,
/// overwrites it, then `save_read` restores it. The printed value must be the
/// saved `42.5` (exact, not i32-truncated to `42`), identically on every host-Lua
/// leg. Proves save/restore serialization carries the full 8-byte f64 field
/// (`field_sizeof(f64)=8`) end to end.
#[test]
fn hostlua_state_buffer_f64_save_round_trip_parity() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hostlua_sb_f64");

    CartProject::new()
        .config(SB_F64_CONFIG)
        .lua(
            r#"
local slot = -1

function init()
    slot = blyt.buf.alloc_slot(S.GAME)
    S.game[slot].health = 42.5
    blyt.save_write(0)
    S.game[slot].health = 99.5
end

function update()
    blyt.save_read(0)
    blyt.debug.print("health=" .. tostring(S.game[slot].health))
    blyt.quit()
end

function draw() end
"#,
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    run_cart_all_legs_with_save_dir(&cart, "health=42.5");
}

/// #253: carries a single f64 state-buffer field through the reset-every-frame
/// save/clear/restore snapshot cycle. The value lives entirely in the state
/// buffer (the only state that survives the host-Lua VM-rebuild), so each frame
/// does a read-modify-write of `S.game[0].health` by an exactly-representable
/// fraction (`+0.25`) and quits once it crosses a fractional threshold — the
/// final value is deterministic and identical across every leg AND invariant to
/// `--reset-every-frame` (the VM-rebuild reset must round-trip the 8-byte f64
/// field byte-identically). Guards the snapshot/restore path against the
/// never-exercised-f64 class; the fractional result would be impossible under the
/// pre-#235 i32 route.
#[test]
fn hostlua_state_buffer_f64_reset_every_frame_parity() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hostlua_sb_f64_ref");

    CartProject::new()
        .config(SB_F64_CONFIG)
        .lua(
            r#"
function init() end

function on_new_state()
    local slot = blyt.buf.alloc_slot(S.GAME)
    S.game[slot].health = 0.5
end

function update()
    S.game[0].health = S.game[0].health + 0.25
    if S.game[0].health >= 5.25 then
        blyt.debug.print("health=" .. tostring(S.game[0].health))
        blyt.quit()
    end
end

function draw() end
"#,
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    // 0.5 + 19 increments of 0.25 first reaches >= 5.25 at exactly 5.25; every
    // step is exactly representable in f64 so tostring is stable across legs, and
    // the fractional result proves the field was never coerced through i32.
    let expected = "health=5.25";

    run_cart_all_legs(&cart, expected);
    // The host-Lua VM-rebuild reset cycle must reach the SAME f64 value each frame.
    run_cart_all_legs_reset_every_frame(&cart, expected);
}

/// S4: the full `hello` acceptance surface — state buffers + entity ref + a plain
/// Lua `frame` counter serialised through `on_save_state`/`on_load_state`, run
/// across every host-Lua leg AND under `--reset-every-frame` (the save/clear/restore
/// determinism stress). Every leg must produce the same trajectory and be invariant
/// to reset-every-frame — proving the VM-rebuild reset cycle round-trips state
/// byte-identically.
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

    run_cart_all_legs(&cart, expected);
    // Reset-every-frame — the host-Lua VM-rebuild cycle must reach the SAME
    // trajectory on every leg.
    run_cart_all_legs_reset_every_frame(&cart, expected);
}
