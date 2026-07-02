//! Palette bundling + config-driven pre-init auto-load (issue #201, ADR-0042/0088).
//!
//! Exercises the full pipeline end-to-end: `palettes: default: <name>` in
//! blyt.config.yaml -> devtool validation/encoding -> .cart.config FlatBuffer
//! section -> cart_load.c -> cart_run.c's session-create palette resolution.
//! The exact 256-entry byte content of each built-in is already pinned by the
//! C unit test (tests/unit/test_palettes.c); this test checks distinguishing
//! bytes to confirm the RIGHT table reaches the session through the real
//! build+load path, not the resolver in isolation.

mod common;

use assert_cmd::Command;
use common::{CartProject, require_sdk, require_test_session_api, sdk_dir, test_session_api};
use tempfile::TempDir;

const PLAIN_CART_C: &str = r#"
#include "blyt.h"
void blyt_cart_init(void) {}
void blyt_cart_update(void) { blyt_quit(); }
void blyt_cart_draw(void) {}
"#;

/// Run `test_session_api palette <cart> <lib_dir>` and parse its
/// "PALETTE:<256 x 8-hex>" stdout line into 256 XRGB8888 u32s.
fn session_palette(cart: &std::path::Path) -> Vec<u32> {
    let out = Command::new(test_session_api())
        .args([
            "palette",
            cart.to_str().unwrap(),
            sdk_dir().join("lib").to_str().unwrap(),
        ])
        .assert()
        .success()
        .get_output()
        .stdout
        .clone();
    let text = String::from_utf8_lossy(&out);
    let line = text
        .lines()
        .find(|l| l.starts_with("PALETTE:"))
        .unwrap_or_else(|| panic!("no PALETTE: line in output: {text}"));
    let hex = &line["PALETTE:".len()..];
    assert_eq!(hex.len(), 256 * 8, "expected 256 XRGB8888 hex entries");
    (0..256)
        .map(|i| u32::from_str_radix(&hex[i * 8..i * 8 + 8], 16).unwrap())
        .collect()
}

/// A cart with no `palettes:` declaration auto-loads the console default
/// (aurora) before init() — index 0 is black, index 255 is aurora's pinned
/// sacrificial slot #911437.
#[test]
fn undeclared_palette_defaults_to_aurora() {
    require_sdk();
    require_test_session_api();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("pal_default");
    CartProject::new().c(PLAIN_CART_C).write(&project);

    let cart = common::build_cart(&project);
    let pal = session_palette(&cart);
    assert_eq!(pal[0], 0x0000_0000, "aurora index 0 is black");
    assert_eq!(
        pal[255], 0x0091_1437,
        "aurora index 255 is the transparency slot"
    );
}

/// `palettes: default: vga` in blyt.config.yaml auto-loads the VGA built-in
/// instead of aurora — distinguishing entries prove it's the right table
/// (vga index 1 is 0x0000AA, distinct from aurora's grayscale-ramp index 1).
#[test]
fn declared_default_vga_loads_vga() {
    require_sdk();
    require_test_session_api();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("pal_vga");
    CartProject::new()
        .c(PLAIN_CART_C)
        .config("palettes:\n  default: vga\n")
        .write(&project);

    let cart = common::build_cart(&project);
    let pal = session_palette(&cart);
    assert_eq!(pal[0], 0x0000_0000, "vga index 0 is black");
    assert_eq!(
        pal[1], 0x0000_00AA,
        "vga index 1 (blue) distinguishes it from aurora"
    );
    assert_eq!(
        pal[255], 0x0000_0000,
        "vga's unused tail (248-255) is black-padded"
    );
}

/// An unknown `palettes: default:` name is a build error naming the bad value
/// and the allowed set — no silent fallback (ADR-0088's "all silent handling
/// is rejected" principle).
#[test]
fn unknown_palette_name_is_a_build_error() {
    require_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("pal_bad");
    CartProject::new()
        .c(PLAIN_CART_C)
        .config("palettes:\n  default: palette_vga\n")
        .write(&project);

    let out = common::build_cart_expect_failure(&project);
    assert!(out.contains("unknown built-in palette"), "{out}");
    assert!(out.contains("palette_vga"), "{out}");
}
