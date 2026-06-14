#!/usr/bin/env node
/*
 * tests/dap/run_qemu_dap_test.mjs — QEMU DAP test orchestrator.
 *
 * SSHes into a running QEMU VM, starts blyt_native --dap-port <vm_dap_port>,
 * waits for "blyt: DAP listening on port N" on the SSH channel, then runs
 * dap_test.mjs in TCP mode against the host-side port forward.
 *
 * The caller is responsible for:
 *   - staging blyt_native, libblyt32lua.so, and the cart into the VM
 *   - starting QEMU with hostfwd=tcp::HOST_DAP_PORT-:VM_DAP_PORT
 *
 * Usage:
 *   node run_qemu_dap_test.mjs <ssh_port> <ssh_key> \
 *        <host_dap_port> <vm_dap_port> <cart_path_in_vm>
 *
 *   ssh_port          host port forwarded to VM port 22
 *   ssh_key           path to SSH private key for root@localhost
 *   host_dap_port     host port forwarded to vm_dap_port
 *   vm_dap_port       port blyt_native listens on inside the VM
 *   cart_path_in_vm   absolute path to the .blyt cart inside the VM
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

const SSH_PORT      = process.argv[2];
const SSH_KEY       = process.argv[3];
const HOST_DAP_PORT = parseInt(process.argv[4], 10);
const VM_DAP_PORT   = parseInt(process.argv[5], 10);
const CART_PATH     = process.argv[6];
const BP_LINE       = process.env.BLYT_DAP_BP_LINE || '3';

if (!SSH_PORT || !SSH_KEY || !HOST_DAP_PORT || !VM_DAP_PORT || !CART_PATH) {
    process.stderr.write(
        'usage: run_qemu_dap_test.mjs <ssh_port> <ssh_key>' +
        ' <host_dap_port> <vm_dap_port> <cart_path_in_vm>\n'
    );
    process.exit(1);
}

const SSH_BASE = [
    '-o', 'StrictHostKeyChecking=no',
    '-o', 'UserKnownHostsFile=/dev/null',
    '-o', 'LogLevel=ERROR',
    '-o', 'ConnectTimeout=5',
    '-p', SSH_PORT,
    '-i', SSH_KEY,
];

/*
 * Spawn SSH to run blyt_native in the VM.  The remote shell merges stderr into
 * stdout via "2>&1" so all output (including "DAP listening on port N") arrives
 * on the SSH process stdout.
 */
function spawnBlytNative() {
    /* Forward BLYT_TRACE as a shell env assignment on the remote command so
     * the in-VM mh_trace/native-libblyt32 trace helpers see it.  The
     * port-detect regex below uses a rolling buffer; extra lines are harmless. */
    const trace_env = process.env.BLYT_TRACE
        ? `BLYT_TRACE=${process.env.BLYT_TRACE} `
        : '';
    const remote_cmd = trace_env + [
        '/tmp/blyt_gate/blyt_native',
        '--dap-port', String(VM_DAP_PORT),
        '--no-validate',
        '--lib-dir', '/tmp/blyt_gate/native',
        '--',
        CART_PATH,
        '2>&1',
    ].join(' ');

    return spawn('ssh', [...SSH_BASE, 'root@localhost', remote_cmd], {
        stdio: ['ignore', 'pipe', 'pipe'],
    });
}

/*
 * Watch stdout/stderr of `proc` for "DAP listening on port N".
 * Also forwards all output to process.stderr so it appears in test logs.
 */
function findDapPort(proc) {
    return new Promise((resolve, reject) => {
        let buf = '';
        const timer = setTimeout(
            () => reject(new Error('timeout: blyt_native did not print DAP port')),
            30000
        );
        const check = (chunk) => {
            const s = chunk.toString();
            process.stderr.write(s);
            buf += s;
            const m = buf.match(/DAP listening on port (\d+)/);
            if (m) { clearTimeout(timer); resolve(parseInt(m[1], 10)); }
        };
        proc.stdout.on('data', check);
        proc.stderr.on('data', (d) => process.stderr.write(d));
        proc.on('exit', (code) => {
            clearTimeout(timer);
            if (code !== null && code !== 0)
                reject(new Error(`blyt_native exited early with code ${code}`));
        });
        proc.on('error', (e) => { clearTimeout(timer); reject(e); });
    });
}

async function main() {
    const proc = spawnBlytNative();

    const vmPort = await findDapPort(proc);
    console.log(
        `[qemu_dap_test] blyt_native DAP on tcp://127.0.0.1:${HOST_DAP_PORT}` +
        ` (vm port ${vmPort})`
    );

    const testScript = path.join(__dirname, 'dap_test.mjs');
    const endpoint   = `tcp://127.0.0.1:${HOST_DAP_PORT}`;

    /* The cart's Lua chunk name is canonicalised to
     * /blyt/cart/src/game/lua/main.lua at build time (issue #46); breakpoints
     * match it exactly (issue #51). */
    await new Promise((resolve, reject) => {
        execFile(
            process.execPath,
            [testScript, endpoint, '/blyt/cart/src/game/lua/main.lua', BP_LINE],
            { timeout: 120000 },
            (err, stdout, stderr) => {
                process.stdout.write(stdout);
                process.stderr.write(stderr);
                if (err) reject(err); else resolve();
            }
        );
    });

    /* DAP client sent continue; wait for blyt_native to exit. */
    await new Promise((resolve) => {
        proc.on('exit', resolve);
        setTimeout(() => { proc.kill(); resolve(); }, 8000);
    });
}

main().then(() => process.exit(0)).catch((e) => {
    console.error('[qemu_dap_test] FAILED:', e.message);
    process.exit(1);
});
