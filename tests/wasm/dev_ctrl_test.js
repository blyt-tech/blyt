#!/usr/bin/env node
/*
 * tests/wasm/dev_ctrl_test.js — headless driver for the dev control channel
 * C handler (issue #87).
 *
 * Usage:
 *   node dev_ctrl_test.js <wasm_dir> <cart_v1> <cart_v2>
 *
 * Loads blytplay.js with cart_v1, lets it initialise, then drives lifecycle
 * commands straight into the C handler (blyt_dev_ctrl_command) and checks the
 * JSON responses captured via globalThis.blyt_dev_ctrl_send.  The relay/socket
 * transport is unit-tested separately in devtool/src/run.rs; this exercises the
 * runtime handler in isolation.
 *
 *   reset       → ok, cart re-inits (on_new_state fires)
 *   save_state  → ok
 *   load_state  → ok, on_load_state fires (reason 0 = EXPLICIT)
 *   reload      → ok, swaps to cart_v2, state preserved across the code swap
 *                 (on_load_state reason 3 = HOT_RELOAD reports the v1 value)
 *   out-of-seq  → two commands, responses echo their ids in order
 *
 * Exit 0 on success, non-zero with a diagnostic on failure.
 */

const nodefs = require('node:fs');
const path = require('node:path');

const [, , wasmDir, cartV1, cartV2] = process.argv;
if (!wasmDir || !cartV1 || !cartV2) {
	process.stderr.write(
		'usage: dev_ctrl_test.js <wasm_dir> <cart_v1> <cart_v2>\n',
	);
	process.exit(1);
}

/* Save slots live in the Emscripten MEMFS (in-memory, ephemeral).  ensure_dir
 * in save.c only creates one level, so this must be a shallow path under the
 * MEMFS root. */
const SAVE_DIR = '/blyt_save';

const wasmJsPath = path.join(wasmDir, 'blytplay.js');
if (!nodefs.existsSync(wasmJsPath)) {
	process.stderr.write(`blytplay.js not found at: ${wasmJsPath}\n`);
	process.exit(1);
}

let M = null; // captured Module
const prints = []; // blyt.debug.print output
const responses = []; // dev control responses (JSON strings)

function fail(msg) {
	process.stderr.write(`dev_ctrl_test: ${msg}\n`);
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
	// 1. reset → fresh re-init; on_new_state prints the freshly-initialised value.
	const printsBeforeReset = prints.length;
	expectOk({ id: 1, cmd: 'reset' });
	if (!prints.slice(printsBeforeReset).some((p) => p.startsWith('v1 new'))) {
		fail('reset did not re-run init/on_new_state');
	}

	// 2. save_state slot 1.
	expectOk({ id: 2, cmd: 'save_state', slot: 1 });

	// 3. load_state slot 1 → on_load_state(reason 0).
	const printsBeforeLoad = prints.length;
	expectOk({ id: 3, cmd: 'load_state', slot: 1 });
	if (!prints.slice(printsBeforeLoad).some((p) => p.includes('reason=0'))) {
		fail('load_state did not fire on_load_state with reason 0');
	}

	// 4. reload → swaps to cart_v2; state (score=7) preserved across the code
	//    swap, so v2's on_load_state must report 7 (not v2's fresh init value).
	const printsBeforeReload = prints.length;
	expectOk({ id: 4, cmd: 'reload' });
	const reloadPrints = prints.slice(printsBeforeReload);
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

	// 5. Out-of-sequence: two commands; responses must echo their ids in order.
	const a = command({ id: 10, cmd: 'reset' });
	const b = command({ id: 11, cmd: 'reset' });
	if (a.id !== 10 || b.id !== 11) {
		fail(`id tracking broken: got ${a.id}, ${b.id}`);
	}

	process.stdout.write('dev_ctrl_test: PASS\n');
	process.exit(0);
}

/* blyt.debug.print is routed through blyt_js_log → console.log (an EM_JS
 * call), not Module.print, so capture console.log to collect cart output. */
console.log = (...args) => {
	prints.push(args.map(String).join(' '));
};

global.__blyt_cart_data = new Uint8Array(nodefs.readFileSync(cartV1));
global.__blyt_env_vars = { BLYT_SAVE_DIR: SAVE_DIR };
global.__blyt_init_module = {
	printErr: (t) => process.stderr.write(`${t}\n`),
	onRuntimeInitialized() {
		M = this;
		// Let init() + a few frames run before driving commands.
		setTimeout(drive, 300);
	},
};

require(path.resolve(wasmJsPath));
