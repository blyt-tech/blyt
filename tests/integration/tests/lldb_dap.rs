mod common;

use assert_cmd::Command;
use common::{
    CartProject, blytdebug, build_debug_cart, build_debug_lua_cart, find_wasm_debug_dir,
    lldb_dap_bin, repo_root, require_gdb, require_lldb_dap, require_lua_sdk,
    require_rust_riscv_target, require_sdk, require_symbol_addr, require_wasm_debug,
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

/// Spawn `blytdebug --gdb 0 --dev-ctrl-port 0 --headless <cart>` and return the
/// child, its GDB and dev-control ports (announced up front, #119), and a buffer
/// that accumulates its BLYT_TRACE stderr (the GDB-RSP trace, the reload oracle).
fn spawn_blytdebug_dual_port(
    cart: &std::path::Path,
) -> (
    std::process::Child,
    u16,
    u16,
    std::sync::Arc<std::sync::Mutex<Vec<u8>>>,
) {
    let mut proc = std::process::Command::new(blytdebug())
        .args([
            "--gdb",
            "0",
            "--dev-ctrl-port",
            "0",
            "--headless",
            cart.to_str().unwrap(),
        ])
        .env("BLYT_TRACE", "gdb,dap,lifecycle,frame")
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped())
        .spawn()
        .expect("blytdebug spawn");

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
    (proc, gdb_port, dev_port, dbg_stderr)
}

/// Replay the GDB-RSP Z0 (insert) / z0 (remove) breakpoint packets in `trace` to
/// the final armed set, restricted to cart-code addresses (below the 128 MiB
/// runtime library base).  This is the ground-truth reload oracle — the DAP
/// setBreakpoints re-query is misleading (Spike W §5c).
fn final_armed_cart_bps(trace: &str) -> Vec<u64> {
    const GUEST_LIB_BASE: u64 = 0x0800_0000;
    let mut armed: std::collections::BTreeSet<u64> = std::collections::BTreeSet::new();
    for line in trace.lines() {
        for (marker, insert) in [("recv Z0,", true), ("recv z0,", false)] {
            if let Some(i) = line.find(marker) {
                let hex = line[i + marker.len()..].split(',').next().unwrap_or("");
                if let Ok(addr) = u64::from_str_radix(hex, 16) {
                    if insert {
                        armed.insert(addr);
                    } else {
                        armed.remove(&addr);
                    }
                }
            }
        }
    }
    armed.into_iter().filter(|&a| a < GUEST_LIB_BASE).collect()
}

/// First breakpoint address inserted (the initial attach bind) in `trace`.
fn first_armed_bp(trace: &str) -> Option<u64> {
    for line in trace.lines() {
        if let Some(i) = line.find("recv Z0,") {
            let hex = line[i + "recv Z0,".len()..].split(',').next().unwrap_or("");
            if let Ok(addr) = u64::from_str_radix(hex, 16) {
                return Some(addr);
            }
        }
    }
    None
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

    let (mut proc, gdb_port, dev_port, dbg_stderr) = spawn_blytdebug_dual_port(&v1_cart);

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

    // Ground truth is the GDB-RSP trace (the DAP re-query is misleading, §5c).
    // After the reload the cart breakpoint must be EXACTLY the re-read v2 address
    // — a single clean location, no stale v1 location (acceptance criteria 1+3).
    let cart_armed = final_armed_cart_bps(&trace);
    let a1_resolved = first_armed_bp(&trace).expect("no breakpoint set in trace");
    let line_off = a1_resolved - a1; // line-6 offset within blyt_lldb_test_fn
    let reread = 0x0500_0000 + a2 + line_off; // base A + v2 function + line offset

    assert_eq!(
        cart_armed,
        vec![reread],
        "after reload the breakpoint must be a single location at the re-read v2 \
         address 0x{reread:x} (a1_resolved=0x{a1_resolved:x} v1_fn=0x{a1:x} v2_fn=0x{a2:x}); \
         got {cart_armed:x?}.\n--- trace ---\n{trace}"
    );
}

