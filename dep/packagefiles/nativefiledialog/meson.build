project('nativefiledialog-extended', 'cpp',
  default_options: [
    'warning_level=0',
    'optimization=2',
  ],
)

# nativefiledialog-extended — native OS file dialogs. Only the host
# platform's backend is compiled. The macOS backend (nfd_cocoa.m) is
# Objective-C, so enable that language on darwin only — adding it to
# project() unconditionally would force an objc toolchain on Linux.
if host_machine.system() == 'darwin'
  add_languages('objc', native: false)
endif

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
  nfd_src  = files('src/nfd_cocoa.m')
  nfd_deps = [
    dependency('appleframeworks', modules: ['AppKit', 'Foundation', 'UniformTypeIdentifiers']),
  ]
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
