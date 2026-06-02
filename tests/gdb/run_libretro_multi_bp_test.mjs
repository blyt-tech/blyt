#!/usr/bin/env node
/*
 * tests/gdb/run_libretro_multi_bp_test.mjs — libretro multi-breakpoint orchestrator.
 *
 * Spawns blyt-libretro-runner with BLYT_GDB_PORT=0, waits for the GDB TCP
 * port, then runs multi_bp_test.mjs against it.
 *
 * Usage:
 *   node run_libretro_multi_bp_test.mjs <runner_path> <blyt_libretro.so> <cart_path>
 *
 * Environment:
 *   BLYT_GDB_BP_ADDRS — comma-separated hex addresses passed to multi_bp_test.mjs
 *
 * Exit 0 on success, non-zero on failure.
 * Node.js 22+ required.
 */

'use strict';

import { execFile, spawn } from 'child_process';
import { fileURLToPath }   from 'url';
import path                from 'path';

const __dirname = path.dirname(fileURLToPath(import.meta.url));

const RUNNER  = process.argv[2];
const SO_PATH = process.argv[3];
const CART    = process.argv[4];

if (!RUNNER || !SO_PATH || !CART) {
    process.stderr.write(
        'usage: run_libretro_multi_bp_test.mjs <runner> <blyt_libretro.so> <cart>\n'
    );
    process.exit(1);
}

function findGdbPort(proc) {
    return new Promise((resolve, reject) => {
        let buf = '';
        const timer = setTimeout(
            () => reject(new Error('timeout: runner did not print GDB port')),
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
                reject(new Error(`runner exited early with code ${code}`));
        });
        proc.on('error', (e) => { clearTimeout(timer); reject(e); });
    });
}

async function main() {
    const runner = spawn(RUNNER, [SO_PATH, CART], {
        stdio: ['ignore', 'pipe', 'pipe'],
        env: { ...process.env, BLYT_GDB_PORT: '0' },
    });
    runner.stderr.on('data', (d) => process.stderr.write(d));

    const port = await findGdbPort(runner);
    console.log(`[multi_bp_libretro] runner GDB on tcp://127.0.0.1:${port}`);

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
        runner.on('exit', resolve);
        setTimeout(() => { runner.kill(); resolve(); }, 5000);
    });
}

main().then(() => process.exit(0)).catch((e) => {
    console.error('[multi_bp_libretro] FAILED:', e.message);
    process.exit(1);
});
