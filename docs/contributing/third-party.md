# Third-party dependency management

blyt uses CMake `FetchContent` to download third-party dependencies as versioned
tarballs at configure time. No git submodules; no `git submodule update --init`.
Dependencies land in a shared cache (`~/.cache/blyt/fetchcontent/`) across all
worktrees.

## Dependency table

| Dep | Source | Tag convention |
|---|---|---|
| lua | upstream (github.com/lua/lua) | `v5.4.7` (release tag) |
| flatcc | upstream (github.com/dvidelabs/flatcc) | `v0.6.1` (release tag) |
| zstd | upstream (github.com/facebook/zstd) | `v1.5.7` (release tag) |
| libopenmpt | upstream (github.com/OpenMPT/openmpt) | `libopenmpt-0.8.7` (release tag) |
| libretro-common | upstream (github.com/libretro/libretro-common) | pinned commit SHA (no upstream tags) |
| rv32emu | blyt-tech/rv32emu | `g<upstream-sha>-blyt-v0-p<N>` (custom tarball with softfloat) |
| musl | blyt-tech/musl | `v<upstream>-blyt-v0-p<N>` (e.g. `v1.2.6-blyt-v0-p1`) |
| libcxx | blyt-tech/llvm-project | `v<upstream>-blyt-v0-p<N>` (curated tarball, libcxx+libcxxabi only) |

## Fork conventions

### Branch naming

Long-lived patch branch: `blyt-patches-v0`, `blyt-patches-v1`, … — one branch
per blyt major version, based on an upstream release commit.

### Stable tag naming

`v<upstream-version>-blyt-v<N>-p<M>`

- `<upstream-version>`: upstream release tag (e.g. `1.2.6`, `22.1.5`) or
  short commit SHA for upstream repos with no tags (e.g. `g044cdb7` for rv32emu)
- `<N>`: blyt major version (mirrors `blyt-patches-v<N>` branch number)
- `<M>`: patch counter, incremented with each new blyt patch set on that base

Examples: `v1.2.6-blyt-v0-p1`, `v22.1.5-blyt-v0-p5`, `g044cdb7-blyt-v0-p3`

### Pre-release tag naming (in-flight development)

`v<upstream-version>-blyt-v<N>-p<M>-<feature>`

Cut from a feature branch on the fork during in-progress patch work. Used in
the corresponding blyt feature branch so CI can run against the work-in-progress
patches. **Deleted** after the blyt feature branch merges and the patches
stabilise to a permanent tag.

## Patch workflow

### Local development

To work on blyt-tech fork patches:

```sh
# Clone the fork into third_party/ (gitignored)
git clone https://github.com/blyt-tech/musl third_party/musl
git -C third_party/musl checkout blyt-patches-v0

# cmake auto-detects the local checkout at configure time — no flags needed:
cmake -B build -G Ninja
# Build output: cmake status line "FetchContent musl: using local third_party/musl/"
```

Delete `third_party/<dep>` to revert to the downloaded tarball.

### When CI needs to see in-progress patches

1. Cut a pre-release tag on the fork:
   ```sh
   git -C third_party/<dep> tag v1.2.6-blyt-v0-p2-mypatch
   git -C third_party/<dep> push origin v1.2.6-blyt-v0-p2-mypatch
   ```
2. Update the URL + hash in the blyt feature branch's `CMakeLists.txt`.
3. Push the blyt branch — CI runs against the pre-release tarball.

### Pre-merge gate

Before merging a blyt PR that touches fork deps:

1. Rebase the fork's feature commits onto `blyt-patches-v0`.
2. Run the appropriate release script to create a stable (non-feature) tag
   and upload the tarball:
   ```sh
   bash scripts/release-dep.sh musl v1.2.6-blyt-v0-p2
   # or
   bash scripts/release-rv32emu.sh g<sha>-blyt-v0-p4
   # or
   bash scripts/release-libcxx.sh v22.1.5-blyt-v0-p6
   ```
3. Update `CMakeLists.txt` in the blyt PR with the new URL + SHA256.
4. CI must pass on the stable tag before merge.

Main always points at a stable tag. Pre-release tags are ephemeral and
removed after the feature lands.

### Post-merge cleanup

