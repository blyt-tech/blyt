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
    CartProject, blyt_bin, blytplay, build_lua_cart, require_lua_sdk, require_sdk, require_wasm,
    sdk_dir,
};
use std::io::{BufRead, BufReader, Read as _, Write as _};
use std::net::TcpStream;
use std::sync::mpsc;
use std::sync::{Arc, Mutex};
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
/// on_load_state prints "<tag> load score=<n> reason=<r>".  The host marshals
/// `blyt_load_info_t` through the guest ABI (PR #109), so the cart reads
/// `info.reason` to prove the correct trigger reaches it: SAVE_GAME (0) for a
/// dev-control `load_state`, HOT_RELOAD (3) for a `reload`.
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
             \t\t.. ' reason=' .. tostring(info.reason))\n\
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
    let mut child = std::process::Command::new(blytplay())
        .args(["--headless", "--dev-ctrl-port", "0", cart.to_str().unwrap()])
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
    assert!(
        out.contains("v1 load score=7 reason=0"),
        "reset+save_state race: save did not capture init() state \
         (issue #105), or load_state did not deliver reason=SAVE_GAME(0); \
         player output:\n{out}"
    );
    // reload delivers reason HOT_RELOAD (3) and preserves v1's state into v2.
    assert!(
        out.contains("v2 load score=7 reason=3"),
        "reload did not preserve state across the code swap, or did not deliver \
         reason=HOT_RELOAD(3); player output:\n{out}"
    );
}
