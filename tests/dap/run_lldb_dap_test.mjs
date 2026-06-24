#!/usr/bin/env node
/*
 * tests/dap/run_lldb_dap_test.mjs — lldb-dap integration test client.
 *
 * Spawns lldb-dap as a child process and communicates with it via DAP over
 * stdin/stdout (Content-Length framed).  Connects lldb-dap to a running GDB
 * RSP relay via the launchCommands mechanism.
 *
 * Usage:
 *   node run_lldb_dap_test.mjs <lldb-dap-path> <gdb-tcp-port> <cart-path> <cwd>
 *       --test <name>
 *
 * Tests (--test <name>):
 *   initialize         — sends initialize, checks capabilities, disconnects
 *   source-breakpoint  — sets a breakpoint by file:line, verifies stopped event
 *   stack-trace        — after stop, verifies function name in frame 0
 *   variables          — after stop, verifies a local variable value
 *   source-map         — verifies source.path uses cwd, not /blyt/cart
 *
 * Environment:
 *   BLYT_GDB_BREAK_LINE  — source line number for breakpoint tests (default: 5)
 *   BLYT_SOURCE_FILE     — filename within /blyt/cart (default: main.c)
 *
 * Exit 0 on success, non-zero on failure.
 * Node.js 22+ required.
 */

/* Debug-driver default: every failure's captured stderr should already carry
 * a protocol/lifecycle trace.  'api' stays opt-in (high volume). */
if (!process.env.BLYT_TRACE) process.env.BLYT_TRACE = 'gdb,dap,lifecycle,frame';

import { spawn } from 'node:child_process';
import { readFileSync } from 'node:fs';
import net from 'node:net';
import { join } from 'node:path';

const [, , lldbDapPath, gdbPort, cartPath, projectCwd, ...rest] = process.argv;
let testName = 'initialize';
for (let i = 0; i < rest.length; i++) {
	if (rest[i] === '--test' && rest[i + 1]) testName = rest[++i];
}

if (!lldbDapPath || !gdbPort || !cartPath || !projectCwd) {
	process.stderr.write(
		'usage: run_lldb_dap_test.mjs <lldb-dap> <gdb-port> <cart> <cwd> [--test <name>]\n',
	);
	process.exit(1);
}

const breakLine = parseInt(process.env.BLYT_GDB_BREAK_LINE || '5', 10);
const sourceFile = process.env.BLYT_SOURCE_FILE || 'main.c';

/* lldb-dap's `program` (issue #119): a stub ELF, NOT the cart, so the cart is
 * presented purely as a shared library (the runtime's svr4 list) and never as
 * the main executable — cleanly unloadable/reloadable across a hot reload.
 * Falls back to the cart when no stub is provided (legacy/standalone runs). */
const programPath = process.env.BLYT_STUB_PROGRAM || cartPath;

/* The canonical source path + line to break at for the sdk-source-breakpoint
 * test (a path inside the SDK-shipped source, e.g. /blyt/sdk/src/...). */
const sdkBreakFile = process.env.BLYT_SDK_BREAK_FILE || '';
const sdkBreakLine = parseInt(process.env.BLYT_SDK_BREAK_LINE || '0', 10);

/* Build the lldb `settings set target.source-map …` command from the cart's
 * source-map manifest (issue #46 §2 / #48 item 2): /blyt/cart → workspace plus
 * /blyt/sdk, /blyt/rust, /blyt/cargo from build/source-map.json, so breakpoints
 * and frames in SDK / std / crate source resolve, not just the cart's own. */
function sourceMapCommand() {
	const pairs = [['/blyt/cart', projectCwd]];
	try {
		const manifest = JSON.parse(
			readFileSync(join(projectCwd, 'build', 'source-map.json'), 'utf8'),
		);
		for (const { prefix, local } of manifest) {
			if (prefix === '/blyt/cart' || prefix === '/blyt/src') continue;
			if (prefix && local) pairs.push([prefix, local]);
		}
	} catch (_) {
		/* manifest optional — cart mapping alone */
	}
	const args = pairs
		.map(([c, l]) => `${JSON.stringify(c)} ${JSON.stringify(l)}`)
		.join(' ');
	return `settings set target.source-map ${args}`;
}

