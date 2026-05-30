# Submitting aperture to Flathub

The Flathub submission is a manual, one-time step performed by the
project owner. This document records the procedure so it can be redone
or audited later. Day-to-day Flatpak builds (for testing or for the
GitHub Release) are handled by the manifest at
`packaging/flatpak/io.github.octalium.aperture.yml` and the
`linux-flatpak` job in `.github/workflows/release-linux.yml`.

## Prerequisites

- A GitHub account that controls the upstream repo (`octalium/aperture`).
- The manifest in this directory passing `flatpak-builder` locally.
- `flatpak`, `flatpak-builder`, and `git` installed locally.

## Local pre-flight

Run the manifest end-to-end before submitting to catch hash drift or
network-sandbox issues:

```
flatpak --user remote-add --if-not-exists \
    flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak --user install -y flathub \
    org.freedesktop.Platform//24.08 \
    org.freedesktop.Sdk//24.08

flatpak-builder \
    --user \
    --install-deps-from=flathub \
    --force-clean \
    --repo=repo \
    build-dir packaging/flatpak/io.github.octalium.aperture.yml

flatpak build-bundle repo aperture.flatpak io.github.octalium.aperture
flatpak --user install -y --reinstall aperture.flatpak
flatpak run io.github.octalium.aperture
```

The app should reach the splash / empty-library state. Quit, then
remove the test install:

```
flatpak --user uninstall -y io.github.octalium.aperture
```

## Submitting

1. Fork [`flathub/flathub`](https://github.com/flathub/flathub) on GitHub.
2. Clone the fork and create a branch named after the app id:
   ```
   git checkout -b new-pr/io.github.octalium.aperture
   ```
3. Copy the manifest into the fork (the Flathub submission lives in the
   root of a per-app branch on the `flathub/flathub` repo's `new-pr`
   namespace; see
   [docs.flathub.org/docs/for-app-authors/submission](https://docs.flathub.org/docs/for-app-authors/submission)
   for the current layout — submission conventions evolve).
4. Open a pull request against `flathub/flathub` from that branch.
5. The Flathub reviewers run their own lint and respond with required
   changes. Iterate on the manifest in this repo first, then mirror the
   change into the Flathub PR — keep the canonical manifest here so the
   GitHub Actions workflow stays in sync.
6. Once merged, Flathub builds and publishes the app at
   `https://flathub.org/apps/io.github.octalium.aperture`.

## After submission

- The Flathub build bot rebuilds on each commit to the Flathub repo's
  per-app branch. To publish a new aperture version, push a manifest
  update there that bumps the `tag:` for the `aperture` module.
- The `.flatpak` bundle published in this repo's GH Releases on `v*`
  tags is a sideload artifact; Flathub publication is a separate
  manual or automated flow against the Flathub repo (the
  auto-promotion bot can be wired up later — see flathub.org docs).

## Notes on the manifest

- Runtime is the freedesktop platform (`24.08` LTS as of writing). Bump
  in lockstep with Flathub guidance when a new LTS is cut.
- System deps come from the runtime: Vulkan loader, libpng, libtiff,
  libjpeg, sqlite3. Deps the runtime lacks (glfw, lensfun, libraw) are
  built as separate modules. Vendored deps that require a fetch
  (cimgui, lcms2, cJSON, nativefiledialog, tomlc99) come in through
  meson wraps under `dep/` with `--wrap-mode=nodownload`, and the
  wrap sources are pinned as `sources:` entries alongside the aperture
  module so flatpak-builder can build offline. In-tree wraps (blake3)
  need no `sources:` entry.
- The `aperture` module's `sources:` block defaults to `branch: main`.
  CI rewrites it to `tag: v<version>` before building so the release
  build is bit-for-bit reproducible against the tag.
