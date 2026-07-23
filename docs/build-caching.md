# Build caching

The build uses [ccache](https://ccache.dev) automatically whenever it is on
PATH (`brew install ccache` / `apt install ccache`); disable with
`-DBLYT_CCACHE=OFF` at configure time. The launcher covers every CMake-driven
C/C++ compile: the host tree, the riscv64 cross tree, and — via the `sdk`
target — the nested libc++ trees and both emcmake WASM trees. ccache replays
bit-identical compiler output for identical inputs, so the determinism
contract is unaffected.

With a warm cache, a full rebuild from a wiped `build/` tree is roughly
link-time-only (≈80 s for `build` + `sdk` on an M-series Mac vs several
minutes cold).

Not covered by ccache: the RV32 guest libraries (each `.so` is a single
compile+link clang invocation, which ccache cannot cache) and Rust.

## Rust cart builds (sccache)

`blyt build` sets `RUSTC_WRAPPER=sccache` for cart cargo invocations when
sccache is on PATH (`brew install sccache`; `BLYT_SCCACHE=<path>` to point at
a specific binary, `BLYT_SCCACHE=off` to disable, and a pre-set
`RUSTC_WRAPPER` always wins). The win is the `-Z build-std` recompile of
core/alloc — those units' inputs are machine-global (rust-src under
`~/.rustup`, fixed cart RUSTFLAGS), so after one cart build anywhere on the
machine, every other cart, checkout, and worktree replays them from cache
(≈7 s → ≈2 s for a from-scratch `examples/hello-rust` build).

Each cart keeps its own private cargo target dir. Do **not** be tempted to
share one `--target-dir` across carts instead: cargo's unit hash collides for
packages with the same name and version at different paths, so a second cart
silently reuses the first cart's compiled artifact without ever reading the
second cart's sources (verified on the pinned nightly).

## Sharing the cache across git worktrees

By default ccache includes the compile's working directory in the hash for
debug (`-g`) compiles and hashes include paths as given, so a fresh worktree
misses the cache populated from another checkout. Two settings fix that
(`ccache --set-config=<key>=<value>` writes them to the user config file):

```ini
# Common ancestor of the main checkout and its worktrees, e.g. for
# ~/code/blyt + ~/code/blyt-worktrees/<branch> use:
base_dir = /Users/<you>/code

# emcc is a Python wrapper: its mtime/size don't change when emsdk swaps the
# underlying clang. Hash the version output instead so emsdk upgrades
# invalidate correctly. (Also fine for clang/gcc.)
compiler_check = %compiler% --version
```

`base_dir` makes ccache rewrite absolute paths below it to relative ones
before hashing, so worktree A's compiles hit from worktree B. The
configure-time-generated musl `bits/` headers land in each build tree but are
content-identical across worktrees, so they still hit in ccache's direct
mode.

**Leave `hash_dir` at its default (`true`).** This intentionally limits
cross-worktree sharing to non-`-g` objects (release guest libs, MinSizeRel
libc++, release players, WASM): a debug object compiled in worktree A embeds
A's paths in `DW_AT_comp_dir`/DWARF, and reusing it in worktree B would hand
the GDB/DAP integration tests (which resolve source paths from DWARF) paths
into the wrong checkout. Debug compiles still cache fully *within* each
worktree.

Rejected alternative, recorded for the future: `-ffile-prefix-map=<root>=.`
plus `hash_dir = false` would share debug objects across worktrees too, but
requires teaching the gdb stub, DAP, and lldb-dap tests (and VS Code
debugging) about source-path substitution. Revisit only if debug-build
worktree time is still a problem.

## Generated headers that shadow one on a later `-I` path

ccache's direct mode records the exact set of headers a translation unit
resolved during its **first** compile. If you later introduce a *generated*
header that shadows an existing one further down the `-I` search path, ccache
does not notice: it re-hashes only the headers already in its manifest (still
the old, shadowed one), so a rebuild returns a **stale object** compiled as if
the shadow did not exist. It never re-preprocesses to discover the new file.
Clean builds and CI are unaffected — the shadow exists before the first
compile there — so this bites only incremental dev trees that already cached
the pre-shadow objects (blyt#229; blyt#225 Phase B lost hours to a stale
`frexpl.c.o` that self-recursed into a stack overflow).

The fix is to name the shadow header on the compile command line with
`-include <generated-header>` (see `cmake/blyt_hostlua_vm.cmake`, which does
this for the generated `bits/float.h`). Adding the flag changes direct mode's
primary hash — busting any stale entry — and the header is then recorded and
re-hashed as a real dependency on every build. Give the header an include
guard so the later `#include` is a no-op and the behaviour is identical to the
`-I` shadow alone. The last-resort manual recovery, if you hit a stale object
before wiring this up, is `rm -rf` the affected object dir and rebuild with
`CCACHE_RECACHE=1` (or `ccache -C` to clear the whole cache).

## Inspecting

```sh
ccache -s   # hit/miss statistics (ccache -z to zero them)
ccache -p   # effective configuration and where each value came from
```

The cache lives in `~/Library/Caches/ccache` (macOS) or `~/.cache/ccache`
(Linux); the default 5 GiB `max_size` comfortably fits many full blyt builds
(one full build+sdk stores ≈15 MiB of compressed objects).
