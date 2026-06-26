/* Pure-Lua cart (examples/hello) debugged in native mode through the extension.
 *
 * Determinism: the player starts at (160,120); update() bumps x/y by 1 every
 * 10th frame. So the FIRST stop on main.lua:26 is frame 10 with slot=0, x=161,
 * y=121; the next is frame 20 with x=162. Values are reported as strings.
 */

const h = require('./harness');
const { assert } = h;

const LUA = 'src/game/lua/main.lua';
const BP_ASSIGN_X = 26; // S.character[slot].x = x   (x,y already computed)
const BP_PRINT = 28; // blyt.debug.print(...)
const BP_INIT = 6; // frame = 0   (first statement inside init())

describe('Lua cart (native debug)', () => {
	afterEach(async () => h.reset());

	it('stops at a Lua breakpoint with the expected stack and variables', async () => {
		const wf = h.folder('hello');
		h.addBreakpoint(h.fileUri(wf, LUA), BP_ASSIGN_X);
		await h.startNative(wf);

		const session = await h.waitForSession(h.byMode('lua'), 'lua session');
		const stop = await h.waitStopped(session);
		assert.strictEqual(stop.body.reason, 'breakpoint');

		const frame = await h.topFrame(session, stop.body.threadId);
		assert.strictEqual(
			frame.line,
			BP_ASSIGN_X,
			'stopped on the breakpoint line',
		);
		assert.ok(
			(frame.source?.path || '').endsWith('main.lua'),
			`frame source is main.lua (got ${frame.source?.path})`,
		);

		const vars = await h.locals(session, frame.id);
		assert.strictEqual(vars.frame, '10', 'first qualifying frame is 10');
		assert.strictEqual(vars.slot, '0');
		assert.strictEqual(vars.x, '161');
		assert.strictEqual(vars.y, '121');
	});

	it('continues deterministically to the next frame', async () => {
		const wf = h.folder('hello');
		h.addBreakpoint(h.fileUri(wf, LUA), BP_ASSIGN_X);
		await h.startNative(wf);

		const session = await h.waitForSession(h.byMode('lua'), 'lua session');

		const stop1 = await h.waitStopped(session);
		const f1 = await h.topFrame(session, stop1.body.threadId);
		const v1 = await h.locals(session, f1.id);
		assert.strictEqual(v1.x, '161');

		await h.cont(session, stop1.body.threadId);

		const stop2 = await h.waitStopped(session);
		const f2 = await h.topFrame(session, stop2.body.threadId);
		const v2 = await h.locals(session, f2.id);
		assert.strictEqual(
			v2.frame,
			'20',
			'advanced to the next qualifying frame',
		);
		assert.strictEqual(v2.x, '162', 'x advanced 161 -> 162');
	});

	/* Issue #140 acceptance criterion: a native Lua debug session survives a hot
	 * reload driven by `blyt debug <dir>` — an init() breakpoint re-fires on the
	 * rebuilt Lua state without a session restart.  The extension now wires the
	 * Lua path to startDevtool + startNativeDebug(--dev-ctrl-connect) so the
	 * file-watcher rebuild reaches the live DAP session. */
	it('rebinds an init() breakpoint across a Lua hot reload', async () => {
		const wf = h.folder('hello');
		const uri = h.fileUri(wf, LUA);
		h.addBreakpoint(uri, BP_INIT);
		await h.startNative(wf);

		const session = await h.waitForSession(h.byMode('lua'), 'lua session');

		/* Breakpoint fires when init() runs at boot. */
		const stop1 = await h.waitStopped(session, 'initial init() stop');
		assert.strictEqual(stop1.body.reason, 'breakpoint');
		const f1 = await h.topFrame(session, stop1.body.threadId);
		assert.strictEqual(f1.line, BP_INIT);
		await h.cont(session, stop1.body.threadId);

		/* Edit + save → `blyt debug` watcher rebuilds → dev-control `reload` →
		 * blytdebug hot-swaps the Lua state (same base, breakpoints persist in
		 * g_dap.bps[], no session restart).  The re-armed init() breakpoint
		 * surfaces on the same DAP connection with reason 'breakpoint'. */
		h.touchRebuildLua(uri);
		const stop2 = await h.waitStopped(
			session,
			'post-reload init() stop',
			60000,
		);
		assert.strictEqual(stop2.body.reason, 'breakpoint');
		const f2 = await h.topFrame(session, stop2.body.threadId);
		assert.strictEqual(
			f2.line,
			BP_INIT,
			'init() breakpoint re-fired after the Lua reload',
		);
	});

	it('honors breakpoints added and removed while running', async () => {
		const wf = h.folder('hello');
		const uri = h.fileUri(wf, LUA);
		const bpAssign = h.addBreakpoint(uri, BP_ASSIGN_X);
		await h.startNative(wf);

		const session = await h.waitForSession(h.byMode('lua'), 'lua session');

		const stop1 = await h.waitStopped(session);
		const f1 = await h.topFrame(session, stop1.body.threadId);
		assert.strictEqual(f1.line, BP_ASSIGN_X);

		/* Remove the line we're parked on and add a new one further down the
		 * same frame. If removal failed we'd re-stop at BP_ASSIGN_X; if the add
		 * failed we'd never stop at BP_PRINT. */
		h.removeBreakpoint(bpAssign);
		h.addBreakpoint(uri, BP_PRINT);
		/* Wait for the new breakpoint to bind before continuing, so the continue
		 * can't race the insertion. */
		await h.waitBreakpointBound(session, BP_PRINT);
		await h.cont(session, stop1.body.threadId);

		const stop2 = await h.waitStopped(session);
		const f2 = await h.topFrame(session, stop2.body.threadId);
		assert.strictEqual(
			f2.line,
			BP_PRINT,
			'next stop is the newly added breakpoint, not the removed one',
		);
	});
});
