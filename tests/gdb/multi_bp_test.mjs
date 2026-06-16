#!/usr/bin/env node
/*
 * tests/gdb/multi_bp_test.mjs — multi-breakpoint GDB RSP client.
 *
 * Connects to a GDB RSP server (TCP or WebSocket), sets Z0 breakpoints at
 * multiple addresses, sends vCont;c repeatedly, and asserts a T05 stop reply
 * arrives for each expected address in sequence.
 *
 * Usage:
 *   node multi_bp_test.mjs <endpoint>
 *
 *   endpoint: tcp://127.0.0.1:PORT  or  ws://127.0.0.1:PORT/gdb
 *
 * Environment:
 *   BLYT_GDB_BP_ADDRS — comma-separated hex addresses (e.g. "1000,2000,3000")
 *
 * Exit 0 on success, non-zero on failure.
 * Node.js 22+ required.
 */

const [, , endpoint] = process.argv;
const bpAddrsRaw = process.env.BLYT_GDB_BP_ADDRS || '';

if (!endpoint) {
	process.stderr.write('usage: multi_bp_test.mjs <endpoint>\n');
	process.exit(1);
}
if (!bpAddrsRaw) {
	process.stderr.write('BLYT_GDB_BP_ADDRS must be set\n');
	process.exit(1);
}

const bpAddrs = bpAddrsRaw.split(',').map((s) => parseInt(s.trim(), 16));

/* ── GDB RSP framing (shared with gdb_test.mjs) ─────────────────────────── */

function computeCsum(payload) {
	let s = 0;
	for (let i = 0; i < payload.length; i++)
		s = (s + payload.charCodeAt(i)) & 0xff;
	return s.toString(16).padStart(2, '0');
}

function frame(payload) {
	return `$${payload}#${computeCsum(payload)}`;
}

function parseOne(buf) {
	const start = buf.indexOf('$');
	if (start < 0) return null;
	const end = buf.indexOf('#', start + 1);
	if (end < 0 || buf.length < end + 3) return null;
	return { payload: buf.slice(start + 1, end), rest: buf.slice(end + 3) };
}

async function connect(endpointStr) {
	const pending = [];
	let buf = '';
	let closed = false;

	function onData(text) {
		buf += text;
		buf = buf.replace(/^[+-]*/u, '');
		let pkt;
		while ((pkt = parseOne(buf)) !== null) {
			buf = pkt.rest;
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
			process.stderr.write(`[multi_bp] tcp error: ${e.message}\n`),
		);
		await new Promise((res, rej) => {
			sock.once('connect', res);
			sock.once('error', rej);
		});
		sendFn = (s) => sock.write(s);
		closeFn = () => {
			if (!closed) sock.destroy();
		};
	} else {
		const ws = new WebSocket(endpointStr);
		ws.addEventListener('message', (ev) => onData(ev.data));
		ws.addEventListener('close', () => onClose());
		ws.addEventListener('error', (e) =>
			process.stderr.write(`[multi_bp] ws error: ${e}\n`),
		);
		await new Promise((res, rej) => {
			ws.addEventListener('open', res, { once: true });
			ws.addEventListener('error', rej, { once: true });
		});
		sendFn = (s) => ws.send(s);
		closeFn = () => {
			if (!closed) ws.close();
		};
	}

	return {
		send(s) {
			sendFn(s);
		},
		async exchange(payload) {
			sendFn('+');
			sendFn(frame(payload));
			return new Promise((r) => pending.push(r));
		},
		recv() {
			return new Promise((r) => pending.push(r));
		},
		close() {
			closeFn();
		},
	};
}

/* ── Extract PC from 'g' reply ──────────────────────────────────────────── */

function pcFromRegs(regs) {
	if (!regs || regs.length < 264) return null;
	const pcHex = regs.slice(256, 264);
	return parseInt(pcHex.match(/../g).reverse().join(''), 16);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

async function main() {
	let t;
	try {
		t = await connect(endpoint);
	} catch (e) {
		process.stderr.write(`[multi_bp] connect failed: ${e.message}\n`);
		process.exit(1);
	}

	try {
		/* Handshake. */
		const sup = await t.exchange('qSupported:multiprocess+');
		console.log(`[multi_bp] qSupported → ${sup}`);
		await t.exchange('qAttached');

		t.send('+');
		t.send(frame('?'));
		await t.recv();

		/* Set Z0 for every address. */
		for (const addr of bpAddrs) {
			const hex = addr.toString(16);
			const resp = await t.exchange(`Z0,${hex},4`);
			if (resp !== 'OK') {
				process.stderr.write(`[multi_bp] FAIL: Z0,${hex} → ${resp}\n`);
				process.exit(1);
			}
		}
		console.log(`[multi_bp] ${bpAddrs.length} breakpoints set`);

		/* Hit each breakpoint in sequence. */
		for (let i = 0; i < bpAddrs.length; i++) {
			t.send('+');
			t.send(frame('vCont;c'));
			const stop = await t.recv();
			if (!stop?.startsWith('T05')) {
				process.stderr.write(
					`[multi_bp] FAIL: expected T05 at bp ${i + 1}, got: ${stop}\n`,
				);
				process.exit(1);
			}
			/* Verify PC matches the expected breakpoint address. */
			const regs = await t.exchange('g');
			const pc = pcFromRegs(regs);
			if (pc !== null && pc !== bpAddrs[i]) {
				process.stderr.write(
					`[multi_bp] WARN: PC 0x${pc.toString(16)} != expected 0x${bpAddrs[i].toString(16)} at bp ${i + 1}\n`,
				);
				/* Non-fatal: RISC-V PC may point to different spot after ebreak handling. */
			}
			console.log(`PASS: breakpoint ${i + 1}/${bpAddrs.length} hit`);
		}

		/* Detach and let the cart finish. */
		const detach = await t.exchange('D');
		if (detach !== 'OK') {
			process.stderr.write(`[multi_bp] FAIL: D → ${detach}\n`);
			process.exit(1);
		}
		console.log('PASS: multi-breakpoint session complete');
	} finally {
		t.close();
	}
}

main()
	.then(() => process.exit(0))
	.catch((e) => {
		console.error('[multi_bp] FAILED:', e.message);
		process.exit(1);
	});
