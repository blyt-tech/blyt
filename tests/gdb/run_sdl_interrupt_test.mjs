#!/usr/bin/env node
/*
 * tests/gdb/run_sdl_interrupt_test.mjs — out-of-band interrupt (\x03) test.
 *
 * Spawns blytplay --gdb 0 --headless <cart>, connects a GDB RSP TCP client,
 * sends vCont;c (no breakpoint), waits 100 ms, then sends the out-of-band
 * interrupt byte (\x03).  Asserts that the stub responds with a T02 (SIGINT)
 * stop reply.  Then detaches so the cart exits cleanly.
 *
 * Usage:
 *   node run_sdl_interrupt_test.mjs <blytplay_path> <cart_path>
 *
 * Exit 0 on success, non-zero on failure.
 * Node.js 22+ required.
 */

'use strict';

/* Debug-driver default: every failure's captured stderr should already carry
 * a protocol/lifecycle trace.  'api' stays opt-in (high volume). */
if (!process.env.BLYT_TRACE) process.env.BLYT_TRACE = 'gdb,dap,lifecycle,frame';

import { spawn }        from 'child_process';
import { createConnection } from 'net';
import { fileURLToPath }    from 'url';

const BLYTRUN = process.argv[2];
const CART    = process.argv[3];

if (!BLYTRUN || !CART) {
    process.stderr.write('usage: run_sdl_interrupt_test.mjs <blytplay> <cart>\n');
    process.exit(1);
}

/* ── GDB RSP framing ─────────────────────────────────────────────────────── */

function computeCsum(payload) {
    let s = 0;
    for (let i = 0; i < payload.length; i++) s = (s + payload.charCodeAt(i)) & 0xff;
    return s.toString(16).padStart(2, '0');
}
function frame(payload) { return `$${payload}#${computeCsum(payload)}`; }

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
            buf = buf.replace(/^[+\-]*/u, '');
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
        sock.once('connect', () => resolve({
            send(s) { sock.write(s); },
            async exchange(payload) {
                sock.write('+');
                sock.write(frame(payload));
                return new Promise((r) => pending.push(r));
            },
            recv() { return new Promise((r) => pending.push(r)); },
            /* Send a raw byte (no RSP framing). */
            sendRaw(byte) { sock.write(byte); },
            close() { if (!closed) sock.destroy(); },
        }));
    });
}

/* ── Main ────────────────────────────────────────────────────────────────── */

async function main() {
    const blytplay = spawn(BLYTRUN, ['--gdb', '0', '--headless', CART], {
        stdio: ['ignore', 'pipe', 'pipe'],
    });
    blytplay.stderr.on('data', (d) => process.stderr.write(d));

    const port = await findGdbPort(blytplay);
    console.log(`[interrupt_test] blytplay GDB on tcp://127.0.0.1:${port}`);

    const t = await makeClient(port);

    try {
        /* Handshake. */
        const sup = await t.exchange('qSupported:multiprocess+');
        console.log(`[interrupt_test] qSupported → ${sup}`);
        await t.exchange('qAttached');
        t.send('+');
        t.send(frame('?'));
        await t.recv();

        /* Continue without a breakpoint. */
        t.send('+');
        t.send(frame('vCont;c'));
        console.log('[interrupt_test] sent vCont;c, waiting 100ms then interrupting…');

        await new Promise((r) => setTimeout(r, 100));

        /* Send out-of-band interrupt. */
        t.sendRaw('\x03');

        /* Await T02 (SIGINT) stop reply. */
        const stop = await Promise.race([
            t.recv(),
            new Promise((_, rej) => setTimeout(() => rej(new Error('timeout waiting for T02')), 30000)),
        ]);

        if (!stop || !stop.startsWith('T02')) {
            process.stderr.write(`[interrupt_test] FAIL: expected T02, got: ${stop}\n`);
            process.exit(1);
        }
        console.log('PASS: received T02 (interrupt acknowledged)');

        /* Detach so the cart can exit. */
        const detach = await t.exchange('D');
        if (detach !== 'OK') {
            process.stderr.write(`[interrupt_test] FAIL: D → ${detach}\n`);
            process.exit(1);
        }
        console.log('PASS: detached');
    } finally {
        t.close();
    }

    await new Promise((resolve) => {
        blytplay.on('exit', resolve);
        setTimeout(() => { blytplay.kill(); resolve(); }, 5000);
    });
}

main().then(() => process.exit(0)).catch((e) => {
    console.error('[interrupt_test] FAILED:', e.message);
    process.exit(1);
});
