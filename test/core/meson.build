fs_test = executable(
  'fs_test',
  'fs_test.c',
  files(
    '../../src/core/fs.c',
    '../../src/core/random.c',
    '../../src/core/log.c',
  ) + ap_dir_src,
  include_directories: [aperture_inc, aptest_inc],
  dependencies: platform_deps,
)
test('core/fs', fs_test)