/// LLDB-DAP (issue #119, acceptance criterion 3): N consecutive reloads leave
/// exactly ONE breakpoint location — no stale accumulation.
///
/// Builds carts whose `blyt_lldb_test_fn` sits at distinct addresses, then drives
/// a sequence of reloads cycling through them. The runtime ping-pongs the cart
/// between two fresh guest bases and performs the two-phase solib swap each time;
/// after the final reload the GDB-RSP trace must show a single armed cart
/// breakpoint at a fresh base (not the original base-0 location, and not an
/// accumulation of stale locations). N is kept modest so the libblytc arena does
/// not exhaust across reloads (#133, deferred).
///
/// Requires: blytdebug with BLYT_GDB=ON, SDK, lldb-dap.
#[test]
fn sdl_native_lldb_dap_n_reloads_single_location() {
    require_sdk();
    require_gdb();
    require_lldb_dap();

    const BREAK_LINE: u32 = 6;
    const SOURCE_FILE: &str = "src/game/c/main.c";

    let tmp = TempDir::new().unwrap();
    // Three carts with growing pad → distinct blyt_lldb_test_fn addresses.
    let pads = [
        "g_counter += 1;".to_string(),
        (0..40)
            .map(|i| format!("g_counter += {i};"))
            .collect::<Vec<_>>()
            .join(" "),
        (0..80)
            .map(|i| format!("g_counter += {i};"))
            .collect::<Vec<_>>()
            .join(" "),
    ];
    let dirs: Vec<_> = (0..3).map(|i| tmp.path().join(format!("v{i}"))).collect();
    let carts: Vec<_> = pads
        .iter()
        .zip(&dirs)
        .map(|(pad, dir)| {
            CartProject::new().c(&reload_cart_src(pad)).write(dir);
            build_debug_cart(dir)
        })
        .collect();

    // Reload sequence (4 reloads, modest re #133): cycle v1,v2,v1,v2 over the v0
    // session so each reload moves the function and re-maps at a fresh base.
    let seq = [&carts[1], &carts[2], &carts[1], &carts[2]];
    let reload_csv = seq
        .iter()
        .map(|c| c.to_str().unwrap())
        .collect::<Vec<_>>()
        .join(",");

    let (mut proc, gdb_port, dev_port, dbg_stderr) = spawn_blytdebug_dual_port(&carts[0]);

    let lldb_dap = lldb_dap_bin().expect("lldb-dap not found");
    let driver = repo_root().join("tests/dap/run_lldb_dap_test.mjs");
    let result = Command::new("node")
        .args([
            driver.to_str().unwrap(),
            lldb_dap.to_str().unwrap(),
            &gdb_port.to_string(),
            carts[0].to_str().unwrap(),
            dirs[0].to_str().unwrap(),
            "--test",
            "reload-n",
        ])
        .env("BLYT_GDB_BREAK_LINE", BREAK_LINE.to_string())
        .env("BLYT_SOURCE_FILE", SOURCE_FILE)
        .env(
            "BLYT_STUB_PROGRAM",
            repo_root().join("build/sdk/lib/debug/blyt-debug-stub.elf"),
        )
        .env("BLYT_DEV_CTRL_PORT", dev_port.to_string())
        .env("BLYT_RELOAD_CARTS", &reload_csv)
        .timeout(Duration::from_secs(60))
        .assert();

    let out = result.get_output().clone();
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

    // No accumulation: exactly one armed cart breakpoint, and it is at a fresh
    // reload base (>= 80 MiB) — i.e. it rebound to a reloaded cart, not the
    // original base-0 location, and stale locations were removed each round.
    let cart_armed = final_armed_cart_bps(&trace);
    assert_eq!(
        cart_armed.len(),
        1,
        "after {} reloads the breakpoint must have exactly one location, got {cart_armed:x?}.\n\
         --- trace ---\n{trace}",
        seq.len()
    );
    assert!(
        cart_armed[0] >= 0x0500_0000,
        "the surviving location 0x{:x} is not at a fresh reload base — it did not \
         rebind to a reloaded cart.\n--- trace ---\n{trace}",
        cart_armed[0]
    );
}

