glslc      = find_program('glslc')
embed_spv  = find_program(meson.project_source_root() / 'tools' / 'embed_spv.py')

shader_sources = [
  'histogram.comp',
  'raw_passthrough.comp',
  'color_grade.comp',
  'demosaic.comp',
  'demosaic_malvar.comp',
  'demosaic_adaptive.comp',
  'ahd_interp.comp',
  'ahd_select.comp',
  'denoise.comp',
  'wb.comp',
  'profile.comp',
  'profile_lut.comp',
  'exposure.comp',
  'exposure_soft.comp',
  'grain.comp',
  'hsl.comp',
  'saturation.comp',
  'sharpen.comp',
  'sharpen_blur_h.comp',
  'sharpen_gauss_combine.comp',
  'tone.comp',
  'tone_filmic.comp',
  'vignette.comp',
  'lens_correction.comp',
  'output_transfer.comp',
  'canvas.vert',
  'canvas.frag',
  'grid.vert',
  'grid.frag',
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
