use assert_cmd::Command;
use predicates::prelude::*;
use std::fs;
use std::path::PathBuf;
use tempfile::TempDir;

/// Returns the repository root (two levels above this crate's manifest dir).
fn repo_root() -> PathBuf {
    // CARGO_MANIFEST_DIR = <repo>/devtool/integration
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent() // devtool/
        .unwrap()
        .parent() // repo root
        .unwrap()
        .to_path_buf()
}

/// Path to the CMake build directory (contains blytrun and the runtime libraries).
fn build_dir() -> PathBuf {
    repo_root().join("build")
}

/// Path to the blytrun binary produced by the CMake build.
fn blytrun() -> PathBuf {
    build_dir().join("blytrun")
}

/// Create a minimal cart project in `dir` with a single C source file.
fn write_cart_project(dir: &std::path::Path, source: &str) {
    let c_dir = dir.join("src/game/c");
    fs::create_dir_all(&c_dir).unwrap();
    fs::write(c_dir.join("main.c"), source).unwrap();
}

/// Run `blyt build <project_dir>` and return the expected cart output path.
fn build_cart(project_dir: &std::path::Path) -> PathBuf {
    let sdk = sdk_dir();
    let mut cmd = Command::cargo_bin("blyt").unwrap();
    cmd.args(["build", project_dir.to_str().unwrap()])
        .env("BLYT_SDK_DIR", &sdk)
        .env("BLYT_OBJCOPY", sdk.join("bin/blyt-objcopy"));
    // Use the SDK's riscv32-capable clang if available; system clang on macOS
    // cannot target riscv32 so the test is skipped when the SDK is absent.
    let sdk_clang = sdk.join("bin/blyt-clang");
    if sdk_clang.exists() {
        cmd.env("BLYT_CLANG", &sdk_clang);
    }
    cmd.assert().success();

    // Default output: <parent>/<project_dir_name>.blyt
    project_dir.parent().unwrap().join(format!(
        "{}.blyt",
        project_dir.file_name().unwrap().to_str().unwrap()
    ))
}

/// Path to the assembled SDK directory (build/sdk/).
fn sdk_dir() -> PathBuf {
    build_dir().join("sdk")
}

// -------------------------------------------------------------------------
// Tests
// -------------------------------------------------------------------------

/// SDK end-to-end: use the assembled SDK (build/sdk/) to build and run a cart.
///
/// The SDK binary auto-discovers its toolchain and libraries from its own
/// location in build/sdk/bin/ — no env vars required for blyt build.
/// blytrun needs BLYT_LIB_DIR since it cannot yet auto-discover its libraries.
///
/// Requires `cmake --build build --target sdk` to have completed.
/// Silently skipped if the SDK has not been assembled.
#[test]
fn sdk_e2e_build_and_run() {
    let sdk = sdk_dir();
    let sdk_blyt = sdk.join("bin/blyt");
    let sdk_blytrun = sdk.join("bin/blytrun");

    assert!(
        sdk_blyt.exists() && sdk_blytrun.exists(),
        "SDK not assembled — run `cmake --build build --target sdk` first"
    );

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hello");

    write_cart_project(
        &project,
        r#"
#include "blyt.h"

static int s_frame = 0;

void blyt_cart_init(void)   { blyt_console_debug("hello from sdk"); }
void blyt_cart_update(void) { if (++s_frame >= 1) blyt_quit_ready(); }
void blyt_cart_draw(void)   {}
"#,
    );

    // Build the cart using the assembled SDK binary.  Remove inherited override
    // env vars so that the SDK blyt fully auto-discovers its toolchain
    // (blyt-clang) and runtime libraries from its own location in
    // build/sdk/bin/.
    Command::new(&sdk_blyt)
        .args(["build", project.to_str().unwrap()])
        .env_remove("BLYT_SDK_DIR")
        .env_remove("BLYT_CLANG")
        .env_remove("BLYT_OBJCOPY")
        .assert()
        .success();

    let cart = project.parent().unwrap().join(format!(
        "{}.blyt",
        project.file_name().unwrap().to_str().unwrap()
    ));
    assert!(cart.exists(), "cart not found at {}", cart.display());

    // Run the cart with the SDK's blytrun.
    let out = Command::new(&sdk_blytrun)
        .args(["--headless", cart.to_str().unwrap()])
        .env("BLYT_LIB_DIR", sdk.join("lib"))
        .assert()
        .success()
        .get_output()
        .stdout
        .clone();

    assert!(
        String::from_utf8_lossy(&out).contains("hello from sdk"),
        "expected 'hello from sdk' in output, got: {}",
        String::from_utf8_lossy(&out)
    );
}

