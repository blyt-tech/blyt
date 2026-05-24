mod common;

use assert_cmd::Command;
use common::{CartProject, blytrun, build_cart, has_rust_riscv_target, sdk_dir};
use tempfile::TempDir;

/// Build and run a minimal Rust cart.  Verifies that a pure Rust cart
/// compiled with `language: rust` produces the expected debug output.
///
/// Skipped when:
/// - The SDK is not assembled (no blyt-clang or libblyt32.so)
/// - The riscv32imafc-unknown-none-elf Rust target is not installed
///   (`rustup target add riscv32imafc-unknown-none-elf` to enable)
#[test]
fn rust_cart_debug_output() {
    let sdk = sdk_dir();
    if !sdk.join("bin/blyt-clang").exists() || !sdk.join("lib/libblyt32.so").exists() {
        eprintln!("skipping rust_cart_debug_output: SDK not assembled");
        return;
    }
    if !has_rust_riscv_target() {
        eprintln!("skipping rust_cart_debug_output: riscv32imafc-unknown-none-elf not installed");
        return;
    }

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("rust_hello");

    CartProject::new()
        .rust(
            r#"#![no_std]

#[no_mangle]
pub extern "C" fn blyt_cart_init() {
    blyt::console_debug("hello from rust");
}

#[no_mangle]
pub extern "C" fn blyt_cart_update() {
    blyt::quit();
}

#[no_mangle]
pub extern "C" fn blyt_cart_draw() {}
"#,
        )
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let output = Command::new(blytrun())
        .args(["--headless", cart.to_str().unwrap()])
        .env("BLYT_LIB_DIR", sdk.join("lib"))
        .assert()
        .success()
        .get_output()
        .stdout
        .clone();

    assert!(
        String::from_utf8_lossy(&output).contains("hello from rust"),
        "expected 'hello from rust' in output, got: {}",
        String::from_utf8_lossy(&output)
    );
}

/// Rust game code calls a C library function via extern "C".
///
/// Validates the cross-language path: C library compiled to lib.a, Rust game
/// code declares the symbol via extern "C", final clang link resolves it.
///
/// Skipped when the SDK or riscv32imafc Rust target is not available.
#[test]
fn rust_cart_calls_c_lib_function() {
    let sdk = sdk_dir();
    if !sdk.join("bin/blyt-clang").exists() || !sdk.join("lib/libblyt32.so").exists() {
        eprintln!("skipping rust_cart_calls_c_lib_function: SDK not assembled");
        return;
    }
    if !has_rust_riscv_target() {
        eprintln!(
            "skipping rust_cart_calls_c_lib_function: riscv32imafc-unknown-none-elf not installed"
        );
        return;
    }

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("rust_c_lib");

    CartProject::new()
        .lib_file("mathlib", "include/mathlib.h", "int add(int a, int b);")
        .lib_file(
            "mathlib",
            "mathlib.c",
            "#include \"mathlib.h\"\nint add(int a, int b) { return a + b; }\n",
        )
        .rust(
            r#"#![no_std]

extern "C" {
    fn add(a: i32, b: i32) -> i32;
}

#[no_mangle]
pub extern "C" fn blyt_cart_init() {
    let result = unsafe { add(3, 4) };
    if result == 7 {
        blyt::console_debug("rust+c ok");
    } else {
        blyt::console_debug("rust+c wrong");
    }
}

#[no_mangle]
pub extern "C" fn blyt_cart_update() {
    blyt::quit();
}

#[no_mangle]
pub extern "C" fn blyt_cart_draw() {}
"#,
        )
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let output = Command::new(blytrun())
        .args(["--headless", cart.to_str().unwrap()])
        .env("BLYT_LIB_DIR", sdk.join("lib"))
        .assert()
        .success()
        .get_output()
        .stdout
        .clone();

    assert!(
        String::from_utf8_lossy(&output).contains("rust+c ok"),
        "expected 'rust+c ok' in output, got: {}",
        String::from_utf8_lossy(&output)
    );
}

