#!/usr/bin/env node
/*
 * tests/gdb/run_sdl_gdb_test.mjs — SDL2 GDB test orchestrator.
 *
 * Spawns blytrun --gdb 0 --headless <cart>, waits for the GDB TCP port to
 * appear on stdout ("blyt: GDB listening on port N"), then runs gdb_test.mjs
 * in TCP mode against that port.
 *
 * Usage:
 *   node run_sdl_gdb_test.mjs <blytrun_path> <cart_path>
 *
 * Environment:
 *   BLYT_GDB_BREAK_ADDR — hex address to set Z0 breakpoint (passed to gdb_test.mjs)
 *
 * Exit 0 on success, non-zero on failure.
 * Node.js 22+ required.
 */

'use strict';

import { execFile, spawn } from 'child_process';
import { fileURLToPath }   from 'url';
import path                from 'path';

const __dirname = path.dirname(fileURLToPath(import.meta.url));

const BLYTRUN = process.argv[2];
const CART    = process.argv[3];

if (!BLYTRUN || !CART) {
    process.stderr.write('usage: run_sdl_gdb_test.mjs <blytrun> <cart>\n');
    process.exit(1);
}

/* Wait for "blyt: GDB listening on port N" on proc's stdout or stderr. */
function findGdbPort(proc) {
    return new Promise((resolve, reject) => {
        let buf = '';
        const timer = setTimeout(
            () => reject(new Error('timeout: blytrun did not print GDB port')),
            15000
        );
        function check(chunk) {
            buf += chunk.toString();
            const m = buf.match(/GDB listening on port (\d+)/);
            if (m) { clearTimeout(timer); resolve(parseInt(m[1], 10)); }
        }
        proc.stdout.on('data', check);
        proc.stderr.on('data', check);
        proc.on('exit', (code) => {
            clearTimeout(timer);
            if (code !== null && code !== 0)
                reject(new Error(`blytrun exited early with code ${code}`));
        });
        proc.on('error', (e) => { clearTimeout(timer); reject(e); });
    });
}

async function main() {
    const blytrun = spawn(BLYTRUN, ['--gdb', '0', '--headless', CART], {
        stdio: ['ignore', 'pipe', 'pipe'],
    });
    blytrun.stderr.on('data', (d) => process.stderr.write(d));

    const port = await findGdbPort(blytrun);
    console.log(`[sdl_gdb_test] blytrun GDB on tcp://127.0.0.1:${port}`);

    const testScript = path.join(__dirname, 'gdb_test.mjs');
    const endpoint   = `tcp://127.0.0.1:${port}`;
    const extraArgs  = process.env.BLYT_GDB_BREAK_ADDR
        ? ['--break-addr', process.env.BLYT_GDB_BREAK_ADDR]
        : [];

    await new Promise((resolve, reject) => {
        execFile(
            process.execPath,
            [testScript, endpoint, ...extraArgs],
            { timeout: 30000 },
            (err, stdout, stderr) => {
                process.stdout.write(stdout);
                process.stderr.write(stderr);
                if (err) reject(err); else resolve();
            }
        );
    });

    /* gdb_test sent vCont;c; wait for blytrun to exit (cart completes). */
    await new Promise((resolve) => {
        blytrun.on('exit', resolve);
        setTimeout(() => { blytrun.kill(); resolve(); }, 5000);
    });
}

main().then(() => process.exit(0)).catch((e) => {
    console.error('[sdl_gdb_test] FAILED:', e.message);
    process.exit(1);
});
