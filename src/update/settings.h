#ifndef APERTURE_UPDATE_SETTINGS_H
#define APERTURE_UPDATE_SETTINGS_H

#include "update/check.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Persisted update-flow preferences. Loaded once at app init,
// mutated in place by the Preferences modal, and written back via
// ap_update_settings_save. Defaults live in `_load` (first-run
// users have no settings rows yet).
typedef struct {
    bool check_on_launch;
    char manifest_url[1024];
} ap_update_settings;

// Read settings into `out`. Missing keys fall back to defaults
// (check_on_launch on, manifest_url empty -> the built-in default
// URL is used by the check submitter).
void ap_update_settings_load(ap_update_settings *out);

// Persist `s` to the app-wide settings store.
void ap_update_settings_save(const ap_update_settings *s);

#ifdef __cplusplus
}
#endif

#endif
