/* Unit tests for BlytGdbDapProxy's .lua breakpoint short-circuit.
 *
 * Regression coverage for the WASM hybrid-cart debugging fix: lldb-dap has no
 * Lua runtime knowledge and always reports .lua breakpoints as unverified.
 * VS Code merges breakpoint state across the lldb session and the companion
 * Lua DAP session and shows the worst case, so lldb's "unverified" was
 * overriding the companion session's verified Lua BPs — making them look
 * ignored.  The proxy now answers `.lua` setBreakpoints itself with
 * verified:true and does NOT forward them to lldb.
 *
 * This pins the proxy's *output contract* (verified response, no forward).
 * The visible gutter-merge behavior is internal to VS Code and can only be
 * confirmed by a manual retest there; this test guards the lever the fix pulls.
 *
 * extension.js does `require('vscode')` and `require('node:child_process')` at
 * module load.  We stub both via a Module._load shim before requiring it so
 * the proxy can be constructed and driven under plain node:
 *
 *   node --test tools/vscode/test/
 */

const Module = require('node:module');
const test = require('node:test');
const assert = require('node:assert');
const { EventEmitter } = require('node:events');

/* Records of everything written to the fake lldb-dap process's stdin, so a
 * test can assert whether a request was forwarded.  Reset per construction. */
let lldbStdinWrites = [];

/* Minimal vscode.EventEmitter: `.event` registers a listener and is detached
 * from the instance by the proxy (`this.onDidSendMessage = emitter.event`), so
 * it must stay bound — an arrow class field does that. */
class FakeVsEmitter {
	constructor() {
		this._handlers = [];
	}
	event = (handler) => {
		this._handlers.push(handler);
		return { dispose: () => {} };
	};
	fire(msg) {
		for (const h of this._handlers) h(msg);
	}
}

const origLoad = Module._load;
Module._load = function (request, ...rest) {
	if (request === 'vscode') return { EventEmitter: FakeVsEmitter };
	if (request === 'node:child_process') {
		return {
			spawn() {
				lldbStdinWrites = [];
				const proc = new EventEmitter();
				proc.stdout = new EventEmitter();
				proc.stderr = new EventEmitter();
				proc.stdin = {
					write: (chunk) => {
						lldbStdinWrites.push(chunk.toString());
						return true;
					},
				};
				proc.kill = () => {};
				return proc;
			},
		};
	}
	return origLoad.call(this, request, ...rest);
};

const { _test } = require('../extension.js');
const { BlytGdbDapProxy } = _test;

/* Construct a proxy and collect every message it emits via onDidSendMessage. */
function makeProxy() {
	const proxy = new BlytGdbDapProxy('/nonexistent/lldb-dap');
	const emitted = [];
	proxy.onDidSendMessage((msg) => emitted.push(msg));
	return { proxy, emitted };
}

function setBreakpointsRequest(seq, source, lines) {
	return {
		type: 'request',
		command: 'setBreakpoints',
		seq,
		arguments: { source, breakpoints: lines.map((line) => ({ line })) },
	};
}

/* Flush the microtask/immediate queue: _setBreakpoints is async. */
const tick = () => new Promise((r) => setImmediate(r));

test('.lua setBreakpoints is answered verified and not forwarded to lldb', async () => {
	const { proxy, emitted } = makeProxy();

	proxy.handleMessage(
		setBreakpointsRequest(42, { path: '/proj/src/main.lua' }, [10, 20]),
	);
	await tick();

	assert.strictEqual(
		lldbStdinWrites.length,
		0,
		'.lua breakpoints must NOT be forwarded to lldb-dap',
	);
	assert.strictEqual(emitted.length, 1, 'exactly one response expected');
	const resp = emitted[0];
	assert.strictEqual(resp.type, 'response');
	assert.strictEqual(resp.command, 'setBreakpoints');
	assert.strictEqual(resp.success, true);
	assert.strictEqual(resp.request_seq, 42, 'response echoes the request seq');
	assert.deepStrictEqual(resp.body.breakpoints, [
		{ id: 10, verified: true, line: 10 },
		{ id: 20, verified: true, line: 20 },
	]);
});