/* ── Hybrid reload-while-debugging (issue #119, criteria 4 + 5) ──────────── */

/// Hybrid (Lua + C) cart whose Lua-exported native function `blyt_native_init_work`
/// sits at a *shiftable* address (`pad` grows `blyt_pad_fn`, kept alive by a tail
/// call so the linker retains it before the exported function).  Lua `init()`
/// calls the native function, so a breakpoint inside it FIRES during init() — on
/// both first launch and after a reload.  Line numbers are stable so the native
/// source breakpoint (line 6, the `local_val` assignment) stays valid v1→v2.
fn reload_hybrid_c(pad: &str) -> String {
    format!(
        "#include \"blyt.h\"\n\
         #include <stdint.h>\n\
         volatile uint32_t g_counter = 0;\n\
         void blyt_pad_fn(void) {{ {pad} }}\n\
         BLYT_LUA_EXPORT_VOID(blyt_native_init_work) {{\n\
         \x20\x20\x20\x20volatile uint32_t local_val = 0xdeadbeef;\n\
         \x20\x20\x20\x20g_counter = local_val + 1;\n\
         \x20\x20\x20\x20blyt_pad_fn();\n\
         }}\n"
    )
}

/// Lua side of the hybrid reload cart: `init()` calls the native function at
/// line 3 (the Lua breakpoint; the native breakpoint fires inside that call).
/// No `blyt.quit()` — the cart idles in `update()` so it stays alive to service
/// the dev-control reload and re-run `init()`.
const RELOAD_HYBRID_LUA: &str = "\
function init()\n\
    local x = 0\n\
    blyt_native_init_work()\n\
end\n\
function update() end\n\
function draw()   end\n";

/// Spawn `blytdebug --debug 0 --gdb 0 --dev-ctrl-port 0 --headless <cart>` and
/// return the child, its DAP (Lua), GDB (native) and dev-control ports, and a
/// buffer accumulating its BLYT_TRACE stderr.
fn spawn_blytdebug_hybrid_ports(
    cart: &std::path::Path,
) -> (
    std::process::Child,
    u16,
    u16,
    u16,
    std::sync::Arc<std::sync::Mutex<Vec<u8>>>,
) {
    let mut proc = std::process::Command::new(blytdebug())
        .args([
            "--debug",
            "0",
            "--gdb",
            "0",
            "--dev-ctrl-port",
            "0",
            "--headless",
            cart.to_str().unwrap(),
        ])
        .env("BLYT_TRACE", "gdb,dap,lifecycle,frame")
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped())
        .spawn()
        .expect("blytdebug spawn");

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

    use std::io::{BufRead, BufReader};
    let stdout = proc.stdout.take().unwrap();
    let (tx, rx) = std::sync::mpsc::channel();
    std::thread::spawn(move || {
        let port_after = |line: &str, marker: &str| -> Option<u16> {
            let i = line.find(marker)?;
            let rest = &line[i + marker.len()..];
            let end = rest
                .find(|c: char| !c.is_ascii_digit())
                .unwrap_or(rest.len());
            rest[..end].parse::<u16>().ok()
        };
        let (mut dap, mut gdb, mut dev) = (None, None, None);
        let mut sent = false;
        // Keep draining stdout for the whole run: the cart/runtime may print
        // after the ports are announced, and if this reader stops the stdout
        // pipe fills and blocks blytdebug mid-init() (the cart never reaches
        // its breakpoints).
        for line in BufReader::new(stdout).lines().map_while(Result::ok) {
            if sent {
                continue;
            }
            if let Some(p) = port_after(&line, "DAP listening on port ") {
                dap = Some(p);
            }
            if let Some(p) = port_after(&line, "GDB listening on port ") {
                gdb = Some(p);
            }
            if let Some(p) = port_after(&line, "Dev control: listening on 127.0.0.1:") {
                dev = Some(p);
            }
            if let (Some(a), Some(g), Some(d)) = (dap, gdb, dev) {
                let _ = tx.send((a, g, d));
                sent = true;
            }
        }
    });
    let (dap_port, gdb_port, dev_port) = rx
        .recv_timeout(Duration::from_secs(10))
        .expect("blytdebug did not announce DAP, GDB and dev-control ports");
    (proc, gdb_port, dap_port, dev_port, dbg_stderr)
}

