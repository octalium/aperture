#ifndef APERTURE_APP_ROOT_H
#define APERTURE_APP_ROOT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Application data root, resolved per platform:
//
//   Linux / *BSD:  $XDG_DATA_HOME/aperture, or $HOME/.local/share/aperture
//   macOS:         $HOME/Library/Application Support/aperture
//   Windows:       %APPDATA%/aperture
//
// Cached on first call; valid for the program's lifetime. Returns
// NULL if the platform's standard env vars are unset.
const char *ap_app_root_path(void);

// Ensure the app root and the libraries/ subdirectory exist
// (mkdir -p semantics). Returns 0 on success.
int ap_app_root_ensure(void);

// Build the absolute path of <app_root>/<sub> into `buf`. The
// separator between root and sub is the platform-native one
// ('/' on POSIX, '\\' on Windows). Returns 0 on success.
int ap_app_root_join(const char *sub, char *buf, size_t buflen);

#ifdef __cplusplus
}
#endif

#endif
