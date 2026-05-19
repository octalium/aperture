#ifndef APERTURE_APP_LAYOUT_PROFILES_H
#define APERTURE_APP_LAYOUT_PROFILES_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Named ImGui layout state stored under <app_root>/layouts/.
//
// ap_imgui_init turns off ImGui's auto-persist; this module owns
// explicit load/save and the "what's currently active" pointer.
// Drags during a session don't reach disk unless the user clicks
// "Save Current As…".

#define AP_LAYOUT_NAME_LEN  64
#define AP_LAYOUT_MAX_ENUM  64

// Resolve <app_root>/layouts, create if missing, and load whatever
// profile the .current pointer named (if any). Safe to call before
// the dockspace exists — the load just stashes ini data; ImGui
// applies it when the windows are next drawn. Returns 0 on success.
int  ap_layout_init(void);

// Snapshot of saved profile names. Returns count written (≤ max),
// or -1 on error. Order is filesystem readdir; sort upstream if
// you want stable display.
int  ap_layout_list(char names[][AP_LAYOUT_NAME_LEN], int max);

// Name of the active profile, or empty string for "no profile —
// running on whatever the dock builder produced".
const char *ap_layout_active_name(void);

// Make `name` the active profile: persist the .current pointer,
// wipe runtime ImGui settings, load the named profile from disk.
// Returns 0 on success.
int  ap_layout_set_active(const char *name);

// Capture the current ImGui state to a profile named `name`
// (overwriting if present) and make it active.
int  ap_layout_save_current_as(const char *name);

// Reload the active profile from disk, dropping any in-session
// edits. No-op if no profile is active.
int  ap_layout_reload_active(void);

// Drop runtime settings + clear the .current pointer. Sets the
// "dock builder should rebuild" flag (read via the function below);
// the dockspace setup in run_frame consults this each frame so it
// can re-run the default builder without a restart.
void ap_layout_reset_to_default(void);

// Read-and-consume the "rebuild default dock layout" flag. Returns
// true at most once after a reset; the dock builder uses it to
// gate re-running on top of the previous frame's layout.
bool ap_layout_consume_rebuild_request(void);

#ifdef __cplusplus
}
#endif

#endif
