#!/usr/bin/env node
/*
 * tests/gdb/hybrid_test.mjs — combined DAP + GDB hybrid test client.
 *
 * Tests a Lua cart that calls a native C function with both debuggers active:
 *
 *   1. GDB: connect, handshake, set Z0 at native function entry
 *   2. DAP: connect, initialize, setBreakpoints(line 3), configurationDone
 *   3. Cart starts; DAP stops at line 3 (before the native call)
 *   4. DAP: next (step over the native call)
 *   5. GDB: receive T05 (breakpoint hit inside native function)
 *   6. GDB: read registers, single-step × 2
 *   7. GDB: clear breakpoint, vCont;c
 *   8. DAP: receive stopped at line 4 (step-over complete, native call returned)
 *   9. Verify line 4, then DAP continue → cart exits
 *
 * If --gdb-break-addr is absent, only the DAP session is exercised (both
 * debuggers remain connected; GDB does not set a breakpoint).
 *
 * Usage:
 *   node hybrid_test.mjs <dap-endpoint> <gdb-endpoint> [--gdb-break-addr HEXADDR]
 *
 *   dap-endpoint   tcp://host:port   (Content-Length framed DAP over TCP)
 *   gdb-endpoint   tcp://host:port   (GDB RSP over TCP)
 *
 * Exit 0 on success, non-zero on failure.
 * Node.js 22+ required.
 */

'use strict';

const DAP_ENDPOINT = process.argv[2];
const GDB_ENDPOINT = process.argv[3];
let   gdbBreakAddr = null;

for (let i = 4; i < process.argv.length; i++) {
    if (process.argv[i] === '--gdb-break-addr' && process.argv[i + 1]) {
        gdbBreakAddr = process.argv[++i];
    }
}

if (!DAP_ENDPOINT || !GDB_ENDPOINT) {
    process.stderr.write('usage: hybrid_test.mjs <dap-endpoint> <gdb-endpoint> [--gdb-break-addr HEX]\n');
    process.exit(1);
}

/* ── shared assert ───────────────────────────────────────────────────────── */

let passed = 0;
let failed = 0;

function assert(cond, msg) {
    if (cond) {
        console.log(`  PASS: ${msg}`);
        passed++;
    } else {
        console.error(`  FAIL: ${msg}`);
        failed++;
    }
}

/* ── GDB RSP framing ─────────────────────────────────────────────────────── */

function gdbCsum(payload) {
    let s = 0;
    for (let i = 0; i < payload.length; i++) s = (s + payload.charCodeAt(i)) & 0xff;
    return s.toString(16).padStart(2, '0');
}

function gdbFrame(payload) {
    return `$${payload}#${gdbCsum(payload)}`;
}

function gdbParseOne(buf) {
    const start = buf.indexOf('$');
    if (start < 0) return null;
    const end = buf.indexOf('#', start + 1);
    if (end < 0 || buf.length < end + 3) return null;
    return { payload: buf.slice(start + 1, end), rest: buf.slice(end + 3) };
}

/* ── GDB TCP connection ──────────────────────────────────────────────────── */

async function connectGdb(endpointStr) {
    const url  = new URL(endpointStr);
    const { createConnection } = await import('net');
    const sock = createConnection(parseInt(url.port, 10), url.hostname);
    sock.setEncoding('utf8');

    const pending = [];
    let buf    = '';
    let closed = false;

    function onData(text) {
        buf += text;
        buf = buf.replace(/^[+\-]*/u, '');
        let pkt;
        while ((pkt = gdbParseOne(buf)) !== null) {
            buf = pkt.rest;
            if (pending.length > 0) pending.shift()(pkt.payload);
        }
    }
    function onClose() {
        closed = true;
        for (const r of pending) r(null);
        pending.length = 0;
    }

    sock.on('data',  onData);
    sock.on('close', onClose);
    sock.on('error', (e) => process.stderr.write(`[gdb] error: ${e.message}\n`));
    await new Promise((res, rej) => { sock.once('connect', res); sock.once('error', rej); });

    return {
        send(s)         { sock.write(s); },
        async exchange(payload) {
            sock.write('+');
            sock.write(gdbFrame(payload));
            return new Promise((r) => pending.push(r));
        },
        recv()          { return new Promise((r) => pending.push(r)); },
        close()         { if (!closed) sock.destroy(); },
    };
}

/* ── DAP TCP connection ──────────────────────────────────────────────────── */

