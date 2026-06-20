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
		const cBp = h.addBreakpoint(h.fileUri(wf, C), BP_C_LOG);
		h.addBreakpoint(h.fileUri(wf, LUA), BP_LUA_ASSIGN_X);
		await h.startNative(wf);

		const native = await h.waitForSession(
			h.byMode('native'),
			'native session',
		);
		const lua = await h.waitForSession(h.byMode('lua'), 'lua session');

		/* C side fires first, from on_new_state's greeting.log("init …"). */
		const cStop = await h.waitStopped(native);
		const cFrame = await h.topFrame(native, cStop.body.threadId);
		assert.strictEqual(cFrame.line, BP_C_LOG);
		assert.ok((cFrame.source?.path || '').endsWith('greeting.c'));
		const cVars = await h.locals(native, cFrame.id);
		assert.ok(
			(cVars.s || '').includes('init player pos: 160, 120'),
			`C local s holds the init string (got ${cVars.s})`,
		);

		/* Let the cart run on into update(), where the Lua breakpoint waits. */
		h.removeBreakpoint(cBp);
		await h.cont(native, cStop.body.threadId);

		const luaStop = await h.waitStopped(lua);
		const luaFrame = await h.topFrame(lua, luaStop.body.threadId);
		assert.strictEqual(luaFrame.line, BP_LUA_ASSIGN_X);
		assert.ok((luaFrame.source?.path || '').endsWith('main.lua'));
		const luaVars = await h.locals(lua, luaFrame.id);
		assert.strictEqual(luaVars.slot, '0');
		assert.strictEqual(luaVars.x, '161');
		assert.strictEqual(luaVars.y, '121');
	});

	/* The lldb proxy's .lua "verified" short-circuit is added by the
	 * wasm-hybrid-debug-fix branch (Fix 2). On main the proxy forwards .lua
	 * breakpoints to lldb (reported unverified), so this assertion fails. Change
	 * `it.skip` back to `it` when that fix lands to re-enable the regression. */
	it.skip('Lua breakpoint stops the cart and is verified by the lldb proxy (Fix 2)', async () => {
		const wf = h.folder('hello-lua-c');
		h.addBreakpoint(h.fileUri(wf, LUA), BP_LUA_ASSIGN_X);
		await h.startNative(wf);

		const native = await h.waitForSession(
			h.byMode('native'),
			'native session',
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
