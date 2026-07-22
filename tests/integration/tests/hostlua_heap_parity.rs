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
//! SCOPE (#231 → #230): the first test drives the seam's two dominant divergent
//! headers — `Table` (array-part tables) and `TString` (long, non-interned string
//! bodies) — where the size normalisation is byte-exact across legs.
//!
//! The second test drives the constructs that were once attributed to a runner
//! *execution-model* divergence (the wasm `co_body` coroutine vs the native
//! direct-C runner). That attribution turned out to be one of three, and the
//! chain is worth recording because each layer hid the next:
//!
//!   1. **Execution model (#242).** Real, but only part of it. Unifying both legs
//!      onto one shared coroutine driver (`runtime/shared/blyt_hostlua_driver.h`)
//!      made coroutine threads byte-exact and took the residual 320 B -> 160 B.
//!   2. **Object sizing (#267).** Lua 5.5 gives long strings three layouts
//!      (`luaS_sizelngstr`); the seam modelled all three with the `LSTRREG`
//!      formula on the premise that pure Lua never makes an external string —
//!      false, because `luaL_pushresult` converts a boxed buffer via
//!      `lua_pushexternalstring`. Every boxed string was over-counted by the
//!      host−rv32 width of `falloc`+`ud`.
//!   3. **Order and pacing (#267).** With sizes exact, the count *still* moved:
//!      `cart_allocations` comes off a first-fit arena, so it depends on
//!      allocation and free ORDER too. Two things perturbed it — the legs
//!      hand-registered their scaffolding in different orders (fixed by
//!      `blyt_hostlua_api.h`), and the fork's GC pacing constants were on host
//!      pointer widths, so the 64-bit VM swept at different points than the
//!      32-bit legs (fixed by pinning them to rv32).
//!
//! The moral, for whoever debugs the next one: a divergence here is NOT
//! necessarily a per-object size error. Sizes being byte-identical across legs
//! does not imply the counts are — order and GC pacing reach the number too.

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

/// The #242 convergence cart: the constructs the #231 seam could NOT make
/// byte-exact, because they are sensitive to the *allocation sequence* rather
/// than to per-object size. Each one is here because the two runners perturbed
/// it differently while wasm drove the cart through a `co_body` coroutine and
/// native called lifecycle functions straight from C:
///
///   - **short-string interning** — a hash table keyed by many distinct short
///     strings. Short strings are interned in the global string table (`g->strt`),
///     which rehashes when its load factor tips; the two runners interned
///     different scaffolding strings (the `co_body` chunk source: `update`,
///     `draw`, `coroutine`, `yield`, `__blyt_phase_update`, … on wasm; only the
///     callback names on native), so the rehash landed at different counts.
///   - **`luaL_Buffer`-boxed strings** — `string.rep` past the ~512 B aux-buffer
///     threshold spills to a heap box (`lauxlib.c` `resizebox`), ~16 B/string,
///     and the box is a RAW byte buffer the seam accounts at host `nsize` (see
///     `BLYT_HOSTLUA_HEAP_RV_UNSET`).
///   - **closures with open upvalues** — an upvalue still pointing at a live
///     stack slot; boxed on close.
///   - **coroutine threads** — a cart-created `lua_State`. The wasm runner
///     already had two driver threads live; native had none.
///
/// Once ONE shared runner drives both legs (#242), the allocation sequence is
/// identical by construction and every one of these must land byte-exact.
const RESIDUAL_LUA: &str = r#"
local KEEP = {}

