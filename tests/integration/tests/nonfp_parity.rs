//! Non-FP determinism parity matrix on the native host-Lua path (#235,
//! completing Spike Z Q5).
//!
//! FP determinism is proven separately (`fp_parity.rs`, ADR-0135 / #225 / #227)
//! and is not re-derived here. This module validates the **non-FP** surface that
//! host-Lua-everywhere also rides on, on the *full* host-Lua runner (state
//! buffers, S-proxy, GC) — the parts the minimal Spike-Z control binary in
//! `native_hostlua.rs` deliberately omits.
//!
//! ## Reference model (ADR-0136 — NOT the emulated-RV32-Lua oracle)
//!
//! ADR-0136 drops the emulated-Lua-under-rv32emu leg as the determinism
//! reference (#236 retires it). Parity is asserted against:
//!   - **cross-host-Lua-leg agreement** — native blytplay (`BLYT_HOSTLUA=1`,
//!     exercised on both x86-64 CI and arm64), wasm (auto host-Lua fast path),
//!     and the libretro core's embedded host-Lua path produce byte-identical
//!     observable output;
//!   - a **pinned golden** digest (regenerate rarely, reviewed);
//!   - **RISC-V hardware parity** via the QEMU native gate — the same non-FP
//!     carts run through the native RV32 guest-lib path on real RISC-V in
//!     `native_qemu.rs` (Gate 33: non-FP parity), asserting the same golden.
//!
//! The still-shipping emulated blytplay + libretro legs are also asserted here
//! as a free cross-check while they ship; those two calls retire with #236.
//!
//! ## Surface covered
//!
//! Integer overflow/wrap, bitwise, div/mod, string interning, and table
//! iteration order are already smoked on the minimal native leg by
//! `native_hostlua::native_hostlua_nonfp_parity`. This module adds the two
//! surfaces that need the full runtime:
//!   - **NaN-boundary canonicalization** (`nonfp_nan_boundary_f64_state_buffer_parity`)
//!     — the honest gap flagged in #225: `blyt_canon_f64` (ADR-0010) covering
//!     every NaN that reaches an f64 state-buffer field was reasoned by
//!     inspection, never exercised. Writing an f64 field through the `S` proxy
//!     also first exposed a latent host-Lua bug — the proxy generator routed
//!     f64 (tag 8) fields to the `i32` accessors on both the native and WASM
//!     host-Lua legs (`type_names[]` stopped at `bool`) — fixed alongside this.
//!   - **GC timing/order** (`nonfp_gc_finalizer_order_parity`) — finalizer
//!     (`__gc`) execution order across collection phases, a deterministic,
//!     exec-model-independent GC-order signal, folded into a digest.
//!
//! `guest_heap_used` byte-accounting has its own dedicated host-Lua parity
//! coverage in `hostlua_heap_parity.rs` (#231, on the byte-exact Table/TString
//! constructs; the exec-model residual is tracked by #242) and is intentionally
//! kept out of these cross-leg goldens to avoid coupling to that residual.
//!
//! The shared cart sources + pinned goldens live in `common::nonfp` so this
//! suite and the QEMU gate never drift.

mod common;

use std::path::Path;

use common::nonfp;
use common::{
    CartProject, build_lua_cart, require_libretro_core, require_lua_sdk, require_sdk, require_wasm,
    run_cart_libretro, run_cart_libretro_with_env, run_cart_native, run_cart_native_with_env,
    run_cart_wasm,
};
use tempfile::TempDir;

/// Assert `expected` appears on every shipped leg that runs a pure-Lua cart: the
/// three host-Lua reference legs (native blytplay `BLYT_HOSTLUA=1`, wasm auto
/// host-Lua, libretro core `BLYT_HOSTLUA=1`) plus the still-shipping emulated
/// blytplay + libretro legs as a cross-check. Mirrors the FP suite's
/// `run_cart_all_legs` + explicit host-Lua legs, in one place.
fn assert_all_hostlua_legs(cart: &Path, expected: &str) {
    // Emulated cross-check (retires with #236).
    run_cart_native(cart, expected);
    run_cart_libretro(cart, expected);
    // ADR-0136 reference: the three host-Lua legs.
    run_cart_native_with_env(cart, &[("BLYT_HOSTLUA", "1")], expected);
    run_cart_wasm(cart, expected);
    run_cart_libretro_with_env(cart, &[("BLYT_HOSTLUA", "1")], expected);
}

#[test]
fn nonfp_nan_boundary_f64_state_buffer_parity() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("nonfp_nan");
    CartProject::new()
        .config(nonfp::NAN_CONFIG)
        .lua(nonfp::NAN_CART)
        .write(&project);
    let cart = build_lua_cart(&project);

    // Cross-leg agreement + golden: the digest folds the *read-back* bits, so it
    // only matches if every leg canonicalized the NaN identically.
    assert_all_hostlua_legs(&cart, nonfp::NAN_DIGEST);
    // Positive ADR-0010 contract: the literal canonical value on every leg.
    assert_all_hostlua_legs(&cart, nonfp::NAN_CANON_LINE);
}

#[test]
fn nonfp_gc_finalizer_order_parity() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("nonfp_gc");
    CartProject::new().lua(nonfp::GC_CART).write(&project);
    let cart = build_lua_cart(&project);

    assert_all_hostlua_legs(&cart, nonfp::GC_DIGEST);
}
