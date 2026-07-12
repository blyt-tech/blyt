//! Host-Lua floating-point determinism parity gate (#223 AC1, ADR-0135/0136).
//!
//! After #236, a pure-Lua cart runs on the host-Lua path (the Lua VM compiled
//! natively for the host) on every non-RISC-V host — the emulated RV32 Lua VM is
//! retired as a shipped path AND as the determinism oracle (ADR-0136). The
//! reference is now the softfloat-correct answer pinned as a **golden** digest:
//! IEEE-754 correctly-rounded ops + `-ffp-contract=off` (no FMA) + RISC-V NaN
//! canonicalization ⇒ native f64 == the softfloat reference by construction
//! (ADR-0136). Determinism is the core contract (ADR-0007); netplay/replay/rewind
//! depend on it.
//!
//! This gate pins that contract from four independent angles: (1) **cross-host-Lua-leg
//! agreement** — `run_cart_all_legs` asserts the same `[blyt:fphash]` on blytplay,
//! wasm, and the libretro core, all host-Lua; (2) the **pinned golden** below;
//! (3) the **contraction-torture** control (`fp_native_hostlua_contraction_teeth`)
//! which proves the gate would catch an FMA-induced divergence; and (4) an
//! **independent softfloat implementation** — the RV32 guest-lib Lua running
//! natively on real RISC-V via the QEMU native gate (`native_qemu.rs`), which
//! reproduces the same golden. The cart (no typed exports, no state layouts)
//! evaluates the cart-reachable transcendental + number-conversion surface over an
//! adversarial corpus and folds the RAW f64 bit patterns of every result into an
//! i32 FNV-1a digest. Integer hashing is bit-deterministic across every leg, so the
//! ONLY thing that can move the digest is a divergent f64 bit pattern.
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

use common::fp::{FP_PARITY_CART, FP_PARITY_DIGEST};

use common::{
    CartProject, build_dir, build_lua_cart, hostlua_native, hostlua_native_fma,
    require_hostlua_native, require_hostlua_native_fma, require_libretro_core, require_lua_sdk,
    require_sdk, require_wasm, run_cart_all_legs, run_cart_native_hostlua,
};
use std::path::{Path, PathBuf};
use tempfile::TempDir;

/// The CORE reference digest (#225 / Spike Z): the transcendental + Zone-1
/// surface only, excluding the Phase-B number-format surface. This is the value
/// the native host-Lua leg must reproduce bit-for-bit on FMA silicon — the
/// determinism gate for host-Lua-everywhere. Regenerate alongside
/// `FP_PARITY_DIGEST` if the corpus changes.
const FP_PARITY_CORE_DIGEST: &str = "[blyt:fphash-core] 031a4987";

/// The host-Lua path must compute `math.*`, `^`, and number↔string conversion
/// bit-identically to the softfloat-correct golden across an adversarial FP corpus
/// (ADR-0135 contract). After #236 all three legs — blytplay, WASM, and the
/// libretro core — run host-Lua, so asserting the same digest on every leg is the
/// cross-host-Lua-leg agreement check; the golden pins the softfloat answer and the
/// QEMU native gate re-derives it on an independent RISC-V softfloat.
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

/// Spike Z / #225 (Q4 — Phase B, strtod / number-format): the native host-Lua
/// leg must reproduce the **FULL** parity digest — the core transcendental +
/// Zone-1 surface *plus* the number↔string conversion surface (`tostring` /
/// `tonumber` / `string.format`) — bit-identically to the emulated softfloat
/// reference across x86-64 / arm64.
///
/// The difference from [`fp_native_hostlua_core_parity`] is the conversion
/// surface. Before Phase B it resolved to the host toolchain's `strtod` /
/// `snprintf` — a coincidental, unpinned agreement (it happens to match on the
/// dev host's libc). Phase B routes `lua_str2number` / `l_sprintf` through the
/// pinned in-house musl `strtod` + `vfprintf` subset (`blyt_fpm_strtod` /
/// `blyt_fpm_snprintf`), so this digest is now pinned to the same conversion
/// implementation the emulated reference uses. Together with
/// [`fp_native_hostlua_conversions_hermetic`] (which proves the conversions are
/// actually routed through the seam, not the host libc) this closes Q4.
#[test]
fn fp_native_hostlua_conversions_parity() {
    require_sdk();
    require_lua_sdk();
    require_hostlua_native();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("fp_parity");
    CartProject::new().lua(FP_PARITY_CART).write(&project);
    let cart = build_lua_cart(&project);

    run_cart_native_hostlua(&cart, FP_PARITY_DIGEST);
}

