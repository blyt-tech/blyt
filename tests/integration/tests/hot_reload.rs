//! Hot reload (issue #88): the embedded file watcher in `blyt run ./dir`.
//!
//! Watching is implicit in project-dir mode — editing a source file rebuilds
//! the cart incrementally and broadcasts `{"id":N,"cmd":"reload"}` to every
//! connected dev control client.  These tests drive the devtool side over its
//! dev control TCP port (the broadcast hub); the runtime-side reload sequence
//! is covered by `dev_control.rs` / `wasm.rs`.

mod common;

use common::{CartProject, blyt_bin, require_sdk, require_wasm, sdk_dir};
use std::fs;
use std::io::{BufRead, BufReader, Read as _};
use std::net::TcpStream;
use std::path::Path;
use std::time::Duration;
use tempfile::TempDir;

/// A minimal Lua cart that runs forever, so the server stays up while we probe.
fn idle_lua_cart(tmp: &TempDir, name: &str) -> std::path::PathBuf {
    let project = tmp.path().join(name);
    CartProject::new()
        .lua("function init() end\nfunction update() end\nfunction draw() end\n")
        .write(&project);
    project
}

/// The cart's main source file (CartProject writes Lua to src/game/lua/main.lua).
fn main_lua(project: &Path) -> std::path::PathBuf {
    project.join("src/game/lua/main.lua")
}

/// Overwrite main.lua with a draw body carrying `marker`, so each edit is a
/// genuine content change that re-fingerprints the build.
fn edit_main(project: &Path, marker: &str) {
    fs::write(
        main_lua(project),
        format!(
            "function init() end\nfunction update() end\nfunction draw() local _ = {marker} end\n"
        ),
    )
    .unwrap();
}

fn spawn_blyt_run(project: &Path) -> std::process::Child {
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

/// Read stdout until the dev control banner appears; return the announced TCP
/// port and the still-open stdout handle (kept alive so the server does not
/// SIGPIPE on its later watcher log writes).
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

/// Connect a dev control client and wrap it in a line reader with the given
/// read timeout.  Returns the reader (its inner stream owns the timeout).
fn connect_client(port: u16, timeout: Duration) -> BufReader<TcpStream> {
    let stream = TcpStream::connect(("127.0.0.1", port)).expect("connect dev control");
    stream.set_read_timeout(Some(timeout)).unwrap();
    BufReader::new(stream)
}

/// Read one newline-delimited line; `None` on timeout/EOF.
fn read_line(reader: &mut BufReader<TcpStream>) -> Option<String> {
    let mut line = String::new();
    match reader.read_line(&mut line) {
        Ok(0) | Err(_) => None,
        Ok(_) => Some(line.trim_end().to_string()),
    }
}

const RELOAD_1: &str = r#"{"id":1,"cmd":"reload"}"#;

/// Editing a watched source file rebuilds the cart and signals connected dev
/// control clients with `{"id":1,"cmd":"reload"}`.
#[test]
fn hot_reload_edit_triggers_reload_signal() {
    require_sdk();
    require_wasm();

    let tmp = TempDir::new().unwrap();
    let project = idle_lua_cart(&tmp, "hot_reload_basic");

    let mut serve = spawn_blyt_run(&project);
    let (port, _stdout) = read_dev_ctrl_port(&mut serve);

    let mut reader = connect_client(port, Duration::from_secs(20));
    // Let the client register with the hub before the rebuild broadcasts.
    std::thread::sleep(Duration::from_millis(300));

    edit_main(&project, "1");

    let line = read_line(&mut reader);
    let _ = serve.kill();
    assert_eq!(
        line.as_deref(),
        Some(RELOAD_1),
        "edit did not produce a reload signal"
    );
}

/// A failed rebuild keeps the running cart and sends no reload; the dev control
/// connection stays open and a subsequent good build reloads as normal.
#[test]
fn hot_reload_compile_error_skips_then_recovers() {
    require_sdk();
    require_wasm();

    let tmp = TempDir::new().unwrap();
    let project = idle_lua_cart(&tmp, "hot_reload_error");

    let mut serve = spawn_blyt_run(&project);
    let (port, _stdout) = read_dev_ctrl_port(&mut serve);

    let mut reader = connect_client(port, Duration::from_secs(3));
    std::thread::sleep(Duration::from_millis(300));

    // Introduce a syntax error: no reload should arrive within the window.
    fs::write(main_lua(&project), "function init( end\n").unwrap();
    assert_eq!(
        read_line(&mut reader),
        None,
        "a failed build must not send a reload signal"
    );

    // The connection must still be usable — fix the source and expect a reload.
    // The failed build did not consume id 1, so the first success is still id 1.
    reader
        .get_ref()
        .set_read_timeout(Some(Duration::from_secs(20)))
        .unwrap();
    edit_main(&project, "2");

    let line = read_line(&mut reader);
    let _ = serve.kill();
    assert_eq!(
        line.as_deref(),
        Some(RELOAD_1),
        "recovery build did not produce a reload signal"
    );
}

/// Multiple TCP clients (blytplay + VS Code, say) all receive the broadcast.
#[test]
fn hot_reload_broadcasts_to_all_clients() {
    require_sdk();
    require_wasm();

    let tmp = TempDir::new().unwrap();
    let project = idle_lua_cart(&tmp, "hot_reload_broadcast");

    let mut serve = spawn_blyt_run(&project);
    let (port, _stdout) = read_dev_ctrl_port(&mut serve);

    let mut a = connect_client(port, Duration::from_secs(20));
    let mut b = connect_client(port, Duration::from_secs(20));
    std::thread::sleep(Duration::from_millis(300));

    edit_main(&project, "3");

    let la = read_line(&mut a);
    let lb = read_line(&mut b);
    let _ = serve.kill();
    assert_eq!(
        la.as_deref(),
        Some(RELOAD_1),
        "client A missed the broadcast"
    );
    assert_eq!(
        lb.as_deref(),
        Some(RELOAD_1),
        "client B missed the broadcast"
    );
}

/// A burst of rapid writes coalesces into exactly one rebuild / one reload.
#[test]
fn hot_reload_burst_coalesces_to_one() {
    require_sdk();
    require_wasm();

    let tmp = TempDir::new().unwrap();
    let project = idle_lua_cart(&tmp, "hot_reload_burst");

    let mut serve = spawn_blyt_run(&project);
    let (port, _stdout) = read_dev_ctrl_port(&mut serve);

    let mut reader = connect_client(port, Duration::from_secs(20));
    std::thread::sleep(Duration::from_millis(300));

    // Five rapid back-to-back writes — well inside the debounce settle window.
    for i in 0..5 {
        edit_main(&project, &i.to_string());
    }

    // Exactly one reload, then nothing more within a generous window.
    let first = read_line(&mut reader);
    assert_eq!(
        first.as_deref(),
        Some(RELOAD_1),
        "burst did not produce a reload"
    );

    reader
        .get_ref()
        .set_read_timeout(Some(Duration::from_secs(2)))
        .unwrap();
    let second = read_line(&mut reader);
    let _ = serve.kill();
    assert_eq!(
        second, None,
        "burst produced more than one reload (expected coalesce): {second:?}"
    );
}
