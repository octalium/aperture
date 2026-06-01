#include "export_coord.h"

#include "jobs.h"

#include "core/job.h"
#include "core/log.h"
#include "edit/viewport.h"
#include "gpu/pipeline_graph.h"
#include "io/raw.h"
#include "photo/photo.h"
#include "ui/status.h"

#include <stdlib.h>
#include <string.h>

// How many photos to decode ahead of the GPU stage. The raw decode runs
// on the worker pool; this bounds how many decoded raws (large buffers)
// may sit in memory waiting for the main-thread GPU stage, so the decode
// prefetch can't reintroduce the OOM the byte budget closed on the encode
// side. Small: just enough to keep a decode in flight while the GPU stage
// works the previous one, so the main thread never blocks on libraw.
#define AP_EXPORT_DECODE_AHEAD 3

// A decoded photo waiting for the main-thread GPU stage. For the live
// open-photo item there is no decode — `use_open` is set and `raw` is
// unused (the GPU stage reads back app->photo directly).
typedef struct {
    int          item;       // index into coord->items
    ap_raw_image raw;        // decoded raw, owned (valid when !use_open)
    bool         use_open;
} export_ready;

// The whole batch's state, owned by ap_app as app->export_coord.
struct ap_export_coord {
    ap_export_settings settings;
    ap_export_item    *items;          // owned; freed on teardown
    int                count;

    int                decode_cursor;  // next item to submit for decode
    int                inflight_decode;// decode jobs submitted, not yet ready
    export_ready       ready[AP_EXPORT_DECODE_AHEAD];
    int                ready_count;     // decoded photos awaiting the GPU stage

    int                processed;       // items accounted (GPU-staged or dropped)
    int                written;         // encodes that actually wrote a file
    int                inflight_encode; // encode work items not yet completed
    size_t             bytes_inflight;  // summed RGBA buffers allocated
    int                max_inflight;    // concurrent-encode cap (worker count)
    ap_job            *job;             // user-visible job (label/progress/cancel)
};

ap_export_coord *ap_export_coord_create(ap_app *app, ap_export_item *items,
                                        int count, const ap_export_settings *s)
{
    if (!app || !items || count <= 0 || !s) return NULL;

    ap_export_coord *c = calloc(1, sizeof(*c));
    if (!c) return NULL;

    c->settings = *s;
    c->items    = items;
    c->count    = count;

    int n_threads = ap_worker_pool_thread_count(app->workers);
    c->max_inflight = n_threads > 0 ? n_threads : 1;

    char label[64];
    snprintf(label, sizeof(label), "Exporting %d photo%s",
             count, count == 1 ? "" : "s");
    c->job = ap_job_begin(AP_JOB_KIND_EXPORT, label, count, NULL);
    if (!c->job) {
        free(c);
        return NULL;
    }
    return c;
}

// Free any decoded raws still queued for the GPU stage. Used on cancel +
// teardown so a pending prefetch never leaks.
static void free_ready(ap_export_coord *c)
{
    for (int i = 0; i < c->ready_count; i++) {
        if (!c->ready[i].use_open) ap_raw_image_free(&c->ready[i].raw);
    }
    c->ready_count = 0;
}

// Free the coordinator and clear the app slot. The job block is freed
// by the caller after ap_job_finish (matching the registry lifetime
// contract); here we only release coordinator-owned memory.
static void coord_free(ap_app *app, ap_export_coord *c)
{
    free_ready(c);
    free(c->items);
    free(c->job);
    free(c);
    app->export_coord = NULL;
}

// Submit a background raw decode for items[idx] to the worker pool. The
// completion (handle_photo_open_complete, from_coord) hands the decoded
// raw back via ap_export_coord_decode_complete. Returns true on submit.
static bool submit_decode(ap_app *app, ap_export_coord *c, int idx)
{
    photo_open_job *j = calloc(1, sizeof(*j));
    if (!j) {
        AP_ERROR("export: decode job alloc failed");
        return false;
    }
    j->base.run    = photo_open_job_run;
    j->from_coord  = true;
    j->coord_item  = idx;
    snprintf(j->path, sizeof(j->path), "%s", c->items[idx].src_abs);
    ap_worker_pool_submit(app->workers, &j->base);
    return true;
}

