#!/usr/bin/env node
/*
 * tests/dap/reload_dap_test.mjs — WASM-dev reload-while-debugging continuity test
 * (issue #90, slice 4).
 *
 * Proves the contract ADR-0045 promises: a hot reload in a WASM-dev debug
 * session is *uninterrupted* — the same VS Code DAP connection keeps working
 * across the cart swap, so the debugger does not need to reconnect.
 *
 * Self-contained (no child process): starts an in-process WebSocket relay,
 * loads blytdebug.js (the DAP-enabled WASM runtime) with the relay port and the
 * dev-control reload stubs injected, then drives a DAP client inline over the
 * relay:
 *   initialize → launch → setBreakpoints(draw line) → configurationDone →
 *   stopped(breakpoint)                    [session works]
 *   clear breakpoints → continue           [cart free-runs v1]
 *   ccall blyt_dev_ctrl_command(reload)    [swap v1 → v2, HOT_RELOAD]
 *   re-arm breakpoint → stopped again      [SAME connection survived the reload]
 *
 * The reload is driven straight into the C handler (like dev_ctrl_test.js) — the
 * dev-control transport is unit-tested elsewhere; here the point is the DAP
 * session, which rides the relay.  If the reload terminated the session, the
 * re-armed setBreakpoints / stopped would time out and the test fails.
 *
 * Usage:
 *   node reload_dap_test.mjs <wasm_dir> <cart_v1> <cart_v2> <bp_line>
 *
 * Exit 0 on success, non-zero on failure.  Node.js 22+ (built-in WebSocket).
 */

if (!process.env.BLYT_TRACE) process.env.BLYT_TRACE = 'dap,lifecycle,frame';

import { createHash } from 'node:crypto';
import { existsSync, readFileSync } from 'node:fs';
import { createServer } from 'node:http';
import { createRequire } from 'node:module';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));

const WASM_DIR = process.argv[2];
const CART_V1 = process.argv[3];
const CART_V2 = process.argv[4];
const BP_LINE = parseInt(process.argv[5] || '8', 10);
const SOURCE = '/blyt/cart/src/game/lua/main.lua';

if (!WASM_DIR || !CART_V1 || !CART_V2) {
	process.stderr.write(
		'usage: reload_dap_test.mjs <wasm_dir> <cart_v1> <cart_v2> <bp_line>\n',
	);
	process.exit(1);
}

/* ── Minimal WebSocket relay (server side, no masking) ─────────────────────── */

const WS_MAGIC = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';

function wsHandshake(req, socket) {
	const key = req.headers['sec-websocket-key'];
	if (!key) {
		socket.destroy();
		return false;
	}
	const accept = createHash('sha1')
		.update(key + WS_MAGIC)
		.digest('base64');
	socket.write(
		'HTTP/1.1 101 Switching Protocols\r\n' +
			'Upgrade: websocket\r\n' +
			'Connection: Upgrade\r\n' +
			`Sec-WebSocket-Accept: ${accept}\r\n` +
			'\r\n',
	);
	return true;
}

function wsParseFrames(buf, onFrame) {
	while (buf.length >= 2) {
		const b1 = buf[1];
		const opcode = buf[0] & 0x0f;
		const masked = !!(b1 & 0x80);
		let payLen = b1 & 0x7f;
		let offset = 2;
		if (payLen === 126) {
			if (buf.length < 4) break;
			payLen = buf.readUInt16BE(2);
			offset = 4;
		} else if (payLen === 127) {
			if (buf.length < 10) break;
			payLen = Number(buf.readBigUInt64BE(2));
			offset = 10;
		}
		const maskOffset = masked ? offset : null;
		if (masked) offset += 4;
		if (buf.length < offset + payLen) break;
		const payload = buf.slice(offset, offset + payLen);
		if (masked) {
			const mask = buf.slice(maskOffset, maskOffset + 4);
			for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i & 3];
		}
		buf = buf.slice(offset + payLen);
		if (opcode === 0x01 || opcode === 0x02)
			onFrame(payload.toString('utf8'));
		else if (opcode === 0x08) onFrame(null);
	}
	return buf;
}

function wsFrame(text) {
	const payload = Buffer.from(text, 'utf8');
	const len = payload.length;
	let header;
	if (len < 126) {
		header = Buffer.from([0x81, len]);
	} else if (len < 65536) {
		header = Buffer.alloc(4);
		header[0] = 0x81;
		header[1] = 126;
		header.writeUInt16BE(len, 2);
	} else {
		header = Buffer.alloc(10);
		header[0] = 0x81;
		header[1] = 127;
		header.writeBigUInt64BE(BigInt(len), 2);
	}
	return Buffer.concat([header, payload]);
}

