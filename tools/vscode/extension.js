'use strict';

const vscode = require('vscode');
const cp     = require('child_process');
const path   = require('path');
const fs     = require('fs');

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
const pendingUrls  = new Map();
const sessionUrls  = new Map();
let   nextId = 0;
let   g_gamePanel = null; /* custom WebviewPanel for the cart, or null */

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
    return (vscode.workspace.getConfiguration('blyt').get('sdkDir', '').trim()
        || process.env.BLYT_SDK_DIR || '').trim();
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
            'Blyt: set blyt.sdkDir in VS Code settings (or BLYT_SDK_DIR env var) to the blyt SDK directory.'
        );
    }
    return blyt;
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

/* ── Inline debug adapter for blyt-run (no debug ports) ─────────────────── */

/* Minimal DAP adapter: accepts the VS Code handshake, manages the process
 * lifecycle, and sends `terminated` when the process exits.  No breakpoints,
 * no stack frames — this is purely a process wrapper so VS Code's stop button
 * kills the cart server. */
class BlytRunAdapter {
    constructor(proc, output) {
        this._proc  = proc;
        this._output = output;
        this._seq   = 0;
        this.sendMessage = null; /* set by VS Code's DebugAdapterInlineImplementation */
        proc.on('exit', () => {
            if (this.sendMessage)
                this.sendMessage({ seq: ++this._seq, type: 'event', event: 'terminated', body: {} });
        });
    }
    _respond(req, body) {
        this.sendMessage({
            seq: ++this._seq, type: 'response',
            request_seq: req.seq, success: true,
            command: req.command, body: body || {},
        });
    }
    onError(err) { this._output.appendLine(`[run] adapter error: ${err.message}`); }
    onClose() {}
    handleMessage(msg) {
        if (msg.type !== 'request') return;
        switch (msg.command) {
            case 'initialize':
                this._respond(msg, {});
                this.sendMessage({ seq: ++this._seq, type: 'event', event: 'initialized', body: {} });
                break;
            case 'launch':
            case 'configurationDone':
                this._respond(msg, {});
                break;
            case 'setBreakpoints':
                this._respond(msg, {
                    breakpoints: (msg.arguments?.breakpoints ?? []).map(() => ({ verified: false })),
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
                try { this._proc.kill(); } catch (_) {}
                this._respond(msg, {});
                break;
            default:
                this.sendMessage({
                    seq: ++this._seq, type: 'response',
                    request_seq: msg.seq, success: false,
                    command: msg.command, message: 'not supported',
                });
        }
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
        proc.stdout.on('data', d => output.append(d.toString()));
        proc.stderr.on('data', d => output.append(d.toString()));
        proc.on('error', err => reject(new Error(`Could not start blyt build: ${err.message}`)));
        proc.on('exit', code => {
            if (code === 0) resolve();
            else reject(new Error(`blyt build failed (exit code ${code})`));
        });
    });
}

/* ── Start blyt run --debug ───────────────────────────────────────────────── */

/* Spawns `blyt run --debug <cartPath>` and resolves once the process prints
 * both the HTTP and DAP ports.  All stdout/stderr is forwarded to `output`. */
function startBlytRun(cartPath, cwd, output) {
    const blyt = findBlyt();
    if (!blyt) return Promise.reject(new Error('blyt SDK not configured'));
    return new Promise((resolve, reject) => {
        const proc = cp.spawn(blyt, ['run', '--debug', cartPath], {
            cwd,
            stdio: ['ignore', 'pipe', 'pipe'],
        });

        let buf      = '';
        let httpPort = 0;
        let gdbPort  = 0;
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
                        `\n       (set architecture riscv:rv32, then: target remote :${gdbPort})`
                    );
                }
            }

