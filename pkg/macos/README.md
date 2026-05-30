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
    dylibbundler create-dmg \
    librsvg
```

`librsvg` ships `rsvg-convert`, used by `build-app.sh` to rasterize the
iconset entries from the project SVG.

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
  raw formats listed in `pkg/metadata/io.github.octalium.aperture.xml`.
- `meson.build` — installs the configured `Info.plist` to
  `$prefix/share/aperture/macos/Info.plist`. Active only when
  `host_machine.system() == 'darwin'`.
- `build-app.sh` — stages a `meson install`, lays out the
  `Aperture.app/Contents/{MacOS,Frameworks,Resources}/` tree, renders a
  complete `.icns` (all standard sizes + @2x) from the project SVG via
  `rsvg-convert` + `iconutil`, runs `dylibbundler` to rewrite brew
  dylib paths to `@executable_path/../Frameworks/`, and bundles
  MoltenVK's ICD JSON next to `libMoltenVK.dylib`.
- `build-dmg.sh` — prefers `create-dmg` with a drag-to-Applications
  layout. Falls back to `hdiutil` (plain UDZO image) when `create-dmg`
  is unavailable.

## Distribution

- Channel: `.dmg` attached to the GitHub Release.
- Code-signing: **none**. First-run users see the Gatekeeper
  "unidentified developer" warning and right-click → Open to launch.
  Revisit when there's appetite for an Apple Developer ID ($99/year).
- Notarization: out of scope for the same reason.

## Auto-update: notify-only

The app's version-check infrastructure (`src/update/check.{c,h}`)
detects new releases on the GitHub Releases API and surfaces an in-app
update banner when one is available. Clicking the banner opens the
GitHub Releases page in the system browser — same model as Flatpak
builds. There is no embedded auto-updater, no signing keys, no
operator setup.

The download + install flow is the user's responsibility: grab the new
`.dmg`, drag it to `/Applications`, replacing the old `Aperture.app`.
