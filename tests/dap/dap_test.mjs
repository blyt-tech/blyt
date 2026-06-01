#!/usr/bin/env node
/*
 * tests/dap/dap_test.mjs — DAP test client for blyt (WebSocket or TCP).
 *
 * Drives the blyt DAP server through a minimal breakpoint session:
 *   initialize → launch → setBreakpoints → configurationDone →
 *   stopped(breakpoint) → stackTrace / scopes / variables →
 *   next (step) → stopped(step) → continue → cart exits
 *
 * Usage:
 *   node dap_test.mjs <endpoint> <source_path> <breakpoint_line>
 *
 *   endpoint       ws://host:port/path  — WebSocket (WASM relay)
 *                  tcp://host:port      — raw TCP Content-Length (SDL2/libretro)
 *   source_path    basename the runtime reports (e.g. "main.lua")
 *   breakpoint_line  1-based line number to break on
 *
 * Exit 0 on success, 1 on failure, 2 on timeout.
 *
 * Requires Node.js 22+ (built-in WebSocket client for ws:// mode).
 */

'use strict';

const ENDPOINT             = process.argv[2];
const SOURCE_PATH          = process.argv[3] || 'cart';
const BP_LINE              = parseInt(process.argv[4] || '4', 10);
const TIMEOUT_MS           = 20000;

/* Optional feature flags */
const LOADED_SOURCES_CHECK = !!process.env.BLYT_DAP_LOADED_SOURCES;
const CONDITIONAL_COND     = process.env.BLYT_DAP_CONDITIONAL_COND || '';
const CONDITIONAL_COND_EDIT = process.env.BLYT_DAP_CONDITIONAL_COND_EDIT || '';
const TEST_RESTART         = !!process.env.BLYT_DAP_TEST_RESTART;
const EXCEPTION_FILTER     = process.env.BLYT_DAP_EXCEPTION_FILTER || '';
const EVALUATE_EXPR        = process.env.BLYT_DAP_EVALUATE_EXPR || '';
const EVALUATE_EXPECT      = process.env.BLYT_DAP_EVALUATE_EXPECT || '';

if (!ENDPOINT) {
    process.stderr.write('usage: dap_test.mjs <endpoint> <source_path> <bp_line>\n');
    process.exit(2);
}

/* ── DAP session state ──────────────────────────────────────────────────── */

let seq = 1;
let _sendRaw;   /* set by connectTransport(); sends a raw JSON string */
let _closeConn; /* set by connectTransport(); tears down the connection */
const pending    = new Map();  /* request_seq → { resolve, reject, timer } */
const eventQueue = [];         /* buffered events not yet consumed */
const waiters    = [];         /* waiters registered via nextEvent() */

function nextSeq() { return seq++; }

function send(obj) {
    _sendRaw(JSON.stringify(obj));
}

function request(command, args = {}) {
    const reqSeq = nextSeq();
    return new Promise((resolve, reject) => {
        const timer = setTimeout(
            () => reject(new Error(`timeout waiting for ${command}`)),
            TIMEOUT_MS
        );
        pending.set(reqSeq, { resolve, reject, timer });
        send({ seq: reqSeq, type: 'request', command, arguments: args });
    });
}

function nextEvent(name) {
    return new Promise((resolve, reject) => {
        const timer = setTimeout(
            () => reject(new Error(`timeout waiting for event "${name}"`)),
            TIMEOUT_MS
        );
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
        /* Check already-buffered events first. */
        for (const e of eventQueue) {
            if (check(e)) return;
        }
        waiters.push(check);
    });
}

function onMessage(text) {
    let msg;
    try { msg = JSON.parse(text); } catch { return; }

    if (msg.type === 'response') {
        const p = pending.get(msg.request_seq);
        if (p) {
            clearTimeout(p.timer);
            pending.delete(msg.request_seq);
            if (msg.success) p.resolve(msg.body || {});
            else p.reject(new Error(msg.message || `${msg.command} failed`));
        }
    } else if (msg.type === 'event') {
        eventQueue.push(msg);
        for (const w of [...waiters]) w(msg);
    }
}

/* ── Assertions ─────────────────────────────────────────────────────────── */

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

/* ── Transport: WebSocket or raw TCP ────────────────────────────────────── */

/* Parse complete Content-Length–framed messages from a Buffer, calling
 * onMessage() for each, and returning the unconsumed tail. */
