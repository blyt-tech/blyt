#!/usr/bin/env node
/*
 * tests/gdb/gdb_test.mjs — minimal GDB RSP client.
 *
 * Connects to a GDB RSP server (TCP or WebSocket relay), performs a handshake,
 * optionally sets a software breakpoint and verifies stop/step behaviour.
 *
 * Usage:
 *   node gdb_test.mjs <endpoint> [--break-addr HEXADDR]
 *
 *   endpoint:
 *     tcp://127.0.0.1:PORT    — direct TCP connection (SDL2 / libretro)
 *     ws://127.0.0.1:PORT/gdb — WebSocket relay (WASM)
 *
 * Environment:
 *   BLYT_GDB_BREAK_ADDR          — hex address to set Z0 breakpoint (overrides --break-addr)
 *   BLYT_GDB_EXEC_FILE_CHECK      — verify qXfer:exec-file:read response
 *   BLYT_GDB_LIBRARY_CHECK        — verify qXfer:libraries-svr4:read contains libblyt32.so
 *   BLYT_GDB_FEATURES_CHECK       — verify qXfer:features:read returns target.xml with riscv arch
 *   BLYT_GDB_PROCESS_INFO         — verify qProcessInfo returns riscv32/endian:little
 *   BLYT_GDB_MEM_ADDR             — hex address to test m (memory read) packet
 *   BLYT_GDB_REGISTER_WRITE_CHECK — verify P/p register write roundtrip
 *   BLYT_GDB_THREAD_STOP_INFO     — verify qThreadStopInfo1 contains T05 after a stop
 *   BLYT_GDB_DETACH               — send D (detach) instead of final vCont;c
 *
 * Exit 0 on success, non-zero on failure.
 * Node.js 22+ required.
 */

const [, , endpoint, ...rest] = process.argv;
let breakAddr = process.env.BLYT_GDB_BREAK_ADDR || null;
for (let i = 0; i < rest.length; i++) {
	if (rest[i] === '--break-addr' && rest[i + 1]) {
		breakAddr = rest[++i];
	}
}

if (!endpoint) {
	process.stderr.write(
		'usage: gdb_test.mjs <endpoint> [--break-addr HEXADDR]\n',
	);
	process.exit(1);
}

/* ── GDB RSP framing ─────────────────────────────────────────────────────── */

function computeCsum(payload) {
	let s = 0;
	for (let i = 0; i < payload.length; i++)
		s = (s + payload.charCodeAt(i)) & 0xff;
	return s.toString(16).padStart(2, '0');
}

function frame(payload) {
	return `$${payload}#${computeCsum(payload)}`;
}

/* Parse a single RSP packet from buf.  Returns { payload, rest } or null. */
function parseOne(buf) {
	const start = buf.indexOf('$');
	if (start < 0) return null;
	const end = buf.indexOf('#', start + 1);
	if (end < 0 || buf.length < end + 3) return null;
	const payload = buf.slice(start + 1, end);
	const rest = buf.slice(end + 3); /* skip #XX */
	return { payload, rest };
}

/* ── Transport abstraction ───────────────────────────────────────────────── */

async function connect(endpointStr) {
	/* pending[] = resolve callbacks waiting for the next RSP packet */
	const pending = [];
	let buf = '';
	let closed = false;

	function onData(text) {
		/* Consume and discard any acks ('+') or naks ('-') at the front. */
		buf += text;
		buf = buf.replace(/^[+-]*/u, '');
		let pkt;
		while ((pkt = parseOne(buf)) !== null) {
			buf = pkt.rest;
			/* Each received packet must be acked; the caller does this via send(). */
			if (pending.length > 0) pending.shift()(pkt.payload);
		}
	}

	function onClose() {
		closed = true;
		for (const r of pending) r(null);
		pending.length = 0;
	}

	let sendFn, closeFn;

	if (endpointStr.startsWith('tcp://')) {
		const url = new URL(endpointStr);
		const { createConnection } = await import('node:net');
		const sock = createConnection(parseInt(url.port, 10), url.hostname);
		sock.setEncoding('utf8');
		sock.on('data', onData);
		sock.on('close', onClose);
		sock.on('error', (e) =>
			process.stderr.write(`[gdb] tcp error: ${e.message}\n`),
		);
		await new Promise((res, rej) => {
			sock.once('connect', res);
			sock.once('error', rej);
		});
		sendFn = (s) => sock.write(s);
		closeFn = () => {
			if (!closed) sock.destroy();
		};
	} else if (
		endpointStr.startsWith('ws://') ||
		endpointStr.startsWith('wss://')
	) {
		const ws = new WebSocket(endpointStr); /* Node.js 22+ built-in */
		ws.addEventListener('message', (ev) => onData(ev.data));
		ws.addEventListener('close', () => onClose());
		ws.addEventListener('error', (e) =>
			process.stderr.write(`[gdb] ws error: ${e}\n`),
		);
		await new Promise((res, rej) => {
			ws.addEventListener('open', res, { once: true });
			ws.addEventListener('error', rej, { once: true });
		});
		sendFn = (s) => ws.send(s);
		closeFn = () => {
			if (!closed) ws.close();
		};
	} else {
		throw new Error(`unsupported endpoint: ${endpointStr}`);
	}

	return {
		/* Send a raw string (ack, or framed packet). */
		send(s) {
			sendFn(s);
		},
		/* Send a framed RSP packet and wait for the server's framed response. */
		async exchange(payload) {
			sendFn('+'); /* ack any previous server-initiated packet */
			sendFn(frame(payload));
			return new Promise((r) => pending.push(r));
		},
		/* Wait for the next server packet (no send). */
		recv() {
			return new Promise((r) => pending.push(r));
		},
		close() {
			closeFn();
		},
	};
}

