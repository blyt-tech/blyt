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
const fs = require('node:fs');
const crypto = require('node:crypto');
const assert = require('node:assert');

/* session.id -> { session, stops: [], stopWaiters: [], terminated } */
const records = new Map();
/* pending waitForSession() calls: { pred, resolve, timer } */
const sessionWaiters = [];

/* BLYT_IT_TIMEOUT_SCALE (default 1): every wait below multiplies its timeout by
 * this, so a loaded CI runner or an emulated container gets proportional
 * headroom without slowing fast local runs. Applied INSIDE each helper so an
 * explicit call-site timeout (e.g. the post-reload waitStopped) scales too. The
 * mocha per-test ceiling (suite/index.js) scales by the same factor and stays
 * above the summed waits, so the harness's descriptive timeout — not a bare
 * mocha kill — is what surfaces on a slow-but-progressing test. runTests.js
 * forwards the env var into each cart window. */
const TIMEOUT_SCALE = Number(process.env.BLYT_IT_TIMEOUT_SCALE) || 1;
const scaled = (ms) => Math.round(ms * TIMEOUT_SCALE);
let installed = false;

/* Append a compact, timestamped label to a session's bounded adapter trail. */
function note(rec, label) {
	rec.trail.push(`+${Date.now() - rec.t0}ms ${label}`);
	if (rec.trail.length > 60) rec.trail.shift();
}

/* One-line-ish description of a session's observed state, for timeout messages:
 * mode, whether it terminated, queued stops, and its recent adapter trail. */
function describeSession(rec) {
	const mode = rec.session.configuration?._blytMode ?? '?';
	const tail = rec.trail.slice(-12).join(' | ') || '(no adapter traffic)';
	return (
		`session[${mode}] terminated=${rec.terminated} ` +
		`queuedStops=${rec.stops.length} bpResults=${rec.bpResults.length}\n` +
		`  adapter trail: ${tail}`
	);
}

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
			/* Bounded trail of adapter traffic (compact labels + ms since this
			 * record was created), so a timeout can report what the adapter DID
			 * send instead of a bare "timeout" — the difference between "session
			 * terminated mid-reload" and "session alive, reload never arrived"
			 * (issue #249). */
			t0: Date.now(),
			trail: [],
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
						const nverified = breakpoints.filter(
							(b) => b.verified,
						).length;
						note(
							rec,
							`setBreakpoints ${path.basename(source) || '?'} ` +
								`verified=${nverified}/${breakpoints.length}`,
						);
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
					note(
						rec,
						m.event === 'stopped'
							? `stopped(${m.body?.reason ?? '?'})`
							: m.event,
					);
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

/* Await the blyt extension's activation.  It activates lazily (onDebugResolve:
 * blyt et al.), and VS Code normally activates it before resolving a launch —
 * but forcing activation to complete BEFORE we call startDebugging removes any
 * activation/registration race from the readiness path: if the debug-config
 * provider / adapter factory were not yet registered, startDebugging would
 * fast-refuse (return false).  Cached so it runs once per window (issue #304). */
let activation;
function activateExtension() {
	if (!activation) {
		const ext = vscode.extensions.getExtension('blyt.blyt');
		activation = Promise.resolve(ext ? ext.activate() : undefined);
	}
	return activation;
}

/* The extension appends a one-line reason to BLYT_IT_DIAG_FILE (when set) each
 * time it cancels a launch (resolver returns undefined) — otherwise the reason
 * lives only in the extension's Output channel, which a test cannot read.  Read
 * back the last such line so a startDebugging===false failure names its cause
 * instead of surfacing a bare boolean (issue #304). */
function lastResolverDiag() {
	const p = process.env.BLYT_IT_DIAG_FILE;
	if (!p) return '';
	try {
		const lines = fs.readFileSync(p, 'utf8').trim().split('\n');
		return lines[lines.length - 1] || '';
	} catch {
		return '';
	}
}

