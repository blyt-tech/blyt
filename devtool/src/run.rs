use std::fs;
use std::io::{BufRead, BufReader, Read, Write};
use std::net::{TcpListener, TcpStream};
use std::path::{Path, PathBuf};
use std::sync::Arc;

use tungstenite::Message;

use crate::build::sdk_root_from_exe;

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
 * Public entry point
 * ------------------------------------------------------------------------- */

pub fn run(cart_path: &Path, debug: bool) -> Result<(), RunError> {
    let cart_path = cart_path
        .canonicalize()
        .map_err(|e| err(format!("cannot open cart '{}': {}", cart_path.display(), e)))?;

    let wasm_dir = find_wasm_dir()?;

    // Validate that required WASM files are present.
    for name in &["blytrun.js", "blytrun.wasm", "blytrun.html"] {
        if !wasm_dir.join(name).exists() {
            return Err(err(format!(
                "WASM runtime file missing: {}/{}\n\
                 Rebuild with:\n\
                 \x20 emcmake cmake -B build-wasm -S frontends/wasm\n\
                 \x20 cmake --build build-wasm",
                wasm_dir.display(),
                name
            )));
        }
    }

    // Bind on an OS-assigned port so there are no port collisions.
    let listener = TcpListener::bind("127.0.0.1:0")?;
    let port = listener.local_addr()?.port();

    // DAP relay: WebSocket on PORT+1 (WASM runtime), raw TCP on PORT+2 (VS Code).
    // Only started when --debug is passed.
    let (dap_ws_port, dap_tcp_port) = if debug {
        start_dap_relay(port.wrapping_add(1), port.wrapping_add(2))
    } else {
        (0, 0)
    };

    // GDB relay: WebSocket on PORT+3 (WASM runtime), raw TCP on PORT+4 (gdb-multiarch).
    // Only started when --debug is passed.
    let (gdb_ws_port, gdb_tcp_port) = if debug {
        start_gdb_relay(port.wrapping_add(3), port.wrapping_add(4))
    } else {
        (0, 0)
    };

    println!("blyt run: serving on http://127.0.0.1:{port}/");
    println!("  Cart:     {}", cart_path.display());
    println!("  WASM dir: {}", wasm_dir.display());
    println!();
    println!("Open http://127.0.0.1:{port}/ in your browser.");
    println!("Press Ctrl+C to stop.");
    if debug {
        println!();
        println!("  DAP debugger (Lua):    127.0.0.1:{dap_tcp_port}");
        println!("  GDB debugger:          127.0.0.1:{gdb_tcp_port}");
        println!("  (gdb-multiarch: set arch riscv:rv32 && target remote :{gdb_tcp_port})");
    }

    serve(
        listener,
        Arc::new(wasm_dir),
        Arc::new(cart_path),
        dap_ws_port,
        gdb_ws_port,
    )
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

fn start_dap_relay(ws_port: u16, tcp_port: u16) -> (u16, u16) {
    let ws_listener = TcpListener::bind(format!("127.0.0.1:{ws_port}"))
        .expect("DAP relay: WebSocket bind failed");
    let tcp_listener =
        TcpListener::bind(format!("127.0.0.1:{tcp_port}")).expect("DAP relay: TCP bind failed");

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
        let delay = std::time::Duration::from_millis(1);
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
            std::thread::sleep(delay);
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
}

/* -------------------------------------------------------------------------
 * WASM directory discovery
 *
 * Resolution order:
 *   1. $BLYT_WASM_DIR            — explicit override
 *   2. <sdk>/share/wasm/         — SDK layout (blyt_sdk.cmake Step 7)
 *   3. <repo>/build-wasm/        — manual emcmake cmake invocation
 * ------------------------------------------------------------------------- */

pub(crate) fn find_wasm_dir() -> Result<PathBuf, RunError> {
    if let Ok(d) = std::env::var("BLYT_WASM_DIR") {
        let p = PathBuf::from(&d);
        if p.join("blytrun.js").exists() {
            return Ok(p);
        }
        return Err(err(format!(
            "BLYT_WASM_DIR={d} does not contain blytrun.js"
        )));
    }

    // SDK layout: <sdk>/share/wasm/blytrun.js  (binary lives in <sdk>/bin/blyt)
    if let Some(sdk) = sdk_root_from_exe() {
        let p = sdk.join("share").join("wasm");
        if p.join("blytrun.js").exists() {
            return Ok(p);
        }
    }

    // Dev layout: walk up from the binary looking for build-wasm/
    if let Ok(exe) = std::env::current_exe() {
        for ancestor in exe.ancestors().skip(1) {
            let candidate = ancestor.join("build-wasm");
            if candidate.join("blytrun.js").exists() {
                return Ok(candidate);
            }
        }
    }

    Err(err(
        "cannot find blytrun.js — build the WASM runtime first:\n\
         \x20 emcmake cmake -B build-wasm -S frontends/wasm\n\
         \x20 cmake --build build-wasm\n\
         or set BLYT_WASM_DIR to the directory containing blytrun.js.",
    ))
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
    let ws_listener = TcpListener::bind(format!("127.0.0.1:{ws_port}"))
        .expect("GDB relay: WebSocket bind failed");
    let tcp_listener =
        TcpListener::bind(format!("127.0.0.1:{tcp_port}")).expect("GDB relay: TCP bind failed");

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
        let delay = std::time::Duration::from_millis(1);
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
            std::thread::sleep(delay);
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
 * HTTP server
 * ------------------------------------------------------------------------- */

fn serve(
    listener: TcpListener,
    wasm_dir: Arc<PathBuf>,
    cart_path: Arc<PathBuf>,
    dap_port: u16,
    gdb_port: u16,
) -> Result<(), RunError> {
    for stream in listener.incoming() {
        let stream = match stream {
            Ok(s) => s,
            Err(_) => continue,
        };
        let wasm_dir = Arc::clone(&wasm_dir);
        let cart_path = Arc::clone(&cart_path);
        std::thread::spawn(move || {
            handle_connection(stream, &wasm_dir, &cart_path, dap_port, gdb_port);
        });
    }
    Ok(())
}

fn handle_connection(
    mut stream: TcpStream,
    wasm_dir: &Path,
    cart_path: &Path,
    dap_port: u16,
    gdb_port: u16,
) {
    let mut buf = [0u8; 4096];
    let n = match stream.read(&mut buf) {
        Ok(0) | Err(_) => return,
        Ok(n) => n,
    };
    let request = std::str::from_utf8(&buf[..n]).unwrap_or("");
    let path = request_path(request);

    let (file_path, content_type, inject_dap): (PathBuf, &str, bool) = match path {
        "/" | "/index.html" => (
            wasm_dir.join("blytrun.html"),
            "text/html; charset=utf-8",
            true,
        ),
        "/blytrun.js" => (wasm_dir.join("blytrun.js"), "application/javascript", false),
        "/blytrun.wasm" => (wasm_dir.join("blytrun.wasm"), "application/wasm", false),
        "/cart.blyt" => (cart_path.to_path_buf(), "application/octet-stream", false),
        _ => {
            respond(&mut stream, 404, "text/plain", b"Not Found");
            return;
        }
    };

    match fs::read(&file_path) {
        Ok(data) => {
            if inject_dap {
                // Replace {{BLYT_DAP_PORT}} and {{BLYT_GDB_PORT}} with actual relay ports.
                let html = String::from_utf8_lossy(&data);
                let patched = html
                    .replace("{{BLYT_DAP_PORT}}", &dap_port.to_string())
                    .replace("{{BLYT_GDB_PORT}}", &gdb_port.to_string());
                respond(&mut stream, 200, content_type, patched.as_bytes());
            } else {
                respond(&mut stream, 200, content_type, &data);
            }
        }
        Err(_) => respond(&mut stream, 404, "text/plain", b"File Not Found"),
    }
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
