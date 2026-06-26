#!/usr/bin/env node
/*
 * tests/dap/run_sdl_lua_reload_test.mjs — SDL2 Lua DAP reload continuity test
 * (issue #140).
 *
 * Proves a pure-Lua native debug session (blytdebug --debug) survives a hot
 * reload: the same DAP TCP connection stays open and source-line breakpoints
 * re-arm on the new Lua state without a session restart.  Also proves that an
 * init() breakpoint fires after the reload.
 *
 * Drives the test inline:
 *   initialize → launch → setBreakpoints(bp_line) → configurationDone →
 *   stopped(breakpoint, first init())
 *   continue
 *   dev-control reload → v2 cart hot-swapped
 *   stopped(breakpoint, re-armed init())   ← same DAP connection
 *   continue
 *
 * Usage:
 *   node run_sdl_lua_reload_test.mjs <blytdebug> <cart_v1> <cart_v2> <bp_line>
 *
 *   <blytdebug>  path to the blytdebug binary
 *   <cart_v1>    initial cart loaded on startup
 *   <cart_v2>    replacement cart swapped in by the dev-control reload
 *   <bp_line>    1-based line in init() shared by both carts
 *
 * blytdebug is started with --dev-ctrl-port 0 so the test drives the reload
 * directly without needing a devtool process.
 *
 * Exit 0 on success, non-zero on failure.  Node.js 22+ required.
 */

if (!process.env.BLYT_TRACE) process.env.BLYT_TRACE = 'dap,lifecycle,frame';

import { spawn } from 'node:child_process';
import net from 'node:net';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));

const BLYTDEBUG = process.argv[2];
const CART_V1 = process.argv[3];
const CART_V2 = process.argv[4];
const BP_LINE = parseInt(process.argv[5] || '2', 10);
const SOURCE = '/blyt/cart/src/game/lua/main.lua';
const TIMEOUT_MS = 60000;

if (!BLYTDEBUG || !CART_V1 || !CART_V2) {
	process.stderr.write(
		'usage: run_sdl_lua_reload_test.mjs' +
			' <blytdebug> <cart_v1> <cart_v2> <bp_line>\n',
	);
	process.exit(1);
}

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

/* ── DAP TCP client ──────────────────────────────────────────────────────── */

function connectDap(port) {
	return new Promise((resolve, reject) => {
		let seq = 1;
		let buf = Buffer.alloc(0);
		const pending = new Map();
		const eventQueue = [];
		const waiters = [];

		const sock = net.createConnection({ port, host: '127.0.0.1' });
		sock.on('error', reject);
		sock.on('connect', () => resolve({ req, nextEvent, close }));

		sock.on('data', (chunk) => {
			buf = Buffer.concat([buf, chunk]);
			while (true) {
				const hdr = buf
					.toString('ascii', 0, Math.min(buf.length, 256))
					.match(/^Content-Length:\s*(\d+)\r\n\r\n/);
				if (!hdr) break;
				const hlen = Buffer.byteLength(hdr[0]);
				const blen = parseInt(hdr[1], 10);
				if (buf.length < hlen + blen) break;
				const msg = JSON.parse(buf.toString('utf8', hlen, hlen + blen));
				buf = buf.slice(hlen + blen);
				if (msg.type === 'response') {
					const p = pending.get(msg.request_seq);
					if (p) {
						clearTimeout(p.timer);
						pending.delete(msg.request_seq);
						if (msg.success) p.resolve(msg);
						else
							p.reject(
								new Error(
									`DAP ${msg.command} failed: ${msg.message}`,
								),
							);
					}
				} else if (msg.type === 'event') {
					const wi = waiters.findIndex(
						(w) => !w.filter || w.filter === msg.event,
					);
					if (wi >= 0) {
						const w = waiters.splice(wi, 1)[0];
						clearTimeout(w.timer);
						w.resolve(msg);
					} else {
						eventQueue.push(msg);
					}
				}
			}
		});

		function send(obj) {
			const json = JSON.stringify(obj);
			const hdr = `Content-Length: ${Buffer.byteLength(json)}\r\n\r\n`;
			sock.write(hdr + json);
		}

		function req(command, args = {}) {
			const reqSeq = seq++;
			return new Promise((resolve, reject) => {
				const timer = setTimeout(
					() =>
						reject(
							new Error(
								`timeout waiting for ${command} response`,
							),
						),
					TIMEOUT_MS,
				);
				pending.set(reqSeq, { resolve, reject, timer });
				send({
					seq: reqSeq,
					type: 'request',
					command,
					arguments: args,
				});
			});
		}

		function nextEvent(filter) {
			return new Promise((resolve, reject) => {
				const idx = eventQueue.findIndex(
					(e) => !filter || e.event === filter,
				);
				if (idx >= 0) {
					resolve(eventQueue.splice(idx, 1)[0]);
					return;
				}
				const timer = setTimeout(
					() =>
						reject(
							new Error(
								`timeout waiting for event '${filter || 'any'}'`,
							),
						),
					TIMEOUT_MS,
				);
				waiters.push({ filter, resolve, reject, timer });
			});
		}

		function close() {
			sock.destroy();
		}
	});
}