/// The lifecycle callbacks fire in the correct order: init, then update+draw
/// each iteration, with quit signalled after the second update.
///
/// Requires the SDK to be assembled (riscv32 toolchain in build/sdk/).
#[test]
fn hello_cart_lifecycle_output() {
    assert!(
        sdk_dir().join("bin/blyt-clang").exists(),
        "SDK not assembled — run `cmake --build build --target sdk` first"
    );
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hello");

    write_cart_project(
        &project,
        r#"
#include "blyt.h"

static int s_frame = 0;

void blyt_cart_init(void)   { blyt_console_debug("init"); }
void blyt_cart_update(void) {
    blyt_console_debug("update");
    if (++s_frame >= 2) blyt_quit_ready();
}
void blyt_cart_draw(void)   { blyt_console_debug("draw"); }
"#,
    );

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let output = Command::new(blytrun())
        .args(["--headless", cart.to_str().unwrap()])
        .env("BLYT_LIB_DIR", sdk_dir().join("lib"))
        .assert()
        .success()
        .get_output()
        .stdout
        .clone();
    let out = String::from_utf8_lossy(&output);

    // init fires exactly once, before any update/draw
    assert!(out.contains("init"), "missing 'init' in output: {out}");
    // update and draw both appear (multiple iterations)
    assert!(out.contains("update"), "missing 'update' in output: {out}");
    assert!(out.contains("draw"), "missing 'draw' in output: {out}");
    // init must appear before the first update
    let init_pos = out.find("init").unwrap();
    let update_pos = out.find("update").unwrap();
    assert!(init_pos < update_pos, "init must precede first update");
}

/// blyt build with no source files produces a clear error and non-zero exit.
#[test]
fn build_empty_project_fails_with_error() {
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("empty");
    fs::create_dir_all(project.join("src/game/c")).unwrap();

    Command::cargo_bin("blyt")
        .unwrap()
        .args(["build", project.to_str().unwrap()])
        .env("BLYT_SDK_DIR", sdk_dir())
        .env("BLYT_OBJCOPY", sdk_dir().join("bin/blyt-objcopy"))
        .assert()
        .failure()
        .stderr(predicate::str::contains("no .c files"));
}

/// A cart that calls abort() must exit with a non-zero status and the
/// frontend must report the abort (not confuse it with a clean exit).
#[test]
fn cart_abort_surfaces_as_error() {
    assert!(
        sdk_dir().join("bin/blyt-clang").exists(),
        "SDK not assembled — run `cmake --build build --target sdk` first"
    );

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("abort_cart");

    write_cart_project(
        &project,
        r#"
#include "blyt.h"
#include <stdlib.h>

void blyt_cart_init(void)   { abort(); }
void blyt_cart_update(void) { blyt_quit_ready(); }
void blyt_cart_draw(void)   {}
"#,
    );

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    Command::new(blytrun())
        .args(["--headless", cart.to_str().unwrap()])
        .env("BLYT_LIB_DIR", sdk_dir().join("lib"))
        .assert()
        .failure() // non-zero exit — abort is not a clean exit
        .stderr(predicate::str::contains("aborted"));
}

/// blytrun --headless rejects a non-existent cart path with a non-zero exit.
#[test]
fn run_missing_cart_fails() {
    Command::new(blytrun())
        .args(["--headless", "/nonexistent/path/cart.blyt"])
        .env("BLYT_LIB_DIR", sdk_dir().join("lib"))
        .assert()
        .failure();
}

