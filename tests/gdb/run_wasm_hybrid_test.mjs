#!/usr/bin/env node
/*
 * tests/gdb/run_wasm_hybrid_test.mjs — WASM hybrid (DAP + GDB) test orchestrator.
 *
 * Starts a WebSocket relay HTTP server (paths /dap and /gdb) that the WASM
 * debug runtime connects to, plus two TCP servers for hybrid_test.mjs to use
 * as DAP and GDB endpoints.  Bridges messages bidirectionally between the two
 * transports with protocol translation (DAP: add/strip Content-Length; GDB:
 * raw passthrough).
 *
 * Usage:
 *   node run_wasm_hybrid_test.mjs <wasm_dir> <cart_path>
 *
 * Environment:
 *   BLYT_GDB_BREAK_ADDR — hex VMA of the native function entry point (from
 *                          readelf on the cart ELF).  If absent, only the DAP
 *                          session is exercised; the GDB breakpoint path is
 *                          skipped.
 *
 * Exit 0 on success, non-zero on failure.
 * Node.js 22+ required.
 */

/* Debug-driver default: every failure's captured stderr should already carry
 * a protocol/lifecycle trace.  'api' stays opt-in (high volume). */
if (!process.env.BLYT_TRACE) process.env.BLYT_TRACE = 'gdb,dap,lifecycle,frame';

import { execFile } from 'node:child_process';
import { createHash } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { createServer as createHttpServer } from 'node:http';
import { createRequire } from 'node:module';
import { createServer as createTcpServer } from 'node:net';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));

const WASM_DIR = process.argv[2];
const CART_PATH = process.argv[3];

if (!WASM_DIR || !CART_PATH) {
	process.stderr.write(
		'usage: run_wasm_hybrid_test.mjs <wasm_dir> <cart_path>\n',
	);
	process.exit(1);
}

/* ── Minimal WebSocket frame codec ─────────────────────────────────────── */

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
			`Sec-WebSocket-Accept: ${accept}\r\n\r\n`,
	);
	return true;
}

