use std::fs;
use std::io::{BufRead, BufReader, Read, Write};
use std::net::{TcpListener, TcpStream};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex, mpsc};

use notify::{RecursiveMode, Watcher};
use tungstenite::Message;

use crate::build::{build_for_dev, json_escape, sdk_root_from_exe};

/* -------------------------------------------------------------------------
 * Error type
 * ------------------------------------------------------------------------- */

#[derive(Debug)]
pub struct RunError(String);

impl std::fmt::Display for RunError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}", self.0)
    }
}

impl From<std::io::Error> for RunError {
    fn from(e: std::io::Error) -> Self {
        RunError(e.to_string())
    }
}

fn err(msg: impl Into<String>) -> RunError {
    RunError(msg.into())
}

/* -------------------------------------------------------------------------
 * Public entry points (ADR-0129)
 *
 * `run` serves a release cart with the release WASM runtime and no debugger.
 * `debug` serves a debug (.dbg.blyt) cart with the debug WASM runtime and the
 * DAP + GDB relays.  The two artifacts are fully separate (share/wasm vs
 * share/wasm-debug, blytplay.* vs blytdebug.*) so a release page carries zero
 * debug machinery.
 * ------------------------------------------------------------------------- */

#[derive(Clone, Copy)]
enum Mode {
    Release,
    Debug,
}

impl Mode {
    /// WASM artifact basename (blytplay.* / blytdebug.*).
    fn wasm_name(self) -> &'static str {
        match self {
            Mode::Release => "blytplay",
            Mode::Debug => "blytdebug",
        }
    }
    fn is_debug(self) -> bool {
        matches!(self, Mode::Debug)
    }
}

/// Serve a release cart in the browser (no debugger).
pub fn run(cart_path: &Path, trace: Option<&str>) -> Result<(), RunError> {
    serve_cart(cart_path, Mode::Release, trace, None)
}

/// Serve a debug cart with the DAP/GDB debug runtime.
pub fn debug(cart_path: &Path, trace: Option<&str>) -> Result<(), RunError> {
    serve_cart(cart_path, Mode::Debug, trace, None)
}

/// Resolve the BLYT_TRACE channel list for the served runtime: the --trace
/// flag wins, then the BLYT_TRACE environment variable.  Sanitised to the
/// channel-list alphabet so the value is safe to splice into the shell.html
/// script template.
fn resolve_trace(flag: Option<&str>) -> String {
    let raw = flag
        .map(str::to_owned)
        .or_else(|| std::env::var("BLYT_TRACE").ok())
        .unwrap_or_default();
    raw.chars()
        .filter(|c| c.is_ascii_alphanumeric() || *c == ',' || *c == '_')
        .collect()
}

fn serve_cart_from_project_dir(
    project_dir: &Path,
    mode: Mode,
    trace: Option<&str>,
) -> Result<(), RunError> {
    let elf_path = build_for_dev(project_dir, mode.is_debug(), false)
        .map_err(|e| err(format!("build failed: {e}")))?;
    // Canonicalise so the file watcher and the build/ exclusion match the
    // absolute paths notify reports for fs events.
    let project_dir = project_dir
        .canonicalize()
        .unwrap_or_else(|_| project_dir.to_path_buf());
    // project_dir = Some: the dev control channel (ADR-0045/issue #87) is
    // always-on in project-dir mode so the embedded file watcher (issue #88),
    // VS Code, and manual tooling can drive runtime lifecycle commands
    // (reload/save/load/reset).  Passing the source dir also enables hot reload.
    serve_cart(&elf_path, mode, trace, Some(&project_dir))
}

fn serve_cart(
    cart_path: &Path,
    mode: Mode,
    trace: Option<&str>,
    project_dir: Option<&Path>,
) -> Result<(), RunError> {
    if cart_path.is_dir() {
        return serve_cart_from_project_dir(cart_path, mode, trace);
    }

    let cart_path = cart_path
        .canonicalize()
        .map_err(|e| err(format!("cannot open cart '{}': {}", cart_path.display(), e)))?;

    // Debug mode requires a debuggable cart: native (C/Rust/C++) carts need DWARF
    // (a `blyt build --debug` build); Lua carts step via the debug runtime's
    // master hook, so any Lua cart (`.cart.lua` section) is fine (ADR-0129).
    if mode.is_debug() && !cart_is_debuggable(&cart_path)? {
        return Err(err(format!(
            "cart is not a debug build — rebuild with `blyt build --debug`:\n  {}",
            cart_path.display()
        )));
    }

    let name = mode.wasm_name();
    let wasm_dir = find_wasm_dir_for(mode)?;

    // Validate that required WASM files are present.
    for ext in ["js", "wasm", "html"] {
        let file = format!("{name}.{ext}");
        if !wasm_dir.join(&file).exists() {
            return Err(err(format!(
                "WASM runtime file missing: {}/{file}\n\
                 Rebuild the SDK (cmake --build build --target sdk).",
                wasm_dir.display(),
            )));
        }
    }

    // Bind on an OS-assigned port so there are no port collisions.
    let listener = TcpListener::bind("127.0.0.1:0")?;
    let port = listener.local_addr()?.port();

    let debug = mode.is_debug();

    // DAP relay: WebSocket on PORT+1 (WASM runtime), raw TCP on PORT+2 (VS Code).
    // GDB relay: WebSocket on PORT+3 (WASM runtime), raw TCP on PORT+4 (gdb).
    // Only started in debug mode.
    let (dap_ws_port, dap_tcp_port) = if debug {
        start_dap_relay(port.wrapping_add(1), port.wrapping_add(2))
    } else {
        (0, 0)
    };
    let (gdb_ws_port, gdb_tcp_port) = if debug {
        start_gdb_relay(port.wrapping_add(3), port.wrapping_add(4))
    } else {
        (0, 0)
    };

    // Dev control hub: WebSocket on PORT+5 (WASM runtime), raw newline-delimited
    // JSON TCP on PORT+6 (external tools — blytplay, VS Code).  Unlike DAP/GDB
    // this is always started in project-dir mode, independent of debug (the
    // release player must support hot reload too — issue #87).  The TCP side is a
    // broadcast hub accepting any number of clients so the file watcher can
    // signal `reload` to all of them at once (issue #88).
    let (dev_ctrl_ws_port, dev_ctrl_tcp_port, dev_ctrl_hub) = match project_dir {
        Some(_) => {
            let (ws, tcp, hub) = start_dev_ctrl_relay(port.wrapping_add(5), port.wrapping_add(6));
            (ws, tcp, Some(hub))
        }
        None => (0, 0, None),
    };

    let trace = resolve_trace(trace);

    println!("blyt run: serving on http://127.0.0.1:{port}/");
    println!("  Cart:     {}", cart_path.display());
    println!("  WASM dir: {}", wasm_dir.display());
    if !trace.is_empty() {
        println!("  Trace:    {trace} (BLYT_TRACE — browser console / stderr)");
    }
    println!();
    println!("Open http://127.0.0.1:{port}/ in your browser.");
    println!("Press Ctrl+C to stop.");
    if dev_ctrl_tcp_port != 0 {
        println!();
        println!("  Dev control:  127.0.0.1:{dev_ctrl_tcp_port}   (TCP — send JSON commands here)");
    }
    if project_dir.is_some() {
        println!("  Watching:     src/, blyt.build.yaml, blyt.config.yaml, blyt.info.yaml");
    }
    if debug {
        println!();
        println!("  DAP debugger (Lua):    127.0.0.1:{dap_tcp_port}");
        println!("  GDB debugger:          127.0.0.1:{gdb_tcp_port}");
        println!("  (gdb-multiarch: set arch riscv:rv32 && target remote :{gdb_tcp_port})");
        // Announce the canonical source-path manifest (issue #46 §2) so the VS
        // Code extension can load the /blyt/* → local mappings instead of
        // hardcoding them.  Lives in the cart's build dir alongside the .blyt.
        if let Some(map) = cart_path
            .parent()
            .map(|d| d.join("source-map.json"))
            .filter(|p| p.exists())
        {
            println!("  Source map:            {}", map.display());
        }
    }

    // Hot reload (issue #88): in project-dir mode, watch the sources and rebuild
    // on change, broadcasting `reload` to every connected dev control client.
    if let (Some(pdir), Some(hub)) = (project_dir, &dev_ctrl_hub) {
        start_watcher(pdir.to_path_buf(), debug, hub.clone());
    }

    serve(
        listener,
        Arc::new(wasm_dir),
        Arc::new(cart_path),
        Arc::new(name.to_string()),
        Arc::new(trace),
        dap_ws_port,
        gdb_ws_port,
        dev_ctrl_ws_port,
    )
}

