#!/usr/bin/env node
/*
 * tests/gdb/run_sdl_gdb_test.mjs — SDL2 GDB test orchestrator.
 *
 * Spawns blytplay --gdb 0 --headless <cart>, waits for the GDB TCP port to
 * appear on stdout ("blyt: GDB listening on port N"), then runs gdb_test.mjs
 * in TCP mode against that port.
 *
 * Usage:
 *   node run_sdl_gdb_test.mjs <blytplay_path> <cart_path>
 *
 * Environment:
 *   BLYT_GDB_BREAK_ADDR — hex address to set Z0 breakpoint (passed to gdb_test.mjs)
 *
 * Exit 0 on success, non-zero on failure.
 * Node.js 22+ required.
 */

/* Debug-driver default: every failure's captured stderr should already carry
 * a protocol/lifecycle trace.  'api' stays opt-in (high volume). */
if (!process.env.BLYT_TRACE) process.env.BLYT_TRACE = 'gdb,dap,lifecycle,frame';

import { execFile, spawn } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));

const BLYTRUN = process.argv[2];
const CART = process.argv[3];

if (!BLYTRUN || !CART) {
	process.stderr.write('usage: run_sdl_gdb_test.mjs <blytplay> <cart>\n');
	process.exit(1);
}

/* Wait for "blyt: GDB listening on port N" on proc's stdout or stderr. */
function findGdbPort(proc) {
	return new Promise((resolve, reject) => {
		let buf = '';
		const timer = setTimeout(
			() => reject(new Error('timeout: blytplay did not print GDB port')),
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
				reject(new Error(`blytplay exited early with code ${code}`));
		});
		proc.on('error', (e) => {
			clearTimeout(timer);
			reject(e);
		});
	});
}

async function main() {
	// #251: BLYT_HOSTLUA + --host-lua opts a hybrid onto the native host-Lua path,
	// so the GDB stub attaches to its native rv32 half while the Lua half runs on
	// the host VM (GDB-only here — no DAP).
	const hostLuaArgs = process.env.BLYT_HOSTLUA ? ['--host-lua'] : [];
	const blytplay = spawn(
		BLYTRUN,
		['--gdb', '0', '--headless', ...hostLuaArgs, CART],
		{
			stdio: ['ignore', 'pipe', 'pipe'],
		},
	);
	blytplay.stderr.on('data', (d) => process.stderr.write(d));

	const port = await findGdbPort(blytplay);
	console.log(`[sdl_gdb_test] blytplay GDB on tcp://127.0.0.1:${port}`);

	const testScript = path.join(__dirname, 'gdb_test.mjs');
	const endpoint = `tcp://127.0.0.1:${port}`;
	const extraArgs = process.env.BLYT_GDB_BREAK_ADDR
		? ['--break-addr', process.env.BLYT_GDB_BREAK_ADDR]
		: [];

	await new Promise((resolve, reject) => {
		execFile(
			process.execPath,
			[testScript, endpoint, ...extraArgs],
			{ timeout: 120000 },
			(err, stdout, stderr) => {
				process.stdout.write(stdout);
				process.stderr.write(stderr);
				if (err) reject(err);
				else resolve();
			},
		);
	});

	/* gdb_test sent vCont;c; wait for blytplay to exit (cart completes). */
	await new Promise((resolve) => {
		blytplay.on('exit', resolve);
		setTimeout(() => {
			blytplay.kill();
			resolve();
		}, 5000);
	});
}

main()
	.then(() => process.exit(0))
	.catch((e) => {
		console.error('[sdl_gdb_test] FAILED:', e.message);
		process.exit(1);
	});