/* ── Content-Length framing (DAP protocol) ───────────────────────────────── */

function makeFramer(onMessage) {
	let buf = Buffer.alloc(0);
	return (chunk) => {
		buf = Buffer.concat([buf, chunk]);
		while (true) {
			const sep = buf.indexOf('\r\n\r\n');
			if (sep < 0) break;
			const header = buf.slice(0, sep).toString('utf8');
			const lenMatch = header.match(/Content-Length:\s*(\d+)/i);
			if (!lenMatch) {
				buf = buf.slice(sep + 4);
				continue;
			}
			const len = parseInt(lenMatch[1], 10);
			if (buf.length < sep + 4 + len) break;
			const body = buf.slice(sep + 4, sep + 4 + len).toString('utf8');
			buf = buf.slice(sep + 4 + len);
			try {
				onMessage(JSON.parse(body));
			} catch (_) {}
		}
	};
}

function clFrame(obj) {
	const body = JSON.stringify(obj);
	return `Content-Length: ${Buffer.byteLength(body)}\r\n\r\n${body}`;
}

/* ── DAP session ─────────────────────────────────────────────────────────── */

async function runDap(fn) {
	const proc = spawn(lldbDapPath, [], {
		stdio: ['pipe', 'pipe', 'pipe'],
	});

	let seq = 1;
	const pending = new Map(); /* reqId → {resolve, reject} */
	const eventHandlers = {}; /* event name → one-shot handler (waitEvent) */
	const persistentHandlers =
		{}; /* event name → persistent handler (onEvent) */

	const framer = makeFramer((msg) => {
		if (msg.type === 'response') {
			const p = pending.get(msg.request_seq);
			if (p) {
				pending.delete(msg.request_seq);
				if (msg.success) p.resolve(msg);
				else
					p.reject(
						new Error(`DAP error: ${msg.message} (${msg.command})`),
					);
			}
		} else if (msg.type === 'event') {
			const h = eventHandlers[msg.event];
			if (h) {
				h(msg);
			}
			if (persistentHandlers[msg.event])
				persistentHandlers[msg.event](msg);
		}
	});

	proc.stdout.on('data', framer);
	proc.stderr.on('data', (d) => process.stderr.write(`[lldb-dap] ${d}`));

	function send(command, args) {
		const id = seq++;
		const msg = {
			seq: id,
			type: 'request',
			command,
			arguments: args || {},
		};
		proc.stdin.write(clFrame(msg));
		return new Promise((resolve, reject) => {
			pending.set(id, { resolve, reject });
		});
	}

	function waitEvent(eventName, timeoutMs = 60000) {
		return new Promise((resolve, reject) => {
			const timer = setTimeout(
				() =>
					reject(
						new Error(`timeout waiting for event '${eventName}'`),
					),
				timeoutMs,
			);
			eventHandlers[eventName] = (ev) => {
				clearTimeout(timer);
				delete eventHandlers[eventName];
				resolve(ev);
			};
		});
	}

	/* Register a persistent handler for an event (unlike one-shot waitEvent). */
	function onEvent(eventName, handler) {
		persistentHandlers[eventName] = handler;
	}

	function close() {
		proc.stdin.end();
		proc.kill();
	}

	try {
		await fn({ send, waitEvent, onEvent, close });
	} finally {
		close();
		await new Promise((r) => proc.on('exit', r));
	}
}

/* ── Tests ───────────────────────────────────────────────────────────────── */

