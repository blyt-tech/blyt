/* Entry point for the headless VS Code debugger integration suite.
 *
 * Runs OUTSIDE the VS Code extension host (plain node). It:
 *   1. locates the blyt SDK (BLYT_SDK_DIR or the worktree's build/sdk),
 *   2. copies each example cart into its own throwaway workspace and builds it
 *      `--debug` (priming the incremental build so the extension's own
 *      in-session build is a near no-op),
 *   3. for each cart, launches a headless VS Code via @vscode/test-electron —
 *      with that single cart as the only workspace folder — and runs its Mocha
 *      spec inside the extension host.
 *
 * One cart per window, deliberately: a real user never has multiple carts open
 * in one window, and the extension resolves the cart to debug from the active
 * editor / workspace folder. Isolating each cart in its own window mirrors that
 * and lets the tests exercise the real auto-detection path (no explicit `cart`).
 *
 * The suite drives real `blyt` debug sessions through the extension's public
 * surface (vscode.debug.startDebugging / breakpoints / customRequest), so it
 * exercises the extension's session orchestration — not just the runtime.
 *
 * NOTE: native debug sessions open an SDL2 window (the extension does not pass
 * --headless in native mode), so this currently needs a display. On Linux that
 * means wrapping the run in xvfb-run; the `test-vscode` cmake target does this
 * automatically. See test/integration/README.md.
 */

const path = require('node:path');
const os = require('node:os');
const fs = require('node:fs');
const cp = require('node:child_process');
const { runTests } = require('@vscode/test-electron');

/* Pin the VS Code build so runs are deterministic and the download is
 * cacheable in CI. Bump deliberately. */
const VSCODE_VERSION = '1.125.1';

/* Each cart runs in its own VS Code window with only its own spec.
 *
 * `name` (optional) is the display + throwaway-workspace key; it defaults to
 * `dir` and only needs setting when two entries share a dir (so their
 * workspaces don't collide).  `env` (optional) is merged into extensionTestsEnv
 * for that window — used to select a runtime variant of the SAME cart+spec, e.g.
 * BLYT_HOSTLUA=1 runs the pure-Lua debug flow through the native host-Lua VM
 * (#234) instead of the emulated rv32 Lua VM, asserting identical debugger
 * behaviour across both. */
const CARTS = [
	{ dir: 'hello', spec: 'lua.test.js' },
	{
		dir: 'hello',
		spec: 'lua.test.js',
		name: 'hello-hostlua',
		env: { BLYT_HOSTLUA: '1' },
	},
	{ dir: 'hello-c', spec: 'c.test.js' },
	{ dir: 'hello-c', spec: 'wasm.test.js' },
	{ dir: 'hello-lua-c', spec: 'hybrid.test.js' },
];

function fail(msg) {
	console.error(`\n[runTests] ${msg}\n`);
	process.exit(1);
}

/* BLYT_IT_CART narrows the run to a single cart window (parity with the cargo
 * suites' BLYT_TEST_FILTER). Matches against the cart dir, the spec filename, or
 * the `dir/spec` pair — whichever the caller finds convenient. */
function selectCarts(carts) {
	const sel = process.env.BLYT_IT_CART;
	if (!sel) return carts;
	const filtered = carts.filter(
		({ dir, spec, name }) =>
			dir === sel ||
			spec === sel ||
			name === sel ||
			`${name || dir}/${spec}`.includes(sel),
	);
	if (filtered.length === 0)
		fail(
			`BLYT_IT_CART=${sel} matched no cart (have: ` +
				`${carts.map((c) => `${c.dir}/${c.spec}`).join(', ')})`,
		);
	return filtered;
}

