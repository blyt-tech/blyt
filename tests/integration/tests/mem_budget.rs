//! Unified 16 MB memory-budget enforcement, end-to-end across the emulated legs
//! (blytplay / WASM / libretro) — issue #158, ADR-0008/0027.
//!
//! These pin the cart-visible contract: a guest `malloc` and a resource `load`
//! draw on ONE 16 MB budget, and the point at which an allocation fails is
//! identical across every leg (the determinism contract). The cheap host-side
//! mechanics (the arena accounting, the footprint/LRU policy) are unit-tested in
//! test_arena / test_mem_budget / test_resource_budget; this is the integration
//! oracle that the wiring actually enforces the budget through a real cart.

mod common;
use common::*;
use tempfile::TempDir;

/// A C cart that measures guest-heap headroom (64 KiB allocations until `malloc`
/// returns NULL) twice: once with nothing resident, then again after `load`ing a
/// 4 MiB resource (which reserves 4 MiB of non-evictable footprint up front).
/// The second count must be strictly smaller — proving heap and resource cache
/// share the SAME budget, not two separate pools — and both counts must be
/// byte-identical across legs.
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

    blyt_resource_h h = BLYT_RESOURCE_INVALID;
    blyt_result_t lr = blyt_resource_load(R_BIG, &h); /* reserve 4 MiB footprint */

    int b = fill_heap(); /* only ~12 MB now available to the heap */

    char line[128];
    snprintf(line, sizeof(line), "BUDGET a=%d b=%d lr=%d shrank=%d", a, b, (int)lr,
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

/// A C cart that `load`s five distinct 4 MiB resources in turn. Each load reserves
/// 4 MiB of non-evictable footprint up front, so the first four (16 MiB) fit the
/// budget exactly and the fifth must fail — at the SAME load on every leg. This
/// is the "deterministic nil at the cap across all legs" acceptance criterion.
const LOAD_CAP_C: &str = r#"
#include "blyt.h"
#include <stdio.h>

void blyt_cart_init(void) {
    int ok = 0, first_fail = -1;
    for (int id = 1; id <= 5; id++) {
        blyt_resource_h h = BLYT_RESOURCE_INVALID;
        blyt_result_t r = blyt_resource_load((blyt_resource_id_t)id, &h);
        if (r == BLYT_OK && h != BLYT_RESOURCE_INVALID)
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
