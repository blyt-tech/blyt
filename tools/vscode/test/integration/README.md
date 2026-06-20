# VS Code debugger integration suite

Headless end-to-end tests that drive real `blyt` debug sessions **through the
VS Code extension** (`vscode.debug.startDebugging`, the breakpoint API, and
`session.customRequest`) via [`@vscode/test-electron`]. Unlike the runtime-level
drivers in `tests/dap` / `tests/gdb`, these exercise the extension's own session
orchestration — cart detection, native vs. companion-Lua session launch, and the
`BlytGdbDapProxy` breakpoint handling.

## What it covers

`runTests.js` copies the `hello` (Lua), `hello-c` (C), and `hello-lua-c`
(hybrid) example carts each into its own single-folder workspace, builds it
`--debug`, and launches a headless VS Code **per cart** — one cart per window,
mirroring real use — running that cart's Mocha spec from `suite/`:

- **lua.test.js** — set a Lua breakpoint; assert stack frame + local values;
  continue across frames; add/remove breakpoints while running.
- **c.test.js** — the same matrix against a pure-C cart (lldb-dap + GDB stub).
- **hybrid.test.js** — breakpoints in **both** languages at once; assert each
  session's stack/variables; assert the lldb proxy reports `.lua` breakpoints
  verified (the Fix 2 short-circuit on this branch).

One cart per window is deliberate: a real user never opens multiple carts in one
window, and the extension resolves which cart to debug from the active editor /
workspace folder. Isolating each cart lets the tests use the real auto-detection
path (no explicit `cart` in the launch config).

Assertions use the carts' deterministic state (player starts at 160,120 and
advances 1px every 10th frame), so the first stop is always `x=161, y=121`.

## Running locally

```sh
cd tools/vscode
npm install                 # once (installs @vscode/test-electron, mocha, glob)
npm run test:integration    # builds carts, downloads VS Code on first run, runs
```

`BLYT_SDK_DIR` defaults to this worktree's `build/sdk`; set it to override.
Build the SDK first if needed: `cmake --build build --target sdk`.

## Display requirement / CI

Native debug sessions open an SDL2 window (the extension does not pass
`--headless` in native mode), **and** `@vscode/test-electron` launches a real
Electron app — so this needs a display. Locally on macOS that just means a few
windows flicker open and closed. On Linux CI, wrap the command in a virtual
framebuffer:

```sh
xvfb-run -a npm run test:integration
```

Wiring this into CI (a job that installs the SDK, then `xvfb-run`s the suite) is
a follow-up; it is not yet part of `.github/workflows/ci.yml`.

[`@vscode/test-electron`]: https://github.com/microsoft/vscode-test
