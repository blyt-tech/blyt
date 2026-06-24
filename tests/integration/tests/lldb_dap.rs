mod common;

use assert_cmd::Command;
use common::{
    CartProject, blytdebug, build_debug_cart, lldb_dap_bin, repo_root, require_gdb,
    require_lldb_dap, require_rust_riscv_target, require_sdk, require_symbol_addr,
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
    run_lldb_dap_test_env(test_name, project, cart, &[]);
}

/// As `run_lldb_dap_test`, with extra environment for the Node driver (e.g.
/// BLYT_SDK_BREAK_FILE/LINE for the sdk-source-breakpoint scenario).
fn run_lldb_dap_test_env(
    test_name: &str,
    project: &std::path::Path,
    cart: &std::path::Path,
    extra_env: &[(&str, String)],
) {
    let lldb_dap = lldb_dap_bin().expect("lldb-dap not found");

    // Spawn blytplay --gdb 0 --headless <cart> with piped stdout/stderr so we
    // can extract the GDB port, then keep it alive for the lldb-dap session.
    let mut blytplay_proc = std::process::Command::new(blytdebug())
        .args(["--gdb", "0", "--headless", cart.to_str().unwrap()])
        .env("BLYT_TRACE", "gdb,dap,lifecycle,frame")
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped())
        .spawn()
        .expect("blytplay spawn");

    // Read lines until "GDB listening on port N" or timeout.
    use std::io::{BufRead, BufReader};
    let stdout = blytplay_proc.stdout.take().unwrap();
    let mut stderr = blytplay_proc.stderr.take().unwrap();

    // Capture stderr (carries the BLYT_TRACE protocol trace) in a background
    // thread so it doesn't block the process; printed on failure below.
    let stderr_buf = std::sync::Arc::new(std::sync::Mutex::new(Vec::new()));
    {
        let stderr_buf = stderr_buf.clone();
        std::thread::spawn(move || {
            use std::io::Read;
            let mut chunk = [0u8; 4096];
            loop {
                match stderr.read(&mut chunk) {
                    Ok(0) | Err(_) => break,
                    Ok(n) => stderr_buf.lock().unwrap().extend_from_slice(&chunk[..n]),
                }
            }
        });
    }

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
    let port = match rx.recv_timeout(Duration::from_secs(10)) {
        Ok(p) => p,
        Err(_) => {
            let _ = blytplay_proc.kill();
            panic!("blytplay did not announce GDB port within 10s");
        }
    };

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
        // lldb-dap `program` = the stub ELF, never the cart (issue #119): the
        // cart is presented purely as a shared library so it stays cleanly
        // unloadable/reloadable across a hot reload.
        .env(
            "BLYT_STUB_PROGRAM",
            repo_root().join("build/sdk/lib/debug/blyt-debug-stub.elf"),
        )
        .envs(extra_env.iter().map(|(k, v)| (*k, v.as_str())))
        .timeout(Duration::from_secs(30))
        .assert();

    let _ = blytplay_proc.kill();

    if let Err(e) = result.try_success() {
        let captured = stderr_buf.lock().unwrap();
        eprintln!(
            "--- blytdebug stderr (BLYT_TRACE) ---\n{}",
            String::from_utf8_lossy(&captured)
        );
        panic!("{e}");
    }
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
    // Asserts symbol lookup works (DWARF/symtab present); the value itself
    // is not needed — lldb-dap sets the breakpoint by source line.
    require_symbol_addr(&cart, "blyt_lldb_test_fn");

    run_lldb_dap_test("source-breakpoint", &project, &cart);
}