/// Cart allocates a heap buffer, formats a float into it with snprintf, logs
/// it via blyt_console_debug, then frees the buffer.
///
/// Validates malloc + snprintf + free through libblytc.so.
/// Requires the SDK (libblytc.so) to be assembled.
#[test]
fn hello_cart_malloc_debug() {
    assert!(
        sdk_dir().join("lib/libblytc.so").exists(),
        "SDK not assembled or libblytc.so missing — \
         run `cmake --build build --target sdk` first"
    );
    assert!(
        sdk_dir().join("bin/blyt-clang").exists(),
        "SDK not assembled — run `cmake --build build --target sdk` first"
    );

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("malloc_debug");

    // Format the float as two integer parts to stay entirely within RV32F
    // (single-precision) arithmetic.  The %f specifier always promotes float
    // to double, which requires compiler-rt soft-double builtins not yet
    // bundled in the SDK.  The integer approach exercises malloc, snprintf,
    // and free without triggering those dependencies.
    write_cart_project(
        &project,
        r#"
#include "blyt.h"
#include <stdlib.h>
#include <stdio.h>

void blyt_cart_init(void) {
    float val = 3.14159f;
    char *buf = (char *)malloc(32);
    if (!buf) {
        blyt_console_debug("malloc failed");
        return;
    }
    int whole = (int)val;
    int frac  = (int)((val - (float)whole) * 10000.0f + 0.5f);
    snprintf(buf, 32, "pi=%d.%04d", whole, frac);
    blyt_console_debug(buf);
    free(buf);
}
void blyt_cart_update(void) { blyt_quit_ready(); }
void blyt_cart_draw(void)   {}
"#,
    );

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let output = Command::new(blytrun())
        .args(["--headless", cart.to_str().unwrap()])
        .env("BLYT_LIB_DIR", sdk_dir().join("lib"))
        .assert()
        .success()
        .get_output()
        .stdout
        .clone();

    let out = String::from_utf8_lossy(&output);
    assert!(
        out.contains("pi=3.1416"),
        "expected 'pi=3.1416' in output, got: {out}"
    );
}

/// Stress-tests the libblytc.so arena allocator boundary conditions:
///   1. Basic malloc / free / realloc / calloc correctness
///   2. OOM: single allocation larger than the 16 MiB arena returns NULL
///   3. calloc zeroing
///   4. realloc preserves data
///   5. coalescing: free two adjacent blocks, verify the merged space can
///      satisfy an allocation larger than either block alone
///
/// The cart prints "arena tests passed" on success; individual failures
/// are logged with a "FAIL:" prefix so the assertion below catches them.
#[test]
fn arena_boundary_tests() {
    assert!(
        sdk_dir().join("lib/libblytc.so").exists(),
        "SDK not assembled or libblytc.so missing — \
         run `cmake --build build --target sdk` first"
    );
    assert!(
        sdk_dir().join("bin/blyt-clang").exists(),
        "SDK not assembled — run `cmake --build build --target sdk` first"
    );

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("arena_boundary");

    write_cart_project(
        &project,
        r#"
#include "blyt.h"
#include <stdlib.h>
#include <string.h>

static int g_ok = 1;

static void check(int cond, const char *msg) {
    if (!cond) {
        blyt_console_debug(msg);
        g_ok = 0;
    }
}

void blyt_cart_init(void) {
    /* --- 1. Basic alloc / free --- */
    void *p = malloc(64);
    check(p != NULL, "FAIL: basic malloc returned NULL");
    if (p) {
        memset(p, 0xAB, 64);
        free(p);
    }

    /* --- 2. calloc zero-initialises --- */
    int *arr = (int *)calloc(16, sizeof(int));
    check(arr != NULL, "FAIL: calloc returned NULL");
    if (arr) {
        int zeroed = 1;
        for (int i = 0; i < 16; i++) if (arr[i] != 0) { zeroed = 0; break; }
        check(zeroed, "FAIL: calloc did not zero-initialise");
        free(arr);
    }

    /* --- 3. realloc grows and preserves data --- */
    char *s = (char *)malloc(8);
    check(s != NULL, "FAIL: malloc for realloc returned NULL");
    if (s) {
        memcpy(s, "hello", 6);
        char *s2 = (char *)realloc(s, 64);
        check(s2 != NULL, "FAIL: realloc returned NULL");
        if (s2) {
            check(memcmp(s2, "hello", 6) == 0,
                  "FAIL: realloc did not preserve data");
            free(s2);
        }
    }

    /* --- 4. OOM: request more than the 16 MiB arena --- */
    void *big = malloc(17u * 1024u * 1024u); /* 17 MiB > 16 MiB arena */
    check(big == NULL, "FAIL: over-arena malloc should return NULL");
    if (big) free(big);

    /* --- 5. Coalescing: free two adjacent blocks, then allocate merged size.
     *
     * Allocate A (512 bytes) then B (512 bytes) consecutively so they are
     * physically adjacent in the arena.  Free both.  The allocator should
     * coalesce them into a single ~1 KiB free region.  An 800-byte malloc
     * then succeeds only if coalescing happened (each individual block is
     * too small to satisfy it after fragmentation).
     *
     * Note: each block carries a 16-byte header, so block A occupies
     * 512 + 16 = 528 bytes and block B likewise.  After freeing both the
     * merged region is 1056 bytes; an 800-byte request (+ 16 header = 816)
     * fits inside. */
    void *a = malloc(512);
    void *b = malloc(512);
    check(a != NULL && b != NULL, "FAIL: setup allocs for coalesce test");
    if (a && b) {
        free(a);
        free(b);
        /* Request 800 bytes — fits in the merged region but not in either
         * 512-byte slot individually. */
        void *merged = malloc(800);
        check(merged != NULL,
              "FAIL: post-coalesce malloc failed (coalescing not working)");
        if (merged) free(merged);
    }

    if (g_ok) blyt_console_debug("arena tests passed");
}
void blyt_cart_update(void) { blyt_quit_ready(); }
void blyt_cart_draw(void)   {}
"#,
    );

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let output = Command::new(blytrun())
        .args(["--headless", cart.to_str().unwrap()])
        .env("BLYT_LIB_DIR", sdk_dir().join("lib"))
        .assert()
        .success()
        .get_output()
        .stdout
        .clone();

    let out = String::from_utf8_lossy(&output);
    assert!(
        !out.contains("FAIL:"),
        "arena boundary test failures:\n{out}"
    );
    assert!(
        out.contains("arena tests passed"),
        "expected 'arena tests passed' in output, got:\n{out}"
    );
}

