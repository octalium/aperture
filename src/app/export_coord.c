#include "export_coord.h"

#include "jobs.h"

#include "core/job.h"
#include "core/log.h"
#include "edit/viewport.h"
#include "gpu/pipeline_graph.h"
#include "photo/photo.h"
#include "ui/status.h"

#include <stdlib.h>
#include <string.h>

// The whole batch's state, owned by ap_app as app->export_coord.
struct ap_export_coord {
    ap_export_settings settings;
    ap_export_item    *items;          // owned; freed on teardown
    int                count;
    int                cursor;         // next item to open + read back
    int                inflight_encode;// encode work items not yet completed
    size_t             bytes_inflight; // summed RGBA buffers allocated
    int                max_inflight;   // concurrent-encode cap (worker count)
    ap_job            *job;            // user-visible job (label/progress/cancel)
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
    c->cursor   = 0;

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

// Free the coordinator and clear the app slot. The job block is freed
// by the caller after ap_job_finish (matching the registry lifetime
// contract); here we only release coordinator-owned memory.
static void coord_free(ap_app *app, ap_export_coord *c)
{
    free(c->items);
    free(c->job);
    free(c);
    app->export_coord = NULL;
}

// Open + read back + frame one photo, then submit a CPU encode job.
// Returns the framed byte size on success (already added to
// bytes_inflight by the caller via the return), 0 on a skip/failure
// that should advance the cursor without counting bytes.
static size_t open_and_submit(ap_app *app, ap_export_coord *c,
                              const ap_export_item *it)
{
    ap_photo *photo = NULL;
    bool own_photo = false;
    if (it->use_open_photo) {
        photo = app->photo;
    } else {
        photo = ap_photo_open(app->gpu, it->src_abs);
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

    // Terminal: all photos consumed (or cancellation stopped scheduling)
    // and every encode has drained.
    if ((c->cursor >= c->count || canceling) && c->inflight_encode == 0) {
        ap_job_finish(c->job, canceling ? AP_JOB_CANCELED : AP_JOB_DONE);
        if (canceling) {
            ap_status_notify(AP_STATUS_INFO, "Export canceled.");
        } else {
            ap_status_notify(AP_STATUS_INFO, "Export complete: %d photo%s.",
                             c->count, c->count == 1 ? "" : "s");
        }
        coord_free(app, c);
        return;
    }

    // While canceling, schedule nothing new; just let encodes drain.
    if (canceling || c->cursor >= c->count) return;

    // Backpressure gate: don't open the next photo while it would push
    // us over the byte budget or the concurrent-encode cap. An empty
    // pipeline always opens at least one photo so we never deadlock on
    // a single photo larger than the budget.
    if (c->inflight_encode >= c->max_inflight) return;
    if (c->bytes_inflight > 0 &&
        c->bytes_inflight >= AP_EXPORT_BYTE_BUDGET) return;

    size_t bytes = open_and_submit(app, c, &c->items[c->cursor]);
    if (bytes > 0) {
        c->bytes_inflight += bytes;
        c->inflight_encode++;
    }
    c->cursor++;
    ap_job_progress(c->job, c->cursor, c->count);
}

void ap_export_coord_encode_done(ap_app *app, size_t bytes)
{
    if (!app || !app->export_coord) return;
    ap_export_coord *c = app->export_coord;
    if (c->bytes_inflight >= bytes) c->bytes_inflight -= bytes;
    else                            c->bytes_inflight = 0;
    if (c->inflight_encode > 0) c->inflight_encode--;
}

void ap_export_coord_shutdown(ap_app *app)
{
    if (!app || !app->export_coord) return;
    ap_export_coord *c = app->export_coord;

    ap_job_request_cancel(c->job);

    // Drain in-flight encodes: wait the pool idle, then drain completed
    // items through the normal handler so their RGBA buffers free and
    // the inflight count settles. drain_all_workers routes each export
    // job to its completion arm, which accounts back to the coordinator.
    while (c->inflight_encode > 0) {
        ap_worker_pool_wait_idle(app->workers);
        ap_work_item *it = ap_worker_pool_poll(app->workers);
        if (!it) break;
        // route only export-coord jobs here; others go through discard.
        if (it->run == export_job_run) {
            export_job *j = (export_job *)it;
            if (j->from_coord) ap_export_coord_encode_done(app, j->rgba_bytes);
            free(j->rgba);
            free(j);
        } else {
            discard_completed_item(app, it);
        }
    }

    // c may have been freed by encode_done's caller path; re-fetch.
    c = app->export_coord;
    if (!c) return;
    ap_job_finish(c->job, AP_JOB_CANCELED);
    coord_free(app, c);
}
