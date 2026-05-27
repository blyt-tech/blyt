use std::fs;
use std::path::{Path, PathBuf};

use crate::run::find_wasm_dir;

/* -------------------------------------------------------------------------
 * Error type
 * ------------------------------------------------------------------------- */

#[derive(Debug)]
pub struct ExportError(String);

impl std::fmt::Display for ExportError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}", self.0)
    }
}

impl From<std::io::Error> for ExportError {
    fn from(e: std::io::Error) -> Self {
        ExportError(e.to_string())
    }
}

fn err(msg: impl Into<String>) -> ExportError {
    ExportError(msg.into())
}

/* -------------------------------------------------------------------------
 * Public entry point
 * ------------------------------------------------------------------------- */

pub fn run(cart_path: &Path, output: Option<&Path>) -> Result<(), ExportError> {
    let cart_path = cart_path
        .canonicalize()
        .map_err(|e| err(format!("cannot open cart '{}': {}", cart_path.display(), e)))?;

    let wasm_dir = find_wasm_dir().map_err(|e| ExportError(e.to_string()))?;

    let js = fs::read_to_string(wasm_dir.join("blytplay.js"))
        .map_err(|e| err(format!("cannot read blytplay.js: {e}")))?;
    let wasm_bytes = fs::read(wasm_dir.join("blytplay.wasm"))
        .map_err(|e| err(format!("cannot read blytplay.wasm: {e}")))?;
    let cart_bytes = fs::read(&cart_path).map_err(|e| err(format!("cannot read cart: {e}")))?;

    let cart_name = cart_path
        .file_stem()
        .and_then(|s| s.to_str())
        .unwrap_or("blyt cart");

    let out = output
        .map(PathBuf::from)
        .unwrap_or_else(|| cart_path.with_extension("html"));

    let html = generate_html(cart_name, &cart_bytes, &wasm_bytes, &js);
    fs::write(&out, &html)?;

    println!("exported: {} ({} KB)", out.display(), html.len() / 1024);
    Ok(())
}

/* -------------------------------------------------------------------------
 * HTML generation
 *
 * Produces a single self-contained HTML file.  The WASM binary and cart are
 * embedded as base64 strings and decoded at runtime via atob().  Setting
 * Module.wasmBinary to the decoded ArrayBuffer makes Emscripten skip its
 * normal fetch; the cart is written into MEMFS via a preRun hook so the
 * runtime finds it at /cart.blyt as usual.
 * ------------------------------------------------------------------------- */

fn generate_html(cart_name: &str, cart_bytes: &[u8], wasm_bytes: &[u8], js: &str) -> String {
    let title = html_escape(cart_name);
    let wasm_b64 = base64_encode(wasm_bytes);
    let cart_b64 = base64_encode(cart_bytes);

    let mut html = String::with_capacity(512 + wasm_b64.len() + cart_b64.len() + js.len());

    html.push_str("<!doctype html>\n");
    html.push_str("<html lang=\"en\">\n");
    html.push_str("<head>\n");
    html.push_str("<meta charset=\"utf-8\">\n");
    html.push_str("<title>");
    html.push_str(&title);
    html.push_str("</title>\n");
    html.push_str("<style>\n");
    html.push_str(
        "html,body{margin:0;padding:0;background:#111;\
         display:flex;flex-direction:column;\
         align-items:center;justify-content:center;height:100vh}\n",
    );
    html.push_str(
        "#canvas{width:640px;height:480px;\
         image-rendering:pixelated;image-rendering:crisp-edges;\
         border:1px solid #333}\n",
    );
    html.push_str("</style>\n</head>\n<body>\n");
    html.push_str("<canvas id=\"canvas\" width=\"320\" height=\"240\"></canvas>\n");

    // Inline script: decode embedded data and configure the Emscripten Module.
    //
    // Module.wasmBinary — ArrayBuffer of the WASM module.  When set,
    // Emscripten uses it directly and skips any fetch of blytplay.wasm.
    //
    // preRun — writes the cart into the virtual filesystem before main()
    // runs, exactly as the dev shell does via a fetch.
    html.push_str("<script>\n");
    html.push_str("(function(){\n");
    html.push_str("function b64(s){");
    html.push_str(
        "var bin=atob(s),a=new Uint8Array(bin.length);\
         for(var i=0;i<bin.length;i++)a[i]=bin.charCodeAt(i);return a}\n",
    );
    html.push_str("var wasmBytes=b64(\"");
    html.push_str(&wasm_b64);
    html.push_str("\");\n");
    html.push_str("var cartBytes=b64(\"");
    html.push_str(&cart_b64);
    html.push_str("\");\n");
    html.push_str("window.Module={\n");
    html.push_str("  canvas:document.getElementById(\"canvas\"),\n");
    html.push_str("  wasmBinary:wasmBytes.buffer,\n");
    html.push_str("  preRun:[function(){FS.writeFile(\"/cart.blyt\",cartBytes);}],\n");
    html.push_str("  print:function(s){console.log(s);},\n");
    html.push_str("  printErr:function(s){console.error(s);},\n");
    html.push_str("};\n");
    html.push_str("}());\n");
    html.push_str("</script>\n");

    // Inline blytplay.js — the Emscripten JS glue with module_pre.js baked in.
    // module_pre.js inside blytplay.js reads window.Module and picks up our
    // wasmBinary and preRun without modification (globalThis.__blyt_cart_data
    // is not set, so module_pre.js adds no additional preRun hooks).
    html.push_str("<script>\n");
    html.push_str(js);
    html.push_str("\n</script>\n");

    html.push_str("</body>\n</html>\n");
    html
}

/* -------------------------------------------------------------------------
 * HTML attribute escaping
 * ------------------------------------------------------------------------- */

fn html_escape(s: &str) -> String {
    s.replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
        .replace('"', "&quot;")
}

/* -------------------------------------------------------------------------
 * Base64 encoder (RFC 4648, no line wrapping)
 * ------------------------------------------------------------------------- */

fn base64_encode(data: &[u8]) -> String {
    const CHARS: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::with_capacity((data.len() + 2) / 3 * 4);
    for chunk in data.chunks(3) {
        let b0 = chunk[0] as u32;
        let b1 = if chunk.len() > 1 { chunk[1] as u32 } else { 0 };
        let b2 = if chunk.len() > 2 { chunk[2] as u32 } else { 0 };
        let n = (b0 << 16) | (b1 << 8) | b2;
        out.push(CHARS[(n >> 18 & 63) as usize] as char);
        out.push(CHARS[(n >> 12 & 63) as usize] as char);
        out.push(if chunk.len() > 1 {
            CHARS[(n >> 6 & 63) as usize] as char
        } else {
            '='
        });
        out.push(if chunk.len() > 2 {
            CHARS[(n & 63) as usize] as char
        } else {
            '='
        });
    }
    out
}