/* Activate the extension, call startDebugging, and turn a `false` return into a
 * self-diagnosing error: classify by how quickly it was refused (a prompt
 * refusal = the resolver returned undefined/threw; a slow one = something hung),
 * and fold in the extension's own last cancellation reason when available.
 * Every launch path goes through here (issue #304). */
async function startDebuggingChecked(wf, config, label) {
	install();
	await activateExtension();
	const t0 = Date.now();
	const ok = await vscode.debug.startDebugging(wf, config);
	if (!ok) {
		const ms = Date.now() - t0;
		const ext = vscode.extensions.getExtension('blyt.blyt');
		const kind =
			ms < 2000
				? 'fast refusal — the debug-config resolver returned undefined/null ' +
					'or threw (launch cancelled); NOT a timeout'
				: 'slow refusal';
		const diag = lastResolverDiag();
		throw new Error(
			`vscode.debug.startDebugging returned false for ${label} ` +
				`after ${ms}ms — ${kind}. extension active=${ext?.isActive}.` +
				(diag ? `\n  resolver reason: ${diag}` : ''),
		);
	}
}

/* Launch a player-mode (native SDL2 window) debug session for the given folder.
 * No explicit `cart`: with one cart per window, the extension auto-detects it
 * (the real F5 path).  The extension resolves cart type itself and tags the
 * session via `_blytMode`: pure-Lua -> one 'lua' session; pure-native -> one
 * 'gdb' session; hybrid -> 'gdb' + companion 'lua'.  (`mode: 'player'` is the
 * #90 rename of the former `native`; `_blytMode: 'gdb'` the rename of the former
 * `native`.) */
async function startNative(wf) {
	await startDebuggingChecked(
		wf,
		{
			type: 'blyt',
			request: 'launch',
			name: `blyt:${path.basename(wf.uri.fsPath)}`,
			mode: 'player',
		},
		`native (player) ${path.basename(wf.uri.fsPath)}`,
	);
}

/* Launch a default WASM-mode debug session (no `mode`): `blyt debug <dir>`
 * serves the cart in a webview panel with the DAP/GDB relays.  Same cart-type
 * resolution and `_blytMode` tags as startNative (pure-native → 'gdb', Lua →
 * 'lua', hybrid → 'gdb' + 'lua') — the difference is the transport (browser
 * relay vs direct).  Regression coverage for the #90 WASM-debug path, including
 * the native `program` return (the `debugCart` ReferenceError). */
async function startWasm(wf) {
	await startDebuggingChecked(
		wf,
		{
			type: 'blyt',
			request: 'launch',
			name: `blyt-wasm:${path.basename(wf.uri.fsPath)}`,
		},
		`wasm-debug ${path.basename(wf.uri.fsPath)}`,
	);
}

/* Wait until a 'blyt' session matching `pred` exists. */
function waitForSession(pred, label = 'session', timeoutMs = 60000) {
	timeoutMs = scaled(timeoutMs);
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
	timeoutMs = scaled(timeoutMs);
	const rec = records.get(session.id);
	if (!rec) return Promise.reject(new Error('no record for session'));
	if (rec.stops.length) return Promise.resolve(rec.stops.shift());
	return new Promise((resolve, reject) => {
		const timer = setTimeout(() => {
			const i = rec.stopWaiters.findIndex((w) => w.resolve === resolve);
			if (i >= 0) rec.stopWaiters.splice(i, 1);
			reject(
				new Error(
					`timeout waiting for ${label} after ${timeoutMs}ms\n` +
						`  ${describeSession(rec)}`,
				),
			);
		}, timeoutMs);
		rec.stopWaiters.push({ resolve, timer });
	});
}

/* Wait for the next 'stopped' event on ANY of `sessions`, returning
 * { session, ev } for whichever stops first. Used when two sessions (native +
 * companion Lua) can stop in a platform-dependent order. */