function wsParseFrames(buf, onFrame) {
	while (buf.length >= 2) {
		const b0 = buf[0],
			b1 = buf[1];
		const opcode = b0 & 0x0f;
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

/* ── DAP Content-Length framing ─────────────────────────────────────────── */

/* Parse one or more DAP messages from a Buffer; call onMsg(json) for each. */
function dapParseFrames(buf, onMsg) {
	while (true) {
		let i = 0;
		for (; i + 3 < buf.length; i++) {
			if (
				buf[i] === 0x0d &&
				buf[i + 1] === 0x0a &&
				buf[i + 2] === 0x0d &&
				buf[i + 3] === 0x0a
			)
				break;
		}
		if (i + 3 >= buf.length) return buf;
		const hdr = buf.slice(0, i).toString('utf8');
		const m = hdr.match(/Content-Length:\s*(\d+)/i);
		if (!m) {
			buf = buf.slice(i + 4);
			continue;
		}
		const len = parseInt(m[1], 10);
		if (buf.length < i + 4 + len) return buf;
		onMsg(buf.slice(i + 4, i + 4 + len).toString('utf8'));
		buf = buf.slice(i + 4 + len);
	}
}

function dapWrap(json) {
	return `Content-Length: ${Buffer.byteLength(json, 'utf8')}\r\n\r\n${json}`;
}

/* ── Relay infrastructure ───────────────────────────────────────────────── */

/*
 * Start the relay infrastructure:
 *   - HTTP server at wsPort accepting WebSocket upgrades at /dap and /gdb
 *   - TCP server at dapTcpPort for hybrid_test.mjs DAP client
 *   - TCP server at gdbTcpPort for hybrid_test.mjs GDB client
 *
 * Returns { wsPort, dapTcpPort, gdbTcpPort, waitForWasmConnections, shutdown }.
 */
function startRelays() {
	return new Promise((resolve) => {
		/* Per-path WebSocket state: set when WASM connects. */
		const wsSides = { '/dap': null, '/gdb': null };
		/* Resolve functions for the "WASM connected" promises. */
		const wsResolvers = { '/dap': null, '/gdb': null };
		const wsPromises = {
			'/dap': new Promise((r) => {
				wsResolvers['/dap'] = r;
			}),
			'/gdb': new Promise((r) => {
				wsResolvers['/gdb'] = r;
			}),
		};

		/* Pending messages from WASM → test-client side, queued until TCP connects. */
		const pendingDap = [];
		const pendingGdb = [];

		/* HTTP server for WASM WebSocket connections. */
		const httpServer = createHttpServer();
		httpServer.on('upgrade', (req, socket) => {
			const p = req.url;
			if (p !== '/dap' && p !== '/gdb') {
				socket.destroy();
				return;
			}
			if (!wsHandshake(req, socket)) return;

			const side = { socket, buf: Buffer.alloc(0), closed: false };
			socket.on('data', (chunk) => {
				side.buf = Buffer.concat([side.buf, chunk]);
				side.buf = wsParseFrames(side.buf, (text) => {
					if (side.onmsg) side.onmsg(text);
					else if (p === '/dap' && text !== null)
						pendingDap.push(text);
					else if (p === '/gdb' && text !== null)
						pendingGdb.push(text);
				});
			});
			socket.on('close', () => {
				side.closed = true;
			});
			socket.on('error', () => {
				side.closed = true;
			});
			process.stderr.write(
				`[wasm_hybrid] WASM WebSocket connected: ${p}\n`,
			);
			wsSides[p] = side;
			if (wsResolvers[p]) wsResolvers[p](side);
		});

		/* TCP servers for hybrid_test.mjs. */
		const dapTcpServer = createTcpServer();
		const gdbTcpServer = createTcpServer();

		/* Wire up DAP TCP ↔ WebSocket relay when the TCP connection arrives. */
		dapTcpServer.on('connection', (sock) => {
			const ws = wsSides['/dap'];
			let tcpBuf = Buffer.alloc(0);

			/* Flush any WASM messages that arrived before TCP connected. */
			for (const json of pendingDap) sock.write(dapWrap(json), 'utf8');
			pendingDap.length = 0;

			/* WebSocket (WASM) → TCP (test client): add Content-Length. */
			if (ws)
				ws.onmsg = (json) => {
					if (json !== null && !sock.destroyed)
						sock.write(dapWrap(json), 'utf8');
				};

			/* TCP (test client) → WebSocket (WASM): strip Content-Length. */
			sock.on('data', (chunk) => {
				tcpBuf = Buffer.concat([tcpBuf, chunk]);
				tcpBuf = dapParseFrames(tcpBuf, (json) => {
					if (ws && !ws.closed) ws.socket.write(wsFrame(json));
				});
			});
			sock.on('error', () => {});
		});

		/* Wire up GDB TCP ↔ WebSocket relay when the TCP connection arrives. */
		gdbTcpServer.on('connection', (sock) => {
			const ws = wsSides['/gdb'];

			/* Flush any WASM messages that arrived before TCP connected. */
			for (const rsp of pendingGdb) sock.write(rsp, 'utf8');
			pendingGdb.length = 0;

			/* WebSocket (WASM) → TCP (test client): passthrough RSP text. */
			if (ws)
				ws.onmsg = (text) => {
					if (text !== null && !sock.destroyed)
						sock.write(text, 'utf8');
				};

			/* TCP (test client) → WebSocket (WASM): passthrough RSP text. */
			sock.on('data', (chunk) => {
				if (ws && !ws.closed)
					ws.socket.write(wsFrame(chunk.toString('utf8')));
			});
			sock.on('error', () => {});
		});

		/* Start all three servers and resolve once ports are known. */
		let started = 0;
		let wsPort = 0,
			dapTcpPort = 0,
			gdbTcpPort = 0;

		function tryResolve() {
			if (++started < 3) return;
			resolve({
				wsPort,
				dapTcpPort,
				gdbTcpPort,
				waitForWasmConnections: () =>
					Promise.all([wsPromises['/dap'], wsPromises['/gdb']]),
				shutdown: () => {
					httpServer.close();
					dapTcpServer.close();
					gdbTcpServer.close();
				},
			});
		}

		httpServer.listen(0, '127.0.0.1', () => {
			wsPort = httpServer.address().port;
			tryResolve();
		});
		dapTcpServer.listen(0, '127.0.0.1', () => {
			dapTcpPort = dapTcpServer.address().port;
			tryResolve();
		});
		gdbTcpServer.listen(0, '127.0.0.1', () => {
			gdbTcpPort = gdbTcpServer.address().port;
			tryResolve();
		});
	});
}

/* ── Load WASM debug runtime ─────────────────────────────────────────────── */

function loadWasmRuntime(wasmDir, cartPath, wsPort) {
	return new Promise((resolve, reject) => {
		const cartData = readFileSync(cartPath);

		globalThis.__blyt_cart_data = new Uint8Array(cartData);
		// module_pre.js copies __blyt_env_vars into the Emscripten ENV (preRun),
		// so the C-side getenv("BLYT_TRACE") sees the same channels as native legs.
		if (process.env.BLYT_TRACE) {
			globalThis.__blyt_env_vars = Object.assign(
				globalThis.__blyt_env_vars || {},
				{
					BLYT_TRACE: process.env.BLYT_TRACE,
				},
			);
		}
		globalThis.__blyt_dap_port = wsPort;
		globalThis.__blyt_gdb_port = wsPort;

		globalThis.__blyt_init_module = {
			print: (s) => process.stderr.write(`${s}\n`),
			printErr: (s) => process.stderr.write(`${s}\n`),
			onExit: (code) => resolve(code),
		};

		process.stderr.write(
			`[wasm_hybrid] node ${process.version}; require start\n`,
		);
		try {
			const require = createRequire(import.meta.url);
			require(path.join(wasmDir, 'blytdebug.js'));
			process.stderr.write('[wasm_hybrid] require returned\n');
		} catch (e) {
			process.stderr.write(`[wasm_hybrid] require threw: ${e.message}\n`);
			reject(e);
		}
	});
}

/* ── Main ───────────────────────────────────────────────────────────────── */

async function main() {
	const relay = await startRelays();
	process.stderr.write(
		`[wasm_hybrid] relays up — ws:${relay.wsPort} dap-tcp:${relay.dapTcpPort} gdb-tcp:${relay.gdbTcpPort}\n`,
	);

	const _runtimeDone = loadWasmRuntime(
		WASM_DIR,
		CART_PATH,
		relay.wsPort,
	).catch((e) => {
		console.error('[wasm_hybrid] WASM error:', e.message);
	});

	/* Emit a heartbeat every 5 s so CI logs show the process is alive. */
	const heartbeat = setInterval(
		() =>
			process.stderr.write(
				'[wasm_hybrid] waiting for WASM WebSocket connections...\n',
			),
		5000,
	);

	/* Wait for WASM to connect both /dap and /gdb WebSocket paths.
	 * Use Promise.race so the timeout properly rejects the async chain —
	 * a throw inside setTimeout is not a rejection and may be silently
	 * swallowed by the ESM unhandled-rejection handler in Node.js 24+. */
	await Promise.race([
		relay.waitForWasmConnections(),
		new Promise((_, reject) =>
			setTimeout(
				() =>
					reject(
						new Error(
							'timeout: WASM did not connect both DAP and GDB paths',
						),
					),
				60000,
			),
		),
	]);
	clearInterval(heartbeat);
	console.log(
		`[wasm_hybrid] WASM connected — DAP tcp://127.0.0.1:${relay.dapTcpPort}  GDB tcp://127.0.0.1:${relay.gdbTcpPort}`,
	);

	const testScript = path.join(__dirname, 'hybrid_test.mjs');
	const dapEndpoint = `tcp://127.0.0.1:${relay.dapTcpPort}`;
	const gdbEndpoint = `tcp://127.0.0.1:${relay.gdbTcpPort}`;
	const extraArgs = process.env.BLYT_GDB_BREAK_ADDR
		? ['--gdb-break-addr', process.env.BLYT_GDB_BREAK_ADDR]
		: [];

	await new Promise((resolve, reject) => {
		execFile(
			process.execPath,
			[testScript, dapEndpoint, gdbEndpoint, ...extraArgs],
			{ timeout: 120000 },
			(err, stdout, stderr) => {
				process.stdout.write(stdout);
				process.stderr.write(stderr);
				if (err) reject(err);
				else resolve();
			},
		);
	});

	relay.shutdown();
	/* Explicit exit: Emscripten WebSocket keepalives are only released by
	 * emscripten_WebSocket_close() inside the WASM module; they will keep
	 * Node.js alive indefinitely after the test completes on Linux/Node 24+.
	 * All test assertions have passed by the time we reach here. */
	process.exit(0);
}

main().catch((e) => {
	console.error('[wasm_hybrid] FAILED:', e.message);
	process.exit(1);
});
