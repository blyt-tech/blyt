//! Persistent resources declared in the manifest — end-to-end across the three
//! legs (blytplay / WASM / libretro), issue #160, ADR-0028.
//!
//! A `persistent_resources` declaration pre-loads the named resources before
//! `init()` runs, pins them in cache for the cart's whole life (never evicted),
//! makes `release`/`unpin` no-ops on residency, and reserves their bytes against
//! the unified 16 MB budget from cart start. These pin the cart-visible contract
//! identically on every leg; the cheap host-side mechanics are unit-tested in
//! test_resource_eviction. The native bare-metal leg is covered by the QEMU gate
//! (native_qemu.rs).
//!
//! The over-budget acceptance criterion (AC4) is realised primarily as a
//! build-time guard — the packer refuses an over-budget or unknown persistent set
//! — which is deterministic and cross-leg-identical by construction (same packer,
//! before any cart ships). The runtime carries a defensive mirror (unit-tested).

mod common;
use common::*;
use tempfile::TempDir;

/// AC1 + AC3 + AC5 (C): a persistent resource is resident from frame 0 (AC5), its
/// bytes are readable in `init()` by referencing the constant directly — no load
/// handle exists (AC1, ADR-0134) — and `pin`+`unpin` leave it resident and its
/// bytes valid (AC3) — all identical across every leg.
const USABLE_C: &str = r#"
#include "blyt.h"
#include "cart_resources.h"
#include <stddef.h>
#include <stdio.h>

/* Is `id` reported resident by the introspection API? Persistent resources are
 * listed from frame 0 even when never pinned (#160). */
static int is_resident(blyt_resource_id_t id) {
    blyt_mem_resource_t buf[8];
    uint32_t n = blyt_mem_resources(buf, 8);
    for (uint32_t i = 0; i < n; i++)
        if (buf[i].id == id)
            return 1;
    return 0;
}

static unsigned sum_bytes(const void *p, size_t n) {
    unsigned s = 0;
    for (size_t i = 0; i < n; i++)
        s += ((const unsigned char *)p)[i];
    return s;
}

void blyt_cart_init(void) {
    blyt_resource_id_t pid = (blyt_resource_id_t)R_PERS;

    /* AC5: resident from frame 0 with no prior load. */
    int r0 = is_resident(pid);

    /* AC1: read its bytes by referencing the constant directly (ADR-0134) — the
     * raw pin window; the bytes are already resident, so this is the preloaded
     * path. */
    const void *p = NULL;
    size_t sz = 0;
    unsigned sum = 0;
    if (blyt_resource_pin(pid, &p, &sz) == BLYT_OK && p)
        sum = sum_bytes(p, sz);
    blyt_resource_unpin(pid);

    /* AC3: pin/unpin is a no-op on residency — a persistent resource stays
     * resident and its bytes stay valid afterwards. */
    int r1 = is_resident(pid);
    const void *p2 = NULL;
    size_t sz2 = 0;
    unsigned sum2 = 0;
    if (blyt_resource_pin(pid, &p2, &sz2) == BLYT_OK && p2)
        sum2 = sum_bytes(p2, sz2);
    blyt_resource_unpin(pid);

    char line[128];
    snprintf(line, sizeof(line), "PERS r0=%d sum=%u sz=%u r1=%d sum2=%u", r0, sum, (unsigned)sz, r1,
             sum2);
    blyt_console_debug(line);
}
void blyt_cart_update(void) { blyt_quit(); }
void blyt_cart_draw(void) {}
"#;

/// `pers.bin` = {0x10,0x20,0x30,0x40,0x50}: sum 240, length 5 (raw, byte-exact).
const PERS_BYTES: &[u8] = &[0x10, 0x20, 0x30, 0x40, 0x50];

#[test]
fn persistent_usable_without_load_all_legs() {
    require_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("pers_usable_c");
    CartProject::new()
        .c(USABLE_C)
        .asset_bytes("pers.bin", PERS_BYTES)
        .persistent(&["pers"])
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());
    // r0=1 (resident from frame 0), sum/sz of the 5 known bytes, r1=1 (still
    // resident after release/unpin), sum2 identical (bytes still valid).
    run_cart_all_legs(&cart, "PERS r0=1 sum=240 sz=5 r1=1 sum2=240");
}