async function main() {
	const extensionDevelopmentPath = path.resolve(__dirname, '..', '..');
	const extensionTestsPath = path.resolve(__dirname, 'suite', 'index.js');
	const repoRoot = path.resolve(extensionDevelopmentPath, '..', '..');
	const sdkDir =
		process.env.BLYT_SDK_DIR || path.join(repoRoot, 'build', 'sdk');

	for (const bin of ['blyt', 'blytdebug', 'blyt-lldb-dap']) {
		if (!fs.existsSync(path.join(sdkDir, 'bin', bin)))
			fail(
				`missing ${path.join(sdkDir, 'bin', bin)} — build the sdk target first ` +
					`(cmake --build build --target sdk), or set BLYT_SDK_DIR.`,
			);
	}

	const tmpRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'blyt-vscode-it-'));

	/* BLYT_VSCODE_TEST_CACHE redirects the @vscode/test-electron download cache
	 * (default: <package>/.vscode-test). The test-linux-docker leg points it at a
	 * named volume so the pinned VS Code build is not re-downloaded every run. */
	const cachePath = process.env.BLYT_VSCODE_TEST_CACHE || undefined;

	let anyFailed = false;
	for (const { dir, spec, name, env: cartEnv } of selectCarts(CARTS)) {
		const key =
			name || dir; /* display + workspace key (unique per entry) */
		/* Each cart is its own single-folder workspace. Copy (minus build/) and
		 * build --debug; the extension rebuilds incrementally in-session.  The
		 * workspace folder keeps its example basename (`dir`) — the specs look it
		 * up by name via h.folder(dir) — under a per-entry parent (`key`) so two
		 * entries sharing a dir (e.g. the emulated + host-Lua `hello` legs) get
		 * distinct workspaces. */
		const src = path.join(repoRoot, 'examples', dir);
		const workspace = path.join(tmpRoot, key, dir);
		fs.cpSync(src, workspace, {
			recursive: true,
			filter: (s) => path.basename(s) !== 'build',
		});
		console.log(`[runTests] blyt build --debug ${key} …`);
		const r = cp.spawnSync(
			path.join(sdkDir, 'bin', 'blyt'),
			['build', '--debug', workspace],
			{ env: { ...process.env, BLYT_SDK_DIR: sdkDir }, stdio: 'inherit' },
		);
		if (r.status !== 0) fail(`blyt build --debug ${dir} failed`);

		const userDataDir = fs.mkdtempSync(
			path.join(os.tmpdir(), 'blyt-vscode-ud-'),
		);
		/* Both of VS Code's writable state dirs must be redirected out of the
		 * package. @vscode/test-electron defaults --extensions-dir (and
		 * --user-data-dir) to <package>/.vscode-test/*, which BLYT_VSCODE_TEST_CACHE
		 * does NOT cover — cachePath only redirects the VS Code *download*. Under
		 * test-linux-docker the repo is mounted read-only, so that default made the
		 * leg depend on host state: a checkout that had never run this suite
		 * natively had no .vscode-test/, and VS Code died at startup with ENOENT
		 * (mkdir), while one that had ran but logged EROFS on every extensions.json
		 * write. A tmp dir per window is both hermetic and writable. */
		const extensionsDir = fs.mkdtempSync(
			path.join(os.tmpdir(), 'blyt-vscode-ext-'),
		);
		console.log(`[runTests] running ${spec} in a ${key} window …`);
		try {
			await runTests({
				version: VSCODE_VERSION,
				cachePath,
				extensionDevelopmentPath,
				extensionTestsPath,
				launchArgs: [
					workspace,
					'--user-data-dir',
					userDataDir,
					'--extensions-dir',
					extensionsDir,
					'--disable-workspace-trust',
					'--disable-gpu',
					'--no-cached-data',
					/* The WASM-debug leg runs the cart's frame loop (requestAnimationFrame)
					 * inside a VS Code webview. Chromium throttles rAF and timers in
					 * renderers it considers backgrounded/occluded — which a headless,
					 * unfocused test window often is — so the cart stalls before frame 10
					 * and the breakpoint never hits (a 120s timeout, not a 2–20s stop).
					 * Disable that throttling so the loop advances deterministically
					 * regardless of window focus. */
					'--disable-background-timer-throttling',
					'--disable-renderer-backgrounding',
					'--disable-backgrounding-occluded-windows',
				],
				extensionTestsEnv: {
					BLYT_SDK_DIR: sdkDir,
					BLYT_TRACE: '',
					BLYT_IT_SPEC: spec,
					BLYT_IT_GREP: process.env.BLYT_IT_GREP || '',
					/* Per-entry overrides (e.g. BLYT_HOSTLUA=1); the extension's
					 * spawned blytdebug inherits the extension-host env. */
					...(cartEnv || {}),
				},
			});
		} catch (err) {
			anyFailed = true;
			console.error(
				`[runTests] ${key} (${spec}) failed: ${err?.message ? err.message : err}`,
			);
		}

		/* Reap the cart's `blyt debug` dev-server, if it outlived its window.
		 * The extension kills it on session terminate / deactivate, but
		 * @vscode/test-electron tears the window down at end-of-run, and a
		 * non-detached grandchild can outlive that exit and orphan (holding
		 * ports + spinning the cart loop). Keying the kill on this run's unique
		 * tmpRoot makes it unambiguously our own process — never a developer's
		 * real session. Best-effort; unix-only (the suite runs on macOS/Linux).
		 * The proper fix lives extension-side (kill the dev-server even on abrupt
		 * window teardown) — tracked in #142; this reaper goes away once that
		 * lands. */
		reapDevServers(tmpRoot);
	}

	if (anyFailed) fail('one or more cart windows had failing tests');
}

/* Kill any lingering process whose command line references `marker` (this run's
 * throwaway workspace root). Silent if pkill is absent or matches nothing. */
function reapDevServers(marker) {
	try {
		cp.spawnSync('pkill', ['-f', marker], { stdio: 'ignore' });
	} catch {
		/* no pkill (non-unix) — nothing to do */
	}
}

main();
