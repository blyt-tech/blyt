#!/usr/bin/env node
/*
 * tests/dap/run_wasm_lldb_dap_test.mjs — headless WASM lldb-dap orchestrator.
 *
 * Reproduces the `blyt debug <dir>` browser-relay debug path (issue #144)
 * without VS Code or a real browser:
 *
 *   lldb-dap ──TCP──▶ [WS↔TCP bridge] ──WebSocket──▶ WASM runtime (in Node)
 *
 * The WASM runtime (share/wasm-debug/blytdebug.js) runs the cart through
 * rv32emu and speaks GDB RSP over a WebSocket, exactly as in the browser.  The
 * bridge mirrors the devtool's GDB relay (devtool/src/run.rs): it forwards each
 * TCP read chunk as a SINGLE WebSocket frame.  Because lldb-dap pipelines RSP
 * packets in no-ack mode, TCP coalesces them, so a frame can carry multiple
 * packets — the case that broke the WASM transport's one-packet-per-frame
 * assumption (issue #144).
 *
 * The lldb-dap side is driven by the shared run_lldb_dap_test.mjs client (the
 * same driver used by the native SDL lldb-dap suite), so the WASM leg asserts
 * the identical observable contract.
 *
 * Usage:
 *   node run_wasm_lldb_dap_test.mjs <wasm_dir> <lldb-dap> <cart> <cwd> \
 *        [--test <name>]
 *
 * Environment (passed through to run_lldb_dap_test.mjs):
 *   BLYT_STUB_PROGRAM, BLYT_SOURCE_FILE, BLYT_GDB_BREAK_LINE, BLYT_TRACE
 *
 * Exit 0 on success, non-zero on failure.  Node.js 22+ required.
 */

if (!process.env.BLYT_TRACE) process.env.BLYT_TRACE = 'gdb,dap,lifecycle,frame';

import { execFile } from 'node:child_process';
import { createHash } from 'node:crypto';
import { existsSync, readFileSync } from 'node:fs';
import { createServer } from 'node:http';
import { createRequire } from 'node:module';
import { createServer as createTcpServer } from 'node:net';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));

const WASM_DIR = process.argv[2];
const LLDB_DAP = process.argv[3];
const CART_PATH = process.argv[4];
const PROJECT_CWD = process.argv[5];

let TEST_NAME = 'auto-start';
for (let i = 6; i < process.argv.length; i++) {
	if (process.argv[i] === '--test' && process.argv[i + 1])
		TEST_NAME = process.argv[++i];
}

if (!WASM_DIR || !LLDB_DAP || !CART_PATH || !PROJECT_CWD) {
	process.stderr.write(
		'usage: run_wasm_lldb_dap_test.mjs <wasm_dir> <lldb-dap> <cart> <cwd> [--test <name>]\n',
	);
	process.exit(1);
}

/* ── Minimal WebSocket server framing (server side speaks to the WASM client) ── */

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

/* ── WS (WASM) ↔ TCP (lldb-dap) bridge ──────────────────────────────────────
 *
 * Faithful to devtool/src/run.rs `run_gdb_relay_session`: each raw TCP read
 * chunk is forwarded as one WebSocket frame, and each WebSocket frame's payload
 * is written verbatim to the TCP socket. */