            const m = buf.match(/DAP debugger \(Lua\):\s+127\.0\.0\.1:(\d+)/);
            if (m && !resolved) {
                resolved = true;
                resolve({ proc, httpPort, dapPort: parseInt(m[1], 10), gdbPort });
            }
        }

        proc.stdout.on('data', check);
        proc.stderr.on('data', d => output.append(d.toString()));

        proc.on('error', err => {
            if (!resolved)
                reject(new Error(
                    `Could not start blyt: ${err.message}\n` +
                    'Set blyt.sdkDir in VS Code settings, or set BLYT_SDK_DIR.'
                ));
        });

        proc.on('exit', code => {
            if (!resolved)
                reject(new Error(`blyt exited (code ${code}) before the DAP relay was ready`));
        });

        setTimeout(() => {
            if (!resolved) {
                proc.kill();
                reject(new Error('blyt run --debug did not start within 15 s'));
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
        let buf = '', resolved = false;
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
        proc.stderr.on('data', d => output.append(d.toString()));
        proc.on('error', err => {
            if (!resolved) reject(new Error(`Could not start blyt: ${err.message}`));
        });
        proc.on('exit', code => {
            if (!resolved) reject(new Error(`blyt exited (code ${code}) before serving`));
        });
        setTimeout(() => {
            if (!resolved) { proc.kill(); reject(new Error('blyt run did not start within 15 s')); }
        }, 15000);
    });
}

/* ── Cart project detection ───────────────────────────────────────────────── */

/* Walk up from startPath to find the nearest directory containing
 * blyt.info.yaml.  Returns the project directory path, or null. */
function findCartProject(startPath) {
    let dir = startPath;
    try {
        if (!fs.statSync(startPath).isDirectory()) dir = path.dirname(startPath);
    } catch { return null; }
    while (true) {
        if (fs.existsSync(path.join(dir, 'blyt.info.yaml'))) return dir;
        const parent = path.dirname(dir);
        if (parent === dir) return null;
        dir = parent;
    }
}

function isLuaCart(projectDir) {
    return fs.existsSync(path.join(projectDir, 'src', 'game', 'lua'));
}

/* Find the blyt cart project for the current context: active editor first,
 * then workspace folder.  Returns { projectDir, cart } for a Lua cart,
 * or null. */
