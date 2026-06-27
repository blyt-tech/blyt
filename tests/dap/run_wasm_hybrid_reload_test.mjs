#!/usr/bin/env node
/*
 * tests/dap/run_wasm_hybrid_reload_test.mjs — dual-client HYBRID
 * reload-while-debugging driver for the WASM browser-relay path (issue #165).
 *
 * The WASM equivalent of run_sdl_hybrid_reload_test.mjs (issue #119, acceptance
 * criteria 4 + 5): ONE hybrid debug session with BOTH debug views live at once —
 *   - native (C):  lldb-dap (child, DAP over stdio) ──TCP──▶ [WS↔TCP bridge]
 *                  ──WebSocket──▶ the in-process WASM runtime's GDB RSP.
 *   - Lua:         an inline DAP client ──WebSocket──▶ [WS relay] ──WebSocket──▶
 *                  the WASM runtime's companion Lua DAP.
 * — plus the in-process reload (a blyt_dev_ctrl_command ccall on the Emscripten
 * Module; the WASM runtime has no dev-control TCP port, unlike the SDL player).
 *
 * Both views set a breakpoint inside init() (native: a line in the Lua-exported
 * C function init() calls; Lua: the native call-site line).  init() runs once at
 * startup (both fire) and again after a hot reload; we drive the reload and
 * assert BOTH init() breakpoints fire again AFTER the reload (criterion 4) while
 * neither client is torn down (criterion 5).  __blyt_cart_path is repointed at
 * the rebuilt cart so lldb re-reads the new DWARF after the swap (issue #144).
 *
 * Usage:
 *   node run_wasm_hybrid_reload_test.mjs \
 *       <wasm_dir> <lldb-dap> <v1-cart> <cwd> <v2-cart>
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
import { createHash } from 'node:crypto';
import { existsSync, readFileSync } from 'node:fs';
import { createServer } from 'node:http';
import { createRequire } from 'node:module';
import { createServer as createTcpServer } from 'node:net';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));

const [, , WASM_DIR, LLDB_DAP, V1_CART, PROJECT_CWD, V2_CART] = process.argv;
if (!WASM_DIR || !LLDB_DAP || !V1_CART || !PROJECT_CWD || !V2_CART) {
	process.stderr.write(
		'usage: run_wasm_hybrid_reload_test.mjs <wasm_dir> <lldb-dap> ' +
			'<v1-cart> <cwd> <v2-cart>\n',
	);
	process.exit(1);
}

const nativeBreakLine = parseInt(process.env.BLYT_NATIVE_BREAK_LINE || '0', 10);
const nativeSourceFile =
	process.env.BLYT_NATIVE_SOURCE_FILE || 'src/game/c/main.c';
const luaBreakLine = parseInt(process.env.BLYT_LUA_BREAK_LINE || '0', 10);
const luaSource =
	process.env.BLYT_LUA_SOURCE || '/blyt/cart/src/game/lua/main.lua';
const programPath = process.env.BLYT_STUB_PROGRAM || V1_CART;

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
		log(`  PASS: ${msg}`);
		passed++;
	} else {
		log(`  FAIL: ${msg}`);
		failed++;
	}
}
const log = (s) => process.stderr.write(`${s}\n`);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function waitFor(pred, timeoutMs, desc) {
	const deadline = Date.now() + timeoutMs;
	while (Date.now() < deadline) {
		if (pred()) return true;
		await sleep(50);
	}
	throw new Error(`timeout waiting for ${desc}`);
}

function sourceMapCommand() {
	const pairs = [['/blyt/cart', PROJECT_CWD]];
	try {
		const manifest = JSON.parse(
			readFileSync(
				path.join(PROJECT_CWD, 'build', 'source-map.json'),
				'utf8',
			),
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

/* ── WebSocket server framing (server speaks to the WASM client) ───────────── */

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

