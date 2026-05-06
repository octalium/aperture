project(
  'aperture',
  ['c', 'cpp'],
  version: '0.0.1',
  default_options: [
    'c_std=c17',
    'cpp_std=c++17',
    'warning_level=2',
    'b_ndebug=if-release',
  ],
  meson_version: '>=1.3.0',
  subproject_dir: 'dep',
)

add_project_arguments(
  '-DAPERTURE_VERSION="@0@"'.format(meson.project_version()),
  language: ['c', 'cpp'],
)

vulkan_dep = dependency('vulkan')
glfw_dep   = dependency('glfw3')
cimgui_dep = dependency('cimgui')
libraw_dep = dependency('libraw')

aperture_deps = [
  vulkan_dep,
  glfw_dep,
  cimgui_dep,
  libraw_dep,
]

aperture_inc = include_directories('src')
aperture_sources = []

subdir('src')

aperture_exe = executable(
  'aperture',
  aperture_sources,
  include_directories: aperture_inc,
  dependencies: aperture_deps,
  install: true,
)
