#ifndef APERTURE_CORE_FS_H
#define APERTURE_CORE_FS_H

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// atomically replace `dst` with `src`, overwriting `dst` if it exists.
// this is exactly POSIX rename() semantics; the windows CRT rename()
// instead fails when `dst` exists, so callers that depend on the
// replace (the tmp-file + rename atomic-write pattern) must route
// through here. returns 0 on success, -1 on failure.
//
// on failure across volumes errno is set to EXDEV so callers can fall
// back to copy + unlink, matching the unix contract.
int ap_rename_replace(const char *src, const char *dst);

// Atomic-write session. The canonical "write a temp file, then durably
// swap it into place" pattern, in one place so every writer gets the
// same crash-safety — no half-written file is ever visible at the final
// path, and a successful commit survives a power loss. Use it like:
//
//     ap_atomic *a = ap_atomic_open(path);
//     if (!a) return -1;
//     if (write_everything(ap_atomic_file(a)) != 0) { ap_atomic_abort(a); return -1; }
//     if (ap_atomic_commit(a) != 0) return -1;
//
typedef struct ap_atomic ap_atomic;

// Open a uniquely-named temp file beside `final_path` for writing.
// Returns NULL on error.
ap_atomic *ap_atomic_open(const char *final_path);

// The stream to write the new contents to. Valid until commit/abort.
FILE *ap_atomic_file(ap_atomic *a);

// fflush + fsync the temp, close it, rename it over `final_path`, and
// fsync the parent directory so the replacement survives a crash.
// Frees `a`. Returns 0 on success; on any failure the temp is removed,
// `final_path` is left untouched, and -1 is returned.
int ap_atomic_commit(ap_atomic *a);

// Discard the temp and free `a`. Safe on NULL.
void ap_atomic_abort(ap_atomic *a);

// Path-based variant for writers that must open the output by path
// (e.g. libtiff's TIFFOpen). Fill `out` with a unique temp path beside
// `final_path`; write+close that path yourself, then commit/discard it.
// Returns 0 on success, -1 if the path would not fit.
int ap_atomic_temp_path(const char *final_path, char *out, size_t out_size);

// Durably commit a temp written via ap_atomic_temp_path: fsync it,
// rename over `final_path`, fsync the parent dir. Returns 0 on success;
// on failure the temp is removed and -1 returned.
int ap_atomic_commit_temp(const char *temp_path, const char *final_path);

// Discard a temp from ap_atomic_temp_path (unlink). Safe on NULL.
void ap_atomic_discard_temp(const char *temp_path);

#ifdef __cplusplus
}
#endif

#endif
