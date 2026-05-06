glslc      = find_program('glslc')
embed_spv  = find_program(meson.project_source_root() / 'tools' / 'embed_spv.py')

shader_sources = [
  'process.comp',
  'encode.comp',
]

shader_headers = []

foreach src : shader_sources
  base = src.replace('.', '_')

  spv = custom_target('spv_' + base,
    input: src,
    output: src + '.spv',
    command: [glslc, '-O', '--target-env=vulkan1.3', '-o', '@OUTPUT@', '@INPUT@'],
  )

  hdr = custom_target('hdr_' + base,
    input: spv,
    output: base + '_spv.h',
    command: [embed_spv, '@INPUT@', '@OUTPUT@', base + '_spv'],
  )

  shader_headers += hdr
endforeach

shaders_dep = declare_dependency(
  sources: shader_headers,
  include_directories: include_directories('.'),
)
