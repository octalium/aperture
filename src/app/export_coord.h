#ifndef APERTURE_APP_EXPORT_COORD_H
#define APERTURE_APP_EXPORT_COORD_H

/*
 * Export coordinator — the single main-thread, per-frame, incremental
 * state machine that drives single / batch / quick export.
 *
 * Convergence point for every export path. Each export is one
 * user-visible ap_job. The expensive raw decode runs on the worker pool
 * (a small prefetch ahead of the GPU stage); the per-frame pump does only
 * the GPU-bound work that must stay on the main thread — upload, render,
 * readback — then frames the result and submits a CPU-only encode work
 * item. So a batch export no longer blocks the UI on libraw decode.
 *
 * It is the sole owner of the per-photo RGBA allocation gate, so peak
 * RGBA is bounded to AP_EXPORT_BYTE_BUDGET + one photo regardless of how
 * many photos the selection holds (the decode prefetch is separately
 * bounded by a small ahead count). This structurally eliminates the
 * batch-export OOM rather than patching it.
 *
 * Cancellation is the shared ap_job mechanism: the pump checks the job's
 * cancel flag each frame, stops scheduling new decodes/photos, frees
 * pending decoded raws, lets in-flight encodes drain, and finishes
 * CANCELED.
 */

#include "app_priv.h"
#include "output/export.h"

#include <stdbool.h>
#include <stdint.h>

// Soft cap on summed in-flight RGBA buffers. The gate is reactive: the
// coordinator stops opening the next photo once bytes_inflight has
// already reached this budget, so peak is bounded to BUDGET + one photo
// (the photo that pushed it over). bytes_inflight == 0 always opens one
// photo so a single image larger than the budget never deadlocks.
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
// buffer size the encode held; `ok` is whether the file was written (so
// the final tally counts real successes, not just submissions).
void ap_export_coord_encode_done(ap_app *app, size_t bytes, bool ok);

// Hand a completed background decode (a from_coord photo_open_job) to the
// coordinator. On success it takes ownership of j->raw (moved into the
// ready queue for the GPU stage); on failure / cancel / no coordinator it
// frees j->raw. Either way j->raw is consumed — the caller only frees the
// job struct. Main thread (photo-open completion handler).
struct photo_open_job;
void ap_export_coord_decode_complete(ap_app *app, struct photo_open_job *j);

// Tear the coordinator down immediately: request cancel, wait for all
// in-flight encodes to drain, free their buffers and the coordinator.
// Called on shutdown and before any library mutation that would
// invalidate the indices/photos the pump still has queued (sort, delete,
// close) — drain_all_workers calls it so those callers quiesce export
// work, not just the worker pool. Safe when app->export_coord is NULL.
void ap_export_coord_abort(ap_app *app);

#endif /* APERTURE_APP_EXPORT_COORD_H */
