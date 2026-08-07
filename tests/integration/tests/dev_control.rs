//! Dev control channel (issue #87).
//!
//! Covers three faces of the channel:
//!  - the devtool side (`blyt run ./dir`) announces the dev control TCP port
//!    and accepts a connection (`run_announces_dev_control_port`);
//!  - the native player speaks the protocol end-to-end over its own TCP
//!    listener (`native_dev_control_lifecycle_commands`);
//!  - the WASM C handler is exercised by `wasm.rs::wasm_dev_control_*`.
//! The newline-delimited JSON relay itself is unit-tested in
//! `devtool/src/run.rs` (`dev_ctrl_relay_session_bidirectional`).

mod common;

use common::{
    CartProject, blyt_bin, blytplay, build_lua_cart, libretro_so, require_libretro_core,
    require_lua_sdk, require_sdk, require_wasm, sdk_dir, test_libretro_core,
};
use std::io::{BufRead, BufReader, Read as _, Write as _};
use std::net::{TcpListener, TcpStream};
use std::sync::mpsc;
use std::sync::{Arc, Mutex};
use std::time::Duration;
use tempfile::TempDir;

/// Minimal Lua cart that runs forever (so the server stays up while we probe).
fn idle_lua_cart(tmp: &TempDir) -> std::path::PathBuf {
    let project = tmp.path().join("dev_ctrl_test");
    CartProject::new()
        .lua(
            "function init() end\n\
             function update() end\n\
             function draw() end\n",
        )
        .write(&project);
    project
}

fn spawn_blyt_run(project: &std::path::Path) -> std::process::Child {
    let sdk = sdk_dir();
    let mut cmd = std::process::Command::new(blyt_bin());
    cmd.args(["run", project.to_str().unwrap()])
        .env("BLYT_SDK_DIR", &sdk)
        .env("BLYT_OBJCOPY", sdk.join("bin/blyt-objcopy"));
    for (var, rel) in [
        ("BLYT_CLANG", "bin/blyt-clang"),
        ("BLYT_LUAC", "bin/blyt-luac"),
    ] {
        let p = sdk.join(rel);
        if p.exists() {
            cmd.env(var, &p);
        }
    }
    cmd.stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped())
        .spawn()
        .expect("blyt run spawn")
}

/// Read stdout until the dev control banner line appears; return the announced
/// TCP port and the still-open stdout handle (keep it alive so the server does
/// not SIGPIPE on later writes).
fn read_dev_ctrl_port(child: &mut std::process::Child) -> (u16, std::process::ChildStdout) {
    let mut stdout = child.stdout.take().unwrap();
    let mut banner = String::new();
    loop {
        let mut chunk = [0u8; 1024];
        let n = stdout.read(&mut chunk).expect("read stdout");
        if n == 0 {
            let mut stderr_out = String::new();
            if let Some(mut e) = child.stderr.take() {
                e.read_to_string(&mut stderr_out).ok();
            }
            panic!(
                "blyt run exited before announcing the dev control port;\
                 \nstdout so far:\n{banner}\nstderr:\n{stderr_out}"
            );
        }
        banner.push_str(&String::from_utf8_lossy(&chunk[..n]));
        // Banner line: "  Dev control:  127.0.0.1:<port>   (TCP — …)"
        if let Some(idx) = banner.find("Dev control:") {
            let rest = &banner[idx..];
            if let Some(addr) = rest.find("127.0.0.1:") {
                let after = &rest[addr + "127.0.0.1:".len()..];
                let end = after
                    .find(|c: char| !c.is_ascii_digit())
                    .unwrap_or(after.len());
                if end > 0 {
                    let port: u16 = after[..end].parse().expect("dev ctrl port parse");
                    return (port, stdout);
                }
            }
        }
    }
}

/// `blyt run ./project` announces the dev control TCP port and accepts a TCP
/// connection on it (the relay's accept loop is up even before any WASM page
/// connects on the WebSocket side).
#[test]
fn run_announces_dev_control_port() {
    require_sdk();
    require_wasm();

    let tmp = TempDir::new().unwrap();
    let project = idle_lua_cart(&tmp);

    let mut serve = spawn_blyt_run(&project);
    let (port, _stdout) = read_dev_ctrl_port(&mut serve);

    assert!(port != 0, "dev control port must be a real bound port");

    // The relay must accept a TCP connection on the announced port.
    let conn = TcpStream::connect(("127.0.0.1", port));
    let _ = serve.kill();
    conn.unwrap_or_else(|e| panic!("connect to dev control TCP {port}: {e}"));
}

/* ── native player ────────────────────────────────────────────────────────── */

const DEV_CTRL_CONFIG: &str = "\
save_version: 5
records:
  Game:
    fields:
      - { name: score, type: i32 }
state_buffers:
  game:
    record: Game
    count: 1
";

/// Build a pure-Lua cart whose init() sets game.score to `score`, and whose
/// on_load_state prints "<tag> load score=<n> reason=<r> version=<v>".  The host
/// marshals `blyt_load_info_t` through the guest ABI (PR #109), so the cart
/// reads `info.reason` to prove the correct trigger reaches it: SAVE_GAME (0)
/// for a dev-control `load_state`, HOT_RELOAD (3) for a `reload`.  It also reads
/// `info.saved_cart_version` (issue #112): the cart's declared save_version (5)
/// for a load_state (read from the save header), 0 for a non-SAVE_GAME reload.
fn score_cart(tmp: &TempDir, name: &str, tag: &str, score: i32) -> std::path::PathBuf {
    let project = tmp.path().join(name);
    CartProject::new()
        .config(DEV_CTRL_CONFIG)
        .lua(&format!(
            "local slot = -1\n\
             function init()\n\
             \tslot = blyt.buf.alloc_slot(S.GAME)\n\
             \tS.game[slot].score = {score}\n\
             end\n\
             function update() end\n\
             function draw() end\n\
             function on_load_state(info)\n\
             \tblyt.debug.print('{tag} load score=' .. tostring(S.game[slot].score)\n\
             \t\t.. ' reason=' .. tostring(info.reason)\n\
             \t\t.. ' version=' .. tostring(info.saved_cart_version))\n\
             end\n"
        ))
        .write(&project);
    build_lua_cart(&project)
}