/// LLDB-DAP (issue #119, acceptance criteria 4 + 5): in a single HYBRID debug
/// session — native lldb-dap + companion Lua DAP, both live at once — a
/// breakpoint in `init()` on BOTH the Lua and native sides fires again after a
/// hot reload, and neither client is torn down across the reload.
///
/// Builds two hybrid carts whose native `init()`-called function sits at
/// different addresses, starts the dual-client session against v1 (both init()
/// breakpoints fire at startup), drives the REAL dev-control `reload` to v2, and
/// asserts both init() breakpoints fire AGAIN after the reload (proving the
/// post-reload gate armed both views before init() ran) while both DAP
/// connections stay responsive (the session/connection persists).
///
/// The native view auto-continues lldb's library-change (exception/SIGTRAP)
/// stops from the two-phase solib swap, mirroring the extension's reload-window
/// behaviour.
///
/// Requires: blytdebug with BLYT_DAP=ON and BLYT_GDB=ON, Lua SDK, lldb-dap.
#[test]
fn sdl_hybrid_lldb_dap_reload_fires_both_init_breakpoints() {
    require_sdk();
    require_lua_sdk();
    require_gdb();
    require_lldb_dap();

    const NATIVE_BREAK_LINE: u32 = 6;
    const NATIVE_SOURCE_FILE: &str = "src/game/c/main.c";
    // Lua line 3 is the native call site (proven to fire in run_sdl_hybrid_test).
    const LUA_BREAK_LINE: u32 = 3;

    let tmp = TempDir::new().unwrap();
    let v1_pad = "g_counter += 1;".to_string();
    let v2_pad = (0..60)
        .map(|i| format!("g_counter += {i};"))
        .collect::<Vec<_>>()
        .join(" ");

    let v1_dir = tmp.path().join("v1");
    let v2_dir = tmp.path().join("v2");
    CartProject::new()
        .c(&reload_hybrid_c(&v1_pad))
        .lua(RELOAD_HYBRID_LUA)
        .write(&v1_dir);
    CartProject::new()
        .c(&reload_hybrid_c(&v2_pad))
        .lua(RELOAD_HYBRID_LUA)
        .write(&v2_dir);
    let v1_cart = build_debug_lua_cart(&v1_dir);
    let v2_cart = build_debug_lua_cart(&v2_dir);

    let a1 = require_symbol_addr(&v1_cart, "blyt_native_init_work");
    let a2 = require_symbol_addr(&v2_cart, "blyt_native_init_work");
    assert_ne!(
        a1, a2,
        "test setup broken: the native fn did not move v1→v2"
    );

    let (mut proc, gdb_port, dap_port, dev_port, dbg_stderr) =
        spawn_blytdebug_hybrid_ports(&v1_cart);

    let lldb_dap = lldb_dap_bin().expect("lldb-dap not found");
    let driver = repo_root().join("tests/dap/run_sdl_hybrid_reload_test.mjs");
    let result = Command::new("node")
        .args([
            driver.to_str().unwrap(),
            lldb_dap.to_str().unwrap(),
            &gdb_port.to_string(),
            &dap_port.to_string(),
            &dev_port.to_string(),
            v1_cart.to_str().unwrap(),
            v1_dir.to_str().unwrap(),
            v2_cart.to_str().unwrap(),
        ])
        .env("BLYT_NATIVE_BREAK_LINE", NATIVE_BREAK_LINE.to_string())
        .env("BLYT_NATIVE_SOURCE_FILE", NATIVE_SOURCE_FILE)
        .env("BLYT_LUA_BREAK_LINE", LUA_BREAK_LINE.to_string())
        .env("BLYT_LUA_SOURCE", "/blyt/cart/src/game/lua/main.lua")
        .env(
            "BLYT_STUB_PROGRAM",
            repo_root().join("build/sdk/lib/debug/blyt-debug-stub.elf"),
        )
        .timeout(Duration::from_secs(90))
        .assert();

    let out = result.get_output().clone();
    std::thread::sleep(Duration::from_millis(300));
    let trace = String::from_utf8_lossy(&dbg_stderr.lock().unwrap()).to_string();
    let _ = proc.kill();

    if let Err(e) = result.try_success() {
        eprintln!(
            "--- driver stdout ---\n{}\n--- driver stderr ---\n{}\n--- blytdebug stderr ---\n{trace}",
            String::from_utf8_lossy(&out.stdout),
            String::from_utf8_lossy(&out.stderr),
        );
        panic!("{e}");
    }
}

