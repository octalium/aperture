#ifndef APERTURE_PANELS_H
#define APERTURE_PANELS_H

#include "app/app.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ap_panel ap_panel;

// A single ImGui panel. The draw callback is invoked once per frame
// when the app's current mode matches `mode` (or when `mode` is
// AP_MODE_ANY). The callback owns its own ImGui Begin/End pair.
typedef void (*ap_panel_draw_fn)(ap_app *app);

struct ap_panel {
    const char       *name;
    ap_mode           mode;
    ap_panel_draw_fn  draw;
};

// NULL-terminated array of registered panels. Defined in
// src/panels/registry.c. Adding a panel: create a file in
// src/panels/, declare a `const ap_panel panel_<name>`, add it to
// the registry array, add to src/panels/meson.build.
extern const ap_panel *const ap_panel_registry[];

#ifdef __cplusplus
}
#endif

#endif