function drainCLMessages(buf) {
    while (true) {
        /* Find the \r\n\r\n header terminator. */
        let i = 0;
        for (; i + 3 < buf.length; i++) {
            if (buf[i] === 0x0d && buf[i+1] === 0x0a &&
                buf[i+2] === 0x0d && buf[i+3] === 0x0a) break;
        }
        if (i + 3 >= buf.length) return buf;  /* incomplete header */
        const hdr = buf.slice(0, i).toString('utf8');
        const m   = hdr.match(/Content-Length:\s*(\d+)/i);
        if (!m) return buf.slice(i + 4);      /* malformed — skip */
        const len = parseInt(m[1], 10);
        if (buf.length < i + 4 + len) return buf;  /* incomplete body */
        const body = buf.slice(i + 4, i + 4 + len).toString('utf8');
        buf = buf.slice(i + 4 + len);
        onMessage(body);
    }
}

async function connectTransport() {
    if (ENDPOINT.startsWith('tcp://')) {
        const { createConnection } = await import('net');
        const url  = new URL(ENDPOINT);
        const host = url.hostname;
        const port = parseInt(url.port, 10);
        let   tcpBuf = Buffer.alloc(0);

        await new Promise((resolve, reject) => {
            const sock = createConnection({ host, port });
            _sendRaw = (text) => {
                const hdr = `Content-Length: ${Buffer.byteLength(text, 'utf8')}\r\n\r\n`;
                sock.write(hdr + text, 'utf8');
            };
            _closeConn = () => sock.destroy();
            sock.on('data', (chunk) => {
                tcpBuf = drainCLMessages(Buffer.concat([tcpBuf, chunk]));
            });
            sock.on('connect', resolve);
            sock.on('error',   reject);
        });
    } else {
        /* WebSocket mode (ws:// or wss://) */
        const ws = new WebSocket(ENDPOINT);
        _sendRaw   = (text) => ws.send(text);
        _closeConn = () => ws.close();
        await new Promise((resolve, reject) => {
            ws.addEventListener('open',  resolve);
            ws.addEventListener('error', reject);
        });
        ws.addEventListener('message', (ev) => onMessage(
            typeof ev.data === 'string' ? ev.data : ev.data.toString()
        ));
    }
}

/* ── Main test ───────────────────────────────────────────────────────────── */

