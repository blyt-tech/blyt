//! Issue #46: canonical debug source paths.
//!
//! Every path embedded in a cart or shipped debug library — DWARF, Rust panic
//! Location strings, C __FILE__ — must be a machine-independent `/blyt/*`
//! canonical path, never the author's absolute build/HOME paths.  These tests
//! assert that directly on the produced artifacts (no debugger needed), plus
//! the byte-for-byte determinism that the canonicalisation buys, the
//! source-map manifest, and the presence of the shipped SDK source tree.
//!
//! Assertions are pure byte inspection so they need no llvm-dwarfdump in CI.

mod common;

use common::{
    CartProject, blyt_bin, build_cart, build_debug_cart, repo_root, require_cpp_sdk,
    require_rust_riscv_target, require_sdk, sdk_dir, write_c_cart_project, write_rust_cart_project,
};
use std::path::Path;
use tempfile::TempDir;

/// A minimal C cart that quits on the first update tick.
const TINY_C: &str = r#"
#include "blyt.h"
static int s_frame = 0;
void blyt_cart_init(void)   { blyt_console_debug("source-paths cart"); }
void blyt_cart_update(void) { if (++s_frame >= 1) blyt_quit(); }
void blyt_cart_draw(void)   {}
"#;

/// A minimal Rust cart that uses `alloc` (so build-std core/alloc DWARF lands in
/// the cart) and quits on the first tick.
const TINY_RUST: &str = r#"
#![no_std]
extern crate alloc;
use alloc::vec::Vec;
use blyt::*;

#[no_mangle]
pub extern "C" fn blyt_cart_init() {
    let v: Vec<u8> = alloc::vec![1, 2, 3];
    console_debug(if v.len() == 3 { "rust source-paths" } else { "?" });
}
#[no_mangle]
pub extern "C" fn blyt_cart_update() { quit(); }
#[no_mangle]
pub extern "C" fn blyt_cart_draw() {}
"#;

fn read(p: &Path) -> Vec<u8> {
    std::fs::read(p).unwrap_or_else(|e| panic!("read {}: {e}", p.display()))
}

fn contains(haystack: &[u8], needle: &[u8]) -> bool {
    !needle.is_empty() && haystack.windows(needle.len()).any(|w| w == needle)
}

/// Assert an artifact embeds none of the machine-specific roots that would
/// betray where it was built (the determinism + privacy contract).  `extra`
/// lets a caller add the cart's own temp project dir.
fn assert_no_machine_paths(bytes: &[u8], extra: &[&Path]) {
    let mut roots: Vec<String> = vec![repo_root().display().to_string()];
    if let Ok(home) = std::env::var("HOME") {
        if !home.is_empty() {
            roots.push(home);
        }
    }
    for p in extra {
        roots.push(p.display().to_string());
    }
    for root in roots {
        assert!(
            !contains(bytes, root.as_bytes()),
            "artifact leaks machine path {root:?}"
        );
    }
}

#[test]
fn source_map_manifest_has_canonical_prefixes_and_absolute_locals() {
    require_sdk();
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hello");
    write_c_cart_project(&project, TINY_C);
    build_debug_cart(&project);

    let manifest = std::fs::read_to_string(project.join("build/source-map.json"))
        .expect("source-map.json must be written");

    for prefix in [
        "/blyt/cart",
        "/blyt/sdk",
        "/blyt/rust",
        "/blyt/cargo",
        "/blyt/src",
    ] {
        assert!(
            manifest.contains(&format!("\"prefix\": \"{prefix}\"")),
            "manifest missing prefix {prefix}:\n{manifest}"
        );
    }
    // Every `local` must be an absolute path (a debug client needs that).
    for line in manifest.lines().filter(|l| l.contains("\"local\"")) {
        let local = line
            .split("\"local\": \"")
            .nth(1)
            .and_then(|s| s.split('"').next())
            .unwrap_or("");
        assert!(
            local.starts_with('/'),
            "manifest `local` is not absolute: {line}"
        );
    }
}

