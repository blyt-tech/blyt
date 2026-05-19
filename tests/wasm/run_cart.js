#!/usr/bin/env node
/*
 * tests/wasm/run_cart.js — headless Node.js driver for the blyt WASM runtime.
 *
 * Usage:
 *   node run_cart.js <wasm_dir> <cart_path> [<frame0_output_path>]
 *
 * Sets up globals read by frontends/wasm/module_pre.js (injected into the
 * Emscripten IIFE via --pre-js), then requires blyt_wasm.js.
 *
 * Without <frame0_output_path>: runs until the cart exits naturally.
 *   blyt_console_debug output goes to stdout.  Exit code mirrors the cart.
 *
 * With <frame0_output_path>: sets globalThis.__blyt_frame0_path so that the
 *   blyt_js_dump_frame0_if_headless EM_JS function in wasm_main.c writes the
 *   320×240 XRGB8888 frame directly to <frame0_output_path> on the host
 *   filesystem after the first BLYT_ECALL_FRAME_DONE, then exits.
 *
 * Used by the Rust integration test wasm_testcard_frame0_matches_golden.
 */

"use strict";

var nodefs = require("fs");
var path = require("path");

var wasmDir = process.argv[2];
var cartPath = process.argv[3];
var frame0OutPath = process.argv[4] || null;

if (!wasmDir || !cartPath) {
  process.stderr.write("usage: run_cart.js <wasm_dir> <cart_path> [<frame0_output>]\n");
  process.exit(1);
}

var wasmJsPath = path.join(wasmDir, "blyt_wasm.js");
if (!nodefs.existsSync(wasmJsPath)) {
  process.stderr.write("blyt_wasm.js not found at: " + wasmJsPath + "\n");
  process.exit(1);
}

try {
  global.__blyt_cart_data = new Uint8Array(nodefs.readFileSync(cartPath));
} catch (e) {
  process.stderr.write("cannot read cart: " + e.message + "\n");
  process.exit(1);
}

/* module_pre.js (inside the Emscripten IIFE) reads these globals:
 *   __blyt_cart_data   — written to /cart.blyt in MEMFS via preRun
 *   __blyt_frame0_path — if set, blyt_js_dump_frame0_if_headless writes here
 *   __blyt_init_module — extra Module fields (print, printErr) */
global.__blyt_frame0_path = frame0OutPath;
global.__blyt_init_module = {
  print: function (text) {
    process.stdout.write(text + "\n");
  },
  printErr: function (text) {
    process.stderr.write(text + "\n");
  },
};

require(path.resolve(wasmJsPath));
