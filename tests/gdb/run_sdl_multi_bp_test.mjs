#!/usr/bin/env node
/*
 * tests/gdb/run_sdl_multi_bp_test.mjs — SDL2 multi-breakpoint orchestrator.
 *
 * Spawns blytplay --gdb 0 --headless <cart>, waits for the GDB TCP port, then
 * runs multi_bp_test.mjs against it.
 *
 * Usage:
 *   node run_sdl_multi_bp_test.mjs <blytplay_path> <cart_path>
 *
 * Environment:
 *   BLYT_GDB_BP_ADDRS — comma-separated hex addresses passed to multi_bp_test.mjs
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
    process.stderr.write('usage: run_sdl_multi_bp_test.mjs <blytplay> <cart>\n');
    process.exit(1);
}

function findGdbPort(proc) {
    return new Promise((resolve, reject) => {
        let buf = '';
        const timer = setTimeout(
            () => reject(new Error('timeout: blytplay did not print GDB port')),
            60000
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
                reject(new Error(`blytplay exited early with code ${code}`));
        });
        proc.on('error', (e) => { clearTimeout(timer); reject(e); });
    });
}

async function main() {
    const blytplay = spawn(BLYTRUN, ['--gdb', '0', '--headless', CART], {
        stdio: ['ignore', 'pipe', 'pipe'],
    });
    blytplay.stderr.on('data', (d) => process.stderr.write(d));

    const port = await findGdbPort(blytplay);
    console.log(`[multi_bp_sdl] blytplay GDB on tcp://127.0.0.1:${port}`);

    const testScript = path.join(__dirname, 'multi_bp_test.mjs');
    const endpoint   = `tcp://127.0.0.1:${port}`;

    await new Promise((resolve, reject) => {
        execFile(
            process.execPath,
            [testScript, endpoint],
            {
                timeout: 30000,
                env: { ...process.env },
            },
            (err, stdout, stderr) => {
                process.stdout.write(stdout);
                process.stderr.write(stderr);
                if (err) reject(err); else resolve();
            }
        );
    });

    await new Promise((resolve) => {
        blytplay.on('exit', resolve);
        setTimeout(() => { blytplay.kill(); resolve(); }, 5000);
    });
}

main().then(() => process.exit(0)).catch((e) => {
    console.error('[multi_bp_sdl] FAILED:', e.message);
    process.exit(1);
});
