#!/usr/bin/env node
/*
 * tests/dap/run_libretro_dap_test.mjs — libretro DAP test orchestrator.
 *
 * Spawns blyt-libretro-runner with BLYT_DAP_PORT=0, waits for the DAP TCP
 * port to appear on stdout ("blyt: DAP listening on port N"), then runs
 * dap_test.mjs in TCP mode against that port.
 *
 * Usage:
 *   node run_libretro_dap_test.mjs <runner_path> <blyt_libretro.so> <cart_path>
 *
 * Environment:
 *   BLYT_DAP_BP_LINE  — 1-based line to break on (default 3)
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
const BP_LINE = process.env.BLYT_DAP_BP_LINE || '3';

if (!RUNNER || !SO_PATH || !CART) {
    process.stderr.write(
        'usage: run_libretro_dap_test.mjs <runner> <blyt_libretro.so> <cart>\n'
    );
    process.exit(1);
}

/* Wait for "blyt: DAP listening on port N" on proc's stdout. */
function findDapPort(proc) {
    return new Promise((resolve, reject) => {
        let buf = '';
        const timer = setTimeout(
            () => reject(new Error('timeout: runner did not print DAP port')),
            15000
        );
        proc.stdout.on('data', (chunk) => {
            buf += chunk.toString();
            const m = buf.match(/DAP listening on port (\d+)/);
            if (m) { clearTimeout(timer); resolve(parseInt(m[1], 10)); }
        });
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
        env: { ...process.env, BLYT_DAP_PORT: '0' },
    });
    runner.stderr.on('data', (d) => process.stderr.write(d));

    const port = await findDapPort(runner);

    const testScript = path.join(__dirname, 'dap_test.mjs');
    const endpoint   = `tcp://127.0.0.1:${port}`;

    await new Promise((resolve, reject) => {
        execFile(
            process.execPath,
            [testScript, endpoint, 'main.lua', BP_LINE],
            { timeout: 30000 },
            (err, stdout, stderr) => {
                process.stdout.write(stdout);
                process.stderr.write(stderr);
                if (err) reject(err); else resolve();
            }
        );
    });

    /* DAP client sent continue; wait for the runner to exit. */
    await new Promise((resolve) => {
        runner.on('exit', resolve);
        setTimeout(() => { runner.kill(); resolve(); }, 5000);
    });
}

main().then(() => process.exit(0)).catch((e) => {
    console.error('[libretro_dap_test] FAILED:', e.message);
    process.exit(1);
});
