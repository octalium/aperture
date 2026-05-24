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

# cheap reproducibility: rewrite the build root in embedded debug info
# (DW_AT_comp_dir) so identical sources yield identical binaries
# regardless of where they were built. ninja already compiles aperture
# and its subprojects from the build dir with relative source paths
# (DW_AT_name like ../src/foo.c), so the remaining absolute leak is
# DW_AT_comp_dir. SOURCE_DATE_EPOCH (honoured by the compiler's
# __DATE__/__TIME__) pins macro-baked timestamps when callers set it.
#
# uses add_global_arguments so the flag reaches every wrap subproject
# too — otherwise their .o files retain the absolute build root and
# leak it into the final executable's debug info.
cc = meson.get_compiler('c')
repro_flag = '-ffile-prefix-map=' + meson.project_build_root() + '=./build'
if cc.has_argument(repro_flag)
  add_global_arguments(repro_flag, language: ['c', 'cpp'])
endif

vulkan_dep  = dependency('vulkan')
glfw_dep    = dependency('glfw3')
cimgui_dep  = dependency('cimgui')
libraw_dep  = dependency('libraw')
# Debian / Ubuntu: sudo apt install liblensfun-dev
lensfun_dep = dependency('lensfun')
sqlite3_dep = dependency('sqlite3')
tomlc99_dep = dependency('tomlc99')
libjpeg_dep = dependency('libjpeg')
libtiff_dep = dependency('libtiff-4')
libpng_dep  = dependency('libpng')
# Built from the wrap unconditionally — a system liblcms2, if present,
# would otherwise be picked ahead of it; vendoring keeps the colour
# engine version deterministic across machines.
lcms2_dep   = subproject('lcms2').get_variable('lcms2_dep')
blake3_dep  = subproject('blake3').get_variable('blake3_dep')
nfd_dep     = dependency('nativefiledialog-extended')
threads_dep = dependency('threads')

aperture_inc = include_directories('src')
aperture_sources = []

subdir('shaders')
subdir('src')

if host_machine.system() == 'linux'
  subdir('packaging/linux')
endif

aperture_deps = [
  vulkan_dep,
  glfw_dep,
  cimgui_dep,
  libraw_dep,
  lensfun_dep,
  sqlite3_dep,
  tomlc99_dep,
  libjpeg_dep,
  libtiff_dep,
  libpng_dep,
  lcms2_dep,
  blake3_dep,
  nfd_dep,
  threads_dep,
  shaders_dep,
]

aperture_exe = executable(
  'aperture',
  aperture_sources,
  include_directories: aperture_inc,
  dependencies: aperture_deps,
  install: true,
)
