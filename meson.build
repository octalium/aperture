project(
  'aperture',
  ['c', 'cpp'],
  version: '0.1.0',
  default_options: [
    'c_std=c17',
    'cpp_std=c++17',
    'warning_level=2',
    'b_ndebug=if-release',
  ],
  meson_version: '>=1.3.0',
  subproject_dir: 'dep',
)

# nativefiledialog's macOS backend (nfd_cocoa.m) needs objc; only
# enable on darwin so Linux builds don't require an objc toolchain.
if host_machine.system() == 'darwin'
  add_languages('objc', native: false)
endif

add_project_arguments(
  '-DAPERTURE_VERSION="@0@"'.format(meson.project_version()),
  '-DAP_VERSION_STRING="@0@"'.format(meson.project_version()),
  language: ['c', 'cpp'],
)

# Distribution channel selects the ap_updater backend; see meson.options.
ap_dist = get_option('ap_dist')
if ap_dist == 'flatpak'
  add_project_arguments('-DAP_DIST_FLATPAK=1', language: ['c', 'cpp'])
endif

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

vulkan_dep  = subproject('vulkan-headers').get_variable('vulkan_dep')
glfw_dep    = subproject('glfw').get_variable('glfw_dep')
cimgui_dep  = dependency('cimgui')
libjpeg_dep = subproject('libjpeg-turbo').get_variable('libjpeg_dep')
libraw_dep  = subproject('libraw').get_variable('libraw_dep')
# Debian / Ubuntu: sudo apt install liblensfun-dev
lensfun_dep = dependency('lensfun')
sqlite3_dep = subproject('sqlite3').get_variable('sqlite3_dep')
tomlc99_dep = dependency('tomlc99')
libtiff_dep = dependency('libtiff-4')
libpng_dep  = dependency('libpng')
# Built from the wrap unconditionally — a system liblcms2, if present,
# would otherwise be picked ahead of it; vendoring keeps the colour
# engine version deterministic across machines.
lcms2_dep   = subproject('lcms2').get_variable('lcms2_dep')
blake3_dep  = subproject('blake3').get_variable('blake3_dep')
cjson_dep   = subproject('cJSON').get_variable('cjson_dep')
nfd_dep     = dependency('nativefiledialog-extended',
  fallback: ['nativefiledialog', 'nfd_dep'])
threads_dep = dependency('threads')

# HTTP for the update checker. Windows uses WinHTTP (system),
# macOS uses NSURLSession via Foundation (system), Linux uses a
# vendored mbedTLS + a small HTTPS GET shim in src/net/.
http_deps = []
if host_machine.system() == 'windows'
  http_deps += cc.find_library('winhttp')
elif host_machine.system() == 'darwin'
  http_deps += dependency('Foundation')
else
  http_deps += subproject('mbedtls').get_variable('mbedtls_dep')
endif

aperture_inc = include_directories('src')
aperture_sources = []

subdir('shaders')
subdir('src')

if host_machine.system() == 'linux'
  subdir('packaging/linux')
elif host_machine.system() == 'darwin'
  subdir('packaging/macos')
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
  cjson_dep,
  nfd_dep,
  threads_dep,
  shaders_dep,
] + http_deps

aperture_exe = executable(
  'aperture',
  aperture_sources,
  include_directories: aperture_inc,
  dependencies: aperture_deps,
  install: true,
)

subdir('test')