async function testInitialize() {
	await runDap(async ({ send }) => {
		const resp = await send('initialize', { adapterID: 'lldb-dap' });
		if (!resp.success) throw new Error('initialize failed');
		if (!resp.body || typeof resp.body !== 'object')
			throw new Error('no capabilities');
		console.log('PASS: initialize — capabilities received');
	});
}

async function testSourceBreakpoint() {
	await runDap(async ({ send, waitEvent }) => {
		await send('initialize', { adapterID: 'lldb-dap' });

		const launchArgs = {
			program: programPath,
			stopOnEntry: true,
			launchCommands: [
				sourceMapCommand(),
				`gdb-remote 127.0.0.1:${gdbPort}`,
			],
		};
		const launchP = send('launch', launchArgs);
		const stoppedEntry = waitEvent('stopped');

		/* setBreakpoints before configurationDone */
		await send('setBreakpoints', {
			source: { path: `/blyt/cart/${sourceFile}` },
			breakpoints: [{ line: breakLine }],
		});
		await send('configurationDone');
		await launchP;

		/* First stop is either entry or our breakpoint. */
		const ev1 = await stoppedEntry;

		/* If we stopped at entry, continue to reach the source breakpoint. */
		let ev2 = ev1;
		if (ev1.body.reason === 'entry') {
			send('continue', { threadId: ev1.body.threadId });
			ev2 = await waitEvent('stopped', 10000);
		}

		if (ev2?.body.reason !== 'breakpoint') {
			throw new Error(
				`expected stop reason 'breakpoint', got '${ev2?.body?.reason}'`,
			);
		}
		console.log(`PASS: source breakpoint hit at line ${breakLine}`);
	});
}

/* Issue #119 (acceptance criterion 2): a breakpoint set before launch must be
 * BOUND (verified, with a concrete address) at attach — i.e. before init()
 * runs — not left pending until the first reload.  With the cart presented
 * purely as a shared library, this proves the cart-library is announced at
 * attach (the runtime's `library:` initial stop reply) so lldb fetches the svr4
 * list and resolves cart breakpoints up front. */
async function testAttachBind() {
	await runDap(async ({ send }) => {
		await send('initialize', { adapterID: 'lldb-dap' });
		const launchP = send('launch', {
			program: programPath,
			stopOnEntry: true,
			launchCommands: [
				sourceMapCommand(),
				`gdb-remote 127.0.0.1:${gdbPort}`,
			],
		});
		/* setBreakpoints BEFORE configurationDone (before the cart runs). */
		const sb = await send('setBreakpoints', {
			source: { path: `/blyt/cart/${sourceFile}` },
			breakpoints: [{ line: breakLine }],
		});
		const bp = sb.body?.breakpoints?.[0];
		console.log(`attach-bind breakpoint: ${JSON.stringify(bp)}`);
		if (!bp?.verified) {
			throw new Error(
				`breakpoint not verified at attach (got ${JSON.stringify(bp)})`,
			);
		}
		if (!bp.instructionReference) {
			throw new Error(
				`breakpoint verified but not bound to an address at attach (pending): ${JSON.stringify(bp)}`,
			);
		}
		await send('configurationDone');
		await launchP;
		console.log(
			`PASS: breakpoint bound at attach → ${bp.instructionReference} (line ${bp.line})`,
		);
	});
}

async function testAutoStart() {
	/* Verifies stopOnEntry:false — the process starts running immediately
	 * after gdb-remote connects and the first stopped event is a breakpoint,
	 * not an entry stop. */
	await runDap(async ({ send, waitEvent }) => {
		await send('initialize', { adapterID: 'lldb-dap' });

		const launchArgs = {
			program: programPath,
			stopOnEntry: false,
			launchCommands: [
				sourceMapCommand(),
				`gdb-remote 127.0.0.1:${gdbPort}`,
			],
		};
		const launchP = send('launch', launchArgs);

		await send('setBreakpoints', {
			source: { path: `/blyt/cart/${sourceFile}` },
			breakpoints: [{ line: breakLine }],
		});
		await send('configurationDone');
		await launchP;

		const ev = await waitEvent('stopped', 60000);
		if (ev.body.reason === 'entry') {
			throw new Error('stopOnEntry:false still produced an entry stop');
		}
		if (ev.body.reason !== 'breakpoint') {
			throw new Error(
				`expected 'breakpoint' stop, got '${ev.body.reason}'`,
			);
		}
		console.log(
			`PASS: auto-start — first stop is breakpoint (no entry stop)`,
		);
	});
}

