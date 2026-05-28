project('tomlc99', 'c',
  default_options: [
    'c_std=c11',
    'warning_level=0',
  ],
)

tomlc99_lib = static_library('tomlc99',
  'upstream/toml.c',
  include_directories: include_directories('upstream'),
)

tomlc99_dep = declare_dependency(
  link_with: tomlc99_lib,
  include_directories: include_directories('upstream'),
)

meson.override_dependency('tomlc99', tomlc99_dep)
