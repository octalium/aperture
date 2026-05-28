project('libraw', 'cpp',
  default_options: [
    'cpp_std=c++14',
    'warning_level=0',
    'optimization=2',
  ],
)

# libraw 0.22.x — raw decoder. Upstream is autotools-driven; the
# source list and compile flags here mirror Makefile.am's
# `lib_libraw_a_SOURCES`. Optional internal backends (RawSpeed,
# DNGSDK, GoPro) are guarded by USE_* macros and become no-ops at
# compile time when those macros are undefined — we leave them all
# off. The two internal deps we DO wire are libjpeg (for JPEG-in-DNG
# decoding) and lcms2 (for ICC profile handling); both are vendored
# elsewhere in the tree (libjpeg via the sibling shim, lcms2 from #521).

cxx = meson.get_compiler('cpp')

# Route both deps through the sibling subproject directly. Going via
# dependency('lcms2', fallback: ...) would find a system lcms2 first
# and then collide with the root's force-vendored override at lcms2's
# meson.override_dependency() call. libjpeg is exposed under the
# canonical 'libjpeg' name by the libjpeg-turbo shim — using that name
# here keeps wiring symmetrical and works whether resolution comes
# from this overlay or the root.
libjpeg_dep = subproject('libjpeg-turbo').get_variable('libjpeg_dep')
lcms2_dep   = subproject('lcms2').get_variable('lcms2_dep')

libraw_public_inc = include_directories('upstream')
libraw_build_inc  = include_directories('upstream', 'upstream/internal')

libraw_sources = files(
  'upstream/src/libraw_c_api.cpp',
  'upstream/src/libraw_datastream.cpp',
  'upstream/src/decoders/canon_600.cpp',
  'upstream/src/decoders/crx.cpp',
  'upstream/src/decoders/pana8.cpp',
  'upstream/src/decoders/decoders_dcraw.cpp',
  'upstream/src/decoders/sonycc.cpp',
  'upstream/src/decompressors/losslessjpeg.cpp',
  'upstream/src/decoders/decoders_libraw_dcrdefs.cpp',
  'upstream/src/decoders/olympus14.cpp',
  'upstream/src/decoders/decoders_libraw.cpp',
  'upstream/src/decoders/dng.cpp',
  'upstream/src/decoders/fp_dng.cpp',
  'upstream/src/decoders/fuji_compressed.cpp',
  'upstream/src/decoders/generic.cpp',
  'upstream/src/decoders/kodak_decoders.cpp',
  'upstream/src/decoders/load_mfbacks.cpp',
  'upstream/src/decoders/smal.cpp',
  'upstream/src/decoders/unpack_thumb.cpp',
  'upstream/src/decoders/unpack.cpp',
  'upstream/src/demosaic/aahd_demosaic.cpp',
  'upstream/src/demosaic/ahd_demosaic.cpp',
  'upstream/src/demosaic/dcb_demosaic.cpp',
  'upstream/src/demosaic/dht_demosaic.cpp',
  'upstream/src/demosaic/misc_demosaic.cpp',
  'upstream/src/demosaic/xtrans_demosaic.cpp',
  'upstream/src/integration/dngsdk_glue.cpp',
  'upstream/src/integration/rawspeed_glue.cpp',
  'upstream/src/metadata/adobepano.cpp',
  'upstream/src/metadata/canon.cpp',
  'upstream/src/metadata/ciff.cpp',
  'upstream/src/metadata/cr3_parser.cpp',
  'upstream/src/metadata/epson.cpp',
  'upstream/src/metadata/exif_gps.cpp',
  'upstream/src/metadata/fuji.cpp',
  'upstream/src/metadata/identify_tools.cpp',
  'upstream/src/metadata/identify.cpp',
  'upstream/src/metadata/kodak.cpp',
  'upstream/src/metadata/leica.cpp',
  'upstream/src/metadata/makernotes.cpp',
  'upstream/src/metadata/mediumformat.cpp',
  'upstream/src/metadata/minolta.cpp',
  'upstream/src/metadata/misc_parsers.cpp',
  'upstream/src/metadata/nikon.cpp',
  'upstream/src/metadata/normalize_model.cpp',
  'upstream/src/metadata/olympus.cpp',
  'upstream/src/metadata/hasselblad_model.cpp',
  'upstream/src/metadata/p1.cpp',
  'upstream/src/metadata/pentax.cpp',
  'upstream/src/metadata/samsung.cpp',
  'upstream/src/metadata/sony.cpp',
  'upstream/src/metadata/tiff.cpp',
  'upstream/src/postprocessing/aspect_ratio.cpp',
  'upstream/src/postprocessing/dcraw_process.cpp',
  'upstream/src/postprocessing/mem_image.cpp',
  'upstream/src/postprocessing/postprocessing_aux.cpp',
  'upstream/src/postprocessing/postprocessing_utils_dcrdefs.cpp',
  'upstream/src/postprocessing/postprocessing_utils.cpp',
  'upstream/src/preprocessing/ext_preprocess.cpp',
  'upstream/src/preprocessing/raw2image.cpp',
  'upstream/src/preprocessing/subtract_black.cpp',
  'upstream/src/tables/cameralist.cpp',
  'upstream/src/tables/colorconst.cpp',
  'upstream/src/tables/colordata.cpp',
  'upstream/src/tables/wblists.cpp',
  'upstream/src/utils/curves.cpp',
  'upstream/src/utils/decoder_info.cpp',
  'upstream/src/utils/init_close_utils.cpp',
  'upstream/src/utils/open.cpp',
  'upstream/src/utils/phaseone_processing.cpp',
  'upstream/src/utils/read_utils.cpp',
  'upstream/src/utils/thumb_utils.cpp',
  'upstream/src/utils/utils_dcraw.cpp',
  'upstream/src/utils/utils_libraw.cpp',
  'upstream/src/write/apply_profile.cpp',
  'upstream/src/write/file_write.cpp',
  'upstream/src/write/tiff_writer.cpp',
  'upstream/src/x3f/x3f_parse_process.cpp',
  'upstream/src/x3f/x3f_utils_patched.cpp',
)

libraw_cpp_args = [
  '-DLIBRAW_NOTHREADS',
  '-DUSE_JPEG',
  '-DUSE_JPEG8',
  '-DUSE_LCMS2',
]

libraw_lib = static_library('raw', libraw_sources,
  cpp_args: libraw_cpp_args,
  include_directories: libraw_build_inc,
  dependencies: [libjpeg_dep, lcms2_dep],
  install: false,
)

libraw_dep = declare_dependency(
  link_with: libraw_lib,
  include_directories: libraw_public_inc,
  dependencies: [libjpeg_dep, lcms2_dep],
)

meson.override_dependency('libraw', libraw_dep)
