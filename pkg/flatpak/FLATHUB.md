# Submitting aperture to Flathub

A manual, one-time step performed by the project owner. This records the
procedure for redo or audit. Day-to-day Flatpak builds (testing, GitHub
Release) run from the manifest at
`pkg/flatpak/io.github.octalium.aperture.yml` via the `linux-flatpak` job
in `.github/workflows/release-linux.yml`.

## Prerequisites

- A GitHub account controlling the upstream repo (`octalium/aperture`).
- The manifest in this directory passing `flatpak-builder` locally.
- `flatpak`, `flatpak-builder`, and `git` installed.

## Local pre-flight

Build the manifest end-to-end first, to catch hash drift or
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
    build-dir pkg/flatpak/io.github.octalium.aperture.yml

flatpak build-bundle repo aperture.flatpak io.github.octalium.aperture
flatpak --user install -y --reinstall aperture.flatpak
flatpak run io.github.octalium.aperture
```

The app should reach the splash / empty-library state. Quit, then remove
the test install:

```
flatpak --user uninstall -y io.github.octalium.aperture
```

## Submitting

1. Fork [`flathub/flathub`](https://github.com/flathub/flathub).
2. Clone the fork and branch on the app id:
   ```
   git checkout -b new-pr/io.github.octalium.aperture
   ```
3. Copy the manifest into the fork root (submissions live on a per-app
   branch in `flathub/flathub`'s `new-pr` namespace; see
   [docs.flathub.org/docs/for-app-authors/submission](https://docs.flathub.org/docs/for-app-authors/submission)
   for the current layout — conventions evolve).
4. Open a PR against `flathub/flathub` from that branch.
5. Reviewers run their own lint and request changes. Edit the manifest
   in this repo first, then mirror into the Flathub PR — the canonical
   manifest stays here so the workflow stays in sync.
6. On merge, Flathub builds and publishes at
   `https://flathub.org/apps/io.github.octalium.aperture`.

## After submission

- The Flathub build bot rebuilds on each commit to the per-app branch.
  Publish a new version by pushing a manifest update there that bumps the
  `aperture` module's `tag:`.
- The `.flatpak` bundle in this repo's GH Releases on `v*` tags is a
  sideload artifact. Flathub publication is a separate flow against the
  Flathub repo (auto-promotion can be wired up later — see flathub.org).

## Notes on the manifest

- Runtime: the freedesktop platform (`24.08` LTS as of writing). Bump in
  lockstep with Flathub guidance on each new LTS.
- System deps come from the runtime: Vulkan loader, libpng, libtiff, and
  the common desktop stack. The only dep built as a separate module is
  `lensfun`.
- Vendored submodules under `dep/<name>/upstream/` (cimgui, lcms2, cJSON,
  nativefiledialog, tomlc99, glfw, libraw, libjpeg-turbo, vulkan-headers,
  mbedtls) build via meson wraps with `--wrap-mode=nodownload`; their wrap
  sources are pinned as `sources:` entries on the aperture module for
  offline builds. In-tree copies (blake3, sqlite3) carry no `sources:`
  entry.
- The `aperture` module's `sources:` defaults to `branch: main`. CI
  rewrites it to `tag: v<version>` before building, for a bit-for-bit
  reproducible release.
