#ifndef APERTURE_LIBRARY_IMPORT_H
#define APERTURE_LIBRARY_IMPORT_H

#include "library/library.h"

#include <stdbool.h>

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
    AP_IMPORT_COLLIDE_SUFFIX    = 2,  // append _0001, _0002, ... to the stem
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
    // When true (default), look each source up in the library's
    // photos.identity column (derived from EXIF: Make | Model |
    // DateTimeOriginal | SubSecTimeOriginal) before copying; a match
    // anywhere in the library is treated as already imported and
    // skipped (counted as dup_content). Sources whose EXIF can't
    // populate every field of the identity tuple skip the lookup —
    // see `strict_identity` for what happens to them next. Rows
    // imported before the identity column existed have a NULL
    // identity, so re-importing them into a legacy library will copy
    // again; rebuild the library or clean by hand if that matters.
    // Turn this flag off when you genuinely want two copies of the
    // same shot in different subdirs. Independent of the byte-
    // equality safety check at the destination path, which always
    // runs.
    bool dedupe_content;
    // When true, sources whose EXIF can't populate the full identity
    // tuple are skipped (counted as skip_incomplete_identity) instead
    // of copied. Default false: uncertain sources are copied as if
    // they were fresh, matching the pre-identity behaviour. Only
    // meaningful when dedupe_content is on.
    bool strict_identity;
} ap_import_settings;

// Aggregate counts for a completed import run.
typedef struct {
    int  imported;                  // files successfully copied
    int  dup_content;               // files skipped: identical content already in library
    int  renamed_collision;         // files renamed to resolve a name collision
    int  skip_collision;            // files skipped: name collision, SKIP policy
    int  skip_incomplete_identity;  // files skipped: insufficient EXIF + strict_identity on
    int  errored;                   // files that failed to copy
    bool cancelled;                 // user requested cancel before the run finished
} ap_import_report;

// Load the library's import settings, filling any unset field with its
// default (subdir "raw", keep original names, suffix-on-collision).
void ap_import_settings_load(const ap_library *lib, ap_import_settings *out);

// Persist the import settings on the library.
void ap_import_settings_save(ap_library *lib, const ap_import_settings *s);

// Progress callback used by ap_import_run_ex. Called after each file
// attempt (copied or skipped) with the number completed so far and the
// total in the collection. `userdata` is the pointer passed to run_ex.
// Return true to continue, false to cancel the run; on cancel the
// importer stops before the next file and sets report->cancelled.
typedef bool (*ap_import_progress_fn)(int done, int total, void *userdata);

// Copy every raw file found under `src_dir` (searched recursively) into
// <lib_root>/<s.subdir>/, applying the naming + collision rules. The
// source files are not modified. Fills `*report` (may be NULL) with
// per-outcome counts. Returns 0 on success, -1 on a setup failure
// (bad arguments, destination not creatable).
int ap_import_run(ap_library *lib, const char *src_dir,
                  const ap_import_settings *s, ap_import_report *report);

// Like ap_import_run but calls `progress` (when non-NULL) after each
// file attempt, passing the progress callback `userdata` through.
int ap_import_run_ex(ap_library *lib, const char *src_dir,
                     const ap_import_settings *s, ap_import_report *report,
                     ap_import_progress_fn progress, void *userdata);

// Like ap_import_run_ex but takes the library root and db path as strings
// instead of an ap_library pointer. Safe to call from a worker thread
// because it does not touch shared library state. `db_path` may be NULL
// to skip content-dedupe (identity lookup).
int ap_import_run_into(const char *lib_root, const char *db_path,
                       const char *src_dir, const ap_import_settings *s,
                       ap_import_report *report,
                       ap_import_progress_fn progress, void *userdata);

#ifdef __cplusplus
}
#endif

#endif
