mod common;

use assert_cmd::Command;
use common::{
    CartProject, blytdebug, build_debug_cart, debug_lib_dir, find_symbol_addr, lldb_dap_bin,
    repo_root, require_gdb, require_lldb_dap, require_sdk,
};
use std::time::Duration;
use tempfile::TempDir;

/* ── Test cart source ────────────────────────────────────────────────────── */

/// C source for LLDB-DAP tests.
///
/// Line numbers are significant — `breakLine()` returns the line of the
/// `local_val` assignment so tests can set a source breakpoint there.
/// The -ffile-prefix-map flag rewrites `<cwd>/` to `/blyt/src/` in DWARF;
/// LLDB's `settings set target.source-map /blyt/src <cwd>` reverses it.
const LLDB_TEST_C: &str = concat!(
    "#include \"blyt.h\"\n",                                              // 1
    "#include <stdint.h>\n",                                              // 2
    "volatile uint32_t g_counter = 0;\n",                                 // 3
    "void blyt_lldb_test_fn(void) {\n",                                   // 4
    "    volatile uint32_t local_val = 0xdeadbeef;\n",                    // 5  ← breakLine
    "    g_counter = local_val + 1;\n",                                   // 6
    "}\n",                                                                // 7
    "static int g_frame = 0;\n",                                          // 8
    "void blyt_cart_init(void)   { blyt_lldb_test_fn(); }\n",             // 9
    "void blyt_cart_update(void) { if (++g_frame >= 3) blyt_quit(); }\n", // 10
    "void blyt_cart_draw(void)   {}\n",                                   // 11
);

const BREAK_LINE: u32 = 5;

/* ── Helper: spawn blytplay with GDB, run an lldb-dap test script ────────── */

fn run_lldb_dap_test(test_name: &str, project: &std::path::Path, cart: &std::path::Path) {
    let lldb_dap = lldb_dap_bin().expect("lldb-dap not found");

    // Spawn blytplay --gdb 0 --headless <cart> with piped stdout/stderr so we
    // can extract the GDB port, then keep it alive for the lldb-dap session.
    let mut blytplay_proc = std::process::Command::new(blytdebug())
        .args(["--gdb", "0", "--headless", cart.to_str().unwrap()])
        .env("BLYT_LIB_DIR", debug_lib_dir())
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped())
        .spawn()
        .expect("blytplay spawn");

    // Read lines until "GDB listening on port N" or timeout.
    use std::io::{BufRead, BufReader};
    let mut gdb_port: Option<u16> = None;
    let stdout = blytplay_proc.stdout.take().unwrap();
    let mut stderr = blytplay_proc.stderr.take().unwrap();

    // Drain stderr in a background thread so it doesn't block the process.
    std::thread::spawn(move || {
        let _ = std::io::copy(&mut stderr, &mut std::io::sink());
    });

    // Read stdout until the port announcement or EOF.
    let stdout_reader = BufReader::new(stdout);
    let (tx, rx) = std::sync::mpsc::channel();
    std::thread::spawn(move || {
        for line in stdout_reader.lines().flatten() {
            if let Some(idx) = line.find("GDB listening on port ") {
                let rest = &line[idx + "GDB listening on port ".len()..];
                let end = rest
                    .find(|c: char| !c.is_ascii_digit())
                    .unwrap_or(rest.len());
                if let Ok(p) = rest[..end].parse::<u16>() {
                    let _ = tx.send(p);
                    return;
                }
            }
        }
        // Process exited without printing port.
        drop(tx);
    });

    // Wait up to 10s for the port.
    match rx.recv_timeout(Duration::from_secs(10)) {
        Ok(p) => gdb_port = Some(p),
        Err(_) => {
            let _ = blytplay_proc.kill();
            panic!("blytplay did not announce GDB port within 10s");
        }
    }

    let port = gdb_port.unwrap();

    // Run the Node.js DAP test client.
    let orchestrator = repo_root().join("tests/dap/run_lldb_dap_test.mjs");
    let result = Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            lldb_dap.to_str().unwrap(),
            &port.to_string(),
            cart.to_str().unwrap(),
            project.to_str().unwrap(),
            "--test",
            test_name,
        ])
        .env("BLYT_GDB_BREAK_LINE", BREAK_LINE.to_string())
        .env("BLYT_SOURCE_FILE", "src/game/c/main.c")
        .timeout(Duration::from_secs(30))
        .assert();

    let _ = blytplay_proc.kill();

    result.success();
}

/* ── Tests ───────────────────────────────────────────────────────────────── */

/// LLDB-DAP: initialize handshake with the GDB RSP relay.
///
/// Spawns blytplay with --gdb, connects lldb-dap to the relay, sends
/// `initialize`, and verifies non-empty capabilities are returned.
///
/// Requires: blytplay with BLYT_GDB=ON, SDK, lldb-dap (or blyt-lldb-dap).
#[test]
fn sdl_native_lldb_dap_initialize() {
    require_sdk();
    require_gdb();
    require_lldb_dap();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("lldb_init");
    CartProject::new().c(LLDB_TEST_C).write(&project);
    let cart = build_debug_cart(&project);
    assert!(cart.exists());

    run_lldb_dap_test("initialize", &project, &cart);
}

