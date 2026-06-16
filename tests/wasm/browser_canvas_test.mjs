#!/usr/bin/env node

/*
 * tests/wasm/browser_canvas_test.mjs — headless Chromium canvas test.
 *
 * Loads a standalone blyt HTML page in headless Chromium via Playwright,
 * waits for blyt_js_present to paint at least one frame, reads the canvas
 * via getImageData, and compares it byte-for-byte against a golden XRGB8888
 * binary frame.
 *
 * Usage:
 *   node browser_canvas_test.mjs <html_path> <golden_bin_path>
 *
 * Exit 0 on success, non-zero on failure.
 *
 * Pixel format notes:
 *   golden (testcard_frame0.bin): raw uint32_t array, little-endian bytes
 *     [B, G, R, X] per pixel (XRGB8888 host byte order).
 *   canvas getImageData: [R, G, B, A] per pixel (RGBA8888).
 *
 * Requires playwright and the Chromium browser:
 *   npm install --prefix tests/wasm
 *   npx playwright install chromium
 */

import { readFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';
import { chromium } from 'playwright';

const [, , htmlPath, goldenPath] = process.argv;
if (!htmlPath || !goldenPath) {
	process.stderr.write(
		'usage: browser_canvas_test.mjs <html_path> <golden_bin_path>\n',
	);
	process.exit(1);
}

const W = 320,
	H = 240;
const TIMEOUT_MS = 60000;

const goldenBin = readFileSync(goldenPath);
if (goldenBin.length !== W * H * 4) {
	process.stderr.write(
		`golden size mismatch: expected ${W * H * 4} bytes, got ${goldenBin.length}\n`,
	);
	process.exit(1);
}

let browser;
try {
	browser = await chromium.launch({ headless: true });
	const page = await browser.newPage();

	page.on('console', (msg) => {
		if (msg.type() === 'error')
			process.stderr.write(`[browser] ${msg.text()}\n`);
	});
	page.on('pageerror', (err) =>
		process.stderr.write(`[browser error] ${err.message}\n`),
	);

	/* Intercept the very first putImageData call — frame 0 — before any
	 * subsequent frame can overwrite the canvas.  The testcard includes a
	 * frame counter that differs per frame, so sniffing putImageData is the
	 * only reliable way to capture exactly frame 0.                          */
	await page.addInitScript(() => {
		const _orig = CanvasRenderingContext2D.prototype.putImageData;
		window.__blyt_frame0_rgba = null;
		CanvasRenderingContext2D.prototype.putImageData = function (
			imageData,
			...rest
		) {
			_orig.call(this, imageData, ...rest);
			if (!window.__blyt_frame0_rgba)
				window.__blyt_frame0_rgba = Array.from(imageData.data);
		};
	});

	await page.goto(pathToFileURL(htmlPath).href);

	/* Wait for frame 0 to be captured. */
	await page.waitForFunction(() => Array.isArray(window.__blyt_frame0_rgba), {
		timeout: TIMEOUT_MS,
	});
	const canvasRGBA = await page.evaluate(() => window.__blyt_frame0_rgba);

	await browser.close();
	browser = null;

	/* Compare.
	 * golden bytes per pixel: [B, G, R, X]
	 * canvas bytes per pixel: [R, G, B, A]               */
	const npixels = W * H;
	let mismatches = 0;
	for (let i = 0; i < npixels; i++) {
		const gB = goldenBin[4 * i + 0];
		const gG = goldenBin[4 * i + 1];
		const gR = goldenBin[4 * i + 2];

		const cR = canvasRGBA[4 * i + 0];
		const cG = canvasRGBA[4 * i + 1];
		const cB = canvasRGBA[4 * i + 2];
		const cA = canvasRGBA[4 * i + 3];

		if (cR !== gR || cG !== gG || cB !== gB || cA !== 255) {
			if (mismatches < 5)
				process.stderr.write(
					`pixel (${i % W},${Math.floor(i / W)}): ` +
						`golden=(${gR},${gG},${gB}) canvas=(${cR},${cG},${cB},${cA})\n`,
				);
			mismatches++;
		}
	}

	if (mismatches > 0) {
		process.stderr.write(
			`FAIL: ${mismatches} of ${npixels} pixels differ from golden\n`,
		);
		process.exit(1);
	}

	console.log(`PASS: canvas matches golden (${npixels} pixels)`);
	process.exit(0);
} catch (e) {
	if (browser) await browser.close().catch(() => {});
	process.stderr.write(`ERROR: ${e.message}\n`);
	if (
		e.message.includes("Executable doesn't exist") ||
		e.message.includes('browserType.launch')
	) {
		process.stderr.write(
			'Hint: run `npx playwright install chromium` to download the browser.\n',
		);
	}
	process.exit(1);
}
