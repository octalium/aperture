#ifndef APERTURE_LIBRARY_IMPORT_H
#define APERTURE_LIBRARY_IMPORT_H

#include "library/library.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AP_IMPORT_SUBDIR_LEN  64
#define AP_IMPORT_PATTERN_LEN 256

// Filename handling for copied files.
typedef enum {
    AP_IMPORT_NAME_KEEP    = 0,  // keep the source filename
    AP_IMPORT_NAME_PATTERN = 1,  // rename via `pattern`
} ap_import_naming;

// What to do when a destination filename already exists.
typedef enum {
    AP_IMPORT_COLLIDE_SKIP      = 0,  // leave the existing file, skip
    AP_IMPORT_COLLIDE_OVERWRITE = 1,  // replace it
    AP_IMPORT_COLLIDE_SUFFIX    = 2,  // append _1, _2, ... to the stem
} ap_import_collision;

// Per-library import preferences, persisted in the library's settings.
//
// `pattern` (used when naming == PATTERN) expands these tokens; the
// file's original extension is always appended afterwards:
//   {ORIG} {YYYY} {MM} {DD} {HH} {MIN} {SEC} {SEQ}
// Date / time come from the raw's EXIF capture time, falling back to
// the source file's modification time. {SEQ} is a 4-digit per-import
// counter starting at 1.
typedef struct {
    char subdir[AP_IMPORT_SUBDIR_LEN];    // destination subdir under the root
    int  naming;                          // ap_import_naming
    char pattern[AP_IMPORT_PATTERN_LEN];  // rename pattern
    int  collision;                       // ap_import_collision
} ap_import_settings;

// Load the library's import settings, filling any unset field with its
// default (subdir "raw", keep original names, skip-on-collision).
void ap_import_settings_load(const ap_library *lib, ap_import_settings *out);

// Persist the import settings on the library.
void ap_import_settings_save(ap_library *lib, const ap_import_settings *s);

// Copy every raw file found under `src_dir` (searched recursively) into
// <lib_root>/<s.subdir>/, applying the naming + collision rules. The
// source files are not modified. Writes the number of files copied to
// *out_imported (may be NULL). Returns 0 on success, -1 on a setup
// failure (bad arguments, destination not creatable).
int ap_import_run(ap_library *lib, const char *src_dir,
                  const ap_import_settings *s, int *out_imported);

#ifdef __cplusplus
}
#endif

#endif
