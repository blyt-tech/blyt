//! Unified 16 MB memory-budget enforcement, end-to-end across the emulated legs
//! (blytplay / WASM / libretro) — issue #158, ADR-0008/0027.
//!
//! These pin the cart-visible contract: a guest `malloc` and a resource `load`
//! draw on ONE 16 MB budget, and the point at which an allocation fails is
//! identical across every leg (the determinism contract). The cheap host-side
//! mechanics (the arena accounting, the footprint/LRU policy) are unit-tested in
//! test_arena / test_mem_budget / test_resource_budget; this is the integration
//! oracle that the wiring actually enforces the budget through a real cart.
//!
//! The C carts exercise the rv32 heap on all three legs; the Lua cart additionally
//! pins the `guest_heap_used` byte-parity AC — its Lua VM runs as wasm32 on the
//! WASM host-Lua fast path but rv32 on blytplay/libretro, and the single shared
//! arena must make the count identical regardless (#158). The native bare-metal
//! leg is covered by the QEMU gate (native_qemu.rs Gate 16).

mod common;
use common::*;
use tempfile::TempDir;

/// A pure-Lua counterpart of `BUDGET_C`: it measures guest-heap headroom by
/// allocating 64 KiB long strings until Lua's allocator raises `LUA_ERRMEM`
/// (caught by `pcall`), twice — once with nothing resident, then again while
/// holding a loaded 4 MiB resource (which reserves 4 MiB of non-evictable
/// footprint up front). The second count must be strictly smaller, proving the
/// Lua VM heap and the resource cache share the SAME 16 MB budget. Crucially,
/// the Lua VM runs natively as wasm32 on the WASM host-Lua fast path but as rv32
/// on blytplay/libretro — so identical counts across all three legs is the
/// `guest_heap_used` byte-parity acceptance criterion of #158: the single
/// runtime/shared arena makes a Lua cart hit the cap at the same logical point
/// regardless of leg, the same way `BUDGET_C` proves it for a C cart.
const BUDGET_LUA: &str = r#"
local R = require("cart_resources")
local CHUNK = 64 * 1024

