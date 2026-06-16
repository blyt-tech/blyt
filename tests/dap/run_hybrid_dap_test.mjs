#!/usr/bin/env node
/*
 * tests/dap/run_hybrid_dap_test.mjs — hybrid (Lua+C) DAP test orchestrator.
 *
 * Spawns blytdebug --debug 0 --gdb 0 --headless <cart>, waits for both
 * the Lua DAP port and GDB port on stdout, then:
 *   1. Connects a minimal GDB RSP stub to the GDB port (satisfies
 *      blyt_session_gdb_wait_attached without needing lldb-dap).
 *   2. Runs dap_test.mjs against the Lua DAP port to verify that Lua
 *      breakpoints fire in hybrid mode.
 *
 * Usage:
 *   node run_hybrid_dap_test.mjs <blytdebug_path> <cart_path>
 *
 * Environment (forwarded to dap_test.mjs):
 *   BLYT_DAP_BP_LINE  — 1-based line to break on (default 3)
 *
 * Exit 0 on success, non-zero on failure.
 * Node.js 22+ required.
 */

/* Debug-driver default: every failure's captured stderr should already carry
 * a protocol/lifecycle trace.  'api' stays opt-in (high volume). */
if (!process.env.BLYT_TRACE) process.env.BLYT_TRACE = 'gdb,dap,lifecycle,frame';

import { execFile, spawn } from 'node:child_process';
import { createConnection } from 'node:net';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));

const BLYTRUN = process.argv[2];
const CART = process.argv[3];

if (!BLYTRUN || !CART) {
	process.stderr.write('usage: run_hybrid_dap_test.mjs <blytdebug> <cart>\n');
	process.exit(1);
}

/* Wait for both "DAP listening on port N" and "GDB listening on port N"
 * on proc's stdout (INFO-level libretro log → stdout via sdl_log). */
function findPorts(proc) {
	return new Promise((resolve, reject) => {
		let buf = '';
		let dapPort = null;
		let gdbPort = null;
		const timer = setTimeout(
			() =>
				reject(
					new Error('timeout: blytdebug did not print both ports'),
				),
			60000,
		);
		proc.stdout.on('data', (chunk) => {
			buf += chunk.toString();
			if (!dapPort) {
				const m = buf.match(/DAP listening on port (\d+)/);
				if (m) dapPort = parseInt(m[1], 10);
			}
			if (!gdbPort) {
				const m = buf.match(/GDB listening on port (\d+)/);
				if (m) gdbPort = parseInt(m[1], 10);
			}
			if (dapPort && gdbPort) {
				clearTimeout(timer);
				resolve({ dapPort, gdbPort });
			}
		});
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

/* Minimal GDB RSP stub: connect to the GDB port, ack any incoming RSP packets
 * with '+'.  The cart does not hit any C breakpoints during the Lua DAP test,
 * so typically no packets arrive — but we ack defensively in case of a fault. */
function connectGdbStub(port) {
	return new Promise((resolve, reject) => {
		const sock = createConnection({ host: '127.0.0.1', port });
		let buf = Buffer.alloc(0);
		sock.on('connect', () => resolve(sock));
		sock.on('error', reject);
		/* Parse RSP packets ('$<data>#<2-hex-checksum>') and send '+'. */
		sock.on('data', (chunk) => {
			buf = Buffer.concat([buf, chunk]);
			while (buf.length > 0) {
				/* Skip standalone '+' or '-' acknowledgements from the server. */
				if (buf[0] === 0x2b || buf[0] === 0x2d) {
					buf = buf.slice(1);
					continue;
				}
				const start = buf.indexOf(0x24); /* '$' */
				if (start < 0) {
					buf = Buffer.alloc(0);
					break;
				}
				if (start > 0) {
					buf = buf.slice(start);
					continue;
				}
				const hash = buf.indexOf(0x23, 1); /* '#' */
				if (hash < 0 || buf.length < hash + 3) break;
				sock.write('+');
				buf = buf.slice(hash + 3);
			}
		});
	});
}

async function main() {
	const proc = spawn(
		BLYTRUN,
		['--debug', '0', '--gdb', '0', '--headless', CART],
		{
			stdio: ['ignore', 'pipe', 'pipe'],
		},
	);
	proc.stderr.on('data', (d) => process.stderr.write(d));

	const { dapPort, gdbPort } = await findPorts(proc);

	/* Connect the GDB stub first — blytdebug calls gdb_wait_attached()
	 * only after dap_wait_ready(), so the DAP client must connect and send
	 * configurationDone before blytdebug polls for the GDB connection.
	 * We open both sockets before the DAP test so the GDB socket is ready
	 * the moment blytdebug starts waiting for it. */
	const gdbSock = await connectGdbStub(gdbPort);

	/* Run dap_test.mjs in TCP mode against the Lua DAP port.  The cart's Lua
	 * chunk name is canonicalised to /blyt/cart/src/game/lua/main.lua at build
	 * time (issue #46); breakpoints match it exactly (issue #51). */
	const testScript = path.join(__dirname, 'dap_test.mjs');
	const endpoint = `tcp://127.0.0.1:${dapPort}`;
	const bpLine = process.env.BLYT_DAP_BP_LINE || '3';

	await new Promise((resolve, reject) => {
		execFile(
			process.execPath,
			[testScript, endpoint, '/blyt/cart/src/game/lua/main.lua', bpLine],
			{ timeout: 30000, env: { ...process.env } },
			(err, stdout, stderr) => {
				process.stdout.write(stdout);
				process.stderr.write(stderr);
				if (err) reject(err);
				else resolve();
			},
		);
	});

	gdbSock.destroy();

	/* Wait for blytdebug to exit (cart calls blyt.quit()). */
	await new Promise((resolve) => {
		proc.on('exit', resolve);
		setTimeout(() => {
			proc.kill();
			resolve();
		}, 5000);
	});
}

main()
	.then(() => process.exit(0))
	.catch((e) => {
		console.error('[hybrid_dap_test] FAILED:', e.message);
		process.exit(1);
	});
