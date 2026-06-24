#!/usr/bin/env node
/*
 * tests/dap/run_sdl_hybrid_reload_test.mjs — dual-client hybrid
 * reload-while-debugging driver (issue #119, acceptance criteria 4 + 5).
 *
 * Drives ONE hybrid debug session with BOTH debug views live at once:
 *   - native (C):  lldb-dap (spawned as a child, DAP over stdin/stdout) attached
 *                  to the player's GDB RSP relay; `program` is the stub ELF so the
 *                  cart is a cleanly reloadable shared library (issue #119).
 *   - Lua:         a raw DAP client over TCP (the companion Lua DAP session).
 *
 * Both views set a breakpoint inside init():
 *   - Lua:    a source line in init() (BLYT_LUA_BREAK_LINE).
 *   - native: a source line inside the Lua-exported C function that init() calls
 *             (BLYT_NATIVE_BREAK_LINE in BLYT_NATIVE_SOURCE_FILE).
 *
 * The cart's init() runs once at startup (both breakpoints fire) and again after
 * a hot reload.  We drive a REAL dev-control `reload` (pointing at the rebuilt v2
 * cart, whose native function moved) and assert that AFTER the reload BOTH the
 * Lua and native init() breakpoints fire again — proving the post-reload gate
 * armed both views before init() ran (criterion 4) — and that neither client was
 * torn down / reconnected across the reload (criterion 5).
 *
 * The native view auto-continues lldb's library-change stops transparently: the
 * two-phase solib swap surfaces as reason "exception" (signal SIGTRAP) stops that
 * are NOT real user breakpoints; only reason "breakpoint" stops are init()-bp
 * hits.  This mirrors the reload-window auto-continue the VS Code extension's
 * BlytGdbDapProxy performs in production.
 *
 * Usage:
 *   node run_sdl_hybrid_reload_test.mjs \
 *       <lldb-dap> <gdb-port> <dap-port> <dev-ctrl-port> <v1-cart> <cwd> <v2-cart>
 *
 * Environment:
 *   BLYT_NATIVE_BREAK_LINE   native source line for the C breakpoint
 *   BLYT_NATIVE_SOURCE_FILE  project-relative C file (e.g. src/game/c/main.c)
 *   BLYT_LUA_BREAK_LINE      Lua source line for the init() breakpoint
 *   BLYT_LUA_SOURCE          canonical Lua path (e.g. /blyt/cart/src/game/lua/main.lua)
 *   BLYT_STUB_PROGRAM        lldb-dap `program` stub ELF (issue #119)
 *
 * Exit 0 on success, non-zero on failure.  Node.js 22+ required.
 */

if (!process.env.BLYT_TRACE) process.env.BLYT_TRACE = 'gdb,dap,lifecycle,frame';

import { spawn } from 'node:child_process';
import { readFileSync } from 'node:fs';
import net from 'node:net';
import { join } from 'node:path';

const [
	,
	,
	lldbDapPath,
	gdbPort,
	dapPort,
	devCtrlPort,
	v1Cart,
	projectCwd,
	v2Cart,
] = process.argv;

if (
	!lldbDapPath ||
	!gdbPort ||
	!dapPort ||
	!devCtrlPort ||
	!v1Cart ||
	!projectCwd ||
	!v2Cart
) {
	process.stderr.write(
		'usage: run_sdl_hybrid_reload_test.mjs <lldb-dap> <gdb-port> ' +
			'<dap-port> <dev-ctrl-port> <v1-cart> <cwd> <v2-cart>\n',
	);
	process.exit(1);
}

const nativeBreakLine = parseInt(process.env.BLYT_NATIVE_BREAK_LINE || '0', 10);
const nativeSourceFile =
	process.env.BLYT_NATIVE_SOURCE_FILE || 'src/game/c/main.c';
const luaBreakLine = parseInt(process.env.BLYT_LUA_BREAK_LINE || '0', 10);
const luaSource =
	process.env.BLYT_LUA_SOURCE || '/blyt/cart/src/game/lua/main.lua';
const programPath = process.env.BLYT_STUB_PROGRAM || v1Cart;

if (!nativeBreakLine || !luaBreakLine) {
	process.stderr.write(
		'need BLYT_NATIVE_BREAK_LINE and BLYT_LUA_BREAK_LINE\n',
	);
	process.exit(1);
}

/* ── assert ──────────────────────────────────────────────────────────────── */

let passed = 0;
let failed = 0;
function assert(cond, msg) {
	if (cond) {
		console.log(`  PASS: ${msg}`);
		passed++;
	} else {
		console.error(`  FAIL: ${msg}`);
		failed++;
	}
}

function sleep(ms) {
	return new Promise((r) => setTimeout(r, ms));
}

