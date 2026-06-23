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
				assert.strictEqual(v.x, '161');
				assert.strictEqual(v.y, '121');
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
});
