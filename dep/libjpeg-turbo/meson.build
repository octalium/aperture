project('libjpeg-turbo', 'c',
  version: '3.1.4.1',
  default_options: [
    'c_std=c11',
    'warning_level=0',
    'optimization=2',
  ],
)

# libjpeg-turbo, vendored from-source. The upstream is CMake-driven and
# includes a nasm/intrinsics SIMD layer per arch; this overlay disables
# SIMD (no nasm dep, no per-arch subprojects) and builds the portable C
# code path only. Performance is somewhat lower than distro packages
# that ship SIMD-enabled binaries; correctness is identical.

cc = meson.get_compiler('c')

jpeg_lib_version = 80
jpeg_version     = meson.project_version()

# pack the version into the same integer libjpeg-turbo's CMake derives:
# major * 1_000_000 + minor * 1000 + patch (build segment ignored here —
# the canonical formula in CMakeLists.txt collapses 4-segment versions
# the same way for the .h define).
jpeg_turbo_version_number = 3001004

jconfig_data = configuration_data()
jconfig_data.set('JPEG_LIB_VERSION',             jpeg_lib_version)
jconfig_data.set('VERSION',                      jpeg_version)
jconfig_data.set('LIBJPEG_TURBO_VERSION_NUMBER', jpeg_turbo_version_number)
jconfig_data.set('C_ARITH_CODING_SUPPORTED',     1)
jconfig_data.set('D_ARITH_CODING_SUPPORTED',     1)
# no simd; intentionally left undefined so #cmakedefine WITH_SIMD becomes /* */.
# RIGHT_SHIFT_IS_UNSIGNED: undefined on every compiler aperture targets
# (gcc, clang, msvc all sign-extend as expected); leaving it unset is
# consistent with libjpeg-turbo's CMake autodetect on these compilers.

jconfig_h = configure_file(
  input:         'upstream/src/jconfig.h.in',
  output:        'jconfig.h',
  configuration: jconfig_data,
  format:        'cmake@',
)

jconfigint_data = configuration_data()
jconfigint_data.set('BUILD',              '0')
jconfigint_data.set('CMAKE_PROJECT_NAME', 'libjpeg-turbo')
jconfigint_data.set('VERSION',            jpeg_version)
jconfigint_data.set('SIZE_T',             cc.sizeof('size_t'))

if host_machine.system() == 'windows'
  jconfigint_data.set('HIDDEN',       '')
  jconfigint_data.set('INLINE',       '__inline')
  jconfigint_data.set('THREAD_LOCAL', '__declspec(thread)')
else
  jconfigint_data.set('HIDDEN',       '__attribute__((visibility("hidden")))')
  jconfigint_data.set('INLINE',       'inline __attribute__((always_inline))')
  jconfigint_data.set('THREAD_LOCAL', '__thread')
endif

if cc.compiles('int main(void){return __builtin_ctzl(1L);}',
    name: 'has __builtin_ctzl')
  jconfigint_data.set('HAVE_BUILTIN_CTZL', 1)
endif
if cc.has_header('intrin.h')
  jconfigint_data.set('HAVE_INTRIN_H', 1)
endif

jconfigint_data.set('C_ARITH_CODING_SUPPORTED', 1)
jconfigint_data.set('D_ARITH_CODING_SUPPORTED', 1)

jconfigint_h = configure_file(
  input:         'upstream/src/jconfigint.h.in',
  output:        'jconfigint.h',
  configuration: jconfigint_data,
  format:        'cmake@',
)

jversion_data = configuration_data()
jversion_data.set('COPYRIGHT_YEAR', '1991-2026')

jversion_h = configure_file(
  input:         'upstream/src/jversion.h.in',
  output:        'jversion.h',
  configuration: jversion_data,
  format:        'cmake@',
)

