#!/usr/bin/env node
/*
 * tests/dap/run_sdl_dap_test.mjs — SDL2 DAP test orchestrator.
 *
 * Spawns blytplay --debug --headless <cart>, waits for the DAP TCP port to
 * appear on stdout ("blyt: DAP listening on port N"), then runs dap_test.mjs
 * in TCP mode against that port.
 *
 * Usage:
 *   node run_sdl_dap_test.mjs <blytplay_path> <cart_path>
 *
 * Environment:
 *   BLYT_DAP_BP_LINE  — 1-based line to break on (default 3)
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
const BP_LINE = process.env.BLYT_DAP_BP_LINE || '3';

if (!BLYTRUN || !CART) {
    process.stderr.write('usage: run_sdl_dap_test.mjs <blytplay> <cart>\n');
    process.exit(1);
}

/* Wait for "blyt: DAP listening on port N" on proc's stdout. */
function findDapPort(proc) {
    return new Promise((resolve, reject) => {
        let buf = '';
        const timer = setTimeout(
            () => reject(new Error('timeout: blytplay did not print DAP port')),
            60000
        );
        proc.stdout.on('data', (chunk) => {
            buf += chunk.toString();
            const m = buf.match(/DAP listening on port (\d+)/);
            if (m) { clearTimeout(timer); resolve(parseInt(m[1], 10)); }
        });
        proc.on('exit', (code) => {
            clearTimeout(timer);
            if (code !== null && code !== 0)
                reject(new Error(`blytplay exited early with code ${code}`));
        });
        proc.on('error', (e) => { clearTimeout(timer); reject(e); });
    });
}

async function main() {
    const blytplay = spawn(BLYTRUN, ['--debug', '--headless', CART], {
        stdio: ['ignore', 'pipe', 'pipe'],
    });
    blytplay.stderr.on('data', (d) => process.stderr.write(d));

    /* Register before anything can exit so an early death is never missed
     * (a signal exit has code === null and previously passed silently). */
    const blytplayExit = new Promise((resolve) => {
        blytplay.on('exit', (code, signal) => resolve({ code, signal }));
    });

    const port = await findDapPort(blytplay);

    const testScript = path.join(__dirname, 'dap_test.mjs');
    const endpoint   = `tcp://127.0.0.1:${port}`;

    await new Promise((resolve, reject) => {
        execFile(
            process.execPath,
            [testScript, endpoint, 'main.lua', BP_LINE],
            { timeout: 120000 },
            (err, stdout, stderr) => {
                process.stdout.write(stdout);
                process.stderr.write(stderr);
                if (err) reject(err); else resolve();
            }
        );
    });

    /* DAP client sent continue and the cart calls blyt.quit(): blytplay must
     * exit on its own, cleanly.  The old code killed it after 5s and passed
     * regardless, so a post-continue hang or crash was invisible. */
    const exited = await Promise.race([
        blytplayExit,
        new Promise((resolve) => setTimeout(() => resolve(null), 10000)),
    ]);
    if (!exited) {
        blytplay.kill();
        throw new Error('blytplay did not exit within 10s after the DAP session ended');
    }
    if (exited.signal)
        throw new Error(`blytplay died with signal ${exited.signal}`);
    if (exited.code !== 0)
        throw new Error(`blytplay exited with code ${exited.code}`);
}

main().then(() => process.exit(0)).catch((e) => {
    console.error('[sdl_dap_test] FAILED:', e.message);
    process.exit(1);
});