/// Spawn the player draining stdout on a background thread; returns the child,
/// the announced dev control port, and a shared buffer of all stdout lines.
fn spawn_player_with_dev_ctrl(
    cart: &std::path::Path,
    save_dir: &std::path::Path,
) -> (std::process::Child, u16, Arc<Mutex<Vec<String>>>) {
    spawn_player_with_dev_ctrl_args(cart, save_dir, &[])
}

/// As `spawn_player_with_dev_ctrl`, but prepends `extra_args` before the cart —
/// a general hook for passing extra player flags to a dev-ctrl reload test.
fn spawn_player_with_dev_ctrl_args(
    cart: &std::path::Path,
    save_dir: &std::path::Path,
    extra_args: &[&str],
) -> (std::process::Child, u16, Arc<Mutex<Vec<String>>>) {
    let mut args: Vec<&str> = vec!["--headless", "--dev-ctrl-port", "0"];
    args.extend_from_slice(extra_args);
    args.push(cart.to_str().unwrap());
    let mut child = std::process::Command::new(blytplay())
        .args(&args)
        .env("BLYT_SAVE_DIR", save_dir)
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped())
        .spawn()
        .expect("blytplay spawn");

    let stdout = child.stdout.take().unwrap();
    let lines = Arc::new(Mutex::new(Vec::<String>::new()));
    let lines_w = Arc::clone(&lines);
    let (port_tx, port_rx) = mpsc::channel::<u16>();

    std::thread::spawn(move || {
        let mut reader = BufReader::new(stdout);
        let mut sent = false;
        loop {
            let mut line = String::new();
            match reader.read_line(&mut line) {
                Ok(0) | Err(_) => break,
                Ok(_) => {
                    if !sent {
                        if let Some(idx) = line.find("listening on 127.0.0.1:") {
                            let rest = &line[idx + "listening on 127.0.0.1:".len()..];
                            let end = rest
                                .find(|c: char| !c.is_ascii_digit())
                                .unwrap_or(rest.len());
                            if let Ok(p) = rest[..end].parse::<u16>() {
                                let _ = port_tx.send(p);
                                sent = true;
                            }
                        }
                    }
                    lines_w.lock().unwrap().push(line.trim_end().to_string());
                }
            }
        }
    });

    let port = port_rx
        .recv_timeout(std::time::Duration::from_secs(20))
        .expect("player did not announce dev control port");
    (child, port, lines)
}

/// Send one command line and read the single JSON response line.
fn dev_ctrl_cmd(stream: &mut TcpStream, reader: &mut impl BufRead, cmd: &str) -> String {
    stream.write_all(format!("{cmd}\n").as_bytes()).unwrap();
    stream.flush().unwrap();
    let mut line = String::new();
    reader.read_line(&mut line).expect("read dev ctrl response");
    line.trim_end().to_string()
}

/// Read one JSON response line.
fn dev_ctrl_read(reader: &mut impl BufRead) -> String {
    let mut line = String::new();
    reader.read_line(&mut line).expect("read dev ctrl response");
    line.trim_end().to_string()
}

