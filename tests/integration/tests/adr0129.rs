//! ADR-0129: debug/release build-variant assertions.
//!
//! These verify the observable outcomes of the split directly (no debugger
//! orchestration needed): release carts are optimized + stripped, debug carts
//! carry DWARF and a `.dbg.blyt` name, `blyt debug` refuses a release native
//! cart, and the release native player ships no GDB/DAP machinery.

mod common;

use common::{
    blyt_bin, blytplay, build_cart, build_debug_cart, require_sdk, sdk_dir, write_c_cart_project,
};
use std::path::Path;
use tempfile::TempDir;

/// A minimal native C cart that quits on the first update tick.
const TINY_C: &str = r#"
void blyt_console_debug(const char *);
void blyt_quit(void);
void blyt_cart_init(void)   { blyt_console_debug("adr0129 cart"); }
void blyt_cart_update(void) { blyt_quit(); }
void blyt_cart_draw(void)   {}
"#;

/// Minimal ELF32 little-endian section-name reader (no external tools).
fn elf_section_names(path: &Path) -> Vec<String> {
    let data = std::fs::read(path).expect("read cart ELF");
    assert!(
        data.len() > 52 && &data[0..4] == b"\x7fELF" && data[4] == 1,
        "not ELF32"
    );
    let rd_u32 = |o: usize| u32::from_le_bytes([data[o], data[o + 1], data[o + 2], data[o + 3]]);
    let rd_u16 = |o: usize| u16::from_le_bytes([data[o], data[o + 1]]);

    let e_shoff = rd_u32(0x20) as usize;
    let e_shentsize = rd_u16(0x2e) as usize;
    let e_shnum = rd_u16(0x30) as usize;
    let e_shstrndx = rd_u16(0x32) as usize;
    let strtab_off = rd_u32(e_shoff + e_shstrndx * e_shentsize + 0x10) as usize;

    let mut names = Vec::new();
    for i in 0..e_shnum {
        let name_off = strtab_off + rd_u32(e_shoff + i * e_shentsize) as usize;
        if let Some(end) = data[name_off..].iter().position(|&b| b == 0) {
            names.push(String::from_utf8_lossy(&data[name_off..name_off + end]).into_owned());
        }
    }
    names
}

#[test]
fn debug_cart_has_dwarf_and_dbg_name() {
    require_sdk();
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("adr0129_dbg");
    write_c_cart_project(&project, TINY_C);

    let cart = build_debug_cart(&project);
    assert!(
        cart.to_str().unwrap().ends_with(".dbg.blyt"),
        "debug cart should be <name>.dbg.blyt, got {}",
        cart.display()
    );
    assert!(
        cart.exists(),
        "debug cart not produced at {}",
        cart.display()
    );

    let sections = elf_section_names(&cart);
    assert!(
        sections.iter().any(|s| s.starts_with(".debug_")),
        "debug cart must carry DWARF (.debug_*); sections: {sections:?}"
    );
}

#[test]
fn release_cart_is_stripped() {
    require_sdk();
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("adr0129_rel");
    write_c_cart_project(&project, TINY_C);

    let cart = build_cart(&project);
    assert!(
        cart.to_str().unwrap().ends_with(".blyt") && !cart.to_str().unwrap().ends_with(".dbg.blyt"),
        "release cart should be <name>.blyt, got {}",
        cart.display()
    );

    let sections = elf_section_names(&cart);
    assert!(
        !sections.iter().any(|s| s.starts_with(".debug_")),
        "release cart must not carry DWARF; sections: {sections:?}"
    );
    assert!(
        !sections.iter().any(|s| s == ".symtab"),
        "release cart must be stripped of .symtab; sections: {sections:?}"
    );
}

#[test]
fn blyt_debug_rejects_release_native_cart() {
    require_sdk();
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("adr0129_reject");
    write_c_cart_project(&project, TINY_C);

    let cart = build_cart(&project); // release: no DWARF, no .cart.lua

    // `blyt debug` checks for DWARF before it ever needs the WASM runtime, so it
    // aborts with the rebuild hint regardless of WASM availability.
    let out = std::process::Command::new(blyt_bin())
        .args(["debug", cart.to_str().unwrap()])
        .env("BLYT_SDK_DIR", sdk_dir())
        .output()
        .expect("run blyt debug");
    assert!(
        !out.status.success(),
        "blyt debug must fail on a release native cart"
    );
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(
        stderr.contains("not a debug build"),
        "expected rebuild hint, got stderr: {stderr}"
    );
}

#[test]
fn release_blytplay_has_no_debugger_symbols() {
    // The shipped release player must carry zero GDB/DAP machinery (ADR-0129).
    let bin = blytplay();
    if !bin.exists() {
        return; // player not built in this configuration
    }
    let nm = std::process::Command::new("nm").arg(&bin).output();
    if let Ok(out) = nm {
        let syms = String::from_utf8_lossy(&out.stdout);
        for needle in ["dap_server", "gdb_stub", "fc_gdb", "fc_dap"] {
            assert!(
                !syms.contains(needle),
                "release blytplay must not contain debugger symbol `{needle}`"
            );
        }
    }
}