/// Spike Z / #225 (Q4 — Phase B hermeticity): prove the native host-Lua leg's
/// number↔string conversions actually reach the pinned in-house musl subset,
/// never the host toolchain's `strtod` / `snprintf`. Without this,
/// [`fp_native_hostlua_conversions_parity`] passing proves nothing about
/// hermeticity — the dev host's libc `strtod`/`snprintf` happen to agree with
/// the reference, so a digest match alone cannot distinguish "pinned to musl"
/// from "using an unpinned host libc that coincidentally matches".
///
/// Proven at the symbol level (mirrors
/// [`fp_seam_hermetic_no_host_libm_transcendentals`], but for the Phase-B
/// conversion surface): the compiled native VM object (`onelua.c.o`, the whole
/// Lua fork) must import the `blyt_fpm_strtod` / `blyt_fpm_snprintf` seam
/// entries (routing engaged) and must NOT import bare `strtod` / `snprintf`
/// (which would be the host libc). `l_sprintf` is Lua's sole float/integer
/// formatting choke point and `lua_str2number` its sole string→number one, so
/// after the seam edit neither host symbol should remain undefined in the object.
#[test]
fn fp_native_hostlua_conversions_hermetic() {
    require_sdk();
    require_lua_sdk();
    require_hostlua_native();

    // The native VM object: the Lua fork compiled with the seam engaged. Its
    // path mirrors the source under the blyt_hostlua_native_vm target's dir.
    let vm_dir = build_dir().join("frontends/native-hostlua/CMakeFiles/blyt_hostlua_native_vm.dir");
    let obj = find_file_named(&vm_dir, "onelua.c.o").unwrap_or_else(|| {
        panic!(
            "onelua.c.o not found under {} — build blyt_hostlua_native first",
            vm_dir.display()
        )
    });

    // Undefined (imported) symbols. `nm` prints Mach-O names with a leading
    // underscore (`_strtod`) and ELF names without (`strtod`); normalize by
    // stripping one leading underscore so the assertions are arch-portable.
    let out = std::process::Command::new("nm")
        .arg(&obj)
        .output()
        .expect("failed to run nm");
    assert!(out.status.success(), "nm failed on {}", obj.display());
    let nm = String::from_utf8_lossy(&out.stdout);
    let undefined: Vec<String> = nm
        .lines()
        .filter_map(|l| {
            let mut it = l.split_whitespace();
            match (it.next(), it.next()) {
                (Some("U"), Some(sym)) => Some(sym.strip_prefix('_').unwrap_or(sym).to_string()),
                _ => None,
            }
        })
        .collect();

    // Routing engaged: the pinned seam entries are imported.
    for fpm in ["blyt_fpm_strtod", "blyt_fpm_snprintf"] {
        assert!(
            undefined.iter().any(|s| s == fpm),
            "expected onelua.c.o to import {fpm} (Phase-B seam not engaged?); imports: {undefined:?}"
        );
    }

    // Hermetic: no bare host-libc conversion symbol is imported.
    for host in ["strtod", "snprintf"] {
        assert!(
            !undefined.iter().any(|s| s == host),
            "onelua.c.o imports host libc `{host}` — number conversion not routed through the \
             Phase-B seam (imports: {undefined:?})"
        );
    }
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

/// Collect every file named `name` under `root` (depth-first), newest first by
/// mtime. Used to pick the *active* build object when a stale copy from an
/// earlier configure (e.g. a `_deps/lua-src` object left over before a
/// `third_party/lua` override) may also linger in the tree.
fn find_files_named_newest_first(root: &Path, name: &str) -> Vec<PathBuf> {
    let mut hits = Vec::new();
    fn walk(dir: &Path, name: &str, out: &mut Vec<PathBuf>) {
        let Ok(entries) = std::fs::read_dir(dir) else {
            return;
        };
        for e in entries.flatten() {
            let p = e.path();
            if p.is_dir() {
                walk(&p, name, out);
            } else if p.file_name().and_then(|n| n.to_str()) == Some(name) {
                out.push(p);
            }
        }
    }
    walk(root, name, &mut hits);
    hits.sort_by_key(|p| {
        std::cmp::Reverse(
            std::fs::metadata(p)
                .and_then(|m| m.modified())
                .unwrap_or(std::time::UNIX_EPOCH),
        )
    });
    hits
}

/// Spike Z / #225 (Q4 — Phase B hermeticity, WASM half): the WASM host-Lua
/// fast path's number↔string conversions must reach the pinned in-house musl
/// subset, never Emscripten's module libc `strtod` / `snprintf`. This is the
/// WASM companion to [`fp_native_hostlua_conversions_hermetic`] — Phase B's WASM
/// half hardens the shipping host-Lua path (blytplay/wasm) regardless of the
/// strategic host-Lua-everywhere decision.
///
/// `lobject.c.o` is where `tostringbuffFloat` lives (it calls both
/// `lua_str2number` for the readback and `l_sprintf` for the format), so after
/// the seam edit it must import `blyt_fpm_strtod` / `blyt_fpm_snprintf` and must
/// NOT import bare `strtod` / `snprintf`. Uses the newest object so a stale
/// pre-override `_deps/lua-src` copy cannot shadow the active `third_party/lua`
/// build locally (CI builds a single copy from the pinned fork tag).
#[test]
fn fp_seam_hermetic_no_host_libc_conversions() {
    require_sdk();
    require_wasm();

    let wasm_tree = build_dir().join("build-wasm");
    let obj = find_files_named_newest_first(&wasm_tree, "lobject.c.o")
        .into_iter()
        .next()
        .unwrap_or_else(|| {
            panic!(
                "lobject.c.o not found under {} — build the WASM runtime (sdk target) first",
                wasm_tree.display()
            )
        });

    let out = std::process::Command::new(emnm())
        .arg(&obj)
        .output()
        .expect("failed to run emnm");
    assert!(out.status.success(), "emnm failed on {}", obj.display());
    let nm = String::from_utf8_lossy(&out.stdout);
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

    // Routing engaged: the pinned conversion seam entries are imported.
    for fpm in ["blyt_fpm_strtod", "blyt_fpm_snprintf"] {
        assert!(
            undefined.contains(&fpm),
            "expected lobject.c.o to import {fpm} (Phase-B seam not engaged on WASM?); \
             imports: {undefined:?}"
        );
    }

    // Hermetic: no bare Emscripten-libc conversion symbol is imported.
    for host in ["strtod", "snprintf"] {
        assert!(
            !undefined.contains(&host),
            "lobject.c.o imports host libc `{host}` — number conversion not routed through the \
             Phase-B seam on WASM (imports: {undefined:?})"
        );
    }
}
