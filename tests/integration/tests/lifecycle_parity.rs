//! Cart lifecycle parity across execution legs (#283).
//!
//! ADR-0087 fixes the cart lifecycle as
//! `init → on_new_state → [update → draw] loop → on_quit → cleanup`, and blyt's
//! core contract is that every leg reproduces it bit-identically. The number of
//! times each callback runs is cart-observable, so a leg that runs one extra
//! frame — or silently skips a callback — is a determinism break.
//!
//! These tests assert the EXACT marker sequence (count and order) via
//! `run_cart_all_legs_exact*`, not a substring. The `.contains()` helpers are
//! what let both bugs below live in the tree unnoticed (#284).

mod common;

use common::{
    CartProject, build_cart, build_lua_cart, capture_cart_libretro_with_flags,
    capture_cart_native_with_flags, cart_markers, require_lua_sdk, require_sdk, require_wasm,
    run_cart_all_legs_exact, run_cart_all_legs_exact_reset_every_frame,
};
use tempfile::TempDir;

/// The tag every marker these carts print is wrapped in (`<lc:...>`), so the
/// comparison sees only cart output — never the libretro banner or an ungated
/// guest trace line — and stays exact on every leg regardless of how each frames
/// the surrounding output. See `common::cart_markers`.
const M: &str = "lc";

/// The state-buffer schema the reset-every-frame carts share: a frame counter
/// that must survive each cycle's save/zero/restore round trip, so the cart keeps
/// advancing towards its own `quit()` instead of restarting from 0 forever.
const GLOBALS_CONFIG: &str = "\
records:
  Globals:
    fields:
      - { name: frame, type: i32 }
state_buffers:
  globals:
    record: Globals
    count: 1
";

/// #283 — the teeth for the frame-count divergence, on a **C** cart: no Lua
/// anywhere, so all three legs run the emulated rv32 path and nothing about
/// host-Lua can be blamed for a difference.
///
/// Under `--reset-every-frame` the driver runs one save/restore/`init()` cycle
/// after each completed frame. A cart that quits during frame 3's `update()`
/// therefore sees four `init()`s — the boot one plus one after each of the three
/// frames — and then `on_quit`/`cleanup` as the loop unwinds. It must NOT see a
/// fifth: once the cart has run `cleanup()` it is done, and re-entering `init()`
/// on a finished cart violates ADR-0087's ordering outright.
///
/// Before the fix, blytplay emitted exactly that fifth `init()` *after*
/// `CLEANUP`, while the libretro and WASM drivers did not — a 5-vs-4 divergence
/// in a cart-observable count. blytplay's loop gated its reset cycle only on its
/// own `g_quit` (SDL quit / `--quit-after`) and never on `blyt_libretro_is_done()`,
/// so it drove one more cycle into a session whose cart had already exited; the
/// libretro driver breaks on `is_done()` before the cycle and the WASM tick gates
/// on `BLYT_RUN_FRAME_DONE`, which is why only blytplay diverged.
#[test]
fn c_cart_reset_every_frame_lifecycle_exact_all_legs() {
    require_sdk();
    require_wasm();
    common::require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("lifecycle_c");

    CartProject::new()
        .config(GLOBALS_CONFIG)
        .c(r#"
#include "blyt.h"
#include "cart_state.h"
#include <stdio.h>

static int32_t s_frame = 0;

void blyt_cart_init(void) {
    int32_t slot = -1;
    blyt_buffer_alloc_slot(S_GLOBALS, &slot);
    blyt_console_debug("<lc:init>");
}

void blyt_cart_on_new_state(void) {}

void blyt_cart_on_save_state(void) {
    blyt_buffer_set_i32(S_GLOBALS, 0, S_GLOBALS_FRAME, s_frame);
}

void blyt_cart_on_load_state(blyt_load_info_t info) {
    (void)info;
    s_frame = blyt_buffer_get_i32(S_GLOBALS, 0, S_GLOBALS_FRAME);
}

void blyt_cart_update(void) {
    char buf[32];
    s_frame++;
    snprintf(buf, sizeof(buf), "<lc:update %d>", s_frame);
    blyt_console_debug(buf);
    if (s_frame >= 3)
        blyt_quit();
}

void blyt_cart_draw(void) {}

void blyt_cart_on_quit(void) {
    blyt_console_debug("<lc:on_quit>");
    blyt_quit();
}

void blyt_cart_cleanup(void) {
    blyt_console_debug("<lc:cleanup>");
}
"#)
        .write(&project);

    let cart = build_cart(&project);

    run_cart_all_legs_exact_reset_every_frame(
        &cart,
        M,
        &[
            "init", "update 1", "init", "update 2", "init", "update 3", "init", "on_quit",
            "cleanup",
        ],
    );
}

/// #283 (second bug) — a **Lua** cart's `on_quit()` and `cleanup()` must run on
/// every leg, exactly once, in ADR-0087 order.
///
/// ADR-0087 §"Lua entry point names" maps `blyt_cart_on_quit → on_quit` and
/// `blyt_cart_cleanup → cleanup`, and `blyt.h` documents both. But the emulated
/// Lua shim (`runtime/guest/src/libblyt32lua/blyt32lua.c`) defined strong
/// `blyt_cart_init/update/draw/on_new_state/on_save_state/on_load_state/
/// on_assets_reloaded` and simply omitted these two — as did the WASM bridge
/// variant, and neither name appeared in `blyt32lua.sym`. The weak libblytcommon
/// no-ops therefore won, and a Lua cart's `on_quit`/`cleanup` had never once run
/// on the emulated path (nor on bare metal, which links the same shim), while the
/// host-Lua driver chunk called them faithfully.
///
/// Latent since the Lua runtime was introduced, and invisible precisely because
/// every existing assertion was a substring match for something the cart printed
/// *earlier* in its life (#284, the anti-#98 class).
#[test]
fn lua_cart_lifecycle_on_quit_and_cleanup_run_on_every_leg() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    common::require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("lifecycle_lua");

    CartProject::new()
        .lua(
            r#"
local frame = 0

function init() blyt.debug.print("<lc:init>") end

function update()
    frame = frame + 1
    blyt.debug.print("<lc:update " .. frame .. ">")
    if frame >= 3 then blyt.quit() end
end

function draw() end

function on_quit() blyt.debug.print("<lc:on_quit>") end
function cleanup() blyt.debug.print("<lc:cleanup>") end
"#,
        )
        .write(&project);

    let cart = build_lua_cart(&project);

    run_cart_all_legs_exact(
        &cart,
        M,
        &[
            "init", "update 1", "update 2", "update 3", "on_quit", "cleanup",
        ],
    );
}

