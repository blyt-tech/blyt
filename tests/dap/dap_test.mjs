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

const ENDPOINT = process.argv[2];
const SOURCE_PATH = process.argv[3] || 'cart';
const BP_LINE = parseInt(process.argv[4] || '4', 10);
const TIMEOUT_MS = 60000;

/* Optional feature flags */
const LOADED_SOURCES_CHECK = !!process.env.BLYT_DAP_LOADED_SOURCES;
const CONDITIONAL_COND = process.env.BLYT_DAP_CONDITIONAL_COND || '';
const CONDITIONAL_COND_EDIT = process.env.BLYT_DAP_CONDITIONAL_COND_EDIT || '';
const TEST_RESTART = !!process.env.BLYT_DAP_TEST_RESTART;
const EXCEPTION_FILTER = process.env.BLYT_DAP_EXCEPTION_FILTER || '';
/* When set, the cart is expected to raise a Lua error during init() with no
 * exception breakpoint configured.  The driver only completes configuration and
 * lets the runtime run to its error; the orchestrator then asserts (on the
 * runtime's console output) that the error was reported and the debug runtime
 * exited cleanly rather than aborting at runtimeKeepalivePop (issue #102). */
const EXPECT_INIT_ERROR = !!process.env.BLYT_DAP_EXPECT_INIT_ERROR;
const EVALUATE_EXPR = process.env.BLYT_DAP_EVALUATE_EXPR || '';
const EVALUATE_EXPECT = process.env.BLYT_DAP_EVALUATE_EXPECT || '';
/* When set to the workspace dir, the launch request carries a sourceMap so the
 * relay reverse-maps setBreakpoints paths inward and localises stackTrace /
 * loadedSources paths outward to this dir (issue #51).  SOURCE_PATH is then the
 * local workspace path; without it, SOURCE_PATH is the canonical /blyt/cart one. */
const CWD = process.env.BLYT_DAP_CWD || '';

if (!ENDPOINT) {
	process.stderr.write(
		'usage: dap_test.mjs <endpoint> <source_path> <bp_line>\n',
	);
	process.exit(2);
}

/* ── DAP session state ──────────────────────────────────────────────────── */

let seq = 1;
let _sendRaw; /* set by connectTransport(); sends a raw JSON string */
let _closeConn; /* set by connectTransport(); tears down the connection */
const pending = new Map(); /* request_seq → { resolve, reject, timer } */
const eventQueue = []; /* buffered events not yet consumed */
const waiters = []; /* waiters registered via nextEvent() */

function nextSeq() {
	return seq++;
}

function send(obj) {
	_sendRaw(JSON.stringify(obj));
}

function request(command, args = {}) {
	const reqSeq = nextSeq();
	return new Promise((resolve, reject) => {
		const timer = setTimeout(
			() => reject(new Error(`timeout waiting for ${command}`)),
			TIMEOUT_MS,
		);
		pending.set(reqSeq, { resolve, reject, timer });
		send({ seq: reqSeq, type: 'request', command, arguments: args });
	});
}

function nextEvent(name) {
	return new Promise((resolve, reject) => {
		const timer = setTimeout(
			() => reject(new Error(`timeout waiting for event "${name}"`)),
			TIMEOUT_MS,
		);
		const check = (e) => {
			if (e.event === name) {
				clearTimeout(timer);
				const widx = waiters.indexOf(check);
				if (widx >= 0) waiters.splice(widx, 1);
				/* Consume the event so a later nextEvent() for the same name
				 * waits for a fresh one instead of resolving with this stale
				 * entry (e.g. the restart flow must observe the *second*
				 * "stopped", not instantly re-consume the first). */
				const qidx = eventQueue.indexOf(e);
				if (qidx >= 0) eventQueue.splice(qidx, 1);
				resolve(e);
				return true;
			}
			return false;
		};
		/* Check already-buffered (unconsumed) events first. */
		for (const e of eventQueue) {
			if (check(e)) return;
		}
		waiters.push(check);
	});
}

