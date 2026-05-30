#ifndef APERTURE_CORE_FS_H
#define APERTURE_CORE_FS_H

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

#ifdef __cplusplus
}
#endif

#endif
