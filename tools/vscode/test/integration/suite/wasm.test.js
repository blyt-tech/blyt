/* Pure-C cart (examples/hello-c) debugged in the default WASM mode: the cart
 * runs in a webview panel served by `blyt debug <dir>`, lldb-dap (wrapped by
 * BlytGdbDapProxy) talks GDB RSP through the devtool's browser relay.
 *
 * This is #90's headline mode and the path that carried the `debugCart`
 * ReferenceError (the native `program` return) — which only surfaced once a
 * session actually resolved through it.  Same deterministic state as the native
 * leg: the first stop on hello.c:34 is frame 10 with slot=0, x=161, y=121.
 */

const h = require('./harness');
const { assert } = h;

const C = 'src/game/c/hello.c';
const BP_SNPRINTF = 34; // snprintf(buf, ...)   (x,y already assigned)

/* Fixed by #144.  Under #119's cart-as-library/stub-program model, lldb-dap's
 * `program` is the stub ELF, so it reads the cart's DWARF from the file named in
 * the svr4 library list.  The WASM runtime loads the cart from the in-memory
 * "/cart.blyt", which host-side lldb cannot open — so the breakpoint never bound
 * and the session timed out.  The fix injects a host-resolvable cart-ELF path
 * (`blyt debug` → shell.html → blyt_session_gdb_set_cart_path).  A headless,
 * Electron-free version of this lives in the integration suite as
 * lldb_dap.rs::wasm_lldb_dap_source_breakpoint (tests/dap/run_wasm_lldb_dap_test.mjs). */
describe('C cart (WASM debug)', () => {
	afterEach(async () => h.reset());

	it('resolves a WASM debug session and stops at a C breakpoint', async () => {
		const wf = h.folder('hello-c');
		h.addBreakpoint(h.fileUri(wf, C), BP_SNPRINTF);
		await h.startWasm(wf);

		/* A 'gdb' session resolving at all proves the WASM-debug native return
		 * is well-formed (the debugCart ReferenceError threw here before the
		 * session could start). */
		const session = await h.waitForSession(
			h.byMode('gdb'),
			'gdb (WASM) session',
		);

		/* WASM brings up a browser relay + WASM-ready handshake before the stop,
		 * so allow extra time vs the native leg. */
		const stop = await h.waitStopped(
			session,
			'WASM breakpoint stop',
			120000,
		);
		assert.strictEqual(stop.body.reason, 'breakpoint');

		const frame = await h.topFrame(session, stop.body.threadId);
		assert.strictEqual(frame.line, BP_SNPRINTF);
		assert.ok((frame.source?.path || '').endsWith('hello.c'));

		const vars = await h.locals(session, frame.id);
		assert.strictEqual(vars.x, '161');
		assert.strictEqual(vars.y, '121');
	});
});