/// #283 as originally reported: the same **hybrid** cart, driven by the same
/// blytplay/libretro loop, must produce an identical marker sequence whether the
/// Lua half runs emulated (default for a hybrid) or on the host-Lua fast path
/// (`BLYT_HOSTLUA=1`). The reported symptom was 5 `init()`s emulated vs 4 on the
/// three host-Lua legs; this pins both halves of that comparison directly, since
/// the exec model is the only variable between the two runs.
#[test]
fn hybrid_reset_every_frame_lifecycle_identical_emulated_and_hostlua() {
    require_sdk();
    require_lua_sdk();
    common::require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("lifecycle_hybrid");

    CartProject::new()
        .config(GLOBALS_CONFIG)
        .c(r#"#include "blyt.h"
/* Present only to make this a hybrid — a hybrid keeps its Lua half on the
 * emulated rv32 path by default, which is the leg under comparison here. */
BLYT_LUA_EXPORT_I32(noop, int32_t x) {
    return x;
}
"#)
        .lua(
            r#"
local frame

function init() blyt.debug.print("<lc:init>") end

function on_new_state()
    blyt.buf.alloc_slot(S.GLOBALS)
    frame = 0
end

function update()
    frame = frame + 1
    blyt.debug.print("<lc:update " .. frame .. ">")
    if frame >= 3 then blyt.quit() end
end

function draw() end

function on_quit() blyt.debug.print("<lc:on_quit>") end
function cleanup() blyt.debug.print("<lc:cleanup>") end

function on_save_state() S.globals[0].frame = frame end
function on_load_state(_info) frame = S.globals[0].frame end
"#,
        )
        .write(&project);

    let cart = build_lua_cart(&project);

    let expected = vec![
        "init".to_string(),
        "update 1".to_string(),
        "init".to_string(),
        "update 2".to_string(),
        "init".to_string(),
        "update 3".to_string(),
        "init".to_string(),
        "on_quit".to_string(),
        "cleanup".to_string(),
    ];

    let flags = ["--reset-every-frame"];
    let hostlua = [("BLYT_HOSTLUA", "1")];

    for (leg, out) in [
        (
            "blytplay emulated",
            capture_cart_native_with_flags(&cart, &flags, &[]),
        ),
        (
            "blytplay host-Lua",
            capture_cart_native_with_flags(&cart, &flags, &hostlua),
        ),
        (
            "libretro emulated",
            capture_cart_libretro_with_flags(&cart, &flags, &[]),
        ),
        (
            "libretro host-Lua",
            capture_cart_libretro_with_flags(&cart, &flags, &hostlua),
        ),
    ] {
        assert_eq!(
            cart_markers(&out, M),
            expected,
            "\n{leg} leg diverged.\nfull output:\n{out}",
        );
    }
}