/* -------------------------------------------------------------------------
 * Debug-cart verification.  A cart is debuggable under `blyt debug` if it
 * carries DWARF (.debug_* — native debug build) or is a Lua cart (.cart.lua,
 * which steps via the debug runtime master hook regardless of cart build).
 * Minimal ELF32 (little-endian) section-name scan — no external tools.
 * ------------------------------------------------------------------------- */

fn cart_is_debuggable(cart_path: &Path) -> Result<bool, RunError> {
    let data = fs::read(cart_path)
        .map_err(|e| err(format!("cannot read cart '{}': {}", cart_path.display(), e)))?;

    // ELF32 header offsets (little-endian).
    if data.len() < 52 || &data[0..4] != b"\x7fELF" || data[4] != 1 {
        return Err(err(format!(
            "not a valid ELF32 cart: {}",
            cart_path.display()
        )));
    }
    let rd_u32 = |off: usize| -> u32 {
        u32::from_le_bytes([data[off], data[off + 1], data[off + 2], data[off + 3]])
    };
    let rd_u16 = |off: usize| -> u16 { u16::from_le_bytes([data[off], data[off + 1]]) };

    let e_shoff = rd_u32(0x20) as usize;
    let e_shentsize = rd_u16(0x2e) as usize;
    let e_shnum = rd_u16(0x30) as usize;
    let e_shstrndx = rd_u16(0x32) as usize;
    if e_shoff == 0 || e_shnum == 0 || e_shstrndx >= e_shnum {
        return Ok(false);
    }

    // .shstrtab section header → its file offset.
    let strtab_hdr = e_shoff + e_shstrndx * e_shentsize;
    if strtab_hdr + 0x18 > data.len() {
        return Ok(false);
    }
    let strtab_off = rd_u32(strtab_hdr + 0x10) as usize;

    for i in 0..e_shnum {
        let sh = e_shoff + i * e_shentsize;
        if sh + 4 > data.len() {
            break;
        }
        let name_off = strtab_off + rd_u32(sh) as usize;
        // Read the NUL-terminated section name.
        if let Some(end) = data[name_off..].iter().position(|&b| b == 0) {
            let name = &data[name_off..name_off + end];
            if name.starts_with(b".debug_") || name.starts_with(b".zdebug_") || name == b".cart.lua"
            {
                return Ok(true);
            }
        }
    }
    Ok(false)
}

/* -------------------------------------------------------------------------
 * DAP relay
 *
 * Two listeners:
 *   ws_port  — WebSocket, for the WASM runtime (injected into shell.html)
 *   tcp_port — raw TCP Content-Length framing, for VS Code
 *
 * Each session bridges one WebSocket connection and one TCP connection,
 * converting between the two framings bidirectionally.
 *
 * Returns the actual bound ports (may differ from requested if 0 is passed).
 * ------------------------------------------------------------------------- */

/// Bind a relay listener on the preferred port, falling back to an
/// OS-assigned one.  The preferred ports (HTTP port +1..+4) sit in the same
/// ephemeral range the OS hands to every concurrent bind(:0)/connect, so
/// collisions are routine under load (this killed `blyt debug` on busy CI
/// runners).  Falling back is always safe: every consumer — the banner, the
/// {{BLYT_*_PORT}} page injection, the VS Code extension regexes, the test
/// drivers — reads the actual bound port, never the +N arithmetic.
fn bind_relay(preferred: u16, what: &str) -> TcpListener {
    TcpListener::bind(("127.0.0.1", preferred))
        .or_else(|_| TcpListener::bind(("127.0.0.1", 0)))
        .unwrap_or_else(|e| panic!("{what}: bind failed: {e}"))
}

fn start_dap_relay(ws_port: u16, tcp_port: u16) -> (u16, u16) {
    let ws_listener = bind_relay(ws_port, "DAP relay (WebSocket)");
    let tcp_listener = bind_relay(tcp_port, "DAP relay (TCP)");

    let actual_ws = ws_listener
        .local_addr()
        .map(|a| a.port())
        .unwrap_or(ws_port);
    let actual_tcp = tcp_listener
        .local_addr()
        .map(|a| a.port())
        .unwrap_or(tcp_port);

    std::thread::spawn(move || {
        run_relay_loop(ws_listener, tcp_listener);
    });

    (actual_ws, actual_tcp)
}

fn run_relay_loop(ws_listener: TcpListener, tcp_listener: TcpListener) {
    use std::sync::mpsc;

    loop {
        let (ws_tx, ws_rx) = mpsc::channel();
        let (tcp_tx, tcp_rx) = mpsc::channel::<TcpStream>();

        // Accept the WASM runtime WebSocket connection.
        let ws_listener2 = ws_listener.try_clone().expect("clone ws listener");
        std::thread::spawn(move || {
            if let Ok((stream, _)) = ws_listener2.accept() {
                if let Ok(ws) = tungstenite::accept(stream) {
                    let _ = ws_tx.send(ws);
                }
            }
        });

        // Accept the VS Code TCP connection.
        let tcp_listener2 = tcp_listener.try_clone().expect("clone tcp listener");
        std::thread::spawn(move || {
            if let Ok((stream, _)) = tcp_listener2.accept() {
                let _ = tcp_tx.send(stream);
            }
        });

        // Wait for both sides to connect.
        let ws = match ws_rx.recv() {
            Ok(ws) => ws,
            Err(_) => continue,
        };
        let tcp = match tcp_rx.recv() {
            Ok(s) => s,
            Err(_) => continue,
        };

        run_relay_session(ws, tcp);
        // Session ended; loop to accept the next pair.
    }
}

type WsConn = tungstenite::WebSocket<TcpStream>;

fn run_relay_session(mut ws: WsConn, tcp: TcpStream) {
    use std::io::ErrorKind::WouldBlock;
    use std::sync::mpsc;

    // Two channels bridge the two transports:
    //   tcp_to_ws: TCP reader thread → WS I/O thread (writes to WS)
    //   ws_to_tcp: WS I/O thread (reads from WS) → TCP writer (this thread)
    let (tcp_to_ws_tx, tcp_to_ws_rx) = mpsc::channel::<String>();
    let (ws_to_tcp_tx, ws_to_tcp_rx) = mpsc::channel::<String>();

    let tcp_write = match tcp.try_clone() {
        Ok(s) => s,
        Err(_) => return,
    };
    let tcp_read = BufReader::new(tcp);

    // Thread: blocking TCP reader → sends messages to ws I/O thread.
    std::thread::spawn(move || {
        let mut r = tcp_read;
        while let Some(msg) = read_cl(&mut r) {
            if tcp_to_ws_tx.send(msg).is_err() {
                break;
            }
        }
    });

    // Thread: owns the WebSocket exclusively; polls non-blocking so it can
    // interleave reads (WS→TCP) and writes (TCP→WS) without a shared mutex.
    std::thread::spawn(move || {
        ws.get_mut().set_nonblocking(true).ok();
        let delay = std::time::Duration::from_micros(100);
        'relay: loop {
            // Drain all pending inbound WS frames.
            loop {
                match ws.read() {
                    Ok(Message::Text(s)) => {
                        if ws_to_tcp_tx.send(s.to_string()).is_err() {
                            break 'relay;
                        }
                    }
                    Ok(Message::Binary(b)) => {
                        if let Ok(s) = String::from_utf8(b.into()) {
                            if ws_to_tcp_tx.send(s).is_err() {
                                break 'relay;
                            }
                        }
                    }
                    Ok(Message::Close(_)) => break 'relay,
                    Err(tungstenite::Error::Io(e)) if e.kind() == WouldBlock => break,
                    Err(_) => break 'relay,
                    _ => break,
                }
            }
            // Write any messages that arrived from the TCP reader.
            loop {
                match tcp_to_ws_rx.try_recv() {
                    Ok(msg) => {
                        if ws.send(Message::Text(msg.into())).is_err() {
                            break 'relay;
                        }
                    }
                    Err(mpsc::TryRecvError::Empty) => break,
                    Err(mpsc::TryRecvError::Disconnected) => break 'relay,
                }
            }
            // Block until the TCP reader delivers the next message or 1ms elapses.
            // This wakes immediately when LLDB data arrives instead of sleeping a full ms.
            match tcp_to_ws_rx.recv_timeout(delay) {
                Ok(msg) => {
                    if ws.send(Message::Text(msg.into())).is_err() {
                        break 'relay;
                    }
                }
                Err(mpsc::RecvTimeoutError::Timeout) => {}
                Err(mpsc::RecvTimeoutError::Disconnected) => break 'relay,
            }
        }
    });

    // This thread: forwards WS messages to the TCP client (VS Code) with CL framing.
    let mut tcp_write = tcp_write;
    for msg in ws_to_tcp_rx {
        if write_cl(&mut tcp_write, &msg).is_err() {
            break;
        }
    }
}

