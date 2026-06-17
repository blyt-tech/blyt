mod common;

use assert_cmd::Command;
use common::{blyt_bin, blytplay, build_cart, require_sdk, sdk_dir};
use predicates::prelude::*;
use std::fs;
use tempfile::TempDir;

const SMOKE_C_SOURCE: &str = r#"
#include "blyt.h"

static int s_frame = 0;

void blyt_cart_init(void) {
    blyt_console_debug("init");
}
void blyt_cart_update(void) {
    blyt_console_debug("update");
    if (++s_frame >= 1) blyt_quit();
}
void blyt_cart_draw(void) {
    blyt_console_debug("draw");
}
"#;

/// Write a project that uses compile_command with blyt-clang as the external
/// compiler for a freeform language name.  This exercises the full external
/// compile path without requiring any non-standard toolchain.
fn write_external_c_project(dir: &std::path::Path) {
    fs::create_dir_all(dir).unwrap();
    fs::write(
        dir.join("blyt.info.yaml"),
        "id: external\ntitle: External Compile Test\n",
    )
    .unwrap();
    // Use "c-via-external" as the language name — not in the CartLanguage enum,
    // which proves the devtool accepts freeform names when compile_command is set.
    let clang_str = sdk_dir()
        .join("bin/blyt-clang")
        .to_string_lossy()
        .into_owned();
    fs::write(
        dir.join("blyt.build.yaml"),
        format!(
            "language: c-via-external\n\
             source_extension: .c\n\
             compile_command: >\n  \
               {clang_str}\n  \
               --target=riscv32 -march=rv32imafdc -mabi=ilp32d\n  \
               -nostdlib -fpie -ffunction-sections -fdata-sections\n  \
               -ffp-contract=off -fno-fast-math -fwrapv -frounding-math -fsignaling-nans\n  \
               -O2 -c -I@SDK_INCLUDE@\n  \
               -o @OBJFILE@\n  \
               @SRCFILES@\n",
        ),
    )
    .unwrap();
    let src_dir = dir.join("src/game/c-via-external");
    fs::create_dir_all(&src_dir).unwrap();
    fs::write(src_dir.join("main.c"), SMOKE_C_SOURCE).unwrap();
}

// -------------------------------------------------------------------------
// Success cases
// -------------------------------------------------------------------------