/* ── WASM lldb-dap over the browser GDB relay (issue #144) ───────────────────
 *
 * The `blyt debug <dir>` WASM debug path drives the WASM runtime with lldb-dap
 * over a WS↔TCP relay.  run_wasm_lldb_dap_test.mjs reproduces that bridge
 * headlessly (WASM runtime in Node ↔ lldb-dap over TCP), so this is the
 * CI-stable equivalent of the VS Code wasm.test.js "C cart (WASM debug)" case —
 * same observable contract, no Electron.  It reuses the native run_lldb_dap
 * client driver, asserting identical behaviour across the native and WASM legs. */
fn run_wasm_lldb_dap_test(test_name: &str, project: &std::path::Path, cart: &std::path::Path) {
    let lldb_dap = lldb_dap_bin().expect("lldb-dap not found");
    let wasm_dir = find_wasm_debug_dir();
    let orchestrator = repo_root().join("tests/dap/run_wasm_lldb_dap_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            wasm_dir.to_str().unwrap(),
            lldb_dap.to_str().unwrap(),
            cart.to_str().unwrap(),
            project.to_str().unwrap(),
            "--test",
            test_name,
        ])
        .env("BLYT_GDB_BREAK_LINE", BREAK_LINE.to_string())
        .env("BLYT_SOURCE_FILE", "src/game/c/main.c")
        // lldb-dap `program` = the stub ELF, never the cart (issue #119).
        .env(
            "BLYT_STUB_PROGRAM",
            repo_root().join("build/sdk/lib/debug/blyt-debug-stub.elf"),
        )
        .timeout(Duration::from_secs(120))
        .assert()
        .success();
}

/// LLDB-DAP over the WASM browser relay (issue #144): a source breakpoint in a
/// pure-C cart binds via the svr4 library list and actually stops.  Regression
/// guard for the cart-as-library/stub-program model (#119), which broke the WASM
/// relay path — a coalesced RSP frame (lldb-dap pipelines in no-ack mode; the
/// relay forwards each TCP read as one WebSocket frame) dropped the `vCont;c`,
/// so the cart never continued to the breakpoint and the session timed out.
///
/// Requires: SDK + debug WASM runtime, lldb-dap, `readelf` for DWARF.
#[test]
fn wasm_lldb_dap_source_breakpoint() {
    require_sdk();
    require_wasm_debug();
    require_lldb_dap();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("wasm_lldb_src_bp");
    CartProject::new().c(LLDB_TEST_C).write(&project);
    let cart = build_debug_cart(&project);
    assert!(cart.exists());
    require_symbol_addr(&cart, "blyt_lldb_test_fn");

    run_wasm_lldb_dap_test("auto-start", &project, &cart);
}

/* ── WASM reload-while-debugging (issue #165) ─────────────────────────────────
 *
 * The WASM equivalents of the SDL reload-while-debug tests above.  The WASM
 * runtime has no dev-control TCP port — its reload is an in-process ccall — so
 * the orchestrator (run_wasm_lldb_dap_test.mjs) stands up a dev-control shim that
 * the shared reload-rebind / reload-n driver tests connect to via
 * BLYT_DEV_CTRL_PORT, exactly as they connect to the native player's port.  The
 * GDB-RSP trace (the ground-truth reload oracle, §5c) is read from the
 * orchestrator's combined stdout+stderr (the WASM gdb stub's BLYT_TRACE output),
 * and the same final_armed_cart_bps replay used by the SDL tests asserts the
 * post-reload armed set.
 *
 * These three tests are `#[ignore]`'d pending #170: the WASM reload path
 * (`wasm_session_reload`) is run-mode only and does not re-arm an attached
 * debugger across the swap, so the breakpoint never rebinds/refires on the
 * reloaded code.  They fail today at exactly that assertion (the reload itself
 * succeeds and the harness drives end-to-end), and are the acceptance criteria
 * for #170 — remove the `#[ignore]` when the WASM debug-reload sequence lands. */

