#ifndef APERTURE_UI_STATUS_H
#define APERTURE_UI_STATUS_H

#include "core/compat.h"  // AP_PRINTF_FMT

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AP_STATUS_INFO  = 0,
    AP_STATUS_ERROR = 1,
} ap_status_kind;

// Opaque handle returned by ap_status_progress_begin. Pass it to the
// progress-update and finish calls. Invalid (zero) means allocation
// failed; callers may ignore it safely — all calls on an invalid handle
// are no-ops.
typedef unsigned int ap_status_id;

// Push a transient notification. It fades after a few seconds.
// Thread-safe: may be called from any thread.
void ap_status_notify(ap_status_kind kind, const char *fmt, ...)
    AP_PRINTF_FMT(2, 3);

// Register a new in-flight progress entry. `label` is copied; it is
// shown next to the progress bar while the operation is running.
// `total` is the expected number of units (files, photos, …); use 0
// when the total is not yet known (renders as an indeterminate bar).
// Returns a non-zero id on success; 0 if the entry could not be
// allocated (callers should treat 0 as "no tracking").
// Thread-safe: may be called from any thread.
ap_status_id ap_status_progress_begin(const char *label, int total);

// Update the "done" counter and, when total > 0, the expected total for
// an in-flight entry. `done` is the new absolute count of completed
// units (not a delta); `total` is the expected total — pass 0 to leave
// the existing total unchanged.
// Thread-safe: may be called from any thread.
void ap_status_progress_update(ap_status_id id, int done, int total);

// Mark an in-flight entry complete. The bar disappears after a brief
// hold so the user sees it finish. `ok` selects the completion colour.
// Thread-safe: may be called from any thread.
void ap_status_progress_finish(ap_status_id id, int ok);

// Render the status surface. Call once per frame from ap_app_run_frame
// after all panels. Main thread only.
void ap_status_draw(void);

#ifdef __cplusplus
}
#endif

#endif
