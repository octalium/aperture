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
    int64_t created_at;  // unix seconds
} ap_registry_entry;

// Read up to `max` rows from the registry, ordered by most-recent
// created_at. Returns the number written, or -1 on error. Safe to
// call before any library has been opened (creates the registry db
// + schema if needed).
int ap_registry_list(ap_registry_entry *out, int max);

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

// Relative path of the n-th photo (n in [0, count)). Owned by the
// library; valid until close.
const char *ap_library_photo_relative_path(const ap_library *lib, int index);

// Build the absolute path for the n-th photo into `buf`. Returns 0
// on success, nonzero if the buffer is too small or the index is out
// of range.
int ap_library_photo_absolute_path(const ap_library *lib, int index,
                                   char *buf, size_t buflen);

#ifdef __cplusplus
}
#endif

#endif