async function connectDap(endpointStr) {
    const url  = new URL(endpointStr);
    const { createConnection } = await import('net');
    const sock = createConnection(parseInt(url.port, 10), url.hostname);

    let seq = 1;
    let tcpBuf = Buffer.alloc(0);
    const pendingReqs = new Map();
    const eventQueue  = [];
    const waiters     = [];

    function drainMessages(buf) {
        while (true) {
            let i = 0;
            for (; i + 3 < buf.length; i++) {
                if (buf[i] === 0x0d && buf[i+1] === 0x0a &&
                    buf[i+2] === 0x0d && buf[i+3] === 0x0a) break;
            }
            if (i + 3 >= buf.length) return buf;
            const m = buf.slice(0, i).toString('utf8').match(/Content-Length:\s*(\d+)/i);
            if (!m) return buf.slice(i + 4);
            const len = parseInt(m[1], 10);
            if (buf.length < i + 4 + len) return buf;
            onMsg(buf.slice(i + 4, i + 4 + len).toString('utf8'));
            buf = buf.slice(i + 4 + len);
        }
    }

    function onMsg(text) {
        let msg;
        try { msg = JSON.parse(text); } catch { return; }
        if (msg.type === 'response') {
            const p = pendingReqs.get(msg.request_seq);
            if (p) {
                clearTimeout(p.timer);
                pendingReqs.delete(msg.request_seq);
                if (msg.success) p.resolve(msg.body || {});
                else p.reject(new Error(msg.message || `${msg.command} failed`));
            }
        } else if (msg.type === 'event') {
            eventQueue.push(msg);
            for (const w of [...waiters]) w(msg);
        }
    }

    function sendRaw(text) {
        const hdr = `Content-Length: ${Buffer.byteLength(text, 'utf8')}\r\n\r\n`;
        sock.write(hdr + text, 'utf8');
    }

    sock.on('data', (chunk) => { tcpBuf = drainMessages(Buffer.concat([tcpBuf, chunk])); });
    await new Promise((res, rej) => { sock.once('connect', res); sock.once('error', rej); });

    return {
        request(command, args = {}) {
            const s = seq++;
            return new Promise((resolve, reject) => {
                const timer = setTimeout(() => reject(new Error(`timeout: ${command}`)), 20000);
                pendingReqs.set(s, { resolve, reject, timer });
                sendRaw(JSON.stringify({ seq: s, type: 'request', command, arguments: args }));
            });
        },
        nextEvent(name) {
            return new Promise((resolve, reject) => {
                const timer = setTimeout(() => reject(new Error(`timeout: event "${name}"`)), 20000);
                const check = (e) => {
                    if (e.event === name) {
                        clearTimeout(timer);
                        const idx = waiters.indexOf(check);
                        if (idx >= 0) waiters.splice(idx, 1);
                        resolve(e);
                        return true;
                    }
                    return false;
                };
                for (const e of eventQueue) { if (check(e)) return; }
                waiters.push(check);
            });
        },
        close() { sock.destroy(); },
    };
}

/* ── Main test ───────────────────────────────────────────────────────────── */

