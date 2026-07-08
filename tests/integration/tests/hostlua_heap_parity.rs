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
//! (`wasm_main.c`) — same environment setup, same allocation sequence — so once
//! the seam models its object sizes down to 32-bit the two must report the
//! byte-identical count. The 64-bit path over-reports today because its Lua
//! objects carry 8-byte pointers (pointer-bearing headers: TString 28→48,
//! Table 28→48, Proto 84→128, …); the seam substitutes the rv32 sizes
//! (DIRECTION 1) so `guest_heap_used` matches wasm32 exactly.
//!
//! NOTE — the oracle is wasm32, NOT the emulated/bare-metal rv32 *guest-lib* Lua
//! path (rv32emu / hardware running libblyt32lua). Those two runners are
//! byte-identical in per-object size but differ by a fixed ~360 B runner
//! baseline (the guest-lib path registers the blyt32 API in the guest heap; the
//! host-Lua fast path keeps that glue host-side), so guest-lib-rv32 ≠
//! host-Lua-wasm32 today independent of word size. That gap is expected to
//! dissolve when epic #230 retires the guest-lib Lua path (host-Lua everywhere,
//! incl. rv32 hardware via the native fast path); it is out of scope for the
//! size seam, which only makes native host-Lua match its own wasm sibling.
//!
//! Until the seam lands this is RED at the second assertion: the type-covering
//! cart allocates a spread of pointer-bearing objects (strings, array + hash
//! tables, closures with upvalues, nested Protos, coroutine threads) whose
//! headers diverge 64-bit vs 32-bit, so the native host-Lua count differs from
//! wasm32's.

mod common;
use common::*;
use tempfile::TempDir;

/// A type-covering pure-Lua cart. It allocates a deterministic spread of the
/// pointer-bearing internal object types whose 32-bit vs 64-bit header sizes
/// diverge — TString (short interned + long bodies), Table (array + hash parts),
/// LClosure + UpVal + nested Proto, and coroutine threads (lua_State) — holds
/// every one live in `KEEP` so a full collection cannot reclaim them, then reads
/// `cart_allocations` (the arena's live `guest_heap_used`). The fixed Lua hash
/// seed makes string interning + table layout identical across legs, so the only
/// thing that can move the count is the per-object header size — exactly the
/// seam's job to normalise.
const HEAP_LUA: &str = r#"
local KEEP = {}

-- 100 distinct closures sharing one nested Proto, each capturing two upvalues:
-- drives LClosure + UpVal + Proto (all pointer-bearing headers).
local function make_closures(n)
    local t = {}
    for i = 1, n do
        local a, b = i, i * 2
        t[i] = function() return a + b end
    end
    return t
end

function init()
    -- Short interned strings as hash keys: TString headers + Table hash (Node) part.
    local h = {}
    for i = 1, 200 do
        h["key_" .. i] = i
    end
    KEEP[#KEEP + 1] = h

    -- Array-part tables: Table header + TValue array (TValue is 16 B on both
    -- ABIs, so only the diverging Table header should move the count).
    for _ = 1, 50 do
        local a = {}
        for j = 1, 32 do
            a[j] = j
        end
        KEEP[#KEEP + 1] = a
    end

    -- Long strings: TString header (diverges) + body bytes (arch-identical).
    for i = 1, 20 do
        KEEP[#KEEP + 1] = string.rep("x", 512 + i)
    end

    -- Closures with upvalues + a nested Proto.
    KEEP[#KEEP + 1] = make_closures(100)

    -- Coroutine threads: lua_State header + StackValue stack.
    for _ = 1, 10 do
        KEEP[#KEEP + 1] = coroutine.create(function()
            coroutine.yield()
        end)
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
/// for this runner).
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

    // Native 64-bit host-Lua fast path (`BLYT_HOSTLUA=1`): identical runner, but
    // 8-byte-pointer objects over-report until the seam models the count down to
    // rv32 sizes. RED until BLYT_HOSTLUA_HEAP_SEAM lands.
    let hostlua = heap_used(&capture_cart_native(&cart, &[("BLYT_HOSTLUA", "1")]));
    assert_eq!(
        hostlua, wasm,
        "native host-Lua guest_heap_used must equal its wasm32 sibling \
         (SEAM TARGET, #231): host-sized pointer-bearing object headers \
         over-report until BLYT_HOSTLUA_HEAP_SEAM normalises them to rv32 sizes"
    );
}