async function waitFor(pred, timeoutMs, desc) {
	const deadline = Date.now() + timeoutMs;
	while (Date.now() < deadline) {
		if (pred()) return true;
		await sleep(50);
	}
	throw new Error(`timeout waiting for ${desc}`);
}

/* The lldb `settings set target.source-map …` command from the cart manifest
 * (same as run_lldb_dap_test.mjs) so the C breakpoint resolves by source line. */
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
		/* manifest optional */
	}
	const args = pairs
		.map(([c, l]) => `${JSON.stringify(c)} ${JSON.stringify(l)}`)
		.join(' ');
	return `settings set target.source-map ${args}`;
}

/* ── Content-Length DAP framing (shared) ─────────────────────────────────── */

function clFrame(obj) {
	const body = JSON.stringify(obj);
	return `Content-Length: ${Buffer.byteLength(body)}\r\n\r\n${body}`;
}

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

/* A DAP endpoint (request/response + persistent event handlers) over an
 * arbitrary write/onData transport.  Used for both the lldb-dap child (stdio)
 * and the Lua DAP server (TCP). */
function makeDapEndpoint(write, label) {
	let seq = 1;
	const pending = new Map();
	const eventHandlers = {};
	const onData = makeFramer((msg) => {
		if (msg.type === 'response') {
			const p = pending.get(msg.request_seq);
			if (p) {
				pending.delete(msg.request_seq);
				if (msg.success) p.resolve(msg);
				else
					p.reject(
						new Error(`[${label}] ${msg.command}: ${msg.message}`),
					);
			}
		} else if (msg.type === 'event') {
			const h = eventHandlers[msg.event];
			if (h) h(msg);
		}
	});
	function request(command, args) {
		const id = seq++;
		write(
			clFrame({
				seq: id,
				type: 'request',
				command,
				arguments: args || {},
			}),
		);
		return new Promise((resolve, reject) => {
			const timer = setTimeout(
				() => reject(new Error(`[${label}] timeout: ${command}`)),
				60000,
			);
			pending.set(id, {
				resolve: (m) => {
					clearTimeout(timer);
					resolve(m);
				},
				reject: (e) => {
					clearTimeout(timer);
					reject(e);
				},
			});
		});
	}
	function onEvent(name, handler) {
		eventHandlers[name] = handler;
	}
	return { request, onEvent, onData };
}

/* ── Main ────────────────────────────────────────────────────────────────── */

/* Tracks breakpoint hits per phase for one view. */
function makeTracker() {
	return { startup: 0, reloaded: 0 };
}
const nativeHits = makeTracker();
const luaHits = makeTracker();
let phase = 'startup';