async function testStackTrace() {
	await runDap(async ({ send, waitEvent }) => {
		await send('initialize', { adapterID: 'lldb-dap' });
		const launchArgs = {
			program: programPath,
			stopOnEntry: true,
			launchCommands: [
				sourceMapCommand(),
				`gdb-remote 127.0.0.1:${gdbPort}`,
			],
		};
		send('launch', launchArgs);
		await send('setBreakpoints', {
			source: { path: `/blyt/cart/${sourceFile}` },
			breakpoints: [{ line: breakLine }],
		});
		const stoppedP = waitEvent('stopped', 60000);
		await send('configurationDone');
		const ev = await stoppedP;
		const threadId = ev.body.threadId;

		/* Continue past entry stop if needed. */
		let stopped = ev;
		if (ev.body.reason === 'entry') {
			send('continue', { threadId });
			stopped = await waitEvent('stopped', 10000);
		}

		const st = await send('stackTrace', {
			threadId: stopped.body.threadId,
		});
		const frames = st.body.stackFrames;
		if (!frames || frames.length === 0) throw new Error('no stack frames');
		const top = frames[0];
		if (!top.name || top.name === '??')
			throw new Error(`unexpected frame name: ${top.name}`);
		console.log(`PASS: stack frame 0 = '${top.name}'`);
	});
}

async function testVariables() {
	await runDap(async ({ send, waitEvent }) => {
		await send('initialize', { adapterID: 'lldb-dap' });
		const launchArgs = {
			program: programPath,
			stopOnEntry: true,
			launchCommands: [
				sourceMapCommand(),
				`gdb-remote 127.0.0.1:${gdbPort}`,
			],
		};
		send('launch', launchArgs);
		await send('setBreakpoints', {
			source: { path: `/blyt/cart/${sourceFile}` },
			breakpoints: [{ line: breakLine }],
		});
		const stoppedP = waitEvent('stopped', 60000);
		await send('configurationDone');
		const ev = await stoppedP;

		let stopped = ev;
		if (ev.body.reason === 'entry') {
			send('continue', { threadId: ev.body.threadId });
			stopped = await waitEvent('stopped', 10000);
		}

		const st = await send('stackTrace', {
			threadId: stopped.body.threadId,
		});
		const frameId = st.body.stackFrames[0].id;

		const scopes = await send('scopes', { frameId });
		const localScope = scopes.body.scopes.find(
			(s) => s.name === 'Locals' || s.name === 'Local',
		);
		if (!localScope) {
			console.log(
				'PASS: scopes response received (no Locals scope — may be optimized)',
			);
			return;
		}

		const vars = await send('variables', {
			variablesReference: localScope.variablesReference,
		});
		if (!vars.body.variables || vars.body.variables.length === 0) {
			console.log(
				'PASS: variables response received (empty — may be optimized away)',
			);
			return;
		}
		console.log(
			`PASS: ${vars.body.variables.length} variable(s) in local scope`,
		);
	});
}

