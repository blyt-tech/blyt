#!/usr/bin/env node
/*
 * tests/dap/run_dap_test.mjs — end-to-end WASM DAP test orchestrator.
 *
 * Starts an in-process WebSocket relay, loads blytplay.js (the WASM binary)
 * with the relay port injected, then runs dap_test.mjs against the relay.
 *
 * Usage:
 *   node run_dap_test.mjs <wasm_dir> <cart_path>
 *
 * Environment:
 *   BLYT_DAP_BP_LINE  — line number to break on (default 4)
 *
 * Exit 0 on success, non-zero on failure.
 *
 * Implements a minimal RFC-6455 WebSocket server using Node.js net + http,
 * requiring no npm packages.
 *
 * Node.js 22+ required (built-in WebSocket client for dap_test.mjs).
 */

'use strict';

import { createServer }  from 'http';
import { createHash }    from 'crypto';
import { createRequire } from 'module';
import { execFile }      from 'child_process';
import { fileURLToPath } from 'url';
import { readFileSync }  from 'fs';
import path              from 'path';

const __dirname = path.dirname(fileURLToPath(import.meta.url));

const WASM_DIR   = process.argv[2];
const CART_PATH  = process.argv[3];
const BP_LINE    = parseInt(process.env.BLYT_DAP_BP_LINE || '4', 10);

if (!WASM_DIR || !CART_PATH) {
    process.stderr.write('usage: run_dap_test.mjs <wasm_dir> <cart_path>\n');
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

/* Parse one or more complete frames from a Buffer; call onFrame(text) for
 * each complete text frame.  Returns remaining unparsed bytes. */
function wsParseFrames(buf, onFrame) {
    while (buf.length >= 2) {
        const b0 = buf[0];
        const b1 = buf[1];
        const opcode  = b0 & 0x0f;
        const masked  = !!(b1 & 0x80);
        let   payLen  = b1 & 0x7f;
        let   offset  = 2;

        if (payLen === 126) {
            if (buf.length < 4) break;
            payLen = buf.readUInt16BE(2);
            offset = 4;
        } else if (payLen === 127) {
            if (buf.length < 10) break;
            payLen = Number(buf.readBigUInt64BE(2));
            offset = 10;
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

        if (opcode === 0x01 || opcode === 0x02) {  /* text or binary */
            onFrame(payload.toString('utf8'));
        } else if (opcode === 0x08) {               /* close */
            onFrame(null);
        }
        /* ping/pong ignored for simplicity */
    }
    return buf;
}

/* Encode a text message as a WebSocket frame (server side: no masking). */
function wsFrame(text) {
    const payload = Buffer.from(text, 'utf8');
    const len     = payload.length;
    let   header;
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
        let sideA = null;   /* WASM runtime */
        let sideB = null;   /* DAP test client */

        function relay(from, to) {
            if (from === null || to === null) return;
            from.onmsg = (text) => {
                if (text === null) return;
                if (to && !to.closed) to.socket.write(wsFrame(text));
            };
        }

        function makeSide(socket) {
            const side = { socket, buf: Buffer.alloc(0), onmsg: null, closed: false };
            socket.on('data', (chunk) => {
                side.buf = Buffer.concat([side.buf, chunk]);
                side.buf = wsParseFrames(side.buf, (text) => {
                    if (side.onmsg) side.onmsg(text);
                });
            });
            socket.on('close', () => { side.closed = true; });
            socket.on('error', () => { side.closed = true; });
            return side;
        }

        const server = createServer();
        server.on('upgrade', (req, socket) => {
            if (!wsHandshake(req, socket)) return;
            if (sideA === null) {
                sideA = makeSide(socket);
                if (sideB !== null) { relay(sideA, sideB); relay(sideB, sideA); }
            } else if (sideB === null) {
                sideB = makeSide(socket);
                relay(sideA, sideB);
                relay(sideB, sideA);
            }
        });

        server.listen(0, '127.0.0.1', () => {
            resolvePort(server.address().port);
        });
    });
}

/* ── Load blytplay.js via require() (same as run_cart.js) ────────────────── */

function loadWasmRuntime(wasmDir, cartPath, dapPort) {
    return new Promise((resolve, reject) => {
        const cartData = readFileSync(cartPath);

        /* Set up globals that module_pre.js / wasm_main.c's EM_JS reads. */
        globalThis.__blyt_cart_data = new Uint8Array(cartData);
        globalThis.__blyt_dap_port  = dapPort;

        /* module_pre.js uses __blyt_init_module as the Module base object.
         * Must be a plain object — arrow functions throw when Emscripten
         * inspects .caller/.arguments on them in strict mode. */
        globalThis.__blyt_init_module = {
            print:    (s) => process.stdout.write(s + '\n'),
            printErr: (s) => process.stderr.write(s + '\n'),
            onExit:   (code) => resolve(code),
        };

        try {
            const require = createRequire(import.meta.url);
            require(path.join(wasmDir, 'blytplay.js'));
        } catch (e) {
            reject(e);
        }
    });
}

/* ── Main ───────────────────────────────────────────────────────────────── */

async function main() {
    const relayPort = await startRelay();
    console.log(`[run_dap_test] relay on ws://127.0.0.1:${relayPort}/dap`);

    /* Load the WASM runtime in the background (it will connect to the relay). */
    const runtimeDone = loadWasmRuntime(WASM_DIR, CART_PATH, relayPort).catch((e) => {
        console.error('[run_dap_test] WASM runtime error:', e.message);
    });

    /* Give the runtime 2 seconds to connect before starting the DAP client. */
    await new Promise((r) => setTimeout(r, 2000));

    /* Run dap_test.mjs as a child process. */
    const testScript = path.join(__dirname, 'dap_test.mjs');
    const wsUrl      = `ws://127.0.0.1:${relayPort}/dap`;

    await new Promise((resolve, reject) => {
        execFile(
            process.execPath,
            [testScript, wsUrl, 'main.lua', String(BP_LINE)],
            { timeout: 30000 },
            (err, stdout, stderr) => {
                process.stdout.write(stdout);
                process.stderr.write(stderr);
                if (err) reject(err);
                else     resolve();
            }
        );
    });

    await runtimeDone;
}

main().then(() => {
    process.exit(0);
}).catch((e) => {
    console.error('[run_dap_test] FAILED:', e.message);
    process.exit(1);
});
