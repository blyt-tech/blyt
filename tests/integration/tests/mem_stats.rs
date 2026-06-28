//! Memory-introspection API tests (`blyt32.mem.stats()`, ADR-0029, issue #159,
//! epic #156 child 4).
//!
//! Covers the cart-facing surface across all three legs (native / WASM /
//! libretro) and all three languages (C / Lua / Rust), plus the host-Lua WASM
//! fast path, and the deterministic-vs-advisory contract:
//!   - `budget_cap` is 16 MB on every leg (deterministic).
//!   - `total_used == cart_allocations + resource_cache_used` at every sample.
//!   - the loaded-resource enumeration is identical across legs for the same
//!     input; the advisory cache numbers are asserted on a single leg only.
//!
//! The scalar stats are published sums read straight from the #158 accounting
//! block (no ECALL); only the `resources_loaded` list is resolved on demand.

mod common;

use common::{
    CartProject, blytplay, build_cart, build_lua_cart, require_libretro_core, require_lua_sdk,
    require_rust_riscv_target, require_sdk, require_wasm, run_cart_all_legs,
};
use tempfile::TempDir;

/// A 2048-byte run of a single byte: maximally compressible, so the packer
/// always ships it zstd-compressed. That means accessing it materializes an
/// *owned* decompressed buffer (2048 bytes) the runtime can later evict — the
/// only way `resource_cache_used` can rise on access and fall on eviction.
const BLOB_LEN: usize = 2048;

/// C cart for the AC1 consume test: load + access R_BLOB (materializing its
/// 2048 decompressed bytes), report stats, release, then report again after the
/// `--evict-every-frame` hook has evicted the now-unreferenced blob. Emits two
/// deterministic lines pinning `resource_cache_used` rising to 2048 and falling
/// to 0, the loaded-resource enumeration, and the total==cart+cache invariant.
const CONSUME_C: &str = r#"
#include "blyt.h"
#include "cart_resources.h"
#include <stdio.h>
#include <stdlib.h>

static void emit(const char *tag) {
    blyt_mem_stats_t s = {0};
    blyt_mem_stats(&s);                       /* scalars: accounting-block read */
    blyt_mem_resource_t res[8];
    uint32_t n = blyt_mem_resources(res, 8);  /* loaded list: on demand */
    uint32_t fid = n > 0 ? res[0].id : 0;
    uint32_t fsz = n > 0 ? res[0].size : 0;
    int inv = s.total_used == s.cart_allocations + s.resource_cache_used;
    char line[160];
    snprintf(line, sizeof(line), "%s cap=%u cache=%u loaded=%u first=%u:%u inv=%d",
             tag, s.budget_cap, s.resource_cache_used, n, fid, fsz, inv);
    blyt_console_debug(line);
}

static blyt_resource_h g_h;

void blyt_cart_init(void) {
    blyt_resource_load(R_BLOB, &g_h);
    size_t len = 0;
    void *p = blyt_resource_bytes_get(R_BLOB, &len); /* materialize the cache */
    free(p);
    emit("AFTER_LOAD");
    blyt_resource_release(g_h); /* now evictable; bytes resident until evicted */
}

void blyt_cart_update(void) {
    static int frame = 0;
    if (++frame >= 2) {
        /* The previous frame boundary's --evict-every-frame hook freed the
         * released blob's decompressed bytes, so the cache is empty again. */
        emit("AFTER_EVICT");
        blyt_quit();
    }
}
void blyt_cart_draw(void) {}
"#;

/// AC1 (the consume oracle, single leg by design — advisory numbers are not
/// cross-leg identical): loading + accessing a known resource raises
/// `resource_cache_used` by its decompressed size and lists it in the loaded
/// enumeration; release + forced eviction drops it back to zero.
#[test]
fn mem_stats_consume_single_leg() {
    require_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("mem_consume");
    CartProject::new()
        .c(CONSUME_C)
        .asset_bytes("blob.dat", &vec![b'A'; BLOB_LEN])
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    // One emulated run (blytplay) with the eviction hook produces both lines:
    // cache rises to 2048 with the blob loaded+accessed, then falls to 0 after
    // release + the frame-boundary eviction.
    use assert_cmd::Command;
    let out = Command::new(blytplay())
        .args(["--headless", "--evict-every-frame", cart.to_str().unwrap()])
        .assert()
        .success()
        .get_output()
        .stdout
        .clone();
    let s = String::from_utf8_lossy(&out);
    for expected in [
        "AFTER_LOAD cap=16777216 cache=2048 loaded=1 first=1:2048 inv=1",
        "AFTER_EVICT cap=16777216 cache=0 loaded=0 first=0:0 inv=1",
    ] {
        assert!(
            s.contains(expected),
            "expected {expected:?} in native output, got:\n{s}"
        );
    }
}