/// Run the WASM lldb-dap orchestrator for a reload test, returning its combined
/// stdout+stderr (which carries the WASM gdb stub's `recv Z0,/z0,` trace).
fn run_wasm_lldb_dap_reload(
    test_name: &str,
    project: &std::path::Path,
    v1_cart: &std::path::Path,
    break_line: u32,
    source_file: &str,
    extra_env: &[(&str, String)],
) -> String {
    let lldb_dap = lldb_dap_bin().expect("lldb-dap not found");
    let wasm_dir = find_wasm_debug_dir();
    let orchestrator = repo_root().join("tests/dap/run_wasm_lldb_dap_test.mjs");
    let out = Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            wasm_dir.to_str().unwrap(),
            lldb_dap.to_str().unwrap(),
            v1_cart.to_str().unwrap(),
            project.to_str().unwrap(),
            "--test",
            test_name,
        ])
        .env("BLYT_GDB_BREAK_LINE", break_line.to_string())
        .env("BLYT_SOURCE_FILE", source_file)
        .env("BLYT_TRACE", "gdb,dap,lifecycle,frame")
        .env(
            "BLYT_STUB_PROGRAM",
            repo_root().join("build/sdk/lib/debug/blyt-debug-stub.elf"),
        )
        .envs(extra_env.iter().map(|(k, v)| (*k, v.as_str())))
        .timeout(Duration::from_secs(180))
        .output()
        .expect("run orchestrator");

    let mut combined = String::from_utf8_lossy(&out.stdout).to_string();
    combined.push_str(&String::from_utf8_lossy(&out.stderr));
    if !out.status.success() {
        eprintln!("--- orchestrator output ---\n{combined}");
        panic!("WASM lldb-dap reload orchestrator failed: {:?}", out.status);
    }
    combined
}

/// WASM reload-while-debugging, native C (issue #165, mirrors
/// `sdl_native_lldb_dap_reload_rebinds_breakpoint`): a hot reload over the WASM
/// browser relay rebinds the breakpoint to the NEW code's address — single
/// location, no stale old location.
///
/// Drives the in-VM cart-as-library swap (`blyt_session_swap_cart`) under a live
/// lldb-dap session and asserts on the GDB-RSP trace that the breakpoint rebinds
/// to v2's re-read address.
#[test]
fn wasm_native_lldb_dap_reload_rebinds_breakpoint() {
    require_sdk();
    require_wasm_debug();
    require_lldb_dap();

    const RELOAD_BREAK_LINE: u32 = 6;

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

    let trace = run_wasm_lldb_dap_reload(
        "reload-rebind",
        &v1_dir,
        &v1_cart,
        RELOAD_BREAK_LINE,
        "src/game/c/main.c",
        &[("BLYT_V2_CART", v2_cart.to_str().unwrap().to_string())],
    );

    let cart_armed = final_armed_cart_bps(&trace);
    let a1_resolved = first_armed_bp(&trace).expect("no breakpoint set in trace");
    let line_off = a1_resolved - a1;
    let reread = 0x0500_0000 + a2 + line_off;

    assert_eq!(
        cart_armed,
        vec![reread],
        "after reload the breakpoint must be a single location at the re-read v2 \
         address 0x{reread:x} (a1_resolved=0x{a1_resolved:x} v1_fn=0x{a1:x} v2_fn=0x{a2:x}); \
         got {cart_armed:x?}.\n--- trace ---\n{trace}"
    );
}

