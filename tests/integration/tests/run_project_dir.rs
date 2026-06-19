mod common;

use common::{CartProject, blyt_bin, require_sdk, require_wasm, require_wasm_debug, sdk_dir};
use std::fs;
use std::io::{Read as _, Write as _};
use std::net::TcpStream;
use tempfile::TempDir;

/// Minimal Lua cart that quits immediately; fast to build (luac only) and
/// covers the pure-Lua path in `blyt run ./dir`.
fn lua_quit_cart(tmp: &TempDir) -> std::path::PathBuf {
    let project = tmp.path().join("run_dir_test");
    CartProject::new()
        .lua(
            "function blyt_cart_init() end\n\
             function blyt_cart_update() blyt_quit() end\n\
             function blyt_cart_draw() end\n",
        )
        .write(&project);
    project
}

/// Spawn `blyt <sub> <project_dir>` with the standard SDK env vars and return
/// the running child process.
fn spawn_blyt(sub: &str, project: &std::path::Path) -> std::process::Child {
    let sdk = sdk_dir();
    let mut cmd = std::process::Command::new(blyt_bin());
    cmd.args([sub, project.to_str().unwrap()])
        .env("BLYT_SDK_DIR", &sdk)
        .env("BLYT_OBJCOPY", sdk.join("bin/blyt-objcopy"));
    let sdk_clang = sdk.join("bin/blyt-clang");
    if sdk_clang.exists() {
        cmd.env("BLYT_CLANG", &sdk_clang);
    }
    let sdk_luac = sdk.join("bin/blyt-luac");
    if sdk_luac.exists() {
        cmd.env("BLYT_LUAC", &sdk_luac);
    }
    cmd.stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::null())
        .spawn()
        .unwrap_or_else(|e| panic!("blyt {sub} spawn: {e}"))
}

/// Read from `child`'s stdout until "http://127.0.0.1:PORT/" appears and
/// return the port number.  Panics (after killing the child) if stdout closes
/// before the banner is seen.
fn read_port(child: &mut std::process::Child, sub: &str) -> u16 {
    let mut stdout = child.stdout.take().unwrap();
    let mut banner = String::new();
    loop {
        let mut chunk = [0u8; 1024];
        let n = stdout.read(&mut chunk).expect("read stdout");
        if n == 0 {
            let _ = child.kill();
            panic!("blyt {sub} exited before announcing a port; banner:\n{banner}");
        }
        banner.push_str(&String::from_utf8_lossy(&chunk[..n]));
        if let Some(idx) = banner.find("http://127.0.0.1:") {
            let rest = &banner[idx + "http://127.0.0.1:".len()..];
            if let Some(end) = rest.find('/') {
                return rest[..end].parse().expect("port parse");
            }
        }
    }
}

/// Fetch the served HTML page at `GET /`; return the full response as a string.
/// Uses `read_to_string` (same as `trace.rs`) to avoid platform RST differences
/// that affect large binary transfers.
fn fetch_root_page(port: u16) -> String {
    let mut conn = TcpStream::connect(("127.0.0.1", port))
        .unwrap_or_else(|e| panic!("connect to blyt server: {e}"));
    conn.write_all(b"GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        .unwrap();
    let mut page = String::new();
    conn.read_to_string(&mut page).unwrap();
    page
}

/* ── release path ─────────────────────────────────────────────────────────── */

/// `blyt run ./project` auto-builds a dev ELF (build/.elf), starts the WASM
/// server, and serves the HTML player page at GET /.
#[test]
fn run_accepts_project_directory() {
    require_sdk();
    require_wasm();

    let tmp = TempDir::new().unwrap();
    let project = lua_quit_cart(&tmp);

    let mut serve = spawn_blyt("run", &project);
    let port = read_port(&mut serve, "run");

    // The release dev ELF must exist and have ELF magic.
    let elf_path = project.join("build/.elf");
    assert!(
        elf_path.exists(),
        "build/.elf not created by blyt run ./dir"
    );
    let magic = &fs::read(&elf_path).expect("read .elf")[..4];
    assert_eq!(magic, b"\x7fELF", "build/.elf has wrong magic");

    // The HTTP server must be up and serving the WASM player page.
    let page = fetch_root_page(port);
    let _ = serve.kill();
    assert!(!page.is_empty(), "blyt run served empty page at GET /");
}

/* ── debug path ───────────────────────────────────────────────────────────── */

/// `blyt debug ./project` builds a debug dev ELF (build/.dbg.elf) and starts
/// the debug WASM server.
#[test]
fn debug_accepts_project_directory() {
    require_sdk();
    require_wasm_debug();

    let tmp = TempDir::new().unwrap();
    let project = lua_quit_cart(&tmp);

    let mut serve = spawn_blyt("debug", &project);
    let port = read_port(&mut serve, "debug");

    // The debug dev ELF must exist and have ELF magic.
    let elf_path = project.join("build/.dbg.elf");
    assert!(
        elf_path.exists(),
        "build/.dbg.elf not created by blyt debug ./dir"
    );
    let magic = &fs::read(&elf_path).expect("read .dbg.elf")[..4];
    assert_eq!(magic, b"\x7fELF", "build/.dbg.elf has wrong magic");

    // The HTTP debug server must be up and serving the WASM player page.
    let page = fetch_root_page(port);
    let _ = serve.kill();
    assert!(!page.is_empty(), "blyt debug served empty page at GET /");
}
