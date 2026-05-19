use std::fs;
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::path::{Path, PathBuf};
use std::sync::Arc;

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

pub fn run(cart_path: &Path) -> Result<(), RunError> {
    let cart_path = cart_path
        .canonicalize()
        .map_err(|e| err(format!("cannot open cart '{}': {}", cart_path.display(), e)))?;

    let wasm_dir = find_wasm_dir()?;

    // Validate that required WASM files are present.
    for name in &["blyt_wasm.js", "blyt_wasm.wasm", "blyt_wasm.html"] {
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

    println!("blyt run: serving on http://127.0.0.1:{port}/");
    println!("  Cart:     {}", cart_path.display());
    println!("  WASM dir: {}", wasm_dir.display());
    println!();
    println!("Open http://127.0.0.1:{port}/ in your browser.");
    println!("Press Ctrl+C to stop.");
    println!();
    // Ports adjacent to the HTTP server are reserved for the debugger
    // WebSocket bridges (planned for a future release).
    println!(
        "  [future] DAP debugger (Lua):    ws://127.0.0.1:{}/dap",
        port.wrapping_add(1)
    );
    println!(
        "  [future] GDB debugger (native): ws://127.0.0.1:{}/gdb",
        port.wrapping_add(2)
    );

    serve(listener, Arc::new(wasm_dir), Arc::new(cart_path))
}

/* -------------------------------------------------------------------------
 * WASM directory discovery
 *
 * Resolution order:
 *   1. $BLYT_WASM_DIR          — explicit override
 *   2. <sdk>/wasm/             — SDK layout (blyt_sdk.cmake Step 7)
 *   3. <repo>/build-wasm/      — manual emcmake cmake invocation
 * ------------------------------------------------------------------------- */

fn find_wasm_dir() -> Result<PathBuf, RunError> {
    if let Ok(d) = std::env::var("BLYT_WASM_DIR") {
        let p = PathBuf::from(&d);
        if p.join("blyt_wasm.js").exists() {
            return Ok(p);
        }
        return Err(err(format!(
            "BLYT_WASM_DIR={d} does not contain blyt_wasm.js"
        )));
    }

    // SDK layout: <sdk>/wasm/blyt_wasm.js  (binary lives in <sdk>/bin/blyt)
    if let Some(sdk) = sdk_root_from_exe() {
        let p = sdk.join("wasm");
        if p.join("blyt_wasm.js").exists() {
            return Ok(p);
        }
    }

    // Dev layout: walk up from the binary looking for build-wasm/
    if let Ok(exe) = std::env::current_exe() {
        for ancestor in exe.ancestors().skip(1) {
            let candidate = ancestor.join("build-wasm");
            if candidate.join("blyt_wasm.js").exists() {
                return Ok(candidate);
            }
        }
    }

    Err(err(
        "cannot find blyt_wasm.js — build the WASM runtime first:\n\
         \x20 emcmake cmake -B build-wasm -S frontends/wasm\n\
         \x20 cmake --build build-wasm\n\
         Or set BLYT_WASM_DIR to point to the directory containing blyt_wasm.js.",
    ))
}

/* -------------------------------------------------------------------------
 * HTTP server
 * ------------------------------------------------------------------------- */

fn serve(
    listener: TcpListener,
    wasm_dir: Arc<PathBuf>,
    cart_path: Arc<PathBuf>,
) -> Result<(), RunError> {
    for stream in listener.incoming() {
        let stream = match stream {
            Ok(s) => s,
            Err(_) => continue,
        };
        let wasm_dir = Arc::clone(&wasm_dir);
        let cart_path = Arc::clone(&cart_path);
        std::thread::spawn(move || {
            handle_connection(stream, &wasm_dir, &cart_path);
        });
    }
    Ok(())
}

fn handle_connection(mut stream: TcpStream, wasm_dir: &Path, cart_path: &Path) {
    let mut buf = [0u8; 4096];
    let n = match stream.read(&mut buf) {
        Ok(0) | Err(_) => return,
        Ok(n) => n,
    };
    let request = std::str::from_utf8(&buf[..n]).unwrap_or("");
    let path = request_path(request);

    let (file_path, content_type): (PathBuf, &str) = match path {
        "/" | "/index.html" => (wasm_dir.join("blyt_wasm.html"), "text/html; charset=utf-8"),
        "/blyt_wasm.js" => (wasm_dir.join("blyt_wasm.js"), "application/javascript"),
        "/blyt_wasm.wasm" => (wasm_dir.join("blyt_wasm.wasm"), "application/wasm"),
        "/cart.blyt" => (cart_path.to_path_buf(), "application/octet-stream"),
        _ => {
            respond(&mut stream, 404, "text/plain", b"Not Found");
            return;
        }
    };

    match fs::read(&file_path) {
        Ok(data) => respond(&mut stream, 200, content_type, &data),
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
         Connection: close\r\n\
         \r\n",
        body.len()
    );
    let _ = stream.write_all(header.as_bytes());
    let _ = stream.write_all(body);
}
