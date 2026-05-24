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

// Application config root, resolved per platform:
//
//   Linux / *BSD:  $XDG_CONFIG_HOME/aperture, or $HOME/.config/aperture
//   macOS:         $HOME/Library/Application Support/aperture
//   Windows:       %APPDATA%/aperture
//
// Holds user-facing configuration (UI layout, preferences). On macOS
// and Windows this collapses onto the data root, matching platform
// convention.
const char *ap_app_config_path(void);

// Ensure the app root and the libraries/ subdirectory exist
// (mkdir -p semantics). Returns 0 on success.
int ap_app_root_ensure(void);

// Ensure the app config root exists (mkdir -p). Returns 0 on success.
int ap_app_config_ensure(void);

// Build the absolute path of <app_root>/<sub> into `buf`. The
// separator between root and sub is the platform-native one
// ('/' on POSIX, '\\' on Windows). Returns 0 on success.
int ap_app_root_join(const char *sub, char *buf, size_t buflen);

// Build the absolute path of <app_config>/<sub> into `buf`. Returns 0
// on success.
int ap_app_config_join(const char *sub, char *buf, size_t buflen);

// Create `path` and all intermediate directories (mkdir -p semantics).
// Returns 0 on success; -1 if any component cannot be created.
int ap_mkdir_p(const char *path);

#ifdef __cplusplus
}
#endif

#endif
