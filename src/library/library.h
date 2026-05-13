#ifndef APERTURE_LIBRARY_H
#define APERTURE_LIBRARY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ap_library ap_library;

// One row from the app-wide library registry.
typedef struct {
    char    id[37];      // RFC 4122 v4 UUID + NUL
    char    path[4096];  // absolute filesystem path
    char    name[128];   // user-set display name; empty if unset
    int64_t created_at;  // unix seconds
} ap_registry_entry;

// Read up to `max` rows from the registry, ordered by most-recent
// created_at. Returns the number written, or -1 on error. Safe to
// call before any library has been opened (creates the registry db
// + schema if needed).
int ap_registry_list(ap_registry_entry *out, int max);

// ----- pipelines (app-wide, registry db) -----

#define AP_PIPELINE_MAX_MODULES 16
#define AP_PIPELINE_NAME_LEN    64
#define AP_PIPELINE_MODULE_LEN  32

typedef struct {
    int64_t id;
    char    name[AP_PIPELINE_NAME_LEN];
    char    modules[AP_PIPELINE_MAX_MODULES][AP_PIPELINE_MODULE_LEN];
    int     module_count;
} ap_pipeline_def;

// Fetch the app-wide default pipeline. The registry is created and
// the default row seeded if either is missing. Returns 0 on success.
int ap_pipeline_get_default(ap_pipeline_def *out);

// Open a directory as a library:
//   - Resolves `path` to an absolute root.
//   - Opens or creates `<root>/library.aperture-db` (SQLite).
//   - Runs schema-create-if-needed for the v1 tables.
//   - Recursively scans the tree for raw files; inserts new ones
//     into the photos table.
//   - Caches the photo list in memory for browsing.
//
// Returns NULL on failure (path not a directory, db open failure,
// out of memory, etc.). On success the caller owns the returned
// pointer until ap_library_close.
ap_library *ap_library_open(const char *path);
void        ap_library_close(ap_library *lib);

const char *ap_library_root(const ap_library *lib);
int         ap_library_photo_count(const ap_library *lib);

// User-set display name. Empty string when unset (callers fall back
// to the basename of the path).
const char *ap_library_name(const ap_library *lib);

// Persist a new display name on the library's registry row. Pass an
// empty string (or NULL) to clear. Returns 0 on success.
int         ap_library_set_name(ap_library *lib, const char *name);

// Relative path of the n-th photo (n in [0, count)). Owned by the
// library; valid until close.
const char *ap_library_photo_relative_path(const ap_library *lib, int index);

// Build the absolute path for the n-th photo into `buf`. Returns 0
// on success, nonzero if the buffer is too small or the index is out
// of range.
int ap_library_photo_absolute_path(const ap_library *lib, int index,
                                   char *buf, size_t buflen);

// ----- thumbnail cache (lifetime-bound to the library) -----

typedef struct ap_thumbnail ap_thumbnail;

// Returns the cached thumbnail for the n-th photo, or NULL if not
// yet decoded.
ap_thumbnail *ap_library_thumbnail(const ap_library *lib, int index);

// Hand a freshly-decoded thumbnail to the library. The library takes
// ownership and will destroy it on close. No-op if `index` is out
// of range.
void ap_library_set_thumbnail(ap_library *lib, int index, ap_thumbnail *t);

// Index of the next photo whose thumbnail hasn't been decoded yet,
// or -1 if every slot is filled. The library does not decide when
// or how decoding happens — callers drive a per-frame pump.
int  ap_library_pending_thumbnail_idx(const ap_library *lib);

#ifdef __cplusplus
}
#endif

#endif