/// Build a cart with compile_command using blyt-clang as the external compiler.
/// The freeform language name "c-via-external" is intentional — proves the
/// devtool does not require a known language name when compile_command is set.
#[test]
fn external_compile_builds_cart() {
    require_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("external");
    write_external_c_project(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not built: {}", cart.display());
}

/// Cart built with compile_command runs and produces expected output.
#[test]
fn external_compile_cart_runs() {
    require_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("external");
    write_external_c_project(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not built: {}", cart.display());

    let output = Command::new(blytplay())
        .args(["--headless", cart.to_str().unwrap()])
        .assert()
        .success()
        .get_output()
        .stdout
        .clone();

    let out = String::from_utf8_lossy(&output);
    assert!(
        out.contains("init"),
        "expected 'init' in output, got: {out}"
    );
    assert!(
        out.contains("update"),
        "expected 'update' in output, got: {out}"
    );
    assert!(
        out.contains("draw"),
        "expected 'draw' in output, got: {out}"
    );
}

/// A lib file with the external language extension is picked up in @SRCFILES@.
#[test]
fn external_compile_picks_up_lib_sources() {
    require_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("ext_lib");
    fs::create_dir_all(&project).unwrap();
    fs::write(
        project.join("blyt.info.yaml"),
        "id: ext_lib\ntitle: External Lib Test\n",
    )
    .unwrap();

    let sdk = sdk_dir();
    let blyt_clang = sdk.join("bin/blyt-clang");
    let clang_str = blyt_clang.to_string_lossy();
    let lld_str = sdk.join("bin/blyt-ld.lld").to_string_lossy().into_owned();
    // Use partial linking (-r) so multiple .c files can be combined into one
    // .o — this is what a real whole-module compiler (Swift -wmo, Zig) does
    // natively; for C the -r flag achieves the same effect.
    // --ld-path= points clang at the SDK's lld rather than the system ld
    // (macOS's system ld does not support -r).
    fs::write(
        project.join("blyt.build.yaml"),
        format!(
            "language: c-via-external\n\
             source_extension: .c\n\
             compile_command: >\n  \
               {clang_str}\n  \
               --target=riscv32 -march=rv32imafdc -mabi=ilp32d\n  \
               -nostdlib -fpie --ld-path={lld_str} -r -I@SDK_INCLUDE@\n  \
               -o @OBJFILE@\n  \
               @SRCFILES@\n",
            clang_str = clang_str,
            lld_str = lld_str,
        ),
    )
    .unwrap();

    // Game code calls a function declared in lib.h and defined in lib.c.
    let game_dir = project.join("src/game/c-via-external");
    fs::create_dir_all(&game_dir).unwrap();
    fs::write(
        game_dir.join("main.c"),
        r#"
#include "blyt.h"
int ext_add(int a, int b);
void blyt_cart_init(void) {
    blyt_console_debug(ext_add(3, 4) == 7 ? "lib ok" : "lib wrong");
}
void blyt_cart_update(void) { blyt_quit(); }
void blyt_cart_draw(void)   {}
"#,
    )
    .unwrap();

    // Library definition — placed in src/lib/extlib/ and must be collected
    // into @SRCFILES@ by collect_external_source_files.
    let lib_dir = project.join("src/lib/extlib");
    fs::create_dir_all(&lib_dir).unwrap();
    fs::write(
        lib_dir.join("lib.c"),
        "int ext_add(int a, int b) { return a + b; }\n",
    )
    .unwrap();

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not built: {}", cart.display());

    let output = Command::new(blytplay())
        .args(["--headless", cart.to_str().unwrap()])
        .assert()
        .success()
        .get_output()
        .stdout
        .clone();

    assert!(
        String::from_utf8_lossy(&output).contains("lib ok"),
        "expected 'lib ok', got: {}",
        String::from_utf8_lossy(&output)
    );
}

// -------------------------------------------------------------------------
// Error cases
// -------------------------------------------------------------------------

fn assert_build_manifest_fails(build_yaml: &str, expected: &str) {
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("err");
    fs::create_dir_all(&project).unwrap();
    fs::write(
        project.join("blyt.info.yaml"),
        "id: err\ntitle: Error Test\n",
    )
    .unwrap();
    fs::write(project.join("blyt.build.yaml"), build_yaml).unwrap();

    Command::new(blyt_bin())
        .args(["build", project.to_str().unwrap()])
        .env("BLYT_SDK_DIR", sdk_dir())
        .env("BLYT_OBJCOPY", sdk_dir().join("bin/blyt-objcopy"))
        .assert()
        .failure()
        .stderr(predicate::str::contains(expected));
}

#[test]
fn compile_command_without_language_fails() {
    assert_build_manifest_fails(
        "compile_command: \"blyt-clang -o @OBJFILE@ @SRCFILES@\"\n",
        "requires `language:`",
    );
}

#[test]
fn compile_command_with_languages_map_fails() {
    assert_build_manifest_fails(
        "languages:\n  c:\ncompile_command: \"blyt-clang -o @OBJFILE@ @SRCFILES@\"\n",
        "cannot be used with `languages:`",
    );
}

#[test]
fn source_extension_without_compile_command_fails() {
    assert_build_manifest_fails(
        "language: c\nsource_extension: .c\n",
        "requires `compile_command`",
    );
}

#[test]
fn strip_sections_without_compile_command_fails() {
    assert_build_manifest_fails(
        "language: c\nstrip_sections:\n  - .foo\n",
        "requires `compile_command`",
    );
}

#[test]
fn compile_command_missing_objfile_placeholder_fails() {
    assert_build_manifest_fails(
        "language: swift\ncompile_command: \"swiftc @SRCFILES@\"\n",
        "@OBJFILE@",
    );
}

#[test]
fn compile_command_missing_srcfiles_placeholder_fails() {
    assert_build_manifest_fails(
        "language: swift\ncompile_command: \"swiftc -o @OBJFILE@\"\n",
        "@SRCFILES@",
    );
}

#[test]
fn compile_command_unknown_placeholder_fails() {
    // Validation fires at manifest parse time — no SDK or source files needed.
    assert_build_manifest_fails(
        "language: swift\ncompile_command: \"swiftc -o @OBJFILE@ @SRCFILES@ @UNKNOWN@\"\n",
        "unknown placeholder",
    );
}

#[test]
fn compile_command_no_source_files_fails() {
    // Source-file check fires in early validation — no SDK needed.
    assert_build_manifest_fails(
        "language: swift\nsource_extension: .swift\ncompile_command: \"swiftc -o @OBJFILE@ @SRCFILES@\"\n",
        "no .swift files",
    );
}
