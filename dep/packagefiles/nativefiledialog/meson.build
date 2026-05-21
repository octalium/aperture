project('nativefiledialog-extended', 'cpp',
  default_options: [
    'warning_level=0',
    'optimization=2',
  ],
)

# nativefiledialog-extended — native OS file dialogs. Only the host
# platform's backend is compiled. The project is Linux-only today; the
# Windows branch is kept ready, and macOS (nfd_cocoa.m, Objective-C)
# needs the objc language added at project() level when that platform
# becomes a build target.
cpp     = meson.get_compiler('cpp')
nfd_inc = include_directories('src/include')

sys      = host_machine.system()
nfd_src  = []
nfd_deps = []

if sys == 'windows'
  nfd_src  = files('src/nfd_win.cpp')
  nfd_deps = [
    cpp.find_library('ole32'),
    cpp.find_library('uuid'),
    cpp.find_library('shell32'),
  ]
elif sys == 'darwin'
  error('nativefiledialog wrap: macOS backend (nfd_cocoa.m) needs the objc language — wire up when macOS is a build target')
else
  # Linux / *BSD — the xdg-desktop-portal backend: D-Bus only, no GTK.
  nfd_src  = files('src/nfd_portal.cpp')
  nfd_deps = [dependency('dbus-1')]
endif

nfd_lib = static_library('nfd', nfd_src,
  include_directories: nfd_inc,
  dependencies: nfd_deps,
  install: false,
)

nfd_dep = declare_dependency(
  link_with: nfd_lib,
  include_directories: nfd_inc,
  dependencies: nfd_deps,
)

meson.override_dependency('nativefiledialog-extended', nfd_dep)