function waitAnyStopped(sessions, label = 'stopped event', timeoutMs = 90000) {
	timeoutMs = scaled(timeoutMs);
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
	timeoutMs = scaled(timeoutMs);
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

/* Trigger a reload-while-debugging cycle by editing a C source on disk: append a
 * unique `used,retain` global so the `blyt debug` file watcher sees a real change
 * and the rebuild produces a byte-distinct ELF (new content hash → lldb re-reads
 * the DWARF instead of caching), WITHOUT shifting any existing line numbers.  The
 * watcher rebuilds, signals the dev-control hub's `reload`, and blytdebug does the
 * two-phase solib swap + reboots the cart (issue #119).  Each call appends a
 * distinct marker so repeated reloads stay byte-distinct. */
let reloadMarkerSeq = 0;
function touchRebuild(uri) {
	reloadMarkerSeq += 1;
	const marker =
		`\n__attribute__((used, retain)) static volatile int ` +
		`blyt_reload_marker_${reloadMarkerSeq} = ${reloadMarkerSeq};\n`;
	fs.appendFileSync(uri.fsPath, marker);
}

/* Trigger a Lua reload-while-debugging cycle (issue #140): append a module-level
 * local variable assignment to the Lua source so the `blyt debug` file watcher
 * sees a change AND the rebuild produces byte-distinct luac output (the devtool
 * only signals a reload when the cart ELF hash changes — a comment-only edit is
 * suppressed).  `local _ = N` is exempt from luacheck unused-variable warnings;
 * the top-level placement means it does not shift any existing line numbers for
 * existing breakpoints.  Each call uses a different N so repeated reloads are
 * always byte-distinct. */
function touchRebuildLua(uri) {
	reloadMarkerSeq += 1;
	const marker = `\nlocal _ = ${reloadMarkerSeq} -- blyt reload marker\n`;
	fs.appendFileSync(uri.fsPath, marker);
}

/* Absolute path of the dev ELF the `blyt debug` watcher rebuilds on every edit
 * (build/.dbg.elf).  build_for_dev rewrites it whenever a reload actually
 * rebuilds the cart, so its bytes are the reload's real readiness signal —
 * observable from the test without reading the devtool's (extension-captured)
 * stdout. */
function devElfPath(wf) {
	return path.join(wf.uri.fsPath, 'build', '.dbg.elf');
}

/* sha1 of a file's bytes, or null if it cannot be read. */
function fileHash(p) {
	try {
		return crypto
			.createHash('sha1')
			.update(fs.readFileSync(p))
			.digest('hex');
	} catch {
		return null;
	}
}

/* Wait until the dev ELF's bytes differ from `beforeHash` — i.e. the reload's
 * rebuild has completed and produced a new cart — then return the new hash.
 * Gating the post-reload stop on THIS (instead of a bare fixed timeout) removes
 * the one behaviour the green headless reload test bypasses: the real
 * file-watcher notice + rebuild.  A slow-under-load rebuild can then no longer
 * be misread as a failed rebind, and a watcher that never notices the edit
 * surfaces as an explicit "did not rebuild" instead of a vague stop-timeout
 * (issue #249). */
async function waitCartRebuilt(elfPath, beforeHash, timeoutMs = 60000) {
	timeoutMs = scaled(timeoutMs);
	const deadline = Date.now() + timeoutMs;
	while (Date.now() < deadline) {
		const h = fileHash(elfPath);
		if (h && h !== beforeHash) return h;
		await new Promise((r) => setTimeout(r, 100));
	}
	throw new Error(
		`dev ELF ${path.basename(elfPath)} did not rebuild within ` +
			`${timeoutMs}ms (hash still ${String(beforeHash).slice(0, 8)}) — ` +
			`the file watcher never noticed the edit, or the rebuild failed`,
	);
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
	touchRebuild,
	touchRebuildLua,
	devElfPath,
	fileHash,
	waitCartRebuilt,
	reset,
	assert,
};