function onMessage(text) {
	let msg;
	try {
		msg = JSON.parse(text);
	} catch {
		return;
	}

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
			if (
				buf[i] === 0x0d &&
				buf[i + 1] === 0x0a &&
				buf[i + 2] === 0x0d &&
				buf[i + 3] === 0x0a
			)
				break;
		}
		if (i + 3 >= buf.length) return buf; /* incomplete header */
		const hdr = buf.slice(0, i).toString('utf8');
		const m = hdr.match(/Content-Length:\s*(\d+)/i);
		if (!m) return buf.slice(i + 4); /* malformed — skip */
		const len = parseInt(m[1], 10);
		if (buf.length < i + 4 + len) return buf; /* incomplete body */
		const body = buf.slice(i + 4, i + 4 + len).toString('utf8');
		buf = buf.slice(i + 4 + len);
		onMessage(body);
	}
}

async function connectTransport() {
	if (ENDPOINT.startsWith('tcp://')) {
		const { createConnection } = await import('node:net');
		const url = new URL(ENDPOINT);
		const host = url.hostname;
		const port = parseInt(url.port, 10);
		let tcpBuf = Buffer.alloc(0);

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
			sock.on('error', reject);
		});
	} else {
		/* WebSocket mode (ws:// or wss://) */
		const ws = new WebSocket(ENDPOINT);
		_sendRaw = (text) => ws.send(text);
		_closeConn = () => ws.close();
		await new Promise((resolve, reject) => {
			ws.addEventListener('open', resolve);
			ws.addEventListener('error', reject);
		});
		ws.addEventListener('message', (ev) =>
			onMessage(
				typeof ev.data === 'string' ? ev.data : ev.data.toString(),
			),
		);
	}
}

/* ── Main test ───────────────────────────────────────────────────────────── */