/// WASM hybrid (Lua + C) reload-while-debugging (issue #165, acceptance criterion
/// 2; mirrors `sdl_hybrid_lldb_dap_reload_fires_both_init_breakpoints`): in a
/// single WASM debug session with BOTH the native lldb-dap and the companion Lua
/// DAP live at once, a breakpoint in `init()` on BOTH the Lua and native sides
/// fires again after a hot reload, and neither client is torn down across the
/// reload.  Reuses the SDL hybrid fixtures (`reload_hybrid_c` / `RELOAD_HYBRID_LUA`)
/// driven over the WASM browser relay (gdb WS↔TCP bridge + Lua DAP WS relay) with
/// the reload driven by the in-process dev-control ccall.
#[test]
fn wasm_hybrid_lldb_dap_reload_fires_both_init_breakpoints() {
    require_sdk();
    require_lua_sdk();
    require_wasm_debug();
    require_lldb_dap();

    const NATIVE_BREAK_LINE: u32 = 6;
    const NATIVE_SOURCE_FILE: &str = "src/game/c/main.c";
    const LUA_BREAK_LINE: u32 = 3;

    let tmp = TempDir::new().unwrap();
    let v1_pad = "g_counter += 1;".to_string();
    let v2_pad = (0..60)
        .map(|i| format!("g_counter += {i};"))
        .collect::<Vec<_>>()
        .join(" ");

    let v1_dir = tmp.path().join("v1");
    let v2_dir = tmp.path().join("v2");
    CartProject::new()
        .c(&reload_hybrid_c(&v1_pad))
        .lua(RELOAD_HYBRID_LUA)
        .write(&v1_dir);
    CartProject::new()
        .c(&reload_hybrid_c(&v2_pad))
        .lua(RELOAD_HYBRID_LUA)
        .write(&v2_dir);
    let v1_cart = build_debug_lua_cart(&v1_dir);
    let v2_cart = build_debug_lua_cart(&v2_dir);

    let a1 = require_symbol_addr(&v1_cart, "blyt_native_init_work");
    let a2 = require_symbol_addr(&v2_cart, "blyt_native_init_work");
    assert_ne!(
        a1, a2,
        "test setup broken: the native fn did not move v1→v2"
    );

    let lldb_dap = lldb_dap_bin().expect("lldb-dap not found");
    let wasm_dir = find_wasm_debug_dir();
    let driver = repo_root().join("tests/dap/run_wasm_hybrid_reload_test.mjs");
    let out = Command::new("node")
        .args([
            driver.to_str().unwrap(),
            wasm_dir.to_str().unwrap(),
            lldb_dap.to_str().unwrap(),
            v1_cart.to_str().unwrap(),
            v1_dir.to_str().unwrap(),
            v2_cart.to_str().unwrap(),
        ])
        .env("BLYT_NATIVE_BREAK_LINE", NATIVE_BREAK_LINE.to_string())
        .env("BLYT_NATIVE_SOURCE_FILE", NATIVE_SOURCE_FILE)
        .env("BLYT_LUA_BREAK_LINE", LUA_BREAK_LINE.to_string())
        .env("BLYT_LUA_SOURCE", "/blyt/cart/src/game/lua/main.lua")
        .env(
            "BLYT_STUB_PROGRAM",
            repo_root().join("build/sdk/lib/debug/blyt-debug-stub.elf"),
        )
        .timeout(Duration::from_secs(180))
        .output()
        .expect("run hybrid reload driver");

    if !out.status.success() {
        panic!(
            "WASM hybrid reload driver failed: {:?}\n--- stdout ---\n{}\n--- stderr ---\n{}",
            out.status,
            String::from_utf8_lossy(&out.stdout),
            String::from_utf8_lossy(&out.stderr),
        );
    }
}

