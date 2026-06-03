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

// The whole batch's state, owned by ap_app as app->export_coord.
//
// Each fresh photo flows through a 3-stage pipeline so the main thread
// never blocks on the two expensive steps:
//   decode (worker)  ->  render+readback (async GPU)  ->  encode (worker)
// Exactly ONE decode and ONE render are in flight at a time — serial
// decode (no concurrent libraw) and a single fenced GPU submission (no
// back-to-back), the two conditions that corrupted the earlier prefetch.
// The live open-photo item (always a lone item) stays fully synchronous.
struct ap_export_coord {
    ap_export_settings settings;
    ap_export_item    *items;          // owned; freed on teardown
    int                count;
    int                cursor;         // next item to start (decode / sync)

    // Decode stage (one at a time).
    bool               decode_inflight;
    ap_raw_image       ready_raw;       // a decoded raw awaiting the GPU stage
    bool               has_ready;
    int                ready_item;

    // GPU render+readback stage (one at a time, fenced).
    ap_photo         *gpu_photo;        // owned; closed when the readback lands
    ap_gpu_readback  *gpu_rb;           // in-flight render+copy, NULL when idle
    uint8_t          *gpu_rgba;         // readback target (gpu_w*gpu_h*4)
    int               gpu_w, gpu_h;
    int               gpu_item;

