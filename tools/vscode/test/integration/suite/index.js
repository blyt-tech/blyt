/* Mocha bootstrap, executed INSIDE the VS Code extension host. @vscode/test-
 * electron requires this module's `run()` and awaits the returned promise. */

const path = require('node:path');
const Mocha = require('mocha');
const { glob } = require('glob');

function run() {
	const mocha = new Mocha({
		ui: 'bdd',
		color: true,
		/* Sessions spin up blytdebug + lldb-dap and run a cart to a breakpoint —
		 * give each test generous headroom. */
		timeout: 120000,
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
