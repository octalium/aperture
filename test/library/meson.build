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
    '../support/aptest_modules.c',
  ),
  include_directories: [aperture_inc, aptest_inc],
  dependencies: [sqlite3_dep, tomlc99_dep, vulkan_dep],
)
test('library/library', library_test)
