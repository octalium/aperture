project('cjson', 'c',
  default_options: [
    'c_std=c11',
    'warning_level=0',
  ],
)

cjson_lib = static_library('cjson',
  'cJSON.c',
  include_directories: include_directories('.'),
)

cjson_dep = declare_dependency(
  link_with: cjson_lib,
  include_directories: include_directories('.'),
)

meson.override_dependency('cjson', cjson_dep)
