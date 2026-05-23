#define _GNU_SOURCE

#include "jobs.h"

#include "output/export.h"
#include "output/jpeg.h"
#include "output/png.h"
#include "output/tiff.h"

#include <stdlib.h>
#include <string.h>

void thumb_job_run(ap_work_item *self)
{
    thumb_job *j = (thumb_job *)self;
    if (j->cache_jpeg) {
        j->ok = (ap_thumbnail_decode_jpeg(j->cache_jpeg, j->cache_jpeg_size,
                                          &j->rgba, &j->w, &j->h) == 0);
    } else {
        j->ok = (ap_thumbnail_decode_cpu(j->path, &j->rgba, &j->w, &j->h) == 0);
    }
}

void photo_open_job_run(ap_work_item *self)
{
    photo_open_job *j = (photo_open_job *)self;
    j->ok = (ap_raw_load(j->path, &j->raw) == 0);
}

void export_job_run(ap_work_item *self)
{
    export_job *j = (export_job *)self;
    int rc;
    switch (j->format) {
    case AP_EXPORT_FORMAT_TIFF:
        rc = ap_export_tiff(j->rgba, NULL, j->width, j->height,
                            (ap_tiff_depth)j->tiff_depth,
                            (ap_tiff_compress)j->tiff_compress,
                            NULL, 0, j->out_path);
        break;
    case AP_EXPORT_FORMAT_PNG:
        rc = ap_export_png(j->rgba, j->width, j->height,
                           (ap_png_depth)j->png_depth,
                           NULL, 0, j->out_path);
        break;
    case AP_EXPORT_FORMAT_JPEG:
    default:
        rc = ap_export_jpeg(j->rgba, j->width, j->height,
                            j->out_path, j->jpeg_quality);
        break;
    }
    j->ok = (rc == 0);
    if (!j->ok) {
        AP_ERROR("export: failed to write %s", j->out_path);
    }
}

void thumb_encode_job_run(ap_work_item *self)
{
    thumb_encode_job *j = (thumb_encode_job *)self;
    j->ok = (ap_thumbnail_encode_jpeg(j->rgba, j->width, j->height,
                                      &j->jpeg, &j->jpeg_size) == 0);
}

static void handle_thumb_complete(ap_app *app, thumb_job *j)
{
    if (app->thumb_inflight > 0) app->thumb_inflight--;
    bool stale = (j->gen != app->thumb_load_gen);
    if (!stale && app->library
        && j->idx >= 0 && j->idx < ap_library_photo_count(app->library))
    {
        if (j->ok && j->rgba && app->grid) {
            ap_thumbnail *t = ap_thumbnail_upload(app->gpu, j->rgba, j->w, j->h);
            if (t) {
                ap_library_set_thumbnail(app->library, j->idx, t);
                int cell = cell_for_photo(app, j->idx);
                if (cell >= 0) {
                    ap_grid_set_thumbnail(app->grid, cell,
                                          ap_thumbnail_view(t),
                                          ap_thumbnail_sampler(t));
                }
            }
        } else if (!j->ok) {
            ap_library_mark_thumbnail_failed(app->library, j->idx);
        }
    }
    free(j->cache_jpeg);
    free(j->rgba);
    free(j);
}

void install_loaded_photo(ap_app *app, photo_open_job *j)
{
    release_photo(app);
    app->photo = ap_photo_open_with_raw(app->gpu, j->path, &j->raw);
    if (!app->photo) {
        AP_ERROR("photo: build from raw failed for %s", j->path);
        ap_app_close_photo(app);
        return;
    }
    ap_pipeline_graph *graph = ap_photo_graph(app->photo);
    ap_gpu_set_graph(app->gpu, graph);
    ap_canvas_set_input(app->canvas,
                        ap_pipeline_graph_output_view(graph),
                        ap_pipeline_graph_output_sampler(graph),
                        ap_pipeline_graph_output_width(graph),
                        ap_pipeline_graph_output_height(graph));
    ap_canvas_reset_view(app->canvas);
    app->mode = AP_MODE_PHOTO;
    bind_mode_view(app);
}

static void handle_photo_open_complete(ap_app *app, photo_open_job *j)
{
    bool stale = (j->gen != app->photo_load_gen);
    if (stale) {
        ap_raw_image_free(&j->raw);
    } else {
        app->photo_loading = false;
        app->loading_path[0] = '\0';
        if (j->ok) {
            install_loaded_photo(app, j);
        } else {
            AP_ERROR("photo: failed to open %s", j->path);
            ap_raw_image_free(&j->raw);
            if (!app->photo && app->mode == AP_MODE_PHOTO) {
                app->mode = AP_MODE_LIBRARY;
                bind_mode_view(app);
            }
        }
    }
    free(j);
}

static void handle_export_complete(ap_app *app, export_job *j)
{
    if (app->export_inflight > 0) app->export_inflight--;
    if (j->ok) {
        const char *slash = strrchr(j->out_path, '/');
        const char *name  = slash ? slash + 1 : j->out_path;
        ap_toast_push(AP_TOAST_INFO, "Saved %s", name);
    } else {
        ap_toast_push(AP_TOAST_ERROR, "Export failed — see the log.");
    }
    free(j->rgba);
    free(j);
}

