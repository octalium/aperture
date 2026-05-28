# External dependencies

Vendored dependencies live here in one of three shapes:

- **git submodule** — upstream cloned under `dep/<name>/`, pinned to a
  specific commit. Most deps. Build description is our own overlay in
  `dep/packagefiles/<name>/meson.build` when the upstream doesn't ship
  meson; the submodule's source tree is read-only from our perspective.
  The overlay is symlinked into the submodule worktree as
  `dep/<name>/meson.build` (untracked) by the `dep-overlays` Makefile
  target so meson finds it at the conventional subproject path.
- **wrapdb wrap** — `<name>.wrap` here; meson fetches the source tarball
  and a wrapdb-maintained build patch on demand. Used where the wrapdb
  overlay is mature and writing our own would be net-burden for no gain
  (libpng, libtiff). The fetched directories (`libpng-*/`, `tiff-*/`)
  are gitignored.
- **in-tree direct copy** — sources committed directly under `dep/<name>/`
  alongside a hand-written `meson.build`. Used where the upstream layout
  doesn't fit either of the above (blake3).

Configured as the project's `subproject_dir` in the root `meson.build`,
overriding Meson's default `subprojects/` location.

## Vendoring policy

| Dependency                  | Shape                | Pin                                                       | Rationale |
| --------------------------- | -------------------- | --------------------------------------------------------- | --------- |
| cimgui                      | submodule (fallback) | git SHA `07fde25e` (cimgui/cimgui upstream)               | docking branch required; pinned for reproducibility |
| tomlc99                     | submodule (fallback) | git SHA `29076dfd` (no upstream tags)                     | small lib, rarely packaged; pinned for reproducibility |
| lcms2                       | submodule (force)    | git tag `lcms2.19`                                        | colour engine version must be deterministic |
| cJSON                       | submodule (force)    | git tag `v1.7.19`                                         | small lib, force-vendored for version determinism |
| nativefiledialog-extended   | submodule (fallback) | git tag `v1.3.0`                                          | rarely packaged |
| libpng                      | wrapdb wrap (fallback) | wrapdb `1.6.58-1`                                       | upstream ships only autotools + CMake; wrapdb's meson overlay is mature, writing our own would be net-burden |
| libtiff                     | wrapdb wrap (fallback) | wrapdb `4.7.1-4`                                        | same as libpng |
| blake3                      | in-tree (force)      | derived from BLAKE3-team/BLAKE3 — hand-rolled two-file portable rewrite | upstream's multi-file SIMD layout doesn't fit a thin overlay; the two-file portable copy is small, owned by us, and load-bearing in import-time hashing |

Dependencies without a wrap or submodule are taken from the system
unconditionally: vulkan, glfw3, libraw, lensfun, sqlite3, libjpeg,
threads. Adding a submodule for any of these is on the table if
portable-release packaging demands it (tracked under #520, phase 2).

## Modes

- **force** — declared via `subproject('name')` in the root
  `meson.build`. Built from the vendored source even if the system has
  a copy.
- **fallback** — declared via `dependency('name')`. Meson picks the
  system package when present and falls back to the vendored source
  otherwise.

## After clone

The submodules need to be initialised:

```
git submodule update --init --recursive
```

`--recursive` is required: `dep/cimgui` has Dear ImGui as a nested
submodule.

## Adding a submodule

1. `git submodule add <upstream-url> dep/<name>` and check out the
   intended pin: `(cd dep/<name> && git checkout <ref>)`.
2. If the upstream build doesn't already expose the dep, add an overlay
   `dep/packagefiles/<name>/meson.build`. The overlay must reference
   the source files at the paths they live in within the submodule.
3. Add `<name>` to the `DEP_OVERLAYS` list in the root `Makefile` so
   the symlink staging picks it up.
4. Reference it from the root `meson.build` — `dependency()` for
   fallback deps, `subproject()` for force-vendored ones. If the
   exposed dep name differs from the subproject directory name, add an
   explicit `fallback:` argument (see `nativefiledialog-extended`).
5. Update the policy table above.

## Adding a wrap

Prefer a submodule. Reach for a wrap only when the upstream layout
makes that impractical — typically when a mature wrapdb overlay already
exists and writing our own would mean owning a fresh meson build
description for a large C library we don't otherwise touch.
