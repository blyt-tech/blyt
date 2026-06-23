/* Test harness for driving blyt debug sessions through the VS Code extension.
 *
 * The public DAP-event surface VS Code exposes to extensions does NOT include
 * the adapter's "stopped" events, so we install a DebugAdapterTracker for type
 * 'blyt' to observe the raw message stream. Combined with session.customRequest
 * (stackTrace/scopes/variables/continue) and the vscode.debug breakpoint API,
 * this lets a test set breakpoints, detect stops, and assert on stack frames
 * and variable values exactly as the running debug adapter reports them.
 *
 * A hybrid cart produces TWO sessions (native C via BlytGdbDapProxy + companion
 * Lua DAP). Both are type 'blyt'; we disambiguate by configuration._blytMode.
 */

const vscode = require('vscode');
const path = require('node:path');
const assert = require('node:assert');

/* session.id -> { session, stops: [], stopWaiters: [], terminated } */
const records = new Map();
/* pending waitForSession() calls: { pred, resolve, timer } */
const sessionWaiters = [];
let installed = false;

function registerSession(session) {
	let rec = records.get(session.id);
	if (!rec) {
		rec = {
			session,
			stops: [],
			stopWaiters: [],
			terminated: false,
			/* setBreakpoints responses, correlated back to their source path:
			 * [{ source, breakpoints }]. Lets a test assert what a given adapter
			 * reported for a given file (e.g. the lldb proxy verifying .lua). */
			bpResults: [],
			/* request_seq -> source.path for in-flight setBreakpoints requests. */
			bpReqSource: new Map(),
			/* pending waitBreakpointBound() calls: { line, resolve, timer }. */
			bpWaiters: [],
		};
		records.set(session.id, rec);
	}
	for (let i = sessionWaiters.length - 1; i >= 0; i--) {
		if (sessionWaiters[i].pred(session)) {
			const w = sessionWaiters.splice(i, 1)[0];
			clearTimeout(w.timer);
			w.resolve(session);
		}
	}
	return rec;
}

function install() {
	if (installed) return;
	installed = true;
	vscode.debug.registerDebugAdapterTrackerFactory('blyt', {
		createDebugAdapterTracker(session) {
			const rec = registerSession(session);
			return {
				onWillReceiveMessage(m) {
					/* VS Code -> adapter. Remember the source of each
					 * setBreakpoints request so we can label its response. */
					if (
						m.type === 'request' &&
						m.command === 'setBreakpoints'
					) {
						const src = m.arguments?.source ?? {};
						rec.bpReqSource.set(m.seq, src.path ?? src.name ?? '');
					}
				},
				onDidSendMessage(m) {
					if (
						m.type === 'response' &&
						m.command === 'setBreakpoints'
					) {
						const source = rec.bpReqSource.get(m.request_seq) ?? '';
						rec.bpReqSource.delete(m.request_seq);
						const breakpoints = m.body?.breakpoints ?? [];
						rec.bpResults.push({ source, breakpoints });
						/* Resolve any waiters whose line is now bound (verified). */
						for (const bp of breakpoints) {
							if (!bp.verified) continue;
							for (
								let i = rec.bpWaiters.length - 1;
								i >= 0;
								i--
							) {
								if (rec.bpWaiters[i].line === bp.line) {
									const w = rec.bpWaiters.splice(i, 1)[0];
									clearTimeout(w.timer);
									w.resolve();
								}
							}
						}
						return;
					}
					if (m.type !== 'event') return;
					if (m.event === 'stopped') {
						if (rec.stopWaiters.length) {
							const w = rec.stopWaiters.shift();
							clearTimeout(w.timer);
							w.resolve(m);
						} else {
							rec.stops.push(m);
						}
					} else if (m.event === 'terminated') {
						rec.terminated = true;
					}
				},
			};
		},
	});
	/* Belt-and-suspenders: also register via the start event in case a session
	 * has no adapter tracker created yet when waitForSession is called. */
	vscode.debug.onDidStartDebugSession((s) => {
		if (s.type === 'blyt') registerSession(s);
	});
}

/* Resolve the WorkspaceFolder whose basename matches `name`. */
function folder(name) {
	const f = (vscode.workspace.workspaceFolders || []).find(
		(wf) => path.basename(wf.uri.fsPath) === name,
	);
	if (!f) throw new Error(`workspace folder "${name}" not found`);
	return f;
}

