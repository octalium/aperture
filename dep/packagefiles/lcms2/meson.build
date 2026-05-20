project('lcms2', 'c',
  default_options: [
    'c_std=c11',
    'warning_level=0',
    'optimization=2',
  ],
)

# Little-CMS core. The library is a flat set of translation units with
# no generated config header — just compile src/*.c. The plugins/ and
# utils/ trees are deliberately excluded; aperture only needs the core
# colour engine.
cc = meson.get_compiler('c')

lcms2_src = files(
  'src/cmsalpha.c',  'src/cmscam02.c', 'src/cmscgats.c', 'src/cmscnvrt.c',
  'src/cmserr.c',    'src/cmsgamma.c', 'src/cmsgmt.c',   'src/cmshalf.c',
  'src/cmsintrp.c',  'src/cmsio0.c',   'src/cmsio1.c',   'src/cmslut.c',
  'src/cmsmd5.c',    'src/cmsmtrx.c',  'src/cmsnamed.c', 'src/cmsopt.c',
  'src/cmspack.c',   'src/cmspcs.c',   'src/cmsplugin.c','src/cmsps2.c',
  'src/cmssamp.c',   'src/cmssm.c',    'src/cmstypes.c', 'src/cmsvirt.c',
  'src/cmswtpnt.c',  'src/cmsxform.c',
)

# Public headers in include/; internal headers sit beside the sources
# in src/ and resolve via same-directory quoted includes.
lcms2_public_inc = include_directories('include')
lcms2_build_inc  = include_directories('include', 'src')

m_dep      = cc.find_library('m', required: false)
thread_dep = dependency('threads')

lcms2_lib = static_library('lcms2', lcms2_src,
  include_directories: lcms2_build_inc,
  dependencies: [m_dep, thread_dep],
  install: false,
)

lcms2_dep = declare_dependency(
  link_with: lcms2_lib,
  include_directories: lcms2_public_inc,
)

meson.override_dependency('lcms2', lcms2_dep)