function startBridge() {
	return new Promise((resolvePort) => {
		let wasmSock = null; // WS server socket to the WASM runtime
		let wasmBuf = Buffer.alloc(0);
		let tcpSock = null; // TCP socket to lldb-dap
		let onWasmConnect = null;

		function flushTcpToWasm(chunk) {
			if (wasmSock && !wasmSock.destroyed) {
				process.stderr.write(
					`[bridge] tcp→ws ${JSON.stringify(chunk.toString('utf8').slice(0, 80))}\n`,
				);
				wasmSock.write(wsFrame(chunk.toString('utf8')));
			}
		}

		const wsServer = createServer();
		wsServer.on('upgrade', (req, socket) => {
			if (req.url !== '/gdb') {
				socket.destroy();
				return;
			}
			if (!wsHandshake(req, socket)) return;
			wasmSock = socket;
			socket.on('data', (chunk) => {
				wasmBuf = Buffer.concat([wasmBuf, chunk]);
				wasmBuf = wsParseFrames(wasmBuf, (text) => {
					if (text === null) {
						if (tcpSock && !tcpSock.destroyed) tcpSock.end();
						return;
					}
					process.stderr.write(
						`[bridge] ws→tcp ${JSON.stringify(text.slice(0, 80))}\n`,
					);
					if (tcpSock && !tcpSock.destroyed) tcpSock.write(text);
				});
			});
			socket.on('close', () => {
				if (tcpSock && !tcpSock.destroyed) tcpSock.destroy();
			});
			socket.on('error', () => {});
			if (onWasmConnect) {
				onWasmConnect();
				onWasmConnect = null;
			}
		});

		const tcpServer = createTcpServer((sock) => {
			tcpSock = sock;
			sock.on('data', (chunk) => flushTcpToWasm(chunk));
			sock.on('close', () => {
				if (wasmSock && !wasmSock.destroyed) wasmSock.destroy();
			});
			sock.on('error', () => {});
		});

		const waitForWasm = (timeoutMs) =>
			new Promise((res, rej) => {
				if (wasmSock !== null) {
					res();
					return;
				}
				const t = setTimeout(
					() =>
						rej(
							new Error(
								'WASM GDB runtime did not connect in time',
							),
						),
					timeoutMs,
				);
				onWasmConnect = () => {
					clearTimeout(t);
					res();
				};
			});

		wsServer.listen(0, '127.0.0.1', () => {
			const wsPort = wsServer.address().port;
			tcpServer.listen(0, '127.0.0.1', () => {
				const tcpPort = tcpServer.address().port;
				resolvePort({ wsPort, tcpPort, waitForWasm });
			});
		});
	});
}

/* ── Load blytdebug.js (the WASM runtime) ───────────────────────────────────── */

function loadWasmRuntime(wasmDir, cartPath, gdbWsPort) {
	return new Promise((resolve, reject) => {
		const cartData = readFileSync(cartPath);
		globalThis.__blyt_cart_data = new Uint8Array(cartData);
		if (process.env.BLYT_TRACE) {
			globalThis.__blyt_env_vars = Object.assign(
				globalThis.__blyt_env_vars || {},
				{ BLYT_TRACE: process.env.BLYT_TRACE },
			);
		}
		globalThis.__blyt_gdb_port = gdbWsPort;
		/* Host-resolvable path to the debug cart ELF so lldb-dap can read its
		 * DWARF (issue #144): the runtime otherwise reports the in-memory
		 * "/cart.blyt", which host-side lldb cannot open. */
		globalThis.__blyt_cart_path = cartPath;
		globalThis.__blyt_init_module = {
			print: (s) => process.stdout.write(`${s}\n`),
			printErr: (s) => process.stderr.write(`${s}\n`),
			onExit: (code) => resolve(code),
		};
		try {
			const require = createRequire(import.meta.url);
			const wasmJs = existsSync(path.join(wasmDir, 'blytdebug.js'))
				? 'blytdebug.js'
				: 'blytplay.js';
			require(path.join(wasmDir, wasmJs));
		} catch (e) {
			reject(e);
		}
	});
}

/* ── Main ───────────────────────────────────────────────────────────────────── */

async function main() {
	const { wsPort, tcpPort, waitForWasm } = await startBridge();
	console.log(
		`[run_wasm_lldb_dap] bridge: ws ${wsPort} (WASM) ↔ tcp ${tcpPort} (lldb-dap)`,
	);

	/* Fire-and-forget: the WASM runtime drives its own Emscripten loop in this
	 * process.  We do NOT await its exit — after lldb-dap detaches the cart may
	 * stay paused at the breakpoint and never reach blyt_quit()/onExit; the test
	 * verdict is the lldb-dap driver's, and process.exit() tears the loop down. */
	loadWasmRuntime(WASM_DIR, CART_PATH, wsPort).catch((e) => {
		console.error('[run_wasm_lldb_dap] WASM runtime error:', e.message);
	});

	await waitForWasm(10000);
	console.log('[run_wasm_lldb_dap] WASM runtime connected over WebSocket');

	const driver = path.join(__dirname, 'run_lldb_dap_test.mjs');
	await new Promise((resolve, reject) => {
		execFile(
			process.execPath,
			[
				driver,
				LLDB_DAP,
				String(tcpPort),
				CART_PATH,
				PROJECT_CWD,
				'--test',
				TEST_NAME,
			],
			{ timeout: 90000, env: process.env },
			(err, stdout, stderr) => {
				process.stdout.write(stdout);
				process.stderr.write(stderr);
				if (err) reject(err);
				else resolve();
			},
		);
	});
}

main()
	.then(() => process.exit(0))
	.catch((e) => {
		console.error('[run_wasm_lldb_dap] FAILED:', e.message);
		process.exit(1);
	});
