#!/usr/bin/env node
/*
 * tests/gdb/run_sdl_hot_block_bp_test.mjs — mid-run breakpoint into an
 * already-translated ("hot") block (issue #146).
 *
 * Spawns blytdebug --gdb 0 --headless <cart>, connects a GDB RSP TCP client,
 * and continues WITHOUT a breakpoint so the cart's hot function executes many
 * times and rv32emu translates its block.  Only THEN does it interrupt (\x03),
 * insert a Z0 software breakpoint at an address *inside* that hot block
 * (BLYT_GDB_BREAK_ADDR — deliberately a mid-block label, not a block entry, so
 * the run-loop's check_break cannot catch it and the in-memory ebreak is the
 * only mechanism that can), and continue again.
 *
 * With the stale-block bug present, the cached block still holds the original
 * (un-patched) instruction, the ebreak never executes, and no stop ever
 * arrives — this driver times out.  With the block-cache flush fix, the block
 * re-translates with the ebreak and a T05 stop reply arrives.
 *
 * Usage:
 *   node run_sdl_hot_block_bp_test.mjs <blytdebug_path> <cart_path>
 *
 * Environment:
 *   BLYT_GDB_BREAK_ADDR — hex address of the mid-block breakpoint.
 *
 * Exit 0 on success, non-zero on failure.
 * Node.js 22+ required.
 */

/* Debug-driver default: every failure's captured stderr should already carry
 * a protocol/lifecycle trace.  'api' stays opt-in (high volume). */
if (!process.env.BLYT_TRACE) process.env.BLYT_TRACE = 'gdb,dap,lifecycle,frame';

import { spawn } from 'node:child_process';
import { createConnection } from 'node:net';

const BLYTRUN = process.argv[2];
const CART = process.argv[3];
const BREAK_ADDR = process.env.BLYT_GDB_BREAK_ADDR;

