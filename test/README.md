# Tests

Sanity + unit tests for aperture. No framework — a test is a plain
`int main` that returns `0` on success and asserts with `AP_TEST_ASSERT`
(see [`include/aptest.h`](include/aptest.h)). meson's `test()` rule
records the exit code; a failed assert `abort()`s and the test fails.

## Run

```
make test            # or: meson test -C build --print-errorlogs
```

Runs on every PR via the `ubuntu-22.04` and `windows-2022` CI jobs;
merges gate on green.

## Coverage

| Test | What it checks |
|---|---|
| `io/exif` | EXIF parsing over synthesized in-memory blobs |
| `library/library` | library schema + dedupe logic against a temp dir |
| `modules/stack_pack` | module pack/unpack round-trip |
| `sidecar/round_trip` | sidecar read/write round-trip |

(`make test` also runs the desktop-file / AppStream metadata validators.)

## Add a test

1. Drop `foo_test.c` under `test/<area>/`:

   ```c
   #include "aptest.h"

   int main(void) {
       AP_TEST_ASSERT(1 + 1 == 2, "math is broken");
       return 0;
   }
   ```

2. Wire it in `test/<area>/meson.build` (mirror
   [`io/meson.build`](io/meson.build)) — link the source under test
   directly, no need to pull in the whole binary:

   ```meson
   foo_test = executable(
     'foo_test', 'foo_test.c',
     files('../../src/<area>/foo.c'),
     include_directories: [aperture_inc, aptest_inc],
   )
   test('<area>/foo', foo_test)
   ```

3. If `test/<area>/` is new, add `subdir('<area>')` to
   [`meson.build`](meson.build).

Favor testing pure functions; extract a pure helper rather than dragging
app state into a test.

## Fixtures

- [`include/aptest_tmpdir.h`](include/aptest_tmpdir.h) — scratch temp
  directory for filesystem/library tests.
- [`include/aptest_modules.h`](include/aptest_modules.h) +
  [`support/aptest_modules.c`](support/aptest_modules.c) — module
  registry fixtures.
- [`support/vulkan_stub/`](support/vulkan_stub/) — headless Vulkan stub
  so GPU-touching code links without a real device.

## Out of scope (for now)

Shader numerical tests (need a GPU + reference path) and end-to-end UI
automation (need an automation toolchain). Add when the need is concrete.