    int                processed;       // items retired (progress)
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

// Tear down any in-flight export render+readback (cancel / teardown).
// ap_gpu_readback_destroy waits the fence first, so the staging buffer is
// not freed while the GPU still references it.
static void free_gpu_inflight(ap_export_coord *c)
{
    if (c->gpu_rb) {
        ap_gpu_readback_destroy(c->gpu_rb);
        c->gpu_rb = NULL;
    }
    free(c->gpu_rgba);
    c->gpu_rgba = NULL;
    if (c->gpu_photo) {
        ap_photo_close(c->gpu_photo);
        c->gpu_photo = NULL;
    }
}

static void coord_free(ap_app *app, ap_export_coord *c)
{
    free_gpu_inflight(c);
    if (c->has_ready) {
        ap_raw_image_free(&c->ready_raw);
        c->has_ready = false;
    }
    free(c->items);
    free(c->job);
    free(c);
    app->export_coord = NULL;
}

// Build a CPU encode job for a framed RGBA buffer and submit it. Takes
// ownership of `rgba`. Returns the byte size (already framed) or 0 on
// alloc failure (rgba freed).
static size_t submit_encode(ap_app *app, ap_export_coord *c,
                            uint8_t *rgba, int w, int h, const char *out_path)
{
    export_job *j = calloc(1, sizeof(*j));
    if (!j) {
        AP_ERROR("export: encode job alloc failed");
        free(rgba);
        return 0;
    }
    size_t bytes = (size_t)w * (size_t)h * 4u;
    j->base.run      = export_job_run;
    j->rgba          = rgba;
    j->width         = w;
    j->height        = h;
    j->format        = c->settings.format;
    j->jpeg_quality  = c->settings.jpeg_quality;
    j->png_depth     = c->settings.png_depth;
    j->tiff_depth    = c->settings.tiff_depth;
    j->tiff_compress = c->settings.tiff_compress;
    snprintf(j->out_path, sizeof(j->out_path), "%s", out_path);
    j->status_id  = 0;          // the coordinator's job owns the bar
    j->from_coord = true;
    j->rgba_bytes = bytes;
    ap_worker_pool_submit(app->workers, &j->base);
    return bytes;
}

// Frame a raw readback buffer through the photo's viewport (crop /
// straighten). Returns the (possibly resampled) buffer + dims; frees the
// input if it resamples. Caller owns the result.
static uint8_t *frame_rgba(ap_photo *photo, uint8_t *rgba, int w, int h,
                           int *out_w, int *out_h)
{
    ap_viewport vp = ap_photo_viewport(photo);
    int fw = 0, fh = 0;
    uint8_t *framed = ap_viewport_resample_rgba8(&vp, rgba, w, h, &fw, &fh);
    if (framed) {
        free(rgba);
        *out_w = fw;
        *out_h = fh;
        return framed;
    }
    *out_w = w;
    *out_h = h;
    return rgba;
}

// Synchronous path for the live open-photo item (always a lone item). It
// shares the bound current_graph with the interactive frame loop, so an
// async readback would race the per-frame render; render + read it back
// synchronously instead. Returns the framed byte size, or 0 on skip.
static size_t process_open_photo(ap_app *app, ap_export_coord *c,
                                 const ap_export_item *it)
{
    // Use the live open photo (its in-memory edits) only if app->photo is
    // still the one this item was resolved for — a navigation could have
    // swapped it. Otherwise open the intended source from disk.
    ap_photo *photo = app->photo;
    bool own_photo = false;
    if (!photo || strcmp(ap_photo_path(photo), it->src_abs) != 0) {
        photo = ap_photo_open(app->gpu, it->src_abs);
        own_photo = true;
        if (!photo) {
            AP_WARN("export: cannot open %s — skipping", it->src_abs);
            return 0;
        }
    }

    int w = ap_photo_width(photo), h = ap_photo_height(photo);
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
    size_t result = 0;
    if (ap_photo_render(photo) == 0 &&
        ap_pipeline_graph_readback(ap_photo_graph(photo), rgba, bytes) == 0) {
        rgba = frame_rgba(photo, rgba, w, h, &w, &h);
        result = submit_encode(app, c, rgba, w, h, it->out_path);
    } else {
        AP_WARN("export: render/readback failed for %s", it->src_abs);
        free(rgba);
    }
    if (own_photo) ap_photo_close(photo);
    return result;
}

// Submit a background raw decode for items[item]. Returns true on submit.
static bool submit_decode(ap_app *app, ap_export_coord *c, int item)
{
    photo_open_job *j = calloc(1, sizeof(*j));
    if (!j) {
        AP_ERROR("export: decode job alloc failed");
        return false;
    }
    j->base.run    = photo_open_job_run;
    j->from_coord  = true;
    j->coord_item  = item;
    snprintf(j->path, sizeof(j->path), "%s", c->items[item].src_abs);
    ap_worker_pool_submit(app->workers, &j->base);
    return true;
}

// Build a photo from a decoded raw (consumes *raw) and begin an async
// render+readback into a fresh gpu_rgba. Sets the gpu_* slot. Returns
// true if a render is now in flight, false on a skip (item retired).
static bool start_render(ap_app *app, ap_export_coord *c, int item,
                         ap_raw_image *raw)
{
    // ap_photo_open_with_raw consumes *raw on every path (success / fail).
    ap_photo *photo = ap_photo_open_with_raw(app->gpu, c->items[item].src_abs,
                                             raw);
    if (!photo) {
        AP_WARN("export: build from raw failed for %s — skipping",
                c->items[item].src_abs);
        return false;
    }
    int w = ap_photo_width(photo), h = ap_photo_height(photo);
    if (w <= 0 || h <= 0) {
        ap_photo_close(photo);
        return false;
    }
    uint8_t *rgba = malloc((size_t)w * (size_t)h * 4u);
    if (!rgba) {
        AP_ERROR("export: out of memory");
        ap_photo_close(photo);
        return false;
    }
    ap_gpu_readback *rb = ap_pipeline_graph_readback_begin(
        ap_photo_graph(photo), ap_photo_stack(photo));
    if (!rb) {
        AP_WARN("export: render begin failed for %s — skipping",
                c->items[item].src_abs);
        free(rgba);
        ap_photo_close(photo);
        return false;
    }

    c->gpu_photo = photo;
    c->gpu_rb    = rb;
    c->gpu_rgba  = rgba;
    c->gpu_w     = w;
    c->gpu_h     = h;
    c->gpu_item  = item;
    return true;
}

void ap_export_coord_pump(ap_app *app)
{
    if (!app) return;
    ap_export_coord *c = app->export_coord;
    if (!c) return;

    bool canceling = ap_job_cancel_requested(c->job);

    // 1. Progress the in-flight async render (non-blocking poll).
    if (c->gpu_rb) {
        int r = ap_gpu_readback_poll(c->gpu_rb, c->gpu_rgba,
                                     (size_t)c->gpu_w * (size_t)c->gpu_h * 4u);
        if (r != 0) {                 // 1 = done, -1 = error (handle freed)
            c->gpu_rb = NULL;
            if (r == 1 && !canceling) {
                int w = c->gpu_w, h = c->gpu_h;
                uint8_t *rgba = frame_rgba(c->gpu_photo, c->gpu_rgba, w, h,
                                           &w, &h);
                c->gpu_rgba = NULL;   // ownership moved into submit_encode
                size_t bytes = submit_encode(app, c, rgba, w, h,
                                             c->items[c->gpu_item].out_path);
                if (bytes > 0) {
                    c->bytes_inflight += bytes;
                    c->inflight_encode++;
                }
            } else {
                free(c->gpu_rgba);
                c->gpu_rgba = NULL;
            }
            ap_photo_close(c->gpu_photo);
            c->gpu_photo = NULL;
            c->processed++;
            ap_job_progress(c->job, c->processed, c->count);
        }
    }

    // On cancel, drop a decoded raw waiting for the GPU stage so it can't
    // leak while we wait for the in-flight decode/render/encodes to drain.
    if (canceling && c->has_ready) {
        ap_raw_image_free(&c->ready_raw);
        c->has_ready = false;
    }

    // 2. Terminal: every stage idle and all encodes drained.
    if ((c->cursor >= c->count || canceling) && !c->decode_inflight &&
        !c->has_ready && c->gpu_rb == NULL && c->inflight_encode == 0) {
        ap_job_finish(c->job, canceling ? AP_JOB_CANCELED : AP_JOB_DONE);
        if (canceling) {
            ap_status_notify(AP_STATUS_INFO, "Export canceled.");
        } else if (c->written == c->count) {
            ap_status_notify(AP_STATUS_INFO, "Export complete: %d photo%s.",
                             c->written, c->written == 1 ? "" : "s");
        } else {
            ap_status_notify(AP_STATUS_ERROR,
                             "Exported %d of %d photo%s — see the log.",
                             c->written, c->count, c->count == 1 ? "" : "s");
        }
        coord_free(app, c);
        return;
    }
    if (canceling) return;  // schedule nothing new; let work drain

    // 3. Start a render from a decoded raw, gated by the encode
    //    backpressure (bytes_inflight == 0 always starts one so a single
    //    oversized photo never deadlocks — peak is BUDGET + one).
    if (c->gpu_rb == NULL && c->has_ready &&
        c->inflight_encode < c->max_inflight &&
        !(c->bytes_inflight > 0 && c->bytes_inflight >= AP_EXPORT_BYTE_BUDGET)) {
        int item = c->ready_item;
        c->has_ready = false;
        if (!start_render(app, c, item, &c->ready_raw)) {
            c->processed++;          // skipped — retire so progress completes
            ap_job_progress(c->job, c->processed, c->count);
        }
    }

    // 4. Advance the pipeline: submit the next decode (fresh items) or
    //    process the lone live-open-photo item synchronously. One decode
    //    in flight at a time, and only while no decoded raw is waiting —
    //    so at most one raw is ever held, and the decode of item N+1
    //    overlaps the render of item N.
    if (!c->decode_inflight && !c->has_ready && c->cursor < c->count) {
        const ap_export_item *it = &c->items[c->cursor];
        if (it->use_open_photo) {
            size_t bytes = process_open_photo(app, c, it);
            if (bytes > 0) {
                c->bytes_inflight += bytes;
                c->inflight_encode++;
            }
            c->cursor++;
            c->processed++;
            ap_job_progress(c->job, c->processed, c->count);
        } else if (submit_decode(app, c, c->cursor)) {
            c->decode_inflight = true;
            c->cursor++;
        } else {
            // Decode job alloc failed (OOM). Retire the item rather than
            // spin on it forever — like every other per-item failure arm,
            // so cursor reaches count and the batch can still terminate.
            c->cursor++;
            c->processed++;
            ap_job_progress(c->job, c->processed, c->count);
        }
    }
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
    if (!c) {
        ap_raw_image_free(&job->raw);   // coordinator gone — discard
        return;
    }
    c->decode_inflight = false;

    if (!job->ok || ap_job_cancel_requested(c->job)) {
        ap_raw_image_free(&job->raw);
        if (!job->ok && !ap_job_cancel_requested(c->job)) {
            // Genuine decode failure (not a cancel): retire the item so
            // the progress bar still reaches 100%.
            c->processed++;
            ap_job_progress(c->job, c->processed, c->count);
        }
        return;
    }

    c->ready_raw  = job->raw;            // move ownership
    c->ready_item = job->coord_item;
    c->has_ready  = true;
    memset(&job->raw, 0, sizeof(job->raw));  // prevent a double-free
}

void ap_export_coord_abort(ap_app *app)
{
    if (!app || !app->export_coord) return;
    ap_export_coord *c = app->export_coord;

    ap_job_request_cancel(c->job);

    // Wait for every in-flight worker item (decode + encode) to finish,
    // then drain the completed queue: the coordinator's encodes free
    // their RGBA + settle inflight_encode, its decodes free their raws,
    // any other completed work goes through its normal discard arm.
    // coord_free then tears down the in-flight render (waiting the GPU
    // fence) + any queued decoded raw.
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
