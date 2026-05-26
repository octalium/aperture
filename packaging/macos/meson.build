app_id = 'io.github.octalium.aperture'

datadir = get_option('prefix') / get_option('datadir')

# Info.plist is configured with the project version + the Sparkle EdDSA
# public key so the bundle script has a single source of truth to copy
# into Aperture.app/Contents/. The public key arrives via the
# SPARKLE_PUBLIC_ED_KEY environment variable; when absent we substitute
# an obviously-invalid placeholder so unsigned local builds are still
# usable but in-app updates refuse to apply (Sparkle rejects updates
# whose signatures don't verify against the embedded key).
info_plist_conf = configuration_data()
info_plist_conf.set('version', meson.project_version())
sparkle_pub_key = run_command(
  ['sh', '-c', 'printf %s "${SPARKLE_PUBLIC_ED_KEY:-PLACEHOLDER_UNSIGNED_BUILD}"'],
  check: true,
).stdout()
info_plist_conf.set('sparkle_public_ed_key', sparkle_pub_key)

configure_file(
  input: 'Info.plist.in',
  output: 'Info.plist',
  configuration: info_plist_conf,
  install: true,
  install_dir: datadir / 'aperture' / 'macos',
)
