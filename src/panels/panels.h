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
    // Optional. When non-NULL, the registry runner only invokes `draw`
    // while `*visible` is true, and the menubar's Edit menu shows a
    // checkable toggle bound to the same address (using `menu_label`).
    // Panels passing this should also pass the same pointer to
    // `igBegin` as the second argument so the title-bar X stays in sync.
    bool             *visible;
    const char       *menu_label;
};

// NULL-terminated array of registered panels. Defined in
// src/panels/panel_table.c. Adding a panel: create a file in
// src/panels/, declare a `const ap_panel panel_<name>`, add it to
// the registry array, add to src/panels/meson.build.
extern const ap_panel *const ap_panel_registry[];

#ifdef __cplusplus
}
#endif

#endif