static void handle_thumb_encode_complete(ap_app *app, thumb_encode_job *j)
{
    if (j->ok && j->jpeg && j->jpeg_size > 0 && app->library
        && j->idx >= 0 && j->idx < ap_library_photo_count(app->library))
    {
        ap_library_store_thumbnail(app->library, j->idx, j->jpeg, j->jpeg_size);
        if (app->grid) {
            ap_grid_set_thumbnail(app->grid, j->idx,
                                  VK_NULL_HANDLE, VK_NULL_HANDLE);
        }
        ap_library_invalidate_thumbnail(app->library, j->idx);
    }
    free(j->rgba);
    free(j->jpeg);
    free(j);
}

void discard_completed_item(ap_app *app, ap_work_item *it)
{
    if (it->run == thumb_job_run) {
        thumb_job *j = (thumb_job *)it;
        if (app->thumb_inflight > 0) app->thumb_inflight--;
        free(j->cache_jpeg);
        free(j->rgba);
        free(j);
    } else if (it->run == photo_open_job_run) {
        photo_open_job *j = (photo_open_job *)it;
        ap_raw_image_free(&j->raw);
        free(j);
    } else if (it->run == export_job_run) {
        export_job *j = (export_job *)it;
        if (app->export_inflight > 0) app->export_inflight--;
        free(j->rgba);
        free(j);
    } else if (it->run == thumb_encode_job_run) {
        thumb_encode_job *j = (thumb_encode_job *)it;
        free(j->rgba);
        free(j->jpeg);
        free(j);
    } else {
        AP_WARN("worker: unknown completed run-fn at discard, leaking item");
    }
}

void drain_all_workers(ap_app *app)
{
    if (!app->workers) return;
    ap_worker_pool_wait_idle(app->workers);
    for (;;) {
        ap_work_item *it = ap_worker_pool_poll(app->workers);
        if (!it) break;
        discard_completed_item(app, it);
    }
}

void drain_one_completed_job(ap_app *app)
{
    if (!app->workers) return;
    ap_work_item *it = ap_worker_pool_poll(app->workers);
    if (!it) return;
    if (it->run == thumb_job_run) {
        handle_thumb_complete(app, (thumb_job *)it);
    } else if (it->run == photo_open_job_run) {
        handle_photo_open_complete(app, (photo_open_job *)it);
    } else if (it->run == export_job_run) {
        handle_export_complete(app, (export_job *)it);
    } else if (it->run == thumb_encode_job_run) {
        handle_thumb_encode_complete(app, (thumb_encode_job *)it);
    } else {
        AP_WARN("worker: unknown completed run-fn, leaking item");
    }
}

void submit_pending_thumbs(ap_app *app)
{
    if (!app->library || !app->workers) return;
    while (app->thumb_inflight < THUMB_MAX_INFLIGHT) {
        int idx = ap_library_pending_thumbnail_idx(app->library);
        if (idx < 0) return;

        thumb_job *j = calloc(1, sizeof(*j));
        if (!j) return;
        j->base.run = thumb_job_run;
        j->idx = idx;
        j->gen = app->thumb_load_gen;
        if (ap_library_photo_absolute_path(app->library, idx,
                                           j->path, sizeof(j->path)) != 0) {
            free(j);
            continue;
        }
        if (app->show_rendered_thumbnails) {
            ap_library_thumbnail_blob(app->library, idx,
                                      &j->cache_jpeg, &j->cache_jpeg_size);
        }
        ap_worker_pool_submit(app->workers, &j->base);
        app->thumb_inflight++;
    }
}

void submit_thumb_refresh(ap_app *app, int idx)
{
    if (idx < 0 || !app->photo || !app->library) return;

    uint8_t *thumb_rgba = NULL;
    int      thumb_w = 0, thumb_h = 0;
    if (ap_photo_readback_rgba(app->photo,
                               &thumb_rgba, &thumb_w, &thumb_h) != 0) return;

    ap_viewport vp = ap_photo_viewport(app->photo);
    int fw = 0, fh = 0;
    uint8_t *framed = ap_viewport_resample_rgba8(&vp, thumb_rgba,
                                                 thumb_w, thumb_h,
                                                 &fw, &fh);
    if (framed) {
        free(thumb_rgba);
        thumb_rgba = framed;
        thumb_w    = fw;
        thumb_h    = fh;
    }

    thumb_encode_job *j = calloc(1, sizeof(*j));
    if (j) {
        j->base.run = thumb_encode_job_run;
        j->rgba     = thumb_rgba;
        j->width    = thumb_w;
        j->height   = thumb_h;
        j->idx      = idx;
        ap_worker_pool_submit(app->workers, &j->base);
    } else {
        AP_ERROR("submit_thumb_refresh: thumb_encode job alloc failed");
        free(thumb_rgba);
    }
}

void toggle_rendered_thumbnails(ap_app *app)
{
    if (!app) return;
    app->show_rendered_thumbnails = !app->show_rendered_thumbnails;
    app->thumb_load_gen++;
    if (!app->library) return;
    int n = ap_library_photo_count(app->library);
    for (int i = 0; i < n; i++) {
        ap_library_invalidate_thumbnail(app->library, i);
    }
    for (int c = 0; c < app->grid_map_count && app->grid; c++) {
        ap_grid_set_thumbnail(app->grid, c, VK_NULL_HANDLE, VK_NULL_HANDLE);
    }
}