// GPU stage for one ready photo: open it from the (already-decoded) raw
// or read back the live open photo, render, read back, frame, and submit
// a CPU encode. Returns the framed byte size on success (the caller adds
// it to bytes_inflight), 0 on a skip/failure. `r->raw` is consumed.
static size_t gpu_process_and_submit(ap_app *app, ap_export_coord *c,
                                     export_ready *r)
{
    const ap_export_item *it = &c->items[r->item];

    ap_photo *photo = NULL;
    bool own_photo = false;
    if (r->use_open) {
        // Read back the live open photo (its in-memory edits) — but only
        // if app->photo is still the photo this item was resolved for. A
        // photo-open completing between resolve and this GPU stage could
        // have swapped app->photo; fall back to opening the intended
        // source from disk rather than export the wrong photo.
        if (app->photo &&
            strcmp(ap_photo_path(app->photo), it->src_abs) == 0) {
            photo = app->photo;
        } else {
            photo = ap_photo_open(app->gpu, it->src_abs);
            own_photo = true;
        }
    } else {
        // Consumes r->raw on every path (success or failure).
        photo = ap_photo_open_with_raw(app->gpu, it->src_abs, &r->raw);
        own_photo = true;
    }
    if (!photo) {
        AP_WARN("export: cannot open %s — skipping", it->src_abs);
        return 0;
    }

    int w = ap_photo_width(photo);
    int h = ap_photo_height(photo);
    if (w <= 0 || h <= 0) {
        if (own_photo) ap_photo_close(photo);
        return 0;
    }

    size_t bytes = (size_t)w * (size_t)h * 4u;
    uint8_t *rgba = malloc(bytes);
    if (!rgba) {
        AP_ERROR("export: out of memory (%zu bytes)", bytes);
        if (own_photo) ap_photo_close(photo);
        return 0;
    }
    // Render the compute chain into display_image before reading it back.
    // An export-only photo is never the bound current_graph, so the frame
    // loop never renders it — without this the readback copies undefined
    // (black) memory. The open photo is already rendered by the frame
    // loop; render_once is a near-no-op for it (record skips on an
    // unchanged stack).
    if (ap_photo_render(photo) != 0) {
        AP_WARN("export: render failed for %s — skipping", it->src_abs);
        free(rgba);
        if (own_photo) ap_photo_close(photo);
        return 0;
    }
    if (ap_pipeline_graph_readback(ap_photo_graph(photo), rgba, bytes) != 0) {
        free(rgba);
        if (own_photo) ap_photo_close(photo);
        return 0;
    }

    // Frame through the viewport (crop / straighten). CPU-only; the
    // readback above already issued vkDeviceWaitIdle on this graph.
    {
        ap_viewport vp = ap_photo_viewport(photo);
        int fw = 0, fh = 0;
        uint8_t *framed = ap_viewport_resample_rgba8(&vp, rgba, w, h, &fw, &fh);
        if (framed) {
            free(rgba);
            rgba  = framed;
            w     = fw;
            h     = fh;
            bytes = (size_t)w * (size_t)h * 4u;
        }
    }

    // Close without persisting: the sidecar + thumbnail are untouched
    // and the readback left the graph idle.
    if (own_photo) ap_photo_close(photo);

    export_job *j = calloc(1, sizeof(*j));
    if (!j) {
        AP_ERROR("export: encode job alloc failed");
        free(rgba);
        return 0;
    }
    j->base.run      = export_job_run;
    j->rgba          = rgba;
    j->width         = w;
    j->height        = h;
    j->format        = c->settings.format;
    j->jpeg_quality  = c->settings.jpeg_quality;
    j->png_depth     = c->settings.png_depth;
    j->tiff_depth    = c->settings.tiff_depth;
    j->tiff_compress = c->settings.tiff_compress;
    snprintf(j->out_path, sizeof(j->out_path), "%s", it->out_path);
    j->status_id  = 0;          // the coordinator's job owns the bar
    j->from_coord = true;
    j->rgba_bytes = bytes;

    ap_worker_pool_submit(app->workers, &j->base);
    return bytes;
}