/* ── Dev-control TCP connection ─────────────────────────────────────────── */

function connectDevCtrl(port) {
	return new Promise((resolve, reject) => {
		const sock = net.createConnection({ port, host: '127.0.0.1' });
		sock.on('error', reject);

		let lineBuf = '';
		const lineWaiters = [];
		const lineQueue = [];

		sock.on('data', (d) => {
			lineBuf += d.toString();
			let nl;
			while ((nl = lineBuf.indexOf('\n')) >= 0) {
				const line = lineBuf.slice(0, nl).trim();
				lineBuf = lineBuf.slice(nl + 1);
				if (!line) continue;
				if (lineWaiters.length) {
					const w = lineWaiters.shift();
					clearTimeout(w.timer);
					w.resolve(line);
				} else {
					lineQueue.push(line);
				}
			}
		});

		sock.on('connect', () => {
			resolve({
				send(obj) {
					sock.write(JSON.stringify(obj) + '\n');
				},
				readLine() {
					return new Promise((resolve, reject) => {
						if (lineQueue.length) {
							resolve(lineQueue.shift());
							return;
						}
						const timer = setTimeout(
							() =>
								reject(
									new Error(
										'timeout waiting for dev-ctrl response',
									),
								),
							TIMEOUT_MS,
						);
						lineWaiters.push({ resolve, reject, timer });
					});
				},
				close() {
					sock.destroy();
				},
			});
		});

		sock.on('error', (e) => {
			for (const w of lineWaiters) {
				clearTimeout(w.timer);
				w.reject(e);
			}
		});
	});
}

/* ── Start blytdebug ─────────────────────────────────────────────────────── */

function startBlytdebug(cartPath) {
	return new Promise((resolve, reject) => {
		const proc = spawn(
			BLYTDEBUG,
			['--debug', '0', '--headless', '--dev-ctrl-port', '0', cartPath],
			{ stdio: ['ignore', 'pipe', 'pipe'] },
		);

		proc.stderr.on('data', (d) => process.stderr.write(d));

		let buf = '';
		let dapPort = 0;
		let devCtrlPort = 0;
		let resolved = false;

		proc.stdout.on('data', (d) => {
			process.stderr.write(d);
			buf += d.toString();
			if (!dapPort) {
				const m = buf.match(/DAP listening on port (\d+)/);
				if (m) dapPort = parseInt(m[1], 10);
			}
			if (!devCtrlPort) {
				const m = buf.match(
					/Dev control: listening on 127\.0\.0\.1:(\d+)/,
				);
				if (m) devCtrlPort = parseInt(m[1], 10);
			}
			if (dapPort && devCtrlPort && !resolved) {
				resolved = true;
				resolve({ proc, dapPort, devCtrlPort });
			}
		});

		proc.on('exit', (code) => {
			if (!resolved)
				reject(
					new Error(
						`blytdebug exited (code ${code}) before announcing ports`,
					),
				);
		});
		proc.on('error', (e) => {
			if (!resolved) reject(e);
		});
		setTimeout(() => {
			if (!resolved) {
				proc.kill();
				reject(
					new Error('blytdebug did not announce ports within 30 s'),
				);
			}
		}, 30000);
	});
}