/// LLDB-DAP (issue #119, acceptance criterion 2): a breakpoint set before
/// launch is BOUND with a concrete address at attach — before init() runs — not
/// left pending until the first reload.
///
/// With the cart presented purely as a shared library (lldb-dap `program` = the
/// stub ELF), this proves the cart-library is announced at attach so lldb reads
/// the svr4 list and resolves cart breakpoints up front. The driver asserts the
/// `setBreakpoints` response (sent before `configurationDone`) reports the
/// breakpoint verified with an `instructionReference`.
///
/// Requires: blytplay with BLYT_GDB=ON, SDK, lldb-dap, `readelf` for DWARF.
#[test]
fn sdl_native_lldb_dap_breakpoint_bound_at_attach() {
    require_sdk();
    require_gdb();
    require_lldb_dap();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("lldb_attach_bind");
    CartProject::new().c(LLDB_TEST_C).write(&project);
    let cart = build_debug_cart(&project);
    assert!(cart.exists());
    require_symbol_addr(&cart, "blyt_lldb_test_fn");

    run_lldb_dap_test("attach-bind", &project, &cart);
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

    // Asserts symbol lookup works (DWARF/symtab present); the value itself
    // is not needed — lldb-dap sets the breakpoint by source line.
    require_symbol_addr(&cart, "blyt_lldb_test_fn");

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

    // Asserts symbol lookup works (DWARF/symtab present); the value itself
    // is not needed — lldb-dap sets the breakpoint by source line.
    require_symbol_addr(&cart, "blyt_lldb_test_fn");

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

    // Asserts symbol lookup works (DWARF/symtab present); the value itself
    // is not needed — lldb-dap sets the breakpoint by source line.
    require_symbol_addr(&cart, "blyt_lldb_test_fn");

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

    // Asserts symbol lookup works (DWARF/symtab present); the value itself
    // is not needed — lldb-dap sets the breakpoint by source line.
    require_symbol_addr(&cart, "blyt_lldb_test_fn");

    run_lldb_dap_test("source-map", &project, &cart);
}

/// LLDB-DAP: a breakpoint *inside SDK-shipped source* binds and fires (#48 item 2).
///
/// Sets a breakpoint by the canonical `/blyt/sdk/rust/blyt/src/lib.rs:<line>`
/// path — the statically-linked blyt SDK crate, whose DWARF travels in the cart
/// — and verifies it binds (against the SDK DWARF) and fires (the cart calls
/// `console_debug`).  Proves the shipped SDK source + the manifest source-map
/// resolve end to end, not just the cart's own source.  The line is resolved
/// from the shipped crate so it tracks edits and is HOME-independent.
#[test]
fn sdl_native_lldb_dap_sdk_source_breakpoint() {
    require_sdk();
    require_gdb();
    require_lldb_dap();
    require_rust_riscv_target();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("rust_sdk_bp");
    CartProject::new()
        .rust(
            "#![no_std]\n\
             use blyt::*;\n\
             #[no_mangle] pub extern \"C\" fn blyt_cart_init() { console_debug(\"sdk bp\"); }\n\
             #[no_mangle] pub extern \"C\" fn blyt_cart_update() { quit(); }\n\
             #[no_mangle] pub extern \"C\" fn blyt_cart_draw() {}\n",
        )
        .write(&project);
    let cart = build_debug_cart(&project);

    // Resolve console_debug's first body statement in the shipped SDK crate.
    let lib_rs = repo_root().join("sdk/rust/blyt/src/lib.rs");
    let src = std::fs::read_to_string(&lib_rs).expect("read SDK lib.rs");
    let sig = src
        .lines()
        .position(|l| l.contains("pub fn console_debug(s: &str)"))
        .expect("console_debug not found in SDK crate");
    let break_line = sig + 2; // 1-based line of the first body statement

    run_lldb_dap_test_env(
        "sdk-source-breakpoint",
        &project,
        &cart,
        &[
            (
                "BLYT_SDK_BREAK_FILE",
                "/blyt/sdk/rust/blyt/src/lib.rs".to_string(),
            ),
            ("BLYT_SDK_BREAK_LINE", break_line.to_string()),
        ],
    );
}

/* ── Reload-while-debugging (issue #119) ─────────────────────────────────── */

/// C source whose `blyt_lldb_test_fn` sits at a *shiftable* address: `pad` grows
/// `blyt_pad_fn` before it, moving the following function while keeping line
/// numbers stable (the breakpoint line stays valid across v1/v2). The function
/// is referenced (via the volatile `g_keep` pointer) but never called, so its
/// breakpoint resolves yet never fires — the cart keeps running update() and
/// services the dev-control channel across the reload.
fn reload_cart_src(pad: &str) -> String {
    format!(
        "#include \"blyt.h\"\n\
         #include <stdint.h>\n\
         volatile uint32_t g_counter = 0;\n\
         void blyt_pad_fn(void) {{ {pad} }}\n\
         void blyt_lldb_test_fn(void) {{\n\
         \x20\x20\x20\x20volatile uint32_t local_val = 0xdeadbeef;\n\
         \x20\x20\x20\x20g_counter = local_val + 1;\n\
         }}\n\
         void (*volatile g_keep)(void) = blyt_lldb_test_fn;\n\
         void blyt_cart_init(void)   {{ blyt_pad_fn(); }}\n\
         void blyt_cart_update(void) {{ if (g_keep) g_counter++; }}\n\
         void blyt_cart_draw(void)   {{}}\n"
    )
}