void ap_export_coord_pump(ap_app *app)
{
    if (!app) return;
    ap_export_coord *c = app->export_coord;
    if (!c) return;

    bool canceling = ap_job_cancel_requested(c->job);

    // On cancel, drop any decoded raws waiting for the GPU stage so they
    // don't leak while we wait for in-flight decodes/encodes to drain.
    if (canceling && c->ready_count > 0) {
        free_ready(c);
    }

    // Terminal: nothing left to decode, no decode/encode in flight, and
    // no decoded photo waiting for the GPU stage.
    bool no_more_to_schedule = (c->decode_cursor >= c->count) || canceling;
    if (no_more_to_schedule && c->inflight_decode == 0 &&
        c->ready_count == 0 && c->inflight_encode == 0) {
        ap_job_finish(c->job, canceling ? AP_JOB_CANCELED : AP_JOB_DONE);
        if (canceling) {
            ap_status_notify(AP_STATUS_INFO, "Export canceled.");
        } else if (c->written == c->count) {
            ap_status_notify(AP_STATUS_INFO, "Export complete: %d photo%s.",
                             c->written, c->written == 1 ? "" : "s");
        } else {
            // Some photos failed to decode / render / encode; the count
            // would otherwise overstate success. The log has the details.
            ap_status_notify(AP_STATUS_ERROR,
                             "Exported %d of %d photo%s — see the log.",
                             c->written, c->count, c->count == 1 ? "" : "s");
        }
        coord_free(app, c);
        return;
    }

    if (canceling) return;  // schedule nothing new; let work drain

    // Decode prefetch: keep up to AHEAD photos decoded or decoding. The
    // live open-photo item needs no decode — it goes straight to ready.
    while (c->inflight_decode + c->ready_count < AP_EXPORT_DECODE_AHEAD &&
           c->decode_cursor < c->count) {
        int idx = c->decode_cursor;
        if (c->items[idx].use_open_photo) {
            c->ready[c->ready_count].item     = idx;
            c->ready[c->ready_count].use_open = true;
            c->ready_count++;
            c->decode_cursor++;
        } else if (submit_decode(app, c, idx)) {
            c->inflight_decode++;
            c->decode_cursor++;
        } else {
            break;  // alloc failure — retry next frame
        }
    }

    // GPU stage: process one decoded photo per pump, gated by the encode
    // cap + the RGBA byte budget (bytes_inflight == 0 always processes one
    // so a single oversized photo never deadlocks — peak is BUDGET + one).
    if (c->ready_count == 0) return;
    if (c->inflight_encode >= c->max_inflight) return;
    if (c->bytes_inflight > 0 && c->bytes_inflight >= AP_EXPORT_BYTE_BUDGET)
        return;

    export_ready r = c->ready[0];
    for (int i = 1; i < c->ready_count; i++) c->ready[i - 1] = c->ready[i];
    c->ready_count--;

    size_t bytes = gpu_process_and_submit(app, c, &r);
    if (bytes > 0) {
        c->bytes_inflight += bytes;
        c->inflight_encode++;
    }
    c->processed++;
    ap_job_progress(c->job, c->processed, c->count);
}

void ap_export_coord_encode_done(ap_app *app, size_t bytes, bool ok)
{
    if (!app || !app->export_coord) return;
    ap_export_coord *c = app->export_coord;
    if (c->bytes_inflight >= bytes) c->bytes_inflight -= bytes;
    else                            c->bytes_inflight = 0;
    if (c->inflight_encode > 0) c->inflight_encode--;
    if (ok) c->written++;
}

void ap_export_coord_decode_complete(ap_app *app, struct photo_open_job *j)
{
    photo_open_job *job = (photo_open_job *)j;
    ap_export_coord *c = app ? app->export_coord : NULL;

    // No coordinator (already torn down), the decode failed, or we are
    // canceling: discard the raw. Otherwise hand it to the ready queue.
    if (!c) {
        ap_raw_image_free(&job->raw);
        return;
    }
    if (c->inflight_decode > 0) c->inflight_decode--;

    if (!job->ok || ap_job_cancel_requested(c->job) ||
        c->ready_count >= AP_EXPORT_DECODE_AHEAD) {
        ap_raw_image_free(&job->raw);
        // A genuine decode failure (not a cancel) still retires the item —
        // count it so the progress bar reaches 100% and the final tally is
        // right. Cancel reports its own message, so don't advance there.
        if (!job->ok && !ap_job_cancel_requested(c->job)) {
            c->processed++;
            ap_job_progress(c->job, c->processed, c->count);
        }
        return;
    }

    c->ready[c->ready_count].item     = job->coord_item;
    c->ready[c->ready_count].raw      = job->raw;   // move ownership
    c->ready[c->ready_count].use_open = false;
    c->ready_count++;
    memset(&job->raw, 0, sizeof(job->raw));         // prevent a double-free
}

void ap_export_coord_abort(ap_app *app)
{
    if (!app || !app->export_coord) return;
    ap_export_coord *c = app->export_coord;

    ap_job_request_cancel(c->job);

    // Wait for every in-flight decode + encode to finish, then drain all
    // the completed items: the coordinator's encodes free their RGBA
    // buffers (and settle inflight_encode), its decodes free their raws,
    // any other completed work goes through its normal discard arm.
    // wait_idle guarantees nothing is still running before we free the
    // coordinator.
    if (app->workers) {
        ap_worker_pool_wait_idle(app->workers);
        for (;;) {
            ap_work_item *it = ap_worker_pool_poll(app->workers);
            if (!it) break;
            if (it->run == export_job_run && ((export_job *)it)->from_coord) {
                export_job *j = (export_job *)it;
                ap_export_coord_encode_done(app, j->rgba_bytes, j->ok);
                free(j->rgba);
                free(j);
            } else if (it->run == photo_open_job_run &&
                       ((photo_open_job *)it)->from_coord) {
                photo_open_job *j = (photo_open_job *)it;
                ap_raw_image_free(&j->raw);
                free(j);
            } else {
                discard_completed_item(app, it);
            }
        }
    }

    ap_job_finish(c->job, AP_JOB_CANCELED);
    coord_free(app, c);
}
