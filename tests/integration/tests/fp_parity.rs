//! Host-Lua floating-point determinism parity gate (#223 AC1, ADR-0135).
//!
//! Lua carts run two ways: the emulated/native-metal path (RV32 Lua VM under
//! rv32emu, or native on RISC-V) and the host-Lua fast path (Lua VM compiled
//! natively for the host — WASM today). The emulated path is a tight softfloat
//! reference (blyt-tech musl generic-C `src/math` + Berkeley SoftFloat with the
//! RISC-V NaN specialization + SOFTFLOAT_ROUND_ODD, `-ffp-contract=off`). The
//! host-Lua fast path must reproduce that reference **bit-for-bit** — determinism
//! is the core contract (ADR-0007); netplay/replay/rewind depend on it.
//!
//! This gate is the oracle for that contract: a pure-Lua cart (no typed exports,
//! no state layouts, so WASM routes it to the host-Lua fast path while
//! native/libretro run the emulated VM) evaluates the cart-reachable
//! transcendental + number-conversion surface over an adversarial corpus and
//! folds the RAW f64 bit patterns of every result into an i32 FNV-1a digest.
//! Integer hashing is bit-deterministic across every leg regardless of the FP
//! result, so the ONLY thing that can move the digest is a divergent f64 bit
//! pattern. `run_cart_all_legs` asserts the same `[blyt:fphash]` on all three
//! legs — so a divergence between the host-Lua fast path and the softfloat
//! reference fails here by construction.
//!
//! To regenerate the expected digests after editing the corpus, run any leg
//! (e.g. `blytplay --headless <cart>`) and copy the emitted `[blyt:fphash]`
//! (full) and `[blyt:fphash-core]` (transcendental + Zone-1 only) values.
//!
//! The CORE digest (`[blyt:fphash-core]`) is the Spike Z / #225 native
//! cross-arch gate: the native host-Lua leg (the Lua fork compiled for the host,
//! blyt_fpm seam engaged, `-ffp-contract=off`) must reproduce it bit-for-bit on
//! FMA silicon (x86-64/arm64). It excludes the Phase-B number-format surface so
//! a host-libc `strtod` difference cannot masquerade as a seam divergence.

mod common;

use common::{
    CartProject, build_dir, build_lua_cart, hostlua_native, hostlua_native_fma,
    require_hostlua_native, require_hostlua_native_fma, require_libretro_core, require_lua_sdk,
    require_sdk, require_wasm, run_cart_all_legs, run_cart_native_hostlua,
};
use std::path::{Path, PathBuf};
use tempfile::TempDir;

