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

metainfo_conf = configuration_data()
metainfo_conf.set('version', meson.project_version())
metainfo_conf.set('release_date', run_command(
  'date', '-u', '+%Y-%m-%d', check: true
).stdout().strip())

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
