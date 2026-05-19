#ifndef APERTURE_APP_H
#define APERTURE_APP_H

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

// Library lifecycle. Opening a library transitions to AP_MODE_LIBRARY
// and closes any currently-open photo. Opening a different library
// closes the previous one.
int         ap_app_open_library(ap_app *app, const char *path);
void        ap_app_close_library(ap_app *app);
ap_library *ap_app_library(ap_app *app);

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

#ifdef __cplusplus
}
#endif

#endif
