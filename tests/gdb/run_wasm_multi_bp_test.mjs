#!/usr/bin/env node
/*
 * tests/gdb/run_wasm_multi_bp_test.mjs — WASM multi-breakpoint GDB orchestrator.
 *
 * Like run_gdb_test.mjs but runs multi_bp_test.mjs against the relay WebSocket
 * instead of gdb_test.mjs.
 *
 * Usage:
 *   node run_wasm_multi_bp_test.mjs <wasm_dir> <cart_path>
 *
 * Environment:
 *   BLYT_GDB_BP_ADDRS — comma-separated hex addresses (passed to multi_bp_test.mjs)
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
import { execFile }      from 'child_process';
import { fileURLToPath } from 'url';
import { readFileSync, existsSync } from 'fs';
import path              from 'path';

const __dirname = path.dirname(fileURLToPath(import.meta.url));

const WASM_DIR  = process.argv[2];
const CART_PATH = process.argv[3];

if (!WASM_DIR || !CART_PATH) {
    process.stderr.write('usage: run_wasm_multi_bp_test.mjs <wasm_dir> <cart_path>\n');
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

/* ── Relay server ───────────────────────────────────────────────────────── */

function startRelay() {
    return new Promise((resolvePort) => {
        let sideA = null;
        let sideB = null;
        let onSideAConnect = null;

        function relay(from, to) {
            if (!from || !to) return;
            from.onmsg = (text) => {
                if (text === null) return;
                if (to && !to.closed) to.socket.write(wsFrame(text));
            };
        }

        function makeSide(socket, label, onClose) {
            const side = { socket, buf: Buffer.alloc(0), onmsg: null, closed: false };
            socket.on('data', (chunk) => {
                side.buf = Buffer.concat([side.buf, chunk]);
                side.buf = wsParseFrames(side.buf, (text) => {
                    if (text === null) {
                        process.stderr.write(`[relay] ${label} close frame received\n`);
                        if (!side.closed) {
                            socket.write(Buffer.from([0x88, 0x00]));
                            socket.destroy();
                        }
                        return;
                    }
                    process.stderr.write(`[relay] ${label}→other: ${JSON.stringify(text.slice(0, 80))}\n`);
                    if (side.onmsg) side.onmsg(text);
                });
            });
            socket.on('close', () => {
                process.stderr.write(`[relay] ${label} socket closed\n`);
                side.closed = true; if (onClose) onClose();
            });
            socket.on('error', (e) => {
                process.stderr.write(`[relay] ${label} socket error: ${e.message}\n`);
                side.closed = true; if (onClose) onClose();
            });
            return side;
        }

        const server = createServer();
        server.on('upgrade', (req, socket) => {
            if (req.url !== '/gdb') { socket.destroy(); return; }
            if (!wsHandshake(req, socket)) return;
            if (sideA === null) {
                sideA = makeSide(socket, 'sideA', () => {
                    process.stderr.write('[relay] sideA closed — destroying sideB\n');
                    if (sideB && !sideB.closed) sideB.socket.destroy();
                });
                if (onSideAConnect) { onSideAConnect(); onSideAConnect = null; }
                if (sideB !== null) { relay(sideA, sideB); relay(sideB, sideA); }
            } else if (sideB === null) {
                sideB = makeSide(socket, 'sideB');
                relay(sideA, sideB);
                relay(sideB, sideA);
            }
        });
        const waitForRuntime = (timeoutMs) =>
            new Promise((res, rej) => {
                if (sideA !== null) { res(); return; }
                const t = setTimeout(() => rej(new Error('WASM GDB runtime did not connect within timeout')), timeoutMs);
                onSideAConnect = () => { clearTimeout(t); res(); };
            });

        server.listen(0, '127.0.0.1', () => resolvePort({ port: server.address().port, waitForRuntime }));
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
    const { port: relayPort, waitForRuntime } = await startRelay();
    console.log(`[run_wasm_multi_bp_test] relay on ws://127.0.0.1:${relayPort}/gdb`);

    const runtimeDone = loadWasmRuntime(WASM_DIR, CART_PATH, relayPort).catch((e) => {
        console.error('[run_wasm_multi_bp_test] WASM runtime error:', e.message);
    });

    await waitForRuntime(10000);
    console.log('[run_wasm_multi_bp_test] WASM runtime connected');

    const testScript = path.join(__dirname, 'multi_bp_test.mjs');
    const wsUrl      = `ws://127.0.0.1:${relayPort}/gdb`;

    await new Promise((resolve, reject) => {
        execFile(
            process.execPath,
            [testScript, wsUrl],
            { timeout: 120000 },
            (err, stdout, stderr) => {
                process.stdout.write(stdout);
                process.stderr.write(stderr);
                if (err) reject(err); else resolve();
            }
        );
    });

    await runtimeDone;
}

main().then(() => process.exit(0)).catch((e) => {
    console.error('[run_wasm_multi_bp_test] FAILED:', e.message);
    process.exit(1);
});
