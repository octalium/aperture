# Contributing to aperture

Thanks for your interest. This document covers the local dev loop, branch
policy, and commit-message conventions.

## Local dev loop

aperture is C + Vulkan + Dear ImGui, built with [Meson](https://mesonbuild.com/).

### System dependencies

See [README.md](README.md#distro-dependencies) for per-distro package
lists. Vendoring policy (which deps are submodules, which are wraps,
which come from the system) is documented in [`dep/README.md`](dep/README.md).

### After clone

Most vendored deps under `dep/` are git submodules. Either clone with
`git clone --recursive`, or — for an existing clone — run:

```
git submodule update --init --recursive
```

`--recursive` is required: `dep/cimgui/upstream/` itself has Dear ImGui
as a nested submodule.

### Build + run

```
meson setup build
meson compile -C build
./build/aperture
```

`make build` is an equivalent shortcut.

On Windows, run from a Developer PowerShell for VS 2022 and use the
vcpkg-aware setup documented in
[`packaging/windows/README.md`](packaging/windows/README.md). `make
windows` wraps the full configure + compile + WiX MSI build in one
step (requires `dotnet tool install --global wix` and the LunarG
[Vulkan SDK](https://vulkan.lunarg.com/sdk/home#windows); `setup-deps.ps1`
fetches the SDK silently if it isn't already on the machine).

For a release build with LTO, stripping, and reproducibility flags:

```
SOURCE_DATE_EPOCH=$(git log -1 --pretty=%ct) \
    meson setup build-release \
        --buildtype=release \
        -Db_lto=true \
        -Dstrip=true
meson compile -C build-release
```

`-ffile-prefix-map` is wired in globally so the absolute build-root
path is rewritten to a stable prefix in embedded debug info (covers
DWARF `DW_AT_comp_dir` from both aperture and its wrap subprojects).
Source paths are already relative under ninja so no source-root remap
is needed. `SOURCE_DATE_EPOCH` pins the compiler's `__DATE__` /
`__TIME__` macros — set it (e.g. to a commit timestamp) for
reproducible builds.

## Branch policy

aperture uses a Git-Flow-flavored model with two long-lived branches:

- `main` — stable, tagged releases land here (dev → main merge on each release)
- `dev` — integration branch; all feature and fix work merges here first

Topic branches branch **off `dev`** regardless of kind:

- `feat/<issue>-<slug>` for features and chores
- `fix/<issue>-<slug>` for bug fixes

Open a PR back into `dev` and link the issue with `Closes #N`. Merges
preserve branching history (no fast-forward, no rebase).

## Commits

Follow [Conventional Commits](https://www.conventionalcommits.org/):

```
<type>(<scope>): <subject>
```

Common types: `feat`, `fix`, `perf`, `refactor`, `chore`, `docs`, `test`.
Scope is the affected subsystem (`import`, `ui`, `gpu`, `lens`, ...).
Keep commits small and self-contained; commit early and often.

Do not include `Co-Authored-By` trailers.

## Issues

File an issue before non-trivial work and link the PR to it. Issues
should be concise and scoped. Hierarchical relationships (epics,
dependencies) are tracked with GitHub's native linking.

## Tests

Tests are plain `int main` binaries wired via meson's `test()` rule.
No framework, no DSL — just `AP_TEST_ASSERT` from `test/include/aptest.h`.
Run the suite with `make test` (or `meson test -C build` directly).

To add a new test:

1. Create `test/<area>/<thing>_test.c` next to its peers. Mirror the
   `src/` layout so the test sits beside the code under test.
2. Compile the source under test directly into the test executable —
   tests are tiny and standalone, not linked against the full binary.
3. Add (or extend) a `test/<area>/meson.build`:

```meson
my_thing_test = executable(
  'my_thing_test',
  'my_thing_test.c',
  files('../../src/<area>/my_thing.c'),
  include_directories: [aperture_inc, aptest_inc],
)
test('<area>/my_thing', my_thing_test)
```

4. If `test/<area>/` is new, `subdir('<area>')` it from `test/meson.build`.

Inside the test, use `AP_TEST_ASSERT(cond, fmt, ...)` for failure
reporting — it prints `file:line` + the formatted message and aborts:

```c
#include "aptest.h"
#include "io/my_thing.h"

int main(void) {
    my_thing_t t;
    AP_TEST_ASSERT(my_thing_init(&t) == 0, "init failed");
    return 0;
}
```

Prefer synthesized in-memory fixtures over checked-in binary files
(see `test/io/exif_test.c` for a worked example: TIFF and CR3 blobs
built byte-by-byte in test code).

## Code style

- Documentation is required on all public entities. Keep it brief and
  descriptive.
- Comments are single-line and lowercase where they add clarity. Avoid
  sectional or separatory comments.