function fileUri(wf, relPath) {
	return vscode.Uri.file(path.join(wf.uri.fsPath, relPath));
}

function addBreakpoint(uri, line) {
	const bp = new vscode.SourceBreakpoint(
		new vscode.Location(uri, new vscode.Position(line - 1, 0)),
	);
	vscode.debug.addBreakpoints([bp]);
	return bp;
}

function removeBreakpoint(bp) {
	vscode.debug.removeBreakpoints([bp]);
}

/* Launch a player-mode (native SDL2 window) debug session for the given folder.
 * No explicit `cart`: with one cart per window, the extension auto-detects it
 * (the real F5 path).  The extension resolves cart type itself and tags the
 * session via `_blytMode`: pure-Lua -> one 'lua' session; pure-native -> one
 * 'gdb' session; hybrid -> 'gdb' + companion 'lua'.  (`mode: 'player'` is the
 * #90 rename of the former `native`; `_blytMode: 'gdb'` the rename of the former
 * `native`.) */
async function startNative(wf) {
	install();
	const ok = await vscode.debug.startDebugging(wf, {
		type: 'blyt',
		request: 'launch',
		name: `blyt:${path.basename(wf.uri.fsPath)}`,
		mode: 'player',
	});
	if (!ok) throw new Error('vscode.debug.startDebugging returned false');
}

/* Launch a default WASM-mode debug session (no `mode`): `blyt debug <dir>`
 * serves the cart in a webview panel with the DAP/GDB relays.  Same cart-type
 * resolution and `_blytMode` tags as startNative (pure-native → 'gdb', Lua →
 * 'lua', hybrid → 'gdb' + 'lua') — the difference is the transport (browser
 * relay vs direct).  Regression coverage for the #90 WASM-debug path, including
 * the native `program` return (the `debugCart` ReferenceError). */
async function startWasm(wf) {
	install();
	const ok = await vscode.debug.startDebugging(wf, {
		type: 'blyt',
		request: 'launch',
		name: `blyt-wasm:${path.basename(wf.uri.fsPath)}`,
	});
	if (!ok) throw new Error('vscode.debug.startDebugging returned false');
}

/* Wait until a 'blyt' session matching `pred` exists. */
function waitForSession(pred, label = 'session', timeoutMs = 60000) {
	for (const rec of records.values())
		if (pred(rec.session)) return Promise.resolve(rec.session);
	return new Promise((resolve, reject) => {
		const timer = setTimeout(() => {
			const i = sessionWaiters.findIndex((w) => w.resolve === resolve);
			if (i >= 0) sessionWaiters.splice(i, 1);
			const seen = [...records.values()]
				.map((r) => r.session.configuration?._blytMode)
				.join(',');
			reject(
				new Error(
					`timeout waiting for ${label} (sessions seen: [${seen}])`,
				),
			);
		}, timeoutMs);
		sessionWaiters.push({ pred, resolve, timer });
	});
}

const byMode = (mode) => (s) => s.configuration._blytMode === mode;

/* Wait for the next 'stopped' event on a session (consumes one queued stop).
 * `label` names what we're waiting for, surfaced in the timeout message. */
function waitStopped(session, label = 'stopped event', timeoutMs = 90000) {
	const rec = records.get(session.id);
	if (!rec) return Promise.reject(new Error('no record for session'));
	if (rec.stops.length) return Promise.resolve(rec.stops.shift());
	return new Promise((resolve, reject) => {
		const timer = setTimeout(() => {
			const i = rec.stopWaiters.findIndex((w) => w.resolve === resolve);
			if (i >= 0) rec.stopWaiters.splice(i, 1);
			reject(new Error(`timeout waiting for ${label}`));
		}, timeoutMs);
		rec.stopWaiters.push({ resolve, timer });
	});
}

/* Wait for the next 'stopped' event on ANY of `sessions`, returning
 * { session, ev } for whichever stops first. Used when two sessions (native +
 * companion Lua) can stop in a platform-dependent order. */