function startRelay() {
	return new Promise((resolvePort) => {
		let sideA = null; /* WASM runtime */
		let sideB = null; /* DAP client */
		function relay(from, to) {
			from.onmsg = (text) => {
				if (text === null) return;
				if (to && !to.closed) to.socket.write(wsFrame(text));
			};
		}
		function makeSide(socket) {
			const side = {
				socket,
				buf: Buffer.alloc(0),
				onmsg: null,
				closed: false,
			};
			socket.on('data', (chunk) => {
				side.buf = Buffer.concat([side.buf, chunk]);
				side.buf = wsParseFrames(side.buf, (t) => {
					if (side.onmsg) side.onmsg(t);
				});
			});
			socket.on('close', () => {
				side.closed = true;
			});
			socket.on('error', () => {
				side.closed = true;
			});
			return side;
		}
		const server = createServer();
		server.on('upgrade', (req, socket) => {
			if (!wsHandshake(req, socket)) return;
			if (sideA === null) {
				sideA = makeSide(socket);
				if (sideB !== null) {
					relay(sideA, sideB);
					relay(sideB, sideA);
				}
			} else if (sideB === null) {
				sideB = makeSide(socket);
				relay(sideA, sideB);
				relay(sideB, sideA);
			}
		});
		server.listen(0, '127.0.0.1', () => resolvePort(server.address().port));
	});
}

/* ── WASM runtime load with dev-control reload stubs ───────────────────────── */

let M = null; /* captured Emscripten Module */
const prints = []; /* blyt.debug.print output */
const devCtrlResponses = []; /* dev control JSON responses */

/* The driver's own output goes to stderr; cart blyt.debug.print routes through
 * blyt_js_log → console.log, which we capture into `prints` (so overriding
 * console.log must not swallow the driver's PASS/FAIL lines). */
const log = (s) => process.stderr.write(`${s}\n`);
console.log = (...args) => {
	prints.push(args.map(String).join(' '));
};

function loadWasmRuntime(dapPort) {
	return new Promise((resolve, reject) => {
		globalThis.__blyt_cart_data = new Uint8Array(readFileSync(CART_V1));
		globalThis.__blyt_env_vars = {
			BLYT_SAVE_DIR: '/blyt_save',
			...(process.env.BLYT_TRACE
				? { BLYT_TRACE: process.env.BLYT_TRACE }
				: {}),
		};
		globalThis.__blyt_dap_port = dapPort;

		/* Runtime → devtool responses. */
		globalThis.blyt_dev_ctrl_send = (json) => devCtrlResponses.push(json);
		/* Reload refetch: write cart_v2 into MEMFS (simulating a rebuild), then
		 * hand back to C — synchronous, so the reload completes inside the
		 * originating blyt_dev_ctrl_command call (matches dev_ctrl_test.js). */
		globalThis.blyt_dev_ctrl_fetch_cart = () => {
			M.FS.writeFile('/cart.blyt', new Uint8Array(readFileSync(CART_V2)));
			M.ccall('blyt_dev_ctrl_reload_fetched', null, ['int'], [1]);
		};

		globalThis.__blyt_init_module = {
			print: (s) => process.stderr.write(`${s}\n`),
			printErr: (s) => process.stderr.write(`${s}\n`),
			onRuntimeInitialized() {
				M = this;
			},
			onExit: (code) => resolve(code),
		};

		try {
			const require = createRequire(import.meta.url);
			const js = existsSync(path.join(WASM_DIR, 'blytdebug.js'))
				? 'blytdebug.js'
				: 'blytplay.js';
			require(path.join(WASM_DIR, js));
		} catch (e) {
			reject(e);
		}
	});
}

/* ── Inline DAP client (over the relay) ────────────────────────────────────── */

let seq = 1;
let ws = null;
const pending = new Map();
const eventQueue = [];
const waiters = [];
const TIMEOUT_MS = 30000;

function send(obj) {
	ws.send(JSON.stringify(obj));
}

function request(command, args = {}) {
	const reqSeq = seq++;
	return new Promise((resolve, reject) => {
		const timer = setTimeout(
			() => reject(new Error(`timeout waiting for ${command}`)),
			TIMEOUT_MS,
		);
		pending.set(reqSeq, { resolve, reject, timer });
		send({ seq: reqSeq, type: 'request', command, arguments: args });
	});
}

function nextEvent(name) {
	return new Promise((resolve, reject) => {
		const timer = setTimeout(
			() => reject(new Error(`timeout waiting for event "${name}"`)),
			TIMEOUT_MS,
		);
		const check = (e) => {
			if (e.event === name) {
				clearTimeout(timer);
				const widx = waiters.indexOf(check);
				if (widx >= 0) waiters.splice(widx, 1);
				const qidx = eventQueue.indexOf(e);
				if (qidx >= 0) eventQueue.splice(qidx, 1);
				resolve(e);
				return true;
			}
			return false;
		};
		for (const e of eventQueue) if (check(e)) return;
		waiters.push(check);
	});
}