async function main() {
	/* ── native: lldb-dap child over stdio ────────────────────────────────── */
	const lldb = spawn(lldbDapPath, [], { stdio: ['pipe', 'pipe', 'pipe'] });
	lldb.stderr.on('data', (d) => process.stderr.write(`[lldb-dap] ${d}`));
	const native = makeDapEndpoint((s) => lldb.stdin.write(s), 'native');
	lldb.stdout.on('data', native.onData);

	/* native stop handler: real breakpoint hits get recorded; library-change
	 * (exception/SIGTRAP) stops and entry stops are auto-continued — exactly the
	 * reload-window behaviour the extension's BlytGdbDapProxy performs. */
	native.onEvent('stopped', (ev) => {
		const reason = ev.body?.reason;
		const tid = ev.body?.threadId ?? 1;
		/* Exactly one native breakpoint is ever set, so a reason:"breakpoint"
		 * stop IS that init() breakpoint.  Other stops (entry, or the
		 * exception/SIGTRAP library-change stops from the two-phase solib swap)
		 * are auto-continued transparently — the reload-window behaviour the
		 * extension's BlytGdbDapProxy performs in production. */
		if (reason === 'breakpoint') {
			nativeHits[phase]++;
			console.log(`[native] breakpoint hit (phase=${phase})`);
		} else {
			console.log(
				`[native] auto-continue stop reason=${reason} (phase=${phase})`,
			);
		}
		native.request('continue', { threadId: tid }).catch(() => {});
	});

	/* ── Lua: raw DAP over TCP ─────────────────────────────────────────────── */
	const luaSock = net.createConnection(parseInt(dapPort, 10), '127.0.0.1');
	const lua = makeDapEndpoint((s) => luaSock.write(s), 'lua');
	luaSock.on('data', lua.onData);
	await new Promise((res, rej) => {
		luaSock.once('connect', res);
		luaSock.once('error', rej);
	});

	lua.onEvent('stopped', (ev) => {
		const reason = ev.body?.reason;
		const tid = ev.body?.threadId ?? 1;
		/* The only Lua breakpoint is the init() one; a breakpoint/step stop is
		 * that hit. */
		if (reason === 'breakpoint' || reason === 'step') luaHits[phase]++;
		console.log(`[lua] stop reason=${reason} (phase=${phase})`);
		lua.request('continue', { threadId: tid }).catch(() => {});
	});

	/* ── native: initialize, launch, setBreakpoints, configurationDone ──────── */
	await native.request('initialize', { adapterID: 'lldb-dap' });
	const nativeLaunchP = native
		.request('launch', {
			program: programPath,
			stopOnEntry: false,
			launchCommands: [
				sourceMapCommand(),
				`gdb-remote 127.0.0.1:${gdbPort}`,
			],
		})
		.catch((e) => console.log(`[native] launch error: ${e.message}`));
	const nbp = await native.request('setBreakpoints', {
		source: { path: `/blyt/cart/${nativeSourceFile}` },
		breakpoints: [{ line: nativeBreakLine }],
	});
	assert(
		nbp.body?.breakpoints?.[0]?.verified === true,
		`native breakpoint verified at attach (${nativeSourceFile}:${nativeBreakLine})`,
	);
	/* native configurationDone first: lldb attaches + finishes its launch
	 * handshake (and continues) so it is fully ready to service the native
	 * breakpoint before init() runs once the Lua side releases the gate. */
	await native.request('configurationDone');
	console.log('[native] configurationDone done');

	/* ── Lua: initialize, launch, setBreakpoints, configurationDone ─────────── */
	await lua.request('initialize', {
		clientID: 'blyt-hybrid-reload',
		adapterID: 'blyt-lua',
		linesStartAt1: true,
		columnsStartAt1: true,
	});
	await lua.request('launch', {});
	const lbp = await lua.request('setBreakpoints', {
		source: { path: luaSource, name: 'main.lua' },
		breakpoints: [{ line: luaBreakLine }],
		lines: [luaBreakLine],
	});
	assert(
		lbp.body?.breakpoints?.[0]?.verified === true,
		`Lua breakpoint verified at attach (line ${luaBreakLine})`,
	);
	/* Lua configurationDone sets dap_ready → the runtime releases the startup
	 * gate and init() runs. */
	await lua.request('configurationDone');
	console.log('[lua] configurationDone done');
	await nativeLaunchP;
	console.log('[native] launch resolved');

	/* ── Startup init(): both breakpoints fire ────────────────────────────── */
	await waitFor(
		() => nativeHits.startup >= 1 && luaHits.startup >= 1,
		20000,
		`both init() breakpoints to fire at startup ` +
			`(native=${() => nativeHits.startup}, lua=${() => luaHits.startup})`,
	);
	assert(
		nativeHits.startup >= 1,
		'native init() breakpoint fired at startup',
	);
	assert(luaHits.startup >= 1, 'Lua init() breakpoint fired at startup');

	/* Let the cart settle into update() before reloading. */
	await sleep(500);

	/* ── Hot reload (real dev-control) → init() re-runs ───────────────────── */
	phase = 'reloaded';
	const reply = await devCtrl(parseInt(devCtrlPort, 10), {
		id: 1,
		cmd: 'reload',
		path: v2Cart,
	});
	console.log(`[reload] dev-control reply: ${reply}`);

	/* After the reload, init() re-runs against v2; both views — re-armed before
	 * init() by the post-reload gate — must fire their init() breakpoint again. */
	await waitFor(
		() => nativeHits.reloaded >= 1 && luaHits.reloaded >= 1,
		25000,
		'both init() breakpoints to fire after reload',
	);
	assert(
		nativeHits.reloaded >= 1,
		'native init() breakpoint fired AFTER reload (criterion 4)',
	);
	assert(
		luaHits.reloaded >= 1,
		'Lua init() breakpoint fired AFTER reload (criterion 4)',
	);

	/* ── Criterion 5: both connections persisted across the reload ────────── */
	const nthreads = await native.request('threads');
	assert(
		Array.isArray(nthreads.body?.threads),
		'native lldb-dap session still responsive after reload (criterion 5)',
	);
	const lthreads = await lua.request('threads');
	assert(
		Array.isArray(lthreads.body?.threads),
		'Lua DAP session still responsive after reload (criterion 5)',
	);

	try {
		lldb.stdin.end();
	} catch (_) {}
	lldb.kill();
	luaSock.destroy();
}

/* Send one JSON dev-control command and resolve with the reply line. */
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

main()
	.then(() => {
		console.log(`\n${passed} passed, ${failed} failed`);
		process.exit(failed > 0 ? 1 : 0);
	})
	.catch((err) => {
		console.error('\nERROR:', err.stack || err.message);
		process.exit(2);
	});