async function run() {
	await connectTransport();

	/* 1. initialize */
	const init = await request('initialize', {
		clientID: 'blyt-dap-test',
		adapterID: 'blyt-lua',
		linesStartAt1: true,
		columnsStartAt1: true,
	});
	assert(
		init.supportsConfigurationDoneRequest === true,
		'initialize: supportsConfigurationDoneRequest',
	);

	await nextEvent('initialized');

	/* 2. launch — when a workspace dir is configured, hand the relay a sourceMap
	 * so it reverse-maps breakpoints inward and localises frames outward (#51). */
	const launchArgs = CWD
		? { sourceMap: ['/blyt/cart', CWD, '/blyt/src', CWD] }
		: {};
	await request('launch', launchArgs);

	/* Init-error mode: no breakpoints, no exception filter.  Just finish
	 * configuration and let the cart error during init().  The runtime must
	 * report the error and exit cleanly (issue #102); the orchestrator awaits
	 * the runtime's exit and checks its console output. */
	if (EXPECT_INIT_ERROR) {
		await request('configurationDone');
		_closeConn();
		return;
	}

	/* Exception-filter mode: skip regular BPs, just wait for an exception stop. */
	if (EXCEPTION_FILTER) {
		await request('setExceptionBreakpoints', {
			filters: [EXCEPTION_FILTER],
		});
		await request('configurationDone');
		const exStopped = await nextEvent('stopped');
		assert(
			exStopped.body.reason === 'exception',
			`exception stop: reason is "exception" (got "${exStopped.body.reason}")`,
		);
		await request('continue', { threadId: 1 });
		_closeConn();
		return;
	}

	/* 3. setBreakpoints */
	const bpArgs = CONDITIONAL_COND
		? [{ line: BP_LINE, condition: CONDITIONAL_COND }]
		: [{ line: BP_LINE }];
	const sb = await request('setBreakpoints', {
		source: { path: SOURCE_PATH, name: SOURCE_PATH },
		breakpoints: bpArgs,
		lines: [BP_LINE],
	});
	assert(
		Array.isArray(sb.breakpoints) && sb.breakpoints.length >= 1,
		`setBreakpoints returns ≥1 entry`,
	);
	if (sb.breakpoints.length >= 1) {
		assert(
			sb.breakpoints[0].verified === true,
			`breakpoint at line ${BP_LINE} verified`,
		);
	}

	/* 4. configurationDone — cart can now start running */
	await request('configurationDone');

	/* 5. Wait for stopped event (breakpoint) */
	const stopped = await nextEvent('stopped');
	assert(
		stopped.body.reason === 'breakpoint' || stopped.body.reason === 'step',
		`stopped.reason is breakpoint (got "${stopped.body.reason}")`,
	);

	/* 6. threads */
	const threads = await request('threads');
	assert(
		threads.threads && threads.threads.length >= 1,
		'threads: at least one thread',
	);

	/* 7. stackTrace */
	const stack = await request('stackTrace', { threadId: 1 });
	assert(
		stack.stackFrames && stack.stackFrames.length >= 1,
		'stackTrace: ≥1 frame',
	);
	if (stack.stackFrames && stack.stackFrames.length >= 1) {
		const frames = stack.stackFrames;
		assert(
			frames.some((f) => f.line === BP_LINE),
			`some frame is at line ${BP_LINE}`,
		);
		const withPath = frames.find((f) => f.source?.path);
		if (withPath && !CWD) {
			/* No sourceMap configured: the relay passes the guest's canonical
			 * /blyt/cart/… chunk-name path through unchanged.  This is the #46/#47
			 * invariant — chunk names are machine-independent (closes #26). */
			assert(
				withPath.source.path.startsWith('/blyt/cart/'),
				`stackTrace source.path is canonical (got '${withPath.source.path}')`,
			);
		} else if (withPath) {
			/* sourceMap configured: the relay localises EVERY frame outward to the
			 * workspace so call-stack-click navigation works (issue #51).  Assert a
			 * non-top frame too, to guard against top-frame-only rewriting. */
			assert(
				!withPath.source.path.startsWith('/blyt/'),
				`stackTrace source.path is localised, not canonical (got '${withPath.source.path}')`,
			);
			assert(
				withPath.source.path.startsWith(CWD),
				`stackTrace source.path is under the workspace (got '${withPath.source.path}')`,
			);
			const nonTop = frames.slice(1).find((f) => f.source?.path);
			assert(
				nonTop?.source.path.startsWith(CWD),
				`non-top frame source.path is localised (got '${nonTop ? nonTop.source.path : 'none'}')`,
			);
		}
	}

	/* 7b. source — VS Code fetches content for sources it cannot open by
	 * path (e.g. the relative paths Lua chunk names produce).
	 * sourceReference 0 means "the file lives on disk at source.path": the
	 * adapter must answer success with no content so the client keeps the
	 * user's editor buffer instead of replacing it with an error message or
	 * an empty payload. */
	const src = await request('source', {
		source: { path: SOURCE_PATH, sourceReference: 0 },
		sourceReference: 0,
	});
	assert(!src.content, 'source(ref=0): success with no content payload');

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
			frameId: topFrameId,
			context: 'watch',
		});
		assert(
			typeof ev.result === 'string' && ev.result !== '?',
			`evaluate("${EVALUATE_EXPR}"): got a result`,
		);
		if (EVALUATE_EXPECT)
			assert(
				ev.result === EVALUATE_EXPECT,
				`evaluate("${EVALUATE_EXPR}"): result is "${EVALUATE_EXPECT}" (got "${ev.result}")`,
			);
	}

	/* 9c. condition edit (optional) — update the breakpoint condition while paused,
	 *     continue, and verify the new condition controls the next stop. */
	if (CONDITIONAL_COND_EDIT) {
		const stopped3P = nextEvent('stopped');
		await request('setBreakpoints', {
			source: { path: SOURCE_PATH, name: SOURCE_PATH },
			breakpoints: [{ line: BP_LINE, condition: CONDITIONAL_COND_EDIT }],
			lines: [BP_LINE],
		});
		await request('continue', { threadId: 1 });
		const stopped3 = await stopped3P;
		assert(
			stopped3.body.reason === 'breakpoint',
			`after condition edit: stopped with reason "breakpoint" (got "${stopped3.body.reason}")`,
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
			if (
				EDIT_STOP_VAR &&
				EDIT_STOP_VAL &&
				Array.isArray(vars3.variables)
			) {
				const v = vars3.variables.find((x) => x.name === EDIT_STOP_VAR);
				assert(
					v != null,
					`condition-edit stop: variable "${EDIT_STOP_VAR}" present`,
				);
				if (v)
					assert(
						v.value === EDIT_STOP_VAL,
						`condition-edit stop: ${EDIT_STOP_VAR} == ${EDIT_STOP_VAL} (got ${v.value})`,
					);
			}
		}
		await request('continue', { threadId: 1 });
		_closeConn();
		return;
	}

	/* 9d. loadedSources (optional) */
	if (LOADED_SOURCES_CHECK) {
		const ls = await request('loadedSources', {});
		assert(
			Array.isArray(ls.sources) && ls.sources.length >= 1,
			'loadedSources: returns ≥1 source',
		);
	}

	/* 9e. restart (optional) — re-run the cart from scratch and stop again.
	 * Two full cycles: the second proves restart works from an already
	 * restarted session, not just from the initial one.  Mirrors the manual
	 * VS Code flow: restart → breakpoint hit again → locals inspectable. */
	if (TEST_RESTART) {
		for (let cycle = 1; cycle <= 2; cycle++) {
			const stoppedP = nextEvent('stopped');
			await request('restart', {});
			await nextEvent('initialized');
			await request('setBreakpoints', {
				source: { path: SOURCE_PATH, name: SOURCE_PATH },
				breakpoints: [{ line: BP_LINE }],
				lines: [BP_LINE],
			});
			await request('configurationDone');
			const stopped = await stoppedP;
			assert(
				stopped.body.reason === 'breakpoint' ||
					stopped.body.reason === 'step',
				`restart ${cycle}: stopped again (got "${stopped.body.reason}")`,
			);
			/* Locals are inspectable in the restarted session.  At the
			 * line-3 stop, init has executed `local x = 42` but not yet
			 * `local y = x + 1`. */
			const rStack = await request('stackTrace', { threadId: 1 });
			const rFrameId = rStack.stackFrames?.[0]?.id ?? 0;
			const rScopes = await request('scopes', { frameId: rFrameId });
			const rVars = await request('variables', {
				variablesReference: rScopes.scopes[0].variablesReference,
			});
			const x = (rVars.variables || []).find((v) => v.name === 'x');
			assert(
				x && x.value === '42',
				`restart ${cycle}: local x == 42 in restarted session (got ${x ? x.value : 'missing'})`,
			);
		}
		await request('continue', { threadId: 1 });
		_closeConn();
		return;
	}

	/* 10. next (step over) */
	const stopped2P = nextEvent('stopped');
	await request('next', { threadId: 1 });
	const stopped2 = await stopped2P;
	assert(
		stopped2.body.reason === 'step' ||
			stopped2.body.reason === 'breakpoint',
		`after next: stopped.reason is step (got "${stopped2.body.reason}")`,
	);

	/* 11. continue — let the cart finish */
	await request('continue', { threadId: 1 });

	_closeConn();
}

run()
	.then(() => {
		console.log(`\n${passed} passed, ${failed} failed`);
		process.exit(failed > 0 ? 1 : 0);
	})
	.catch((err) => {
		console.error('\nERROR:', err.message);
		process.exit(2);
	});