/// The #26-class bug for C/Rust: invoking `blyt build` with a *relative* project
/// path must still produce canonical `/blyt/cart` DWARF and an absolute manifest
/// — the build must not depend on how it was invoked.
#[test]
fn relative_invocation_still_canonical() {
    use assert_cmd::Command;
    require_sdk();
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hello");
    write_c_cart_project(&project, TINY_C);

    let sdk = sdk_dir();
    // Run from the temp dir with a *relative* project arg ("hello").
    let mut cmd = Command::new(blyt_bin());
    cmd.current_dir(tmp.path())
        .args(["build", "--debug", "hello"])
        .env("BLYT_SDK_DIR", &sdk)
        .env("BLYT_OBJCOPY", sdk.join("bin/blyt-objcopy"))
        .env("BLYT_RUST_SDK", repo_root().join("sdk/rust/blyt"));
    if sdk.join("bin/blyt-clang").exists() {
        cmd.env("BLYT_CLANG", sdk.join("bin/blyt-clang"));
    }
    cmd.assert().success();

    let cart = project.join("build/hello.dbg.blyt");
    let bytes = read(&cart);
    assert!(
        contains(&bytes, b"/blyt/cart/src/game/c/"),
        "relative invocation did not canonicalise cart source to /blyt/cart"
    );
    assert_no_machine_paths(&bytes, &[&project]);

    let manifest = std::fs::read_to_string(project.join("build/source-map.json")).unwrap();
    let cart_local = manifest
        .lines()
        .find(|l| l.contains("\"/blyt/cart\""))
        .and_then(|l| l.split("\"local\": \"").nth(1))
        .and_then(|s| s.split('"').next())
        .unwrap_or("");
    assert!(
        cart_local.starts_with('/') && cart_local.ends_with("hello"),
        "manifest /blyt/cart local must be the absolute project dir, got {cart_local:?}"
    );
}

#[test]
fn debug_c_cart_has_canonical_paths_no_leaks() {
    require_sdk();
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hello");
    write_c_cart_project(&project, TINY_C);
    let cart = build_debug_cart(&project);
    let bytes = read(&cart);

    // A plain C cart only references its own source (blyt.h is declarations
    // reached through the PLT, no inline code), so it carries /blyt/cart but not
    // necessarily /blyt/sdk — the inline-header case is covered by the C++ test.
    assert!(
        contains(&bytes, b"/blyt/cart/src/game/c/"),
        "cart source not canonicalised to /blyt/cart"
    );
    assert_no_machine_paths(&bytes, &[&project]);
}

/// C++ carts compile libc++ templates inline.  The cart's own primary source is
/// canonicalised to /blyt/cart, and no machine path leaks — the latter is the
/// meaningful contract here (a broken /blyt/sdk remap would otherwise scatter
/// the build machine's sdk include path through the DWARF).
///
/// Note: the libc++ *header* directory strings sit behind DWARF5
/// .debug_str_offsets indirection, so neither their canonical nor machine form
/// appears as contiguous bytes.  That the shipped SDK source resolves those
/// canonical paths is covered by `sdk_ships_debug_source_tree` plus
/// `debug_guest_libs_have_canonical_paths`; an end-to-end lldb breakpoint inside
/// SDK source is a follow-up once the launch config consumes the manifest (§3).
#[test]
fn debug_cpp_cart_canonicalises_source_no_leaks() {
    require_cpp_sdk();
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("cpp_hello");
    CartProject::new()
        .cpp(
            r#"
#include "blyt.h"
#include <optional>
extern "C" void blyt_cart_init() {
    std::optional<int> v = 42;
    blyt_console_debug(v.has_value() ? "cpp source-paths" : "?");
}
extern "C" void blyt_cart_update() { blyt_quit(); }
extern "C" void blyt_cart_draw()   {}
"#,
        )
        .write(&project);
    let cart = build_debug_cart(&project);
    let bytes = read(&cart);

    assert!(
        contains(&bytes, b"/blyt/cart/src/game/c++"),
        "C++ cart source not canonicalised to /blyt/cart"
    );
    assert_no_machine_paths(&bytes, &[&project]);
}

