app_id = 'io.github.octalium.aperture'

datadir = get_option('prefix') / get_option('datadir')

# Info.plist is configured with the project version so the bundle script
# has a single source of truth to copy into Aperture.app/Contents/.
info_plist_conf = configuration_data()
info_plist_conf.set('version', meson.project_version())

configure_file(
  input: 'Info.plist.in',
  output: 'Info.plist',
  configuration: info_plist_conf,
  install: true,
  install_dir: datadir / 'aperture' / 'macos',
)
