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

The canonical way is the `test-vscode` cmake target, which builds the SDK,
installs the extension's npm deps, then runs the extension unit tests
(`npm test`) **and** this suite against the freshly-built `build/sdk` — on Linux
it wraps the run in `xvfb-run` automatically (see below):

```sh
cmake --build build --target test-vscode
```

Narrowing (parity with the cargo suites' `BLYT_TEST_FILTER`), reconfigure then
build:

```sh
cmake -DBLYT_VSCODE_CART=wasm.test.js -B build   # one cart window (dir or spec)
cmake -DBLYT_VSCODE_GREP='stops at a C' -B build # mocha test-name regex
cmake --build build --target test-vscode
```

Or drive `runTests.js` directly (e.g. when iterating on the suite itself):

```sh
cd tools/vscode
npm install                 # once (installs @vscode/test-electron, mocha, glob)
npm run test:integration    # builds carts, downloads VS Code on first run, runs
```

`BLYT_SDK_DIR` defaults to this worktree's `build/sdk`; set it to override.
Build the SDK first if needed: `cmake --build build --target sdk`. The direct
runner honours `BLYT_IT_CART` / `BLYT_IT_GREP` (what the cmake vars set) and
`BLYT_VSCODE_TEST_CACHE` (override the `@vscode/test-electron` download cache;
the Linux Docker leg points it at a named volume).

## Display requirement / Linux / Docker

Native debug sessions open an SDL2 window (the extension does not pass
`--headless` in native mode), **and** `@vscode/test-electron` launches a real
Electron app — so this needs a display. Locally on macOS that just means a few
windows flicker open and closed. On Linux, wrap the command in a virtual
framebuffer (the `test-vscode` target does this for you when `xvfb-run` is
present):

```sh
xvfb-run -a npm run test:integration
```

To run the whole thing on Linux exactly as CI does — including a display and a
cached VS Code build so repeats don't re-download it — use the Docker target,
which now runs this suite after the cargo workspace tests:

```sh
cmake --build build --target test-linux-docker          # all suites + this one
cmake -DBLYT_SKIP_VSCODE=ON -B build && \
  cmake --build build --target test-linux-docker        # cargo only, skip vscode
```

The container's `Dockerfile.testing` installs `xvfb` + Electron's GUI runtime
libs; the docker run gets `--shm-size=2g` (the default 64 MB `/dev/shm` is too
small for the heavier Electron windows — a webview or multiple debug sessions
exhaust it and the renderer dies with `renderer process gone (reason: crashed,
code: 5)`). The pinned VS Code build is downloaded once into the
`blyt-vscode-test-cache-<arch>` volume, and the extension's JS deps are resolved
from the host's `tools/vscode/node_modules` (the repo is mounted read-only, so
`vscode_ext_deps` installs them on the host first and the container reuses
them).

CI also runs this suite as the standalone `VS Code debugger integration tests`
step in `.github/workflows/ci.yml` (SDK build, then `xvfb-run npm run
test:integration`).

### Gotcha: webview frame-loop throttling

The WASM-debug leg runs the cart's `requestAnimationFrame` loop inside a VS Code
webview. Chromium throttles rAF/timers in renderers it deems
backgrounded/occluded — which a headless, unfocused test window often is — so
the cart can stall before frame 10 and the breakpoint never hits (a 120s
timeout, not a 2–20s stop). `runTests.js` launches VS Code with
`--disable-background-timer-throttling --disable-renderer-backgrounding
--disable-backgrounding-occluded-windows` to keep the loop advancing regardless
of focus. The runner also reaps any `blyt debug` dev-server that outlives its
window (keyed on the run's throwaway workspace path), so repeated local runs
don't accumulate orphaned servers.

[`@vscode/test-electron`]: https://github.com/microsoft/vscode-test