function waitAnyStopped(sessions, label = 'stopped event', timeoutMs = 90000) {
	for (const s of sessions) {
		const rec = records.get(s.id);
		if (rec?.stops.length)
			return Promise.resolve({ session: s, ev: rec.stops.shift() });
	}
	return new Promise((resolve, reject) => {
		let settled = false;
		const entries = [];
		const cleanup = () => {
			for (const { rec, waiter } of entries) {
				const i = rec.stopWaiters.indexOf(waiter);
				if (i >= 0) rec.stopWaiters.splice(i, 1);
			}
		};
		const timer = setTimeout(() => {
			if (settled) return;
			settled = true;
			cleanup();
			reject(new Error(`timeout waiting for ${label}`));
		}, timeoutMs);
		for (const s of sessions) {
			const rec = records.get(s.id);
			if (!rec) continue;
			const waiter = {
				timer: undefined,
				resolve: (ev) => {
					if (settled) return;
					settled = true;
					clearTimeout(timer);
					cleanup();
					resolve({ session: s, ev });
				},
			};
			entries.push({ rec, waiter });
			rec.stopWaiters.push(waiter);
		}
	});
}

async function topFrame(session, threadId) {
	const st = await session.customRequest('stackTrace', {
		threadId,
		startFrame: 0,
		levels: 20,
	});
	const frames = st.stackFrames || [];
	if (!frames.length) throw new Error('empty stackTrace');
	return frames[0];
}

/* Return the Locals scope of a frame as a { name: value } map. */
async function locals(session, frameId) {
	const sc = await session.customRequest('scopes', { frameId });
	const localsScope = (sc.scopes || []).find((s) => s.name === 'Locals');
	if (!localsScope) throw new Error('no Locals scope');
	const v = await session.customRequest('variables', {
		variablesReference: localsScope.variablesReference,
	});
	const map = {};
	for (const x of v.variables || []) map[x.name] = x.value;
	return map;
}

async function cont(session, threadId) {
	await session.customRequest('continue', { threadId });
}

/* setBreakpoints responses this session's adapter returned for sources whose
 * path ends with `suffix` (e.g. '.lua'). Used to assert the lldb proxy's Fix-2
 * short-circuit reported .lua breakpoints as verified without forwarding. */
function breakpointResultsFor(session, suffix) {
	const rec = records.get(session.id);
	if (!rec) return [];
	return rec.bpResults.filter((r) => (r.source || '').endsWith(suffix));
}

/* Resolve once the adapter has reported a breakpoint at `line` as verified.
 * Call after addBreakpoint() and before continuing — otherwise a dynamically
 * added breakpoint can race the continue (the GDB stub inserts it against an
 * already-running process and it never binds), which flakes on slower runners. */
function waitBreakpointBound(session, line, timeoutMs = 30000) {
	const rec = records.get(session.id);
	if (!rec) return Promise.reject(new Error('no record for session'));
	const bound = () =>
		rec.bpResults.some((r) =>
			r.breakpoints.some((b) => b.line === line && b.verified),
		);
	if (bound()) return Promise.resolve();
	return new Promise((resolve, reject) => {
		const timer = setTimeout(() => {
			const i = rec.bpWaiters.findIndex((w) => w.resolve === resolve);
			if (i >= 0) rec.bpWaiters.splice(i, 1);
			reject(
				new Error(
					`timeout waiting for breakpoint at line ${line} to bind`,
				),
			);
		}, timeoutMs);
		rec.bpWaiters.push({ line, resolve, timer });
	});
}

/* Stop everything and clear breakpoints between tests. */
async function reset() {
	try {
		await vscode.debug.stopDebugging();
	} catch {
		/* ignore */
	}
	if (vscode.debug.breakpoints.length)
		vscode.debug.removeBreakpoints(vscode.debug.breakpoints);
	records.clear();
	sessionWaiters.length = 0;
	/* Give the extension a beat to tear down child processes. */
	await new Promise((r) => setTimeout(r, 400));
}

module.exports = {
	install,
	folder,
	fileUri,
	addBreakpoint,
	removeBreakpoint,
	startNative,
	startWasm,
	waitForSession,
	byMode,
	waitStopped,
	waitAnyStopped,
	topFrame,
	locals,
	cont,
	breakpointResultsFor,
	waitBreakpointBound,
	reset,
	assert,
};
