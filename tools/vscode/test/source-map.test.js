/* Unit tests for the pure source-path mapping helpers in extension.js
 * (issue #46 / #51): the canonical `/blyt/*` → local workspace mapping the
 * debugger uses to open the author's own files.  Two consumers:
 *   - sourceMapPairs / sourceMapCommand → lldb-dap's target.source-map.
 *   - localizeCartPath → the Lua DAP path (no lldb to apply the source-map).
 * The integrated behaviour is exercised end-to-end by the headless debugger
 * suite (test/integration) and the runtime relay (tests/integration dap.rs);
 * these pin the branch logic of the pure helpers directly and cheaply.
 *
 * extension.js does `require('vscode')` at module load, so we stub it via a
 * Module._load shim before requiring the extension (see cart-detection.test.js):
 *
 *   node --test tools/vscode/test/ */

const Module = require('node:module');
const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const origLoad = Module._load;
Module._load = function (request, ...rest) {
	if (request === 'vscode')
		return {
			window: { activeTextEditor: undefined },
			workspace: { getConfiguration: () => ({ get: (_k, d) => d }) },
		};
	return origLoad.call(this, request, ...rest);
};

const { _test } = require('../extension.js');
const { sourceMapPairs, sourceMapCommand, localizeCartPath } = _test;

/* A throwaway workspace dir; optionally seed build/source-map.json. */
function makeCwd(sourceMapJson) {
	const cwd = fs.mkdtempSync(path.join(os.tmpdir(), 'blyt-srcmap-'));
	if (sourceMapJson !== undefined) {
		fs.mkdirSync(path.join(cwd, 'build'), { recursive: true });
		fs.writeFileSync(
			path.join(cwd, 'build', 'source-map.json'),
			sourceMapJson,
		);
	}
	return cwd;
}

/* ── sourceMapPairs ───────────────────────────────────────────────────────── */

test('sourceMapPairs without a manifest maps cart + legacy src to the workspace', () => {
	const cwd = makeCwd();
	assert.deepStrictEqual(sourceMapPairs(cwd), [
		['/blyt/cart', cwd],
		['/blyt/src', cwd],
	]);
});

test('sourceMapPairs merges extra prefixes from build/source-map.json', () => {
	const cwd = makeCwd(
		JSON.stringify([
			{ prefix: '/blyt/sdk', local: '/opt/blyt/sdk/src' },
			{ prefix: '/blyt/rust', local: '/opt/rust/src' },
		]),
	);
	assert.deepStrictEqual(sourceMapPairs(cwd), [
		['/blyt/cart', cwd],
		['/blyt/src', cwd],
		['/blyt/sdk', '/opt/blyt/sdk/src'],
		['/blyt/rust', '/opt/rust/src'],
	]);
});

test('sourceMapPairs does not let the manifest override the cart/src mapping', () => {
	const cwd = makeCwd(
		JSON.stringify([
			{ prefix: '/blyt/cart', local: '/somewhere/else' },
			{ prefix: '/blyt/src', local: '/elsewhere' },
			{ prefix: '/blyt/cargo', local: '/opt/cargo/src' },
		]),
	);
	assert.deepStrictEqual(sourceMapPairs(cwd), [
		['/blyt/cart', cwd],
		['/blyt/src', cwd],
		['/blyt/cargo', '/opt/cargo/src'],
	]);
});

test('sourceMapPairs skips manifest entries missing prefix or local', () => {
	const cwd = makeCwd(
		JSON.stringify([
			{ prefix: '/blyt/sdk' }, // no local
			{ local: '/opt/x' }, // no prefix
			{ prefix: '/blyt/cargo', local: '/opt/cargo/src' },
		]),
	);
	assert.deepStrictEqual(sourceMapPairs(cwd), [
		['/blyt/cart', cwd],
		['/blyt/src', cwd],
		['/blyt/cargo', '/opt/cargo/src'],
	]);
});

test('sourceMapPairs falls back to the cart mapping on a malformed manifest', () => {
	const cwd = makeCwd('{ not json');
	assert.deepStrictEqual(sourceMapPairs(cwd), [
		['/blyt/cart', cwd],
		['/blyt/src', cwd],
	]);
});

/* ── sourceMapCommand ─────────────────────────────────────────────────────── */

test('sourceMapCommand renders quoted canonical/local pairs for lldb', () => {
	const cwd = makeCwd(
		JSON.stringify([{ prefix: '/blyt/sdk', local: '/opt/blyt/sdk/src' }]),
	);
	assert.strictEqual(
		sourceMapCommand(cwd),
		`settings set target.source-map ${JSON.stringify('/blyt/cart')} ${JSON.stringify(cwd)} ${JSON.stringify('/blyt/src')} ${JSON.stringify(cwd)} ${JSON.stringify('/blyt/sdk')} ${JSON.stringify('/opt/blyt/sdk/src')}`,
	);
});

/* ── localizeCartPath ─────────────────────────────────────────────────────── */

test('localizeCartPath rewrites a /blyt/cart file to the workspace', () => {
	assert.strictEqual(
		localizeCartPath('/blyt/cart/src/game/main.lua', '/ws'),
		path.join('/ws', 'src/game/main.lua'),
	);
});

test('localizeCartPath maps the bare /blyt/cart prefix to the workspace root', () => {
	assert.strictEqual(localizeCartPath('/blyt/cart', '/ws'), '/ws');
});

test('localizeCartPath handles the legacy /blyt/src prefix', () => {
	assert.strictEqual(
		localizeCartPath('/blyt/src/main.lua', '/ws'),
		path.join('/ws', 'main.lua'),
	);
	assert.strictEqual(localizeCartPath('/blyt/src', '/ws'), '/ws');
});

test('localizeCartPath leaves non-cart canonical paths unchanged', () => {
	assert.strictEqual(
		localizeCartPath('/blyt/sdk/lua/foo.lua', '/ws'),
		'/blyt/sdk/lua/foo.lua',
	);
});

test('localizeCartPath leaves an already-local absolute path unchanged', () => {
	assert.strictEqual(
		localizeCartPath('/home/me/proj/main.lua', '/ws'),
		'/home/me/proj/main.lua',
	);
});

test('localizeCartPath does not match a prefix that is only a substring', () => {
	/* "/blyt/cartoon" must not be treated as the "/blyt/cart" prefix. */
	assert.strictEqual(
		localizeCartPath('/blyt/cartoon/x.lua', '/ws'),
		'/blyt/cartoon/x.lua',
	);
});
