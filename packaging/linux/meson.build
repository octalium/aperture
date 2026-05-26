app_id = 'io.github.octalium.aperture'

datadir = get_option('prefix') / get_option('datadir')

desktop_conf = configuration_data()
desktop_conf.set('bindir', get_option('prefix') / get_option('bindir'))

desktop_file = configure_file(
  input: app_id + '.desktop.in',
  output: app_id + '.desktop',
  configuration: desktop_conf,
  install: true,
  install_dir: datadir / 'applications',
)

# release date is hardcoded per-version; SOURCE_DATE_EPOCH overrides it
# when set, keeping the generated metainfo reproducible.
release_date = run_command(
  find_program('python3'),
  files('release-date.py'),
  '2026-05-24',
  check: true,
).stdout().strip()

metainfo_conf = configuration_data()
metainfo_conf.set('version', meson.project_version())
metainfo_conf.set('release_date', release_date)

metainfo_file = configure_file(
  input: app_id + '.metainfo.xml.in',
  output: app_id + '.metainfo.xml',
  configuration: metainfo_conf,
  install: true,
  install_dir: datadir / 'metainfo',
)

install_data(
  app_id + '.xml',
  install_dir: datadir / 'mime' / 'packages',
)

install_data(
  'icons' / 'hicolor' / 'scalable' / 'apps' / app_id + '.svg',
  install_dir: datadir / 'icons' / 'hicolor' / 'scalable' / 'apps',
)

# rasterized hicolor sizes for desktops/launchers that ignore SVG entries
# (notably KDE Plasma's task manager and several GTK launchers). vendored
# from the scalable SVG; regenerate with packaging/linux/icons/refresh.sh
# when the SVG changes.
foreach size : [16, 32, 48, 64, 128, 256, 512]
  size_str = '@0@x@0@'.format(size)
  install_data(
    'icons' / 'hicolor' / size_str / 'apps' / app_id + '.png',
    install_dir: datadir / 'icons' / 'hicolor' / size_str / 'apps',
    install_tag: 'icons',
  )
endforeach

desktop_file_validate = find_program('desktop-file-validate', required: false)
if desktop_file_validate.found()
  test(
    'validate-desktop-file',
    desktop_file_validate,
    args: [desktop_file],
    suite: 'packaging',
  )
endif

appstreamcli = find_program('appstreamcli', required: false)
if appstreamcli.found()
  test(
    'validate-metainfo',
    appstreamcli,
    args: ['validate', '--no-net', '--pedantic', metainfo_file],
    suite: 'packaging',
  )
endif

meson.add_install_script('post-install.sh')