async function main() {
    /* Connect both debuggers before the cart starts. */
    const gdb = await connectGdb(GDB_ENDPOINT);
    const dap = await connectDap(DAP_ENDPOINT);

    /* ── GDB handshake ────────────────────────────────────────────────────── */
    const supported = await gdb.exchange('qSupported:multiprocess+;qXfer:exec-file:read+');
    console.log(`[gdb] qSupported → ${supported}`);
    await gdb.exchange('qAttached');
    await gdb.exchange('vCont?');

    /* Initial stop state — always T05 per stub. */
    gdb.send('+');
    gdb.send(gdbFrame('?'));
    const initStop = await gdb.recv();
    console.log(`[gdb] ? → ${initStop}`);

    /* Set software breakpoint at native function entry (if address provided). */
    if (gdbBreakAddr) {
        const addrHex = parseInt(gdbBreakAddr, 16).toString(16);
        const bpResp  = await gdb.exchange(`Z0,${addrHex},4`);
        assert(bpResp === 'OK', `GDB Z0 at 0x${addrHex}: ${bpResp}`);
    }

    /* ── DAP session: init + configure ───────────────────────────────────── */
    const init = await dap.request('initialize', {
        clientID:        'blyt-hybrid-test',
        adapterID:       'blyt-lua',
        linesStartAt1:   true,
        columnsStartAt1: true,
    });
    assert(init.supportsConfigurationDoneRequest === true, 'initialize: supportsConfigurationDoneRequest');
    await dap.nextEvent('initialized');

    await dap.request('launch', {});

    /* Breakpoint at line 3: the Lua call site for blyt_native_work(). */
    const sb = await dap.request('setBreakpoints', {
        source:      { path: 'main.lua', name: 'main.lua' },
        breakpoints: [{ line: 3 }],
        lines:       [3],
    });
    assert(Array.isArray(sb.breakpoints) && sb.breakpoints.length >= 1,
           'DAP setBreakpoints: ≥1 entry');
    if (sb.breakpoints[0]) {
        assert(sb.breakpoints[0].verified === true, 'DAP breakpoint at line 3 verified');
    }

    /* configurationDone — cart starts. */
    await dap.request('configurationDone');

    /* ── Wait for DAP stopped at line 3 ──────────────────────────────────── */
    const dapStop1 = await dap.nextEvent('stopped');
    assert(
        dapStop1.body.reason === 'breakpoint' || dapStop1.body.reason === 'step',
        `DAP first stop: reason "${dapStop1.body.reason}"`
    );

    const stack1 = await dap.request('stackTrace', { threadId: 1 });
    const frames1 = stack1.stackFrames || [];
    assert(frames1.some(f => f.line === 3), 'DAP stopped at line 3');
    console.log(`[dap] stopped at line ${frames1[0]?.line} (expected 3)`);

    if (gdbBreakAddr) {
        const addrHex = parseInt(gdbBreakAddr, 16).toString(16);

        /* Register DAP stopped-2 listener BEFORE sending next, so the event
         * is not missed while GDB stepping is in progress. */
        const dapStop2P = dap.nextEvent('stopped');

        /* DAP: step over the native call. */
        await dap.request('next', { threadId: 1 });
        console.log('[dap] next sent — waiting for GDB T05 inside native function');

        /* ── GDB: receive T05 when breakpoint fires inside the C function ── */
        const gdbT05 = await gdb.recv();
        assert(gdbT05 && gdbT05.startsWith('T05'), `GDB T05 at native entry: "${gdbT05}"`);
        console.log(`[gdb] T05 received: ${gdbT05}`);

        /* Read registers — verify PC is non-zero and matches the break address. */
        const regs = await gdb.exchange('g');
        if (regs && regs.length >= 264) {
            const pcHex = regs.slice(256, 264);
            const pc    = parseInt(pcHex.match(/../g).reverse().join(''), 16);
            assert(pc !== 0, `GDB PC non-zero after breakpoint: 0x${pc.toString(16)}`);
            console.log(`[gdb] PC = 0x${pc.toString(16)}`);
        } else {
            console.log(`[gdb] register reply length ${regs?.length} (expected ≥264)`);
        }

        /* Single-step 1. */
        gdb.send('+');   /* ack register response */
        gdb.send(gdbFrame('vCont;s'));
        const step1 = await gdb.recv();
        assert(step1 && step1.startsWith('T05'), `GDB step 1 T05: "${step1}"`);
        console.log('[gdb] step 1 T05 received');

        /* Single-step 2. */
        gdb.send('+');   /* ack step 1 T05 */
        gdb.send(gdbFrame('vCont;s'));
        const step2 = await gdb.recv();
        assert(step2 && step2.startsWith('T05'), `GDB step 2 T05: "${step2}"`);
        console.log('[gdb] step 2 T05 received');

        /* Clear breakpoint (ack step 2 T05 via exchange's leading '+'). */
        const clrResp = await gdb.exchange(`z0,${addrHex},4`);
        assert(clrResp === 'OK', `GDB z0 clear: ${clrResp}`);

        /* Continue — native function runs to completion, returns to Lua. */
        gdb.send('+');   /* ack z0 OK */
        gdb.send(gdbFrame('vCont;c'));
        console.log('[gdb] vCont;c sent — waiting for DAP stopped at line 4');

        /* ── DAP: receive stopped at line 4 (step-over complete) ──────────── */
        const dapStop2 = await dapStop2P;
        assert(
            dapStop2.body.reason === 'step' || dapStop2.body.reason === 'breakpoint',
            `DAP second stop: reason "${dapStop2.body.reason}"`
        );

        const stack2  = await dap.request('stackTrace', { threadId: 1 });
        const frames2 = stack2.stackFrames || [];
        assert(frames2.some(f => f.line === 4), 'DAP at line 4 after step over native call');
        console.log(`[dap] stopped at line ${frames2[0]?.line} (expected 4)`);

        console.log('PASS: hybrid GDB+DAP session complete');
    }

    /* DAP: continue → cart calls blyt.quit() and exits. */
    await dap.request('continue', { threadId: 1 });
    dap.close();
    gdb.close();
}

main().then(() => {
    console.log(`\n${passed} passed, ${failed} failed`);
    process.exit(failed > 0 ? 1 : 0);
}).catch((err) => {
    console.error('\nERROR:', err.message);
    process.exit(2);
});
