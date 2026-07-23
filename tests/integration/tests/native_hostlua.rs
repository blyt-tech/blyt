//! Native host-Lua non-FP parity smoke (#225 / Spike Z, Q5).
//!
//! Q1–Q3 prove the native host-Lua leg's *floating-point* results match the
//! SoftFloat reference. But host-Lua-everywhere rides on more than FP: integer
//! (i32) overflow/wrap, bitwise ops, integer div/mod (Lua floor semantics),
//! string interning + byte ops, and — the sharp one — **table iteration order**,
//! which depends on the Lua string-hash seed. These are proven on WASM host-Lua
//! by the wider suite but had never run on *native* host-Lua.
//!
//! This is the enumerate-and-smoke pass the spike calls for: a pure
//! integer/string cart (NO f64, so it is independent of the FP gate and Phase B)
//! folded to an i32 FNV-1a digest, asserted identical on the native leg AND
//! across the emulated/wasm/libretro legs. It already earned its keep: it caught
//! a native-only divergence — the native VM was using the default time()-based
//! `luai_makeseed`, randomizing table iteration order — now fixed by pinning the
//! seed to 0x424C5954 exactly as the WASM/guest builds do (see
//! frontends/native-hostlua/CMakeLists.txt). A shipped native player must carry
//! the same pin.
//!
//! Still follow-on (needs the full runtime the minimal FP leg deliberately omits;
//! enumerated here, not yet smoked natively): GC step timing/order, the
//! surface/gfx fast-path rasterization, `guest_heap_used` byte-accounting (#158),
//! and state-buffer NaN canonicalization (`blyt_canon_f64`, ADR-0010) at the
//! save boundary. The FP legs already exercise NaN *values*; confirming the
//! boundary canonicalization on a state-buffer-wired native leg is Q5 follow-on.

mod common;

use common::{
    CartProject, build_lua_cart, require_hostlua_native, require_libretro_core, require_lua_sdk,
    require_sdk, require_wasm, run_cart_all_legs_exact, run_cart_native_hostlua,
};
use tempfile::TempDir;

/// Pure integer/string cart — no f64. Only a non-FP semantic divergence (integer
/// wrap, bitwise, string interning, or table iteration order) can move the digest.
const NONFP_CART: &str = r#"
-- Q5 non-FP parity smoke (#225). Folds the i32 integer / bitwise / string /
-- table-iteration surface into an i32 FNV-1a digest. Integer hashing is
-- bit-deterministic, so only a non-FP divergence moves the digest.
local FNV_OFFSET = 0x811c9dc5
local FNV_PRIME = 0x01000193
local hash = FNV_OFFSET
local function fold_byte(b)
    hash = (hash ~ (b & 0xff)) * FNV_PRIME
end
local function fold_i32(x)
    fold_byte(x & 0xff)
    fold_byte((x >> 8) & 0xff)
    fold_byte((x >> 16) & 0xff)
    fold_byte((x >> 24) & 0xff)
end
local function fold_str(s)
    for i = 1, #s do
        fold_byte(s:byte(i))
    end
    fold_byte(0)
end

local function run()
    -- integer (i32) overflow / wrap. lua_Integer is 32-bit (BLYT_LUA_I32_F64);
    -- decimal literals that overflow become floats, so wrapping constants use
    -- hex (which wraps to a valid integer) — e.g. 0x9e3779b1.
    local mx = math.maxinteger
    fold_i32(mx)
    fold_i32(math.mininteger)
    fold_i32(mx + 1)
    fold_i32(mx * 2)
    fold_i32(mx * mx)
    fold_i32(-mx - 2)
    for i = 1, 40 do
        fold_i32((i * 0x9e3779b1) & 0xffffffff)
    end
    -- bitwise
    fold_i32(0xdeadbeef & 0x0f0f0f0f)
    fold_i32(0x12345678 | 0x87654321)
    fold_i32(0xabcd ~ 0x1234)
    fold_i32(1 << 30)
    fold_i32(mx >> 3)
    fold_i32(~0)
    -- integer div / mod (Lua floor semantics for negatives)
    fold_i32(1000000007 // 7)
    fold_i32(1000000007 % 7)
    fold_i32(-17 // 5)
    fold_i32(-17 % 5)
    -- string interning + byte ops
    local parts = {}
    for i = 1, 32 do
        parts[i] = string.char((i * 37) % 256)
    end
    local s = table.concat(parts)
    fold_str(s)
    fold_str(string.format("%d|%x|%08x", mx, 0xcafef00d & 0xffffffff, 42))
    fold_str(("hello world"):rep(3):upper():sub(2, 20))
    fold_i32(#s)
    -- table iteration order: depends on the string-hash seed, which must be
    -- pinned identically on every leg (the divergence this test first caught).
    local t = {}
    t.alpha, t.beta, t.gamma, t.delta = 1, 2, 3, 4
    t[100], t[7], t["a key with spaces"], t.z = 5, 6, 7, 8
    local order = {}
    for k, v in pairs(t) do
        order[#order + 1] = tostring(k) .. "=" .. v
    end
    for i = 1, #order do
        fold_str(order[i])
    end
    local seen = {}
    for i = 1, 50 do
        local key = "k" .. (i % 13)
        seen[key] = (seen[key] or 0) + 1
    end
    local ks = {}
    for k in pairs(seen) do
        ks[#ks + 1] = k
    end
    table.sort(ks)
    for i = 1, #ks do
        fold_str(ks[i] .. ":" .. seen[ks[i]])
    end
end

local function digest_hex()
    local b0, b1, b2, b3 = hash & 0xff, (hash >> 8) & 0xff, (hash >> 16) & 0xff, (hash >> 24) & 0xff
    return string.format("%02x%02x%02x%02x", b3, b2, b1, b0)
end

function init()
    run()
    blyt.debug.print("<m:[blyt:nonfphash] " .. digest_hex() .. ">")
end
function update()
    blyt.quit()
end
function draw() end
"#;

/// The pinned reference digest for the non-FP surface. Regenerate by running any
/// leg and copying the emitted `[blyt:nonfphash]` value.
const NONFP_DIGEST: &str = "[blyt:nonfphash] ff5ba48c";

/// Q5: the native host-Lua leg's non-FP surface (integer/bitwise/string/table
/// iteration) must match the emulated softfloat reference — and the reference is
/// cross-validated by asserting the same digest on emulated/wasm/libretro too.
#[test]
fn native_hostlua_nonfp_parity() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    require_libretro_core();
    require_hostlua_native();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("nonfp");
    CartProject::new().lua(NONFP_CART).write(&project);
    let cart = build_lua_cart(&project);

    // Emulated / WASM / libretro all agree on the reference…
    run_cart_all_legs_exact(&cart, "m", &[NONFP_DIGEST]);
    // …and the native host-Lua leg reproduces it bit-for-bit.
    run_cart_native_hostlua(&cart, NONFP_DIGEST);
}
