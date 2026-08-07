/* Mocha bootstrap, executed INSIDE the VS Code extension host. @vscode/test-
 * electron requires this module's `run()` and awaits the returned promise. */

const path = require('node:path');
const Mocha = require('mocha');
const { glob } = require('glob');

function run() {
	/* BLYT_IT_TIMEOUT_SCALE (default 1) multiplies every timeout — the mocha
	 * per-test ceiling here and each harness wait (suite/harness.js) — so a
	 * loaded CI runner or an emulated container gets proportional headroom
	 * without slowing fast local runs. runTests.js forwards it per cart window.
	 * The base ceiling is set ABOVE the worst single test's summed harness waits
	 * (e.g. the C hot-reload test: waitForSession 60s + waitStopped 90s +
	 * post-reload waitStopped 60s = 210s) so a slow-but-progressing test surfaces
	 * the harness's descriptive timeout instead of a bare mocha kill — the flake
	 * that recurred on slow runners when this ceiling sat below that sum. */
	const scale = Number(process.env.BLYT_IT_TIMEOUT_SCALE) || 1;
	const mocha = new Mocha({
		ui: 'bdd',
		color: true,
		/* Sessions spin up blytdebug + lldb-dap and run a cart to a breakpoint —
		 * give each test generous headroom. */
		timeout: 300000 * scale,
		slow: 20000,
	});

	/* BLYT_IT_GREP narrows the run to matching test names (fast iteration). */
	if (process.env.BLYT_IT_GREP)
		mocha.grep(new RegExp(process.env.BLYT_IT_GREP));

	/* runTests.js opens one cart per VS Code window and sets BLYT_IT_SPEC to the
	 * single spec for that cart, so each window runs only its own tests. */
	const pattern = process.env.BLYT_IT_SPEC || '*.test.js';

	const suiteDir = __dirname;
	return glob(pattern, { cwd: suiteDir }).then(
		(files) =>
			new Promise((resolve, reject) => {
				for (const f of files) mocha.addFile(path.join(suiteDir, f));
				try {
					mocha.run((failures) => {
						if (failures > 0)
							reject(new Error(`${failures} test(s) failed.`));
						else resolve();
					});
				} catch (err) {
					reject(err);
				}
			}),
	);
}

module.exports = { run };
