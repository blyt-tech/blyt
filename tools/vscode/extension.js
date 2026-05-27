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
 *                 assigns a session ID; cleaned up on session termination.  */
const pendingProcs = new Map();
const sessionProcs = new Map();
let   nextId = 0;

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

/* Returns the path to the blytplay binary, or null (silently). */
function findBlytplaySilent() {
    const sdk = sdkDir();
    if (!sdk) return null;
    return path.join(sdk, 'bin', 'blytplay');
}

/* Returns the path to the blytplay binary, or null after showing an error. */
function findBlytplay() {
    const blytplay = findBlytplaySilent();
    if (!blytplay) {
        vscode.window.showErrorMessage(
            'Blyt: set blyt.sdkDir in VS Code settings (or BLYT_SDK_DIR env var) to the blyt SDK directory.'
        );
    }
    return blytplay;
}

/* ── Start blyt build ────────────────────────────────────────────────────── */

function buildCart(cwd, output) {
    const blyt = findBlyt();
    if (!blyt) return Promise.reject(new Error('blyt SDK not configured'));
    return new Promise((resolve, reject) => {
        const proc = cp.spawn(blyt, ['build', cwd], {
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

/* ── Start blytplay --gdb ─────────────────────────────────────────────────── */

/* Spawns `blytplay --gdb 0 --headless <cartPath>` and resolves once the
 * process prints the GDB port ("blyt: GDB listening on port N").
 * All stdout/stderr is forwarded to `output`. */
function startBlytplayGdb(cartPath, cwd, output) {
    const blytplay = findBlytplay();
    if (!blytplay) return Promise.reject(new Error('blyt SDK not configured'));

    const sdk = sdkDir();
    const libDir = sdk ? path.join(sdk, 'lib') : undefined;

    return new Promise((resolve, reject) => {
        const env = { ...process.env };
        if (libDir) env.BLYT_LIB_DIR = libDir;

        const proc = cp.spawn(blytplay, ['--gdb', '0', '--headless', cartPath], {
            cwd,
            env,
            stdio: ['ignore', 'pipe', 'pipe'],
        });

        let buf      = '';
        let resolved = false;

        function check(data) {
            buf += data.toString();
            output.append(data.toString());
            const m = buf.match(/GDB listening on port (\d+)/);
            if (m && !resolved) {
                resolved = true;
                resolve({ proc, gdbPort: parseInt(m[1], 10) });
            }
        }

        proc.stdout.on('data', check);
        proc.stderr.on('data', chunk => { check(chunk); });

        proc.on('error', err => {
            if (!resolved)
                reject(new Error(
                    `Could not start blytplay: ${err.message}\n` +
                    'Set blyt.sdkDir in VS Code settings, or set BLYT_SDK_DIR.'
                ));
        });

        proc.on('exit', code => {
            if (!resolved)
                reject(new Error(`blytplay exited (code ${code}) before the GDB port was announced`));
        });

        setTimeout(() => {
            if (!resolved) {
                proc.kill();
                reject(new Error('blytplay --gdb did not print a GDB port within 15 s'));
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

    autoSetupVscode(output);

    /* ── blyt-lua: Lua DAP debugger ─────────────────────────────────────── */

    /* Connect VS Code's built-in DAP client to the blyt relay TCP port. */
    context.subscriptions.push(
        vscode.debug.registerDebugAdapterDescriptorFactory('blyt-lua', {
            createDebugAdapterDescriptor(session) {
                const { _blytTempId, _blytDapPort } = session.configuration;
                const proc = pendingProcs.get(_blytTempId);
                pendingProcs.delete(_blytTempId);
                if (proc) sessionProcs.set(session.id, proc);
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

                if (httpPort) {
                    vscode.env.openExternal(
                        vscode.Uri.parse(`http://127.0.0.1:${httpPort}/`)
                    );
                }

                const tempId = nextId++;
                pendingProcs.set(tempId, proc);
                return { ...config, cart, _blytTempId: tempId, _blytDapPort: dapPort };
            },
        })
    );

    /* ── blyt-native-gdb: native RISC-V GDB debugger ───────────────────── */

    /* Uses GDB's built-in DAP mode (gdb-multiarch --interpreter=dap).
     * The extension starts blytplay --gdb 0, captures the GDB RSP port, and
     * injects initCommands into the config so GDB connects automatically. */

    context.subscriptions.push(
        vscode.debug.registerDebugAdapterDescriptorFactory('blyt-native-gdb', {
            createDebugAdapterDescriptor(session) {
                const { _blytGdbTempId } = session.configuration;
                const proc = pendingProcs.get(_blytGdbTempId);
                pendingProcs.delete(_blytGdbTempId);
                if (proc) sessionProcs.set(session.id, proc);
                return new vscode.DebugAdapterExecutable('gdb-multiarch', ['--interpreter=dap']);
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

                if (!fs.existsSync(cart)) {
                    output.appendLine(`\n── blyt build`);
                    try {
                        await buildCart(cwd, output);
                    } catch (e) {
                        vscode.window.showErrorMessage(`Blyt: ${e.message}`);
                        return undefined;
                    }
                }

                output.appendLine(`\n── blytplay --gdb 0 --headless ${cart}`);

                let result;
                try {
                    result = await startBlytplayGdb(cart, cwd, output);
                } catch (e) {
                    vscode.window.showErrorMessage(`Blyt: ${e.message}`);
                    return undefined;
                }

                const { proc, gdbPort } = result;
                output.appendLine(`── GDB RSP server listening on 127.0.0.1:${gdbPort}`);
                output.appendLine(`   (gdb-multiarch --interpreter=dap will connect automatically)`);

                const tempId = nextId++;
                pendingProcs.set(tempId, proc);

                /* Build GDB initCommands that set the architecture, load debug
                 * symbols from the cart ELF, and connect to blytplay's RSP server. */
                const initCommands = [
                    'set architecture riscv:rv32',
                    `file ${cart}`,
                    `target remote 127.0.0.1:${gdbPort}`,
                ];

                return { ...config, cart, initCommands, _blytGdbTempId: tempId };
            },
        })
    );

    /* ── Session cleanup ────────────────────────────────────────────────── */

    /* Kill blyt run / blytplay when the debug session ends. */
    context.subscriptions.push(
        vscode.debug.onDidTerminateDebugSession(session => {
            const proc = sessionProcs.get(session.id);
            if (proc) {
                proc.kill();
                sessionProcs.delete(session.id);
                output.appendLine('── blyt process stopped');
            }
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
}

module.exports = { activate, deactivate };