-- Allocate CHUNK-sized long strings until the budget is hit, free them, and
-- return how many fit. Deterministic: the single-sourced arena + the i32/f64
-- Lua object sizes make the count identical on wasm32 and rv32 (#158).
local function fill_heap()
    local t = {}
    local n = 0
    pcall(function()
        while true do
            t[#t + 1] = string.rep("\0", CHUNK)
            n = n + 1
        end
    end)
    t = nil
    collectgarbage("collect")
    return n
end

function init()
    local a = fill_heap() -- full 16 MB available to the heap
    -- Pinning BIG reserves 4 MiB of non-evictable footprint, held to the frame
    -- boundary (no unpin), so it survives the second measurement (ADR-0134: pin
    -- is the residency reservation now that load/release are gone).
    local ptr = blyt.resource.pin(R.BIG:id())
    local b = fill_heap() -- only ~12 MB now available to the heap
    blyt.debug.print(string.format("BUDGET a=%d b=%d loaded=%d shrank=%d", a, b,
        ptr and 1 or 0, (b > 0 and b < a) and 1 or 0))
end

function update() blyt.quit() end
function draw() end
"#;

#[test]
fn lua_heap_budget_shrinks_with_resident_resource_all_legs() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("mem_budget_lua");
    CartProject::new()
        .lua(BUDGET_LUA)
        .asset_bytes("big.bin", &[0u8; 4 * 1024 * 1024])
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    // a=255 (full 16 MB), b=191 (12 MB after the 4 MiB load), load succeeds, and
    // b<a (unified budget). The exact counts are asserted identically on every
    // leg — including the host-Lua wasm32 fast path — which is the cross-leg
    // guest_heap_used byte-parity acceptance criterion (#158). The counts happen
    // to equal BUDGET_C's because the arena block accounting is the same.
    run_cart_all_legs(&cart, "BUDGET a=255 b=191 loaded=1 shrank=1");
    // Native host-Lua fast path (#231): the VM allocates through the same shared
    // 16 MB arena, and the resource footprint feeds the same budget predicate —
    // a=255/b=191 here too (measurement: the 64 KiB body dominates, so the
    // 64-bit-vs-rv32 object-size delta never moves the fail-point). The exact
    // count for small-object exhaustion is host-heap-dependent (advisory).
    run_cart_native_with_env(
        &cart,
        &[("BLYT_HOSTLUA", "1")],
        "BUDGET a=255 b=191 loaded=1 shrank=1",
    );
}

/// A C cart that measures guest-heap headroom (64 KiB allocations until `malloc`
/// returns NULL) twice: once with nothing resident, then again after `pin`ning a
/// 4 MiB resource (which reserves 4 MiB of non-evictable footprint, held to the
/// frame boundary — ADR-0134). The second count must be strictly smaller —
/// proving heap and resource cache share the SAME budget, not two separate pools
/// — and both counts must be byte-identical across legs.
const BUDGET_C: &str = r#"
#include "blyt.h"
#include "cart_resources.h"
#include <stdio.h>
#include <stdlib.h>

#define CHUNK (64 * 1024)
#define MAXP 512

static void *g_ptrs[MAXP];

/* Allocate CHUNK-sized blocks until the budget is hit, then free them all and
 * return how many fit. Deterministic: the single-sourced arena makes the count
 * identical on every leg (#158). */
static int fill_heap(void) {
    int n = 0;
    void *p;
    while (n < MAXP && (p = malloc(CHUNK)) != NULL)
        g_ptrs[n++] = p;
    for (int i = 0; i < n; i++)
        free(g_ptrs[i]);
    return n;
}

void blyt_cart_init(void) {
    int a = fill_heap(); /* full 16 MB available to the heap */

    /* Pinning R_BIG reserves 4 MiB of non-evictable footprint (held to the frame
     * boundary — ADR-0134: pin is the residency reservation). */
    const void *ptr = NULL;
    size_t size = 0;
    blyt_result_t pr = blyt_resource_pin(R_BIG, &ptr, &size);

    int b = fill_heap(); /* only ~12 MB now available to the heap */

    char line[128];
    snprintf(line, sizeof(line), "BUDGET a=%d b=%d lr=%d shrank=%d", a, b, (int)pr,
             (int)(b > 0 && b < a));
    blyt_console_debug(line);
}

void blyt_cart_update(void) {
    blyt_quit();
}
void blyt_cart_draw(void) {}
"#;

#[test]
fn heap_budget_shrinks_with_resident_resource_all_legs() {
    require_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("mem_budget");
    CartProject::new()
        .c(BUDGET_C)
        .asset_bytes("big.bin", &[0u8; 4 * 1024 * 1024])
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    // a=255 (full 16 MB / 64 KiB block incl. header), b=191 (12 MB after the
    // 4 MiB load), load succeeds (lr=0), and b<a (unified budget). The exact
    // counts are asserted identically on every leg — the determinism contract.
    run_cart_all_legs(&cart, "BUDGET a=255 b=191 lr=0 shrank=1");
}

/// A C cart that `pin`s five distinct 4 MiB resources in turn, holding each pin.
/// Each pin reserves 4 MiB of non-evictable footprint up front, so the first four
/// (16 MiB) fit the budget exactly and the fifth must fail — at the SAME pin on
/// every leg. This is the "deterministic refusal at the cap across all legs"
/// acceptance criterion. The cart builds each resource constant from the loop
/// index (kind RESOURCE = bit 29, cart provenance — ADR-0134); a real cart names
/// R_<NAME>.
const LOAD_CAP_C: &str = r#"
#include "blyt.h"
#include <stdio.h>

#define R_ENCODE(id) (0x20000000u | (blyt_resource_id_t)(id)) /* kind RESOURCE, prov cart */

void blyt_cart_init(void) {
    int ok = 0, first_fail = -1;
    for (int id = 1; id <= 5; id++) {
        const void *ptr = NULL;
        size_t size = 0;
        blyt_result_t r = blyt_resource_pin(R_ENCODE(id), &ptr, &size); /* held: reserves footprint */
        if (r == BLYT_OK && ptr)
            ok++;
        else if (first_fail < 0)
            first_fail = id;
    }
    char line[96];
    snprintf(line, sizeof(line), "LOADCAP ok=%d first_fail=%d", ok, first_fail);
    blyt_console_debug(line);
}

void blyt_cart_update(void) {
    blyt_quit();
}
void blyt_cart_draw(void) {}
"#;

#[test]
fn resource_load_fails_at_budget_cap_all_legs() {
    require_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("mem_load_cap");
    let mut p = CartProject::new().c(LOAD_CAP_C);
    // Five 4 MiB resources, ids 1..=5 in sorted-name order.
    for k in 0..5 {
        p = p.asset_bytes(&format!("big{k}.bin"), &[0u8; 4 * 1024 * 1024]);
    }
    p.write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    // Four 4 MiB loads = 16 MiB fits exactly; the fifth crosses the cap and is
    // refused — identically on every leg.
    run_cart_all_legs(&cart, "LOADCAP ok=4 first_fail=5");
}
