// module_pre.js — injected by --pre-js inside the Emscripten IIFE.
//
// This file runs inside the IIFE at the start of the generated JS, after
// Emscripten's initial `var Module = typeof Module != 'undefined' ? Module : {};`
// assignment.  Our second `var Module = (function() { ... })()` overrides it
// before Emscripten reads any Module properties (preRun, print, etc.).
//
// The preRun hook (writes /cart.blyt to MEMFS) is defined here so it can
// access the IIFE-local `FS` variable directly — closures over IIFE-local
// variables are not accessible from outside the IIFE.
//
// Node.js test driver protocol (tests/wasm/run_cart.js):
//   globalThis.__blyt_cart_data   = Uint8Array  — cart bytes to write to MEMFS
//   globalThis.__blyt_frame0_path = string|null — if set, dump frame here on exit
//   globalThis.__blyt_init_module = object      — extra Module fields (print, etc.)
//
// Frame dump:
//   The blyt_js_dump_frame0_if_headless EM_JS function in wasm_main.c writes
//   the frame directly to globalThis.__blyt_frame0_path via require('fs') on
//   the first FRAME_DONE when the path is set.  No Module.onExit hook needed.
/* global globalThis */
var Module = (function () {
  var base =
    typeof globalThis !== "undefined" && globalThis.__blyt_init_module
      ? globalThis.__blyt_init_module
      : typeof Module !== "undefined"
      ? Module
      : {};
  if (!base.preRun) base.preRun = [];
  if (typeof globalThis !== "undefined" && globalThis.__blyt_cart_data) {
    base.preRun.unshift(function () {
      FS.writeFile("/cart.blyt", globalThis.__blyt_cart_data);
    });
  }
  /* ENV injection: run_cart.js sets __blyt_env_vars to forward C getenv() keys
   * (e.g. BLYT_SAVE_DIR) into the Emscripten C environment.  The preRun hook
   * runs after the module-local ENV object is assigned but before C startup
   * initialises __environ, so getenv() sees the injected values. */
  if (typeof globalThis !== "undefined" && globalThis.__blyt_env_vars) {
    base.preRun.push(function () {
      var vars = globalThis.__blyt_env_vars;
      for (var k in vars) {
        ENV[k] = vars[k];
      }
    });
  }
  return base;
})();
