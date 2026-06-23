#!/usr/bin/env node

/*
 * tests/wasm/dev_asset_hotswap_test.mjs — headless Chromium driver for the
 * WASM dev-mode asset hot-swap path (issue #118).
 *
 * Loads a *live* `blyt run ./project` page in headless Chromium (real shell.html,
 * real HTTP resource routes, real dev-control WebSocket + file watcher).  The
 * cart re-reads and prints a text resource every frame; the print is routed to
 * console.log via blyt_js_log, captured here through the page console.
 *
 * Sequence:
 *   1. wait until the page console shows the v1 asset text (proves the resource
 *      was preloaded into MEMFS over HTTP and the cart read it),
 *   2. overwrite the asset file on disk with the v2 text — the running
 *      `blyt run` watcher rebuilds, diffs the resource-id-index, and broadcasts
 *      `update_assets`; shell.html refetches the changed file into MEMFS and the
 *      host reloads its resource table,
 *   3. wait until the console shows the v2 text — the hot-swap took effect with
 *      no VM restart.
 *
 * Usage:
 *   node dev_asset_hotswap_test.mjs <url> <asset_path> <v1_text> <v2_text>
 *
 * Exit 0 on success, non-zero with a diagnostic on failure.
 */

import { writeFileSync } from 'node:fs';
import { chromium } from 'playwright';

const [, , url, assetPath, v1Text, v2Text] = process.argv;
if (!url || !assetPath || !v1Text || !v2Text) {
	process.stderr.write(
		'usage: dev_asset_hotswap_test.mjs <url> <asset_path> <v1_text> <v2_text>\n',
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

	// 1. The preloaded v1 resource must reach the cart.
	await waitFor(v1Text, 'pre-swap asset text');

	// 2. Edit the asset on disk → the live watcher drives the hot-swap.
	writeFileSync(assetPath, v2Text);

	// 3. The swapped-in v2 resource must reach the cart with no reload.
	await waitFor(v2Text, 'post-swap asset text');

	process.stdout.write('dev_asset_hotswap_test: PASS\n');
	await browser.close();
	process.exit(0);
} catch (e) {
	process.stderr.write(`dev_asset_hotswap_test: FAIL: ${e.message}\n`);
	if (browser) await browser.close();
	process.exit(1);
}
