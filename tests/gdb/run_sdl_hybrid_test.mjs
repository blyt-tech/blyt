#!/usr/bin/env node
/*
 * tests/gdb/run_sdl_hybrid_test.mjs — SDL2 hybrid (DAP + GDB) test orchestrator.
 *
 * Spawns blytplay --debug 0 --gdb 0 --headless <cart>, waits for both the DAP
 * and GDB TCP ports, then drives hybrid_test.mjs with both endpoints.
 *
 * Usage:
 *   node run_sdl_hybrid_test.mjs <blytplay_path> <cart_path>
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

'use strict';

/* Debug-driver default: every failure's captured stderr should already carry
 * a protocol/lifecycle trace.  'api' stays opt-in (high volume). */
if (!process.env.BLYT_TRACE) process.env.BLYT_TRACE = 'gdb,dap,lifecycle,frame';

import { execFile, spawn } from 'child_process';
import { fileURLToPath }   from 'url';
import path                from 'path';

const __dirname = path.dirname(fileURLToPath(import.meta.url));

const BLYTRUN = process.argv[2];
const CART    = process.argv[3];

if (!BLYTRUN || !CART) {
    process.stderr.write('usage: run_sdl_hybrid_test.mjs <blytplay> <cart>\n');
    process.exit(1);
}

/* Wait for both "DAP listening on port N" and "GDB listening on port N". */
function findBothPorts(proc) {
    return new Promise((resolve, reject) => {
        let buf = '';
        let dapPort = null;
        let gdbPort = null;
        const timer = setTimeout(
            () => reject(new Error('timeout: blytplay did not print both DAP and GDB ports')),
            15000
        );
        function check(chunk) {
            buf += chunk.toString();
            const dm = buf.match(/DAP listening on port (\d+)/);
            const gm = buf.match(/GDB listening on port (\d+)/);
            if (dm) dapPort = parseInt(dm[1], 10);
            if (gm) gdbPort = parseInt(gm[1], 10);
            if (dapPort && gdbPort) { clearTimeout(timer); resolve({ dapPort, gdbPort }); }
        }
        proc.stdout.on('data', check);
        proc.stderr.on('data', check);
        proc.on('exit', (code) => {
            clearTimeout(timer);
            if (code !== null && code !== 0)
                reject(new Error(`blytplay exited early with code ${code}`));
        });
        proc.on('error', (e) => { clearTimeout(timer); reject(e); });
    });
}

async function main() {
    const blytplay = spawn(BLYTRUN, ['--debug', '0', '--gdb', '0', '--headless', CART], {
        stdio: ['ignore', 'pipe', 'pipe'],
    });
    blytplay.stderr.on('data', (d) => process.stderr.write(d));

    const { dapPort, gdbPort } = await findBothPorts(blytplay);
    console.log(`[hybrid_test] DAP tcp://127.0.0.1:${dapPort}  GDB tcp://127.0.0.1:${gdbPort}`);

    const testScript  = path.join(__dirname, 'hybrid_test.mjs');
    const dapEndpoint = `tcp://127.0.0.1:${dapPort}`;
    const gdbEndpoint = `tcp://127.0.0.1:${gdbPort}`;
    const extraArgs   = process.env.BLYT_GDB_BREAK_ADDR
        ? ['--gdb-break-addr', process.env.BLYT_GDB_BREAK_ADDR]
        : [];

    await new Promise((resolve, reject) => {
        execFile(
            process.execPath,
            [testScript, dapEndpoint, gdbEndpoint, ...extraArgs],
            { timeout: 30000 },
            (err, stdout, stderr) => {
                process.stdout.write(stdout);
                process.stderr.write(stderr);
                if (err) reject(err); else resolve();
            }
        );
    });

    /* Wait for blytplay to exit after DAP/GDB clients send continue. */
    await new Promise((resolve) => {
        blytplay.on('exit', resolve);
        setTimeout(() => { blytplay.kill(); resolve(); }, 5000);
    });
}

main().then(() => process.exit(0)).catch((e) => {
    console.error('[hybrid_test] FAILED:', e.message);
    process.exit(1);
});
