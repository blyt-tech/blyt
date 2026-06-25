/* Unit tests for the pure debug-config helpers in extension.js:
 *   - sdkDir(): blyt.sdkDir setting → BLYT_SDK_DIR env → empty, all trimmed.
 *   - traceChannels(): the --trace value, with its default.
 *   - debugCartPath(): the release `.blyt` → debug `.dbg.blyt` rewrite used
 *     when launching under blytdebug / `blyt debug` (ADR-0129).
 * The integrated launch behaviour is covered by the headless debugger suite
 * (test/integration); these pin the pure decision logic directly.
 *
 * extension.js calls vscode.workspace.getConfiguration('blyt').get(...) inside
 * sdkDir/traceChannels, so the stub here serves config values from a mutable
 * `settings` map the tests control. */

const Module = require('node:module');
const test = require('node:test');
const assert = require('node:assert');
const path = require('node:path');

/* Mutable config backing the stubbed getConfiguration('blyt'). */
let settings = {};

const origLoad = Module._load;
Module._load = function (request, ...rest) {
	if (request === 'vscode')
		return {
			window: { activeTextEditor: undefined },
			workspace: {
				getConfiguration: () => ({
					get: (key, def) => (key in settings ? settings[key] : def),
				}),
			},
		};
	return origLoad.call(this, request, ...rest);
};

const { _test } = require('../extension.js');
const { sdkDir, traceChannels, debugCartPath, debugStubPath } = _test;

/* Reset config + the env var each helper reads to a known clean state. */
function reset() {
	settings = {};
	delete process.env.BLYT_SDK_DIR;
}

/* ── sdkDir ───────────────────────────────────────────────────────────────── */

test('sdkDir prefers the blyt.sdkDir setting over the env var', () => {
	reset();
	settings.sdkDir = '/from/setting';
	process.env.BLYT_SDK_DIR = '/from/env';
	assert.strictEqual(sdkDir(), '/from/setting');
});

test('sdkDir falls back to BLYT_SDK_DIR when the setting is blank', () => {
	reset();
	settings.sdkDir = '   ';
	process.env.BLYT_SDK_DIR = '/from/env';
	assert.strictEqual(sdkDir(), '/from/env');
});

test('sdkDir returns empty when neither setting nor env is set', () => {
	reset();
	assert.strictEqual(sdkDir(), '');
});

test('sdkDir trims surrounding whitespace from the setting', () => {
	reset();
	settings.sdkDir = '  /padded/setting  ';
	assert.strictEqual(sdkDir(), '/padded/setting');
});

test('sdkDir trims surrounding whitespace from the env var', () => {
	reset();
	process.env.BLYT_SDK_DIR = '  /padded/env  ';
	assert.strictEqual(sdkDir(), '/padded/env');
});

/* ── traceChannels ────────────────────────────────────────────────────────── */

test('traceChannels returns the configured value, trimmed', () => {
	reset();
	settings.traceChannels = '  gdb,dap  ';
	assert.strictEqual(traceChannels(), 'gdb,dap');
});

test('traceChannels returns the default when unset', () => {
	reset();
	assert.strictEqual(traceChannels(), 'gdb,dap,lifecycle,frame');
});

test('traceChannels can be emptied to disable tracing', () => {
	reset();
	settings.traceChannels = '';
	assert.strictEqual(traceChannels(), '');
});

/* ── debugCartPath ────────────────────────────────────────────────────────── */

test('debugCartPath rewrites a release cart to its debug variant', () => {
	assert.strictEqual(debugCartPath('hello.blyt'), 'hello.dbg.blyt');
});

test('debugCartPath preserves the directory and only rewrites the suffix', () => {
	assert.strictEqual(
		debugCartPath(path.join('proj', 'build', 'dap_retest.blyt')),
		path.join('proj', 'build', 'dap_retest.dbg.blyt'),
	);
});

test('debugCartPath only touches a trailing .blyt extension', () => {
	/* A ".blyt" earlier in the path (e.g. a dir name) must not be rewritten. */
	assert.strictEqual(
		debugCartPath(path.join('a.blyt', 'hello.blyt')),
		path.join('a.blyt', 'hello.dbg.blyt'),
	);
});

/* ── debugStubPath (issue #119) ───────────────────────────────────────────── */

test('debugStubPath points at the SDK debug stub ELF', () => {
	reset();
	settings.sdkDir = '/sdk';
	assert.strictEqual(
		debugStubPath(),
		path.join('/sdk', 'lib', 'debug', 'blyt-debug-stub.elf'),
	);
});

test('debugStubPath returns empty when the SDK is not configured', () => {
	reset();
	assert.strictEqual(debugStubPath(), '');
});