async function run() {
    await connectTransport();

    /* 1. initialize */
    const init = await request('initialize', {
        clientID:         'blyt-dap-test',
        adapterID:        'blyt-lua',
        linesStartAt1:    true,
        columnsStartAt1:  true,
    });
    assert(
        init.supportsConfigurationDoneRequest === true,
        'initialize: supportsConfigurationDoneRequest'
    );

    await nextEvent('initialized');

    /* 2. launch (adapter ignores arguments; cart is already running) */
    await request('launch', {});

    /* Exception-filter mode: skip regular BPs, just wait for an exception stop. */
    if (EXCEPTION_FILTER) {
        await request('setExceptionBreakpoints', { filters: [EXCEPTION_FILTER] });
        await request('configurationDone');
        const exStopped = await nextEvent('stopped');
        assert(exStopped.body.reason === 'exception',
               `exception stop: reason is "exception" (got "${exStopped.body.reason}")`);
        await request('continue', { threadId: 1 });
        _closeConn();
        return;
    }

    /* 3. setBreakpoints */
    const bpArgs = CONDITIONAL_COND
        ? [{ line: BP_LINE, condition: CONDITIONAL_COND }]
        : [{ line: BP_LINE }];
    const sb = await request('setBreakpoints', {
        source:      { path: SOURCE_PATH, name: SOURCE_PATH },
        breakpoints: bpArgs,
        lines:       [BP_LINE],
    });
    assert(
        Array.isArray(sb.breakpoints) && sb.breakpoints.length >= 1,
        `setBreakpoints returns ≥1 entry`
    );
    if (sb.breakpoints.length >= 1) {
        assert(sb.breakpoints[0].verified === true, `breakpoint at line ${BP_LINE} verified`);
    }

    /* 4. configurationDone — cart can now start running */
    await request('configurationDone');

    /* 5. Wait for stopped event (breakpoint) */
    const stopped = await nextEvent('stopped');
    assert(
        stopped.body.reason === 'breakpoint' || stopped.body.reason === 'step',
        `stopped.reason is breakpoint (got "${stopped.body.reason}")`
    );

    /* 6. threads */
    const threads = await request('threads');
    assert(threads.threads && threads.threads.length >= 1, 'threads: at least one thread');

    /* 7. stackTrace */
    const stack = await request('stackTrace', { threadId: 1 });
    assert(stack.stackFrames && stack.stackFrames.length >= 1, 'stackTrace: ≥1 frame');
    if (stack.stackFrames && stack.stackFrames.length >= 1) {
        const frames = stack.stackFrames;
        assert(
            frames.some(f => f.line === BP_LINE),
            `some frame is at line ${BP_LINE}`
        );
    }

    /* 8. scopes */
    const topFrameId = stack.stackFrames?.[0]?.id ?? 0;
    const scopes = await request('scopes', { frameId: topFrameId });
    assert(scopes.scopes && scopes.scopes.length >= 1, 'scopes: ≥1 scope');

    /* 9. variables */
    if (scopes.scopes && scopes.scopes.length >= 1) {
        const vars = await request('variables', {
            variablesReference: scopes.scopes[0].variablesReference,
        });
        assert(Array.isArray(vars.variables), 'variables: returns array');
    }

    /* 9b. evaluate (optional) — evaluate an arbitrary expression in the top frame */
    if (EVALUATE_EXPR) {
        const ev = await request('evaluate', {
            expression: EVALUATE_EXPR,
            frameId:    topFrameId,
            context:    'watch',
        });
        assert(typeof ev.result === 'string' && ev.result !== '?',
               `evaluate("${EVALUATE_EXPR}"): got a result`);
        if (EVALUATE_EXPECT)
            assert(ev.result === EVALUATE_EXPECT,
                   `evaluate("${EVALUATE_EXPR}"): result is "${EVALUATE_EXPECT}" (got "${ev.result}")`);
    }

    /* 9c. condition edit (optional) — update the breakpoint condition while paused,
     *     continue, and verify the new condition controls the next stop. */
    if (CONDITIONAL_COND_EDIT) {
        const stopped3P = nextEvent('stopped');
        await request('setBreakpoints', {
            source:      { path: SOURCE_PATH, name: SOURCE_PATH },
            breakpoints: [{ line: BP_LINE, condition: CONDITIONAL_COND_EDIT }],
            lines:       [BP_LINE],
        });
        await request('continue', { threadId: 1 });
        const stopped3 = await stopped3P;
        assert(
            stopped3.body.reason === 'breakpoint',
            `after condition edit: stopped with reason "breakpoint" (got "${stopped3.body.reason}")`
        );
        /* Inspect the local that the condition was written against. */
        const stk3 = await request('stackTrace', { threadId: 1 });
        const frame3Id = stk3.stackFrames?.[0]?.id ?? 0;
        const scopes3 = await request('scopes', { frameId: frame3Id });
        if (scopes3.scopes?.length >= 1) {
            const vars3 = await request('variables', {
                variablesReference: scopes3.scopes[0].variablesReference,
            });
            const EDIT_STOP_VAR = process.env.BLYT_DAP_COND_EDIT_STOP_VAR || '';
            const EDIT_STOP_VAL = process.env.BLYT_DAP_COND_EDIT_STOP_VAL || '';
            if (EDIT_STOP_VAR && EDIT_STOP_VAL && Array.isArray(vars3.variables)) {
                const v = vars3.variables.find(x => x.name === EDIT_STOP_VAR);
                assert(v != null, `condition-edit stop: variable "${EDIT_STOP_VAR}" present`);
                if (v) assert(v.value === EDIT_STOP_VAL,
                              `condition-edit stop: ${EDIT_STOP_VAR} == ${EDIT_STOP_VAL} (got ${v.value})`);
            }
        }
        await request('continue', { threadId: 1 });
        _closeConn();
        return;
    }

    /* 9d. loadedSources (optional) */
    if (LOADED_SOURCES_CHECK) {
        const ls = await request('loadedSources', {});
        assert(Array.isArray(ls.sources) && ls.sources.length >= 1,
               'loadedSources: returns ≥1 source');
    }

    /* 9e. restart (optional) — re-run the cart from scratch and stop again */
    if (TEST_RESTART) {
        const stopped3P = nextEvent('stopped');
        await request('restart', {});
        await nextEvent('initialized');
        await request('setBreakpoints', {
            source:      { path: SOURCE_PATH, name: SOURCE_PATH },
            breakpoints: [{ line: BP_LINE }],
            lines:       [BP_LINE],
        });
        await request('configurationDone');
        const stopped3 = await stopped3P;
        assert(
            stopped3.body.reason === 'breakpoint' || stopped3.body.reason === 'step',
            `after restart: stopped again (got "${stopped3.body.reason}")`
        );
        await request('continue', { threadId: 1 });
        _closeConn();
        return;
    }

    /* 10. next (step over) */
    const stopped2P = nextEvent('stopped');
    await request('next', { threadId: 1 });
    const stopped2 = await stopped2P;
    assert(
        stopped2.body.reason === 'step' || stopped2.body.reason === 'breakpoint',
        `after next: stopped.reason is step (got "${stopped2.body.reason}")`
    );

    /* 11. continue — let the cart finish */
    await request('continue', { threadId: 1 });

    _closeConn();
}

run().then(() => {
    console.log(`\n${passed} passed, ${failed} failed`);
    process.exit(failed > 0 ? 1 : 0);
}).catch((err) => {
    console.error('\nERROR:', err.message);
    process.exit(2);
});