#[test]
fn release_c_cart_has_no_machine_paths() {
    require_sdk();
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hello");
    write_c_cart_project(&project, TINY_C);
    let cart = build_cart(&project);
    // Release carries no DWARF, but __FILE__/assert strings still embed paths;
    // the prefix map (applied to release too) must keep them out.
    assert_no_machine_paths(&read(&cart), &[&project]);
}

/// The determinism payoff: the same cart built in two different directories is
/// byte-for-byte identical, debug and release.  This is what makes debug carts
/// safe in the repo's byte-identity testing posture and lets save-state/replay
/// hold across machines.
#[test]
fn carts_are_byte_identical_across_build_dirs() {
    require_sdk();
    let build_in = |root: &Path| -> (Vec<u8>, Vec<u8>) {
        let project = root.join("hello");
        write_c_cart_project(&project, TINY_C);
        let dbg = read(&build_debug_cart(&project));
        let rel = read(&build_cart(&project));
        (dbg, rel)
    };
    let t1 = TempDir::new().unwrap();
    let t2 = TempDir::new().unwrap();
    let (dbg1, rel1) = build_in(t1.path());
    let (dbg2, rel2) = build_in(t2.path());

    assert!(
        dbg1 == dbg2,
        "debug carts differ across build dirs ({} vs {} bytes)",
        dbg1.len(),
        dbg2.len()
    );
    assert!(
        rel1 == rel2,
        "release carts differ across build dirs ({} vs {} bytes)",
        rel1.len(),
        rel2.len()
    );
}

/// The shipped debug guest libraries (§4) must carry canonical
/// `/blyt/sdk/src/...` DWARF and none of the build machine's paths, so they
/// resolve from any SDK install.
#[test]
fn debug_guest_libs_have_canonical_paths() {
    require_sdk();
    let debug_lib = sdk_dir().join("lib/debug");
    let mut checked = 0;
    for entry in std::fs::read_dir(&debug_lib).expect("lib/debug exists") {
        let path = entry.unwrap().path();
        if path.extension().and_then(|e| e.to_str()) != Some("so") {
            continue;
        }
        let bytes = read(&path);
        assert!(
            contains(&bytes, b"/blyt/sdk/src/"),
            "{} has no canonical /blyt/sdk/src DWARF",
            path.display()
        );
        assert!(
            !contains(&bytes, repo_root().display().to_string().as_bytes()),
            "{} leaks the build machine path",
            path.display()
        );
        checked += 1;
    }
    assert!(checked > 0, "no debug guest .so libraries found to check");
}

/// §5: the canonical source tree is shipped, so the libraries' DWARF resolves.
#[test]
fn sdk_ships_debug_source_tree() {
    require_sdk();
    let src = sdk_dir().join("src");
    for anchor in [
        "blyt/src/libblyt32/blyt32.c",
        "blyt-dap/master_hook.c",
        "musl/src/string/memcpy.c",
        "lua/lvm.c",
        "rv32emu/src/softfloat/source/f128_add.c",
        "libcxx/libcxx/src/string.cpp",
    ] {
        assert!(
            src.join(anchor).exists(),
            "shipped SDK source missing: src/{anchor}"
        );
    }
    // The 27 MB LLVM libc tree must NOT have been shipped wholesale — only its
    // __support subset (size guard for the debug source package).
    assert!(
        !src.join("libcxx/libc/src/stdio").exists(),
        "full LLVM libc/src was shipped — only libc/src/__support should be"
    );
}

#[test]
fn debug_rust_cart_canonicalises_toolchain_paths() {
    require_sdk();
    require_rust_riscv_target();
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hello");
    write_rust_cart_project(&project, TINY_RUST);
    let cart = build_debug_cart(&project);
    let bytes = read(&cart);

    // build-std core/alloc → /blyt/rust; the injected SDK crate → /blyt/sdk.
    assert!(
        contains(&bytes, b"/blyt/rust/"),
        "build-std rust-src not canonicalised to /blyt/rust"
    );
    assert!(
        contains(&bytes, b"/blyt/sdk/rust/blyt/"),
        "SDK crate not canonicalised to /blyt/sdk/rust/blyt"
    );
    // No ~/.rustup, ~/.cargo, repo, or temp project path may leak.
    assert_no_machine_paths(&bytes, &[&project]);
}