/// Rust cart whose `blyt_lldb_test_fn` sits at a *shiftable* address: `pad` grows
/// the single-line `blyt_pad_fn` before it, moving the following function while
/// keeping line numbers stable.  The function is referenced (via the exported
/// `G_KEEP` static) but never called, so its breakpoint resolves yet never fires
/// — the cart keeps running update() and services the reload across the swap.
/// Mirrors `reload_cart_src` (the C fixture) so the same break line and trace
/// assertions apply.
fn reload_cart_rust(pad: &str) -> String {
    format!(
        "#![no_std]\n\
         use blyt::*;\n\
         static mut G_COUNTER: u32 = 0;\n\
         #[inline(never)] #[no_mangle] pub extern \"C\" fn blyt_pad_fn() {{ unsafe {{ {pad} }} }}\n\
         #[inline(never)] #[no_mangle] pub extern \"C\" fn blyt_lldb_test_fn() {{\n\
         \x20   let local_val: u32 = 0xdeadbeef;\n\
         \x20   unsafe {{ core::ptr::write_volatile(core::ptr::addr_of_mut!(G_COUNTER), local_val.wrapping_add(1)); }}\n\
         }}\n\
         #[no_mangle] pub static mut G_KEEP: extern \"C\" fn() = blyt_lldb_test_fn;\n\
         #[no_mangle] pub extern \"C\" fn blyt_cart_init() {{ blyt_pad_fn(); }}\n\
         #[no_mangle] pub extern \"C\" fn blyt_cart_update() {{ unsafe {{ let f = core::ptr::read_volatile(core::ptr::addr_of!(G_KEEP)); core::hint::black_box(f); }} }}\n\
         #[no_mangle] pub extern \"C\" fn blyt_cart_draw() {{}}\n"
    )
}

/// A per-write pad string of length `n` for the Rust reload fixture (kept on the
/// single `blyt_pad_fn` line so growing it shifts the following function's
/// address without moving any line numbers).
fn rust_pad(n: usize) -> String {
    (0..n)
        .map(|i| format!("core::ptr::write_volatile(core::ptr::addr_of_mut!(G_COUNTER), {i});"))
        .collect::<Vec<_>>()
        .join(" ")
}

/// WASM reload-while-debugging, native Rust (issue #165, acceptance criterion 3):
/// a Rust cart exercises the native reload-while-debug path — the C test shape
/// with a Rust fixture.  Asserts the same single-location rebind to v2's re-read
/// address on the GDB-RSP trace.
#[test]
fn wasm_rust_lldb_dap_reload_rebinds_breakpoint() {
    require_sdk();
    require_wasm_debug();
    require_lldb_dap();
    require_rust_riscv_target();

    // Break line 7: the `write_volatile` body statement of blyt_lldb_test_fn.
    const RELOAD_BREAK_LINE: u32 = 7;

    let tmp = TempDir::new().unwrap();
    let v1_dir = tmp.path().join("v1");
    let v2_dir = tmp.path().join("v2");
    CartProject::new()
        .rust(&reload_cart_rust(&rust_pad(1)))
        .write(&v1_dir);
    CartProject::new()
        .rust(&reload_cart_rust(&rust_pad(80)))
        .write(&v2_dir);
    let v1_cart = build_debug_cart(&v1_dir);
    let v2_cart = build_debug_cart(&v2_dir);

    let a1 = require_symbol_addr(&v1_cart, "blyt_lldb_test_fn");
    let a2 = require_symbol_addr(&v2_cart, "blyt_lldb_test_fn");
    assert_ne!(a1, a2, "test setup broken: the function did not move v1→v2");

    let trace = run_wasm_lldb_dap_reload(
        "reload-rebind",
        &v1_dir,
        &v1_cart,
        RELOAD_BREAK_LINE,
        "src/game/rust/src/lib.rs",
        &[("BLYT_V2_CART", v2_cart.to_str().unwrap().to_string())],
    );

    let cart_armed = final_armed_cart_bps(&trace);
    let a1_resolved = first_armed_bp(&trace).expect("no breakpoint set in trace");
    let line_off = a1_resolved - a1;
    let reread = 0x0500_0000 + a2 + line_off;

    assert_eq!(
        cart_armed,
        vec![reread],
        "after reload the breakpoint must be a single location at the re-read v2 \
         address 0x{reread:x} (a1_resolved=0x{a1_resolved:x} v1_fn=0x{a1:x} v2_fn=0x{a2:x}); \
         got {cart_armed:x?}.\n--- trace ---\n{trace}"
    );
}
