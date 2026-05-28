project('glfw', 'c',
  default_options: [
    'c_std=c99',
    'warning_level=0',
    'optimization=2',
  ],
)

# glfw 3.4 — windowing + input. The upstream is CMake-driven; this
# overlay mirrors its source-list and per-backend dep wiring without
# adding any new features. macOS uses cocoa (objc), Windows uses win32,
# Linux compiles both X11 and Wayland backends (matching what system
# pkg-config typically provides on Linux distros).

cc  = meson.get_compiler('c')
sys = host_machine.system()

if sys == 'darwin'
  add_languages('objc', native: false)
endif

glfw_public_inc = include_directories('upstream/include')
glfw_build_inc  = include_directories('upstream/include', 'upstream/src')

glfw_common_src = files(
  'upstream/src/context.c', 'upstream/src/init.c',     'upstream/src/input.c',
  'upstream/src/monitor.c', 'upstream/src/platform.c', 'upstream/src/vulkan.c',
  'upstream/src/window.c',
  'upstream/src/egl_context.c', 'upstream/src/osmesa_context.c',
  'upstream/src/null_init.c', 'upstream/src/null_monitor.c',
  'upstream/src/null_window.c', 'upstream/src/null_joystick.c',
)

glfw_src         = glfw_common_src
glfw_deps        = [dependency('threads')]
glfw_defs        = []
glfw_extra_links = []

if sys == 'darwin'
  glfw_src += files(
    'upstream/src/cocoa_time.c',
    'upstream/src/posix_module.c', 'upstream/src/posix_thread.c',
    'upstream/src/cocoa_init.m',   'upstream/src/cocoa_joystick.m',
    'upstream/src/cocoa_monitor.m','upstream/src/cocoa_window.m',
    'upstream/src/nsgl_context.m',
  )
  glfw_defs += '-D_GLFW_COCOA'
  glfw_deps += dependency('appleframeworks',
    modules: ['Cocoa', 'IOKit', 'CoreFoundation'])
elif sys == 'windows'
  glfw_src += files(
    'upstream/src/win32_time.c',   'upstream/src/win32_module.c',
    'upstream/src/win32_thread.c', 'upstream/src/win32_init.c',
    'upstream/src/win32_joystick.c','upstream/src/win32_monitor.c',
    'upstream/src/win32_window.c', 'upstream/src/wgl_context.c',
  )
  glfw_defs += ['-D_GLFW_WIN32', '-DUNICODE', '-D_UNICODE']
  glfw_extra_links += [
    cc.find_library('gdi32'),
    cc.find_library('user32'),
    cc.find_library('shell32'),
  ]
else
  # linux / *bsd: ship both x11 and wayland backends, matching what
  # system glfw packages usually provide. xkbcommon + wayland-scanner
  # are required for the wayland side; x11 needs the usual x extension
  # dev headers.
  glfw_src += files(
    'upstream/src/posix_time.c',   'upstream/src/posix_module.c',
    'upstream/src/posix_thread.c', 'upstream/src/posix_poll.c',
    'upstream/src/linux_joystick.c',
    'upstream/src/x11_init.c',     'upstream/src/x11_monitor.c',
    'upstream/src/x11_window.c',
    'upstream/src/xkb_unicode.c',  'upstream/src/glx_context.c',
    'upstream/src/wl_init.c',      'upstream/src/wl_monitor.c',
    'upstream/src/wl_window.c',
  )
  # glfw's posix code paths assume O_CLOEXEC, memfd_create, and other
  # gnu/linux extensions visible without an explicit feature-test macro.
  # under -std=c99 those are stripped; pull them in via _GNU_SOURCE.
  glfw_defs += ['-D_GLFW_X11', '-D_GLFW_WAYLAND', '-D_GNU_SOURCE']

  glfw_deps += [
    dependency('x11'),
    dependency('xrandr'),
    dependency('xinerama'),
    dependency('xkbcommon'),
    dependency('xcursor'),
    dependency('xi'),
    dependency('xext'),
    dependency('wayland-client'),
    dependency('wayland-cursor'),
    dependency('wayland-egl'),
  ]

  rt_lib = cc.find_library('rt', required: false)
  m_lib  = cc.find_library('m',  required: false)
  dl_lib = cc.find_library('dl', required: false)
  glfw_extra_links += [rt_lib, m_lib, dl_lib]

  if cc.has_function('memfd_create',
      prefix: '#define _GNU_SOURCE\n#include <sys/mman.h>')
    glfw_defs += '-DHAVE_MEMFD_CREATE'
  endif

  # wayland protocol headers — generated at configure time from the xml
  # files shipped under upstream/deps/wayland/. wayland-scanner is
  # required.
  wayland_scanner = find_program('wayland-scanner')

  wayland_protos = [
    'wayland', 'viewporter', 'xdg-shell',
    'idle-inhibit-unstable-v1', 'pointer-constraints-unstable-v1',
    'relative-pointer-unstable-v1', 'fractional-scale-v1',
    'xdg-activation-v1', 'xdg-decoration-unstable-v1',
  ]

  foreach proto : wayland_protos
    xml = files('upstream/deps/wayland/' + proto + '.xml')
    glfw_src += custom_target(proto + '-client-protocol.h',
      input: xml,
      output: proto + '-client-protocol.h',
      command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
    glfw_src += custom_target(proto + '-client-protocol-code.h',
      input: xml,
      output: proto + '-client-protocol-code.h',
      command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
  endforeach
endif

glfw_lib = static_library('glfw', glfw_src,
  c_args: glfw_defs,
  include_directories: glfw_build_inc,
  dependencies: glfw_deps + glfw_extra_links,
  install: false,
)

glfw_dep = declare_dependency(
  link_with: glfw_lib,
  include_directories: glfw_public_inc,
  dependencies: glfw_deps + glfw_extra_links,
)

meson.override_dependency('glfw3', glfw_dep)
