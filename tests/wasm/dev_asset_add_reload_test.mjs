#!/usr/bin/env node

/*
 * tests/wasm/dev_asset_add_reload_test.mjs — headless Chromium driver for the
 * WASM pure-Lua dev-mode ASSET-SET-CHANGE → `reload` path (issue #246).
 *
 * Where dev_asset_hotswap_test.mjs edits an asset's *content* (same resource-id
 * set → the watcher dispatches `update_assets`), this ADDS a new asset — a
 * resource-id-index change — for which devtool's dispatch_signals emits a bare
 * `reload` with NO `update_assets` (see the dispatch_resource_set_change_forces_
 * reload unit test).  That reload closes the old cart and rebuilds; unless the
 * host-Lua fast path reloads g_lua_resources from the new cart, every zero-copy
 * resource alias dangles into the freed old cart map (#246).
 *
 * To make the reload observable, the greeting asset the cart reads every frame is
 * edited to v2 in the SAME coalesced rebuild (the two writes land inside the
 * watcher's 150 ms settle window, so they diff as one id-set change carrying a
 * content change — exactly the ordering the issue notes update_assets cannot
 * rescue).  Pre-fix the post-reload read returns stale v1; post-fix it returns v2.
 *
 * Sequence:
 *   1. wait until the page console shows the v1 greeting text (preloaded + read),
 *   2. add a new asset file AND overwrite the greeting with v2 (back-to-back, one
 *      rebuild) — the watcher diffs a new resource id and broadcasts `reload`,
 *   3. wait until the console shows the v2 greeting text — the reload reloaded the
 *      resource table from the new cart.
 *
 * Usage:
 *   node dev_asset_add_reload_test.mjs <url> <greeting_path> <new_asset_path> \
 *        <v1_text> <v2_text>
 *
 * Exit 0 on success, non-zero with a diagnostic on failure.
 */

import { writeFileSync } from 'node:fs';
import { chromium } from 'playwright';

const [, , url, greetingPath, newAssetPath, v1Text, v2Text] = process.argv;
if (!url || !greetingPath || !newAssetPath || !v1Text || !v2Text) {
	process.stderr.write(
		'usage: dev_asset_add_reload_test.mjs <url> <greeting_path> <new_asset_path> <v1_text> <v2_text>\n',
	);
	process.exit(1);
}

const TIMEOUT_MS = 60000;
const POLL_MS = 100;

const consoleLines = [];

function sawLine(text) {
	return consoleLines.some((l) => l.includes(text));
}

async function waitFor(text, what) {
	const start = Date.now();
	while (Date.now() - start < TIMEOUT_MS) {
		if (sawLine(text)) return;
		await new Promise((r) => setTimeout(r, POLL_MS));
	}
	throw new Error(
		`timed out waiting for ${what} ("${text}").\nconsole so far:\n${consoleLines.join('\n')}`,
	);
}

let browser;
try {
	browser = await chromium.launch({ headless: true });
	const page = await browser.newPage();

	page.on('console', (msg) => {
		consoleLines.push(msg.text());
	});
	page.on('pageerror', (err) =>
		process.stderr.write(`[browser error] ${err.message}\n`),
	);

	await page.goto(url, { waitUntil: 'load', timeout: TIMEOUT_MS });

	// 1. The preloaded v1 greeting must reach the cart.
	await waitFor(v1Text, 'pre-reload greeting text');

	// 2. Add a new asset (forces the id-set change → bare `reload`) AND edit the
	//    greeting to v2 in the same coalesced rebuild.  Written back-to-back so
	//    both land inside the watcher's settle window as one rebuild.
	writeFileSync(newAssetPath, 'sentinel resource added at runtime\n');
	writeFileSync(greetingPath, v2Text);

	// 3. The reloaded cart must read the v2 greeting from the swapped-in cart —
	//    proving the bare reload reloaded g_lua_resources (not left it aliasing
	//    the freed old cart map).
	await waitFor(v2Text, 'post-reload greeting text');

	process.stdout.write('dev_asset_add_reload_test: PASS\n');
	await browser.close();
	process.exit(0);
} catch (e) {
	process.stderr.write(`dev_asset_add_reload_test: FAIL: ${e.message}\n`);
	if (browser) await browser.close();
	process.exit(1);
}
