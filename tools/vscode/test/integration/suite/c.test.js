/* Pure-C cart (examples/hello-c) debugged in native mode: lldb-dap (wrapped by
 * BlytGdbDapProxy) talking GDB RSP to blytdebug's stub.
 *
 * Same deterministic state as the Lua cart: first stop on hello.c:34 is frame
 * 10 with slot=0, x=161, y=121. Unlike Lua, the top frame carries the real
 * C function name and lldb exposes a Globals/Registers scope alongside Locals.
 */

const h = require('./harness');
const { assert } = h;

const C = 'src/game/c/hello.c';
const BP_SNPRINTF = 34; // snprintf(buf, ...)   (x,y already assigned)
const BP_DEBUG = 35; // blyt_console_debug(buf)
const BP_INIT = 10; // s_frame = 0;  (blyt_cart_init body — runs once per boot)

describe('C cart (native debug)', () => {
	afterEach(async () => h.reset());

	it('stops at a C breakpoint with the expected stack and variables', async () => {
		const wf = h.folder('hello-c');
		h.addBreakpoint(h.fileUri(wf, C), BP_SNPRINTF);
		await h.startNative(wf);

		const session = await h.waitForSession(
			h.byMode('gdb'),
			'gdb (native) session',
		);
		const stop = await h.waitStopped(session);
		assert.strictEqual(stop.body.reason, 'breakpoint');

		const frame = await h.topFrame(session, stop.body.threadId);
		assert.strictEqual(frame.line, BP_SNPRINTF);
		assert.strictEqual(frame.name, 'blyt_cart_update');
		assert.ok((frame.source?.path || '').endsWith('hello.c'));

		const vars = await h.locals(session, frame.id);
		assert.strictEqual(vars.slot, '0');
		assert.strictEqual(vars.x, '161');
		assert.strictEqual(vars.y, '121');
	});

	it('continues deterministically to the next frame', async () => {
		const wf = h.folder('hello-c');
		h.addBreakpoint(h.fileUri(wf, C), BP_SNPRINTF);
		await h.startNative(wf);

		const session = await h.waitForSession(
			h.byMode('gdb'),
			'gdb (native) session',
		);

		const stop1 = await h.waitStopped(session);
		const f1 = await h.topFrame(session, stop1.body.threadId);
		assert.strictEqual((await h.locals(session, f1.id)).x, '161');

		await h.cont(session, stop1.body.threadId);

		const stop2 = await h.waitStopped(session);
		const f2 = await h.topFrame(session, stop2.body.threadId);
		assert.strictEqual((await h.locals(session, f2.id)).x, '162');
	});

	it('honors breakpoints added and removed while running', async () => {
		const wf = h.folder('hello-c');
		const uri = h.fileUri(wf, C);
		const bpSnprintf = h.addBreakpoint(uri, BP_SNPRINTF);
		await h.startNative(wf);

		const session = await h.waitForSession(
			h.byMode('gdb'),
			'gdb (native) session',
		);

		const stop1 = await h.waitStopped(session);
		assert.strictEqual(
			(await h.topFrame(session, stop1.body.threadId)).line,
			BP_SNPRINTF,
		);

		h.removeBreakpoint(bpSnprintf);
		h.addBreakpoint(uri, BP_DEBUG);
		/* Wait for lldb to actually bind the new breakpoint before continuing —
		 * otherwise the continue races the insertion and it never fires. */
		await h.waitBreakpointBound(session, BP_DEBUG);
		await h.cont(session, stop1.body.threadId);

		const stop2 = await h.waitStopped(session);
		assert.strictEqual(
			(await h.topFrame(session, stop2.body.threadId)).line,
			BP_DEBUG,
			'next stop is the newly added breakpoint, not the removed one',
		);
	});

	/* Issue #119 acceptance criteria 1+5: a native debug session survives a hot
	 * reload, and an init() breakpoint rebinds and re-fires on the rebuilt code
	 * — the user never re-launches.  init() runs once per boot, so its
	 * breakpoint re-firing proves the cart rebooted AND the breakpoint rebound
	 * across the reload (new guest base, fresh DWARF). */
	it('rebinds an init() breakpoint across a hot reload', async () => {
		const wf = h.folder('hello-c');
		const uri = h.fileUri(wf, C);
		h.addBreakpoint(uri, BP_INIT);
		await h.startNative(wf);

		const session = await h.waitForSession(
			h.byMode('gdb'),
			'gdb (native) session',
		);

		/* The breakpoint binds at attach and fires when init() runs at boot. */
		const stop1 = await h.waitStopped(session, 'initial init() stop');
		assert.strictEqual(stop1.body.reason, 'breakpoint');
		const f1 = await h.topFrame(session, stop1.body.threadId);
		assert.strictEqual(f1.line, BP_INIT);
		assert.strictEqual(f1.name, 'blyt_cart_init');
		await h.cont(session, stop1.body.threadId);

		/* Edit + save → `blyt debug` watcher rebuilds → dev-control `reload` →
		 * blytdebug two-phase solib swap + cart reboot.  The proxy auto-continues
		 * the solib-swap (SIGTRAP) stops; the re-fired init() breakpoint surfaces
		 * as reason 'breakpoint' at the new base.  Generous wait covers the
		 * rebuild + swap. */
		h.touchRebuild(uri);
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
			'init() breakpoint re-fired after the reload',
		);
		assert.strictEqual(f2.name, 'blyt_cart_init');
	});
});