/// The native player listens on the dev control port and services the full
/// command set: reset / save_state / load_state / reload, with reload swapping
/// in a freshly built cart while preserving the state buffer (ADR-0045).
#[test]
fn native_dev_control_lifecycle_commands() {
    require_sdk();
    require_lua_sdk();

    let tmp = TempDir::new().unwrap();
    let save_dir = TempDir::new().unwrap();

    // v1 boots score=7; v2 (different code) boots score=100 — so a reload that
    // preserves v1's score is distinguishable from a fresh boot.
    let v1 = score_cart(&tmp, "native_dc_v1", "v1", 7);
    let v2 = score_cart(&tmp, "native_dc_v2", "v2", 100);

    // Run the player on a copy we can overwrite to simulate `blyt debug`'s
    // in-place rebuild before a reload.
    let work = tmp.path().join("cart.blyt");
    std::fs::copy(&v1, &work).unwrap();

    let (mut child, port, lines) = spawn_player_with_dev_ctrl(&work, save_dir.path());

    let mut stream = TcpStream::connect(("127.0.0.1", port)).expect("connect dev control");
    stream
        .set_read_timeout(Some(std::time::Duration::from_secs(10)))
        .unwrap();
    let read_half = stream.try_clone().unwrap();
    let mut reader = BufReader::new(read_half);

    let check = |resp: &str, id: i64, cmd: &str| {
        assert_eq!(
            resp,
            format!("{{\"id\":{id},\"status\":\"ok\",\"cmd\":\"{cmd}\"}}"),
            "unexpected response"
        );
    };

    // Pipeline reset + save_state in a single write so the player dispatches
    // both in one poll pass with no frame in between — deterministically
    // reproducing the issue-#105 race. `reset` recreates a fresh, pre-init
    // session; unless reset itself boots the cart (runs init), the
    // immediately-following save_state captures empty state (no slot allocated),
    // so a later load_state reports score=0. Booting in reset means save_state
    // serialises the real init() state (score=7).
    stream
        .write_all(
            concat!(
                "{\"id\":1,\"cmd\":\"reset\"}\n",
                "{\"id\":2,\"cmd\":\"save_state\",\"slot\":1}\n",
            )
            .as_bytes(),
        )
        .unwrap();
    stream.flush().unwrap();
    check(&dev_ctrl_read(&mut reader), 1, "reset");
    check(&dev_ctrl_read(&mut reader), 2, "save_state");

    // load_state round-trips on its own: by now a frame has run, so the cart's
    // `slot` upvalue is allocated and on_load_state reads slot 0 — reporting the
    // score that save_state persisted (7 with the fix, 0 without it).
    check(
        &dev_ctrl_cmd(
            &mut stream,
            &mut reader,
            r#"{"id":3,"cmd":"load_state","slot":1}"#,
        ),
        3,
        "load_state",
    );

    // Rebuild in place (overwrite with v2's code), then hot reload.
    std::fs::copy(&v2, &work).unwrap();
    check(
        &dev_ctrl_cmd(&mut stream, &mut reader, r#"{"id":4,"cmd":"reload"}"#),
        4,
        "reload",
    );

    // Out-of-sequence: ids must be echoed back in order.
    let a = dev_ctrl_cmd(&mut stream, &mut reader, r#"{"id":10,"cmd":"reset"}"#);
    let b = dev_ctrl_cmd(&mut stream, &mut reader, r#"{"id":11,"cmd":"reset"}"#);
    assert!(a.contains("\"id\":10"), "id 10 not echoed: {a}");
    assert!(b.contains("\"id\":11"), "id 11 not echoed: {b}");

    // Give the reload's on_load_state output time to flush, then assert the v2
    // code ran with v1's preserved state (score=7, not v2's fresh 100).
    std::thread::sleep(std::time::Duration::from_millis(300));
    let _ = child.kill();
    let _ = child.wait();

    let out = lines.lock().unwrap().join("\n");
    // reset must boot the cart so the save_state pipelined with it captured
    // init()'s state — load_state then reports the preserved score, not 0.
    // load_state delivers reason SAVE_GAME (0) through the guest ABI.
    // load_state reports the cart's save_version (5), read from the save header
    // the save_state wrote (issue #112).
    assert!(
        out.contains("v1 load score=7 reason=0 version=5"),
        "reset+save_state race: save did not capture init() state \
         (issue #105), or load_state did not deliver reason=SAVE_GAME(0) / \
         the saved version (5; issue #112); player output:\n{out}"
    );
    // reload delivers reason HOT_RELOAD (3) and preserves v1's state into v2;
    // a non-SAVE_GAME reason reports version 0 (ADR-0087).
    assert!(
        out.contains("v2 load score=7 reason=3 version=0"),
        "reload did not preserve state across the code swap, or did not deliver \
         reason=HOT_RELOAD(3) / version=0; player output:\n{out}"
    );
}

/// Build a pure-Lua cart that prints its `guest_heap_used` byte count on every
/// init(): `probe heap=<n>`.  It allocates a deterministic, kept-alive table so
/// the count is a fixed non-zero baseline, then reads it via
/// `blyt32.mem.stats().cart_allocations`.  That count is identical on every load
/// **iff** the reload resets the cart-heap accounting to a fresh-load baseline
/// (issue #133 memory hygiene).  Without the reset it climbs monotonically from
/// the first reload, as each reload's allocations stack on the previous cart's
/// un-reclaimed heap.
///
/// (Pure-Lua carts run host-Lua by default after #236, whose allocator draws
/// physical bytes from host malloc with a shadow arena for byte-accounting, #231 —
/// so raw pointer addresses are host-determined and NOT a determinism signal.
/// `guest_heap_used` is the deterministic hygiene signal, reset by
/// `hl_rebuild_and_restore` on every hot swap; this is the host-Lua-appropriate
/// form of the emulated-path arena-reset check.)
fn arena_probe_cart(tmp: &TempDir) -> std::path::PathBuf {
    let project = tmp.path().join("arena_probe");
    CartProject::new()
        .lua(
            "local keep\n\
             function init()\n\
             \tkeep = {}\n\
             \tfor i = 1, 64 do keep[i] = i * 2 end\n\
             \tblyt.debug.print('probe heap=' .. tostring(blyt32.mem.stats().cart_allocations))\n\
             end\n\
             function update() end\n\
             function draw() end\n",
        )
        .write(&project);
    build_lua_cart(&project)
}

/// N consecutive hot reloads of the same cart must reach a stable steady state:
/// every reload's `guest_heap_used` equals the very first (fresh) load's — i.e.
/// the hot-reloaded VM's cart-heap accounting is reset to a fresh-load baseline
/// (issue #133 memory hygiene, spike-W gate G4).  The cart prints its
/// `guest_heap_used` in init(); the reload path must empty the cart heap (reset
/// the accounting) so the count does not drift (and, accumulated over many
/// reloads, does not exhaust the 16 MiB budget).  Pre-fix this fails on the first
/// reload, as the count climbs the moment a second cart load stacks on the
/// persistent heap.
#[test]
fn native_dev_control_reload_arena_steady_state() {
    require_sdk();
    require_lua_sdk();

    let tmp = TempDir::new().unwrap();
    let save_dir = TempDir::new().unwrap();

    let cart = arena_probe_cart(&tmp);
    let (mut child, port, lines) = spawn_player_with_dev_ctrl(&cart, save_dir.path());

    let mut stream = TcpStream::connect(("127.0.0.1", port)).expect("connect dev control");
    stream
        .set_read_timeout(Some(Duration::from_secs(10)))
        .unwrap();
    let read_half = stream.try_clone().unwrap();
    let mut reader = BufReader::new(read_half);

    // Drive N reloads of the same cart, leaving a frame's worth of slack after
    // each so the swapped-in cart's init() actually runs (and prints) before the
    // next reload reboots it.
    const N: usize = 12;
    for i in 0..N {
        let id = 100 + i as i64;
        let resp = dev_ctrl_cmd(
            &mut stream,
            &mut reader,
            &format!(r#"{{"id":{id},"cmd":"reload"}}"#),
        );
        assert!(
            resp.contains(&format!("\"id\":{id}")) && resp.contains("\"status\":\"ok\""),
            "reload {i} did not succeed: {resp}"
        );
        std::thread::sleep(Duration::from_millis(50));
    }

    // Let the final reload's init() output flush.
    std::thread::sleep(Duration::from_millis(300));
    let _ = child.kill();
    let _ = child.wait();

    let out = lines.lock().unwrap().clone();
    let heaps: Vec<&str> = out
        .iter()
        .filter_map(|l| l.split("probe heap=").nth(1).map(str::trim))
        .collect();

    // Initial (fresh) load + N reloads = N+1 probe lines.
    assert!(
        heaps.len() >= N + 1,
        "expected at least {} probe lines (fresh load + {N} reloads), got {}; player output:\n{:#?}",
        N + 1,
        heaps.len(),
        out
    );

    let baseline = heaps[0];
    for (i, h) in heaps.iter().enumerate() {
        assert_eq!(
            *h, baseline,
            "guest_heap_used drifted on reload {i}: {h:?} != fresh-load baseline {baseline:?} — \
             the cart-heap accounting was not reset on the hot swap (issue #133). \
             All probes:\n{heaps:#?}"
        );
    }
}

/// Pure-Lua cart that reads an uncompressed bundled resource in on_load_state and
/// echoes it (issue #246).  Byte-identical Lua across v1/v2, so the ONLY thing a
/// reload can change is the bundled resource content — proving it is the resource
/// TABLE, not the code, that reloaded from the new cart.
/// 0x20000001 = R_GREETING baked constant (kind RESOURCE, id 1; ADR-0134).
const RELOAD_RESOURCE_LUA: &str = r#"
local function greeting()
    return blyt.resource.text_resource(0x20000001):text() or "<nil>"
end
function init()
    blyt.debug.print("init greeting=" .. greeting())
end
function update() end
function draw() end
function on_load_state(info)
    blyt.debug.print("reload greeting=" .. greeting() .. " reason=" .. tostring(info.reason))
end
"#;

/// Drive an in-place dev-ctrl `reload` cart-swap on the native player, swapping a
/// cart whose bundled resource content changed (RES_V1 → RES_V2), and assert the
/// post-swap on_load_state read returns the NEW content (issue #246).
///
/// The pure-Lua cart runs host-Lua by default (ADR-0136), so the dev-ctrl reload
/// drives `blyt_hostlua_reload` (#244), whose resource reload was never exercised
/// because `hello` has no resources.  The reload must surface RES_V2; a stale
/// RES_V1 would mean the resource table still aliases the freed old cart map
/// (resource.c e->data = body).  `extra_args` is a general hook for extra player
/// flags.
fn native_reload_resource_leg(extra_args: &[&str], leg: &str) {
    let tmp = TempDir::new().unwrap();
    let save_dir = TempDir::new().unwrap();

    let project_v1 = tmp.path().join("reload_res_v1");
    CartProject::new()
        .config(DEV_CTRL_CONFIG)
        .lua(RELOAD_RESOURCE_LUA)
        .asset("greeting.txt", "RES_V1")
        .write(&project_v1);
    let v1 = build_lua_cart(&project_v1);

    let project_v2 = tmp.path().join("reload_res_v2");
    CartProject::new()
        .config(DEV_CTRL_CONFIG)
        .lua(RELOAD_RESOURCE_LUA)
        .asset("greeting.txt", "RES_V2")
        .write(&project_v2);
    let v2 = build_lua_cart(&project_v2);

    // Run on a copy we overwrite in place, reproducing `blyt debug`'s in-place
    // rebuild-then-reload (the reload reopens g_cart_path).
    let work = tmp.path().join("cart.blyt");
    std::fs::copy(&v1, &work).unwrap();

    let (mut child, port, lines) =
        spawn_player_with_dev_ctrl_args(&work, save_dir.path(), extra_args);

    let mut stream = TcpStream::connect(("127.0.0.1", port)).expect("connect dev control");
    stream
        .set_read_timeout(Some(Duration::from_secs(10)))
        .unwrap();
    let read_half = stream.try_clone().unwrap();
    let mut reader = BufReader::new(read_half);

    // Wait for cart_v1's init() to run and read its bundled resource BEFORE
    // reloading.  Sending the reload immediately races boot: on a fast runner the
    // swap can happen before frame 0 runs, so v1's init never prints RES_V1 (the
    // CI-observed flake).  This also anchors the pre-swap value the post-reload
    // read must differ from.
    let wait_line = |needle: &str, what: &str| {
        let deadline = std::time::Instant::now() + Duration::from_secs(10);
        loop {
            if lines.lock().unwrap().iter().any(|l| l.contains(needle)) {
                return;
            }
            assert!(
                std::time::Instant::now() < deadline,
                "{leg}: timed out waiting for {what} ({needle:?}); player output:\n{}",
                lines.lock().unwrap().join("\n")
            );
            std::thread::sleep(Duration::from_millis(20));
        }
    };
    wait_line("init greeting=RES_V1", "cart_v1 init resource read");

    // Rebuild in place (overwrite with v2, whose resource ships RES_V2), then
    // hot reload — the swap must reload the resource table from the new cart.
    std::fs::copy(&v2, &work).unwrap();
    let resp = dev_ctrl_cmd(&mut stream, &mut reader, r#"{"id":1,"cmd":"reload"}"#);
    assert!(
        resp.contains("\"id\":1") && resp.contains("\"status\":\"ok\""),
        "{leg}: reload did not succeed: {resp}"
    );

    // The reload's on_load_state runs synchronously inside the reload handler
    // (before the ok response), so RES_V2 is already printed; poll to be robust.
    wait_line(
        "reload greeting=RES_V2 reason=3",
        "post-reload resource read",
    );
    let _ = child.kill();
    let _ = child.wait();

    // Guard against a stale read masquerading as success: the post-reload value
    // must be RES_V2, never the pre-swap RES_V1 (which would mean the table still
    // aliased the freed old cart, #246).
    let out = lines.lock().unwrap().join("\n");
    assert!(
        !out.contains("reload greeting=RES_V1"),
        "{leg}: post-reload read returned the stale old-cart content RES_V1 (#246); \
         player output:\n{out}"
    );
}

/// Host-Lua path (ADR-0136): a pure-Lua cart runs host-Lua by default, so the
/// dev-ctrl reload drives `blyt_hostlua_reload` (#244), which reloads its resource
/// table from the new cart before the old cart is freed.  Pins that #244 handling —
/// `hello` (its only prior reload cart) has zero resources, so this path was never
/// exercised.
#[test]
fn native_dev_control_reload_reloads_resource_hostlua() {
    require_sdk();
    require_lua_sdk();
    native_reload_resource_leg(&[], "native host-Lua");
}

/// #251 (#232 S7 follow-up) — a HYBRID cart on the host-Lua path (`--host-lua`; a
/// hybrid opts in, #236/#257 keep pure-Lua as the default host-Lua case) reloads
/// *coordinated* across BOTH halves: the native rv32 half is `blyt_session_swap_cart`ed
/// and the host Lua VM is rebuilt, together, so neither half is left on the old
/// image (the silent half-reload #98 warns against).  The v1/v2 carts differ in the
/// NATIVE half too — `compute(x)` returns `x+1` in v1 and `x+100` in v2 — so a
/// Lua-only half-reload would surface the v2 Lua marker while `compute(41)` still
/// returned 42; a coordinated reload returns 141.  Replaces the earlier
/// refuse-outright guard (#232 deferred the coordinated swap to #251).
#[test]
fn native_dev_control_reload_hybrid_coordinated_hostlua() {
    require_sdk();
    require_lua_sdk();

    let tmp = TempDir::new().unwrap();
    let save_dir = TempDir::new().unwrap();

    // A hybrid: Lua half + a native C export it calls.  v1 and v2 differ in BOTH
    // halves — the Lua marker string AND the native `compute` result — so the test
    // can tell a coordinated reload (both new) from a silent half-reload (new Lua,
    // stale native).
    let hybrid_c = |ret: &str| {
        format!(
            "#include \"blyt.h\"\n\
             BLYT_LUA_EXPORT_I32(compute, int32_t x) {{ return {ret}; }}\n"
        )
    };
    let hybrid_lua = |tag: &str| {
        format!(
            "function init() blyt.debug.print(\"hybrid {tag} init compute=\" .. compute(41)) end\n\
             function update() end\n\
             function draw() end\n\
             function on_load_state(info)\n\
                 blyt.debug.print(\"hybrid {tag} RELOADED compute=\" .. compute(41))\n\
             end\n"
        )
    };

    let project_v1 = tmp.path().join("hybrid_reload_v1");
    CartProject::new()
        .config(DEV_CTRL_CONFIG)
        .c(&hybrid_c("x + 1"))
        .lua(&hybrid_lua("V1"))
        .write(&project_v1);
    let v1 = build_lua_cart(&project_v1);

    let project_v2 = tmp.path().join("hybrid_reload_v2");
    CartProject::new()
        .config(DEV_CTRL_CONFIG)
        .c(&hybrid_c("x + 100"))
        .lua(&hybrid_lua("V2"))
        .write(&project_v2);
    let v2 = build_lua_cart(&project_v2);

    // Run on a copy we overwrite in place (as `blyt debug`'s rebuild-then-reload).
    let work = tmp.path().join("cart.blyt");
    std::fs::copy(&v1, &work).unwrap();

    let (mut child, port, lines) = spawn_player_with_dev_ctrl_args(&work, save_dir.path(), &[]);

    let mut stream = TcpStream::connect(("127.0.0.1", port)).expect("connect dev control");
    stream
        .set_read_timeout(Some(Duration::from_secs(10)))
        .unwrap();
    let read_half = stream.try_clone().unwrap();
    let mut reader = BufReader::new(read_half);

    let wait_line = |needle: &str| {
        let deadline = std::time::Instant::now() + Duration::from_secs(10);
        loop {
            if lines.lock().unwrap().iter().any(|l| l.contains(needle)) {
                return;
            }
            assert!(
                std::time::Instant::now() < deadline,
                "timed out waiting for {needle:?}; player output:\n{}",
                lines.lock().unwrap().join("\n")
            );
            std::thread::sleep(Duration::from_millis(20));
        }
    };
    wait_line("hybrid V1 init compute=42");

    // Overwrite with v2 and reload — the host-Lua hybrid path must reload BOTH halves.
    std::fs::copy(&v2, &work).unwrap();
    let resp = dev_ctrl_cmd(&mut stream, &mut reader, r#"{"id":1,"cmd":"reload"}"#);
    assert!(
        resp.contains("\"id\":1") && resp.contains("\"status\":\"ok\""),
        "coordinated hybrid reload must succeed on the host-Lua path: {resp}"
    );

    // The rebuilt VM re-runs init() on the NEW cart, and on_load_state fires with the
    // restored state — both call the NEW native `compute` (x+100 ⇒ 141).  Seeing 141
    // (not 42) is the proof the native half swapped, not just the Lua half (anti-#98).
    wait_line("hybrid V2 init compute=141");
    wait_line("hybrid V2 RELOADED compute=141");

    // The process is still running (a corrupted half-reload could crash on the
    // next frame); give it a moment then confirm it has not exited.
    std::thread::sleep(Duration::from_millis(200));
    assert!(
        matches!(child.try_wait(), Ok(None)),
        "player must still be running the reloaded cart after a coordinated reload"
    );

    let _ = child.kill();
    let _ = child.wait();

    // No silent half-reload: the v2 Lua half must never have run against the stale
    // v1 native half (which would print compute=42 under a V2 marker).
    let out = lines.lock().unwrap().join("\n");
    assert!(
        !out.contains("hybrid V2 init compute=42")
            && !out.contains("hybrid V2 RELOADED compute=42"),
        "coordinated reload must not leave the native half on the old image:\n{out}"
    );
}

/// Drive a cart-swap `reload` through the embedded libretro core
/// (test_libretro_core dlopens blyt_libretro.so with its OWN embedded guest
/// libs — a distinct artifact from the sdk/lib blobs blytplay loads).  At frame
/// `after` the harness calls blyt_libretro_reload_at(cart_v2); `extra_env` is a
/// general hook for extra core env.  Returns the captured stderr (cart debug
/// output arrives via the libretro log callback).
fn libretro_reload_capture(
    cart_v1: &std::path::Path,
    cart_v2: &std::path::Path,
    after: u32,
    frames: u32,
    extra_env: &[(&str, &str)],
) -> String {
    use assert_cmd::Command;
    let mut cmd = Command::new(test_libretro_core());
    cmd.arg("--run-frames")
        .arg(frames.to_string())
        .arg("--reload-after")
        .arg(after.to_string())
        .arg("--reload-path")
        .arg(cart_v2)
        .arg(libretro_so())
        .arg(cart_v1);
    for (k, v) in extra_env {
        cmd.env(k, v);
    }
    let out = cmd.assert().success().get_output().stderr.clone();
    String::from_utf8_lossy(&out).into_owned()
}

/// libretro leg of #246: build a pure-Lua cart pair whose bundled resource
/// content differs (RES_V1 → RES_V2), reload-swap v1→v2 mid-run, and assert the
/// post-swap on_load_state read surfaces RES_V2.  A pure-Lua cart runs host-Lua by
/// default (ADR-0136), so this drives the host-Lua reload path (`hostlua_reload_impl`
/// → `blyt_hostlua_reload`, #244); `extra_env` is a general hook.  A stale RES_V1
/// would mean the resource table still aliases the freed old cart map.
fn libretro_reload_resource_leg(extra_env: &[(&str, &str)], leg: &str) {
    let tmp = TempDir::new().unwrap();

    let project_v1 = tmp.path().join("reload_res_v1");
    CartProject::new()
        .config(DEV_CTRL_CONFIG)
        .lua(RELOAD_RESOURCE_LUA)
        .asset("greeting.txt", "RES_V1")
        .write(&project_v1);
    let v1 = build_lua_cart(&project_v1);

    let project_v2 = tmp.path().join("reload_res_v2");
    CartProject::new()
        .config(DEV_CTRL_CONFIG)
        .lua(RELOAD_RESOURCE_LUA)
        .asset("greeting.txt", "RES_V2")
        .write(&project_v2);
    let v2 = build_lua_cart(&project_v2);

    let out = libretro_reload_capture(&v1, &v2, 2, 6, extra_env);
    assert!(
        out.contains("init greeting=RES_V1"),
        "{leg}: cart_v1 did not read its bundled resource at init; core output:\n{out}"
    );
    assert!(
        out.contains("reload greeting=RES_V2 reason=3"),
        "{leg}: post-reload resource read did NOT return the new cart's content \
         (expected RES_V2) — resource table still aliases the freed old cart (#246); \
         core output:\n{out}"
    );
}

/// libretro embedded core, host-Lua path (the default, ADR-0136): pins the #244
/// `blyt_hostlua_reload` resource handling on the embedded-guest-lib artifact.
#[test]
fn libretro_dev_control_reload_reloads_resource_hostlua() {
    require_sdk();
    require_lua_sdk();
    require_libretro_core();
    libretro_reload_resource_leg(&[], "libretro host-Lua");
}

/// libretro embedded core, host-Lua HYBRID coordinated reload (#251): the .so
/// counterpart of `native_dev_control_reload_hybrid_coordinated_hostlua`, run
/// against blyt_libretro.so's OWN embedded guest libs (incl. libblyt32lua-bridge.so).
/// A hybrid runs host-Lua by default (ADR-0136); v1/v2 differ in BOTH halves
/// (native `compute` returns x+1 vs x+100, plus the Lua marker), so seeing 141
/// after the swap proves the native rv32 half was blyt_session_swap_cart'ed in
/// lockstep with the Lua-VM rebuild — not left on the old image (anti-#98).
#[test]
fn libretro_dev_control_reload_hybrid_coordinated_hostlua() {
    require_sdk();
    require_lua_sdk();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();

    let hybrid_c = |ret: &str| {
        format!(
            "#include \"blyt.h\"\n\
             BLYT_LUA_EXPORT_I32(compute, int32_t x) {{ return {ret}; }}\n"
        )
    };
    let hybrid_lua = |tag: &str| {
        format!(
            "function init() blyt.debug.print(\"hybrid {tag} init compute=\" .. compute(41)) end\n\
             function update() end\n\
             function draw() end\n\
             function on_load_state(info)\n\
                 blyt.debug.print(\"hybrid {tag} RELOADED compute=\" .. compute(41))\n\
             end\n"
        )
    };

    let project_v1 = tmp.path().join("libretro_hybrid_v1");
    CartProject::new()
        .config(DEV_CTRL_CONFIG)
        .c(&hybrid_c("x + 1"))
        .lua(&hybrid_lua("V1"))
        .write(&project_v1);
    let v1 = build_lua_cart(&project_v1);

    let project_v2 = tmp.path().join("libretro_hybrid_v2");
    CartProject::new()
        .config(DEV_CTRL_CONFIG)
        .c(&hybrid_c("x + 100"))
        .lua(&hybrid_lua("V2"))
        .write(&project_v2);
    let v2 = build_lua_cart(&project_v2);

    let out = libretro_reload_capture(&v1, &v2, 2, 6, &[]);
    assert!(
        out.contains("hybrid V1 init compute=42"),
        "libretro host-Lua hybrid: v1 did not run its native compute at init; core output:\n{out}"
    );
    assert!(
        out.contains("hybrid V2 RELOADED compute=141"),
        "libretro host-Lua hybrid: coordinated reload did not swap the native half \
         (expected compute=141 from the v2 native code); core output:\n{out}"
    );
    assert!(
        !out.contains("hybrid V2 RELOADED compute=42")
            && !out.contains("hybrid V2 init compute=42"),
        "libretro host-Lua hybrid: silent half-reload — v2 Lua ran against stale v1 native:\n{out}"
    );
}

/* ── native player as an outbound dev-control client (issue #90, option 2) ──── */

/// Spawn the player in dial-out mode (`--dev-ctrl-connect <port>`), draining its
/// stdout on a background thread.  Returns the child and a shared buffer of all
/// stdout lines.  Unlike the listen-mode helper there is no port to parse back —
/// the player dials the hub the caller already owns.
fn spawn_player_dialing(
    cart: &std::path::Path,
    hub_port: u16,
    save_dir: &std::path::Path,
) -> (std::process::Child, Arc<Mutex<Vec<String>>>) {
    let mut child = std::process::Command::new(blytplay())
        .args([
            "--headless",
            "--dev-ctrl-connect",
            &hub_port.to_string(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_SAVE_DIR", save_dir)
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped())
        .spawn()
        .expect("blytplay spawn");

    let stdout = child.stdout.take().unwrap();
    let lines = Arc::new(Mutex::new(Vec::<String>::new()));
    let lines_w = Arc::clone(&lines);
    std::thread::spawn(move || {
        let mut reader = BufReader::new(stdout);
        loop {
            let mut line = String::new();
            match reader.read_line(&mut line) {
                Ok(0) | Err(_) => break,
                Ok(_) => lines_w.lock().unwrap().push(line.trim_end().to_string()),
            }
        }
    });
    (child, lines)
}

/// The native player dials a dev control hub (rather than listening) and
/// services the full command set over the dialed connection — the "option 2"
/// reload wiring for player-dev mode (issue #90).  The test owns the hub: it
/// binds a TCP listener, the player connects outward, and the test drives the
/// same reset / save_state / load_state / reload protocol it would receive from
/// the devtool's broadcast hub.  Proves the dial-out transport carries every
/// command and that a code-swapping reload preserves state, exactly as the
/// listen path does.
#[test]
fn native_dev_control_connect_lifecycle() {
    require_sdk();
    require_lua_sdk();

    let tmp = TempDir::new().unwrap();
    let save_dir = TempDir::new().unwrap();

    let v1 = score_cart(&tmp, "connect_dc_v1", "v1", 7);
    let v2 = score_cart(&tmp, "connect_dc_v2", "v2", 100);

    let work = tmp.path().join("cart.blyt");
    std::fs::copy(&v1, &work).unwrap();

    // Stand up the hub side: an OS-assigned TCP port the player will dial.
    let hub = TcpListener::bind("127.0.0.1:0").expect("bind hub");
    let hub_port = hub.local_addr().unwrap().port();

    let (mut child, lines) = spawn_player_dialing(&work, hub_port, save_dir.path());

    // Accept the player's outbound connection (option 2: the player dials us).
    hub.set_nonblocking(false).unwrap();
    let (stream, _) = hub.accept().expect("player did not dial the hub");
    stream
        .set_read_timeout(Some(Duration::from_secs(10)))
        .unwrap();
    let mut stream = stream;
    let read_half = stream.try_clone().unwrap();
    let mut reader = BufReader::new(read_half);

    let check = |resp: &str, id: i64, cmd: &str| {
        assert_eq!(
            resp,
            format!("{{\"id\":{id},\"status\":\"ok\",\"cmd\":\"{cmd}\"}}"),
            "unexpected response"
        );
    };

    // reset + save_state pipelined in one write (boots the cart in reset so the
    // save captures init() state — same contract as the listen path).
    stream
        .write_all(
            concat!(
                "{\"id\":1,\"cmd\":\"reset\"}\n",
                "{\"id\":2,\"cmd\":\"save_state\",\"slot\":1}\n",
            )
            .as_bytes(),
        )
        .unwrap();
    stream.flush().unwrap();
    check(&dev_ctrl_read(&mut reader), 1, "reset");
    check(&dev_ctrl_read(&mut reader), 2, "save_state");

    check(
        &dev_ctrl_cmd(
            &mut stream,
            &mut reader,
            r#"{"id":3,"cmd":"load_state","slot":1}"#,
        ),
        3,
        "load_state",
    );

    // Code-swap then reload over the dialed connection.
    std::fs::copy(&v2, &work).unwrap();
    check(
        &dev_ctrl_cmd(&mut stream, &mut reader, r#"{"id":4,"cmd":"reload"}"#),
        4,
        "reload",
    );

    std::thread::sleep(Duration::from_millis(300));
    let _ = child.kill();
    let _ = child.wait();

    let out = lines.lock().unwrap().join("\n");
    assert!(
        out.contains("v1 load score=7 reason=0 version=5"),
        "dial-out load_state did not deliver the saved init() state / reason / \
         version; player output:\n{out}"
    );
    assert!(
        out.contains("v2 load score=7 reason=3 version=0"),
        "dial-out reload did not preserve state across the code swap or deliver \
         reason=HOT_RELOAD(3); player output:\n{out}"
    );
}

/// Drain a child's stdout into a shared line buffer on a background thread,
/// returning the buffer.  The thread ends at EOF (process exit / stdout close).
/// Keeping the pipe drained also prevents the child from blocking on a full
/// stdout buffer during a long-running session.
fn drain_stdout_lines(stdout: impl std::io::Read + Send + 'static) -> Arc<Mutex<Vec<String>>> {
    let lines = Arc::new(Mutex::new(Vec::<String>::new()));
    let w = Arc::clone(&lines);
    std::thread::spawn(move || {
        let mut reader = BufReader::new(stdout);
        loop {
            let mut line = String::new();
            match reader.read_line(&mut line) {
                Ok(0) | Err(_) => break,
                Ok(_) => w.lock().unwrap().push(line.trim_end().to_string()),
            }
        }
    });
    lines
}

/// Poll a shared line buffer until some line satisfies `pred`, up to `timeout`.
/// Returns true if a matching line appeared in time.  This is the readiness
/// primitive that replaces fixed sleeps: the caller waits on an observable
/// signal (a banner, the watcher-armed line) rather than guessing a delay.
fn wait_for_line(
    lines: &Arc<Mutex<Vec<String>>>,
    timeout: Duration,
    pred: impl Fn(&str) -> bool,
) -> bool {
    let start = std::time::Instant::now();
    loop {
        if lines.lock().unwrap().iter().any(|l| pred(l)) {
            return true;
        }
        if start.elapsed() >= timeout {
            return false;
        }
        std::thread::sleep(Duration::from_millis(20));
    }
}

/// Parse the TCP port out of a `Dev control:  127.0.0.1:<port> …` banner line.
fn parse_dev_ctrl_port(line: &str) -> Option<u16> {
    let idx = line.find("127.0.0.1:")?;
    let after = &line[idx + "127.0.0.1:".len()..];
    let end = after
        .find(|c: char| !c.is_ascii_digit())
        .unwrap_or(after.len());
    (end > 0).then(|| after[..end].parse().ok()).flatten()
}

/// End-to-end "option 2": the real devtool owns the hub and file watcher; the
/// player dials in and a watcher-driven rebuild reloads the *native* player the
/// same way it reloads the browser page.  Starts `blyt run ./project`, dials its
/// dev control port with `blytplay --dev-ctrl-connect` pointed at the *same*
/// project dir (so the player loads the devtool-rebuilt `build/.elf`), edits a
/// source file, and asserts the player's `on_load_state` fires with
/// reason=HOT_RELOAD(3) — i.e. the broadcast reached the dialed player.
///
/// Synchronisation (issue #290): the edit must not race the devtool's file
/// watcher.  notify does not deliver events that predate its `watch()` calls, so
/// an edit landing before the watcher is armed is lost — no rebuild, no reload,
/// and the generous *read* poll below can never see a reload that never fired.
/// A fixed `sleep(1s)` here under-waits under a saturated run (the hub-dial
/// sibling of #288 / PR #289).  Instead we drain the devtool's stdout and wait
/// for its `[watch] ready` armed-signal (and the player's dial-in banner) before
/// editing, and on failure attribute the miss to the exact stage that dropped it
/// using the watcher's own progress lines.
#[test]
fn player_dials_devtool_hub_and_reloads() {
    require_sdk();
    require_lua_sdk();
    require_wasm();

    let tmp = TempDir::new().unwrap();

    // A project whose on_load_state announces the reload reason.  `blyt run`
    // builds build/.elf in place; the player loads that same ELF and reloads it.
    let project = tmp.path().join("dial_reload");
    CartProject::new()
        .config(DEV_CTRL_CONFIG)
        .lua(
            "local slot = -1\n\
             function init()\n\
             \tslot = blyt.buf.alloc_slot(S.GAME)\n\
             \tS.game[slot].score = 7\n\
             end\n\
             function update() end\n\
             function draw() local _ = 1 end\n\
             function on_load_state(info)\n\
             \tblyt.debug.print('reloaded reason=' .. tostring(info.reason))\n\
             end\n",
        )
        .write(&project);

    // Start the devtool (release project-dir mode → hub + watcher + initial
    // build of build/.elf).  Drain its stdout so we can both read the dev ctrl
    // port and, later, attribute a missed reload to the stage that dropped it.
    let mut serve = spawn_blyt_run(&project);
    let dev_out = drain_stdout_lines(serve.stdout.take().unwrap());

    // Wait for the dev control port banner; fail fast (with stderr) if the
    // devtool exits before announcing it (e.g. the initial build failed).
    let port = loop {
        if let Some(p) = dev_out.lock().unwrap().iter().find_map(|l| {
            l.contains("Dev control:")
                .then(|| parse_dev_ctrl_port(l))
                .flatten()
        }) {
            break p;
        }
        if let Ok(Some(status)) = serve.try_wait() {
            let mut errout = String::new();
            if let Some(mut e) = serve.stderr.take() {
                e.read_to_string(&mut errout).ok();
            }
            panic!(
                "blyt run exited ({status}) before announcing the dev control port;\n\
                 devtool stdout:\n{}\nstderr:\n{errout}",
                dev_out.lock().unwrap().join("\n")
            );
        }
        std::thread::sleep(Duration::from_millis(20));
    };
    assert!(port != 0, "devtool dev control port must be a real port");

    // The player dials the hub and runs on the same project dir (loads build/.elf).
    let save_dir = TempDir::new().unwrap();
    let (mut child, lines) = spawn_player_dialing(&project, port, save_dir.path());

    // Synchronise on real readiness signals instead of a fixed sleep (#290):
    //  1. the devtool's file watcher is armed (`[watch] ready`) — else the edit
    //     predates the inotify watch and is silently dropped; and
    //  2. the player has dialed the hub (`Dev control: connected`) — else the
    //     reload broadcast reaches zero clients.
    assert!(
        wait_for_line(&dev_out, Duration::from_secs(20), |l| l == "[watch] ready"),
        "devtool file watcher never armed ([watch] ready) — the edit would race \
         the inotify watch; devtool output:\n{}",
        dev_out.lock().unwrap().join("\n")
    );
    assert!(
        wait_for_line(&lines, Duration::from_secs(10), |l| l
            .contains("Dev control: connected")),
        "player never dialed the devtool hub (no 'Dev control: connected' \
         banner); player output:\n{}",
        lines.lock().unwrap().join("\n")
    );

    // Edit a watched source file → devtool rebuilds → broadcasts reload → the
    // dialed player reopens build/.elf and runs on_load_state(HOT_RELOAD).
    let main_lua = project.join("src/game/lua/main.lua");
    std::fs::write(
        &main_lua,
        "local slot = -1\n\
         function init()\n\
         \tslot = blyt.buf.alloc_slot(S.GAME)\n\
         \tS.game[slot].score = 7\n\
         end\n\
         function update() end\n\
         function draw() local _ = 2 end\n\
         function on_load_state(info)\n\
         \tblyt.debug.print('reloaded reason=' .. tostring(info.reason))\n\
         end\n",
    )
    .unwrap();

    // Poll the player's stdout for the reload up to a generous window.
    let mut seen = false;
    for _ in 0..50 {
        if lines
            .lock()
            .unwrap()
            .iter()
            .any(|l| l.contains("reloaded reason=3"))
        {
            seen = true;
            break;
        }
        std::thread::sleep(Duration::from_millis(200));
    }

    let _ = child.kill();
    let _ = child.wait();
    let _ = serve.kill();
    let _ = serve.wait(); /* reap the devtool so it doesn't linger as a zombie */

    let out = lines.lock().unwrap().join("\n");
    let dev = dev_out.lock().unwrap().join("\n");

    // Attribute a miss to the exact stage that dropped it (AC #290): the panic
    // must distinguish "the watcher never fired a rebuild" from "the rebuild
    // fired but the broadcast never reached the player", not just dump stdout.
    let watcher_saw_edit = dev
        .lines()
        .any(|l| l.starts_with("[watch] ") && l.ends_with("changed"));
    let rebuild_ran = dev.contains("[build] rebuilding");
    let reload_broadcast = dev.lines().find(|l| l.starts_with("[reload] signalled"));
    let diagnosis = if !watcher_saw_edit {
        "the watcher never observed the edit (no '[watch] … changed') — the \
         inotify watch was not armed for the change despite [watch] ready"
    } else if let Some(rl) = reload_broadcast {
        if rl.contains("signalled 0 ") {
            "the rebuild fired but the reload reached 0 runtimes — the player was \
             not a registered hub client when the broadcast went out"
        } else {
            "the rebuild fired and the reload was broadcast to ≥1 runtime, but the \
             player never applied it (on_load_state did not run) — loss is player-side"
        }
    } else if rebuild_ran {
        "the watcher observed the edit and started a rebuild, but no reload was \
         broadcast (build failed or the cart was deemed unchanged) — see [build]/[reload]"
    } else {
        "the watcher observed the edit but no rebuild started"
    };

    assert!(
        seen,
        "watcher-driven reload did not reach the dialed player (expected \
         on_load_state reason=HOT_RELOAD(3)).\nDiagnosis: {diagnosis}.\n\
         --- devtool output ---\n{dev}\n--- player output ---\n{out}"
    );
}
