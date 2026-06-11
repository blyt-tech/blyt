#!/usr/bin/env node
/*
 * tests/gdb/run_wasm_interrupt_test.mjs — WASM GDB out-of-band interrupt test.
 *
 * Starts an in-process WebSocket relay on /gdb, loads blytplay.js, connects a
 * GDB RSP client, sends vCont;c, waits 100 ms, then sends the out-of-band
 * interrupt byte (\x03) as a WebSocket text frame.  Asserts that the WASM GDB
 * stub responds with T02 (SIGINT).  Then detaches so the cart exits cleanly.
 *
 * Usage:
 *   node run_wasm_interrupt_test.mjs <wasm_dir> <cart_path>
 *
 * Exit 0 on success, non-zero on failure.
 * Node.js 22+ required.
 */

'use strict';

/* Debug-driver default: every failure's captured stderr should already carry
 * a protocol/lifecycle trace.  'api' stays opt-in (high volume). */
if (!process.env.BLYT_TRACE) process.env.BLYT_TRACE = 'gdb,dap,lifecycle,frame';

import { createServer }  from 'http';
import { createHash }    from 'crypto';
import { createRequire } from 'module';
import { fileURLToPath } from 'url';
import { readFileSync, existsSync } from 'fs';
import path              from 'path';

const __dirname = path.dirname(fileURLToPath(import.meta.url));

const WASM_DIR  = process.argv[2];
const CART_PATH = process.argv[3];

if (!WASM_DIR || !CART_PATH) {
    process.stderr.write('usage: run_wasm_interrupt_test.mjs <wasm_dir> <cart_path>\n');
    process.exit(1);
}

/* ── Minimal WebSocket frame codec ─────────────────────────────────────── */

const WS_MAGIC = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';

function wsHandshake(req, socket) {
    const key = req.headers['sec-websocket-key'];
    if (!key) { socket.destroy(); return false; }
    const accept = createHash('sha1').update(key + WS_MAGIC).digest('base64');
    socket.write(
        'HTTP/1.1 101 Switching Protocols\r\n' +
        'Upgrade: websocket\r\n' +
        'Connection: Upgrade\r\n' +
        `Sec-WebSocket-Accept: ${accept}\r\n` +
        '\r\n'
    );
    return true;
}

function wsParseFrames(buf, onFrame) {
    while (buf.length >= 2) {
        const b0 = buf[0];
        const b1 = buf[1];
        const opcode = b0 & 0x0f;
        const masked = !!(b1 & 0x80);
        let payLen   = b1 & 0x7f;
        let offset   = 2;

        if (payLen === 126) {
            if (buf.length < 4) break;
            payLen = buf.readUInt16BE(2); offset = 4;
        } else if (payLen === 127) {
            if (buf.length < 10) break;
            payLen = Number(buf.readBigUInt64BE(2)); offset = 10;
        }

        const maskOffset = masked ? offset : null;
        if (masked) offset += 4;
        if (buf.length < offset + payLen) break;

        const payload = buf.slice(offset, offset + payLen);
        if (masked) {
            const mask = buf.slice(maskOffset, maskOffset + 4);
            for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i & 3];
        }
        buf = buf.slice(offset + payLen);

        if (opcode === 0x01 || opcode === 0x02) onFrame(payload.toString('utf8'));
        else if (opcode === 0x08)               onFrame(null);
    }
    return buf;
}

function wsFrame(text) {
    const payload = Buffer.from(text, 'utf8');
    const len     = payload.length;
    let header;
    if (len < 126) {
        header = Buffer.from([0x81, len]);
    } else if (len < 65536) {
        header = Buffer.alloc(4);
        header[0] = 0x81; header[1] = 126;
        header.writeUInt16BE(len, 2);
    } else {
        header = Buffer.alloc(10);
        header[0] = 0x81; header[1] = 127;
        header.writeBigUInt64BE(BigInt(len), 2);
    }
    return Buffer.concat([header, payload]);
}

/* ── GDB RSP framing ─────────────────────────────────────────────────────── */

function computeCsum(payload) {
    let s = 0;
    for (let i = 0; i < payload.length; i++) s = (s + payload.charCodeAt(i)) & 0xff;
    return s.toString(16).padStart(2, '0');
}
function gdbFrame(payload) { return `$${payload}#${computeCsum(payload)}`; }

function parseOne(buf) {
    const start = buf.indexOf('$');
    if (start < 0) return null;
    const end = buf.indexOf('#', start + 1);
    if (end < 0 || buf.length < end + 3) return null;
    return { payload: buf.slice(start + 1, end), rest: buf.slice(end + 3) };
}

/* ── Relay server with attached inline GDB client ───────────────────────── */

