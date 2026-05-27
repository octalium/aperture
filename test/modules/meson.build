stack_pack_test = executable(
  'stack_pack_test',
  'stack_pack_test.c',
  files(
    '../../src/edit/stack.c',
    '../../src/edit/stack_toml.c',
    '../../src/core/log.c',
    '../support/aptest_modules.c',
  ),
  include_directories: [aperture_inc, aptest_inc],
  dependencies: [tomlc99_dep],
)
test('modules/stack_pack', stack_pack_test)