/* Write one DAP message with Content-Length framing. */
fn write_cl(w: &mut impl Write, json: &str) -> std::io::Result<()> {
    write!(w, "Content-Length: {}\r\n\r\n{}", json.len(), json)?;
    w.flush()
}

/* Read one Content-Length–framed DAP message.  Returns None on EOF or error. */
fn read_cl(r: &mut impl BufRead) -> Option<String> {
    let mut content_length: Option<usize> = None;
    loop {
        let mut line = String::new();
        r.read_line(&mut line).ok()?;
        let line = line.trim_end_matches(|c| c == '\r' || c == '\n');
        if line.is_empty() {
            break; // blank line separates headers from body
        }
        if let Some(val) = line.strip_prefix("Content-Length:") {
            content_length = val.trim().parse().ok();
        }
    }
    let len = content_length?;
    let mut buf = vec![0u8; len];
    r.read_exact(&mut buf).ok()?;
    String::from_utf8(buf).ok()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    fn idx(pairs: &[(u32, &str)]) -> Vec<(u32, String)> {
        pairs.iter().map(|(id, p)| (*id, p.to_string())).collect()
    }

    #[test]
    fn safe_resource_rel_accepts_content_addressed_names() {
        // The names the browser actually fetches (from the resource-id-index).
        assert!(is_safe_resource_rel("greeting-62cedeea5c679bca.data"));
        assert!(is_safe_resource_rel("hero_idle-deadbeefdeadbeef.data"));
        // Nested is allowed as long as no component traverses.
        assert!(is_safe_resource_rel("sprites/hero-aaaa.data"));
    }

    #[test]
    fn safe_resource_rel_rejects_traversal_and_empties() {
        assert!(!is_safe_resource_rel(""));
        assert!(!is_safe_resource_rel(".."));
        assert!(!is_safe_resource_rel("../secret"));
        assert!(!is_safe_resource_rel("a/../../etc/passwd"));
        assert!(!is_safe_resource_rel("a/./b"));
        assert!(!is_safe_resource_rel("a//b")); // empty component
        assert!(!is_safe_resource_rel("/etc/passwd")); // leading slash → empty first component
    }

    #[test]
    fn resource_name_parses_from_staging_path() {
        assert_eq!(
            resource_name_of("resources/greeting-62cedeea5c679bca.data"),
            "greeting"
        );
        assert_eq!(
            resource_name_of("resources/hero_idle-deadbeefdeadbeef.data"),
            "hero_idle"
        );
    }

    #[test]
    fn dispatch_content_edit_is_hot_swap_only() {
        let old = idx(&[(1, "resources/greeting-aaaa.data")]);
        let new = idx(&[(1, "resources/greeting-bbbb.data")]);
        // Same id+name, new fingerprint, no code change → update_assets only.
        assert_eq!(dispatch_signals(&old, &new, false), (false, vec![1]));
    }

    #[test]
    fn dispatch_code_and_asset_change_updates_then_reloads() {
        let old = idx(&[(1, "resources/greeting-aaaa.data")]);
        let new = idx(&[(1, "resources/greeting-bbbb.data")]);
        // Code changed too → update_assets first, then reload.
        assert_eq!(dispatch_signals(&old, &new, true), (true, vec![1]));
    }

    #[test]
    fn dispatch_resource_set_change_forces_reload() {
        let old = idx(&[(1, "resources/greeting-aaaa.data")]);
        // Asset added → id/name set changed → full reload, no update_assets.
        let new = idx(&[
            (1, "resources/greeting-aaaa.data"),
            (2, "resources/farewell-cccc.data"),
        ]);
        assert_eq!(dispatch_signals(&old, &new, false), (true, vec![]));
    }

    #[test]
    fn dispatch_rename_forces_reload() {
        let old = idx(&[(1, "resources/greeting-aaaa.data")]);
        // Same id, different name → reload (the generated R_ constant changed).
        let new = idx(&[(1, "resources/welcome-aaaa.data")]);
        assert_eq!(dispatch_signals(&old, &new, false), (true, vec![]));
    }

    #[test]
    fn dispatch_code_only_change_reloads() {
        let same = idx(&[(1, "resources/greeting-aaaa.data")]);
        assert_eq!(dispatch_signals(&same, &same, true), (true, vec![]));
    }

    #[test]
    fn dispatch_nothing_changed() {
        let same = idx(&[(1, "resources/greeting-aaaa.data")]);
        assert_eq!(dispatch_signals(&same, &same, false), (false, vec![]));
    }

    #[test]
    fn reload_message_run_mode_has_no_path() {
        // Run-mode reload reloads in place — no checksum path (issue #119).
        assert_eq!(reload_message(7, None), r#"{"id":7,"cmd":"reload"}"#);
    }

    #[test]
    fn reload_message_debug_mode_carries_staged_path() {
        // Debug-mode reload carries the content-addressed ELF path so lldb
        // re-reads the new DWARF at a unique path (issue #119).
        assert_eq!(
            reload_message(3, Some("/tmp/proj/build/.dbg.00000000deadbeef.elf")),
            r#"{"id":3,"cmd":"reload","path":"/tmp/proj/build/.dbg.00000000deadbeef.elf"}"#
        );
        // Paths with quotes/backslashes are JSON-escaped.
        assert_eq!(
            reload_message(1, Some(r#"/x/"a"\b"#)),
            r#"{"id":1,"cmd":"reload","path":"/x/\"a\"\\b"}"#
        );
    }

    #[test]
    fn resolve_trace_prefers_flag_and_sanitises() {
        assert_eq!(resolve_trace(Some("api,frame")), "api,frame");
        // Characters outside the channel-list alphabet are stripped so the
        // value is safe to splice into the shell.html script template.
        assert_eq!(
            resolve_trace(Some("api\"</script><script>,frame")),
            "apiscriptscript,frame"
        );
        assert_eq!(resolve_trace(Some("")), "");
    }

    #[test]
    fn cl_codec_roundtrip() {
        let json = r#"{"seq":1,"type":"request","command":"initialize"}"#;
        let mut buf = Vec::new();
        write_cl(&mut buf, json).unwrap();
        assert!(
            std::str::from_utf8(&buf)
                .unwrap()
                .starts_with("Content-Length: ")
        );
        let mut reader = BufReader::new(Cursor::new(buf));
        assert_eq!(read_cl(&mut reader).as_deref(), Some(json));
    }

    #[test]
    fn cl_codec_multiple_messages() {
        let msgs = [r#"{"seq":1}"#, r#"{"seq":2,"longer":true}"#];
        let mut buf = Vec::new();
        for msg in &msgs {
            write_cl(&mut buf, msg).unwrap();
        }
        let mut reader = BufReader::new(Cursor::new(buf));
        for msg in &msgs {
            assert_eq!(read_cl(&mut reader).as_deref(), Some(*msg));
        }
    }

    #[test]
    fn relay_session_bidirectional() {
        // Set up the WebSocket pair (server side goes into relay, client side is the test).
        let ws_listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let ws_port = ws_listener.local_addr().unwrap().port();
        let ws_server_thread = std::thread::spawn(move || {
            let (stream, _) = ws_listener.accept().unwrap();
            tungstenite::accept(stream).unwrap()
        });
        let (mut ws_client, _) =
            tungstenite::connect(format!("ws://127.0.0.1:{ws_port}/")).unwrap();
        let ws_server = ws_server_thread.join().unwrap();

        // Set up the TCP pair (server side goes into relay, client side is the test).
        let tcp_listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let tcp_port = tcp_listener.local_addr().unwrap().port();
        let tcp_client = TcpStream::connect(format!("127.0.0.1:{tcp_port}")).unwrap();
        let (tcp_server, _) = tcp_listener.accept().unwrap();

        // Start the relay session in a background thread.
        std::thread::spawn(move || run_relay_session(ws_server, tcp_server));

        // TCP → WS: VS Code sends a Content-Length message; WASM receives a WebSocket frame.
        let msg_to_wasm = r#"{"seq":1,"type":"request","command":"initialize"}"#;
        let mut tcp_write = tcp_client.try_clone().unwrap();
        write_cl(&mut tcp_write, msg_to_wasm).unwrap();
        let frame = ws_client.read().unwrap();
        assert_eq!(frame.to_text().unwrap(), msg_to_wasm);

        // WS → TCP: WASM sends a WebSocket frame; VS Code receives a Content-Length message.
        let msg_to_vscode = r#"{"seq":1,"type":"response","command":"initialize","success":true}"#;
        ws_client.send(Message::Text(msg_to_vscode.into())).unwrap();
        let mut tcp_read = BufReader::new(tcp_client);
        let received = read_cl(&mut tcp_read).unwrap();
        assert_eq!(received, msg_to_vscode);
    }

    #[test]
    fn gdb_relay_session_bidirectional() {
        // GDB relay forwards raw bytes (no Content-Length framing) in both directions.
        let ws_listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let ws_port = ws_listener.local_addr().unwrap().port();
        let ws_server_thread = std::thread::spawn(move || {
            let (stream, _) = ws_listener.accept().unwrap();
            tungstenite::accept(stream).unwrap()
        });
        let (mut ws_client, _) =
            tungstenite::connect(format!("ws://127.0.0.1:{ws_port}/")).unwrap();
        let ws_server = ws_server_thread.join().unwrap();

        let tcp_listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let tcp_port = tcp_listener.local_addr().unwrap().port();
        let mut tcp_client = TcpStream::connect(format!("127.0.0.1:{tcp_port}")).unwrap();
        let (tcp_server, _) = tcp_listener.accept().unwrap();

        std::thread::spawn(move || run_gdb_relay_session(ws_server, tcp_server));

        // TCP → WS: lldb-dap sends a raw GDB RSP packet; WASM receives it verbatim.
        let rsp_from_lldb = "$qSupported:multiprocess+#df";
        let mut tcp_write = tcp_client.try_clone().unwrap();
        tcp_write.write_all(rsp_from_lldb.as_bytes()).unwrap();
        tcp_write.flush().unwrap();
        let frame = ws_client.read().unwrap();
        assert_eq!(frame.to_text().unwrap(), rsp_from_lldb);

        // WS → TCP: WASM sends a stop reply; lldb-dap receives it verbatim.
        let rsp_from_wasm = "$T05swbreak:;thread:01;#3f";
        ws_client.send(Message::Text(rsp_from_wasm.into())).unwrap();
        let mut buf = [0u8; 64];
        tcp_client
            .set_read_timeout(Some(std::time::Duration::from_secs(2)))
            .unwrap();
        let n = tcp_client.read(&mut buf).unwrap();
        assert_eq!(&buf[..n], rsp_from_wasm.as_bytes());
    }

    #[test]
    fn gdb_relay_tcp_connects_before_wasm() {
        // Verifies that when the TCP client (gdb) connects before the WASM page
        // loads, the relay buffers the TCP connection and forwards data once the
        // WS side eventually connects.
        let ws_listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let ws_port = ws_listener.local_addr().unwrap().port();
        let tcp_listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let tcp_port = tcp_listener.local_addr().unwrap().port();

        std::thread::spawn(move || {
            run_gdb_relay_loop(ws_listener, tcp_listener);
        });

        // Connect TCP first — relay accepts it in background but blocks on WS.
        let mut tcp_client = TcpStream::connect(format!("127.0.0.1:{tcp_port}")).unwrap();

        // Small delay to allow the relay's TCP accept thread to enqueue the stream.
        std::thread::sleep(std::time::Duration::from_millis(30));

        // Now connect the WS side — this unblocks the relay's ws_rx.recv().
        let (mut ws_client, _) =
            tungstenite::connect(format!("ws://127.0.0.1:{ws_port}/")).unwrap();

        // Give the session a moment to start.
        std::thread::sleep(std::time::Duration::from_millis(30));

        // Verify bidirectional forwarding works now that both sides are connected.
        let packet = "$qSupported#df";
        tcp_client.write_all(packet.as_bytes()).unwrap();
        tcp_client.flush().unwrap();
        let frame = ws_client.read().unwrap();
        assert_eq!(frame.to_text().unwrap(), packet);
    }

    #[test]
    fn dev_ctrl_hub_broadcast_and_bidirectional() {
        // The dev control hub is a full-mesh broadcast bus.  With one WS + one
        // TCP client it behaves like the old 1:1 relay (each side hears the
        // other); `hub.broadcast` additionally reaches every client at once.
        let (ws_port, tcp_port, hub) = start_dev_ctrl_relay(0, 0);

        let (mut ws_client, _) =
            tungstenite::connect(format!("ws://127.0.0.1:{ws_port}/")).unwrap();
        let tcp_client = TcpStream::connect(format!("127.0.0.1:{tcp_port}")).unwrap();
        let mut tcp_write = tcp_client.try_clone().unwrap();
        let mut tcp_read = BufReader::new(tcp_client);

        // Let both client handlers register with the hub before we route.
        std::thread::sleep(std::time::Duration::from_millis(150));

        // TCP → WS: external tool sends a command line; WASM receives a frame
        // with the trailing newline stripped.
        let cmd = r#"{"id":1,"cmd":"reset"}"#;
        tcp_write.write_all(format!("{cmd}\n").as_bytes()).unwrap();
        tcp_write.flush().unwrap();
        let frame = ws_client.read().unwrap();
        assert_eq!(frame.to_text().unwrap(), cmd);

        // WS → TCP: runtime sends a response frame; external tool reads it as a
        // newline-terminated JSON line.
        let resp = r#"{"id":1,"status":"ok","cmd":"reset"}"#;
        ws_client.send(Message::Text(resp.into())).unwrap();
        let mut line = String::new();
        tcp_read.read_line(&mut line).unwrap();
        assert_eq!(line.trim_end(), resp);

        // Devtool broadcast (the rebuild loop's reload): reaches every client.
        let reload = r#"{"id":1,"cmd":"reload"}"#;
        assert_eq!(hub.broadcast(None, reload), 2);
        let frame = ws_client.read().unwrap();
        assert_eq!(frame.to_text().unwrap(), reload);
        let mut line = String::new();
        tcp_read.read_line(&mut line).unwrap();
        assert_eq!(line.trim_end(), reload);
    }
}

/* -------------------------------------------------------------------------
 * WASM directory discovery
 *
 * Resolution order:
 *   1. $BLYT_WASM_DIR            — explicit override
 *   2. <sdk>/share/wasm/         — installed SDK layout (blyt_sdk.cmake Step 7)
 *   3. <repo>/build/sdk/share/wasm/ — dev layout (devtool run from the repo;
 *      the emcmake trees emit directly here via BLYT_WASM_OUT_DIR)
 * ------------------------------------------------------------------------- */

/// Locate the release WASM runtime dir (used by `blyt build wasm` packaging).
pub(crate) fn find_wasm_dir() -> Result<PathBuf, RunError> {
    find_wasm_dir_for(Mode::Release)
}

/// Locate the WASM runtime dir for the given build variant (ADR-0129):
///   release → $BLYT_WASM_DIR       → <sdk>/share/wasm       → build/sdk/share/wasm
///   debug   → $BLYT_WASM_DEBUG_DIR → <sdk>/share/wasm-debug → build/sdk/share/wasm-debug
fn find_wasm_dir_for(mode: Mode) -> Result<PathBuf, RunError> {
    let name = mode.wasm_name();
    let js = format!("{name}.js");
    let (env_var, share_sub, dev_sub) = match mode {
        Mode::Release => ("BLYT_WASM_DIR", "wasm", "build/sdk/share/wasm"),
        Mode::Debug => (
            "BLYT_WASM_DEBUG_DIR",
            "wasm-debug",
            "build/sdk/share/wasm-debug",
        ),
    };

    if let Ok(d) = std::env::var(env_var) {
        let p = PathBuf::from(&d);
        if p.join(&js).exists() {
            return Ok(p);
        }
        return Err(err(format!("{env_var}={d} does not contain {js}")));
    }

    // SDK layout: <sdk>/share/<share_sub>/<name>.js  (binary lives in <sdk>/bin/blyt)
    if let Some(sdk) = sdk_root_from_exe() {
        let p = sdk.join("share").join(share_sub);
        if p.join(&js).exists() {
            return Ok(p);
        }
    }

    // Dev layout: walk up from the binary looking for <dev_sub>/
    if let Ok(exe) = std::env::current_exe() {
        for ancestor in exe.ancestors().skip(1) {
            let candidate = ancestor.join(dev_sub);
            if candidate.join(&js).exists() {
                return Ok(candidate);
            }
        }
    }

    Err(err(format!(
        "cannot find {js} — build the SDK first \
         (cmake --build build --target sdk) or set {env_var} to the directory \
         containing {js}.",
    )))
}

/* -------------------------------------------------------------------------
 * GDB relay
 *
 * Bridges the WASM runtime's WebSocket GDB transport and a raw TCP port that
 * gdb-multiarch connects to.  GDB RSP packets are forwarded verbatim in both
 * directions without protocol conversion (RSP is ASCII text in both transports).
 *
 * ws_port  — WebSocket, for the WASM runtime (injected into shell.html)
 * tcp_port — raw TCP, for gdb-multiarch (`target remote 127.0.0.1:PORT`)
 * ------------------------------------------------------------------------- */

fn start_gdb_relay(ws_port: u16, tcp_port: u16) -> (u16, u16) {
    let ws_listener = bind_relay(ws_port, "GDB relay (WebSocket)");
    let tcp_listener = bind_relay(tcp_port, "GDB relay (TCP)");

    let actual_ws = ws_listener
        .local_addr()
        .map(|a| a.port())
        .unwrap_or(ws_port);
    let actual_tcp = tcp_listener
        .local_addr()
        .map(|a| a.port())
        .unwrap_or(tcp_port);

    std::thread::spawn(move || {
        run_gdb_relay_loop(ws_listener, tcp_listener);
    });

    (actual_ws, actual_tcp)
}

fn run_gdb_relay_loop(ws_listener: TcpListener, tcp_listener: TcpListener) {
    use std::sync::mpsc;

    loop {
        let (ws_tx, ws_rx) = mpsc::channel();
        let (tcp_tx, tcp_rx) = mpsc::channel::<TcpStream>();

        let ws_listener2 = ws_listener.try_clone().expect("clone gdb ws listener");
        std::thread::spawn(move || {
            if let Ok((stream, _)) = ws_listener2.accept() {
                if let Ok(ws) = tungstenite::accept(stream) {
                    let _ = ws_tx.send(ws);
                }
            }
        });

        let tcp_listener2 = tcp_listener.try_clone().expect("clone gdb tcp listener");
        std::thread::spawn(move || {
            if let Ok((stream, _)) = tcp_listener2.accept() {
                let _ = tcp_tx.send(stream);
            }
        });

        let ws = match ws_rx.recv() {
            Ok(ws) => ws,
            Err(_) => continue,
        };
        /* Signal to the VS Code extension that the WASM page has connected and
         * the relay is ready.  The extension awaits this before telling lldb-dap
         * to run `gdb-remote`, so LLDB never sees the relay in a half-open state
         * and the 6-second handshake timeout is eliminated. */
        println!("GDB: WASM ready");
        let _ = std::io::Write::flush(&mut std::io::stdout());
        let tcp = match tcp_rx.recv() {
            Ok(s) => s,
            Err(_) => continue,
        };

        run_gdb_relay_session(ws, tcp);
    }
}

fn run_gdb_relay_session(mut ws: WsConn, tcp: TcpStream) {
    use std::io::ErrorKind::WouldBlock;
    use std::sync::mpsc;

    let (tcp_to_ws_tx, tcp_to_ws_rx) = mpsc::channel::<String>();
    let (ws_to_tcp_tx, ws_to_tcp_rx) = mpsc::channel::<String>();

    let tcp_write = match tcp.try_clone() {
        Ok(s) => s,
        Err(_) => return,
    };
    let tcp_read = tcp;

    // Thread: raw TCP reader — sends each read chunk as a WebSocket frame.
    // GDB RSP is stop-wait so each read typically yields exactly one RSP token.
    std::thread::spawn(move || {
        let mut buf = [0u8; 4096];
        let mut r = tcp_read;
        loop {
            match r.read(&mut buf) {
                Ok(0) | Err(_) => break,
                Ok(n) => {
                    let s = String::from_utf8_lossy(&buf[..n]).into_owned();
                    if tcp_to_ws_tx.send(s).is_err() {
                        break;
                    }
                }
            }
        }
    });

    // Thread: owns the WebSocket; polls non-blocking to interleave reads and writes.
    std::thread::spawn(move || {
        ws.get_mut().set_nonblocking(true).ok();
        let delay = std::time::Duration::from_micros(100);
        'relay: loop {
            loop {
                match ws.read() {
                    Ok(Message::Text(s)) => {
                        if ws_to_tcp_tx.send(s.to_string()).is_err() {
                            break 'relay;
                        }
                    }
                    Ok(Message::Binary(b)) => {
                        if let Ok(s) = String::from_utf8(b.into()) {
                            if ws_to_tcp_tx.send(s).is_err() {
                                break 'relay;
                            }
                        }
                    }
                    Ok(Message::Close(_)) => break 'relay,
                    Err(tungstenite::Error::Io(e)) if e.kind() == WouldBlock => break,
                    Err(_) => break 'relay,
                    _ => break,
                }
            }
            loop {
                match tcp_to_ws_rx.try_recv() {
                    Ok(msg) => {
                        if ws.send(Message::Text(msg.into())).is_err() {
                            break 'relay;
                        }
                    }
                    Err(mpsc::TryRecvError::Empty) => break,
                    Err(mpsc::TryRecvError::Disconnected) => break 'relay,
                }
            }
            // Block until the TCP reader delivers the next message or 1ms elapses.
            // This wakes immediately when LLDB data arrives instead of sleeping a full ms.
            match tcp_to_ws_rx.recv_timeout(delay) {
                Ok(msg) => {
                    if ws.send(Message::Text(msg.into())).is_err() {
                        break 'relay;
                    }
                }
                Err(mpsc::RecvTimeoutError::Timeout) => {}
                Err(mpsc::RecvTimeoutError::Disconnected) => break 'relay,
            }
        }
    });

    // Forward WebSocket messages (from WASM) to the TCP client (gdb-multiarch).
    let mut tcp_write = tcp_write;
    for msg in ws_to_tcp_rx {
        if tcp_write.write_all(msg.as_bytes()).is_err() {
            break;
        }
        let _ = tcp_write.flush();
    }
}

/* -------------------------------------------------------------------------
 * Dev control hub (issue #87 + #88, amends ADR-0045)
 *
 * Carries runtime lifecycle commands (reload, save_state, load_state, reset)
 * between external tools and the running runtime(s).  Unlike DAP/GDB this is
 * always started in project-dir mode — the release player must support hot
 * reload too, so it is not gated on debug.
 *
 * Two listeners:
 *   ws_port  — WebSocket, for the WASM runtime (injected as {{BLYT_DEV_CTRL_PORT}})
 *   tcp_port — raw newline-delimited JSON, for external tools (blytplay, VS Code)
 *
 * The protocol is single-line JSON terminated by '\n' (ADR-0045 amendment):
 * each WebSocket frame carries one whole line; the TCP side is newline-framed.
 *
 * Topology is a full-mesh broadcast bus (issue #88): a message from any client
 * — or from the devtool's rebuild loop via `broadcast` — is fanned out to every
 * *other* client.  With one WS + one TCP client this is identical to the old
 * 1:1 relay (#87); with many clients (blytplay + VS Code + the WASM page) the
 * file watcher can signal `reload` to all of them at once.  JSON `id` tags let
 * clients ignore lines that are not theirs.
 * ------------------------------------------------------------------------- */

/// One connected dev-control client (a TCP connection or a WASM-page WebSocket).
/// Its writer thread drains the receiving end of `tx`; each delivered string is
/// one message body (the transport adds its own framing).
struct DevCtrlClient {
    id: u64,
    tx: mpsc::Sender<String>,
}

/// Broadcast hub for the dev control channel.  Cheaply cloneable (shared state
/// behind an `Arc`) so accept loops and the rebuild loop can all hold a handle.
#[derive(Clone)]
struct DevCtrlHub {
    clients: Arc<Mutex<Vec<DevCtrlClient>>>,
    next_id: Arc<AtomicU64>,
}

impl DevCtrlHub {
    fn new() -> Self {
        DevCtrlHub {
            clients: Arc::new(Mutex::new(Vec::new())),
            next_id: Arc::new(AtomicU64::new(0)),
        }
    }

    /// Register a new client; returns its id and the receiver its writer thread
    /// must drain.  Dropping every clone of the returned receiver (i.e. the
    /// client going away) lets the next `broadcast` prune it.
    fn register(&self) -> (u64, mpsc::Receiver<String>) {
        let id = self.next_id.fetch_add(1, Ordering::Relaxed);
        let (tx, rx) = mpsc::channel();
        self.clients.lock().unwrap().push(DevCtrlClient { id, tx });
        (id, rx)
    }

    fn unregister(&self, id: u64) {
        self.clients.lock().unwrap().retain(|c| c.id != id);
    }

    /// Send `msg` to every client except `except` (the originator).  Clients
    /// whose writer channel has hung up are pruned.  Returns the number reached.
    fn broadcast(&self, except: Option<u64>, msg: &str) -> usize {
        let mut clients = self.clients.lock().unwrap();
        let mut reached = 0;
        clients.retain(|c| {
            if Some(c.id) == except {
                return true;
            }
            if c.tx.send(msg.to_string()).is_ok() {
                reached += 1;
                true
            } else {
                false // writer thread gone — prune
            }
        });
        reached
    }
}

fn start_dev_ctrl_relay(ws_port: u16, tcp_port: u16) -> (u16, u16, DevCtrlHub) {
    let ws_listener = bind_relay(ws_port, "dev control relay (WebSocket)");
    let tcp_listener = bind_relay(tcp_port, "dev control relay (TCP)");

    let actual_ws = ws_listener
        .local_addr()
        .map(|a| a.port())
        .unwrap_or(ws_port);
    let actual_tcp = tcp_listener
        .local_addr()
        .map(|a| a.port())
        .unwrap_or(tcp_port);

    let hub = DevCtrlHub::new();

    // WebSocket accept loop: one client per WASM page.
    let ws_hub = hub.clone();
    std::thread::spawn(move || {
        for stream in ws_listener.incoming() {
            let Ok(stream) = stream else { continue };
            let Ok(ws) = tungstenite::accept(stream) else {
                continue;
            };
            let hub = ws_hub.clone();
            std::thread::spawn(move || run_dev_ctrl_ws_client(ws, hub));
        }
    });

    // TCP accept loop: any number of external-tool clients.
    let tcp_hub = hub.clone();
    std::thread::spawn(move || {
        for stream in tcp_listener.incoming() {
            let Ok(stream) = stream else { continue };
            let hub = tcp_hub.clone();
            std::thread::spawn(move || run_dev_ctrl_tcp_client(stream, hub));
        }
    });

    (actual_ws, actual_tcp, hub)
}

/// Service one TCP client: newline-delimited JSON in both directions.
fn run_dev_ctrl_tcp_client(tcp: TcpStream, hub: DevCtrlHub) {
    let (id, rx) = hub.register();

    let write_half = match tcp.try_clone() {
        Ok(s) => s,
        Err(_) => {
            hub.unregister(id);
            return;
        }
    };

    // Writer thread: drain this client's queue → newline-framed lines.
    std::thread::spawn(move || {
        let mut w = write_half;
        for msg in rx {
            let line = format!("{msg}\n");
            if w.write_all(line.as_bytes()).is_err() {
                break;
            }
            let _ = w.flush();
        }
    });

    // Reader (this thread): each line is broadcast to every other client.
    let mut r = BufReader::new(tcp);
    loop {
        let mut line = String::new();
        match r.read_line(&mut line) {
            Ok(0) | Err(_) => break,
            Ok(_) => {
                let trimmed = line.trim_end_matches(['\r', '\n']);
                if trimmed.is_empty() {
                    continue; // ignore blank keepalive lines
                }
                hub.broadcast(Some(id), trimmed);
            }
        }
    }
    hub.unregister(id);
}

/// Service one WASM-page WebSocket client.  A single thread owns the socket and
/// polls non-blocking so it can interleave inbound frames (→ broadcast) with
/// outbound messages drained from this client's hub queue.
fn run_dev_ctrl_ws_client(mut ws: WsConn, hub: DevCtrlHub) {
    use std::io::ErrorKind::WouldBlock;

    let (id, rx) = hub.register();
    ws.get_mut().set_nonblocking(true).ok();
    let delay = std::time::Duration::from_micros(100);

    loop {
        // Drain inbound frames → broadcast to the other clients.
        loop {
            match ws.read() {
                Ok(Message::Text(s)) => {
                    hub.broadcast(Some(id), &s);
                }
                Ok(Message::Binary(b)) => {
                    if let Ok(s) = String::from_utf8(b.into()) {
                        hub.broadcast(Some(id), &s);
                    }
                }
                Ok(Message::Close(_)) => {
                    hub.unregister(id);
                    return;
                }
                Err(tungstenite::Error::Io(e)) if e.kind() == WouldBlock => break,
                Err(_) => {
                    hub.unregister(id);
                    return;
                }
                _ => break,
            }
        }
        // Forward queued messages (from other clients / the rebuild loop).
        match rx.recv_timeout(delay) {
            Ok(msg) => {
                if ws.send(Message::Text(msg.into())).is_err() {
                    hub.unregister(id);
                    return;
                }
                loop {
                    match rx.try_recv() {
                        Ok(msg) => {
                            if ws.send(Message::Text(msg.into())).is_err() {
                                hub.unregister(id);
                                return;
                            }
                        }
                        Err(mpsc::TryRecvError::Empty) => break,
                        Err(mpsc::TryRecvError::Disconnected) => {
                            hub.unregister(id);
                            return;
                        }
                    }
                }
            }
            Err(mpsc::RecvTimeoutError::Timeout) => {}
            Err(mpsc::RecvTimeoutError::Disconnected) => {
                hub.unregister(id);
                return;
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * File watcher + incremental rebuild loop (issue #88)
 *
 * In project-dir mode, watch the cart's sources and rebuild the dev ELF in
 * place on any change, then broadcast `reload` to every connected dev control
 * client.  Watching is implicit in `blyt run`/`blyt debug ./dir` — there is no
 * standalone `blyt watch` command (ADR-0044 amended).
 *
 * Scope: src/ (recursive) + blyt.build.yaml + blyt.config.yaml + blyt.info.yaml.
 * build/ is never watched, so build outputs cannot re-trigger the watcher (a
 * defensive path filter drops any stray build/ events as well).
 *
 * Debounce: block for the first event, then drain a short settle window so a
 * save-storm (e.g. an editor writing several files at once) coalesces into a
 * single rebuild and a single reload signal.
 * ------------------------------------------------------------------------- */

/// Content hash of the built dev ELF, or `None` if it cannot be read.  Used to
/// suppress reloads when a rebuild leaves the cart bytes unchanged.
fn elf_hash(path: &Path) -> Option<u64> {
    fs::read(path).ok().map(|b| xxhash_rust::xxh3::xxh3_64(&b))
}

/// Build the dev-control `reload` command (issue #88/#119).  In debug mode a
/// `staged_path` (a content-addressed copy of the rebuilt debug ELF) is included
/// so a live lldb session re-reads the new DWARF at a unique path; run mode
/// reloads in place and omits it.
fn reload_message(msg_id: u64, staged_path: Option<&str>) -> String {
    match staged_path {
        Some(p) => format!(
            "{{\"id\":{msg_id},\"cmd\":\"reload\",\"path\":\"{}\"}}",
            json_escape(p)
        ),
        None => format!("{{\"id\":{msg_id},\"cmd\":\"reload\"}}"),
    }
}

/// Parse `build/resource-id-index` into `(id, rel_path)` pairs (issue #91).
/// Returns an empty vec when the index is absent (no assets).
fn read_resource_index(build_dir: &Path) -> Vec<(u32, String)> {
    let Ok(content) = fs::read_to_string(build_dir.join("resource-id-index")) else {
        return Vec::new();
    };
    let mut v: Vec<(u32, String)> = content
        .lines()
        .filter_map(|line| {
            let (id_s, rel) = line.split_once(' ')?;
            Some((id_s.parse::<u32>().ok()?, rel.trim().to_string()))
        })
        .collect();
    v.sort();
    v
}

/// Resource name embedded in a staging path `resources/<name>-<fp>.data`.
fn resource_name_of(rel: &str) -> &str {
    let stem = rel.strip_prefix("resources/").unwrap_or(rel);
    let stem = stem.strip_suffix(".data").unwrap_or(stem);
    match stem.rfind('-') {
        Some(i) => &stem[..i],
        None => stem,
    }
}

/// Decide what dev-control signal(s) a rebuild requires by diffing the old/new
/// resource-id-index against whether the cart code changed (ADR-0088, issue #91).
///
/// Returns `(reload, update_asset_ids)`:
///   - resource IDs/names changed (asset add/remove/rename) → full reload;
///   - else code changed + assets changed → update_assets then reload;
///   - else code changed → reload;
///   - else assets changed → update_assets only (hot-swap).
fn dispatch_signals(
    old_index: &[(u32, String)],
    new_index: &[(u32, String)],
    code_changed: bool,
) -> (bool, Vec<u32>) {
    let pairs = |idx: &[(u32, String)]| -> Vec<(u32, String)> {
        idx.iter()
            .map(|(id, rel)| (*id, resource_name_of(rel).to_string()))
            .collect()
    };
    let resource_ids_changed = pairs(old_index) != pairs(new_index);

    if resource_ids_changed {
        return (true, Vec::new());
    }

    // Same id/name set: an asset changed iff its content-addressed path moved.
    let mut assets_changed: Vec<u32> = Vec::new();
    for (id, rel) in new_index {
        if let Some((_, old_rel)) = old_index.iter().find(|(oid, _)| oid == id) {
            if old_rel != rel {
                assets_changed.push(*id);
            }
        }
    }

    match (code_changed, assets_changed.is_empty()) {
        (true, _) => (true, assets_changed), // reload; update_assets first if any
        (false, false) => (false, assets_changed), // hot-swap only
        (false, true) => (false, Vec::new()), // nothing changed
    }
}

/// Content-addressed staging files (`.data` + `.meta`) referenced by `index`,
/// absolute under `build_dir`.
fn staged_files(build_dir: &Path, index: &[(u32, String)]) -> Vec<PathBuf> {
    let mut v = Vec::new();
    for (_, rel) in index {
        v.push(build_dir.join(rel));
        v.push(build_dir.join(rel).with_extension("meta"));
    }
    v
}

fn start_watcher(project_dir: PathBuf, debug: bool, hub: DevCtrlHub) {
    std::thread::spawn(move || {
        let (tx, rx) = mpsc::channel::<PathBuf>();

        let build_dir = project_dir.join("build");
        let handler = {
            let tx = tx.clone();
            let build_dir = build_dir.clone();
            move |res: notify::Result<notify::Event>| {
                let Ok(event) = res else { return };
                for p in &event.paths {
                    if p.starts_with(&build_dir) {
                        continue; // build output must not re-trigger
                    }
                    let _ = tx.send(p.clone());
                    break;
                }
            }
        };

        let mut watcher = match notify::recommended_watcher(handler) {
            Ok(w) => w,
            Err(e) => {
                eprintln!("[watch] could not start file watcher: {e}");
                return;
            }
        };

        let src = project_dir.join("src");
        if src.is_dir() {
            if let Err(e) = watcher.watch(&src, RecursiveMode::Recursive) {
                eprintln!("[watch] cannot watch {}: {e}", src.display());
            }
        }
        // Watch assets/ so edits hot-swap via update_assets (issue #91, amends #88).
        let assets = project_dir.join("assets");
        if assets.is_dir() {
            if let Err(e) = watcher.watch(&assets, RecursiveMode::Recursive) {
                eprintln!("[watch] cannot watch {}: {e}", assets.display());
            }
        }
        for manifest in ["blyt.build.yaml", "blyt.config.yaml", "blyt.info.yaml"] {
            let p = project_dir.join(manifest);
            if p.exists() {
                if let Err(e) = watcher.watch(&p, RecursiveMode::NonRecursive) {
                    eprintln!("[watch] cannot watch {}: {e}", p.display());
                }
            }
        }

        // Track the built ELF's content so a reload is signalled only when the
        // cart bytes actually change.  This keeps a burst of writes (or a
        // no-op rebuild driven by events that arrive while a build is running)
        // to a single reload, and suppresses reloads for edits that don't alter
        // the compiled cart (e.g. a comment-only change).  Seeded from the
        // initial build that already produced the dev ELF before serving.
        let dev_elf = project_dir
            .join("build")
            .join(if debug { ".dbg.elf" } else { ".elf" });
        let mut last_hash = elf_hash(&dev_elf);
        let mut last_index = read_resource_index(&build_dir);
        // Superseded staging files awaiting GC: deleted one rebuild later, by
        // which point the runtime has acked the swap and released any pinned
        // bytes from the in-flight frame (ADR-0088 GC, issue #91).
        let mut pending_gc: Vec<PathBuf> = Vec::new();
        // Previous content-addressed debug ELF awaiting GC (issue #119): a debug
        // reload stages a `.dbg.<hash>.elf` copy so lldb re-reads the new DWARF
        // at a unique path; the previous one is dropped when the next is staged,
        // by which point lldb has cached the rebuilt module.
        let mut prev_dbg_staged: Option<PathBuf> = None;

        let settle = std::time::Duration::from_millis(150);
        let mut msg_id: u64 = 1;
        while let Ok(path) = rx.recv() {
            let rel = path.strip_prefix(&project_dir).unwrap_or(&path);
            println!("[watch] {} changed", rel.display());
            // Coalesce the burst: keep draining until the watcher goes quiet.
            while rx.recv_timeout(settle).is_ok() {}

            println!("[build] rebuilding…");
            match build_for_dev(&project_dir, debug, false) {
                Ok(elf) => {
                    let new_hash = elf_hash(&elf);
                    let code_changed = new_hash.is_some() && new_hash != last_hash;
                    if code_changed {
                        last_hash = new_hash;
                    }
                    let new_index = read_resource_index(&build_dir);
                    let (reload, update_ids) =
                        dispatch_signals(&last_index, &new_index, code_changed);

                    // update_assets first (table updated before any VM restart).
                    if !update_ids.is_empty() {
                        let ids = update_ids
                            .iter()
                            .map(u32::to_string)
                            .collect::<Vec<_>>()
                            .join(",");
                        let msg = format!(
                            "{{\"id\":{msg_id},\"cmd\":\"update_assets\",\"assets\":[{ids}]}}"
                        );
                        let n = hub.broadcast(None, &msg);
                        println!("[assets] update_assets {update_ids:?} → {n} runtime(s)");
                        msg_id += 1;
                    }
                    if reload {
                        // Debug mode (issue #119): stage a content-addressed copy
                        // of the rebuilt ELF and pass its path so a live lldb
                        // session re-reads the new DWARF at a unique path and
                        // rebinds breakpoints.  Run mode reloads in place (no path).
                        let staged_path: Option<String> = if debug {
                            let hash = new_hash.or(last_hash).unwrap_or(0);
                            let staged = build_dir.join(format!(".dbg.{hash:016x}.elf"));
                            match fs::copy(&elf, &staged) {
                                Ok(_) => {
                                    if let Some(old) = prev_dbg_staged.replace(staged.clone()) {
                                        let _ = fs::remove_file(old);
                                    }
                                    let abs = staged.canonicalize().unwrap_or(staged);
                                    Some(abs.to_string_lossy().into_owned())
                                }
                                Err(e) => {
                                    eprintln!("[reload] could not stage debug ELF: {e}");
                                    None
                                }
                            }
                        } else {
                            None
                        };
                        let msg = reload_message(msg_id, staged_path.as_deref());
                        let n = hub.broadcast(None, &msg);
                        println!("[reload] signalled {n} runtime(s)");
                        msg_id += 1;
                    }
                    if !reload && update_ids.is_empty() {
                        println!("[reload] skipped (cart unchanged)");
                    }

                    // GC: drop last round's superseded files now that the runtime
                    // has had a full cycle to consume the swap, then stage this
                    // round's superseded files for the next round.
                    for f in pending_gc.drain(..) {
                        let _ = fs::remove_file(&f);
                    }
                    let new_set: std::collections::HashSet<&String> =
                        new_index.iter().map(|(_, rel)| rel).collect();
                    let superseded: Vec<(u32, String)> = last_index
                        .iter()
                        .filter(|(_, rel)| !new_set.contains(rel))
                        .cloned()
                        .collect();
                    pending_gc = staged_files(&build_dir, &superseded);
                    last_index = new_index;
                }
                Err(e) => {
                    eprintln!("[build] ✗ {e}");
                    println!("[reload] skipped (build failed — cart still running)");
                }
            }
        }
    });
}

/* -------------------------------------------------------------------------
 * HTTP server
 * ------------------------------------------------------------------------- */

#[allow(clippy::too_many_arguments)]
fn serve(
    listener: TcpListener,
    wasm_dir: Arc<PathBuf>,
    cart_path: Arc<PathBuf>,
    wasm_name: Arc<String>,
    trace: Arc<String>,
    dap_port: u16,
    gdb_port: u16,
    dev_ctrl_port: u16,
) -> Result<(), RunError> {
    for stream in listener.incoming() {
        let stream = match stream {
            Ok(s) => s,
            Err(_) => continue,
        };
        let wasm_dir = Arc::clone(&wasm_dir);
        let cart_path = Arc::clone(&cart_path);
        let wasm_name = Arc::clone(&wasm_name);
        let trace = Arc::clone(&trace);
        std::thread::spawn(move || {
            handle_connection(
                stream,
                &wasm_dir,
                &cart_path,
                &wasm_name,
                &trace,
                dap_port,
                gdb_port,
                dev_ctrl_port,
            );
        });
    }
    Ok(())
}

#[allow(clippy::too_many_arguments)]
fn handle_connection(
    mut stream: TcpStream,
    wasm_dir: &Path,
    cart_path: &Path,
    wasm_name: &str,
    trace: &str,
    dap_port: u16,
    gdb_port: u16,
    dev_ctrl_port: u16,
) {
    let mut buf = [0u8; 4096];
    let n = match stream.read(&mut buf) {
        Ok(0) | Err(_) => return,
        Ok(n) => n,
    };
    let request = std::str::from_utf8(&buf[..n]).unwrap_or("");
    let path = request_path(request);

    // Resource serving for WASM dev mode (issue #91/#118).  The browser mirrors
    // the native staging layout into MEMFS: it fetches the resource-id-index, then
    // each content-addressed file the index references, and points the host at the
    // materialised dir via BLYT_RESOURCE_DIR.  The staging dir is the dev ELF's
    // directory (<project>/build).
    //
    // Two load-path routes (issue #118):
    //   GET /resource-id-index             — the index, verbatim.
    //   GET /resources/<name>-<fp>.data    — a content-addressed staging file.
    // The file is addressed by content hash, so it is immutable: the browser
    // always reads the exact version the index it parsed enumerated, with no
    // version race.  (GET /resource/<id> below stays for curl/debug only — it
    // re-resolves id -> *current* version per request, which is exactly the race
    // the content-addressed path avoids, so it must not be the load path.)
    if path == "/resource-id-index" {
        let build_dir = cart_path.parent().unwrap_or(Path::new("."));
        match fs::read(build_dir.join("resource-id-index")) {
            Ok(bytes) => respond(&mut stream, 200, "text/plain", &bytes),
            Err(_) => respond(&mut stream, 404, "text/plain", b"no resource index"),
        }
        return;
    }
    if let Some(rel) = path.strip_prefix("/resources/") {
        let build_dir = cart_path.parent().unwrap_or(Path::new("."));
        match is_safe_resource_rel(rel)
            .then(|| fs::read(build_dir.join("resources").join(rel)).ok())
            .flatten()
        {
            Some(bytes) => respond(&mut stream, 200, "application/octet-stream", &bytes),
            None => respond(&mut stream, 404, "text/plain", b"resource not found"),
        }
        return;
    }
    if let Some(id_str) = path.strip_prefix("/resource/") {
        let build_dir = cart_path.parent().unwrap_or(Path::new("."));
        match id_str
            .parse::<u32>()
            .ok()
            .and_then(|id| resolve_resource_file(build_dir, id))
            .and_then(|p| fs::read(&p).ok())
        {
            Some(bytes) => respond(&mut stream, 200, "application/octet-stream", &bytes),
            None => respond(&mut stream, 404, "text/plain", b"resource not found"),
        }
        return;
    }

    // The emscripten .html references <name>.js, which fetches <name>.wasm, so
    // the served paths are variant-specific (blytplay.* vs blytdebug.*).
    let js_path = format!("/{wasm_name}.js");
    let wasm_path = format!("/{wasm_name}.wasm");

    let (file_path, content_type, inject_dap): (PathBuf, &str, bool) = match path {
        "/" | "/index.html" => (
            wasm_dir.join(format!("{wasm_name}.html")),
            "text/html; charset=utf-8",
            true,
        ),
        p if p == js_path => (
            wasm_dir.join(format!("{wasm_name}.js")),
            "application/javascript",
            false,
        ),
        p if p == wasm_path => (
            wasm_dir.join(format!("{wasm_name}.wasm")),
            "application/wasm",
            false,
        ),
        "/cart.blyt" => (cart_path.to_path_buf(), "application/octet-stream", false),
        _ => {
            respond(&mut stream, 404, "text/plain", b"Not Found");
            return;
        }
    };

    match fs::read(&file_path) {
        Ok(data) => {
            if inject_dap {
                // Replace {{BLYT_DAP_PORT}}/{{BLYT_GDB_PORT}} with the relay
                // ports and {{BLYT_TRACE}} with the trace channel list ("" off).
                let html = String::from_utf8_lossy(&data);
                // Absolute, JS-string-escaped host path of the cart ELF so
                // lldb-dap can open it locally for DWARF (issue #144).
                let cart_abs =
                    fs::canonicalize(cart_path).unwrap_or_else(|_| cart_path.to_path_buf());
                let cart_js = cart_abs
                    .to_string_lossy()
                    .replace('\\', "\\\\")
                    .replace('"', "\\\"");
                let patched = html
                    .replace("{{BLYT_DAP_PORT}}", &dap_port.to_string())
                    .replace("{{BLYT_GDB_PORT}}", &gdb_port.to_string())
                    .replace("{{BLYT_DEV_CTRL_PORT}}", &dev_ctrl_port.to_string())
                    .replace("{{BLYT_CART_PATH}}", &cart_js)
                    .replace("{{BLYT_TRACE}}", trace);
                respond(&mut stream, 200, content_type, patched.as_bytes());
            } else {
                respond(&mut stream, 200, content_type, &data);
            }
        }
        Err(_) => respond(&mut stream, 404, "text/plain", b"File Not Found"),
    }
}

/// True if `rel` (the part after `/resources/`) is safe to read from
/// `<build_dir>/resources/` (issue #118): non-empty, no empty components, and no
/// `.`/`..` traversal.  The browser only ever requests content-addressed
/// filenames the resource-id-index enumerated; this guards the route from being
/// abused to read elsewhere under build_dir.
fn is_safe_resource_rel(rel: &str) -> bool {
    !rel.is_empty()
        && rel
            .split('/')
            .all(|c| !c.is_empty() && c != ".." && c != ".")
}

/// Resolve resource id → absolute content-addressed staging file via the
/// resource-id-index under `build_dir` (issue #91).
fn resolve_resource_file(build_dir: &Path, id: u32) -> Option<PathBuf> {
    read_resource_index(build_dir)
        .into_iter()
        .find(|(rid, _)| *rid == id)
        .map(|(_, rel)| build_dir.join(rel))
}

fn request_path(request: &str) -> &str {
    // "GET /path HTTP/1.1" → "/path"  (strip query string)
    let path = request
        .lines()
        .next()
        .and_then(|line| line.split(' ').nth(1))
        .unwrap_or("/");
    path.split('?').next().unwrap_or("/")
}

fn respond(stream: &mut TcpStream, status: u16, content_type: &str, body: &[u8]) {
    let status_text = match status {
        200 => "OK",
        404 => "Not Found",
        _ => "Error",
    };
    let header = format!(
        "HTTP/1.1 {status} {status_text}\r\n\
         Content-Type: {content_type}\r\n\
         Content-Length: {}\r\n\
         Cache-Control: no-store\r\n\
         Connection: close\r\n\
         \r\n",
        body.len()
    );
    let _ = stream.write_all(header.as_bytes());
    let _ = stream.write_all(body);
}
