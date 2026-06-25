/* Hybrid Lua+C cart (examples/hello-lua-c) debugged in native mode. This is the
 * scenario the wasm-hybrid-debug-fix branch is about: TWO debug sessions at once
 * — a native (C) session via BlytGdbDapProxy/lldb-dap and a companion Lua DAP
 * session — with breakpoints in both languages.
 *
 * Determinism: greeting.log() is first called from on_new_state with the literal
 * "init player pos: 160, 120", so the FIRST hit of greeting.c:5 sees exactly that
 * string. The Lua update breakpoint then hits at frame 10 with slot=0, x=161,
 * y=121 (main.lua here is offset by the `require("greeting")` line, so the x
 * assignment is line 27).
 */

const h = require('./harness');
const { assert } = h;

const LUA = 'src/game/lua/main.lua';
const C = 'src/game/c/greeting.c';
const BP_LUA_ASSIGN_X = 27; // S.character[slot].x = x
const BP_C_LOG = 5; // blyt_console_debug(s)

describe('Hybrid Lua+C cart (native debug)', () => {
	afterEach(async () => h.reset());

	it('binds breakpoints in both Lua and C with the expected values', async () => {
		const wf = h.folder('hello-lua-c');
		/* Both breakpoints are set before launch so the C one binds during
		 * initial config — a C breakpoint set mid-run, while the cart is paused
		 * in the Lua hook, is not reliably inserted by the GDB stub. */
		h.addBreakpoint(h.fileUri(wf, C), BP_C_LOG);
		h.addBreakpoint(h.fileUri(wf, LUA), BP_LUA_ASSIGN_X);
		await h.startNative(wf);

		const native = await h.waitForSession(
			h.byMode('gdb'),
			'gdb (native) session',
		);
		const lua = await h.waitForSession(h.byMode('lua'), 'lua session');

		/* The two sessions can stop in either order across platforms: the C
		 * breakpoint may first trip in on_new_state's greeting.log or later in
		 * update()'s. Handle whichever stops, continue it, and require both to
		 * have hit with the expected location/values before finishing. */
		let luaOk = false;
		let cOk = false;
		for (let i = 0; i < 8 && !(luaOk && cOk); i++) {
			const { session, ev } = await h.waitAnyStopped(
				[native, lua],
				'Lua or C breakpoint',
			);
			if (session === lua) {
				const f = await h.topFrame(lua, ev.body.threadId);
				assert.strictEqual(f.line, BP_LUA_ASSIGN_X);
				assert.ok((f.source?.path || '').endsWith('main.lua'));
				const v = await h.locals(lua, f.id);
				assert.strictEqual(v.slot, '0');
				/* The first update() hit lands on whichever 10-frame tick the Lua
				 * breakpoint was armed by — frame 10 (x=161) on a fast host, but a
				 * slow CI runner can run a tick before the async bp-arm completes
				 * and first stop at frame 20 (x=162).  That is a debugger
				 * arming-vs-frame-loop race, NOT a cart-determinism issue, so assert
				 * the player position is COHERENT for some such tick (x and y
				 * advance together from 160,120) rather than pinning the exact
				 * frame. */
				const x = parseInt(v.x, 10);
				const y = parseInt(v.y, 10);
				assert.ok(
					x >= 161 && y === x - 40,
					`coherent player pos at first update hit (x=${v.x}, y=${v.y})`,
				);
				luaOk = true;
				await h.cont(lua, ev.body.threadId);
			} else {
				const f = await h.topFrame(native, ev.body.threadId);
				assert.strictEqual(f.line, BP_C_LOG);
				assert.ok((f.source?.path || '').endsWith('greeting.c'));
				const v = await h.locals(native, f.id);
				/* "init player pos: 160, 120" or "update frame N player pos: …",
				 * depending on which greeting.log first trips it. */
				assert.ok(
					(v.s || '').includes('player pos'),
					`C local s holds a log string (got ${v.s})`,
				);
				cOk = true;
				await h.cont(native, ev.body.threadId);
			}
		}
		assert.ok(luaOk, 'Lua breakpoint was hit');
		assert.ok(cOk, 'C breakpoint was hit');
	});

	it('Lua breakpoint stops the cart and is verified by the lldb proxy (Fix 2)', async () => {
		const wf = h.folder('hello-lua-c');
		h.addBreakpoint(h.fileUri(wf, LUA), BP_LUA_ASSIGN_X);
		await h.startNative(wf);

		const native = await h.waitForSession(
			h.byMode('gdb'),
			'gdb (native) session',
		);
		const lua = await h.waitForSession(h.byMode('lua'), 'lua session');

		/* Execution must actually halt at the Lua breakpoint — the user-visible
		 * symptom that was broken (process wedged / breakpoints ignored). */
		const luaStop = await h.waitStopped(lua);
		const luaFrame = await h.topFrame(lua, luaStop.body.threadId);
		assert.strictEqual(
			luaFrame.line,
			BP_LUA_ASSIGN_X,
			'Lua execution halted at the breakpoint',
		);

		/* And the native (lldb) session, which also receives the .lua
		 * setBreakpoints, must report it verified rather than unverified — the
		 * BlytGdbDapProxy short-circuit. Otherwise VS Code's worst-case merge
		 * shows the breakpoint as unverified even though it works. */
		const luaResults = h.breakpointResultsFor(native, '.lua');
		assert.ok(
			luaResults.length > 0,
			'native session received the .lua setBreakpoints',
		);
		for (const r of luaResults) {
			for (const bp of r.breakpoints) {
				assert.strictEqual(
					bp.verified,
					true,
					'.lua breakpoint reported verified by the proxy',
				);
			}
		}
	});

	/* Hybrid reload-while-debugging (issue #119 criteria 4+5 — both Lua and native
	 * init breakpoints re-fire after a hot reload) is covered by the Rust
	 * integration test `sdl_hybrid_lldb_dap_reload_fires_both_init_breakpoints`
	 * (tests/integration/tests/lldb_dap.rs), which drives the same dual-client
	 * reload deterministically without an Electron window.  A VS Code-level hybrid
	 * reload test was tried here but proved too fragile under xvfb on slow CI
	 * runners (a third hybrid session in one window racing the prior sessions'
	 * teardown), so it was dropped in favour of that reliable coverage.  The
	 * extension's reload path itself is exercised by the C-native reload test in
	 * c.test.js. */
});
