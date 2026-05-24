# External dependencies

Vendored dependencies live here as Meson wrap files (`*.wrap`). When a
wrap is fetched, Meson populates the matching subdirectory; those
subdirectories are gitignored.

Configured as the project's `subproject_dir` in the root `meson.build`,
overriding Meson's default `subprojects/` location.

## Vendoring policy

| Dependency                  | Mode             | Pinning              | Rationale                                            |
| --------------------------- | ---------------- | -------------------- | ---------------------------------------------------- |
| lcms2                       | force-wrap       | git tag `lcms2.19`   | colour engine version must be deterministic          |
| blake3                      | force-wrap       | wrap-file checksum   | hash-stability across machines                       |
| cimgui                      | fallback         | git SHA `07fde25e` on `docking_inter` (no upstream tags) | docking branch required; pinned for reproducibility   |
| libpng                      | fallback         | wrapdb `1.6.58-1`    | system pkg preferred; wrap covers distros that lack it |
| libtiff                     | fallback         | wrapdb `4.7.1-4`     | same as libpng                                       |
| tomlc99                     | fallback         | git SHA `29076dfd` (no upstream tags) | small lib, rarely packaged; pinned for reproducibility |
| nativefiledialog-extended   | fallback         | git tag `v1.3.0`     | rarely packaged                                      |

Dependencies without a wrap are taken from the system unconditionally:
vulkan, glfw3, libraw, lensfun, sqlite3, libjpeg, threads. Adding a
wrap for any of these is on the table if portable-release packaging
demands it.

## Modes

- **force-wrap** — declared via `subproject('name')` in the root
  `meson.build`. The wrap is built even if the system has a copy.
- **fallback** — declared via `dependency('name')`. Meson picks the
  system package when present and falls back to the wrap otherwise.

## Adding a wrap

1. Drop a `<name>.wrap` here (prefer wrapdb-versioned entries with
   checksums; fall back to `wrap-git` with a pinned tag).
2. If the upstream build doesn't already expose the dep, add an overlay
   under `packagefiles/<name>/`.
3. Reference it from the root `meson.build` — `dependency()` for
   system-preferred deps, `subproject()` for force-wrapped ones.

Document any deviation from "prefer system, fall back to wrap" in the
table above.
