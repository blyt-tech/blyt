const vscode = require('vscode');
const cp = require('node:child_process');
const path = require('node:path');
const fs = require('node:fs');
const yaml = require('js-yaml');

/* ── Process tracking ─────────────────────────────────────────────────────── */

/* Processes are tracked in two stages:
 *   pendingProcs: started during resolveDebugConfiguration, keyed by a
 *                 temporary numeric ID stored in the config object.
 *   sessionProcs: moved here in createDebugAdapterDescriptor once VS Code
 *                 assigns a session ID; cleaned up on session termination.
 *
 * pendingUrls / sessionUrls track the cart HTTP URL for the same lifecycle. */
const pendingProcs = new Map();
const sessionProcs = new Map();
const pendingUrls = new Map();
const sessionUrls = new Map();
let nextId = 0;
let g_gamePanel = null; /* custom WebviewPanel for the cart, or null */

/* ── Cart game panel ──────────────────────────────────────────────────────── */

/* Build the HTML for the custom game panel WebviewPanel.
 * The panel embeds an iframe pointing at the blyt HTTP server.
 * A message listener lets openCartPage() navigate to a new URL without
 * rebuilding the panel (postMessage { type: 'blyt-navigate', url }). */
function makeGameHtml(url) {
	const safeUrl = url.replace(/"/g, '&quot;');
	return `<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta http-equiv="Content-Security-Policy"
      content="default-src 'none'; frame-src http://127.0.0.1:* http://localhost:*; script-src 'unsafe-inline';">
<style>
body {
    /* position:fixed bypasses VS Code's webview CSS injections (margin,
     * padding, height:auto) that break the normal height:100% cascade. */
    position: fixed; top: 0; left: 0; right: 0; bottom: 0;
    margin: 0; padding: 0;
    background: #000; overflow: hidden;
    display: flex; align-items: center; justify-content: center;
}
#f { display: block; border: 0; flex-shrink: 0; }
</style>
</head>
<body>
<iframe id="f" src="${safeUrl}" frameborder="0" scrolling="no"></iframe>
<script>
/* Size the iframe to the largest 4:3 rectangle that fits the panel.
 * Flexbox centres it. Done in the outer wrapper where innerWidth/Height
 * are reliable (no iframe nesting issues). */
function fitFrame() {
    var vw = window.innerWidth, vh = window.innerHeight;
    var f  = document.getElementById('f');
    var w, h;
    if (vw * 3 > vh * 4) { h = vh; w = Math.floor(vh * 4 / 3); }
    else                  { w = vw; h = Math.floor(vw * 3 / 4); }
    f.style.width  = w + 'px';
    f.style.height = h + 'px';
}
window.addEventListener('resize', fitFrame);
fitFrame();

window.addEventListener('message', function(e) {
    if (e.data && e.data.type === 'blyt-navigate')
        document.getElementById('f').src = e.data.url;
});
</script>
</body>
</html>`;
}

/* ── Locate blyt executables ──────────────────────────────────────────────── */

/* Returns the SDK directory, or empty string if not configured. */
function sdkDir() {
	return (
		vscode.workspace.getConfiguration('blyt').get('sdkDir', '').trim() ||
		process.env.BLYT_SDK_DIR ||
		''
	).trim();
}

/* Canonical → local source-path pairs for a cart at `cwd` (issue #46).  Carts
 * embed canonical /blyt/* paths in their debug info; the debugger reverses them
 * here.  /blyt/cart and the legacy /blyt/src map to the workspace folder (so the
 * user's own files open); /blyt/sdk, /blyt/rust and /blyt/cargo come from
 * build/source-map.json, which `blyt build` writes (the SDK and toolchain source
 * trees).  Returns an array of [canonical, local] pairs. */
function sourceMapPairs(cwd) {
	const pairs = [
		['/blyt/cart', cwd],
		['/blyt/src', cwd],
	];
	try {
		const manifest = JSON.parse(
			fs.readFileSync(path.join(cwd, 'build', 'source-map.json'), 'utf8'),
		);
		for (const { prefix, local } of manifest) {
			if (prefix === '/blyt/cart' || prefix === '/blyt/src') continue;
			if (prefix && local) pairs.push([prefix, local]);
		}
	} catch (_) {
		/* manifest is optional — fall back to the cart mapping alone. */
	}
	return pairs;
}

/* The lldb-dap launchCommand that installs every source-map pair in one call:
 *   settings set target.source-map "<canon1>" "<local1>" "<canon2>" "<local2>" …
 * lldb resolves DWARF paths and source breakpoints in both directions. */
function sourceMapCommand(cwd) {
	const args = sourceMapPairs(cwd)
		.map(([c, l]) => `${JSON.stringify(c)} ${JSON.stringify(l)}`)
		.join(' ');
	return `settings set target.source-map ${args}`;
}

/* Rewrite a canonical /blyt/cart (or legacy /blyt/src) source path to the local
 * workspace file, for the Lua DAP path (no lldb in the loop to apply the
 * source-map).  Returns the input unchanged if it is not a cart path. */
function localizeCartPath(p, cwd) {
	for (const prefix of ['/blyt/cart', '/blyt/src']) {
		if (p === prefix) return cwd;
		if (p.startsWith(`${prefix}/`))
			return path.join(cwd, p.slice(prefix.length + 1));
	}
	return p;
}

/* BLYT_TRACE channels for debug sessions, passed as a --trace parameter to
 * blytdebug / `blyt debug` so failures always carry a protocol/lifecycle
 * trace ('api' stays opt-in — high volume).  Empty disables tracing. */
function traceChannels() {
	return vscode.workspace
		.getConfiguration('blyt')
		.get('traceChannels', 'gdb,dap,lifecycle,frame')
		.trim();
}

/* Returns the path to the blyt binary, or null (silently) if the SDK is not
 * configured.  Use this for background / auto-setup paths. */
function findBlytSilent() {
	const sdk = sdkDir();
	if (!sdk) return null;
	return path.join(sdk, 'bin', 'blyt');
}

/* Returns the path to the blyt binary, or null after showing an error
 * notification.  Use this when the user has explicitly triggered an action. */
function findBlyt() {
	const blyt = findBlytSilent();
	if (!blyt) {
		vscode.window.showErrorMessage(
			'Blyt: set blyt.sdkDir in VS Code settings (or BLYT_SDK_DIR env var) to the blyt SDK directory.',
		);
	}
	return blyt;
}

/* Returns the path to an SDK binary by name (e.g. 'blytplay', 'blytdebug'),
 * or null after showing an error notification if the SDK is not configured. */
function findSdkBin(name) {
	const sdk = sdkDir();
	if (!sdk) {
		vscode.window.showErrorMessage(
			'Blyt: set blyt.sdkDir in VS Code settings (or BLYT_SDK_DIR env var) to the blyt SDK directory.',
		);
		return null;
	}
	return path.join(sdk, 'bin', name);
}

/* Find blyt-lldb-dap, the SDK-bundled LLDB Debug Adapter Protocol server.
 * Assembled by `cmake --build build --target sdk` alongside blyt-clang. */
function findLldbDap() {
	const sdk = sdkDir();
	if (sdk) {
		const candidate = path.join(sdk, 'bin', 'blyt-lldb-dap');
		if (fs.existsSync(candidate)) return candidate;
	}
	return 'lldb-dap'; // fallback: rely on PATH
}

/* ── Inline debug adapter for Run Without Debugging (no debug ports) ────── */

/* Minimal DAP adapter: accepts the VS Code handshake, manages the process
 * lifecycle, and sends `terminated` when the process exits.  No breakpoints,
 * no stack frames — this is purely a process wrapper so VS Code's stop button
 * kills the cart server.  Used by the `blyt` descriptor factory when
 * config.noDebug is set (Ctrl+F5 / Run Without Debugging). */
class BlytRunAdapter {
	constructor(proc, output) {
		this._proc = proc;
		this._output = output;
		this._seq = 0;
		/* VS Code's DebugAdapterInlineImplementation requires the DebugAdapter
		 * interface: onDidSendMessage must be a vscode.Event<T>.  Expose an
		 * EventEmitter's .event and fire it to send messages to VS Code. */
		this._emitter = new vscode.EventEmitter();
		this.onDidSendMessage = this._emitter.event;
		proc.on('exit', () => {
			this._emitter.fire({
				seq: ++this._seq,
				type: 'event',
				event: 'terminated',
				body: {},
			});
		});
	}
	_respond(req, body) {
		this._emitter.fire({
			seq: ++this._seq,
			type: 'response',
			request_seq: req.seq,
			success: true,
			command: req.command,
			body: body || {},
		});
	}
	onError(err) {
		this._output.appendLine(`[run] adapter error: ${err.message}`);
	}
	onClose() {}
	handleMessage(msg) {
		if (msg.type !== 'request') return;
		switch (msg.command) {
			case 'initialize':
				this._respond(msg, {});
				this._emitter.fire({
					seq: ++this._seq,
					type: 'event',
					event: 'initialized',
					body: {},
				});
				break;
			case 'launch':
			case 'configurationDone':
				this._respond(msg, {});
				break;
			case 'setBreakpoints':
				this._respond(msg, {
					breakpoints: (msg.arguments?.breakpoints ?? []).map(() => ({
						verified: false,
					})),
				});
				break;
			case 'setExceptionBreakpoints':
				this._respond(msg, { breakpoints: [] });
				break;
			case 'threads':
				this._respond(msg, { threads: [] });
				break;
			case 'disconnect':
			case 'terminate':
				try {
					this._proc.kill();
				} catch (_) {}
				this._respond(msg, {});
				break;
			default:
				this._emitter.fire({
					seq: ++this._seq,
					type: 'response',
					request_seq: msg.seq,
					success: false,
					command: msg.command,
					message: 'not supported',
				});
		}
	}
	dispose() {
		try {
			this._proc.kill();
		} catch (_) {}
	}
}

/* ── Start blyt build ────────────────────────────────────────────────────── */

function buildCart(cwd, output, debug = false) {
	const blyt = findBlyt();
	if (!blyt) return Promise.reject(new Error('blyt SDK not configured'));
	/* Pass --debug to include DWARF debug info (-g -O0) so source-line
	 * breakpoints and variable inspection work in GDB/DAP debug sessions. */
	const args = debug ? ['build', '--debug', cwd] : ['build', cwd];
	return new Promise((resolve, reject) => {
		const proc = cp.spawn(blyt, args, {
			stdio: ['ignore', 'pipe', 'pipe'],
		});
		proc.stdout.on('data', (d) => output.append(d.toString()));
		proc.stderr.on('data', (d) => output.append(d.toString()));
		proc.on('error', (err) =>
			reject(new Error(`Could not start blyt build: ${err.message}`)),
		);
		proc.on('exit', (code) => {
			if (code === 0) resolve();
			else reject(new Error(`blyt build failed (exit code ${code})`));
		});
	});
}

/* ── Start blyt debug ─────────────────────────────────────────────────────── */

/* Spawns `blyt debug <cartPath>` (ADR-0129) and resolves once the process
 * prints both the HTTP and DAP ports.  All stdout/stderr is forwarded to
 * `output`.  The announce text is unchanged from the old `blyt run --debug`,
 * so the port regexes below still match. */
function startBlytRun(cartPath, cwd, output) {
	const blyt = findBlyt();
	if (!blyt) return Promise.reject(new Error('blyt SDK not configured'));
	const args = ['debug', cartPath];
	const trace = traceChannels();
	/* Trace lands in the BROWSER dev console for this flow: the WASM runtime's
	 * stderr goes to printErr in the page, not to this process. */
	if (trace) args.push(`--trace=${trace}`);
	return new Promise((resolve, reject) => {
		const proc = cp.spawn(blyt, args, {
			cwd,
			stdio: ['ignore', 'pipe', 'pipe'],
		});

		let buf = '';
		let httpPort = 0;
		let gdbPort = 0;
		let resolved = false;

		function check(data) {
			buf += data.toString();
			output.append(data.toString());

			if (!httpPort) {
				const m = buf.match(/serving on http:\/\/127\.0\.0\.1:(\d+)/);
				if (m) httpPort = parseInt(m[1], 10);
			}

			if (!gdbPort) {
				const g = buf.match(/GDB debugger:\s+127\.0\.0\.1:(\d+)/);
				if (g) {
					gdbPort = parseInt(g[1], 10);
					output.appendLine(
						`\n── GDB: connect gdb-multiarch to 127.0.0.1:${gdbPort}` +
							`\n       (set architecture riscv:rv32, then: target remote :${gdbPort})`,
					);
				}
			}

			const m = buf.match(/DAP debugger \(Lua\):\s+127\.0\.0\.1:(\d+)/);
			if (m && !resolved) {
				resolved = true;
				resolve({
					proc,
					httpPort,
					dapPort: parseInt(m[1], 10),
					gdbPort,
				});
			}
		}

		proc.stdout.on('data', check);
		proc.stderr.on('data', (d) => output.append(d.toString()));

		proc.on('error', (err) => {
			if (!resolved)
				reject(
					new Error(
						`Could not start blyt: ${err.message}\n` +
							'Set blyt.sdkDir in VS Code settings, or set BLYT_SDK_DIR.',
					),
				);
		});

		proc.on('exit', (code) => {
			if (!resolved)
				reject(
					new Error(
						`blyt exited (code ${code}) before the DAP relay was ready`,
					),
				);
		});

		setTimeout(() => {
			if (!resolved) {
				proc.kill();
				reject(new Error('blyt debug did not start within 15 s'));
			}
		}, 15000);
	});
}

/* Spawns `blyt run <cartPath>` (no --debug) and resolves once the HTTP port
 * is announced.  No DAP or GDB ports are opened. */
function startBlytRunSimple(cartPath, cwd, output) {
	const blyt = findBlyt();
	if (!blyt) return Promise.reject(new Error('blyt SDK not configured'));
	return new Promise((resolve, reject) => {
		const proc = cp.spawn(blyt, ['run', cartPath], {
			cwd,
			stdio: ['ignore', 'pipe', 'pipe'],
		});
		let buf = '',
			resolved = false;
		function check(data) {
			buf += data.toString();
			output.append(data.toString());
			const m = buf.match(/serving on http:\/\/127\.0\.0\.1:(\d+)/);
			if (m && !resolved) {
				resolved = true;
				resolve({ proc, httpPort: parseInt(m[1], 10) });
			}
		}
		proc.stdout.on('data', check);
		proc.stderr.on('data', (d) => output.append(d.toString()));
		proc.on('error', (err) => {
			if (!resolved)
				reject(new Error(`Could not start blyt: ${err.message}`));
		});
		proc.on('exit', (code) => {
			if (!resolved)
				reject(new Error(`blyt exited (code ${code}) before serving`));
		});
		setTimeout(() => {
			if (!resolved) {
				proc.kill();
				reject(new Error('blyt run did not start within 15 s'));
			}
		}, 15000);
	});
}

/* Spawns `blytdebug --gdb 0 <cartPath>` (native cart) or
 * `blytdebug --debug 0 <cartPath>` (Lua cart) and resolves once the process
 * announces a debug port on stdout.  SDK binaries locate their own lib dir
 * relative to themselves so no BLYT_LIB_DIR is needed. */
function startNativeDebug(cartPath, cwd, output, isLua) {
	const blytdebug = findSdkBin('blytdebug');
	if (!blytdebug)
		return Promise.reject(new Error('blytdebug not found in SDK'));
	const flag = isLua ? '--debug' : '--gdb';
	const args = [flag, '0', cartPath];
	const trace = traceChannels();
	if (trace) args.unshift(`--trace=${trace}`);
	return new Promise((resolve, reject) => {
		const proc = cp.spawn(blytdebug, args, {
			cwd,
			stdio: ['ignore', 'pipe', 'pipe'],
		});

		let buf = '',
			resolved = false;

		function check(data) {
			buf += data.toString();
			output.append(data.toString());
			if (isLua) {
				const m = buf.match(/blyt: DAP listening on port (\d+)/);
				if (m && !resolved) {
					resolved = true;
					resolve({ proc, dapPort: parseInt(m[1], 10) });
				}
			} else {
				const m = buf.match(/blyt: GDB listening on port (\d+)/);
				if (m && !resolved) {
					resolved = true;
					resolve({ proc, gdbPort: parseInt(m[1], 10) });
				}
			}
		}

		proc.stdout.on('data', check);
		proc.stderr.on('data', (d) => output.append(d.toString()));

		proc.on('error', (err) => {
			if (!resolved)
				reject(new Error(`Could not start blytdebug: ${err.message}`));
		});

		proc.on('exit', (code) => {
			if (!resolved)
				reject(
					new Error(
						`blytdebug exited (code ${code}) before the debug port was ready`,
					),
				);
		});

		setTimeout(() => {
			if (!resolved) {
				proc.kill();
				reject(
					new Error(
						'blytdebug did not announce a debug port within 15 s',
					),
				);
			}
		}, 15000);
	});
}

/* Spawns `blytdebug --debug 0 --gdb 0 <cartPath>` for hybrid Lua+native carts
 * and resolves once BOTH the DAP port (Lua) and GDB port (native) are announced.
 * The caller starts two debug sessions that share this process. */
function startHybridNativeDebug(cartPath, cwd, output) {
	const blytdebug = findSdkBin('blytdebug');
	if (!blytdebug)
		return Promise.reject(new Error('blytdebug not found in SDK'));
	const hybridArgs = ['--debug', '0', '--gdb', '0', cartPath];
	const hybridTrace = traceChannels();
	if (hybridTrace) hybridArgs.unshift(`--trace=${hybridTrace}`);
	return new Promise((resolve, reject) => {
		const proc = cp.spawn(blytdebug, hybridArgs, {
			cwd,
			stdio: ['ignore', 'pipe', 'pipe'],
		});

		let buf = '',
			dapPort = 0,
			gdbPort = 0,
			resolved = false;

		function tryResolve() {
			if (dapPort && gdbPort && !resolved) {
				resolved = true;
				resolve({ proc, dapPort, gdbPort });
			}
		}

		function check(data) {
			buf += data.toString();
			output.append(data.toString());
			if (!dapPort) {
				const m = buf.match(/blyt: DAP listening on port (\d+)/);
				if (m) dapPort = parseInt(m[1], 10);
			}
			if (!gdbPort) {
				const m = buf.match(/blyt: GDB listening on port (\d+)/);
				if (m) gdbPort = parseInt(m[1], 10);
			}
			tryResolve();
		}

		proc.stdout.on('data', check);
		proc.stderr.on('data', (d) => output.append(d.toString()));

		proc.on('error', (err) => {
			if (!resolved)
				reject(new Error(`Could not start blytdebug: ${err.message}`));
		});
		proc.on('exit', (code) => {
			if (!resolved)
				reject(
					new Error(
						`blytdebug exited (code ${code}) before debug ports were ready`,
					),
				);
		});
		setTimeout(() => {
			if (!resolved) {
				proc.kill();
				reject(
					new Error(
						'blytdebug did not announce both debug ports within 15 s',
					),
				);
			}
		}, 15000);
	});
}

/* ── Cart project detection ───────────────────────────────────────────────── */

/* Walk up from startPath to find the nearest directory containing
 * blyt.info.yaml.  Returns the project directory path, or null. */
function findCartProject(startPath) {
	let dir = startPath;
	try {
		if (!fs.statSync(startPath).isDirectory())
			dir = path.dirname(startPath);
	} catch {
		return null;
	}
	while (true) {
		if (fs.existsSync(path.join(dir, 'blyt.info.yaml'))) return dir;
		const parent = path.dirname(dir);
		if (parent === dir) return null;
		dir = parent;
	}
}

/* Read and parse blyt.build.yaml, returning the YAML object or null.
 * A missing file is normal (pure Lua carts may omit it); parse errors are
 * swallowed so a malformed manifest does not prevent the extension from
 * launching the cart. */
function readBuildManifest(projectDir) {
	const p = path.join(projectDir, 'blyt.build.yaml');
	if (!fs.existsSync(p)) return null;
	try {
		return yaml.load(fs.readFileSync(p, 'utf8'));
	} catch {
		return null;
	}
}

/* Return the declared language set for a cart project.  Handles both the
 * singular `language: lua` shorthand and the map form `languages: { lua:, c: }`.
 * Falls back to {'lua'} when no manifest exists (pure-Lua default). */
function cartLanguages(projectDir) {
	const manifest = readBuildManifest(projectDir);
	if (!manifest) return new Set(['lua']);
	if (typeof manifest.language === 'string')
		return new Set([manifest.language]);
	if (manifest.languages && typeof manifest.languages === 'object')
		return new Set(Object.keys(manifest.languages));
	return new Set(['lua']);
}

function isLuaCart(projectDir) {
	const langs = cartLanguages(projectDir);
	return langs.has('lua') && langs.size === 1;
}

function isHybridCart(projectDir) {
	const langs = cartLanguages(projectDir);
	return langs.has('lua') && langs.size > 1;
}

/* Return the cart's machine `id` from blyt.info.yaml — this is what
 * `blyt build` uses for the output filename (`<id>.blyt`), see
 * default_output in devtool/src/build.rs.  Falls back to the directory
 * basename when the manifest is missing or unparseable, matching the old
 * behaviour for projects where dir name == id (every in-repo example). */
function cartId(projectDir) {
	const p = path.join(projectDir, 'blyt.info.yaml');
	try {
		const info = yaml.load(fs.readFileSync(p, 'utf8'));
		if (info && typeof info.id === 'string' && info.id) return info.id;
	} catch {
		/* fall through to basename */
	}
	return path.basename(projectDir);
}

/* Find any blyt cart project (Lua or native) for the current context.
 * Returns { projectDir, cart } or null. */
function detectAnyCart(folder) {
	const candidates = [];
	const activeFile = vscode.window.activeTextEditor?.document.uri.fsPath;
	if (activeFile) candidates.push(activeFile);
	if (folder) candidates.push(folder.uri.fsPath);

	for (const start of candidates) {
		const projectDir = findCartProject(start);
		if (projectDir) {
			const id = cartId(projectDir);
			const cart = path.join(projectDir, 'build', `${id}.blyt`);
			return { projectDir, cart };
		}
	}
	return null;
}

/* ── VS Code project setup ────────────────────────────────────────────────── */

/* For each workspace folder that is any blyt cart project (Lua or native),
 * run `blyt setup vscode <dir>` if .vscode/launch.json is missing.
 * Runs silently in the background on activation so F5 works immediately. */
async function autoSetupVscode(output) {
	const blyt = findBlytSilent();
	if (!blyt) return; // SDK not yet configured — nothing to do

	const folders = vscode.workspace.workspaceFolders ?? [];
	for (const folder of folders) {
		const projectDir = findCartProject(folder.uri.fsPath);
		if (!projectDir) continue; // not a blyt project at all

		const launchJson = path.join(projectDir, '.vscode', 'launch.json');
		if (fs.existsSync(launchJson)) continue;

		output.appendLine(`[setup] blyt setup vscode ${projectDir}`);
		await new Promise((resolve) => {
			const proc = cp.spawn(blyt, ['setup', 'vscode', projectDir], {
				stdio: ['ignore', 'pipe', 'pipe'],
			});
			proc.stdout.on('data', (d) => output.append(d.toString()));
			proc.stderr.on('data', (d) => output.append(d.toString()));
			proc.on('error', (e) => {
				output.appendLine(`[setup] error: ${e.message}`);
				resolve();
			});
			proc.on('exit', () => resolve());
		});
	}
}

/* ── Native (RISC-V guest) DAP proxy for lldb-dap ────────────────────────── */

/* Wraps lldb-dap as a child process and proxies DAP messages, with two
 * workarounds for LLDB's conditional-breakpoint update bug:
 *
 *  1. true/false normalization — LLDB's C expression evaluator requires
 *     stdbool.h for these names.  Replace bare `true`/`false` with `1`/`0`.
 *
 *  2. Clear-then-add on setBreakpoints — When the program is stopped at a
 *     breakpoint and the user edits the condition, LLDB's stop-reason holds a
 *     stale reference to the old BP object.  If we just update the condition
 *     in-place, LLDB re-evaluates the OLD condition on the next continue.
 *     Sending an empty setBreakpoints first forces LLDB to delete the old BP
 *     object entirely; the subsequent setBreakpoints creates a fresh one,
 *     clearing the stale stop-reason reference. */
class BlytGdbDapProxy {
	constructor(lldbDapBin) {
		this._lldbSeq = 0;
		this._vsSeq = 0;
		this._pending = new Map();
		this._buf = Buffer.alloc(0);

		/* VS Code's DebugAdapterInlineImplementation requires the DebugAdapter
		 * interface: onDidSendMessage must be a vscode.Event<T>, not a plain
		 * assignable property.  Use EventEmitter and expose its .event. */
		this._emitter = new vscode.EventEmitter();
		this.onDidSendMessage = this._emitter.event;

		this._proc = cp.spawn(lldbDapBin, [], {
			stdio: ['pipe', 'pipe', 'pipe'],
		});
		this._proc.stdout.on('data', (chunk) => {
			this._buf = Buffer.concat([this._buf, chunk]);
			this._drain();
		});
		this._proc.stderr.on('data', () => {});
	}

	_drain() {
		while (true) {
			let i = 0;
			for (; i + 3 < this._buf.length; i++) {
				if (
					this._buf[i] === 0x0d &&
					this._buf[i + 1] === 0x0a &&
					this._buf[i + 2] === 0x0d &&
					this._buf[i + 3] === 0x0a
				)
					break;
			}
			if (i + 3 >= this._buf.length) return;
			const hdr = this._buf.slice(0, i).toString('utf8');
			const m = hdr.match(/Content-Length:\s*(\d+)/i);
			if (!m) {
				this._buf = this._buf.slice(i + 4);
				continue;
			}
			const len = parseInt(m[1], 10);
			if (this._buf.length < i + 4 + len) return;
			const body = this._buf.slice(i + 4, i + 4 + len).toString('utf8');
			this._buf = this._buf.slice(i + 4 + len);
			let msg;
			try {
				msg = JSON.parse(body);
			} catch {
				continue;
			}
			if (msg.type === 'response') {
				const p = this._pending.get(msg.request_seq);
				if (p) {
					this._pending.delete(msg.request_seq);
					p(msg);
				}
			} else {
				this._emitter.fire(msg);
			}
		}
	}

	_ask(cmd, args) {
		const seq = ++this._lldbSeq;
		return new Promise((resolve) => {
			this._pending.set(seq, resolve);
			const json = JSON.stringify({
				seq,
				type: 'request',
				command: cmd,
				arguments: args || {},
			});
			this._proc.stdin.write(
				`Content-Length: ${Buffer.byteLength(json, 'utf8')}\r\n\r\n${json}`,
			);
		});
	}

	handleMessage(msg) {
		if (msg.type !== 'request') return;
		if (msg.command === 'setBreakpoints') {
			this._setBreakpoints(msg).catch(() => {});
		} else {
			const vsSeq = msg.seq;
			const seq = ++this._lldbSeq;
			this._pending.set(seq, (resp) =>
				this._emitter.fire({
					...resp,
					seq: ++this._vsSeq,
					request_seq: vsSeq,
				}),
			);
			const json = JSON.stringify({ ...msg, seq });
			this._proc.stdin.write(
				`Content-Length: ${Buffer.byteLength(json, 'utf8')}\r\n\r\n${json}`,
			);
		}
	}

	async _setBreakpoints(vsMsg) {
		const vsSeq = vsMsg.seq;
		const source = vsMsg.arguments?.source ?? {};

		/* Lua breakpoints are handled exclusively by the companion Lua DAP
		 * session (started via vscode.debug.startDebugging for hybrid WASM
		 * carts).  lldb-dap has no Lua runtime knowledge and always returns
		 * "unverified" for .lua files, which causes VS Code to display them
		 * with a yellow (unverified) gutter icon even though the companion
		 * session verified them — making newly set Lua BPs appear ignored.
		 * Short-circuit here: report all Lua BPs as verified so VS Code
		 * shows the correct red gutter icon, without forwarding to lldb. */
		const srcPath = source.path ?? source.name ?? '';
		if (srcPath.endsWith('.lua')) {
			const bps = (vsMsg.arguments?.breakpoints ?? []).map((bp) => ({
				id: bp.line,
				verified: true,
				line: bp.line,
			}));
			this._emitter.fire({
				type: 'response',
				command: 'setBreakpoints',
				success: true,
				request_seq: vsSeq,
				seq: ++this._vsSeq,
				body: { breakpoints: bps },
			});
			return;
		}

		/* Normalize bare true/false to 1/0. */
		const normBps = (vsMsg.arguments?.breakpoints ?? []).map((bp) => {
			if (typeof bp.condition !== 'string') return bp;
			const cond = bp.condition
				.replace(/\btrue\b/g, '1')
				.replace(/\bfalse\b/g, '0');
			return cond !== bp.condition ? { ...bp, condition: cond } : bp;
		});

		/* Phase 1: clear all BPs for this source. */
		await this._ask('setBreakpoints', {
			source,
			breakpoints: [],
			lines: [],
		});

		/* Phase 2: add with normalised conditions. */
		const resp = await this._ask('setBreakpoints', {
			...vsMsg.arguments,
			breakpoints: normBps,
		});

		this._emitter.fire({ ...resp, seq: ++this._vsSeq, request_seq: vsSeq });
	}

	dispose() {
		try {
			this._proc.kill();
		} catch (_) {}
	}
}

/* ── Extension entry point ────────────────────────────────────────────────── */

function activate(context) {
	const output = vscode.window.createOutputChannel('Blyt');

	/* Open the cart game panel, or navigate the existing one to a new URL.
	 *
	 * First launch: creates a custom WebviewPanel in ViewColumn.Two (right of
	 * the main editor area) with preserveFocus so the editor stays active.
	 * Subsequent runs: sends a postMessage to the existing panel's iframe so it
	 * navigates without rebuilding the WebviewPanel.  If the user closed the
	 * panel it will be recreated at the same view column.
	 *
	 * Always awaited by callers. */
	async function openCartPage(url) {
		output.appendLine(`[browser] openCartPage ${url}`);
		if (g_gamePanel) {
			output.appendLine('[browser] navigating existing panel');
			g_gamePanel.webview.postMessage({ type: 'blyt-navigate', url });
			g_gamePanel.reveal(
				g_gamePanel.viewColumn ?? vscode.ViewColumn.Two,
				/* preserveFocus */ true,
			);
			return;
		}
		output.appendLine('[browser] creating new panel');
		g_gamePanel = vscode.window.createWebviewPanel(
			'blyt.game',
			'blyt',
			{ viewColumn: vscode.ViewColumn.Two, preserveFocus: true },
			{ enableScripts: true, retainContextWhenHidden: true },
		);
		g_gamePanel.webview.html = makeGameHtml(url);
		g_gamePanel.onDidDispose(() => {
			output.appendLine('[browser] panel disposed');
			g_gamePanel = null;
		});
	}

	autoSetupVscode(output);

	/* ── blyt: unified cart debugger ────────────────────────────────────── */

	/* A single debug type serves every cart.  `blyt debug` always opens
	 * both a Lua DAP relay and a GDB relay, so one launch exposes both
	 * backends; we choose which to connect by cart type:
	 *   Lua cart    → VS Code's DAP client connects to the Lua DAP TCP port.
	 *   native cart → lldb-dap connects to the GDB relay (RISC-V guest).
	 * Ctrl+F5 (config.noDebug) runs the cart with no relay at all.
	 * Host-engine debugging is out of scope here (see the repo .vscode/). */

	/* Resolve { cart, cwd, isLua, isHybrid } from a launch config, falling back
	 * to project detection.  Returns null (after an error) if no cart is found. */
	function resolveTarget(folder, config) {
		let cart = config.cart;
		let cwd = folder?.uri.fsPath;
		let projectDir = null;
		if (!cart) {
			const found = detectAnyCart(folder);
			if (!found) {
				vscode.window.showErrorMessage(
					'Blyt: open a file inside a blyt cart project, or add "cart" to your launch configuration.',
				);
				return null;
			}
			cart = found.cart;
			cwd = found.projectDir;
			projectDir = found.projectDir;
		} else {
			projectDir = findCartProject(cart);
		}
		cwd = cwd ?? path.dirname(cart);
		return {
			cart,
			cwd,
			isLua: projectDir ? isLuaCart(projectDir) : false,
			isHybrid: projectDir ? isHybridCart(projectDir) : false,
		};
	}

	/* Build the cart, surfacing failures as an error notification.  Returns
	 * false on failure.  `debug` forces a --debug (DWARF) rebuild. */
	async function build(cwd, debug) {
		output.appendLine(
			debug ? `\n── blyt build --debug` : `\n── blyt build`,
		);
		try {
			await buildCart(cwd, output, debug);
			return true;
		} catch (e) {
			vscode.window.showErrorMessage(`Blyt: ${e.message}`);
			return false;
		}
	}

	/* Stash a freshly-started blyt process for the upcoming session and open
	 * the cart game panel if it announced an HTTP port.  Returns the temp ID
	 * to put in the resolved config under _blytTempId. */
	async function trackProc(proc, httpPort) {
		const tempId = nextId++;
		pendingProcs.set(tempId, proc);
		if (httpPort) {
			const cartUrl = `http://127.0.0.1:${httpPort}/`;
			pendingUrls.set(tempId, cartUrl);
			await openCartPage(cartUrl);
		}
		return tempId;
	}

	/* Offer a single "Debug" config when the workspace is a blyt cart project.
	 * Registered for both Initial (F5 with no launch.json) and Dynamic (Add
	 * Configuration button). */
	function provideDebugConfigurations(folder) {
		const found = detectAnyCart(folder);
		if (!found) return [];
		return [
			{
				type: 'blyt',
				request: 'launch',
				name: 'Debug',
				cart: found.cart,
			},
		];
	}

	function resolveDebugConfiguration(_folder, config) {
		return { type: 'blyt', request: 'launch', name: 'Debug', ...config };
	}

	/* Pick the adapter for the resolved session by the mode the resolver chose. */
	context.subscriptions.push(
		vscode.debug.registerDebugAdapterDescriptorFactory('blyt', {
			createDebugAdapterDescriptor(session) {
				const cfg = session.configuration;
				const proc = pendingProcs.get(cfg._blytTempId);
				pendingProcs.delete(cfg._blytTempId);
				const url = pendingUrls.get(cfg._blytTempId);
				pendingUrls.delete(cfg._blytTempId);
				if (proc) sessionProcs.set(session.id, proc);
				if (url) sessionUrls.set(session.id, url);
				if (cfg._blytMode === 'lua')
					return new vscode.DebugAdapterServer(
						cfg._blytDapPort,
						'127.0.0.1',
					);
				if (cfg._blytMode === 'native')
					return new vscode.DebugAdapterInlineImplementation(
						new BlytGdbDapProxy(findLldbDap()),
					);
				/* 'run' — Run Without Debugging: no relay, just a process wrapper. */
				return new vscode.DebugAdapterInlineImplementation(
					new BlytRunAdapter(proc, output),
				);
			},
		}),
	);

	context.subscriptions.push(
		vscode.debug.registerDebugConfigurationProvider('blyt', {
			provideDebugConfigurations,
			resolveDebugConfiguration,
		}),
	);
	context.subscriptions.push(
		vscode.debug.registerDebugConfigurationProvider(
			'blyt',
			{ provideDebugConfigurations },
			vscode.DebugConfigurationProviderTriggerKind.Dynamic,
		),
	);

	/* Start blyt (run/--debug) before the session begins and stash the ports /
	 * mode in the config so createDebugAdapterDescriptor can wire up the right
	 * adapter. */
	context.subscriptions.push(
		vscode.debug.registerDebugConfigurationProvider('blyt', {
			resolveDebugConfiguration,
			async resolveDebugConfigurationWithSubstitutedVariables(
				folder,
				config,
			) {
				/* Pre-resolved configs (e.g. the Lua DAP companion in a hybrid
				 * compound launch) are fully formed — skip all setup. */
				if (config._blytPreresolved) return config;

				output.show(true);
				const target = resolveTarget(folder, config);
				if (!target) return undefined;
				const { cart, cwd, isLua, isHybrid } = target;

				/* Build-only modes: compile the cart, then cancel the launch. */
				if (config.mode === 'build' || config.mode === 'build-debug') {
					const isDbg = config.mode === 'build-debug';
					if (await build(cwd, isDbg)) {
						vscode.window.showInformationMessage(
							`blyt build${isDbg ? ' --debug' : ''} succeeded`,
						);
					}
					return undefined;
				}

				/* Native mode: blytplay (Ctrl+F5) or blytdebug (F5) in an SDL2 window.
				 * Mirrors the WASM path but spawns native binaries directly — no HTTP
				 * server, no webview panel, no WebSocket relay. */
				if (config.mode === 'native') {
					if (config.noDebug) {
						/* Ctrl+F5: run with blytplay (release, no debugger). */
						if (!fs.existsSync(cart) && !(await build(cwd, false)))
							return undefined;
						const blytplay = findSdkBin('blytplay');
						if (!blytplay) return undefined;
						output.appendLine(`\n── blytplay ${cart}`);
						const proc = cp.spawn(blytplay, [cart], {
							cwd,
							stdio: ['ignore', 'pipe', 'pipe'],
						});
						proc.stdout.on('data', (d) =>
							output.append(d.toString()),
						);
						proc.stderr.on('data', (d) =>
							output.append(d.toString()),
						);
						proc.on('error', (err) =>
							vscode.window.showErrorMessage(
								`Blyt: ${err.message}`,
							),
						);
						const tempId = await trackProc(proc, null);
						return {
							...config,
							cart,
							_blytMode: 'run',
							_blytTempId: tempId,
						};
					}

					/* F5: run with blytdebug and connect the IDE debugger. */
					if (!(await build(cwd, true))) return undefined;
					const debugCart = cart.replace(/\.blyt$/, '.dbg.blyt');

					/* Hybrid cart: spawn blytdebug with both --debug and --gdb, then
					 * start a companion Lua DAP session programmatically.  The GDB
					 * (native) session owns the process; the Lua session holds no proc
					 * reference so terminating Lua alone leaves native running. */
					if (isHybrid) {
						output.appendLine(
							`\n── blytdebug --debug 0 --gdb 0 ${debugCart}`,
						);
						let hybridResult;
						try {
							hybridResult = await startHybridNativeDebug(
								debugCart,
								cwd,
								output,
							);
						} catch (e) {
							vscode.window.showErrorMessage(
								`Blyt: ${e.message}`,
							);
							return undefined;
						}
						const {
							proc: hybridProc,
							dapPort: hybridDap,
							gdbPort: hybridGdb,
						} = hybridResult;
						const hybridTempId = nextId++;
						pendingProcs.set(hybridTempId, hybridProc);
						/* Fire and forget: VS Code resolves the Lua session in parallel.
						 * _blytPreresolved skips re-detection; no _blytTempId so the
						 * companion session does not take ownership of the process. */
						vscode.debug.startDebugging(folder, {
							type: 'blyt',
							request: 'launch',
							name: 'Lua (blyt hybrid)',
							_blytMode: 'lua',
							_blytDapPort: hybridDap,
							sourceMap: sourceMapPairs(cwd).flat(),
							_blytPreresolved: true,
						});
						return {
							...config,
							_blytMode: 'native',
							program: debugCart,
							stopOnEntry: false,
							launchCommands: [
								sourceMapCommand(cwd),
								`gdb-remote 127.0.0.1:${hybridGdb}`,
							],
							_blytTempId: hybridTempId,
						};
					}

					output.appendLine(
						`\n── blytdebug ${isLua ? '--debug' : '--gdb'} 0 ${debugCart}`,
					);
					let nativeResult;
					try {
						nativeResult = await startNativeDebug(
							debugCart,
							cwd,
							output,
							isLua,
						);
					} catch (e) {
						vscode.window.showErrorMessage(`Blyt: ${e.message}`);
						return undefined;
					}
					const { proc: nativeProc, gdbPort, dapPort } = nativeResult;

					if (isLua) {
						const tempId = await trackProc(nativeProc, null);
						return {
							...config,
							cart,
							_blytMode: 'lua',
							_blytTempId: tempId,
							_blytDapPort: dapPort,
							sourceMap: sourceMapPairs(cwd).flat(),
						};
					}

					/* Native cart: lldb-dap connects directly to the GDB RSP port.
					 * No WebSocket relay or "wait for WASM ready" step needed. */
					const nativeTempId = nextId++;
					pendingProcs.set(nativeTempId, nativeProc);
					return {
						...config,
						_blytMode: 'native',
						program: debugCart,
						stopOnEntry: false,
						launchCommands: [
							sourceMapCommand(cwd),
							`gdb-remote 127.0.0.1:${gdbPort}`,
						],
						_blytTempId: nativeTempId,
					};
				}

				/* Run Without Debugging (Ctrl+F5): plain `blyt run`, no relay. */
				if (config.noDebug) {
					if (!fs.existsSync(cart) && !(await build(cwd, false)))
						return undefined;
					output.appendLine(`\n── blyt run ${cart}`);
					let result;
					try {
						result = await startBlytRunSimple(cart, cwd, output);
					} catch (e) {
						vscode.window.showErrorMessage(`Blyt: ${e.message}`);
						return undefined;
					}
					const tempId = await trackProc(
						result.proc,
						result.httpPort,
					);
					return {
						...config,
						cart,
						_blytMode: 'run',
						_blytTempId: tempId,
					};
				}

				if (!(await build(cwd, true))) return undefined;
				const debugCart = cart.replace(/\.blyt$/, '.dbg.blyt');

				output.appendLine(`\n── blyt debug ${debugCart}`);
				let result;
				try {
					result = await startBlytRun(debugCart, cwd, output);
				} catch (e) {
					vscode.window.showErrorMessage(`Blyt: ${e.message}`);
					return undefined;
				}
				const { proc, httpPort, dapPort, gdbPort } = result;

				/* Lua cart: connect VS Code's DAP client straight to the relay. */
				if (isLua) {
					const tempId = await trackProc(proc, httpPort);
					return {
						...config,
						cart,
						_blytMode: 'lua',
						_blytTempId: tempId,
						_blytDapPort: dapPort,
						sourceMap: sourceMapPairs(cwd).flat(),
					};
				}

				/* Native cart: lldb-dap connects to the GDB relay (RISC-V guest). */
				const cartUrl = httpPort
					? `http://127.0.0.1:${httpPort}/`
					: null;
				if (!gdbPort) {
					proc.kill();
					vscode.window.showErrorMessage(
						'Blyt: blyt debug did not announce a GDB port. ' +
							'Ensure the debug WASM build includes BLYT_GDB=ON.',
					);
					return undefined;
				}

				/* Open the browser so the WASM page loads and the GDB WebSocket
				 * relay becomes ready.  We must wait for the relay to be ready
				 * before telling lldb-dap to connect — otherwise LLDB's 6-second
				 * handshake timeout fires and the session is broken.
				 *
				 * The relay prints "GDB: WASM ready" (stdout) once the WASM page's
				 * WebSocket connects.  We await that signal here. */
				let wasmActuallyConnected = false;
				const wasmReady = new Promise((resolve) => {
					let done = false;
					function onData(chunk) {
						if (
							!done &&
							chunk.toString().includes('GDB: WASM ready')
						) {
							done = true;
							wasmActuallyConnected = true;
							proc.stdout.removeListener('data', onData);
							resolve();
						}
					}
					proc.stdout.on('data', onData);
					/* Safety timeout: proceed after 30 s even if signal never arrives. */
					setTimeout(() => {
						if (!done) {
							done = true;
							proc.stdout.removeListener('data', onData);
							resolve();
						}
					}, 30000);
				});

				if (cartUrl) {
					await openCartPage(cartUrl);
				}

				output.appendLine(
					`── GDB RSP relay on 127.0.0.1:${gdbPort} — waiting for WASM to load…`,
				);
				await wasmReady;
				if (wasmActuallyConnected) {
					output.appendLine(
						`── WASM connected — lldb-dap will connect to GDB relay`,
					);
				} else {
					output.appendLine(
						`── WASM did not connect within 30 s — proceeding anyway`,
					);
				}

				/* Hybrid cart (Lua+C) in WASM: start a companion Lua DAP session
				 * so Lua breakpoints work.  We do this AFTER wasmReady so the
				 * WASM's DAP WebSocket is already connected to the relay — when
				 * VS Code connects TCP, both relay sides are immediately available
				 * and configurationDone reaches the WASM without any race window.
				 * Falls through to the lldb-dap setup below for the native side. */
				if (isHybrid && dapPort) {
					vscode.debug.startDebugging(folder, {
						type: 'blyt',
						request: 'launch',
						name: 'Lua (blyt hybrid)',
						_blytMode: 'lua',
						_blytDapPort: dapPort,
						sourceMap: sourceMapPairs(cwd).flat(),
						_blytPreresolved: true,
					});
				}

				const tempId = nextId++;
				pendingProcs.set(tempId, proc);
				if (cartUrl) pendingUrls.set(tempId, cartUrl);

				/* lldb-dap launch config:
				 *   program        — cart ELF; LLDB auto-detects riscv32 and loads symbols
				 *   launchCommands — replaces `process launch` with a remote gdb-remote
				 *                    connection; prevents lldb-dap from trying to exec the
				 *                    RISC-V binary locally (which fails on macOS/x86).
				 *
				 * `blyt build` stores canonical (machine-independent) /blyt/*
				 * paths in the DWARF (issue #46).  sourceMapCommand() builds the
				 * reverse mapping (cart → workspace, plus the SDK/toolchain trees
				 * from build/source-map.json) so lldb resolves source-line
				 * breakpoints and frames back to the files on disk. */
				return {
					...config,
					_blytMode: 'native',
					program: debugCart,
					stopOnEntry: false,
					launchCommands: [
						sourceMapCommand(cwd),
						`gdb-remote 127.0.0.1:${gdbPort}`,
					],
					_blytTempId: tempId,
				};
			},
		}),
	);

	/* ── Session cleanup ────────────────────────────────────────────────── */

	/* DAP message tracer for the unified `blyt` type.  Handles, for whichever
	 * backend is active:
	 *   - delayed process kill on disconnect/terminate (gives lldb-dap time to
	 *     finish its GDB handshake),
	 *   - WASM page reload on restart (native lldb-dap GDB reconnect flow),
	 *   - setBreakpoints / lldb output logging,
	 *   - revealing the source line on stop (the game panel can cover it). */
	context.subscriptions.push(
		vscode.debug.registerDebugAdapterTrackerFactory('blyt', {
			createDebugAdapterTracker(session) {
				let pendingReveal = false;
				/* The Lua DAP relay localises canonical /blyt/cart paths to the
				 * workspace in stackTrace/loadedSources using the launch sourceMap
				 * (issue #51), so call-stack-click navigation works for every
				 * frame.  localizeCartPath stays here only as a defensive fallback
				 * for the stop-reveal in case a frame ever arrives un-localised. */
				const luaCwd =
					session.configuration?._blytMode === 'lua'
						? session.workspaceFolder?.uri.fsPath || ''
						: '';
				return {
					onWillReceiveMessage(msg) {
						const tag = msg.command || msg.event || '?';
						if (tag === 'disconnect' || tag === 'terminate') {
							const proc = sessionProcs.get(session.id);
							if (proc) {
								sessionProcs.delete(session.id);
								/* 3 s window for lldb-dap to finish GDB handshake. */
								setTimeout(() => {
									try {
										proc.kill();
									} catch (_) {}
									output.appendLine(
										'── blyt process stopped',
									);
								}, 3000);
							}
						} else if (tag === 'restart') {
							/* LLDB restart flow:
							 *   1. lldb-dap sends k → WASM exits, closes its WebSocket
							 *   2. The relay loop restarts, waiting for a new WS connection
							 *   3. lldb-dap re-runs launchCommands: gdb-remote PORT
							 *   4. LLDB's TCP connects to the relay (buffered in relay's channel)
							 *   5. We reload the WASM iframe → blytplay.js restarts,
							 *      opens a new WebSocket to the relay
							 *   6. Relay pairs the new WS with the buffered TCP → session resumes
							 *
							 * 500 ms gives LLDB time to send k and the WASM to close before we
							 * reload; LLDB's 6 s reconnect timeout gives the page plenty of time
							 * to load and reconnect. */
							const url = sessionUrls.get(session.id);
							if (url) {
								setTimeout(() => {
									output.appendLine(
										'── restart: reloading WASM page for GDB reconnect',
									);
									openCartPage(url);
								}, 500);
							}
						}
					},
					onDidSendMessage(msg) {
						if (
							msg.type === 'response' &&
							msg.command === 'setBreakpoints'
						) {
							const bps = (msg.body?.breakpoints ?? [])
								.map((b) =>
									b.verified
										? `✓${b.id}`
										: `✗${b.id}(${b.message ?? 'unverified'})`,
								)
								.join(' ');
							if (bps)
								output.appendLine(`setBreakpoints: ${bps}`);
						} else if (msg.event === 'output') {
							/* lldb error/warning messages */
							const cat = msg.body?.category ?? '';
							const text = (msg.body?.output ?? '').trimEnd();
							if (text && (cat === 'stderr' || cat === 'console'))
								output.appendLine(
									`[lldb] ${text.slice(0, 200)}`,
								);
						}
						if (msg.type === 'event' && msg.event === 'stopped') {
							pendingReveal = true;
						}
						/* When VS Code receives the stackTrace after a stop, the game
						 * panel may be covering the source editor.  Explicitly reveal
						 * the source so the execution line is visible without needing
						 * the user to click away from the game panel first. */
						if (
							pendingReveal &&
							msg.type === 'response' &&
							msg.command === 'stackTrace'
						) {
							pendingReveal = false;
							const frame = msg.body?.stackFrames?.[0];
							if (frame?.source?.path && frame.line) {
								const srcPath = luaCwd
									? localizeCartPath(
											frame.source.path,
											luaCwd,
										)
									: frame.source.path;
								const uri = vscode.Uri.file(srcPath);
								const line = frame.line - 1;
								vscode.window.showTextDocument(uri, {
									preserveFocus: false,
									selection: new vscode.Range(
										line,
										0,
										line,
										0,
									),
								});
							}
						}
					},
				};
			},
		}),
	);

	/* Kill blyt run / blytplay when the debug session ends.
	 * The game panel (g_gamePanel) is intentionally left open so the user
	 * keeps their layout; subsequent runs navigate it to the new URL. */
	context.subscriptions.push(
		vscode.debug.onDidTerminateDebugSession((session) => {
			/* proc may already be gone (killed by tracker above); that is fine. */
			const proc = sessionProcs.get(session.id);
			if (proc) {
				proc.kill();
				sessionProcs.delete(session.id);
				output.appendLine('── blyt process stopped');
			}
			sessionUrls.delete(session.id);
		}),
	);

	/* ── Build task provider ─────────────────────────────────────────────── */

	/* Provide a "blyt: Build" task for every workspace folder that looks like
	 * a blyt cart project (contains blyt.build.yaml). */
	context.subscriptions.push(
		vscode.tasks.registerTaskProvider('blyt', {
			provideTasks() {
				const folders = vscode.workspace.workspaceFolders ?? [];
				return folders
					.filter((f) =>
						fs.existsSync(
							path.join(f.uri.fsPath, 'blyt.info.yaml'),
						),
					)
					.map(
						(folder) =>
							new vscode.Task(
								{ type: 'blyt', command: 'build' },
								folder,
								'Build',
								'blyt',
								new vscode.ShellExecution('blyt build', {
									cwd: folder.uri.fsPath,
								}),
							),
					);
			},
			resolveTask: () => undefined,
		}),
	);
}

function deactivate() {
	for (const p of pendingProcs.values()) p.kill();
	for (const p of sessionProcs.values()) p.kill();
	if (g_gamePanel) {
		g_gamePanel.dispose();
		g_gamePanel = null;
	}
}

module.exports = { activate, deactivate };

/* Pure cart-detection helpers exported for unit testing (see test/). These
 * have no VS Code dependency; test/cart-detection.test.js stubs the `vscode`
 * module so this file can be required under plain node. */
module.exports._test = {
	findCartProject,
	readBuildManifest,
	cartLanguages,
	isLuaCart,
	isHybridCart,
	cartId,
	detectAnyCart,
	BlytGdbDapProxy,
};