/// LLDB-DAP (issue #119, acceptance criterion 1): a reload-while-debugging
/// rebinds a breakpoint to the NEW code's address — single location, no stale
/// old location.
///
/// Builds two carts whose `blyt_lldb_test_fn` sits at different addresses (v2
/// has a fatter pad function), starts a debug session against v1, sets a source
/// breakpoint, then drives the REAL reload (dev-control `reload` pointing at v2).
/// The runtime swaps the cart in place at a fresh base, re-reports it at v2's
/// path, and fires the solib event; the driver asserts the breakpoint rebinds to
/// v2's re-read address (not the stale v1 address, nor a mere relocation).
///
/// Requires: blytdebug with BLYT_GDB=ON, SDK, lldb-dap.
#[test]
fn sdl_native_lldb_dap_reload_rebinds_breakpoint() {
    require_sdk();
    require_gdb();
    require_lldb_dap();

    const BREAK_LINE: u32 = 6;
    const SOURCE_FILE: &str = "src/game/c/main.c";

    let tmp = TempDir::new().unwrap();
    let v1_pad = "g_counter += 1;".to_string();
    let v2_pad = (0..60)
        .map(|i| format!("g_counter += {i};"))
        .collect::<Vec<_>>()
        .join(" ");

    let v1_dir = tmp.path().join("v1");
    let v2_dir = tmp.path().join("v2");
    CartProject::new()
        .c(&reload_cart_src(&v1_pad))
        .write(&v1_dir);
    CartProject::new()
        .c(&reload_cart_src(&v2_pad))
        .write(&v2_dir);
    let v1_cart = build_debug_cart(&v1_dir);
    let v2_cart = build_debug_cart(&v2_dir);

    let a1 = require_symbol_addr(&v1_cart, "blyt_lldb_test_fn");
    let a2 = require_symbol_addr(&v2_cart, "blyt_lldb_test_fn");
    assert_ne!(a1, a2, "test setup broken: the function did not move v1→v2");

    // Spawn blytdebug with both a GDB port and a dev-control port (listen mode).
    let mut proc = std::process::Command::new(blytdebug())
        .args([
            "--gdb",
            "0",
            "--dev-ctrl-port",
            "0",
            "--headless",
            v1_cart.to_str().unwrap(),
        ])
        .env("BLYT_TRACE", "gdb,dap,lifecycle,frame")
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped())
        .spawn()
        .expect("blytdebug spawn");

    // Drain stderr (carries the BLYT_TRACE RSP trace) so the pipe never blocks.
    let dbg_stderr = std::sync::Arc::new(std::sync::Mutex::new(Vec::new()));
    {
        let mut se = proc.stderr.take().unwrap();
        let buf = dbg_stderr.clone();
        std::thread::spawn(move || {
            use std::io::Read;
            let mut chunk = [0u8; 4096];
            while let Ok(n) = se.read(&mut chunk) {
                if n == 0 {
                    break;
                }
                buf.lock().unwrap().extend_from_slice(&chunk[..n]);
            }
        });
    }

    // Parse both ports from stdout (dev-control is announced up front, #119).
    use std::io::{BufRead, BufReader};
    let stdout = proc.stdout.take().unwrap();
    let (tx, rx) = std::sync::mpsc::channel();
    std::thread::spawn(move || {
        let mut gdb_port = None;
        let mut dev_port = None;
        for line in BufReader::new(stdout).lines().map_while(Result::ok) {
            if let Some(i) = line.find("GDB listening on port ") {
                let rest = &line[i + "GDB listening on port ".len()..];
                let end = rest
                    .find(|c: char| !c.is_ascii_digit())
                    .unwrap_or(rest.len());
                gdb_port = rest[..end].parse::<u16>().ok();
            }
            if let Some(i) = line.find("Dev control: listening on 127.0.0.1:") {
                let rest = &line[i + "Dev control: listening on 127.0.0.1:".len()..];
                let end = rest
                    .find(|c: char| !c.is_ascii_digit())
                    .unwrap_or(rest.len());
                dev_port = rest[..end].parse::<u16>().ok();
            }
            if let (Some(g), Some(d)) = (gdb_port, dev_port) {
                let _ = tx.send((g, d));
                return;
            }
        }
    });
    let (gdb_port, dev_port) = rx
        .recv_timeout(Duration::from_secs(10))
        .expect("blytdebug did not announce both GDB and dev-control ports");

    let lldb_dap = lldb_dap_bin().expect("lldb-dap not found");
    let driver = repo_root().join("tests/dap/run_lldb_dap_test.mjs");
    let result = Command::new("node")
        .args([
            driver.to_str().unwrap(),
            lldb_dap.to_str().unwrap(),
            &gdb_port.to_string(),
            v1_cart.to_str().unwrap(),
            v1_dir.to_str().unwrap(),
            "--test",
            "reload-rebind",
        ])
        .env("BLYT_GDB_BREAK_LINE", BREAK_LINE.to_string())
        .env("BLYT_SOURCE_FILE", SOURCE_FILE)
        .env(
            "BLYT_STUB_PROGRAM",
            repo_root().join("build/sdk/lib/debug/blyt-debug-stub.elf"),
        )
        .env("BLYT_DEV_CTRL_PORT", dev_port.to_string())
        .env("BLYT_V2_CART", v2_cart.to_str().unwrap())
        .env("BLYT_V1_FUNC", a1.to_string())
        .env("BLYT_V2_FUNC", a2.to_string())
        // First reload re-maps the cart at BLYT_RELOAD_BASE_A (80 MiB).
        .env("BLYT_REMAP_BASE", (0x05000000u32).to_string())
        .timeout(Duration::from_secs(40))
        .assert();

    let out = result.get_output().clone();
    // Let the stderr-drain thread flush the final RSP packets, then stop the VM.
    std::thread::sleep(Duration::from_millis(300));
    let trace = String::from_utf8_lossy(&dbg_stderr.lock().unwrap()).to_string();
    let _ = proc.kill();

    if let Err(e) = result.try_success() {
        eprintln!(
            "--- driver stdout ---\n{}\n--- blytdebug stderr ---\n{trace}",
            String::from_utf8_lossy(&out.stdout),
        );
        panic!("{e}");
    }

    // Ground truth is the GDB-RSP trace (the DAP re-query is misleading, §5c):
    // replay the Z0 (insert) / z0 (remove) breakpoint packets to the final
    // armed set, restricted to cart-code addresses (below the 128 MiB runtime
    // library base).  After the reload it must be EXACTLY the re-read v2 address
    // — a single clean location, no stale v1 location (acceptance criteria 1+3).
    const GUEST_LIB_BASE: u64 = 0x0800_0000;
    let mut armed: std::collections::BTreeSet<u64> = std::collections::BTreeSet::new();
    let mut first_bp: Option<u64> = None;
    for line in trace.lines() {
        for (marker, insert) in [("recv Z0,", true), ("recv z0,", false)] {
            if let Some(i) = line.find(marker) {
                let rest = &line[i + marker.len()..];
                let hex = rest.split(',').next().unwrap_or("");
                if let Ok(addr) = u64::from_str_radix(hex, 16) {
                    if insert {
                        if first_bp.is_none() {
                            first_bp = Some(addr);
                        }
                        armed.insert(addr);
                    } else {
                        armed.remove(&addr);
                    }
                }
            }
        }
    }
    let a1_resolved = first_bp.unwrap_or_else(|| panic!("no breakpoint set in trace:\n{trace}"));
    let line_off = a1_resolved - a1; // line-6 offset within blyt_lldb_test_fn
    let reread = 0x0500_0000 + a2 + line_off; // base A + v2 function + line offset
    let cart_armed: Vec<u64> = armed
        .iter()
        .copied()
        .filter(|&a| a < GUEST_LIB_BASE)
        .collect();

    assert_eq!(
        cart_armed,
        vec![reread],
        "after reload the breakpoint must be a single location at the re-read v2 \
         address 0x{reread:x} (a1_resolved=0x{a1_resolved:x} v1_fn=0x{a1:x} v2_fn=0x{a2:x}); \
         got {cart_armed:x?}.\n--- trace ---\n{trace}"
    );
}
