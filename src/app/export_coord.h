#ifndef APERTURE_APP_EXPORT_COORD_H
#define APERTURE_APP_EXPORT_COORD_H

/*
 * Export coordinator — the single main-thread, per-frame, incremental
 * state machine that drives single / batch / quick export.
 *
 * Convergence point for every export path. Each export is one
 * user-visible ap_job; the coordinator opens one photo per pump step
 * (on the main thread, where the GPU readback must happen), frames it,
 * and submits a CPU-only encode work item to the worker pool.
 *
 * It is the sole owner of the per-photo RGBA allocation gate, so peak
 * memory is bounded to AP_EXPORT_BYTE_BUDGET + one photo regardless of
 * how many photos the selection holds. This structurally eliminates the
 * batch-export OOM rather than patching it.
 *
 * Cancellation is the shared ap_job mechanism: the pump checks the
 * job's cancel flag each frame, stops opening new photos, lets in-flight
 * encodes drain, frees their buffers, and finishes CANCELED.
 */

#include "app_priv.h"
#include "output/export.h"

#include <stdbool.h>
#include <stdint.h>

// Hard cap on summed in-flight RGBA buffers. The coordinator never
// opens the next photo while bytes_inflight + est_next would exceed it.
#define AP_EXPORT_BYTE_BUDGET ((size_t)512u * 1024u * 1024u)

// One photo queued for export. Paths are precomputed at submit time
// (pure CPU, no GPU, no per-photo allocation) so the pump only does the
// memory-bounded open/readback/frame/encode work.
typedef struct {
    char src_abs[4096];   // absolute source path to open
    char out_path[4096];  // resolved, collision-checked output path
    bool use_open_photo;  // read back app->photo instead of opening src
} ap_export_item;

// Create the coordinator for `items` (ownership transferred; freed on
// teardown), opening one ap_job of kind EXPORT. Returns NULL on
// allocation failure (caller frees `items`). `count` must be > 0.
ap_export_coord *ap_export_coord_create(ap_app *app, ap_export_item *items,
                                        int count, const ap_export_settings *s);

// Advance the coordinator by one bounded step. Opens at most one photo
// per call, honouring the byte budget and the concurrent-encode cap.
// When the work is fully drained (or cancellation has drained), finishes
// the job, frees the coordinator, and clears app->export_coord. Call
// once per frame from the main thread, next to drain_one_completed_job.
void ap_export_coord_pump(ap_app *app);

// Account a completed encode work item against the coordinator: drop its
// bytes from the in-flight total and decrement the inflight count.
// Called from the export-job completion handler. `bytes` is the RGBA
// buffer size the encode held.
void ap_export_coord_encode_done(ap_app *app, size_t bytes);

// Tear the coordinator down on shutdown: request cancel, pump until all
// in-flight encodes drain, free buffers and the coordinator. Safe when
// app->export_coord is NULL.
void ap_export_coord_shutdown(ap_app *app);

#endif /* APERTURE_APP_EXPORT_COORD_H */