if (!BLYTRUN || !CART) {
	process.stderr.write(
		'usage: run_sdl_hot_block_bp_test.mjs <blytdebug> <cart>\n',
	);
	process.exit(1);
}
if (!BREAK_ADDR) {
	process.stderr.write('BLYT_GDB_BREAK_ADDR must be set\n');
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

function parseOne(buf) {
	const start = buf.indexOf('$');
	if (start < 0) return null;
	const end = buf.indexOf('#', start + 1);
	if (end < 0 || buf.length < end + 3) return null;
	return { payload: buf.slice(start + 1, end), rest: buf.slice(end + 3) };
}

/* ── Port discovery ──────────────────────────────────────────────────────── */

function findGdbPort(proc) {
	return new Promise((resolve, reject) => {
		let buf = '';
		const timer = setTimeout(
			() =>
				reject(new Error('timeout: blytdebug did not print GDB port')),
			60000,
		);
		function check(chunk) {
			buf += chunk.toString();
			const m = buf.match(/GDB listening on port (\d+)/);
			if (m) {
				clearTimeout(timer);
				resolve(parseInt(m[1], 10));
			}
		}
		proc.stdout.on('data', check);
		proc.stderr.on('data', check);
		proc.on('exit', (code) => {
			clearTimeout(timer);
			if (code !== null && code !== 0)
				reject(new Error(`blytdebug exited early with code ${code}`));
		});
		proc.on('error', (e) => {
			clearTimeout(timer);
			reject(e);
		});
	});
}

/* ── Low-level TCP GDB client ────────────────────────────────────────────── */

function makeClient(port) {
	return new Promise((resolve, reject) => {
		const pending = [];
		let buf = '';
		let closed = false;

		const sock = createConnection(port, '127.0.0.1');
		sock.setEncoding('utf8');
		sock.on('data', (text) => {
			buf += text;
			buf = buf.replace(/^[+-]*/u, '');
			let pkt;
			while ((pkt = parseOne(buf)) !== null) {
				buf = pkt.rest;
				if (pending.length > 0) pending.shift()(pkt.payload);
			}
		});
		sock.on('close', () => {
			closed = true;
			for (const r of pending) r(null);
			pending.length = 0;
		});
		sock.on('error', (e) => reject(e));
		sock.once('connect', () =>
			resolve({
				send(s) {
					sock.write(s);
				},
				async exchange(payload) {
					sock.write('+');
					sock.write(frame(payload));
					return new Promise((r) => pending.push(r));
				},
				recv() {
					return new Promise((r) => pending.push(r));
				},
				/* Send a raw byte (no RSP framing). */
				sendRaw(byte) {
					sock.write(byte);
				},
				close() {
					if (!closed) sock.destroy();
				},
			}),
		);
	});
}

function recvWithTimeout(t, ms, what) {
	return Promise.race([
		t.recv(),
		new Promise((_, rej) =>
			setTimeout(() => rej(new Error(`timeout waiting for ${what}`)), ms),
		),
	]);
}

/* ── Extract PC from 'g' reply ──────────────────────────────────────────── */

function pcFromRegs(regs) {
	if (!regs || regs.length < 264) return null;
	const pcHex = regs.slice(256, 264);
	return parseInt(pcHex.match(/../g).reverse().join(''), 16);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

async function main() {
	const blytdebug = spawn(BLYTRUN, ['--gdb', '0', '--headless', CART], {
		stdio: ['ignore', 'pipe', 'pipe'],
	});
	blytdebug.stderr.on('data', (d) => process.stderr.write(d));

	const port = await findGdbPort(blytdebug);
	console.log(`[hot_block_bp] blytdebug GDB on tcp://127.0.0.1:${port}`);

	const t = await makeClient(port);

	try {
		/* Handshake. */
		const sup = await t.exchange('qSupported:multiprocess+');
		console.log(`[hot_block_bp] qSupported → ${sup}`);
		await t.exchange('qAttached');
		t.send('+');
		t.send(frame('?'));
		await t.recv();

		/* Continue with NO breakpoint so the hot function's block is translated
		 * and executed many times before we touch it. */
		t.send('+');
		t.send(frame('vCont;c'));
		console.log('[hot_block_bp] sent vCont;c, letting the block go hot…');
		await new Promise((r) => setTimeout(r, 300));

		/* Interrupt so the breakpoint is inserted while the cart is parked. */
		t.sendRaw('\x03');
		const t02 = await recvWithTimeout(t, 30000, 'T02 (interrupt)');
		if (!t02?.startsWith('T02')) {
			process.stderr.write(
				`[hot_block_bp] FAIL: expected T02, got: ${t02}\n`,
			);
			process.exit(1);
		}
		console.log('[hot_block_bp] interrupted (T02)');

		/* Insert the breakpoint INTO the already-translated block. */
		const bp = await t.exchange(`Z0,${BREAK_ADDR},4`);
		if (bp !== 'OK') {
			process.stderr.write(`[hot_block_bp] FAIL: Z0 → ${bp}\n`);
			process.exit(1);
		}
		console.log(`[hot_block_bp] Z0 set at 0x${BREAK_ADDR}`);

		/* Continue — the patched ebreak must fire on the re-translated block. */
		t.send('+');
		t.send(frame('vCont;c'));
		const stop = await recvWithTimeout(
			t,
			30000,
			'T05 (mid-block breakpoint)',
		);
		if (!stop?.startsWith('T05')) {
			process.stderr.write(
				`[hot_block_bp] FAIL: expected T05 at the mid-block breakpoint, got: ${stop}\n`,
			);
			process.exit(1);
		}
		console.log('PASS: mid-block breakpoint in a hot block hit (T05)');

		/* Confirm the stop is at the breakpoint address. */
		const regs = await t.exchange('g');
		const pc = pcFromRegs(regs);
		const want = parseInt(BREAK_ADDR, 16);
		if (pc !== null && pc !== want) {
			process.stderr.write(
				`[hot_block_bp] WARN: PC 0x${pc.toString(16)} != expected 0x${want.toString(16)}\n`,
			);
		}

		/* Detach so the cart can exit. */
		const detach = await t.exchange('D');
		if (detach !== 'OK') {
			process.stderr.write(`[hot_block_bp] FAIL: D → ${detach}\n`);
			process.exit(1);
		}
		console.log('PASS: detached');
	} finally {
		t.close();
	}

	await new Promise((resolve) => {
		blytdebug.on('exit', resolve);
		setTimeout(() => {
			blytdebug.kill();
			resolve();
		}, 5000);
	});
}

main()
	.then(() => process.exit(0))
	.catch((e) => {
		console.error('[hot_block_bp] FAILED:', e.message);
		process.exit(1);
	});
