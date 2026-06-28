//! Memory introspection (ADR-0029, issue #159) — the Rust layer over the
//! `blyt_mem_stats` API.
//!
//! [`stats`] reports cart-visible working-memory usage so a cart can make
//! informed release decisions near the 16 MB budget.
//!
//! # Determinism contract — read before branching on any field
//!
//! The fields are **tiered**, and mixing the tiers is a bug (ADR-0029
//! amendment, #159):
//!
//! - **Deterministic** — bit-identical across every peer/platform, safe to
//!   branch game logic on:
//!   - [`MemStats::budget_cap`] (always 16 MB)
//!   - the *outcome* of an allocation (a `load` / alloc failing at the cap)
//! - **Advisory** — history-dependent (LRU/eviction order differs across
//!   platforms), must **not** feed deterministic game state; a *tuning* signal
//!   only:
//!   - [`MemStats::resource_cache_used`], [`MemStats::total_used`]
//!   - [`MemStats::resources_loaded`] residency

extern crate alloc;

use alloc::vec::Vec;

// Scalar stats struct shared with the C ABI (blyt_mem_stats_t, 4 x u32).
#[repr(C)]
struct MemStatsRaw {
    resource_cache_used: u32,
    cart_allocations: u32,
    total_used: u32,
    budget_cap: u32,
}

/// One currently-loaded resource and its (decompressed) size, as enumerated by
/// [`stats`] (mirrors the C `blyt_mem_resource_t`).
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ResourceSize {
    pub id: u32,
    pub size: u32,
}

extern "C" {
    // Scalars: a guest-memory read of the published accounting block (no ECALL).
    fn blyt_mem_stats(out: *mut MemStatsRaw);
    // The loaded-resource list: resolved on demand (host-owned table data).
    fn blyt_mem_resources(resources: *mut ResourceSize, cap: u32) -> u32;
}

/// Current cart-visible memory stats. `total_used == cart_allocations +
/// resource_cache_used`.
#[derive(Clone, Debug)]
pub struct MemStats {
    /// Advisory: resident decompressed resource-cache bytes.
    pub resource_cache_used: u32,
    /// Live cart heap bytes (`guest_heap_used`).
    pub cart_allocations: u32,
    /// Advisory: `cart_allocations + resource_cache_used`.
    pub total_used: u32,
    /// Deterministic: the 16 MB working-memory cap.
    pub budget_cap: u32,
    /// Advisory: the currently-loaded resources and their sizes.
    pub resources_loaded: Vec<ResourceSize>,
}

/// Read the current memory stats, enumerating every currently-loaded resource.
///
/// The scalar fields are a cheap guest-memory read; the `resources_loaded`
/// enumeration is the on-demand part (see [`scalars`] if you only need the
/// totals and want to skip it).
pub fn stats() -> MemStats {
    let s = scalars();

    // Size the loaded-resource list, allocate, then fill it. `n` is stable
    // across the two calls — execution is single-threaded and deterministic.
    // SAFETY: a null array with cap 0 just returns the count.
    let n = unsafe { blyt_mem_resources(core::ptr::null_mut(), 0) };
    let mut resources: Vec<ResourceSize> = Vec::with_capacity(n as usize);
    // SAFETY: `resources` has capacity for `n` entries; the call writes at most
    // `n` and returns how many it wrote.
    let written = unsafe { blyt_mem_resources(resources.as_mut_ptr(), n) };
    // SAFETY: the runtime initialised `written` (<= n) entries.
    unsafe { resources.set_len(written.min(n) as usize) };

    MemStats {
        resource_cache_used: s.resource_cache_used,
        cart_allocations: s.cart_allocations,
        total_used: s.total_used,
        budget_cap: s.budget_cap,
        resources_loaded: resources,
    }
}

/// Just the scalar totals — a guest-memory read of the published accounting
/// block, with no host round-trip and no `resources_loaded` allocation. Use
/// this on a hot budget-polling path; use [`stats`] when you also want the
/// per-resource breakdown.
pub fn scalars() -> MemStats {
    let mut raw = MemStatsRaw {
        resource_cache_used: 0,
        cart_allocations: 0,
        total_used: 0,
        budget_cap: 0,
    };
    // SAFETY: `raw` is a valid, writable struct.
    unsafe { blyt_mem_stats(&mut raw) };
    MemStats {
        resource_cache_used: raw.resource_cache_used,
        cart_allocations: raw.cart_allocations,
        total_used: raw.total_used,
        budget_cap: raw.budget_cap,
        resources_loaded: Vec::new(),
    }
}
