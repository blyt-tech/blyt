#!/usr/bin/env node
/*
 * tests/wasm/reload_resource_test.js — headless driver for the WASM pure-Lua
 * fast-path `reload` cart-swap with a resource read AFTER the swap (issue #246).
 *
 * Usage:
 *   node reload_resource_test.js <wasm_dir> <cart_v1> <cart_v2>
 *
 * cart_v1 and cart_v2 are pure-Lua carts (g_session == NULL — the host-Lua fast
 * path) that bundle an uncompressed resource (`greeting.txt`) whose content
 * differs between the two images (RES_V1 → RES_V2).  Both read the resource in
 * on_load_state and print "reload greeting=<content>".
 *
 * The cart-swap reload rebuilds the host-Lua VM from cart_v2's bytecode, closing
 * cart_v1 and opening cart_v2.  Resource-table entries are zero-copy aliases into
 * the cart map (resource.c e->data = body), so the reload MUST reload
 * g_lua_resources from cart_v2 before cart_v1 is freed.  Pre-fix
 * (blyt_dev_ctrl_reload_fetched dropped the table reload) the post-reload read
 * aliases the freed cart_v1 map → stale RES_V1 or garbage, never RES_V2 → this
 * driver fails.  Post-fix it re-reads RES_V2 from the new cart.
 *
 * Where dev_ctrl_test.js only reloads `hello` (zero resources) and asserts state
 * preservation, this pins the resource table across the swap — the coverage hole
 * #246 slipped through.
 *
 * Exit 0 on success, non-zero with a diagnostic on failure.
 */

const nodefs = require('node:fs');
const path = require('node:path');

const [, , wasmDir, cartV1, cartV2] = process.argv;
if (!wasmDir || !cartV1 || !cartV2) {
	process.stderr.write(
		'usage: reload_resource_test.js <wasm_dir> <cart_v1> <cart_v2>\n',
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
const prints = []; // cart debug output (blyt.debug.print)
const responses = []; // dev control responses (JSON strings)

function fail(msg) {
	process.stderr.write(`reload_resource_test: ${msg}\n`);
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
	// Sanity: cart_v1's init() read the bundled resource and printed RES_V1.
	if (!prints.some((p) => p.includes('greeting=RES_V1'))) {
		fail('cart_v1 did not read its bundled resource (RES_V1) at init');
	}

	// reload → swaps to cart_v2; the host-Lua fast path (g_session == NULL) must
	// reload g_lua_resources from cart_v2 before freeing cart_v1, so the post-swap
	// on_load_state read returns RES_V2 (HOT_RELOAD, reason 3).  Pre-fix the table
	// still aliases the freed cart_v1 map → RES_V1 / garbage, never RES_V2.
	const before = prints.length;
	expectOk({ id: 1, cmd: 'reload' });
	const reloadPrints = prints.slice(before);
	if (
		!reloadPrints.some(
			(p) => p.startsWith('reload greeting=') && p.includes('reason=3'),
		)
	) {
		fail('reload did not fire cart_v2 on_load_state(HOT_RELOAD)');
	}
	if (!reloadPrints.some((p) => p.includes('greeting=RES_V2'))) {
		fail(
			"post-reload resource read did NOT return the new cart's content " +
				'(expected RES_V2) — g_lua_resources still aliases the freed old cart (#246)',
		);
	}

	process.stdout.write('reload_resource_test: PASS\n');
	process.exit(0);
}

/* blyt.debug.print is routed through blyt_js_log → console.log (an EM_JS call),
 * not Module.print, so capture console.log to collect cart output. */
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
