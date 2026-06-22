#!/usr/bin/env node
/*
 * tests/wasm/run_cart.js — headless Node.js driver for the blyt WASM runtime.
 *
 * Usage:
 *   node run_cart.js <wasm_dir> <cart_path> [<frame0_output_path>]
 *
 * Sets up globals read by frontends/wasm/module_pre.js (injected into the
 * Emscripten IIFE via --pre-js), then requires blytplay.js.
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

var nodefs = require('node:fs');
var path = require('node:path');

var wasmDir = process.argv[2];
var cartPath = process.argv[3];
var frame0OutPath = process.argv[4] || null;
/* Optional 5th argument: JSON object of env vars to inject into the C
 * environment via globalThis.__blyt_env_vars (read by module_pre.js). */
var envVarsJson = process.argv[5] || null;
/* Optional 6th argument: JSON object mapping a MEMFS path to a host file path;
 * each host file is read and seeded into MEMFS before the cart runs (e.g. a
 * pre-existing .blys save written by another cart process). */
var seedFilesJson = process.argv[6] || null;

if (!wasmDir || !cartPath) {
	process.stderr.write(
		'usage: run_cart.js <wasm_dir> <cart_path> ' +
			'[<frame0_output> [<env_json> [<seed_files_json>]]]\n',
	);
	process.exit(1);
}

var wasmJsPath = path.join(wasmDir, 'blytplay.js');
if (!nodefs.existsSync(wasmJsPath)) {
	process.stderr.write(`blytplay.js not found at: ${wasmJsPath}\n`);
	process.exit(1);
}

try {
	global.__blyt_cart_data = new Uint8Array(nodefs.readFileSync(cartPath));
} catch (e) {
	process.stderr.write(`cannot read cart: ${e.message}\n`);
	process.exit(1);
}

/* module_pre.js (inside the Emscripten IIFE) reads these globals:
 *   __blyt_cart_data   — written to /cart.blyt in MEMFS via preRun
 *   __blyt_frame0_path — if set, blyt_js_dump_frame0_if_headless writes here
 *   __blyt_init_module — extra Module fields (print, printErr) */
global.__blyt_frame0_path = frame0OutPath;
if (envVarsJson) {
	try {
		global.__blyt_env_vars = JSON.parse(envVarsJson);
	} catch (e) {
		process.stderr.write(`run_cart.js: invalid env JSON: ${e.message}\n`);
		process.exit(1);
	}
}
if (seedFilesJson) {
	try {
		const seedMap = JSON.parse(seedFilesJson);
		global.__blyt_seed_files = Object.keys(seedMap).map((memfsPath) => ({
			path: memfsPath,
			data: new Uint8Array(nodefs.readFileSync(seedMap[memfsPath])),
		}));
	} catch (e) {
		process.stderr.write(`run_cart.js: cannot seed files: ${e.message}\n`);
		process.exit(1);
	}
}
global.__blyt_init_module = {
	print: (text) => {
		process.stdout.write(`${text}\n`);
	},
	printErr: (text) => {
		process.stderr.write(`${text}\n`);
	},
};

require(path.resolve(wasmJsPath));