function init()
    -- Short-string interning: distinct short keys drive g->strt rehashes.
    local t = {}
    for i = 1, 200 do
        t["key_" .. i] = i
    end
    KEEP[#KEEP + 1] = t

    -- luaL_Buffer-boxed strings: past the aux-buffer threshold, so the
    -- concat spills to a heap box rather than staying on the stack buffer.
    for i = 1, 12 do
        KEEP[#KEEP + 1] = string.rep("z", 900 + i)
    end

    -- Closures with open upvalues: each closure captures a live local.
    for i = 1, 30 do
        local captured = i
        KEEP[#KEEP + 1] = function()
            captured = captured + 1
            return captured
        end
    end

    -- Coroutine threads: cart-created lua_State objects, suspended (their data
    -- stack / CallInfo is VM scratch the seam excludes; the thread header is not).
    for i = 1, 10 do
        local co = coroutine.create(function()
            coroutine.yield(i)
        end)
        coroutine.resume(co)
        KEEP[#KEEP + 1] = co
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

/// A cart that builds a fixed count of strings of a caller-chosen LENGTH, so a
/// test can walk the `luaL_BUFFERSIZE` aux-buffer threshold. Below it,
/// `string.rep` fills the buffer's on-stack `init.b` and `luaL_pushresult` copies
/// the bytes into a plain long string (`LSTRREG`). At or above it, `prepbuffsize`
/// spills to a heap box, and `luaL_pushresult` hands that box to
/// `lua_pushexternalstring` — producing an EXTERNAL string (`LSTRMEM`), a
/// different `TString` layout with `falloc`/`ud` live (see `LEN_PROBE_*` below).
/// The threshold is pinned to the wasm32 value (512) on every leg by the seam's
/// `LUAL_BUFFERSIZE` override, so both legs cross it at the same length.
const LEN_PROBE_LUA: &str = r#"
local KEEP = {}

function init()
    for _ = 1, 10 do
        KEEP[#KEEP + 1] = string.rep("z", LEN)
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

/// Lengths that keep `string.rep` inside the on-stack aux buffer (`LSTRREG`).
const LEN_PROBE_INLINE: &[usize] = &[100, 500, 512];
/// Lengths that spill to a heap box and become external strings (`LSTRMEM`).
const LEN_PROBE_EXTERNAL: &[usize] = &[513, 600, 900];

/// State layout for the registration-path gate (#270). Declaring ANY state
/// buffer is what makes `hl_active_ctx()` / `active_state_ctx()` non-NULL, which
/// is the sole condition under which `build_vm` arms `api.register_state_api` +
/// `api.register_s_proxy` (`cart_run_hostlua.c:2100`, `wasm_main.c:2273`) and the
/// shared canonical sequence fires the two hooks at `blyt_hostlua_api.h:358-361`.
/// Every other gate cart in this file declares NO layout, so those two hooks —
/// the ONE registration pair #267 left hand-mirrored per leg rather than folding
/// into the shared header — never run, and their allocation order/sizing has zero
/// cross-leg coverage.
///
/// The layout is deliberately rich: `Types` covers all nine field type tags in
/// declaration order (i8=0 … f64=8), so the per-leg `type_names[]` table that
/// must be extended in lockstep (`cart_run_hostlua.c:777`, `wasm_main.c:1625` —
/// the #235 f64-misroute / #253-audit class) is exercised end to end, including
/// the f64=8 boundary; and a second multi-slot `Entity` buffer (`count: 4`) makes
/// `register_s_proxy` emit several row tables and a second metatable pair. The
/// more machinery the generated proxy chunk allocates, the more surface a
/// registration reorder has to refract through the first-fit arena into a
/// `cart_allocations` divergence — which is exactly what this gate must catch.
const STATE_CONFIG: &str = "\
records:
  Types:
    fields:
      - { name: v_i8,   type: i8   }
      - { name: v_u8,   type: u8   }
      - { name: v_i16,  type: i16  }
      - { name: v_u16,  type: u16  }
      - { name: v_i32,  type: i32  }
      - { name: v_u32,  type: u32  }
      - { name: v_f32,  type: f32  }
      - { name: v_bool, type: bool }
      - { name: v_f64,  type: f64  }
  Entity:
    fields:
      - { name: hp, type: i32 }
      - { name: x,  type: f64 }
state_buffers:
  types:
    record: Types
    count: 2
  entity:
    record: Entity
    count: 4
";

/// The #270 gate cart: pairs `STATE_CONFIG` with a cart that actually drives the
/// `S` proxy the registration builds. It allocs slots in both declared buffers
/// and writes/reads a spread of fields across every type tag, so the metatable
/// `__index`/`__newindex` closures and per-slot row tables that
/// `register_s_proxy` emits are all live and touched (not merely constructed).
/// Then, as every other cart here, it allocates a deterministic `KEEP` string
/// spread for arena churn, forces a full collection, and reads `cart_allocations`
/// (`guest_heap_used`). The registration allocations are anchored live in `S`
/// and `blyt.buf`, so they survive the collection and land in the count — meaning
/// any divergence in the state/S-proxy registration ORDER or SIZING between the
/// two legs moves this number. The strings stay under the `luaL_Buffer` threshold
/// (see `HEAP_LUA`) so the count is free of the boxed-external-string residual.
const STATE_LUA: &str = r#"
local KEEP = {}

function init()
    -- Types buffer: touch every declared field once so every accessor closure
    -- the S proxy generated is exercised, across all nine type tags.
    for _ = 1, 2 do
        blyt.buf.alloc_slot(S.TYPES)
    end
    S.types[0].v_i8 = -3
    S.types[0].v_u8 = 200
    S.types[0].v_i16 = -1000
    S.types[0].v_u16 = 60000
    S.types[0].v_i32 = 123456
    S.types[0].v_u32 = 2000000000
    S.types[0].v_f32 = 1.5
    S.types[0].v_bool = true
    S.types[0].v_f64 = 3.25
    S.types[1].v_i32 = S.types[0].v_i32 + 1

    -- Entity buffer: multiple slots, so register_s_proxy emitted several row
    -- tables and a second metatable pair.
    for i = 0, 3 do
        blyt.buf.alloc_slot(S.ENTITY)
        S.entity[i].hp = (i + 1) * 10
        S.entity[i].x = i + 0.5
    end

    -- Deterministic heap spread (under the luaL_Buffer threshold) so the arena
    -- has real churn to refract a registration reorder through, kept live so a
    -- full collection cannot reclaim it.
    for i = 1, 40 do
        KEEP[#KEEP + 1] = string.rep("s", 100 + i)
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

/// The #267 target, stated at the level of the defect it closes: `guest_heap_used`
/// is byte-exact across the host-Lua legs on BOTH sides of the `luaL_Buffer`
/// aux-buffer threshold — for plain long strings AND for the external strings
/// that crossing it produces.
///
/// Why this deserves its own test rather than riding the gate above: Lua 5.5
/// gives long strings three layouts, sized separately by `luaS_sizelngstr`
/// (`LSTRREG` inline content; `LSTRFIX` a bare header over borrowed bytes;
/// `LSTRMEM` external bytes with `falloc`/`ud` live). The seam originally
/// modelled all three with the `LSTRREG` formula on the premise that pure Lua
/// never makes an external string — false, because `luaL_pushresult` converts a
/// boxed buffer via `lua_pushexternalstring`. Every boxed string was therefore
/// over-counted by the host−rv32 width of `falloc`+`ud`.
///
/// That defect was invisible to every existing test: `HEAP_LUA` stays under the
/// threshold, so it only ever exercised `LSTRREG`, which was correct. Walking the
/// boundary is what makes the omission fail loudly — the `>= 513` half of this
/// test is red against a kind-blind model, and the `<= 512` half pins the
/// `LSTRREG` path that must NOT regress while fixing it.
///
/// Deliberately asserts cross-leg EQUALITY, never a literal byte count: the
/// contract is that the legs agree, not that they agree on a particular number.
/// Pinning a magic value would turn every Lua bump into a false failure.
#[test]
fn lua_guest_heap_used_matches_wasm32_across_aux_buffer_threshold() {
    require_sdk();
    require_lua_sdk();
    require_wasm();

    for (kind, lengths) in [
        ("inline/LSTRREG", LEN_PROBE_INLINE),
        ("boxed-external/LSTRMEM", LEN_PROBE_EXTERNAL),
    ] {
        for &len in lengths {
            let tmp = TempDir::new().unwrap();
            let project = tmp.path().join(format!("hostlua_heap_len_{len}"));
            CartProject::new()
                .lua(&LEN_PROBE_LUA.replace("LEN", &len.to_string()))
                .write(&project);

            let cart = build_cart(&project);
            let wasm = heap_used(&capture_cart_wasm(&cart, &[]));
            let hostlua = heap_used(&capture_cart_native(&cart, &[]));

            assert_eq!(
                hostlua,
                wasm,
                "native host-Lua guest_heap_used must equal its wasm32 sibling for \
                 {kind} strings of length {len} (#267): a mismatch on the >= 513 side \
                 means the seam is modelling external strings (LSTRMEM, whose TString \
                 keeps falloc/ud live) with the plain-long-string LSTRREG formula; a \
                 mismatch on the <= 512 side means the LSTRREG path itself regressed \
                 (native={hostlua}, wasm32={wasm}, delta={})",
                hostlua as i64 - wasm as i64
            );
        }
    }
}

/// The #242 target: `guest_heap_used` is byte-exact across the host-Lua legs for
/// the **execution-model-sensitive** constructs too — short-string interning,
/// `luaL_Buffer`-boxed strings, closures with open upvalues, and coroutine
/// threads — not just the #231 seam's dominant Table/TString headers.
///
/// This is what closes the documented residual and, with it, the "never branch on
/// the exact cart_allocations value" caveat in `runtime/guest/include/blyt.h`:
/// once the native and wasm runners are ONE shared coroutine-driven runner, the
/// allocation sequence is identical by construction rather than by hand-mirroring
/// two implementations, so there is nothing left to diverge.
///
/// The runner half of this is DONE (#242): both legs now execute the cart through
/// the one shared coroutine driver (`runtime/shared/blyt_hostlua_driver.h`), which
/// took the residual 320 B -> 160 B and made coroutine threads byte-exact.
///
/// Still RED on the remainder, which turned out NOT to be execution-model at all
/// but a SIZING hole in the #231 seam (#267): strings past the `luaL_Buffer`
/// threshold become external strings, whose `TString` carries three pointers
/// (`contents`/`falloc`/`ud`) that are 24 B on a 64-bit host vs 12 B on rv32.
/// Bisected: boxed strings +144, interning +16, closures -16, threads 0.
///
/// Closed by #267, which resolved the remainder into two distinct defects — see
/// this module's header for the full account.
#[test]
fn lua_guest_heap_used_matches_wasm32_for_exec_model_constructs() {
    require_sdk();
    require_lua_sdk();
    require_wasm();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hostlua_heap_residual");
    CartProject::new().lua(RESIDUAL_LUA).write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let wasm = heap_used(&capture_cart_wasm(&cart, &[]));
    let hostlua = heap_used(&capture_cart_native(&cart, &[]));

    assert_eq!(
        hostlua,
        wasm,
        "native host-Lua guest_heap_used must equal its wasm32 sibling for the \
         execution-model-sensitive constructs (#242 TARGET): interning-heavy \
         tables, luaL_Buffer-boxed strings, open-upvalue closures and coroutine \
         threads. A mismatch here means the two legs are still executing the cart \
         through different runners (native direct-C vs wasm co_body coroutine), \
         which is exactly the residual the runner unification removes \
         (native={hostlua}, wasm32={wasm}, delta={})",
        hostlua as i64 - wasm as i64
    );
}

/// #262: the reverse-trampoline exchange-thread pool must not perturb
/// `cart_allocations` across the host-Lua legs.  A hybrid whose native half
/// re-enters Lua (which allocates, then re-enters native) exercises the pool and
/// the nested-call path; the count must stay byte-identical native-host-Lua vs
/// wasm32.  The #267 lesson applies directly: allocation ORDER through the
/// first-fit arena is in the ADR-0029 contract, so a divergence here — e.g. the
/// two legs creating the pool differently, or the reentry allocating out of
/// order — would be a real determinism break, not a cosmetic one.
#[test]
fn reentrant_reverse_trampoline_heap_parity_across_host_lua_legs() {
    require_sdk();
    require_lua_sdk();
    require_wasm();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hostlua_heap_reentrant");
    CartProject::new()
        .c(r#"#include "blyt.h"
BLYT_LUA_MODULE_EXPORT_RAW(host, inner) {
    lua_Integer x = lua_tointeger(L, 1);
    lua_pushinteger(L, x + 1);
    return 1;
}
BLYT_LUA_MODULE_EXPORT_RAW(host, outer) {
    lua_getglobal(L, "middle");
    lua_pushinteger(L, 1);
    lua_pcall(L, 1, 1, 0); /* middle -> host.inner: native->Lua->native */
    return 1;
}
"#)
        .lua(
            r#"
local host = require("host")
local KEEP = {}
function middle(n)
    -- allocate INSIDE the nested Lua frame so the reentry path touches the arena
    local t = {}
    for j = 1, 24 do
        t[j] = j
    end
    KEEP[#KEEP + 1] = t
    return host.inner(n) + n
end
function init()
    for _ = 1, 40 do
        host.outer()
        KEEP[#KEEP + 1] = string.rep("z", 120 + #KEEP)
    end
    collectgarbage("collect")
    local m = blyt32.mem.stats()
    blyt.debug.print(string.format("HEAP used=%d", m.cart_allocations))
end
function update() blyt.quit() end
function draw() end
"#,
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    let wasm = heap_used(&capture_cart_wasm(&cart, &[]));
    let hostlua = heap_used(&capture_cart_native(&cart, &[("BLYT_HOSTLUA", "1")]));
    assert_eq!(
        hostlua,
        wasm,
        "reverse-trampoline hybrid cart_allocations must be byte-identical across \
         the host-Lua legs (#262/#267): the exchange-thread pool and the nested \
         native->Lua->native allocation ORDER must not diverge native-host-Lua vs \
         wasm32 (native={hostlua}, wasm32={wasm}, delta={})",
        hostlua as i64 - wasm as i64
    );
}

/// The #270 target: `guest_heap_used` is byte-exact across the host-Lua legs for
/// a cart that declares state layouts — the one path the other gate carts here
/// leave entirely uncovered.
///
/// Every other cart in this file is built with no `config_yaml`, so it declares
/// no state buffers; with no layout `hl_active_ctx()` / `active_state_ctx()` is
/// NULL, `build_vm` never arms `api.register_state_api` / `api.register_s_proxy`,
/// and the two hooks in the shared canonical sequence
/// (`blyt_hostlua_api.h:358-361`) never fire. That pair is the ONE registration
/// #267 deliberately left hand-mirrored per leg (native drives a
/// `blyt_state_ctx_t`, wasm a `blyt_session_t`) rather than folding into the
/// shared header — so it is exactly the shape of the original #267 defect (two
/// hand-written sequences that agreed by accident until a differing order
/// refracted through the first-fit arena into a ±16 B `cart_allocations`
/// divergence), and nothing gated it.
///
/// This cart declares layouts (so both hooks arm) and drives the resulting `S`
/// proxy across all nine field type tags and multiple slots, then asserts the
/// two legs' `cart_allocations` are byte-identical. It is NOT expected to be red
/// today — the issue confirms a state-buffer cart is already byte-exact; this is
/// the guard that keeps the state/S-proxy registration order and sizing from
/// silently drifting apart in future. Teeth confirmed by locally reordering one
/// leg's registration (swap `register_state_api`/`register_s_proxy` in
/// `build_vm`, or reorder the buffers inside `register_s_proxy`) and watching it
/// go red.
#[test]
fn lua_guest_heap_used_matches_wasm32_with_state_buffers() {
    require_sdk();
    require_lua_sdk();
    require_wasm();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hostlua_heap_state");
    CartProject::new()
        .config(STATE_CONFIG)
        .lua(STATE_LUA)
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let wasm = heap_used(&capture_cart_wasm(&cart, &[]));
    let hostlua = heap_used(&capture_cart_native(&cart, &[]));

    assert_eq!(
        hostlua,
        wasm,
        "native host-Lua guest_heap_used must equal its wasm32 sibling for a cart \
         that declares state layouts (#270): a mismatch means the two legs' \
         hand-mirrored state_api / S-proxy registration sequences have drifted in \
         order or sizing — the registration allocations refract through the \
         first-fit arena, so a differing order moves cart_allocations and breaks \
         the ADR-0029 determinism tier (native={hostlua}, wasm32={wasm}, delta={})",
        hostlua as i64 - wasm as i64
    );
}
