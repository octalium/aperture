project(
  'aperture',
  'c',
  version: '0.0.1',
  default_options: [
    'c_std=c17',
    'warning_level=2',
    'b_ndebug=if-release',
  ],
  meson_version: '>=1.3.0',
  subproject_dir: 'dep',
)

add_project_arguments(
  '-DAPERTURE_VERSION="@0@"'.format(meson.project_version()),
  language: 'c',
)

aperture_inc = include_directories('src')
aperture_sources = []

subdir('src')

aperture_exe = executable(
  'aperture',
  aperture_sources,
  include_directories: aperture_inc,
  install: true,
)