jpeg_sources = files(
  'upstream/src/jcapimin.c',
  'upstream/src/wrapper/jcapistd-8.c',  'upstream/src/wrapper/jcapistd-12.c',
  'upstream/src/wrapper/jcapistd-16.c',
  'upstream/src/wrapper/jccoefct-8.c',  'upstream/src/wrapper/jccoefct-12.c',
  'upstream/src/wrapper/jccolor-8.c',   'upstream/src/wrapper/jccolor-12.c',
  'upstream/src/wrapper/jccolor-16.c',
  'upstream/src/wrapper/jcdctmgr-8.c',  'upstream/src/wrapper/jcdctmgr-12.c',
  'upstream/src/wrapper/jcdiffct-8.c',  'upstream/src/wrapper/jcdiffct-12.c',
  'upstream/src/wrapper/jcdiffct-16.c',
  'upstream/src/jchuff.c', 'upstream/src/jcicc.c',
  'upstream/src/jcinit.c', 'upstream/src/jclhuff.c',
  'upstream/src/wrapper/jclossls-8.c',  'upstream/src/wrapper/jclossls-12.c',
  'upstream/src/wrapper/jclossls-16.c',
  'upstream/src/wrapper/jcmainct-8.c',  'upstream/src/wrapper/jcmainct-12.c',
  'upstream/src/wrapper/jcmainct-16.c',
  'upstream/src/jcmarker.c', 'upstream/src/jcmaster.c',
  'upstream/src/jcomapi.c',  'upstream/src/jcparam.c',
  'upstream/src/jcphuff.c',
  'upstream/src/wrapper/jcprepct-8.c',  'upstream/src/wrapper/jcprepct-12.c',
  'upstream/src/wrapper/jcprepct-16.c',
  'upstream/src/wrapper/jcsample-8.c',  'upstream/src/wrapper/jcsample-12.c',
  'upstream/src/wrapper/jcsample-16.c',
  'upstream/src/jctrans.c', 'upstream/src/jdapimin.c',
  'upstream/src/wrapper/jdapistd-8.c',  'upstream/src/wrapper/jdapistd-12.c',
  'upstream/src/wrapper/jdapistd-16.c',
  'upstream/src/jdatadst.c', 'upstream/src/jdatasrc.c',
  'upstream/src/wrapper/jdcoefct-8.c',  'upstream/src/wrapper/jdcoefct-12.c',
  'upstream/src/wrapper/jdcolor-8.c',   'upstream/src/wrapper/jdcolor-12.c',
  'upstream/src/wrapper/jdcolor-16.c',
  'upstream/src/wrapper/jddctmgr-8.c',  'upstream/src/wrapper/jddctmgr-12.c',
  'upstream/src/wrapper/jddiffct-8.c',  'upstream/src/wrapper/jddiffct-12.c',
  'upstream/src/wrapper/jddiffct-16.c',
  'upstream/src/jdhuff.c', 'upstream/src/jdicc.c',
  'upstream/src/jdinput.c', 'upstream/src/jdlhuff.c',
  'upstream/src/wrapper/jdlossls-8.c',  'upstream/src/wrapper/jdlossls-12.c',
  'upstream/src/wrapper/jdlossls-16.c',
  'upstream/src/wrapper/jdmainct-8.c',  'upstream/src/wrapper/jdmainct-12.c',
  'upstream/src/wrapper/jdmainct-16.c',
  'upstream/src/jdmarker.c', 'upstream/src/jdmaster.c',
  'upstream/src/wrapper/jdmerge-8.c',   'upstream/src/wrapper/jdmerge-12.c',
  'upstream/src/jdphuff.c',
  'upstream/src/wrapper/jdpostct-8.c',  'upstream/src/wrapper/jdpostct-12.c',
  'upstream/src/wrapper/jdpostct-16.c',
  'upstream/src/wrapper/jdsample-8.c',  'upstream/src/wrapper/jdsample-12.c',
  'upstream/src/wrapper/jdsample-16.c',
  'upstream/src/jdtrans.c', 'upstream/src/jerror.c',
  'upstream/src/jfdctflt.c',
  'upstream/src/wrapper/jfdctfst-8.c',  'upstream/src/wrapper/jfdctfst-12.c',
  'upstream/src/wrapper/jfdctint-8.c',  'upstream/src/wrapper/jfdctint-12.c',
  'upstream/src/wrapper/jidctflt-8.c',  'upstream/src/wrapper/jidctflt-12.c',
  'upstream/src/wrapper/jidctfst-8.c',  'upstream/src/wrapper/jidctfst-12.c',
  'upstream/src/wrapper/jidctint-8.c',  'upstream/src/wrapper/jidctint-12.c',
  'upstream/src/wrapper/jidctred-8.c',  'upstream/src/wrapper/jidctred-12.c',
  'upstream/src/jmemmgr.c', 'upstream/src/jmemnobs.c',
  'upstream/src/jpeg_nbits.c',
  'upstream/src/wrapper/jquant1-8.c',   'upstream/src/wrapper/jquant1-12.c',
  'upstream/src/wrapper/jquant2-8.c',   'upstream/src/wrapper/jquant2-12.c',
  'upstream/src/wrapper/jutils-8.c',    'upstream/src/wrapper/jutils-12.c',
  'upstream/src/wrapper/jutils-16.c',
  'upstream/src/jaricom.c', 'upstream/src/jcarith.c', 'upstream/src/jdarith.c',
)

jpeg_public_inc = include_directories('upstream/src')

# libjpeg-turbo calls setenv() in jinclude.h; under -std=c11 that needs
# an explicit feature-test macro to be visible. _POSIX_C_SOURCE 200809L
# is the right scope and matches what their CMake builds get implicitly
# from the toolchain default on glibc systems.
jpeg_lib_args = []
if host_machine.system() != 'windows'
  jpeg_lib_args += '-D_POSIX_C_SOURCE=200809L'
endif

jpeg_lib = static_library('jpeg',
  [jpeg_sources, jconfig_h, jconfigint_h, jversion_h],
  c_args: jpeg_lib_args,
  include_directories: [jpeg_public_inc, include_directories('.')],
  install: false,
)

libjpeg_dep = declare_dependency(
  link_with: jpeg_lib,
  include_directories: jpeg_public_inc,
)

meson.override_dependency('libjpeg', libjpeg_dep)
