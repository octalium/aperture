# macOS packaging

Produces an unsigned `Aperture.app` wrapped in a `.dmg`, distributed via
GitHub Releases on `v*` tag pushes.

## Targets

- Apple Silicon (arm64) only. A universal binary (Intel + ARM) is out
  of scope; file a follow-up if a user surfaces demand.
- Minimum system version: macOS 11 (Big Sur), set in `Info.plist`.

## Build-host requirements

The packaging scripts assume a macOS host with Homebrew. CI uses the
`macos-14` runner; locally:

```
brew install \
    meson ninja pkg-config \
    glslang shaderc \
    vulkan-headers vulkan-loader molten-vk \
    glfw libraw lensfun \
    jpeg-turbo libpng libtiff sqlite \
    dylibbundler create-dmg
```

## Local build

From the repository root:

```
make app     # Aperture.app only
make macos   # Aperture.app + aperture-<version>-arm64.dmg
```

Both targets refuse to run on non-Darwin hosts.

## Pieces

- `Info.plist.in` — configured with `meson.project_version()` at
  `meson install` time. Carries the bundle id
  (`io.github.octalium.aperture`) and `CFBundleDocumentTypes` for the
  raw formats listed in `packaging/linux/io.github.octalium.aperture.xml`.
- `meson.build` — installs the configured `Info.plist` to
  `$prefix/share/aperture/macos/Info.plist`. Active only when
  `host_machine.system() == 'darwin'`.
- `build-app.sh` — stages a `meson install`, lays out the
  `Aperture.app/Contents/{MacOS,Frameworks,Resources}/` tree, generates
  an `.icns` from the linux 256px raster via `sips` + `iconutil`, runs
  `dylibbundler` to rewrite brew dylib paths to
  `@executable_path/../Frameworks/`, and bundles MoltenVK's ICD JSON
  next to `libMoltenVK.dylib`.
- `build-dmg.sh` — prefers `create-dmg` with a drag-to-Applications
  layout. Falls back to `hdiutil` (plain UDZO image) when `create-dmg`
  is unavailable.
- `fetch-sparkle.sh` — downloads + extracts the pinned Sparkle release
  tarball into `.sparkle-cache/Sparkle-<version>/`. Prints the absolute
  path to `Sparkle.framework`. Invoked at meson configure time (so the
  link step finds the framework) and again from `build-app.sh` (to
  copy it into the bundle).
- `generate-appcast.sh` — calls Sparkle's `sign_update` with the
  release private key (read from `SPARKLE_PRIVATE_KEY` via stdin) and
  emits an `appcast.xml` referencing the signed `.dmg`. Used by the
  `release-macos` workflow.

## Distribution

- Channel: `.dmg` attached to the GitHub Release. Sparkle clients fetch
  the appcast from a stable URL on the `latest` release.
- Code-signing: **none**. First-run users see the Gatekeeper
  "unidentified developer" warning and right-click → Open to launch.
  Revisit when there's appetite for an Apple Developer ID ($99/year).
- Notarization: out of scope for the same reason.
- Auto-update: Sparkle 2.x, EdDSA-signed (independent of Apple Developer
  ID code-signing; see below).

## Auto-update: Sparkle signing keys (operator setup)

Sparkle 2.x verifies every downloaded update with an Ed25519 signature.
The public key is embedded in `Aperture.app/Contents/Info.plist` at
build time via the `SPARKLE_PUBLIC_ED_KEY` environment variable; the
matching private key signs each release in CI from the
`SPARKLE_PRIVATE_KEY` repository secret. Without these, every Sparkle
client refuses to apply updates — that is the intended fail-closed
behaviour.

Generate the keypair once, on a trusted local macOS machine:

```
./packaging/macos/fetch-sparkle.sh   # populates .sparkle-cache/Sparkle-<version>/
SPARKLE_BIN=packaging/macos/.sparkle-cache/Sparkle-*/bin
"$SPARKLE_BIN"/generate_keys
```

`generate_keys` stores the private key in the macOS keychain by default
and prints the public key to stdout. To export the private key for use
as a GitHub Actions secret:

```
"$SPARKLE_BIN"/generate_keys -x sparkle_ed25519.priv
base64 -i sparkle_ed25519.priv | pbcopy   # base64 for SPARKLE_PRIVATE_KEY
```

Then in the GitHub repository's *Settings → Secrets and variables →
Actions*:

| Secret name             | Value                                           |
|-------------------------|-------------------------------------------------|
| `SPARKLE_PUBLIC_ED_KEY` | the public key printed by `generate_keys`       |
| `SPARKLE_PRIVATE_KEY`   | base64 of `sparkle_ed25519.priv`, no newlines   |

After the first release on this keypair, **do not rotate or lose the
private key**. Rotating breaks signature verification for every
existing installation; the only recovery is to ship an out-of-band
update that bakes in a new public key.

The .gitignored `sparkle_ed25519.priv` file should never be committed.
Delete it from disk once the secret is registered; the keychain copy
remains for local verification.
