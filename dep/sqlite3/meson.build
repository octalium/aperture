project('sqlite3', 'c',
  version: '3.53.1',
  default_options: [
    'c_std=c11',
    'warning_level=0',
    'optimization=2',
  ],
)

# SQLite is vendored as an in-tree direct copy of the official
# amalgamation (sqlite3.c + sqlite3.h from sqlite.org's
# sqlite-autoconf tarball) rather than a submodule. The upstream git
# mirror (github.com/sqlite/sqlite) does not ship the amalgamation;
# it's generated from the split sources by a tcl/lemon toolchain at
# release time. We follow the same precedent as blake3: take the
# two-file portable surface, commit it directly, refresh on demand
# via scripts/refresh-sqlite3.sh.

cc = meson.get_compiler('c')

sqlite3_lib = static_library('sqlite3',
  'sqlite3.c',
  include_directories: include_directories('.'),
  dependencies: [
    cc.find_library('m',      required: false),
    cc.find_library('dl',     required: false),
    cc.find_library('pthread', required: false),
  ],
  install: false,
)

sqlite3_dep = declare_dependency(
  link_with: sqlite3_lib,
  include_directories: include_directories('.'),
)

meson.override_dependency('sqlite3', sqlite3_dep)