/* ── Main test ───────────────────────────────────────────────────────────── */

async function main() {
	let t;
	try {
		t = await connect(endpoint);
	} catch (e) {
		process.stderr.write(`[gdb_test] connect failed: ${e.message}\n`);
		process.exit(1);
	}

	try {
		/* 1. qSupported handshake. */
		const supported = await t.exchange(
			'qSupported:multiprocess+;qXfer:exec-file:read+',
		);
		console.log(`[gdb_test] qSupported → ${supported}`);

		/* 2. qAttached — confirm the stub has a process attached. */
		const attached = await t.exchange('qAttached');
		console.log(`[gdb_test] qAttached → ${attached}`);

		/* Optional: qProcessInfo (validates target triple/endianness). */
		if (process.env.BLYT_GDB_PROCESS_INFO) {
			const info = await t.exchange('qProcessInfo');
			if (!info?.includes('riscv32') || !info.includes('endian:little')) {
				process.stderr.write(
					`[gdb_test] FAIL: qProcessInfo: ${info}\n`,
				);
				process.exit(1);
			}
			console.log(
				'PASS: qProcessInfo contains riscv32 and endian:little',
			);
		}

		/* 3. vCont? — enumerate supported vCont actions. */
		const vcontOk = await t.exchange('vCont?');
		console.log(`[gdb_test] vCont? → ${vcontOk}`);

		/* 4. ? — initial stop state (may respond W00 if cart already finished). */
		t.send('+');
		t.send(frame('?'));
		const stopReason = await t.recv();
		console.log(`[gdb_test] ? → ${stopReason}`);

		/* Optional: exec-file query (validates qXfer:exec-file:read). */
		if (process.env.BLYT_GDB_EXEC_FILE_CHECK) {
			const execFile = await t.exchange('qXfer:exec-file:read::0,4000');
			if (!execFile?.startsWith('l') || execFile.length < 2) {
				process.stderr.write(
					`[gdb_test] FAIL: exec-file response: ${execFile}\n`,
				);
				process.exit(1);
			}
			console.log(`PASS: exec-file = ${execFile.slice(1).trim()}`);
		}

		/* Optional: library list query (validates qXfer:libraries-svr4:read). */
		if (process.env.BLYT_GDB_LIBRARY_CHECK) {
			const libs = await t.exchange('qXfer:libraries-svr4:read::0,8000');
			if (!libs?.includes('libblyt32.so')) {
				process.stderr.write(
					`[gdb_test] FAIL: library list missing libblyt32.so: ${libs}\n`,
				);
				process.exit(1);
			}
			console.log('PASS: library list contains libblyt32.so');
		}

		/* Optional: features query (validates qXfer:features:read / target.xml). */
		if (process.env.BLYT_GDB_FEATURES_CHECK) {
			const feat = await t.exchange(
				'qXfer:features:read:target.xml:0,4000',
			);
			if (!feat?.startsWith('l') || !feat.includes('riscv')) {
				process.stderr.write(
					`[gdb_test] FAIL: features response: ${feat}\n`,
				);
				process.exit(1);
			}
			console.log('PASS: qXfer:features:read contains riscv target.xml');
		}

		if (breakAddr) {
			const addrHex = parseInt(breakAddr, 16).toString(16);
			console.log(`[gdb_test] setting Z0 at 0x${addrHex}`);

			/* 5. Set software breakpoint. */
			const bpResp = await t.exchange(`Z0,${addrHex},4`);
			if (bpResp !== 'OK') {
				process.stderr.write(
					`[gdb_test] FAIL: Z0 response: ${bpResp}\n`,
				);
				process.exit(1);
			}
			console.log('PASS: breakpoint set');

			/* 6. Continue and wait for the T05 stop reply. */
			t.send('+');
			t.send(frame('vCont;c'));
			const stopReply = await t.recv();
			if (!stopReply?.startsWith('T05')) {
				process.stderr.write(
					`[gdb_test] FAIL: expected T05, got: ${stopReply}\n`,
				);
				process.exit(1);
			}
			console.log('PASS: GDB stop reply received');

			/* Optional: qThreadStopInfo (validates per-thread stop reason). */
			if (process.env.BLYT_GDB_THREAD_STOP_INFO) {
				const tsi = await t.exchange('qThreadStopInfo1');
				if (!tsi?.includes('T05')) {
					process.stderr.write(
						`[gdb_test] FAIL: qThreadStopInfo1: ${tsi}\n`,
					);
					process.exit(1);
				}
				console.log('PASS: qThreadStopInfo1 contains T05');
			}

			/* 7. Read all registers — verify PC (reg 32) is non-zero.
			 * The reply is 33×4 bytes little-endian = 264 hex chars. */
			const regs = await t.exchange('g');
			if (regs && regs.length >= 264) {
				const pcHex = regs.slice(256, 264);
				const pc = parseInt(pcHex.match(/../g).reverse().join(''), 16);
				if (pc === 0) {
					process.stderr.write(
						'[gdb_test] FAIL: PC is zero after stop\n',
					);
					process.exit(1);
				}
				console.log(`PASS: PC = 0x${pc.toString(16)}`);
			} else {
				console.log(`[gdb_test] register reply: ${regs}`);
			}

			/* Optional: single register write + read roundtrip (validates P packet).
			 * Save and restore x1 (ra) so the breakpointed function can still
			 * return correctly after the test — corrupting ra without restoring it
			 * would send execution to an invalid address and hang the emulator. */
			if (process.env.BLYT_GDB_REGISTER_WRITE_CHECK) {
				const savedRa = await t.exchange('p1');
				const pResp = await t.exchange('P1:cdab3412');
				if (pResp !== 'OK') {
					process.stderr.write(
						`[gdb_test] FAIL: P1 response: ${pResp}\n`,
					);
					process.exit(1);
				}
				const pRead = await t.exchange('p1');
				if (pRead !== 'cdab3412') {
					process.stderr.write(
						`[gdb_test] FAIL: p1 after P1: ${pRead}\n`,
					);
					process.exit(1);
				}
				await t.exchange(`P1:${savedRa}`);
				console.log('PASS: P/p register write roundtrip');
			}

			/* Optional: memory read at a known address. */
			if (process.env.BLYT_GDB_MEM_ADDR) {
				const memAddr = parseInt(
					process.env.BLYT_GDB_MEM_ADDR,
					16,
				).toString(16);
				const memResp = await t.exchange(`m${memAddr},4`);
				if (memResp?.length !== 8 || memResp.startsWith('E')) {
					process.stderr.write(
						`[gdb_test] FAIL: m response for 0x${memAddr}: ${memResp}\n`,
					);
					process.exit(1);
				}
				console.log(`PASS: memory read 0x${memAddr} = 0x${memResp}`);
			}

			/* 8. Single-step. */
			t.send('+');
			t.send(frame('vCont;s'));
			const stepReply = await t.recv();
			if (!stepReply?.startsWith('T05')) {
				process.stderr.write(
					`[gdb_test] FAIL: expected T05 after step, got: ${stepReply}\n`,
				);
				process.exit(1);
			}
			console.log('PASS: step response received');

			/* 9. Clear breakpoint then detach (BLYT_GDB_DETACH=1) or continue. */
			await t.exchange(`z0,${addrHex},4`);
			if (process.env.BLYT_GDB_DETACH) {
				const detachResp = await t.exchange('D');
				if (detachResp !== 'OK') {
					process.stderr.write(
						`[gdb_test] FAIL: D response: ${detachResp}\n`,
					);
					process.exit(1);
				}
				console.log('PASS: detach OK');
				t.close();
				return; /* cart continues freely; don't wait for session end */
			}
			t.send('+');
			t.send(frame('vCont;c'));
		} else {
			/* No breakpoint address supplied — just continue to let cart run. */
			t.send('+');
			t.send(frame('vCont;c'));
		}

		/* Wait for the connection to close (cart exited or server sent W/X). */
		process.stderr.write('[gdb_test] waiting for session end...\n');
		const final = await t.recv();
		process.stderr.write(
			`[gdb_test] recv resolved: ${JSON.stringify(final)}\n`,
		);

		console.log('PASS: GDB session complete');
	} finally {
		t.close();
	}
}

main()
	.then(() => process.exit(0))
	.catch((e) => {
		console.error('[gdb_test] FAILED:', e.message);
		process.exit(1);
	});