/// Symbol-presence probe: build a cart that takes the address of every function
/// the SDK headers promise to provide.  If any symbol is absent from
/// libblyt32.so (which absorbs all libblytc.so sources), the link fails and
/// the test fails.  The cart never has to run — the build step is the test.
///
/// Coverage: allocator, key string functions, printf, stdlib conversions, and
/// the float math surface (sinf/cosf families).  Each category exercises a
/// distinct source group in the curated musl subset.
#[test]
fn libblytc_symbol_probe() {
    assert!(
        sdk_dir().join("lib/libblytc.so").exists(),
        "SDK not assembled — run `cmake --build build --target sdk` first"
    );
    assert!(
        sdk_dir().join("bin/blyt-clang").exists(),
        "SDK not assembled — run `cmake --build build --target sdk` first"
    );

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("symbol_probe");

    write_cart_project(
        &project,
        r#"
/* Symbol-presence probe for libblytc.so (ADR-0120).
 *
 * Takes the address of every function the SDK promises.  Using addresses
 * (not calls) ensures the linker must resolve the symbols without us needing
 * to supply correct arguments or run the cart.
 *
 * __attribute__((used)) on the table prevents the optimiser from discarding
 * the references before the linker sees them.
 */
#include "blyt.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <ctype.h>

__attribute__((used))
static void *const blytc_probe[] = {
    /* -- allocator -- */
    (void *)malloc, (void *)free, (void *)realloc, (void *)calloc,

    /* -- string / memory -- */
    (void *)memcpy,  (void *)memmove,  (void *)memset,  (void *)memcmp,
    (void *)memchr,  (void *)strlen,   (void *)strcmp,   (void *)strncmp,
    (void *)strcpy,  (void *)strncpy,  (void *)strcat,   (void *)strncat,
    (void *)strchr,  (void *)strrchr,  (void *)strstr,   (void *)strtok,

    /* -- stdlib conversions -- */
    (void *)strtol,  (void *)strtoul,  (void *)strtod,  (void *)strtof,
    (void *)atoi,    (void *)atol,     (void *)atof,
    (void *)qsort,   (void *)abs,

    /* -- printf (memory-backed only; no fd I/O) -- */
    (void *)snprintf, (void *)vsnprintf,

    /* -- ctype -- */
    (void *)isalpha, (void *)isdigit, (void *)isalnum,
    (void *)isspace, (void *)islower, (void *)isupper,
    (void *)tolower, (void *)toupper,

    /* -- float math (f32 surface, ADR-0005) -- */
    (void *)sinf,    (void *)cosf,    (void *)tanf,
    (void *)asinf,   (void *)acosf,   (void *)atanf,  (void *)atan2f,
    (void *)expf,    (void *)logf,    (void *)log2f,  (void *)log10f,
    (void *)powf,    (void *)sqrtf,   (void *)fabsf,
    (void *)floorf,  (void *)ceilf,   (void *)roundf, (void *)truncf,
    (void *)fmodf,   (void *)hypotf,  (void *)fmaf,
    (void *)copysignf, (void *)fmaxf, (void *)fminf,
    (void *)ldexpf,  (void *)frexpf,  (void *)modff,

    /* -- double math variants (completeness; no first-class use) -- */
    (void *)sin,     (void *)cos,     (void *)sqrt,
    (void *)exp,     (void *)log,     (void *)pow,
    (void *)fabs,    (void *)floor,   (void *)ceil,
};

void blyt_cart_init(void)   { (void)blytc_probe; }
void blyt_cart_update(void) { blyt_quit_ready(); }
void blyt_cart_draw(void)   {}
"#,
    );

    // A successful build (link step) is the assertion — no need to run.
    let cart = build_cart(&project);
    assert!(
        cart.exists(),
        "symbol probe cart not found at {}",
        cart.display()
    );
}

