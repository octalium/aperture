sidecar_test = executable(
  'sidecar_test',
  'sidecar_test.c',
  files(
    '../../src/sidecar/sidecar.c',
    '../../src/edit/stack.c',
    '../../src/edit/stack_toml.c',
    '../../src/photo/culling.c',
    '../../src/photo/keywords.c',
    '../../src/photo/metadata.c',
    '../../src/core/log.c',
    '../support/aptest_modules.c',
  ),
  include_directories: [aperture_inc, aptest_inc],
  dependencies: [tomlc99_dep],
)
test('sidecar/round_trip', sidecar_test)