After the blyt PR merges and CI on main is green, delete the pre-release
release and tag from each affected fork:

```sh
gh release delete v1.2.6-blyt-v0-p2-mypatch --repo blyt-tech/musl --yes --cleanup-tag
# or
gh release delete g<sha>-blyt-v0-p4-mypatch --repo blyt-tech/rv32emu --yes --cleanup-tag
```

`--cleanup-tag` deletes the tag along with the release. For rv32emu this
also removes the uploaded tarball asset. For deps that use GitHub's
auto-generated tarball (musl, lua) there is no asset to remove, but the
release entry and tag are still cleaned up the same way.

## Custom tarballs

Two deps need custom tarballs because GitHub's auto-generated archive is
insufficient:

### rv32emu

`src/softfloat` is a nested git submodule — GitHub's tarball omits it.
`scripts/release-rv32emu.sh` embeds softfloat inline and uploads the result
as a GitHub release asset on `blyt-tech/rv32emu`.

### libcxx

The full `llvm-project` tarball is hundreds of MB; only `libcxx/` and
`libcxxabi/` subtrees (plus `runtimes/`, `cmake/`, and partial `libc/`) are
needed. `scripts/release-libcxx.sh` extracts these and uploads the curated
tarball as a release asset on `blyt-tech/llvm-project`.

### musl and upstream deps

musl uses GitHub's auto-generated tarball via `scripts/release-dep.sh`.
Upstream deps (lua, flatcc, zstd, libopenmpt, libretro-common) use the
auto-generated tarballs directly — no release script needed.

## Release workflows

Three `workflow_dispatch` workflows in `.github/workflows/` wrap the scripts
for triggered releases:

- `release-dep.yml` — for musl (and any future standard fork dep)
- `release-rv32emu.yml` — for the custom rv32emu tarball
- `release-libcxx.yml` — for the curated libcxx tarball

Trigger from the GitHub Actions UI or via:
```sh
gh workflow run release-rv32emu.yml --field tag=g044cdb7-blyt-v0-p4
```

## FetchContent cache

`FETCHCONTENT_BASE_DIR` defaults to `~/.cache/blyt/fetchcontent/`, shared
across all local worktrees. CI uses an `actions/cache` entry on the same path,
keyed on the SHA256 of `CMakeLists.txt`.

The `test-linux-docker` target mounts a `blyt-fetchcontent-cache-<arch>`
Docker volume at `/root/.cache/blyt/fetchcontent` so the container build
also caches downloads across runs.

## Bundled assets (in runtime binary)

Unlike the build-time dependencies above, these are runtime-shipped assets
(ADR-0042) hand-authored directly into `runtime/shared/` — not fetched, not
built from an external source tree.

### Palettes (issue #201)

- **aurora** (`palette_default` / `BLYT_PALETTE_AURORA` / `BLYT_PALETTE_DEFAULT`)
  — DawnBringer "Aurora" (256 colors), sourced verbatim from its canonical
  listing on [Lospec](https://lospec.com/palette-list/aurora). Aurora has no
  explicit license anywhere authoritative (Lospec states none, Ettinger's hex
  gist has none, DawnBringer has no written grant even for DB16/32). Included
  **provisionally**: a bare list of RGB triples is not independently
  copyrightable, and the palette is attributed here to its author (Ettinger,
  "DawnBringer"). This is a risk-accepted decision, not a cleared license —
  revisit before a 1.0 release (author blyt's own CC0 palette, or switch to an
  explicitly CC0 source, if the risk calculus changes).
- **vga** (`BLYT_PALETTE_VGA`) — the standard VGA 256-color default DAC
  palette: 16 EGA colors + a 16-step grayscale ramp + a 216-color HSV cube.
  Public-domain PC hardware standard; `runtime/shared/blyt_palettes.c`'s array
  is a from-scratch reconstruction of the documented construction, not a copy
  of any third-party source file.
- **ega** (`BLYT_PALETTE_EGA`) — the standard 16-color EGA palette. Public
  domain PC hardware standard.
- **cga** (`BLYT_PALETTE_CGA`) — CGA palette 1, high intensity
  (black/cyan/magenta/white). Public domain PC hardware standard.