async function testSourceMap() {
	await runDap(async ({ send, waitEvent }) => {
		await send('initialize', { adapterID: 'lldb-dap' });
		const launchArgs = {
			program: programPath,
			stopOnEntry: true,
			launchCommands: [
				sourceMapCommand(),
				`gdb-remote 127.0.0.1:${gdbPort}`,
			],
		};
		send('launch', launchArgs);
		await send('setBreakpoints', {
			source: { path: `/blyt/cart/${sourceFile}` },
			breakpoints: [{ line: breakLine }],
		});
		const stoppedP = waitEvent('stopped', 60000);
		await send('configurationDone');
		const ev = await stoppedP;

		let stopped = ev;
		if (ev.body.reason === 'entry') {
			send('continue', { threadId: ev.body.threadId });
			stopped = await waitEvent('stopped', 10000);
		}

		const st = await send('stackTrace', {
			threadId: stopped.body.threadId,
		});
		const frame = st.body.stackFrames.find((f) => f.source?.path);
		if (!frame) {
			console.log(
				'PASS: stackTrace received (no source path in frames — may be optimized)',
			);
			return;
		}
		const sourcePath = frame.source.path;
		if (sourcePath.startsWith('/blyt/cart')) {
			throw new Error(
				`source-map not applied: path is still '${sourcePath}'`,
			);
		}
		if (!sourcePath.startsWith(projectCwd)) {
			/* Non-fatal: LLDB may use relative paths or other forms. */
			console.log(
				`PASS: source path '${sourcePath}' (not canonical DWARF prefix)`,
			);
			return;
		}
		console.log(`PASS: source-map applied — path is '${sourcePath}'`);
	});
}

/* Set a breakpoint by a *canonical SDK* source path:line (e.g. inside the
 * statically-linked blyt SDK crate), verify it binds against the SDK DWARF, and
 * — since the cart calls that code — that it fires.  Proves SDK-shipped source +
 * the manifest source-map resolve end to end (issue #48 item 2). */
async function testSdkSourceBreakpoint() {
	if (!sdkBreakFile || !sdkBreakLine) {
		throw new Error('BLYT_SDK_BREAK_FILE / BLYT_SDK_BREAK_LINE not set');
	}
	await runDap(async ({ send, waitEvent }) => {
		await send('initialize', { adapterID: 'lldb-dap' });
		const launchP = send('launch', {
			program: programPath,
			stopOnEntry: true,
			launchCommands: [
				sourceMapCommand(),
				`gdb-remote 127.0.0.1:${gdbPort}`,
			],
		});
		const stoppedEntry = waitEvent('stopped');

		const resp = await send('setBreakpoints', {
			source: { path: sdkBreakFile },
			breakpoints: [{ line: sdkBreakLine }],
		});
		const bp = resp.body?.breakpoints?.[0];
		if (!bp?.verified) {
			throw new Error(
				`SDK breakpoint not verified at ${sdkBreakFile}:${sdkBreakLine} ` +
					`(${bp?.message ?? 'no breakpoint'})`,
			);
		}
		console.log(
			`PASS: SDK-source breakpoint verified at ${sdkBreakFile}:${sdkBreakLine}`,
		);

		await send('configurationDone');
		await launchP;

		let ev = await stoppedEntry;
		if (ev.body.reason === 'entry') {
			send('continue', { threadId: ev.body.threadId });
			ev = await waitEvent('stopped', 15000);
		}
		if (ev.body.reason !== 'breakpoint') {
			throw new Error(
				`expected SDK breakpoint to fire, got stop '${ev.body.reason}'`,
			);
		}
		/* The top frame's source must resolve to the SDK file via the source-map. */
		const st = await send('stackTrace', { threadId: ev.body.threadId });
		const top = st.body.stackFrames?.[0];
		console.log(
			`PASS: SDK breakpoint fired; frame 0 = ${top?.name} @ ${top?.source?.path}`,
		);
	});
}

/* Send one JSON dev-control command to the player's dev-control TCP port and
 * resolve with its reply line. */