/// LLDB-DAP: set a source breakpoint by file:line and verify it fires.
///
/// After connecting to the GDB relay via lldb-dap, sets a breakpoint at
/// BREAK_LINE in the source file (using the /blyt/src canonical path) and
/// verifies a 'stopped' event with reason 'breakpoint' arrives.
///
/// Requires: blytplay with BLYT_GDB=ON, SDK, lldb-dap, `readelf` for DWARF.
#[test]
fn sdl_native_lldb_dap_source_breakpoint() {
    require_sdk();
    require_gdb();
    require_lldb_dap();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("lldb_src_bp");
    CartProject::new().c(LLDB_TEST_C).write(&project);
    let cart = build_debug_cart(&project);
    assert!(cart.exists());

    // Verify DWARF is present by checking we can find the test function.
    let addr = find_symbol_addr(&cart, "blyt_lldb_test_fn");
    if addr.is_none() {
        // DWARF not available — skip source-level test.
        println!("skipping: blyt_lldb_test_fn symbol not found (readelf unavailable)");
        return;
    }

    run_lldb_dap_test("source-breakpoint", &project, &cart);
}

/// LLDB-DAP: stopOnEntry:false — cart auto-continues; first stop is a breakpoint.
///
/// Verifies that with `stopOnEntry: false` the runtime does not pause at entry
/// and instead runs until the first user breakpoint fires.
///
/// Requires: blytplay with BLYT_GDB=ON, SDK, lldb-dap, `readelf` for DWARF.
#[test]
fn sdl_native_lldb_dap_auto_start() {
    require_sdk();
    require_gdb();
    require_lldb_dap();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("lldb_autostart");
    CartProject::new().c(LLDB_TEST_C).write(&project);
    let cart = build_debug_cart(&project);
    assert!(cart.exists());

    let addr = find_symbol_addr(&cart, "blyt_lldb_test_fn");
    if addr.is_none() {
        println!("skipping: blyt_lldb_test_fn not found (readelf unavailable)");
        return;
    }

    run_lldb_dap_test("auto-start", &project, &cart);
}

/// LLDB-DAP: stackTrace shows the correct function name after a stop.
///
/// After the breakpoint fires, sends a `stackTrace` request and verifies
/// frame 0 has a non-empty, non-'??' function name.
///
/// Requires: blytplay with BLYT_GDB=ON, SDK, lldb-dap.
#[test]
fn sdl_native_lldb_dap_stack_trace() {
    require_sdk();
    require_gdb();
    require_lldb_dap();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("lldb_stack");
    CartProject::new().c(LLDB_TEST_C).write(&project);
    let cart = build_debug_cart(&project);
    assert!(cart.exists());

    let addr = find_symbol_addr(&cart, "blyt_lldb_test_fn");
    if addr.is_none() {
        println!("skipping: blyt_lldb_test_fn not found");
        return;
    }

    run_lldb_dap_test("stack-trace", &project, &cart);
}

/// LLDB-DAP: scopes/variables returns the local variable scope.
///
/// After stopping at the breakpoint, requests scopes and variables for
/// frame 0.  Asserts that the response is well-formed; if DWARF has local
/// variable info, asserts at least one variable is present.
///
/// Requires: blytplay with BLYT_GDB=ON, SDK, lldb-dap.
#[test]
fn sdl_native_lldb_dap_variables() {
    require_sdk();
    require_gdb();
    require_lldb_dap();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("lldb_vars");
    CartProject::new().c(LLDB_TEST_C).write(&project);
    let cart = build_debug_cart(&project);
    assert!(cart.exists());

    let addr = find_symbol_addr(&cart, "blyt_lldb_test_fn");
    if addr.is_none() {
        println!("skipping: blyt_lldb_test_fn not found");
        return;
    }

    run_lldb_dap_test("variables", &project, &cart);
}

/// LLDB-DAP: source-map maps /blyt/src paths back to the project directory.
///
/// Verifies that `settings set target.source-map /blyt/src <cwd>` in the
/// launchCommands causes stackTrace source paths to use the actual project
/// directory rather than the canonical /blyt/src prefix embedded in DWARF.
///
/// Requires: blytplay with BLYT_GDB=ON, SDK, lldb-dap.
#[test]
fn sdl_native_lldb_dap_source_map() {
    require_sdk();
    require_gdb();
    require_lldb_dap();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("lldb_srcmap");
    CartProject::new().c(LLDB_TEST_C).write(&project);
    let cart = build_debug_cart(&project);
    assert!(cart.exists());

    let addr = find_symbol_addr(&cart, "blyt_lldb_test_fn");
    if addr.is_none() {
        println!("skipping: blyt_lldb_test_fn not found");
        return;
    }

    run_lldb_dap_test("source-map", &project, &cart);
}
