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
  'upstream/src/cmsalpha.c',  'upstream/src/cmscam02.c', 'upstream/src/cmscgats.c', 'upstream/src/cmscnvrt.c',
  'upstream/src/cmserr.c',    'upstream/src/cmsgamma.c', 'upstream/src/cmsgmt.c',   'upstream/src/cmshalf.c',
  'upstream/src/cmsintrp.c',  'upstream/src/cmsio0.c',   'upstream/src/cmsio1.c',   'upstream/src/cmslut.c',
  'upstream/src/cmsmd5.c',    'upstream/src/cmsmtrx.c',  'upstream/src/cmsnamed.c', 'upstream/src/cmsopt.c',
  'upstream/src/cmspack.c',   'upstream/src/cmspcs.c',   'upstream/src/cmsplugin.c','upstream/src/cmsps2.c',
  'upstream/src/cmssamp.c',   'upstream/src/cmssm.c',    'upstream/src/cmstypes.c', 'upstream/src/cmsvirt.c',
  'upstream/src/cmswtpnt.c',  'upstream/src/cmsxform.c',
)

# Public headers in upstream/include/; internal headers sit beside the
# sources in upstream/src/ and resolve via same-directory quoted includes.
lcms2_public_inc = include_directories('upstream/include')
lcms2_build_inc  = include_directories('upstream/include', 'upstream/src')

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
