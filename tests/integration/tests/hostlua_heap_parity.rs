//! Cross-leg `guest_heap_used` byte-equality for the host-Lua fast path — the
//! acceptance oracle for the #231 heap-accounting seam (BLYT_HOSTLUA_HEAP_SEAM,
//! epic #230, ADR-0029/0008).
//!
//! ADR-0029 promises the deterministic tier of the memory-introspection API is
//! bit-identical across every leg: `blyt32.mem.stats().cart_allocations`
//! (`guest_heap_used`) must be the SAME number on a 32-bit console and on the
//! 64-bit desktop, because the allocation OUTCOME (does malloc succeed, at which
//! point does the 16 MB budget bite) is a value carts are entitled to branch on
//! and to expect identical across a save/rewind/netplay peer set.
//!
//! The canonical oracle is the **wasm32** host-Lua fast path: the native
//! host-Lua fast path (`cart_run_hostlua.c`) is the SAME runner as the wasm one
//! (`wasm_main.c`) — same allocation sequence — so once the seam models its
//! object sizes down to 32-bit the two report the byte-identical count. The
//! 64-bit path over-reports without the seam because its Lua objects carry
//! 8-byte pointers (pointer-bearing headers: `TString` 28→48, `Table` 28→48,
//! `Proto` 84→128, …); the seam substitutes the rv32 sizes (DIRECTION 1) so
//! `guest_heap_used` matches wasm32 exactly.
//!
//! NOTE — the oracle is wasm32, NOT the emulated/bare-metal rv32 *guest-lib* Lua
//! path (rv32emu / hardware running libblyt32lua). Those two runners are
//! byte-identical in per-object size but differ by a fixed runner baseline (the
//! guest-lib path registers the blyt32 API in the guest heap; the host-Lua fast
//! path keeps that glue host-side), so guest-lib-rv32 ≠ host-Lua-wasm32 today
//! independent of word size. That gap is expected to dissolve when epic #230
//! retires the guest-lib Lua path (host-Lua everywhere, incl. rv32 hardware via
//! the native fast path); it is out of scope for the size seam.
//!
//! SCOPE (#231 → #230): this cart drives the seam's two dominant divergent
//! headers — `Table` (array-part tables) and `TString` (long, non-interned
//! string bodies) — where the size normalisation is byte-exact across legs. A
//! handful of other constructs (short-string *interning* — the string table
//! rehashes at counts that differ because the two runners intern different
//! scaffolding strings; `luaL_Buffer`-boxed strings > the aux-buffer threshold;
//! closures with open upvalues; coroutine threads) still carry a small (≤ a few
//! hundred bytes) *execution-model* residual: the wasm runner drives the cart
//! through a `co_body` coroutine while the native runner calls lifecycle
//! functions directly from C. Excluding the VM data stack + CallInfo from
//! `guest_heap_used` (the #231 fix) removes the bulk of that difference; the last
//! residual needs the two runners to execute identically, which is the epic-#230
//! "one shared runner" unification. Tracked as a #230 follow-up; NOT asserted
//! here so this gate pins the seam's sizing without depending on that refactor.

mod common;
use common::*;
use tempfile::TempDir;

/// A cart that allocates a deterministic spread of the two pointer-bearing
/// internal object types whose 32-bit vs 64-bit header sizes diverge and whose
/// accounting is byte-exact across the host-Lua legs: `Table` (array-part tables:
/// `Table` header + a `TValue` array, 16 B on both ABIs, so only the diverging
/// header moves the count) and long `TString` (header diverges, body bytes are
/// arch-identical). It holds every object live in `KEEP` so a full collection
/// cannot reclaim them, then reads `cart_allocations` (the arena's live
/// `guest_heap_used`). The strings are built with `string.rep` (long strings are
/// never interned, so the string table is untouched) and stay under the
/// `luaL_Buffer` threshold, keeping the count free of the execution-model
/// residual documented above. The fixed Lua hash seed makes table layout
/// identical across legs, so the only thing that can move the count is the
/// per-object header size — exactly the seam's job to normalise.
const HEAP_LUA: &str = r#"
local KEEP = {}

function init()
    -- Array-part tables: Table header (rv32 28 vs host 48) + a TValue array
    -- (16 B on both ABIs). Only the diverging header should move the count.
    for _ = 1, 60 do
        local a = {}
        for j = 1, 24 do
            a[j] = j
        end
        KEEP[#KEEP + 1] = a
    end

    -- Long, non-interned strings under the luaL_Buffer aux-buffer threshold:
    -- TString long header (diverges) + body bytes (arch-identical).
    for i = 1, 40 do
        KEEP[#KEEP + 1] = string.rep("x", 100 + i)
    end
    for i = 1, 20 do
        KEEP[#KEEP + 1] = string.rep("y", 300 + i)
    end

    collectgarbage("collect")
    local m = blyt32.mem.stats()
    blyt.debug.print(string.format("HEAP used=%d", m.cart_allocations))
end

function update()
    blyt.quit()
end

function draw() end
"#;

/// Parse the single `HEAP used=<n>` line the cart prints.
fn heap_used(output: &str) -> u64 {
    let line = output
        .lines()
        .find(|l| l.contains("HEAP used="))
        .unwrap_or_else(|| panic!("no 'HEAP used=' line in output:\n{output}"));
    let n = line.rsplit("used=").next().unwrap().trim();
    n.parse::<u64>()
        .unwrap_or_else(|_| panic!("bad HEAP used value {n:?} in line {line:?}"))
}

/// The #231 seam target: the native 64-bit host-Lua fast path reports the
/// byte-identical `guest_heap_used` as its wasm32 sibling (the 32-bit canonical
/// for this runner) for the seam's dominant pointer-bearing headers.
#[test]
fn lua_guest_heap_used_matches_wasm32_on_native_host_lua() {
    require_sdk();
    require_lua_sdk();
    require_wasm();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hostlua_heap_parity");
    CartProject::new().lua(HEAP_LUA).write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    // The oracle: the host-Lua fast path built for wasm32 — the same runner as
    // the native path, so its 32-bit object sizes are the canonical target.
    let wasm = heap_used(&capture_cart_wasm(&cart, &[]));

    // Native 64-bit host-Lua path (the default for a pure-Lua cart on non-RISC-V
    // hosts, ADR-0136): identical runner, but 8-byte-pointer objects over-report
    // unless the seam models the count down to rv32 sizes.
    let hostlua = heap_used(&capture_cart_native(&cart, &[]));
    assert_eq!(
        hostlua, wasm,
        "native host-Lua guest_heap_used must equal its wasm32 sibling \
         (SEAM TARGET, #231): the BLYT_HOSTLUA_HEAP_SEAM rv32 sizing of \
         pointer-bearing Table/TString headers must make the 64-bit count \
         byte-identical to wasm32"
    );
}