function onMessage(text) {
	let msg;
	try {
		msg = JSON.parse(text);
	} catch {
		return;
	}
	if (msg.type === 'response') {
		const p = pending.get(msg.request_seq);
		if (p) {
			clearTimeout(p.timer);
			pending.delete(msg.request_seq);
			if (msg.success) p.resolve(msg.body || {});
			else p.reject(new Error(msg.message || `${msg.command} failed`));
		}
	} else if (msg.type === 'event') {
		eventQueue.push(msg);
		for (const w of [...waiters]) w(msg);
	}
}

function connectClient(port) {
	return new Promise((resolve, reject) => {
		ws = new WebSocket(`ws://127.0.0.1:${port}/dap`);
		ws.addEventListener('open', resolve);
		ws.addEventListener('error', reject);
		ws.addEventListener('message', (ev) =>
			onMessage(
				typeof ev.data === 'string' ? ev.data : ev.data.toString(),
			),
		);
	});
}

function setBp(bps) {
	return request('setBreakpoints', {
		source: { path: SOURCE, name: SOURCE },
		breakpoints: bps.map((line) => ({ line })),
		lines: bps,
	});
}

let failed = 0;
function assert(cond, msg) {
	if (cond) log(`  PASS: ${msg}`);
	else {
		log(`  FAIL: ${msg}`);
		failed++;
	}
}

async function main() {
	const relayPort = await startRelay();
	const runtimeDone = loadWasmRuntime(relayPort).catch((e) =>
		console.error('[reload_dap_test] WASM runtime error:', e.message),
	);

	/* Let the runtime connect its DAP WebSocket to the relay first. */
	await new Promise((r) => setTimeout(r, 2000));
	await connectClient(relayPort);

	/* 1. Establish a debug session and stop at the draw() breakpoint. */
	await request('initialize', { adapterID: 'blyt-lua', linesStartAt1: true });
	await nextEvent('initialized');
	await request('launch', {});
	await setBp([BP_LINE]);
	await request('configurationDone');
	const stopped1 = await nextEvent('stopped');
	assert(
		stopped1.body.reason === 'breakpoint' ||
			stopped1.body.reason === 'step',
		`v1: stopped at the draw() breakpoint (reason "${stopped1.body.reason}")`,
	);

	/* 2. Clear the breakpoint and let v1 free-run. */
	await setBp([]);
	await request('continue', { threadId: 1 });

	/* 3. Trigger a hot reload (v1 → v2) straight into the C handler. */
	const respBefore = devCtrlResponses.length;
	const printsBefore = prints.length;
	M.ccall(
		'blyt_dev_ctrl_command',
		null,
		['string'],
		['{"id":1,"cmd":"reload"}'],
	);
	const reloadResp = devCtrlResponses
		.slice(respBefore)
		.map((r) => JSON.parse(r))
		.find((r) => r.cmd === 'reload');
	assert(
		reloadResp && reloadResp.status === 'ok',
		`reload acknowledged ok (got ${JSON.stringify(reloadResp)})`,
	);
	assert(
		prints.slice(printsBefore).some((p) => p.includes('reason=3')),
		'reload ran on_load_state(HOT_RELOAD, reason=3)',
	);

	/* 4. The crux: re-arm the breakpoint on the SAME connection and verify the
	 * reloaded v2 cart stops again — i.e. the DAP session was uninterrupted. */
	await setBp([BP_LINE]);
	const stopped2 = await nextEvent('stopped');
	assert(
		stopped2.body.reason === 'breakpoint' ||
			stopped2.body.reason === 'step',
		`v2: breakpoint hit again after reload — session uninterrupted (reason "${stopped2.body.reason}")`,
	);

	/* Confirm the session is still fully responsive post-reload. */
	const threads = await request('threads');
	assert(
		threads.threads && threads.threads.length >= 1,
		'post-reload: threads request still answered on the same connection',
	);

	await setBp([]);
	await request('continue', { threadId: 1 });
	ws.close();
	void runtimeDone;
}

function dumpDiagnostics() {
	log(`\n-- dev control responses --\n${devCtrlResponses.join('\n')}`);
	log(`-- cart prints --\n${prints.join('\n')}`);
}

main()
	.then(() => {
		log(`\n${failed === 0 ? 'PASS' : 'FAIL'} (${failed} failures)`);
		if (failed > 0) dumpDiagnostics();
		process.exit(failed > 0 ? 1 : 0);
	})
	.catch((e) => {
		log(`[reload_dap_test] ERROR: ${e.message}`);
		dumpDiagnostics();
		process.exit(2);
	});
