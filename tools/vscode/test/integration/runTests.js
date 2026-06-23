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
 * --headless in native mode), so this currently needs a display. On CI that
 * means wrapping the run in xvfb-run; see test/integration/README.md.
 */

const path = require('node:path');
const os = require('node:os');
const fs = require('node:fs');
const cp = require('node:child_process');
const { runTests } = require('@vscode/test-electron');

/* Pin the VS Code build so runs are deterministic and the download is
 * cacheable in CI. Bump deliberately. */
const VSCODE_VERSION = '1.125.1';

/* Each cart runs in its own VS Code window with only its own spec. */
const CARTS = [
	{ dir: 'hello', spec: 'lua.test.js' },
	{ dir: 'hello-c', spec: 'c.test.js' },
	{ dir: 'hello-c', spec: 'wasm.test.js' },
	{ dir: 'hello-lua-c', spec: 'hybrid.test.js' },
];

function fail(msg) {
	console.error(`\n[runTests] ${msg}\n`);
	process.exit(1);
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

	let anyFailed = false;
	for (const { dir, spec } of CARTS) {
		/* Each cart is its own single-folder workspace. Copy (minus build/) and
		 * build --debug; the extension rebuilds incrementally in-session. */
		const src = path.join(repoRoot, 'examples', dir);
		const workspace = path.join(tmpRoot, dir);
		fs.cpSync(src, workspace, {
			recursive: true,
			filter: (s) => path.basename(s) !== 'build',
		});
		console.log(`[runTests] blyt build --debug ${dir} …`);
		const r = cp.spawnSync(
			path.join(sdkDir, 'bin', 'blyt'),
			['build', '--debug', workspace],
			{ env: { ...process.env, BLYT_SDK_DIR: sdkDir }, stdio: 'inherit' },
		);
		if (r.status !== 0) fail(`blyt build --debug ${dir} failed`);

		const userDataDir = fs.mkdtempSync(
			path.join(os.tmpdir(), 'blyt-vscode-ud-'),
		);
		console.log(`[runTests] running ${spec} in a ${dir} window …`);
		try {
			await runTests({
				version: VSCODE_VERSION,
				extensionDevelopmentPath,
				extensionTestsPath,
				launchArgs: [
					workspace,
					'--user-data-dir',
					userDataDir,
					'--disable-workspace-trust',
					'--disable-gpu',
					'--no-cached-data',
				],
				extensionTestsEnv: {
					BLYT_SDK_DIR: sdkDir,
					BLYT_TRACE: '',
					BLYT_IT_SPEC: spec,
					BLYT_IT_GREP: process.env.BLYT_IT_GREP || '',
				},
			});
		} catch (err) {
			anyFailed = true;
			console.error(
				`[runTests] ${dir} (${spec}) failed: ${err?.message ? err.message : err}`,
			);
		}
	}

	if (anyFailed) fail('one or more cart windows had failing tests');
}

main();
