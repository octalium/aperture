#ifndef APERTURE_APP_H
#define APERTURE_APP_H

#include "edit/stack.h"
#include "library/library.h"
#include "output/export.h"
#include "photo/metadata.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AP_MODE_LIBRARY = 0,
    AP_MODE_PHOTO   = 1,
    AP_MODE_EXPORT  = 2,
} ap_mode;

// Sentinel for panels that should appear in every mode (info / FPS / etc.).
#define AP_MODE_ANY  ((ap_mode)-1)

// Library-grid group filter. AP_GROUP_FILTER_GROUP pairs with a group
// name; ALL and UNGROUPED ignore the name.
typedef enum {
    AP_GROUP_FILTER_ALL       = 0,
    AP_GROUP_FILTER_UNGROUPED = 1,
    AP_GROUP_FILTER_GROUP     = 2,
} ap_group_filter;

typedef struct ap_app     ap_app;
typedef struct ap_photo   ap_photo;
typedef struct ap_library ap_library;

// Top-level application: owns the gpu, the current mode, and the
// currently-open photo (if any).
ap_app *ap_app_create(int width, int height, const char *title);
void    ap_app_destroy(ap_app *app);

// Main loop helpers.
bool ap_app_should_run(ap_app *app);
int  ap_app_run_frame(ap_app *app);
void ap_app_wait_idle(ap_app *app);

// Mode access. Default at create-time is AP_MODE_LIBRARY; opening a
// photo transitions to AP_MODE_PHOTO automatically; closing the photo
// transitions back to AP_MODE_LIBRARY.
ap_mode ap_app_mode(const ap_app *app);
void    ap_app_set_mode(ap_app *app, ap_mode mode);

// Photo lifecycle. open_photo is asynchronous - the worker pool
// decodes the raw on a background thread; the photo binds to the
// canvas on a later frame once decode + GPU upload finish. Closing
// or opening another photo while one is loading invalidates the
// in-flight load.
int       ap_app_open_photo(ap_app *app, const char *path);
void      ap_app_close_photo(ap_app *app);
ap_photo *ap_app_photo(ap_app *app);
bool      ap_app_photo_loading(const ap_app *app);

// Loupe canvas. Panels need it to rebind after a pipeline-graph
// rebuild (the new graph has different output VkImageView / sampler).
typedef struct ap_canvas ap_canvas;
ap_canvas *ap_app_canvas(ap_app *app);

// Rebuild the open photo's pipeline graph from its current edit
// stack and re-point everything that referenced the old graph (the
// GPU's current-graph pointer + the canvas binding). Panels call
// this after any structural edit-stack change; the View Raw toggle
// calls it too. No-op when no photo is open.
void ap_app_rebuild_photo_graph(ap_app *app);

// Synchronous GPU readback + asynchronous JPEG encode+write. Returns
// 0 if the readback succeeded and the encode job was queued.
int       ap_app_request_jpeg_export(ap_app *app, ap_photo *photo,
                                     const char *out_path, int quality);

// Export mode. AP_MODE_EXPORT shows the open photo on the canvas with
// the contextual Format / Quality / Naming / Destination panels. The
// app owns one ap_export_settings struct, loaded from the library on
// entry and saved back on a successful export; the panels mutate it
// in place.
ap_export_settings *ap_app_export_settings(ap_app *app);

// Enter Export mode. Requires an open photo (it is the export
// subject). Loads the export settings from the library. No-op when no
// photo is open.
void ap_app_enter_export(ap_app *app);

// Resolve the export settings into a concrete output path for the
// currently-open photo and run the encode. Performs a synchronous GPU
// readback, then queues the format-appropriate encode job on a worker.
// Honours the collision policy (overwrite / auto-suffix / skip).
// Returns 0 when the encode job was queued, 1 when the export was
// skipped by the collision policy, -1 on error.
int ap_app_run_export(ap_app *app);

// Library lifecycle. Opening a library transitions to AP_MODE_LIBRARY
// and closes any currently-open photo. Opening a different library
// closes the previous one.
int         ap_app_open_library(ap_app *app, const char *path);
void        ap_app_close_library(ap_app *app);
ap_library *ap_app_library(ap_app *app);
// Open the Import Photos dialog on the next frame (sets the same flag
// the File > Import menu item uses). No-op when no library is open.
void        ap_app_request_import(ap_app *app);