/// The parity cart. Pure Lua, no exports/layouts → host-Lua fast path on WASM.
/// See the module doc for what it proves. Kept in one place so the corpus and the
/// pinned digest below stay in sync.
const FP_PARITY_CART: &str = r#"
-- FP parity gate (#223 AC1). Folds the raw f64 bit patterns of the
-- cart-reachable Zone-2 (transcendental) + number-conversion surface over an
-- adversarial corpus into an i32 FNV-1a digest. Only a divergent f64 bit pattern
-- can move the digest (integer hashing is bit-deterministic across legs).

-- ---- i32 FNV-1a over bytes -------------------------------------------------
-- 0x811C9DC5 wraps into i32 as a negative value; the wrap is identical on every
-- leg. 0x01000193 (16777619) fits i32. Integer * wraps mod 2^32 (two's comp).
local FNV_OFFSET = 0x811c9dc5
local FNV_PRIME = 0x01000193

local hash = FNV_OFFSET
-- Snapshot of `hash` taken after the transcendental + Zone-1 surface and before
-- the Phase-B number-format surface (see run_corpus): the #225 cross-arch gate
-- value. Defaults to the full hash so a leg that never calls run_corpus still
-- prints a well-defined line.
local core_hash = FNV_OFFSET

local function fold_byte(b)
    hash = (hash ~ b) * FNV_PRIME
end

-- Fold every byte of an arbitrary (binary-safe) string into the digest.
local function fold_str(s)
    for i = 1, #s do
        fold_byte(s:byte(i))
    end
    -- length terminator so "ab".."" and "a".."b" boundaries can't collide
    fold_byte(0)
end

-- ADR-0010: a NaN's sign and payload are NOT part of the determinism contract —
-- they are canonicalized at the state-buffer boundary (blyt_canon_f64). WASM
-- makes an arithmetically-produced NaN's sign bit nondeterministic (x86-64
-- yields fff8..., arm64 / the softfloat reference yield the RISC-V canonical
-- 7ff8...), so hashing the raw transient NaN would test something the console
-- deliberately does not pin. Canonicalize every NaN to 0x7ff8000000000000 before
-- it enters the digest, exactly as the state-buffer boundary does; finite results
-- (where the seam's guarantee actually lives) fold through unchanged.
local CANON_NAN = string.unpack("<d", "\0\0\0\0\0\0\248\127") -- 0x7ff8000000000000
local function canon(x)
    if x ~= x then -- x ~= x is true iff x is NaN, regardless of sign/payload
        return CANON_NAN
    end
    return x
end

-- Fold a double's raw little-endian IEEE-754 bits (NaN canonicalized per above).
-- string.pack("<d") writes the exact bit pattern the VM holds.
local function fold_f64(x)
    fold_str(string.pack("<d", canon(x)))
end

-- ---- human-readable spot values (localize any divergence) ------------------
local spot = {}
local function packhex(x)
    -- big-endian raw bytes -> 16 hex nibbles
    local b = string.pack(">d", x)
    local out = {}
    for i = 1, 8 do
        out[i] = string.format("%02x", b:byte(i))
    end
    return table.concat(out)
end
local function spotval(name, x)
    spot[#spot + 1] = name .. "=" .. packhex(canon(x))
end

-- ---- adversarial corpus ----------------------------------------------------
-- Compile-time literals: identical bytecode on every leg (luac runs once). The
-- divergence under test is the RUNTIME evaluation of math.*/^ on these inputs.
local pi = math.pi
local inf = math.huge
local nan = inf - inf
local inputs = {
    0.0,
    -0.0,
    1.0,
    -1.0,
    0.5,
    -0.5,
    2.0,
    -2.0,
    3.0,
    10.0,
    100.0,
    0.1,
    0.2,
    0.3,
    0.75,
    -0.75,
    pi,
    pi / 2,
    pi / 4,
    pi * 2,
    pi * 1000000.0, -- near-multiple-of-pi argument reduction stress
    355.0 / 113.0,
    1e-1,
    1e-300,
    2.2250738585072014e-308, -- min normal
    5e-324, -- min positive subnormal
    1e-320, -- a subnormal
    1e300,
    1.7976931348623157e308, -- DBL_MAX
    1e308,
    inf,
    -inf,
    nan,
}

-- Unary functions. asin/acos/log/sqrt applied to out-of-domain inputs yield NaN
-- / +-Inf on purpose: NaN-payload and infinity bit patterns are exactly what the
-- softfloat RISC-V specialization pins and where a foreign libm can diverge.
local unary = {
    { "sin", math.sin },
    { "cos", math.cos },
    { "tan", math.tan },
    { "asin", math.asin },
    { "acos", math.acos },
    { "atan", math.atan },
    { "exp", math.exp },
    { "log", math.log },
    { "sqrt", math.sqrt }, -- Zone-1 (IEEE sqrt) but included for completeness
}

-- Binary. atan2 == math.atan(y, x); pow == y ^ x (Lua `^` -> luai_numpow, the
-- Zone-2 op ADR-0135 routes through the seam); fmod is Zone-1.
local pairs_yx = {}
do
    local sample = { -3.0, -1.0, -0.5, 0.0, 0.5, 1.0, 2.0, 3.0, 10.0, pi, 0.1, inf, -inf, nan }
    for i = 1, #sample do
        for j = 1, #sample do
            pairs_yx[#pairs_yx + 1] = { sample[i], sample[j] }
        end
    end
end

-- strtod corpus: parsed at RUNTIME via tonumber (exercises l_str2d per leg, not
-- the compile-time lexer).
local strtod_inputs = {
    "0",
    "-0",
    "0.1",
    "0.2",
    "3.141592653589793",
    "2.718281828459045",
    "1e308",
    "1e-308",
    "5e-324",
    "2.2250738585072014e-308",
    "1.7976931348623157e308",
    "0.30000000000000004",
    "123456789.123456789",
    "1e-1",
    "9007199254740993", -- 2^53+1, not exactly representable
    "inf",
    "-inf",
    "nan",
}

local function run_corpus()
    -- Zone-2 unary transcendentals
    for u = 1, #unary do
        local f = unary[u][2]
        for i = 1, #inputs do
            fold_f64(f(inputs[i]))
        end
    end
    -- atan2 (binary atan), pow (^), fmod
    for p = 1, #pairs_yx do
        local y, x = pairs_yx[p][1], pairs_yx[p][2]
        fold_f64(math.atan(y, x))
        fold_f64(y ^ x)
        fold_f64(math.fmod(y, x))
    end
    -- CORE digest snapshot (#225 / Spike Z): everything folded so far is the
    -- Zone-2 transcendental + Zone-1 (sqrt/fmod) surface the ADR-0135 seam pins.
    -- The native host-Lua leg must reproduce this bit-for-bit on FMA silicon
    -- (x86-64/arm64) using ONLY the in-house musl kernels — no host libm, no
    -- strtod/number-format (that surface is Phase B and stays out of the gate,
    -- else a host-libc strtod difference would masquerade as a seam divergence).
    core_hash = hash
    -- number formatting: tostring(x) bytes AND tonumber(tostring(x)) round trip.
    -- canon() first so tostring(NaN) is "nan" on every host (an x86-64 sign-set
    -- NaN would otherwise stringify to "-nan"); finite values are unchanged.
    for i = 1, #inputs do
        local s = tostring(canon(inputs[i]))
        fold_str(s)
        -- Lua's tonumber deliberately rejects "inf"/"nan" (-> nil), identically
        -- on every leg; guard so non-finite round-trips fold a constant.
        fold_f64(tonumber(s) or 0.0)
    end
    -- strtod: tonumber(string)
    for i = 1, #strtod_inputs do
        fold_f64(tonumber(strtod_inputs[i]) or 0.0)
    end

    -- spot values for human divergence localization
    spotval("sin1", math.sin(1.0))
    spotval("cos1", math.cos(1.0))
    spotval("tan1", math.tan(1.0))
    spotval("exp1", math.exp(1.0))
    spotval("log2", math.log(2.0))
    spotval("pow_2_0.5", 2.0 ^ 0.5)
    spotval("atan2_1_1", math.atan(1.0, 1.0))
    spotval("sinbigpi", math.sin(pi * 1000000.0))
    spotval("asin2_nan", math.asin(2.0))
    spotval("sqrtneg_nan", math.sqrt(-1.0))
    spotval("ts_0.1", tonumber(tostring(0.1)))
end

local function digest_hex(h)
    local b0 = h & 0xff
    local b1 = (h >> 8) & 0xff
    local b2 = (h >> 16) & 0xff
    local b3 = (h >> 24) & 0xff
    return string.format("%02x%02x%02x%02x", b3, b2, b1, b0)
end

function init()
    run_corpus()
    -- Core (transcendental + Zone-1) — the #225 native cross-arch gate value.
    blyt.debug.print("[blyt:fphash-core] " .. digest_hex(core_hash))
    -- Full (adds Phase-B number-format) — the #223 Phase-A cross-leg gate value.
    blyt.debug.print("[blyt:fphash] " .. digest_hex(hash))
    for i = 1, #spot do
        blyt.debug.print("[blyt:fpspot] " .. spot[i])
    end
end

function update()
    blyt.quit()
end

function draw() end
"#;

/// The pinned reference digest — the softfloat reference value every leg must
/// reproduce. Regenerate (see module doc) if the corpus above changes.
const FP_PARITY_DIGEST: &str = "[blyt:fphash] f7a69261";

/// The CORE reference digest (#225 / Spike Z): the transcendental + Zone-1
/// surface only, excluding the Phase-B number-format surface. This is the value
/// the native host-Lua leg must reproduce bit-for-bit on FMA silicon — the
/// determinism gate for host-Lua-everywhere. Regenerate alongside
/// `FP_PARITY_DIGEST` if the corpus changes.
const FP_PARITY_CORE_DIGEST: &str = "[blyt:fphash-core] 031a4987";

/// The host-Lua fast path must compute `math.*`, `^`, and number↔string
/// conversion bit-identically to the emulated softfloat reference across an
/// adversarial FP corpus (ADR-0135 contract). Asserting the same digest on
/// native (emulated), WASM (host-Lua fast path), and libretro (emulated) proves
/// the fast path does not diverge.
#[test]
fn fp_transcendental_parity_across_legs() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("fp_parity");
    CartProject::new().lua(FP_PARITY_CART).write(&project);
    let cart = build_lua_cart(&project);

    run_cart_all_legs(&cart, FP_PARITY_DIGEST);
}

/// Spike Z / #225 (Q1 + Q3): the native host-Lua leg — the same Lua fork and
/// cart bytecode as every other leg, but compiled native for the host
/// (x86-64 / arm64) with the ADR-0135 `blyt_fpm` transcendental seam and
/// `-ffp-contract=off` — must compute the transcendental + Zone-1 surface
/// bit-identically to the emulated softfloat reference on **FMA hardware**. This
/// is the claim WASM structurally cannot test (WASM MVP has no scalar FMA).
///
/// The same test binary runs on the arm64 dev host and (via the CI-mirroring
/// `test-linux-docker` container) on x86-64, so a green run on both is the
/// cross-arch host-vs-host determinism gate — an x86-64 desktop and an arm64
/// handheld agreeing to the last bit, which is what netplay/replay need.
///
/// Scoped to the CORE digest (`FP_PARITY_CORE_DIGEST`): the number-format /
/// strtod surface is Phase B (not yet seam-pinned; still resolves to host libc
/// here), so it is deliberately outside this gate — a host strtod difference
/// must not masquerade as a transcendental-seam divergence. Q4 folds
/// conversions in once Phase B lands.
#[test]
fn fp_native_hostlua_core_parity() {
    require_sdk();
    require_lua_sdk();
    require_hostlua_native();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("fp_parity");
    CartProject::new().lua(FP_PARITY_CART).write(&project);
    let cart = build_lua_cart(&project);

    run_cart_native_hostlua(&cart, FP_PARITY_CORE_DIGEST);
}

/// The `-ffp-contract=off` reference for the runner's C-level contraction
/// torture — decomposed multiply-add is IEEE correctly-rounded, so this is
/// stable across arch and compiler. The `-ffp-contract=fast` build must diverge
/// from it on FMA silicon.
const FP_TORTURE_OFF_DIGEST: &str = "d002fa13";

/// Extract the `[blyt:fptorture]` digest a native host-Lua runner emits.
fn hostlua_torture(bin: &Path, cart: &Path) -> String {
    let out = std::process::Command::new(bin)
        .arg(cart)
        .output()
        .unwrap_or_else(|e| panic!("failed to run {}: {e}", bin.display()));
    assert!(out.status.success(), "{} exited nonzero", bin.display());
    let s = String::from_utf8_lossy(&out.stdout);
    s.lines()
        .find_map(|l| l.strip_prefix("[blyt:fptorture] "))
        .unwrap_or_else(|| panic!("no [blyt:fptorture] line in {} output:\n{s}", bin.display()))
        .trim()
        .to_string()
}

/// Spike Z / #225 (Q1 negative control): prove the parity gate has TEETH on FMA
/// silicon. The real host-Lua path (interpreter + musl kernels) is
/// contraction-invariant — a Lua cart's `a*b+c` is two separate rounded VM ops,
/// and the in-house kernels are contraction-safe — so `fp_native_hostlua_core_parity`
/// staying green under `-ffp-contract=fast` proves determinism but not detection.
/// This test flips the flag on a deliberately contraction-prone C Horner /
/// dot-product chain: under `-ffp-contract=off` it is IEEE decomposed
/// (== the pinned reference); under `-ffp-contract=fast` on FMA hardware the
/// compiler fuses it to a single-rounding FMA and the digest MUST move. A green
/// run confirms FMA is present, the flag is honored, and the harness would catch
/// an FMA-induced divergence if one reached the real path.
#[test]
fn fp_native_hostlua_contraction_teeth() {
    require_sdk();
    require_lua_sdk();
    require_hostlua_native();
    require_hostlua_native_fma();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("fp_parity");
    CartProject::new().lua(FP_PARITY_CART).write(&project);
    let cart = build_lua_cart(&project);

    let off = hostlua_torture(&hostlua_native(), &cart);
    let fma = hostlua_torture(&hostlua_native_fma(), &cart);

    assert_eq!(
        off, FP_TORTURE_OFF_DIGEST,
        "contraction-off torture digest drifted (expected {FP_TORTURE_OFF_DIGEST}, got {off})"
    );
    assert_ne!(
        off, fma,
        "FMA-contraction control did not diverge from the off build — the gate has no teeth on \
         this host (no hardware FMA, or -ffp-contract is not being honored)"
    );
}

/// The Zone-2 transcendentals the seam routes (ADR-0135). Zone-1 ops
/// (`sqrt/floor/ceil/fabs/fmod/frexp/ldexp`) are IEEE-exact and legitimately keep
/// the native libm symbol, so they are NOT in this set.
const SEAM_TRANSCENDENTALS: &[&str] = &[
    "sin", "cos", "tan", "asin", "acos", "atan", "atan2", "exp", "log", "log2", "log10",
];

/// Recursively find the first file named `name` under `root` (depth-first).
fn find_file_named(root: &Path, name: &str) -> Option<PathBuf> {
    let entries = std::fs::read_dir(root).ok()?;
    let mut dirs = Vec::new();
    for e in entries.flatten() {
        let p = e.path();
        if p.is_dir() {
            dirs.push(p);
        } else if p.file_name().and_then(|n| n.to_str()) == Some(name) {
            return Some(p);
        }
    }
    for d in dirs {
        if let Some(f) = find_file_named(&d, name) {
            return Some(f);
        }
    }
    None
}

/// Locate `emnm` (Emscripten's `nm`): PATH first, else next to `emcc`.
fn emnm() -> PathBuf {
    if std::process::Command::new("emnm")
        .arg("--version")
        .output()
        .is_ok()
    {
        return PathBuf::from("emnm");
    }
    let emcc = which_emcc().expect("emcc not found — Emscripten is required for the WASM seam");
    emcc.with_file_name("emnm")
}

fn which_emcc() -> Option<PathBuf> {
    let out = std::process::Command::new("which")
        .arg("emcc")
        .output()
        .ok()?;
    if !out.status.success() {
        return None;
    }
    Some(PathBuf::from(
        String::from_utf8_lossy(&out.stdout).trim().to_string(),
    ))
}

/// AC5 (hermetic): the host-Lua VM must reach the pinned in-house musl kernels
/// for its Zone-2 transcendentals, never the host toolchain's libm. Proven at the
/// symbol level: the compiled `lmathlib.c.o` in the WASM host-Lua build must
/// import the `blyt_fpm_*` seam entries (routing engaged) and must NOT import any
/// bare libm transcendental (`sin`/`cos`/`exp`/…). Zone-1 libm symbols
/// (`sqrt`/`floor`/…) may still appear — those are IEEE-exact and native ==
/// softfloat.
///
/// NOTE: this asserts the seam is live, which requires the blyt-tech/lua fork to
/// carry the ADR-0135 edits (BLYT_HOSTLUA_FP_SEAM routing). It passes once the
/// fork pin is at the seam-bearing tag (or a local third_party/lua override is in
/// place); against a pre-seam Lua it fails by design.
#[test]
fn fp_seam_hermetic_no_host_libm_transcendentals() {
    require_sdk();
    require_wasm();

    // The release WASM host-Lua VM objects live under build/build-wasm.
    let wasm_tree = build_dir().join("build-wasm");
    let obj = find_file_named(&wasm_tree, "lmathlib.c.o").unwrap_or_else(|| {
        panic!(
            "lmathlib.c.o not found under {} — build the WASM runtime (sdk target) first",
            wasm_tree.display()
        )
    });

    let out = std::process::Command::new(emnm())
        .arg(&obj)
        .output()
        .expect("failed to run emnm");
    assert!(out.status.success(), "emnm failed on {}", obj.display());
    let nm = String::from_utf8_lossy(&out.stdout);

    // Undefined (imported) symbols: lines like "         U <name>".
    let undefined: Vec<&str> = nm
        .lines()
        .filter_map(|l| {
            let mut it = l.split_whitespace();
            match (it.next(), it.next()) {
                (Some("U"), Some(sym)) => Some(sym),
                _ => None,
            }
        })
        .collect();

    // Routing engaged: the seam entries are imported.
    for fpm in [
        "blyt_fpm_sin",
        "blyt_fpm_cos",
        "blyt_fpm_exp",
        "blyt_fpm_log",
    ] {
        assert!(
            undefined.contains(&fpm),
            "expected lmathlib.c.o to import {fpm} (seam not engaged?); imports: {undefined:?}"
        );
    }

    // Hermetic: no bare libm transcendental is imported.
    for &t in SEAM_TRANSCENDENTALS {
        assert!(
            !undefined.contains(&t),
            "lmathlib.c.o imports host libm `{t}` — Zone-2 transcendental not routed through the seam (imports: {undefined:?})"
        );
    }
}
