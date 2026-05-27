mod common;

use assert_cmd::Command;
use common::{CartProject, blytplay, build_cart, require_cpp_sdk, sdk_dir};
use tempfile::TempDir;

/// Build and run a minimal C++ cart.
///
/// Verifies that a pure C++ cart compiled with `language: "c++"` produces
/// the expected debug output.  Requires libc++ to have been built as part of
/// `cmake --build build --target sdk`.
///
/// Skipped when the SDK (blyt-clang++ or libc++.a) is not available.
#[test]
fn cpp_cart_debug_output() {
    require_cpp_sdk();
    let sdk = sdk_dir();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("cpp_hello");

    CartProject::new()
        .cpp(
            r#"
#include "blyt.h"
#include <optional>

extern "C" void blyt_cart_init() {
    std::optional<int> v = 42;
    if (v.has_value() && *v == 42)
        blyt_console_debug("hello from c++");
    else
        blyt_console_debug("c++ wrong");
}

extern "C" void blyt_cart_update() { blyt_quit(); }
extern "C" void blyt_cart_draw()   {}
"#,
        )
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let output = Command::new(blytplay())
        .args(["--headless", cart.to_str().unwrap()])
        .env("BLYT_LIB_DIR", sdk.join("lib"))
        .assert()
        .success()
        .get_output()
        .stdout
        .clone();

    assert!(
        String::from_utf8_lossy(&output).contains("hello from c++"),
        "expected 'hello from c++' in output, got: {}",
        String::from_utf8_lossy(&output)
    );
}
