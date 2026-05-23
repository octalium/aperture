project('blake3', 'c',
  default_options: [
    'c_std=c99',
    'warning_level=0',
  ],
)

blake3_lib = static_library('blake3',
  'blake3.c',
  include_directories: include_directories('.'),
)

blake3_dep = declare_dependency(
  link_with: blake3_lib,
  include_directories: include_directories('.'),
)

meson.override_dependency('blake3', blake3_dep)