/// Rust game code calls a C++ library function via extern "C".
///
/// The library is written in C++ (compiled with clang++) but exposes its API
/// through a C ABI so Rust can call it without any C++ awareness (ADR-0121).
/// The library itself uses no STL, so libc++.a is not required at link time.
///
/// Skipped when SDK or riscv32imafc Rust target is absent.
#[test]
fn rust_cart_calls_cpp_lib_over_c_abi() {
    let sdk = sdk_dir();
    if !sdk.join("bin/blyt-clang").exists() || !sdk.join("lib/libblyt32.so").exists() {
        eprintln!("skipping rust_cart_calls_cpp_lib_over_c_abi: SDK not assembled");
        return;
    }
    if !sdk.join("bin/blyt-clang++").exists() {
        eprintln!("skipping rust_cart_calls_cpp_lib_over_c_abi: blyt-clang++ not in SDK");
        return;
    }
    if !has_rust_riscv_target() {
        eprintln!(
            "skipping rust_cart_calls_cpp_lib_over_c_abi: riscv32imafc-unknown-none-elf not installed"
        );
        return;
    }

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("rust_cpp_lib");

    CartProject::new()
        .lib_file(
            "cpplib",
            "include/cpplib.h",
            // The header uses the C++ / C guard pattern so it can be included
            // from both C++ implementation files and C/Rust extern declarations.
            "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\
             int cpp_multiply(int a, int b);\n\
             #ifdef __cplusplus\n}\n#endif\n",
        )
        .lib_file(
            "cpplib",
            "cpplib.cpp",
            // C++ implementation; extern \"C\" on the definition makes the
            // symbol name un-mangled and callable from any language via the C ABI.
            "#include \"cpplib.h\"\n\
             extern \"C\" int cpp_multiply(int a, int b) { return a * b; }\n",
        )
        .rust(
            r#"#![no_std]

extern "C" {
    fn cpp_multiply(a: i32, b: i32) -> i32;
}

#[no_mangle]
pub extern "C" fn blyt_cart_init() {
    let result = unsafe { cpp_multiply(6, 7) };
    if result == 42 {
        blyt::console_debug("rust+cpp ok");
    } else {
        blyt::console_debug("rust+cpp wrong");
    }
}

#[no_mangle]
pub extern "C" fn blyt_cart_update() {
    blyt::quit();
}

#[no_mangle]
pub extern "C" fn blyt_cart_draw() {}
"#,
        )
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let output = Command::new(blytrun())
        .args(["--headless", cart.to_str().unwrap()])
        .env("BLYT_LIB_DIR", sdk.join("lib"))
        .assert()
        .success()
        .get_output()
        .stdout
        .clone();

    assert!(
        String::from_utf8_lossy(&output).contains("rust+cpp ok"),
        "expected 'rust+cpp ok' in output, got: {}",
        String::from_utf8_lossy(&output)
    );
}

/// Rust game code calls a function from a Rust library in src/lib/.
///
/// The lib is compiled as part of the same cargo invocation (via --config
/// patch injection), so the Rust type system works across the boundary and
/// there is no fingerprint mismatch.
///
/// Skipped when SDK or riscv32imafc Rust target is absent.
#[test]
fn rust_cart_calls_rust_lib() {
    let sdk = sdk_dir();
    if !sdk.join("bin/blyt-clang").exists() || !sdk.join("lib/libblyt32.so").exists() {
        eprintln!("skipping rust_cart_calls_rust_lib: SDK not assembled");
        return;
    }
    if !has_rust_riscv_target() {
        eprintln!("skipping rust_cart_calls_rust_lib: riscv32imafc-unknown-none-elf not installed");
        return;
    }

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("rust_lib_test");

    CartProject::new()
        .rust_lib(
            "arith",
            "#![no_std]\npub fn add(a: i32, b: i32) -> i32 { a + b }\n",
        )
        .rust(
            r#"#![no_std]

use arith::add;

#[no_mangle]
pub extern "C" fn blyt_cart_init() {
    if add(3, 4) == 7 {
        blyt::console_debug("rust+rust ok");
    } else {
        blyt::console_debug("rust+rust wrong");
    }
}

#[no_mangle]
pub extern "C" fn blyt_cart_update() {
    blyt::quit();
}

#[no_mangle]
pub extern "C" fn blyt_cart_draw() {}
"#,
        )
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let output = Command::new(blytrun())
        .args(["--headless", cart.to_str().unwrap()])
        .env("BLYT_LIB_DIR", sdk.join("lib"))
        .assert()
        .success()
        .get_output()
        .stdout
        .clone();

    assert!(
        String::from_utf8_lossy(&output).contains("rust+rust ok"),
        "expected 'rust+rust ok' in output, got: {}",
        String::from_utf8_lossy(&output)
    );
}