/// Path to the test_session_api binary produced by the CMake build.
fn test_session_api() -> std::path::PathBuf {
    build_dir().join("test_session_api")
}

/// Session API: drives a cart via blyt_session_create/run_frame/destroy directly
/// (not via the blyt_cart_run wrapper).  Verifies BLYT_RUN_FRAME_DONE is returned
/// at least once before the cart exits cleanly.
#[test]
fn session_api_run_frame() {
    assert!(
        sdk_dir().join("bin/blyt-clang").exists(),
        "SDK not assembled — run `cmake --build build --target sdk` first"
    );
    assert!(
        test_session_api().exists(),
        "test_session_api not built — run `cmake --build build` first"
    );

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("session_cart");
    write_cart_project(
        &project,
        r#"
#include "blyt.h"
static int s_frame = 0;
void blyt_cart_init(void)   { blyt_console_debug("session-init"); }
void blyt_cart_update(void) { if (++s_frame >= 2) blyt_quit_ready(); }
void blyt_cart_draw(void)   {}
"#,
    );

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let out = Command::new(test_session_api())
        .args([
            "session",
            cart.to_str().unwrap(),
            sdk_dir().join("lib").to_str().unwrap(),
        ])
        .assert()
        .success()
        .get_output()
        .stdout
        .clone();

    assert!(
        String::from_utf8_lossy(&out).contains("session-init"),
        "expected 'session-init' in output, got: {}",
        String::from_utf8_lossy(&out)
    );
}

/// Registry: loads the guest .so files into memory, registers them with
/// blyt_register_lib, then runs a cart without BLYT_LIB_DIR — exercising the
/// in-memory registry path through dynlink.
#[test]
fn registry_replaces_blyt_lib_dir() {
    assert!(
        sdk_dir().join("lib/libblyt32.so").exists(),
        "SDK not assembled — run `cmake --build build --target sdk` first"
    );
    assert!(
        sdk_dir().join("bin/blyt-clang").exists(),
        "SDK not assembled — run `cmake --build build --target sdk` first"
    );
    assert!(
        test_session_api().exists(),
        "test_session_api not built — run `cmake --build build` first"
    );

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("registry_cart");
    write_cart_project(
        &project,
        r#"
#include "blyt.h"
void blyt_cart_init(void)   { blyt_console_debug("registry-ok"); }
void blyt_cart_update(void) { blyt_quit_ready(); }
void blyt_cart_draw(void)   {}
"#,
    );

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let out = Command::new(test_session_api())
        .args([
            "registry",
            cart.to_str().unwrap(),
            sdk_dir().join("lib").to_str().unwrap(),
        ])
        // Explicitly clear BLYT_LIB_DIR to ensure the registry is the only source
        .env_remove("BLYT_LIB_DIR")
        .assert()
        .success()
        .get_output()
        .stdout
        .clone();

    assert!(
        String::from_utf8_lossy(&out).contains("registry-ok"),
        "expected 'registry-ok' in output, got: {}",
        String::from_utf8_lossy(&out)
    );
}