/* ── Main ────────────────────────────────────────────────────────────────── */

async function main() {
	const { proc, dapPort, devCtrlPort } = await startBlytdebug(CART_V1);
	const blytdebugExit = new Promise((r) =>
		proc.on('exit', (code, signal) => r({ code, signal })),
	);

	/* Connect dev-ctrl before DAP so the game loop can accept it immediately
	 * once the DAP client connects and configuration completes. */
	const devCtrl = await connectDevCtrl(devCtrlPort);

	const dap = await connectDap(dapPort);

	/* DAP handshake */
	await dap.req('initialize', {
		adapterID: 'blyt',
		linesStartAt1: true,
		columnsStartAt1: true,
	});
	await dap.nextEvent('initialized');
	await dap.req('launch', {
		type: 'blyt',
		request: 'launch',
		name: 'Lua reload test',
	});
	await dap.req('setBreakpoints', {
		source: { path: SOURCE },
		breakpoints: [{ line: BP_LINE }],
	});
	await dap.req('configurationDone');

	/* ── First stop: init() breakpoint at startup ── */
	const stop1 = await dap.nextEvent('stopped');
	assert(
		stop1.body.reason === 'breakpoint',
		`first stop is a breakpoint (got '${stop1.body.reason}')`,
	);
	console.log(
		`  first stop: reason=${stop1.body.reason}, thread=${stop1.body.threadId}`,
	);

	await dap.req('continue', { threadId: stop1.body.threadId });

	/* ── Dev-control reload: swap in v2 cart ── */

	/* Send the reload command but do NOT await yet — the dev-ctrl ack arrives
	 * only after reload_impl completes, which blocks waiting for the DAP
	 * continue below.  Awaiting here would deadlock. */
	const reloadAck = devCtrl.readLine();
	devCtrl.send({ id: 1, cmd: 'reload', path: CART_V2 });

	/* ── Second stop: re-armed breakpoint fires in the new init() ── */
	const stop2 = await dap.nextEvent('stopped');
	assert(
		stop2.body.reason === 'breakpoint',
		`post-reload stop is a breakpoint (got '${stop2.body.reason}')`,
	);
	assert(
		stop2.body.threadId === stop1.body.threadId,
		'same thread ID — DAP session persisted across the reload (not restarted)',
	);
	console.log(
		`  post-reload stop: reason=${stop2.body.reason}, thread=${stop2.body.threadId}`,
	);

	/* Continue so reload_impl can return and the dev-ctrl ack can be sent. */
	await dap.req('continue', { threadId: stop2.body.threadId });

	const ackLine = await reloadAck;
	const ack = JSON.parse(ackLine);
	assert(
		ack.status === 'ok' && ack.cmd === 'reload',
		`reload command acknowledged (got ${ackLine})`,
	);

	dap.close();
	devCtrl.close();

	/* The cart runs indefinitely (no blyt.quit()) — kill blytdebug cleanly. */
	const exited = await Promise.race([
		blytdebugExit,
		new Promise((r) => {
			proc.kill();
			setTimeout(() => r(null), 5000);
		}),
	]);
	if (exited && exited.signal && exited.signal !== 'SIGTERM') {
		console.error(`blytdebug died with unexpected signal ${exited.signal}`);
		failed++;
	}

	console.log(`\n${passed} passed, ${failed} failed`);
	if (failed > 0) process.exit(1);
}

main().catch((e) => {
	console.error('[sdl_lua_reload_test] FAILED:', e.message);
	process.exit(1);
});
