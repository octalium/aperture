# Contributing to aperture

Thanks for your interest. This document covers the local dev loop, branch
policy, and commit-message conventions. Scope and architectural direction
live in [SPEC.md](SPEC.md) — read that first if you intend to propose a
feature.

## Local dev loop

aperture is C + Vulkan + Dear ImGui, built with [Meson](https://mesonbuild.com/).

### System dependencies

See [README.md](README.md#distro-dependencies) for per-distro package
lists. Wrap policy (which deps are vendored, which are pulled from the
system) is documented in [`dep/README.md`](dep/README.md).

### Build + run

```
meson setup build
meson compile -C build
./build/aperture
```

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

- `main` — spec-only until v0; tagged releases land here once development
  begins shipping
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

## Code style

- Documentation is required on all public entities. Keep it brief and
  descriptive.
- Comments are single-line and lowercase where they add clarity. Avoid
  sectional or separatory comments.
- See [SPEC.md](SPEC.md) for the architectural stance — in particular
  the module-registry convention and the "no workarounds" rule.
