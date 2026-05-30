# vulkan_stub feeds a tiny vulkan/vulkan.h shim (typedefs only) so the
# test binary can compile sources that transitively include the real
# header (photo/thumbnail.h here) without requiring the Vulkan SDK on
# the test host. The test never calls into the API.
library_vulkan_stub_inc = include_directories('../support/vulkan_stub')

library_test = executable(
  'library_test',
  'library_test.c',
  'library_test_stubs.c',
  files(
    '../../src/library/library.c',
    '../../src/sidecar/sidecar.c',
    '../../src/app/root.c',
    '../../src/edit/stack.c',
    '../../src/edit/stack_toml.c',
    '../../src/photo/culling.c',
    '../../src/photo/keywords.c',
    '../../src/photo/metadata.c',
    '../../src/core/log.c',
    '../../src/core/fs.c',
    '../../src/core/random.c',
    '../support/aptest_modules.c',
  ) + ap_dir_src,
  include_directories: [aperture_inc, aptest_inc, library_vulkan_stub_inc],
  dependencies: [sqlite3_dep, tomlc99_dep] + platform_deps,
)
test('library/library', library_test)
