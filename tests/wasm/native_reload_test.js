#!/usr/bin/env node
/*
 * tests/wasm/native_reload_test.js — headless driver for native/hybrid live
 * reload in WASM-dev run mode (issue #124).
 *
 * Usage:
 *   node native_reload_test.js <wasm_dir> <cart_v1> <cart_v2> [extra_expect]
 *
 * `extra_expect` (optional) is an extra substring that must appear in the
 * reload's output — used by the hybrid leg to assert the NATIVE module reloaded
 * too (e.g. "tag=2"), not just the host-Lua side.
 *
 * Loads blytplay.js with cart_v1 (a cart with native code — pure C/Rust/C++ or
 * a Lua+native hybrid), lets init() run, then drives a single `reload` command
 * straight into the C handler (blyt_dev_ctrl_command).  Where the pure-Lua
 * driver (dev_ctrl_test.js) exercises a host-Lua bytecode swap, this exercises
 * the in-VM cart-as-library module swap (blyt_session_swap_cart, issue #127):
 * the rebuilt cart's new code must run, with state preserved across the swap.
 *
 *   reload → ok, swaps to cart_v2; state (score=7) preserved across the code
 *            swap, so v2's on_load_state reports 7 (HOT_RELOAD, reason 3),
 *            NOT v2's fresh init value (100).
 *
 * Exit 0 on success, non-zero with a diagnostic on failure.
 */

const nodefs = require('node:fs');
const path = require('node:path');

const [, , wasmDir, cartV1, cartV2, extraExpect] = process.argv;
if (!wasmDir || !cartV1 || !cartV2) {
	process.stderr.write(
		'usage: native_reload_test.js <wasm_dir> <cart_v1> <cart_v2> [extra_expect]\n',
	);
	process.exit(1);
}

/* Save slots live in the Emscripten MEMFS (in-memory, ephemeral). */
const SAVE_DIR = '/blyt_save';

const wasmJsPath = path.join(wasmDir, 'blytplay.js');
if (!nodefs.existsSync(wasmJsPath)) {
	process.stderr.write(`blytplay.js not found at: ${wasmJsPath}\n`);
	process.exit(1);
}

let M = null; // captured Module
const prints = []; // cart debug output (blyt_console_debug / blyt.debug.print)
const responses = []; // dev control responses (JSON strings)

function fail(msg) {
	process.stderr.write(`native_reload_test: ${msg}\n`);
	process.stderr.write(`prints:\n${prints.join('\n')}\n`);
	process.stderr.write(`responses:\n${responses.join('\n')}\n`);
	process.exit(1);
}

/* Runtime → devtool: collect each response line. */
globalThis.blyt_dev_ctrl_send = (json) => {
	responses.push(json);
};

/* Reload re-fetch: write cart_v2 into MEMFS (simulating a rebuild), then hand
 * back to C.  Synchronous here, so the whole reload completes inside the
 * originating blyt_dev_ctrl_command call. */
globalThis.blyt_dev_ctrl_fetch_cart = () => {
	M.FS.writeFile('/cart.blyt', new Uint8Array(nodefs.readFileSync(cartV2)));
	M.ccall('blyt_dev_ctrl_reload_fetched', null, ['int'], [1]);
};

/* Send one command and return the single response object it produced. */
function command(obj) {
	const before = responses.length;
	M.ccall('blyt_dev_ctrl_command', null, ['string'], [JSON.stringify(obj)]);
	const produced = responses.slice(before);
	if (produced.length !== 1) {
		fail(
			`command ${JSON.stringify(obj)} produced ${produced.length} responses`,
		);
	}
	return JSON.parse(produced[0]);
}

function expectOk(obj) {
	const r = command(obj);
	if (r.status !== 'ok' || r.id !== obj.id || r.cmd !== obj.cmd) {
		fail(
			`expected ok for ${JSON.stringify(obj)}, got ${JSON.stringify(r)}`,
		);
	}
	return r;
}

function drive() {
	// reload → swaps to cart_v2 via the in-VM module swap; state (score=7)
	// preserved across the code swap, so v2's on_load_state must report 7
	// (HOT_RELOAD), not v2's fresh init value (100).
	const before = prints.length;
	expectOk({ id: 1, cmd: 'reload' });
	const reloadPrints = prints.slice(before);
	if (
		!reloadPrints.some(
			(p) => p.startsWith('v2 load') && p.includes('reason=3'),
		)
	) {
		fail('reload did not run v2 on_load_state(HOT_RELOAD)');
	}
	if (!reloadPrints.some((p) => p.includes('score=7'))) {
		fail(
			'reload did not preserve state across the code swap (expected score=7)',
		);
	}
	if (extraExpect && !reloadPrints.some((p) => p.includes(extraExpect))) {
		fail(`reload output missing expected substring "${extraExpect}"`);
	}

	process.stdout.write('native_reload_test: PASS\n');
	process.exit(0);
}

/* Cart debug output is routed through blyt_js_log → console.log (an EM_JS
 * call) for both native (blyt_console_debug) and Lua carts, so capture
 * console.log to collect it. */
console.log = (...args) => {
	prints.push(args.map(String).join(' '));
};

global.__blyt_cart_data = new Uint8Array(nodefs.readFileSync(cartV1));
global.__blyt_env_vars = { BLYT_SAVE_DIR: SAVE_DIR };
global.__blyt_init_module = {
	printErr: (t) => process.stderr.write(`${t}\n`),
	onRuntimeInitialized() {
		M = this;
		// Let init() + a few frames run before driving the reload.
		setTimeout(drive, 300);
	},
};

require(path.resolve(wasmJsPath));