/// AC2 (C): under budget pressure a persistent resource is NOT evicted while a
/// non-persistent sibling is. Proven through the *deterministic* budget tier (the
/// epic forbids asserting advisory residency numbers cross-leg): a persistent
/// 4 MiB resource permanently reserves 4 MiB of the 16 MB budget (so heap headroom
/// is 12 MiB), whereas a materialised-but-unloaded 4 MiB sibling reserves nothing
/// (it is evictable) — so the measured headroom is 12 MiB, not 8 MiB. The exact
/// count is identical on every leg. A regression either way moves the count:
/// persistent not reserved → 16 MiB headroom; sibling wrongly reserved → 8 MiB.
const EVICT_C: &str = r#"
#include "blyt.h"
#include "cart_resources.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#define CHUNK (64 * 1024)
#define MAXP 512
static void *g_ptrs[MAXP];

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
    /* Materialise the non-persistent sibling (pin+unpin) so it becomes resident
     * but stays evictable (pin_count == 0, not persistent). */
    const void *p = NULL;
    size_t s = 0;
    blyt_resource_pin((blyt_resource_id_t)R_TRANS4, &p, &s);
    blyt_resource_unpin((blyt_resource_id_t)R_TRANS4);

    /* Persistent 4 MiB reserves budget from start; the transient 4 MiB does not. */
    int n = fill_heap();

    /* Persistent still resident and correct after the pressure. */
    const void *pp = NULL;
    size_t ps = 0;
    int pers_ok = (blyt_resource_pin((blyt_resource_id_t)R_PERS4, &pp, &ps) == BLYT_OK && pp &&
                   ps == 4u * 1024u * 1024u);
    blyt_resource_unpin((blyt_resource_id_t)R_PERS4);

    char line[96];
    snprintf(line, sizeof(line), "PERSEVICT n=%d pers_ok=%d", n, pers_ok);
    blyt_console_debug(line);
}
void blyt_cart_update(void) { blyt_quit(); }
void blyt_cart_draw(void) {}
"#;

#[test]
fn persistent_not_evicted_under_pressure_all_legs() {
    require_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("pers_evict_c");
    CartProject::new()
        .c(EVICT_C)
        // pers4 (id 1) persistent, trans4 (id 2) not — sorted by name.
        .asset_bytes("pers4.bin", &[0u8; 4 * 1024 * 1024])
        .asset_bytes("trans4.bin", &[0u8; 4 * 1024 * 1024])
        .persistent(&["pers4"])
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());
    // 12 MiB headroom (only the persistent 4 MiB reserved) -> 191 blocks; the
    // persistent resource survives the pressure (pers_ok=1). Same on every leg.
    run_cart_all_legs(&cart, "PERSEVICT n=191 pers_ok=1");
}

/// AC1 + AC5 (Lua, incl. the WASM host-Lua fast path): a persistent resource is
/// resident from frame 0 via `blyt32.mem.stats().resources_loaded`, without the
/// cart ever calling `load`. Covers the host-Lua preload path specifically.
const RESIDENT_LUA: &str = r#"
local R = require("cart_resources")
function init()
    local m = blyt32.mem.stats()
    local resident = 0
    -- resources_loaded reports the baked constant id (ADR-0134); match R.PERS.
    for _, r in ipairs(m.resources_loaded) do
        if r.id == R.PERS:id() then resident = 1 end
    end
    blyt.debug.print(string.format("PERSLUA resident=%d loaded=%d", resident, #m.resources_loaded))
end
function update() blyt.quit() end
function draw() end
"#;

#[test]
fn persistent_resident_from_frame0_lua_all_legs() {
    require_lua_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("pers_resident_lua");
    CartProject::new()
        .lua(RESIDENT_LUA)
        .asset_bytes("pers.bin", PERS_BYTES)
        .persistent(&["pers"])
        .write(&project);

    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());
    // The single persistent resource is the only resident one, from frame 0. Host-Lua
    // is the default for a pure-Lua cart on non-RISC-V hosts (ADR-0136), so all three
    // legs preload persistent resources into the runner's resource table (no session).
    run_cart_all_legs(&cart, "PERSLUA resident=1 loaded=1");
}

/// AC4 (build-time guard, the primary over-budget oracle): a persistent set whose
/// decompressed total exceeds 16 MiB fails the build — deterministic and
/// cross-leg-identical because it is the same packer, before any cart ships.
const MINIMAL_C: &str = r#"
#include "blyt.h"
void blyt_cart_init(void) {}
void blyt_cart_update(void) { blyt_quit(); }
void blyt_cart_draw(void) {}
"#;

#[test]
fn persistent_over_budget_fails_build() {
    require_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("pers_over_budget");
    // A single 17 MiB persistent resource exceeds the 16 MiB budget (and exercises
    // the single-oversized-resource-trips-the-sum case).
    CartProject::new()
        .c(MINIMAL_C)
        .asset_bytes("huge.bin", &[0u8; 17 * 1024 * 1024])
        .persistent(&["huge"])
        .write(&project);

    let err = build_cart_expect_failure(&project);
    assert!(
        err.contains("budget") || err.contains("16 MiB"),
        "expected an over-budget build error, got: {err}"
    );
}

#[test]
fn persistent_unknown_name_fails_build() {
    require_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("pers_unknown_name");
    CartProject::new()
        .c(MINIMAL_C)
        .asset_bytes("real.bin", b"hello")
        .persistent(&["nonexistent"])
        .write(&project);

    let err = build_cart_expect_failure(&project);
    assert!(
        err.contains("not a known resource") && err.contains("nonexistent"),
        "expected an unknown-resource build error, got: {err}"
    );
}