// ── Cross-leg / cross-language parity (AC2, AC3, AC4) ────────────────────────
//
// Each cart loads one uncompressed asset (`tag.dat` = "hello", 5 bytes, id 1)
// and reports the DETERMINISTIC half of the API: budget_cap (16 MB on every
// leg), the total==cart+cache invariant, and the loaded-resource enumeration
// (id + size — deterministic because loads are). The advisory cache numbers are
// deliberately not in the asserted line. The three carts emit the SAME string,
// so one expected value pins both cross-leg and cross-language parity.
const PARITY_EXPECT: &str = "MEM cap=16777216 inv=1 loaded=1 first=1:5";

const PARITY_C: &str = r#"
#include "blyt.h"
#include "cart_resources.h"
#include <stdio.h>

void blyt_cart_init(void) {
    blyt_resource_h h;
    blyt_resource_load(R_TAG, &h);
    blyt_mem_stats_t s = {0};
    blyt_mem_stats(&s);
    blyt_mem_resource_t res[8];
    uint32_t n = blyt_mem_resources(res, 8);
    int inv = s.total_used == s.cart_allocations + s.resource_cache_used;
    uint32_t fid = n > 0 ? res[0].id : 0;
    uint32_t fsz = n > 0 ? res[0].size : 0;
    char line[128];
    snprintf(line, sizeof(line), "MEM cap=%u inv=%d loaded=%u first=%u:%u",
             s.budget_cap, inv, n, fid, fsz);
    blyt_console_debug(line);
}
void blyt_cart_update(void) { blyt_quit(); }
void blyt_cart_draw(void) {}
"#;

const PARITY_LUA: &str = r#"
local R = require("cart_resources")
function init()
    local h = R.TAG:load()
    local m = blyt32.mem.stats()
    local inv = (m.total_used == m.cart_allocations + m.resource_cache_used) and 1 or 0
    local first = m.resources_loaded[1]
    local fid = first and first.id or 0
    local fsz = first and first.size or 0
    blyt.debug.print(string.format("MEM cap=%d inv=%d loaded=%d first=%d:%d",
        m.budget_cap, inv, #m.resources_loaded, fid, fsz))
    h:release()
end
function update() blyt.quit() end
function draw() end
"#;

const PARITY_RUST: &str = r#"#![no_std]

extern crate alloc;
use alloc::format;

include!(env!("BLYT_CART_RESOURCES_RS"));

#[no_mangle]
pub extern "C" fn blyt_cart_init() {
    let h = R_TAG.load();
    let m = blyt::mem::stats();
    let inv = if m.total_used == m.cart_allocations + m.resource_cache_used { 1 } else { 0 };
    let (fid, fsz) = m
        .resources_loaded
        .first()
        .map(|r| (r.id, r.size))
        .unwrap_or((0, 0));
    blyt::console_debug(&format!(
        "MEM cap={} inv={} loaded={} first={}:{}",
        m.budget_cap, inv, m.resources_loaded.len(), fid, fsz
    ));
    h.release();
}

#[no_mangle]
pub extern "C" fn blyt_cart_update() { blyt::quit(); }

#[no_mangle]
pub extern "C" fn blyt_cart_draw() {}
"#;

/// AC2/AC3/AC4 (C): budget_cap is 16 MB and the loaded enumeration + invariant
/// are identical across native / WASM / libretro for the same input.
#[test]
fn mem_stats_c_parity_all_legs() {
    require_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("mem_parity_c");
    CartProject::new()
        .c(PARITY_C)
        .asset_bytes("tag.dat", b"hello")
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());
    run_cart_all_legs(&cart, PARITY_EXPECT);
}

/// AC4 (Lua, incl. the WASM host-Lua fast path): `blyt32.mem.stats()` returns
/// the same fields as the C surface for the same state, identically across legs.
#[test]
fn mem_stats_lua_parity_all_legs() {
    require_lua_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("mem_parity_lua");
    CartProject::new()
        .lua(PARITY_LUA)
        .asset_bytes("tag.dat", b"hello")
        .write(&project);

    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());
    run_cart_all_legs(&cart, PARITY_EXPECT);
}

/// AC4 (Rust): the Rust SDK surface returns the same fields as C/Lua for the
/// same state, identically across legs.
#[test]
fn mem_stats_rust_parity_all_legs() {
    require_sdk();
    require_rust_riscv_target();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("mem_parity_rust");
    CartProject::new()
        .rust(PARITY_RUST)
        .asset_bytes("tag.dat", b"hello")
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());
    run_cart_all_legs(&cart, PARITY_EXPECT);
}
