#ifndef APERTURE_APP_ROOT_H
#define APERTURE_APP_ROOT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Application data root (XDG-style on Linux):
//   $XDG_DATA_HOME/aperture, or $HOME/.local/share/aperture as a fallback.
//
// Cached on first call; valid for the program's lifetime. Returns NULL
// only if neither environment variable is set (extremely unusual).
const char *ap_app_root_path(void);

// Ensure the app root and the libraries/ subdirectory exist
// (mkdir -p semantics). Returns 0 on success.
int ap_app_root_ensure(void);

// Build the absolute path of <app_root>/<sub> into `buf`. Returns 0
// on success, nonzero if the buffer is too small.
int ap_app_root_join(const char *sub, char *buf, size_t buflen);

#ifdef __cplusplus
}
#endif

#endif