test('.lua short-circuit also triggers on source.name when path is absent', async () => {
	const { proxy, emitted } = makeProxy();

	proxy.handleMessage(setBreakpointsRequest(7, { name: 'main.lua' }, [3]));
	await tick();

	assert.strictEqual(lldbStdinWrites.length, 0);
	assert.strictEqual(emitted.length, 1);
	assert.strictEqual(emitted[0].success, true);
	assert.deepStrictEqual(emitted[0].body.breakpoints, [
		{ id: 3, verified: true, line: 3 },
	]);
});

test('.lua short-circuit with no breakpoints returns an empty verified set', async () => {
	const { proxy, emitted } = makeProxy();

	proxy.handleMessage(setBreakpointsRequest(1, { path: '/p/a.lua' }, []));
	await tick();

	assert.strictEqual(lldbStdinWrites.length, 0);
	assert.strictEqual(emitted.length, 1);
	assert.deepStrictEqual(emitted[0].body.breakpoints, []);
});

test('non-.lua setBreakpoints is forwarded to lldb, not fabricated', async () => {
	const { proxy, emitted } = makeProxy();

	proxy.handleMessage(
		setBreakpointsRequest(99, { path: '/proj/src/main.c' }, [5]),
	);
	await tick();

	/* Forwarded to lldb (the proxy clears then re-adds, so at least one write),
	 * and the proxy does NOT fabricate a verified response — the real response
	 * only arrives once lldb replies, which the fake never does. */
	assert.ok(
		lldbStdinWrites.length > 0,
		'.c breakpoints must be forwarded to lldb-dap',
	);
	assert.ok(
		lldbStdinWrites.some((w) => w.includes('"command":"setBreakpoints"')),
		'forwarded payload should be a setBreakpoints request',
	);
	assert.strictEqual(
		emitted.length,
		0,
		'no response is fabricated for non-.lua sources',
	);
});

/* ── reload-window auto-continue (issue #119) ─────────────────────────────────
 * A reload's two-phase solib swap surfaces to lldb as reason "exception" /
 * "signal SIGTRAP" stops.  Within the reload window the proxy must auto-continue
 * these transparently; real user breakpoints, and any stop after the window,
 * must pass through.  shouldAutoContinueStop is the pure decision the proxy uses
 * in _drain. */

const stoppedEv = (body) => ({ type: 'event', event: 'stopped', body });

test('exception/SIGTRAP stop within the reload window is auto-continued', () => {
	const until = 1000;
	assert.strictEqual(
		BlytGdbDapProxy.shouldAutoContinueStop(
			stoppedEv({ reason: 'exception', description: 'signal SIGTRAP' }),
			500,
			until,
		),
		true,
	);
	/* reason "exception" alone (no description) also counts. */
	assert.strictEqual(
		BlytGdbDapProxy.shouldAutoContinueStop(
			stoppedEv({ reason: 'exception' }),
			500,
			until,
		),
		true,
	);
});

test('a real breakpoint stop within the window passes through', () => {
	assert.strictEqual(
		BlytGdbDapProxy.shouldAutoContinueStop(
			stoppedEv({ reason: 'breakpoint' }),
			500,
			1000,
		),
		false,
	);
});

test('an exception stop after the window passes through', () => {
	assert.strictEqual(
		BlytGdbDapProxy.shouldAutoContinueStop(
			stoppedEv({ reason: 'exception', description: 'signal SIGTRAP' }),
			1500,
			1000,
		),
		false,
	);
	/* No reload yet (window = 0) → never auto-continue. */
	assert.strictEqual(
		BlytGdbDapProxy.shouldAutoContinueStop(
			stoppedEv({ reason: 'exception' }),
			1,
			0,
		),
		false,
	);
});

test('non-stopped events are never auto-continued', () => {
	assert.strictEqual(
		BlytGdbDapProxy.shouldAutoContinueStop(
			{ type: 'event', event: 'output', body: {} },
			500,
			1000,
		),
		false,
	);
});