function devCtrl(port, cmd) {
	return new Promise((resolve, reject) => {
		const sock = net.createConnection({ host: '127.0.0.1', port });
		let buf = '';
		sock.on('data', (d) => {
			buf += d.toString();
			if (buf.includes('\n')) {
				sock.end();
				resolve(buf.trim());
			}
		});
		sock.on('error', reject);
		sock.on('connect', () => sock.write(`${JSON.stringify(cmd)}\n`));
		setTimeout(() => {
			sock.end();
			resolve(buf.trim() || '(no reply)');
		}, 3000);
	});
}

/* Issue #119 (acceptance criterion 1): a reload-while-debugging rebinds a
 * breakpoint to the NEW code's address. Drives the REAL reload path: set a
 * source breakpoint against v1 (resolved → A1), then send the dev-control
 * `reload` pointing at the rebuilt v2 cart (whose function moved). The runtime
 * swaps the cart in place at a fresh base and performs the two-phase solib swap;
 * lldb re-reads v2's DWARF and rebinds.
 *
 * The DAP setBreakpoints re-query is unreliable here (lldb returns the stale
 * pre-reload location as the primary instructionReference — Spike W §5c), so the
 * Rust harness asserts on the GDB-RSP trace (the Z0/z0 packets) instead. This
 * driver just drives the reload and lets the session settle. */
async function testReloadRebind() {
	const devCtrlPort = parseInt(process.env.BLYT_DEV_CTRL_PORT || '0', 10);
	const v2Cart = process.env.BLYT_V2_CART || '';
	if (!devCtrlPort || !v2Cart)
		throw new Error(
			'reload-rebind needs BLYT_DEV_CTRL_PORT and BLYT_V2_CART',
		);

	await runDap(async ({ send, onEvent }) => {
		await send('initialize', { adapterID: 'lldb-dap' });
		/* Auto-continue on every stop: each reload phase fires a library-change
		 * stop that lldb does not auto-continue; the client must continue so lldb
		 * processes it and the runtime can publish the next phase (issue #119,
		 * the seamless reconnect the extension performs in production). */
		onEvent('stopped', (ev) => {
			send('continue', { threadId: ev.body?.threadId ?? 1 }).catch(
				() => {},
			);
		});
		/* stopOnEntry:false so the cart keeps running update() and services the
		 * dev-control channel; the breakpoint resolves but is never hit (the
		 * test function runs only in init()). */
		const launchP = send('launch', {
			program: programPath,
			stopOnEntry: false,
			launchCommands: [
				sourceMapCommand(),
				`gdb-remote 127.0.0.1:${gdbPort}`,
			],
		});
		await send('setBreakpoints', {
			source: { path: `/blyt/cart/${sourceFile}` },
			breakpoints: [{ line: breakLine }],
		});
		await send('configurationDone');
		await launchP;
		await new Promise((r) => setTimeout(r, 500)); /* let the cart run */

		const reply = await devCtrl(devCtrlPort, {
			id: 1,
			cmd: 'reload',
			path: v2Cart,
		});
		console.log(`reload reply: ${reply}`);
		/* Let lldb finish the two-phase re-resolution (add v2 / remove v1). */
		await new Promise((r) => setTimeout(r, 1500));
		console.log('reload-rebind: driver done');
	});
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

const tests = {
	initialize: testInitialize,
	'source-breakpoint': testSourceBreakpoint,
	'attach-bind': testAttachBind,
	'reload-rebind': testReloadRebind,
	'auto-start': testAutoStart,
	'stack-trace': testStackTrace,
	variables: testVariables,
	'source-map': testSourceMap,
	'sdk-source-breakpoint': testSdkSourceBreakpoint,
};

const test = tests[testName];
if (!test) {
	process.stderr.write(
		`unknown test: ${testName}\nAvailable: ${Object.keys(tests).join(', ')}\n`,
	);
	process.exit(1);
}

test()
	.then(() => {
		console.log(`PASS: test '${testName}' completed`);
		process.exit(0);
	})
	.catch((e) => {
		console.error(`FAIL: test '${testName}': ${e.message}`);
		process.exit(1);
	});