function detectCart(folder) {
    const candidates = [];
    const activeFile = vscode.window.activeTextEditor?.document.uri.fsPath;
    if (activeFile) candidates.push(activeFile);
    if (folder) candidates.push(folder.uri.fsPath);

    for (const start of candidates) {
        const projectDir = findCartProject(start);
        if (projectDir && isLuaCart(projectDir)) {
            const name = path.basename(projectDir);
            const cart = path.join(projectDir, 'build', `${name}.blyt`);
            return { projectDir, cart };
        }
    }
    return null;
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
            const name = path.basename(projectDir);
            const cart = path.join(projectDir, 'build', `${name}.blyt`);
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
    if (!blyt) return;  // SDK not yet configured — nothing to do

    const folders = vscode.workspace.workspaceFolders ?? [];
    for (const folder of folders) {
        const projectDir = findCartProject(folder.uri.fsPath);
        if (!projectDir) continue;  // not a blyt project at all

        const launchJson = path.join(projectDir, '.vscode', 'launch.json');
        if (fs.existsSync(launchJson)) continue;

        output.appendLine(`[setup] blyt setup vscode ${projectDir}`);
        await new Promise(resolve => {
            const proc = cp.spawn(blyt, ['setup', 'vscode', projectDir], {
                stdio: ['ignore', 'pipe', 'pipe'],
            });
            proc.stdout.on('data', d => output.append(d.toString()));
            proc.stderr.on('data', d => output.append(d.toString()));
            proc.on('error', e => {
                output.appendLine(`[setup] error: ${e.message}`);
                resolve();
            });
            proc.on('exit', () => resolve());
        });
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
            g_gamePanel.reveal(g_gamePanel.viewColumn ?? vscode.ViewColumn.Two, /* preserveFocus */ true);
            return;
        }
        output.appendLine('[browser] creating new panel');
        g_gamePanel = vscode.window.createWebviewPanel(
            'blyt.game',
            'blyt',
            { viewColumn: vscode.ViewColumn.Two, preserveFocus: true },
            { enableScripts: true, retainContextWhenHidden: true }
        );
        g_gamePanel.webview.html = makeGameHtml(url);
        g_gamePanel.onDidDispose(() => {
            output.appendLine('[browser] panel disposed');
            g_gamePanel = null;
        });
    }

    autoSetupVscode(output);

    /* ── blyt-lua: Lua DAP debugger ─────────────────────────────────────── */

    /* Connect VS Code's built-in DAP client to the blyt relay TCP port. */
    context.subscriptions.push(
        vscode.debug.registerDebugAdapterDescriptorFactory('blyt-lua', {
            createDebugAdapterDescriptor(session) {
                const { _blytTempId, _blytDapPort } = session.configuration;
                const proc = pendingProcs.get(_blytTempId);
                pendingProcs.delete(_blytTempId);
                const url = pendingUrls.get(_blytTempId);
                pendingUrls.delete(_blytTempId);
                if (proc) sessionProcs.set(session.id, proc);
                if (url)  sessionUrls.set(session.id, url);
                return new vscode.DebugAdapterServer(_blytDapPort, '127.0.0.1');
            },
        })
    );

    /* Offer a "Blyt: Debug Lua Cart" config when the workspace looks like a
     * blyt Lua project.  Registered for both Initial (F5 with no launch.json)
     * and Dynamic (Add Configuration button).  Cart path mirrors blyt build's
     * output convention: <project-dir>/build/<project-dir-name>.blyt. */
    function provideDebugConfigurations(folder) {
        console.error('[blyt] provideDebugConfigurations', folder?.uri.fsPath);
        output.show(true);
        output.appendLine(`[diag] provideDebugConfigurations folder=${folder?.uri.fsPath}`);
        const found = detectCart(folder);
        output.appendLine(`[diag] detectCart => ${JSON.stringify(found)}`);
        if (!found) return [];
        return [{ type: 'blyt-lua', request: 'launch', name: 'Debug Lua', cart: found.cart }];
    }

    function resolveDebugConfiguration(folder, config) {
        console.error('[blyt] resolveDebugConfiguration', folder?.uri.fsPath, config);
        return {
            type: 'blyt-lua',
            request: 'launch',
            name: 'Debug Lua',
            ...config,
        };
    }

    context.subscriptions.push(
        vscode.debug.registerDebugConfigurationProvider('blyt-lua',
            { provideDebugConfigurations, resolveDebugConfiguration })
    );
    context.subscriptions.push(
        vscode.debug.registerDebugConfigurationProvider('blyt-lua',
            { provideDebugConfigurations },
            vscode.DebugConfigurationProviderTriggerKind.Dynamic)
    );

    /* Start blyt run --debug before the session begins; inject the DAP port
     * into the config so createDebugAdapterDescriptor can use it. */
    context.subscriptions.push(
        vscode.debug.registerDebugConfigurationProvider('blyt-lua', {
            resolveDebugConfiguration,
            async resolveDebugConfigurationWithSubstitutedVariables(folder, config) {
                console.error('[blyt] resolveDebugConfigurationWithSubstitutedVariables', folder?.uri.fsPath, config);
                output.show(true);
                output.appendLine(`[diag] resolveWithSubstitutedVars folder=${folder?.uri.fsPath} cart=${config.cart}`);
                let cart = config.cart;
                let cwd  = folder?.uri.fsPath;
                if (!cart) {
                    const found = detectCart(folder);
                    if (!found) {
                        vscode.window.showErrorMessage(
                            'Blyt: open a file inside a blyt cart project, or add "cart" to your launch configuration.'
                        );
                        return undefined;
                    }
                    cart = found.cart;
                    cwd  = found.projectDir;
                }
                cwd = cwd ?? path.dirname(cart);
                output.show(true);

                if (!fs.existsSync(cart)) {
                    output.appendLine(`\n── blyt build`);
                    try {
                        await buildCart(cwd, output);
                    } catch (e) {
                        vscode.window.showErrorMessage(`Blyt: ${e.message}`);
                        return undefined;
                    }
                }

                output.appendLine(`\n── blyt run --debug ${cart}`);

                let result;
                try {
                    result = await startBlytRun(cart, cwd, output);
                } catch (e) {
                    vscode.window.showErrorMessage(`Blyt: ${e.message}`);
                    return undefined;
                }

                const { proc, httpPort, dapPort } = result;

                const tempId = nextId++;
                pendingProcs.set(tempId, proc);
                if (httpPort) {
                    const cartUrl = `http://127.0.0.1:${httpPort}/`;
                    pendingUrls.set(tempId, cartUrl);
                    await openCartPage(cartUrl);
                }
                return { ...config, cart, _blytTempId: tempId, _blytDapPort: dapPort };
            },
        })
    );

    /* ── blyt-native-gdb: native RISC-V debugger via LLDB ──────────────── */

    /* Uses lldb-dap (the LLDB Debug Adapter Protocol server) from the same
     * Homebrew LLVM tree as blyt-clang.  The cart runs in the WASM frontend
     * via `blyt run --debug`, which starts a GDB RSP relay alongside the DAP
     * relay.  LLDB connects to that relay via `gdb-remote`. */

    context.subscriptions.push(
        vscode.debug.registerDebugAdapterDescriptorFactory('blyt-native-gdb', {
            createDebugAdapterDescriptor(session) {
                const { _blytGdbTempId } = session.configuration;
                const proc = pendingProcs.get(_blytGdbTempId);
                pendingProcs.delete(_blytGdbTempId);
                const url = pendingUrls.get(_blytGdbTempId);
                pendingUrls.delete(_blytGdbTempId);
                if (proc) sessionProcs.set(session.id, proc);
                if (url)  sessionUrls.set(session.id, url);
                return new vscode.DebugAdapterExecutable(findLldbDap());
            },
        })
    );

    function provideGdbDebugConfigurations(folder) {
        const found = detectAnyCart(folder);
        if (!found) return [];
        return [{
            type: 'blyt-native-gdb',
            request: 'launch',
            name: 'Debug Native (GDB)',
            cart: found.cart,
        }];
    }

    function resolveGdbDebugConfiguration(folder, config) {
        return {
            type: 'blyt-native-gdb',
            request: 'launch',
            name: 'Debug Native (GDB)',
            ...config,
        };
    }

    context.subscriptions.push(
        vscode.debug.registerDebugConfigurationProvider('blyt-native-gdb',
            { provideDebugConfigurations: provideGdbDebugConfigurations,
              resolveDebugConfiguration: resolveGdbDebugConfiguration })
    );
    context.subscriptions.push(
        vscode.debug.registerDebugConfigurationProvider('blyt-native-gdb',
            { provideDebugConfigurations: provideGdbDebugConfigurations },
            vscode.DebugConfigurationProviderTriggerKind.Dynamic)
    );

    context.subscriptions.push(
        vscode.debug.registerDebugConfigurationProvider('blyt-native-gdb', {
            resolveDebugConfiguration: resolveGdbDebugConfiguration,
            async resolveDebugConfigurationWithSubstitutedVariables(folder, config) {
                output.show(true);
                output.appendLine(`[diag] gdb resolveWithSubstitutedVars folder=${folder?.uri.fsPath} cart=${config.cart}`);
                let cart = config.cart;
                let cwd  = folder?.uri.fsPath;
                if (!cart) {
                    const found = detectAnyCart(folder);
                    if (!found) {
                        vscode.window.showErrorMessage(
                            'Blyt: open a file inside a blyt cart project, or add "cart" to your launch configuration.'
                        );
                        return undefined;
                    }
                    cart = found.cart;
                    cwd  = found.projectDir;
                }
                cwd = cwd ?? path.dirname(cart);

                /* Always build with --debug so the cart contains DWARF line info
                 * for source-level breakpoints.  The build is incremental (fast
                 * if sources haven't changed since the last debug build). */
                output.appendLine(`\n── blyt build --debug`);
                try {
                    await buildCart(cwd, output, true);
                } catch (e) {
                    vscode.window.showErrorMessage(`Blyt: ${e.message}`);
                    return undefined;
                }

                output.appendLine(`\n── blyt run --debug ${cart}`);

                let result;
                try {
                    result = await startBlytRun(cart, cwd, output);
                } catch (e) {
                    vscode.window.showErrorMessage(`Blyt: ${e.message}`);
                    return undefined;
                }

                const { proc, httpPort, gdbPort } = result;
                const cartUrl = httpPort ? `http://127.0.0.1:${httpPort}/` : null;

                if (!gdbPort) {
                    proc.kill();
                    vscode.window.showErrorMessage(
                        'Blyt: blyt run --debug did not announce a GDB port. ' +
                        'Ensure the WASM build includes BLYT_GDB=ON.'
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
                        if (!done && chunk.toString().includes('GDB: WASM ready')) {
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

                output.appendLine(`── GDB RSP relay on 127.0.0.1:${gdbPort} — waiting for WASM to load…`);
                await wasmReady;
                if (wasmActuallyConnected) {
                    output.appendLine(`── WASM connected — lldb-dap will connect to GDB relay`);
                } else {
                    output.appendLine(`── WASM did not connect within 30 s — proceeding anyway`);
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
                 * `blyt build --debug` uses `-ffile-prefix-map=<project_dir>=/blyt/src`
                 * to store canonical (machine-independent) paths in the DWARF.  We must
                 * tell lldb the reverse mapping so it can resolve source-line breakpoints
                 * back to the actual files on disk. */
                return {
                    ...config,
                    program: cart,
                    stopOnEntry: false,
                    launchCommands: [
                        `settings set target.source-map /blyt/src ${JSON.stringify(cwd)}`,
                        `gdb-remote 127.0.0.1:${gdbPort}`,
                    ],
                    _blytGdbTempId: tempId,
                };
            },
        })
    );

    /* ── blyt-run: run without debug ───────────────────────────────────── */

    context.subscriptions.push(
        vscode.debug.registerDebugAdapterDescriptorFactory('blyt-run', {
            createDebugAdapterDescriptor(session) {
                const { _blytRunTempId } = session.configuration;
                const proc = pendingProcs.get(_blytRunTempId);
                pendingProcs.delete(_blytRunTempId);
                const url = pendingUrls.get(_blytRunTempId);
                pendingUrls.delete(_blytRunTempId);
                if (proc) sessionProcs.set(session.id, proc);
                if (url)  sessionUrls.set(session.id, url);
                return new vscode.DebugAdapterInlineImplementation(
                    new BlytRunAdapter(proc, output)
                );
            },
        })
    );

    function provideRunConfigurations(folder) {
        const found = detectAnyCart(folder);
        if (!found) return [];
        return [{ type: 'blyt-run', request: 'launch', name: 'Run' }];
    }

    context.subscriptions.push(
        vscode.debug.registerDebugConfigurationProvider('blyt-run',
            { provideDebugConfigurations: provideRunConfigurations,
              resolveDebugConfiguration(folder, config) {
                  return { type: 'blyt-run', request: 'launch', name: 'Run', ...config };
              } })
    );
    context.subscriptions.push(
        vscode.debug.registerDebugConfigurationProvider('blyt-run',
            { provideDebugConfigurations: provideRunConfigurations },
            vscode.DebugConfigurationProviderTriggerKind.Dynamic)
    );
    context.subscriptions.push(
        vscode.debug.registerDebugConfigurationProvider('blyt-run', {
            async resolveDebugConfigurationWithSubstitutedVariables(folder, config) {
                let cart = config.cart;
                let cwd  = folder?.uri.fsPath;
                if (!cart) {
                    const found = detectAnyCart(folder);
                    if (!found) {
                        vscode.window.showErrorMessage(
                            'Blyt: open a file inside a blyt cart project, or add "cart" to your launch configuration.'
                        );
                        return undefined;
                    }
                    cart = found.cart;
                    cwd  = found.projectDir;
                }
                cwd = cwd ?? path.dirname(cart);
                output.show(true);

                if (!fs.existsSync(cart)) {
                    output.appendLine(`\n── blyt build`);
                    try {
                        await buildCart(cwd, output);
                    } catch (e) {
                        vscode.window.showErrorMessage(`Blyt: ${e.message}`);
                        return undefined;
                    }
                }

                output.appendLine(`\n── blyt run ${cart}`);
                let result;
                try {
                    result = await startBlytRunSimple(cart, cwd, output);
                } catch (e) {
                    vscode.window.showErrorMessage(`Blyt: ${e.message}`);
                    return undefined;
                }

                const { proc, httpPort } = result;
                const tempId = nextId++;
                pendingProcs.set(tempId, proc);
                if (httpPort) {
                    const cartUrl = `http://127.0.0.1:${httpPort}/`;
                    pendingUrls.set(tempId, cartUrl);
                    await openCartPage(cartUrl);
                }
                return { ...config, cart, _blytRunTempId: tempId };
            },
        })
    );

    /* ── Session cleanup ────────────────────────────────────────────────── */

    /* DAP message tracer: logs events/commands (type only) so we can see
     * whether lldb-dap emits "stopped", "initialized", etc. to VS Code.
     * Also handles the delayed kill on disconnect/terminate. */
    context.subscriptions.push(
        vscode.debug.registerDebugAdapterTrackerFactory('blyt-native-gdb', {
            createDebugAdapterTracker(session) {
                return {
                    onWillReceiveMessage(msg) {
                        const tag = msg.command || msg.event || '?';
                        if (tag === 'disconnect' || tag === 'terminate') {
                            const proc = sessionProcs.get(session.id);
                            if (proc) {
                                sessionProcs.delete(session.id);
                                /* 3 s window for lldb-dap to finish GDB handshake. */
                                setTimeout(() => {
                                    try { proc.kill(); } catch (_) {}
                                    output.appendLine('── blyt process stopped');
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
                                    output.appendLine('── restart: reloading WASM page for GDB reconnect');
                                    openCartPage(url);
                                }, 500);
                            }
                        }
                    },
                    onDidSendMessage(msg) {
                        if (msg.type === 'response' && msg.command === 'setBreakpoints') {
                            const bps = (msg.body?.breakpoints ?? []).map(b =>
                                b.verified
                                    ? `✓${b.id}`
                                    : `✗${b.id}(${b.message ?? 'unverified'})`
                            ).join(' ');
                            if (bps) output.appendLine(`setBreakpoints: ${bps}`);
                        } else if (msg.event === 'output') {
                            /* lldb error/warning messages */
                            const cat = msg.body?.category ?? '';
                            const text = (msg.body?.output ?? '').trimEnd();
                            if (text && (cat === 'stderr' || cat === 'console'))
                                output.appendLine(`[lldb] ${text.slice(0, 200)}`);
                        }
                    },
                };
            },
        })
    );

    context.subscriptions.push(
        vscode.debug.registerDebugAdapterTrackerFactory('blyt-lua', {
            createDebugAdapterTracker(_session) {
                let pendingReveal = false;
                return {
                    onDidSendMessage(msg) {
                        if (msg.type === 'response' && msg.command === 'setBreakpoints') {
                            const bps = (msg.body?.breakpoints ?? []).map(b =>
                                b.verified ? `✓${b.id}` : `✗${b.id}(${b.message ?? 'unverified'})`
                            ).join(' ');
                            if (bps) output.appendLine(`setBreakpoints: ${bps}`);
                        }
                        if (msg.type === 'event' && msg.event === 'stopped') {
                            pendingReveal = true;
                        }
                        /* When VS Code receives the stackTrace after a stop, the game
                         * panel may be covering the source editor.  Explicitly reveal
                         * the source so the execution line is visible without needing
                         * the user to click away from the game panel first. */
                        if (pendingReveal && msg.type === 'response' && msg.command === 'stackTrace') {
                            pendingReveal = false;
                            const frame = msg.body?.stackFrames?.[0];
                            if (frame?.source?.path && frame.line) {
                                const uri = vscode.Uri.file(frame.source.path);
                                const line = frame.line - 1;
                                vscode.window.showTextDocument(uri, {
                                    preserveFocus: false,
                                    selection: new vscode.Range(line, 0, line, 0),
                                });
                            }
                        }
                    },
                };
            },
        })
    );

    /* Kill blyt run / blytplay when the debug session ends.
     * The game panel (g_gamePanel) is intentionally left open so the user
     * keeps their layout; subsequent runs navigate it to the new URL. */
    context.subscriptions.push(
        vscode.debug.onDidTerminateDebugSession(session => {
            /* proc may already be gone (killed by tracker above); that is fine. */
            const proc = sessionProcs.get(session.id);
            if (proc) {
                proc.kill();
                sessionProcs.delete(session.id);
                output.appendLine('── blyt process stopped');
            }
            sessionUrls.delete(session.id);
        })
    );

    /* ── Build task provider ─────────────────────────────────────────────── */

    /* Provide a "blyt: Build" task for every workspace folder that looks like
     * a blyt cart project (contains blyt.build.yaml). */
    context.subscriptions.push(
        vscode.tasks.registerTaskProvider('blyt', {
            provideTasks() {
                const folders = vscode.workspace.workspaceFolders ?? [];
                return folders
                    .filter(f => fs.existsSync(path.join(f.uri.fsPath, 'blyt.info.yaml')))
                    .map(folder =>
                        new vscode.Task(
                            { type: 'blyt', command: 'build' },
                            folder,
                            'Build',
                            'blyt',
                            new vscode.ShellExecution('blyt build', {
                                cwd: folder.uri.fsPath,
                            })
                        )
                    );
            },
            resolveTask: () => undefined,
        })
    );
}

function deactivate() {
    for (const p of pendingProcs.values()) p.kill();
    for (const p of sessionProcs.values()) p.kill();
    if (g_gamePanel) { g_gamePanel.dispose(); g_gamePanel = null; }
}

module.exports = { activate, deactivate };
