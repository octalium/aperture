#ifndef APERTURE_UI_FILE_DIALOG_H
#define APERTURE_UI_FILE_DIALOG_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Native OS folder picker. Opens the platform's file dialog — on Linux
// via xdg-desktop-portal — and blocks until the user picks or cancels.
//
// On a pick, copies the chosen absolute path into `out` (NUL-
// terminated, truncated if it does not fit) and returns true. Returns
// false on cancel or error. `default_path` seeds the dialog's initial
// directory; pass NULL or "" for the platform default.
bool ap_file_dialog_pick_folder(char *out, size_t out_len,
                                const char *default_path);

#ifdef __cplusplus
}
#endif

#endif