// Apply a metadata-override patch to every photo currently in the
// library grid's selection set. The bulk-edit panel + the photo-mode
// Sync-to-selection button both route through here. Returns the
// number of photos written, or -1 if no library / no grid.
int ap_app_apply_metadata_to_selection(ap_app *app,
                                       const ap_photo_metadata *patch,
                                       const bool patch_set[AP_META_FIELD_COUNT]);

// Replace the edit stack of every selected photo with the contents
// of the named pipeline. Skips the currently-open photo to avoid
// desyncing its in-memory stack from the sidecar; reopening the
// photo picks up the rewritten stack. Returns the number written,
// or -1 on error.
int ap_app_apply_pipeline_to_selection(ap_app *app, int64_t pipeline_id);

// Culling: set the rating / pick-reject flag / colour label on every
// photo currently in the library grid's selection. Each call touches
// one culling field and leaves the other two unchanged. The grid
// keyboard shortcuts route through here. Returns the number of photos
// written, or -1 when there is no library / grid.
int ap_app_set_selection_rating(ap_app *app, int rating);
int ap_app_set_selection_flag(ap_app *app, ap_flag flag);
int ap_app_set_selection_color(ap_app *app, ap_color_label color);

// Library-grid group filter. Setting it rebuilds the visible grid;
// the Groups panel reads the active filter back to highlight it.
// `kind` is an ap_group_filter; `name` is used only for
// AP_GROUP_FILTER_GROUP.
void        ap_app_set_group_filter(ap_app *app, int kind, const char *name);
int         ap_app_group_filter_kind(const ap_app *app);
const char *ap_app_group_filter_name(const ap_app *app);

// Edit clipboard: copy the open photo's stack into an in-memory
// clipboard and paste it back onto the open photo, replacing its stack.
// ap_app_copy_edits returns -1 when no photo is open. ap_app_paste_edits
// returns -1 when the clipboard is empty or no photo is open; on success
// the photo's pipeline graph is rebuilt immediately.
int         ap_app_copy_edits(ap_app *app);
int         ap_app_paste_edits(ap_app *app);

// Undo / redo the most recent edit-stack mutation on the open photo.
// Both snapshot before mutating (caller responsibility) and rebuild the
// pipeline graph. Return true when a state was available; false = no-op.
bool ap_app_undo(ap_app *app);
bool ap_app_redo(ap_app *app);

// Snapshot the open photo's current edit stack into its undo history.
// Must be called before every stack mutation so the mutation is undoable.
// No-op when no photo is open.
void ap_app_edit_snapshot(ap_app *app);

// True when the edit clipboard holds a stack (i.e. copy has been called
// at least once).
bool        ap_app_has_edit_clipboard(const ap_app *app);

// Apply the edit clipboard's stack to every selected library photo's
// sidecar, skipping the currently-open photo. Mirrors the pipeline-to-
// selection path. Returns the number of photos written, or -1 on error
// (no library, no grid, or empty clipboard).
int         ap_app_sync_edits_to_selection(ap_app *app);

// Number of photos currently selected in the library grid.
int         ap_app_grid_selection_count(const ap_app *app);

// Add (add=true) or remove (add=false) every selected grid photo
// to / from `group`. Returns the number of photos affected, or -1 on
// error.
int         ap_app_assign_selection_to_group(ap_app *app,
                                             const char *group, bool add);

// Library-grid sort order. Setting a new sort reloads the photo list
// from the db and rebuilds the grid map; all in-flight thumbnail jobs
// are drained first. No-op when the sort key is already active.
void            ap_app_set_sort(ap_app *app, ap_library_sort sort);
ap_library_sort ap_app_sort(const ap_app *app);

// Library-grid search filter. `query` is a case-insensitive substring
// matched against each photo's relative path. An empty or NULL query
// shows all photos. Rebuilds the grid map immediately.
void        ap_app_set_search(ap_app *app, const char *query);
const char *ap_app_search(const ap_app *app);

// Before/after compare: bypass all user-edit stages in the current
// photo's pipeline graph without a graph rebuild. While active the
// canvas shows the demosaic'd, unedited image (only the transport
// stages — demosaic + output_transfer — run). Stages with no edit-
// stack entry (transport stages, entry_idx == -1) are always left
// running so the display pipeline stays coherent.
//
// Calling with `on = true` skips every user-edit stage; `on = false`
// restores them. The bypass is not persisted — it resets automatically
// when the photo is closed or replaced.
//
// No-op when no photo is open or the photo has no pipeline graph yet.
void ap_app_set_compare_original(ap_app *app, bool on);
bool ap_app_compare_original(const ap_app *app);

#ifdef __cplusplus
}
#endif

#endif