function startRelay() {
    return new Promise((resolvePort) => {
        let wasmSide   = null; /* WASM runtime */
        let clientSide = null; /* inline GDB client (this process) */
        let onWasmConnect = null;

        let recvBuf = '';
        const recvPending = [];

        function dispatchRecv(text) {
            recvBuf += text;
            recvBuf = recvBuf.replace(/^[+\-]*/u, '');
            let pkt;
            while ((pkt = parseOne(recvBuf)) !== null) {
                recvBuf = pkt.rest;
                if (recvPending.length > 0) recvPending.shift()(pkt.payload);
            }
        }

        function makeSide(socket, label, onMsg, onClose) {
            const side = { socket, buf: Buffer.alloc(0), closed: false };
            socket.on('data', (chunk) => {
                side.buf = Buffer.concat([side.buf, chunk]);
                side.buf = wsParseFrames(side.buf, (text) => {
                    if (text === null) {
                        if (!side.closed) { socket.write(Buffer.from([0x88, 0x00])); socket.destroy(); }
                        return;
                    }
                    process.stderr.write(`[relay] ${label}→client: ${JSON.stringify(text.slice(0, 80))}\n`);
                    if (onMsg) onMsg(text);
                });
            });
            socket.on('close', () => { side.closed = true; if (onClose) onClose(); });
            socket.on('error', (e) => {
                process.stderr.write(`[relay] ${label} error: ${e.message}\n`);
                side.closed = true;
            });
            return side;
        }

        const server = createServer();
        server.on('upgrade', (req, socket) => {
            if (req.url !== '/gdb') { socket.destroy(); return; }
            if (!wsHandshake(req, socket)) return;
            /* First connection is the WASM runtime. */
            if (wasmSide === null) {
                wasmSide = makeSide(socket, 'wasm', (text) => {
                    /* Forward WASM → client (dispatchRecv). */
                    dispatchRecv(text);
                    /* Also echo back '+' ACK for every proper packet. */
                    if (text.includes('$')) wasmSide.socket.write(wsFrame('+'));
                }, () => {
                    process.stderr.write('[relay] WASM side closed\n');
                    for (const r of recvPending) r(null);
                    recvPending.length = 0;
                });
                if (onWasmConnect) { onWasmConnect(); onWasmConnect = null; }
            }
        });

        const waitForRuntime = (timeoutMs) =>
            new Promise((res, rej) => {
                if (wasmSide !== null) { res(); return; }
                const t = setTimeout(() => rej(new Error('WASM GDB runtime did not connect within timeout')), timeoutMs);
                onWasmConnect = () => { clearTimeout(t); res(); };
            });

        /* Inline GDB client interface: sends to WASM, receives from WASM. */
        const client = {
            send(s) {
                if (wasmSide && !wasmSide.closed)
                    wasmSide.socket.write(wsFrame(s));
            },
            async exchange(payload) {
                this.send('+');
                this.send(gdbFrame(payload));
                return new Promise((r) => recvPending.push(r));
            },
            recv() { return new Promise((r) => recvPending.push(r)); },
        };

        server.listen(0, '127.0.0.1', () =>
            resolvePort({ port: server.address().port, waitForRuntime, client }));
    });
}

/* ── Load blytplay.js ────────────────────────────────────────────────────── */

function loadWasmRuntime(wasmDir, cartPath, gdbPort) {
    return new Promise((resolve, reject) => {
        const cartData = readFileSync(cartPath);
        globalThis.__blyt_cart_data = new Uint8Array(cartData);
        // module_pre.js copies __blyt_env_vars into the Emscripten ENV (preRun),
        // so the C-side getenv("BLYT_TRACE") sees the same channels as native legs.
        if (process.env.BLYT_TRACE) {
            globalThis.__blyt_env_vars = Object.assign(globalThis.__blyt_env_vars || {}, {
                BLYT_TRACE: process.env.BLYT_TRACE,
            });
        }
        globalThis.__blyt_gdb_port  = gdbPort;
        globalThis.__blyt_init_module = {
            print:    (s) => process.stdout.write(s + '\n'),
            printErr: (s) => process.stderr.write(s + '\n'),
            onExit:   (code) => resolve(code),
        };
        try {
            const require = createRequire(import.meta.url);
            const wasmJs = existsSync(path.join(wasmDir, 'blytdebug.js'))
                ? 'blytdebug.js'
                : 'blytplay.js';
            require(path.join(wasmDir, wasmJs));
        } catch (e) {
            reject(e);
        }
    });
}

/* ── Main ───────────────────────────────────────────────────────────────── */

async function main() {
    const { port: relayPort, waitForRuntime, client } = await startRelay();
    console.log(`[wasm_interrupt_test] relay on ws://127.0.0.1:${relayPort}/gdb`);

    loadWasmRuntime(WASM_DIR, CART_PATH, relayPort).catch((e) => {
        console.error('[wasm_interrupt_test] WASM runtime error:', e.message);
    });

    await waitForRuntime(10000);
    console.log('[wasm_interrupt_test] WASM runtime connected');

    /* Handshake. */
    const sup = await client.exchange('qSupported:multiprocess+');
    console.log(`[wasm_interrupt_test] qSupported → ${sup}`);
    await client.exchange('qAttached');
    client.send('+');
    client.send(gdbFrame('?'));
    await client.recv();

    /* Continue without a breakpoint. */
    client.send('+');
    client.send(gdbFrame('vCont;c'));
    console.log('[wasm_interrupt_test] sent vCont;c, waiting 100ms then interrupting…');

    await new Promise((r) => setTimeout(r, 100));

    /* Send out-of-band interrupt byte as a WebSocket text frame. */
    client.send('\x03');

    /* Await T02 (SIGINT) stop reply. */
    const stop = await Promise.race([
        client.recv(),
        new Promise((_, rej) => setTimeout(() => rej(new Error('timeout waiting for T02')), 30000)),
    ]);

    if (!stop || !stop.startsWith('T02')) {
        process.stderr.write(`[wasm_interrupt_test] FAIL: expected T02, got: ${stop}\n`);
        process.exit(1);
    }
    console.log('PASS: received T02 (interrupt acknowledged)');

    /* Detach so the cart can exit. */
    const detach = await client.exchange('D');
    if (detach !== 'OK') {
        process.stderr.write(`[wasm_interrupt_test] FAIL: D → ${detach}\n`);
        process.exit(1);
    }
    console.log('PASS: detached');
}

main().then(() => process.exit(0)).catch((e) => {
    console.error('[wasm_interrupt_test] FAILED:', e.message);
    process.exit(1);
});