/* ── GDB WS (WASM) ↔ TCP (lldb-dap) bridge ─────────────────────────────────── */
function startGdbBridge() {
	return new Promise((resolvePort) => {
		let wasmSock = null;
		let wasmBuf = Buffer.alloc(0);
		let tcpSock = null;
		let onWasmConnect = null;

		const wsServer = createServer();
		wsServer.on('upgrade', (req, socket) => {
			if (req.url !== '/gdb') {
				socket.destroy();
				return;
			}
			if (!wsHandshake(req, socket)) return;
			wasmSock = socket;
			/* see tcpServer note below (issue #179) */
			socket.setNoDelay(true);
			socket.on('data', (chunk) => {
				wasmBuf = Buffer.concat([wasmBuf, chunk]);
				wasmBuf = wsParseFrames(wasmBuf, (text) => {
					if (text === null) {
						if (tcpSock && !tcpSock.destroyed) tcpSock.end();
						return;
					}
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
			/* Disable Nagle on both relay legs (issue #179).  Forwarding small RSP
			 * packets one at a time with Nagle on costs a ~40 ms delayed-ACK per
			 * round-trip (occasionally a ~200 ms persist-timer stall), stacking a
			 * fixed ~700 ms onto the post-reload breakpoint re-arm that overran the
			 * orchestrator's waitFor under CI load and timed the test out. */
			sock.setNoDelay(true);
			sock.on('data', (chunk) => {
				if (wasmSock && !wasmSock.destroyed)
					wasmSock.write(wsFrame(chunk.toString('utf8')));
			});
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

/* ── Lua DAP WS relay (two WS clients bridged: WASM runtime + inline client) ── */
function startDapRelay() {
	return new Promise((resolvePort) => {
		let sideA = null; /* WASM runtime */
		let sideB = null; /* inline Lua client */
		let onWasmConnect = null;
		function relay(from, to) {
			from.onmsg = (text) => {
				if (text === null) return;
				if (to && !to.closed) to.socket.write(wsFrame(text));
			};
		}
		function makeSide(socket) {
			/* no Nagle on the Lua DAP relay either (issue #179) */
			socket.setNoDelay(true);
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
				if (onWasmConnect) {
					onWasmConnect();
					onWasmConnect = null;
				}
			} else if (sideB === null) {
				sideB = makeSide(socket);
				relay(sideA, sideB);
				relay(sideB, sideA);
			}
		});
		const waitForWasm = (timeoutMs) =>
			new Promise((res, rej) => {
				if (sideA !== null) {
					res();
					return;
				}
				const t = setTimeout(
					() =>
						rej(
							new Error(
								'WASM DAP runtime did not connect in time',
							),
						),
					timeoutMs,
				);
				onWasmConnect = () => {
					clearTimeout(t);
					res();
				};
			});
		server.listen(0, '127.0.0.1', () =>
			resolvePort({ port: server.address().port, waitForWasm }),
		);
	});
}

/* ── DAP Content-Length framing + endpoint (lldb-dap stdio + Lua WS) ───────── */

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

/* Dispatch a decoded DAP message to pending requests / event handlers, and send
 * via the supplied `frame` encoder. */
function makeDapCore(send, label, frame) {
	let seq = 1;
	const pending = new Map();
	const eventHandlers = {};
	function dispatch(msg) {
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
	}
	function request(command, args) {
		const id = seq++;
		send(
			frame({ seq: id, type: 'request', command, arguments: args || {} }),
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
	return { request, onEvent, dispatch };
}

/* lldb-dap over stdio: Content-Length framing. */
function makeDapEndpoint(write, label) {
	const core = makeDapCore(write, label, clFrame);
	return { ...core, onData: makeFramer(core.dispatch) };
}

/* Lua DAP over the WS relay: one raw JSON object per WebSocket frame (the WASM
 * Lua DAP transport's framing — see reload_dap_test.mjs), NOT Content-Length. */
function makeWsJsonDapEndpoint(send, label) {
	const core = makeDapCore(send, label, (obj) => JSON.stringify(obj));
	return {
		...core,
		onText: (text) => {
			let msg;
			try {
				msg = JSON.parse(text);
			} catch {
				return;
			}
			core.dispatch(msg);
		},
	};
}

/* ── WASM runtime load (in-process) with reload ccall stubs ────────────────── */

let M = null;
const devCtrlResponses = [];

function loadWasmRuntime(gdbWsPort, dapWsPort) {
	return new Promise((resolve, reject) => {
		globalThis.__blyt_cart_data = new Uint8Array(readFileSync(V1_CART));
		globalThis.__blyt_env_vars = {
			BLYT_SAVE_DIR: '/blyt_save',
			...(process.env.BLYT_TRACE
				? { BLYT_TRACE: process.env.BLYT_TRACE }
				: {}),
		};
		globalThis.__blyt_gdb_port = gdbWsPort;
		globalThis.__blyt_dap_port = dapWsPort;
		/* Host-resolvable cart path so lldb reads the cart's DWARF (issue #144). */
		globalThis.__blyt_cart_path = V1_CART;
		globalThis.blyt_dev_ctrl_send = (json) => devCtrlResponses.push(json);
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

/* Drive the reload straight into the C handler (the WASM runtime has no
 * dev-control TCP port): write v2 into MEMFS, repoint the host cart path so lldb
 * re-reads the new DWARF, then ccall blyt_dev_ctrl_command(reload). */
function driveReload() {
	globalThis.__blyt_cart_path = V2_CART;
	globalThis.blyt_dev_ctrl_fetch_cart = () => {
		M.FS.writeFile('/cart.blyt', new Uint8Array(readFileSync(V2_CART)));
		M.ccall('blyt_dev_ctrl_reload_fetched', null, ['int'], [1]);
	};
	const before = devCtrlResponses.length;
	M.ccall(
		'blyt_dev_ctrl_command',
		null,
		['string'],
		['{"id":1,"cmd":"reload"}'],
	);
	return devCtrlResponses
		.slice(before)
		.map((r) => JSON.parse(r))
		.find((r) => r.cmd === 'reload');
}

/* ── Main ──────────────────────────────────────────────────────────────────── */

const nativeHits = { startup: 0, reloaded: 0 };
const luaHits = { startup: 0, reloaded: 0 };
let phase = 'startup';

async function main() {
	const gdb = await startGdbBridge();
	const dap = await startDapRelay();
	log(
		`[run_wasm_hybrid_reload] gdb ws ${gdb.wsPort}↔tcp ${gdb.tcpPort}, dap relay ${dap.port}`,
	);

	const runtimeDone = loadWasmRuntime(gdb.wsPort, dap.port).catch((e) =>
		log(`[run_wasm_hybrid_reload] WASM runtime error: ${e.message}`),
	);

	/* Both debug WebSockets must connect before we drive the clients. */
	await gdb.waitForWasm(10000);
	await dap.waitForWasm(10000);
	log('[run_wasm_hybrid_reload] WASM runtime connected (gdb + dap)');

	/* ── native: lldb-dap child over stdio ──────────────────────────────────── */
	const lldb = spawn(LLDB_DAP, [], { stdio: ['pipe', 'pipe', 'pipe'] });
	lldb.stderr.on('data', (d) => process.stderr.write(`[lldb-dap] ${d}`));
	const native = makeDapEndpoint((s) => lldb.stdin.write(s), 'native');
	lldb.stdout.on('data', native.onData);

	native.onEvent('stopped', (ev) => {
		const reason = ev.body?.reason;
		const tid = ev.body?.threadId ?? 1;
		if (reason === 'breakpoint') {
			nativeHits[phase]++;
			log(`[native] breakpoint hit (phase=${phase})`);
		} else {
			log(
				`[native] auto-continue stop reason=${reason} (phase=${phase})`,
			);
		}
		native.request('continue', { threadId: tid }).catch(() => {});
	});

	/* ── Lua: inline DAP client over the WS relay ───────────────────────────── */
	const luaWs = new WebSocket(`ws://127.0.0.1:${dap.port}/dap`);
	const lua = makeWsJsonDapEndpoint((s) => luaWs.send(s), 'lua');
	luaWs.addEventListener('message', (ev) =>
		lua.onText(typeof ev.data === 'string' ? ev.data : ev.data.toString()),
	);
	await new Promise((res, rej) => {
		luaWs.addEventListener('open', res);
		luaWs.addEventListener('error', rej);
	});

	lua.onEvent('stopped', (ev) => {
		const reason = ev.body?.reason;
		const tid = ev.body?.threadId ?? 1;
		if (reason === 'breakpoint' || reason === 'step') luaHits[phase]++;
		log(`[lua] stop reason=${reason} (phase=${phase})`);
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
				`gdb-remote 127.0.0.1:${gdb.tcpPort}`,
			],
		})
		.catch((e) => log(`[native] launch error: ${e.message}`));
	const nbp = await native.request('setBreakpoints', {
		source: { path: `/blyt/cart/${nativeSourceFile}` },
		breakpoints: [{ line: nativeBreakLine }],
	});
	assert(
		nbp.body?.breakpoints?.[0]?.verified === true,
		`native breakpoint verified at attach (${nativeSourceFile}:${nativeBreakLine})`,
	);
	await native.request('configurationDone');

	/* ── Lua: initialize, launch, setBreakpoints, configurationDone ─────────── */
	await lua.request('initialize', {
		clientID: 'blyt-wasm-hybrid-reload',
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
	await lua.request('configurationDone');
	await nativeLaunchP;

	/* ── Startup init(): both breakpoints fire ──────────────────────────────── */
	await waitFor(
		() => nativeHits.startup >= 1 && luaHits.startup >= 1,
		20000,
		'both init() breakpoints to fire at startup',
	);
	assert(
		nativeHits.startup >= 1,
		'native init() breakpoint fired at startup',
	);
	assert(luaHits.startup >= 1, 'Lua init() breakpoint fired at startup');

	await sleep(500);

	/* ── Hot reload (in-process ccall) → init() re-runs ─────────────────────── */
	phase = 'reloaded';
	const reloadResp = driveReload();
	log(`[reload] runtime reply: ${JSON.stringify(reloadResp)}`);
	assert(
		reloadResp && reloadResp.status === 'ok',
		`reload acknowledged ok (got ${JSON.stringify(reloadResp)})`,
	);

	/* After the reload both views — re-armed before init() by the post-reload
	 * gate — must fire their init() breakpoint again (criterion 4). */
	try {
		await waitFor(
			() => nativeHits.reloaded >= 1 && luaHits.reloaded >= 1,
			25000,
			'both init() breakpoints to fire after reload',
		);
	} catch (e) {
		log(
			`[reload] post-reload fire counts: native=${nativeHits.reloaded} lua=${luaHits.reloaded}`,
		);
		throw e;
	}
	assert(
		nativeHits.reloaded >= 1,
		'native init() breakpoint fired AFTER reload (criterion 4)',
	);
	assert(
		luaHits.reloaded >= 1,
		'Lua init() breakpoint fired AFTER reload (criterion 4)',
	);

	/* ── Criterion 5: both connections persisted across the reload ──────────── */
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
	luaWs.close();
	void runtimeDone;
}

main()
	.then(() => {
		log(`\n${passed} passed, ${failed} failed`);
		process.exit(failed > 0 ? 1 : 0);
	})
	.catch((err) => {
		log(`\nERROR: ${err.stack || err.message}`);
		process.exit(2);
	});
