#ifndef APERTURE_CORE_DIR_H
#define APERTURE_CORE_DIR_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// portable directory iteration. wraps POSIX opendir/readdir/closedir
// on unix and the FindFirstFileW/FindNextFileW state machine on
// windows behind one cursor type. yields entry names only ("." and
// ".." inclusive, matching readdir); callers stat the full child path
// themselves to classify entries, so behaviour is identical on every
// target.
typedef struct ap_dir ap_dir;

// open `path` for iteration. returns NULL if the directory cannot be
// opened; the caller distinguishes "absent" from "error" via
// ap_dir_open_errno (which mirrors errno: ENOENT for a missing dir).
ap_dir *ap_dir_open(const char *path);

// errno-style code from the most recent ap_dir_open failure on this
// thread. ENOENT means the directory does not exist. valid only
// immediately after ap_dir_open returned NULL.
int ap_dir_open_errno(void);

// advance to the next entry. returns its name (valid until the next
// ap_dir_read or ap_dir_close on the same handle) or NULL at the end
// of the directory. the name is UTF-8 on every platform.
const char *ap_dir_read(ap_dir *d);

// release the handle. NULL is a no-op.
void ap_dir_close(ap_dir *d);

#ifdef __cplusplus
}
#endif

#endif
