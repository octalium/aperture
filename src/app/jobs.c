#define _GNU_SOURCE

#include "jobs.h"

#include "export_coord.h"
#include "io/raw.h"
#include "library/import.h"
#include "sidecar/sidecar.h"
#include "output/export.h"
#include "output/jpeg.h"
#include "output/png.h"
#include "output/tiff.h"
#include "update/check.h"
#include "update/updater.h"

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

// Returning false here breaks the importer's per-file loop; the
// importer marks the run as cancelled and reports the partial counts.
static bool import_progress_cb(int done, int total, void *userdata)
{
    import_job *j = (import_job *)userdata;
    ap_job_progress(j->job, done, total);
    return !ap_job_cancel_requested(j->job);
}

void import_job_run(ap_work_item *self)
{
    import_job *j = (import_job *)self;
    j->ok = (ap_import_run_into(j->lib_root, j->db_path, j->src_dir,
                                &j->settings, &j->report,
                                import_progress_cb, j) == 0);
}

// Attach the lens override to one photo if its EXIF lens matches. Pure
// CPU + path-based sidecar I/O; no library cache or db mutation.
static bool selection_edit_lens_one(selection_edit_job *j, int idx)
{
    char abs[4096];
    if (ap_library_photo_absolute_path(j->library, idx, abs, sizeof(abs)) != 0)
        return false;

    char peer_lens[AP_META_VALUE_LEN];
    if (ap_raw_lens_model(abs, peer_lens, sizeof(peer_lens)) != 0) return false;
    if (strcmp(peer_lens, j->match_exif_lens) != 0) return false;

    ap_edit_stack stack;
    ap_sidecar_ancillary ancillary;
    if (ap_sidecar_load_full(abs, &stack, &ancillary) != 0) return false;

    int hit = -1;
    for (int e = 0; e < stack.count; e++) {
        if (strcmp(stack.entries[e].module_name, "lens_correction") == 0) {
            hit = e;
            break;
        }
    }
    if (hit < 0) return false;

    snprintf(stack.entries[hit].str_params[j->lens_slot], AP_EDIT_STR_LEN,
             "%s", j->override_lens);
    return ap_library_apply_stack_to_photo(j->library, idx, &stack,
                                           &ancillary) == 0;
}

// Run one per-item sidecar op for photo index `idx`. Returns true on a
// successful write.
static bool selection_edit_one(selection_edit_job *j, int idx)
{
    switch (j->op) {
    case AP_SEL_EDIT_PIPELINE:
        return ap_library_apply_pipeline_to_photo(j->library, idx,
                                                  j->pipeline_id) == 0;
    case AP_SEL_EDIT_STACK:
        return ap_library_apply_stack_to_photo(j->library, idx,
                                               &j->stack, NULL) == 0;
    case AP_SEL_EDIT_METADATA:
        return ap_library_apply_metadata_patch(j->library, idx,
                                               &j->patch, j->patch_set) == 0;
    case AP_SEL_EDIT_LENS:
        return selection_edit_lens_one(j, idx);
    }
    return false;
}

void selection_edit_job_run(ap_work_item *self)
{
    selection_edit_job *j = (selection_edit_job *)self;
    int wrote = 0;
    for (int k = 0; k < j->count; k++) {
        if (ap_job_cancel_requested(j->job)) break;
        if (selection_edit_one(j, j->indices[k])) wrote++;
        ap_job_progress(j->job, k + 1, j->count);
    }
    atomic_store(&j->wrote, wrote);
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
                                          ap_thumbnail_sampler(t),
                                          ap_thumbnail_width(t),
                                          ap_thumbnail_height(t));
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
        ap_status_progress_finish(j->status_id, 0);
    } else {
        app->photo_loading = false;
        app->loading_path[0] = '\0';
        if (j->ok) {
            ap_status_progress_finish(j->status_id, 1);
            install_loaded_photo(app, j);
        } else {
            ap_status_progress_finish(j->status_id, 0);
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
    if (j->from_coord) {
        // The export coordinator owns the progress bar + per-batch
        // notify; here we only account the RGBA bytes back so the
        // budget gate releases and the pump can schedule the next photo.
        if (!j->ok) AP_ERROR("export: encode failed for %s", j->out_path);
        ap_export_coord_encode_done(app, j->rgba_bytes);
        free(j->rgba);
        free(j);
        return;
    }
    if (app->export_inflight > 0) app->export_inflight--;
    ap_status_progress_finish(j->status_id, j->ok);
    if (j->ok) {
        const char *slash = strrchr(j->out_path, '/');
        const char *name  = slash ? slash + 1 : j->out_path;
        ap_status_notify(AP_STATUS_INFO, "Saved %s", name);
    } else {
        ap_status_notify(AP_STATUS_ERROR, "Export failed — see the log.");
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
        // Reset the cell that actually shows this photo before invalidating
        // its cached thumbnail. The grid is indexed by display cell, which
        // differs from the library index under a group/search filter, so
        // using j->idx here would leave the real cell's descriptor pointing
        // at the image invalidate_thumbnail destroys below -> the next grid
        // render samples a freed VkImageView -> VK_ERROR_DEVICE_LOST.
        if (app->grid) {
            int cell = cell_for_photo(app, j->idx);
            if (cell >= 0) {
                ap_grid_set_thumbnail(app->grid, cell,
                                      VK_NULL_HANDLE, VK_NULL_HANDLE, 0, 0);
            }
        }
        ap_library_invalidate_thumbnail(app->library, j->idx);
    }
    free(j->rgba);
    free(j->jpeg);
    free(j);
}

static void handle_update_check_complete(ap_app *app, ap_update_check_job *j)
{
    app->update.check_inflight = false;
    if (!j->ok) {
        if (j->error[0]) {
            AP_WARN("update: version check failed: %s", j->error);
        }
        ap_updater_set_pending(NULL, false);
        app->update.available = false;
        free(j);
        return;
    }

    ap_updater_set_pending(&j->manifest, j->newer);
    app->update.available = j->newer;
    if (j->newer) {
        app->update.manifest = j->manifest;
        AP_INFO("update: %s available (running %s)",
                j->manifest.latest, j->current_version);
        if (!app->update.modal_dismissed) app->update.modal = true;
    } else {
        AP_INFO("update: running latest (%s)", j->current_version);
    }
    free(j);
}

static void handle_selection_edit_complete(ap_app *app, selection_edit_job *j)
{
    app->selection_edit_inflight = false;
    bool canceled = ap_job_cancel_requested(j->job);
    int wrote = atomic_load(&j->wrote);

    // Thumbnail invalidation touches the library cache the main thread
    // owns, so it happens here, not on the worker. Re-validate each
    // index against the current photo count + thumb generation: a
    // concurrent reorder/delete (PR 2) bumps the gen and the indices
    // would be stale — discard rather than corrupt the grid.
    if (app->library && j->thumb_gen == app->thumb_load_gen) {
        int n = ap_library_photo_count(app->library);
        for (int k = 0; k < j->count; k++) {
            int idx = j->indices[k];
            if (idx >= 0 && idx < n) {
                ap_library_invalidate_thumbnail(app->library, idx);
            }
        }
    }

    ap_job_finish(j->job, canceled ? AP_JOB_CANCELED : AP_JOB_DONE);
    if (canceled) {
        ap_status_notify(AP_STATUS_INFO,
                         "Edit canceled: %d photo%s written.",
                         wrote, wrote == 1 ? "" : "s");
    } else if (wrote > 0) {
        ap_status_notify(AP_STATUS_INFO, "Applied to %d photo%s.",
                         wrote, wrote == 1 ? "" : "s");
    }

    free(j->indices);
    free(j->job);
    free(j);
}

static void handle_import_complete(ap_app *app, import_job *j)
{
    app->import_inflight  = false;
    app->import_job_id    = 0;
    app->import_report    = j->report;
    bool ok = j->ok && !j->report.cancelled;
    ap_job_finish(j->job, j->report.cancelled ? AP_JOB_CANCELED
                          : (ok ? AP_JOB_DONE : AP_JOB_FAILED));

    if (j->ok) {
        // Incremental rescan instead of a full library reopen: keeps
        // the db connection open and only walks the tree + reloads
        // the photo cache. Drops the perceived "main thread blocks
        // at import end" pause.
        if (app->library) {
            ap_library_rescan(app->library, ap_app_sort(app));
        }

        int n = j->report.imported;
        if (j->report.cancelled) {
            snprintf(app->import_status, sizeof(app->import_status),
                     "Cancelled. Imported %d photo%s before stop.",
                     n, n == 1 ? "" : "s");
            ap_status_notify(AP_STATUS_INFO,
                             "Import cancelled: %d photo%s imported.",
                             n, n == 1 ? "" : "s");
        } else {
            snprintf(app->import_status, sizeof(app->import_status),
                     "Imported %d photo%s.", n, n == 1 ? "" : "s");
            ap_status_notify(AP_STATUS_INFO,
                             "Import complete: %d photo%s.",
                             n, n == 1 ? "" : "s");
        }
    } else {
        snprintf(app->import_status, sizeof(app->import_status),
                 "Import failed -- see the log.");
        ap_status_notify(AP_STATUS_ERROR, "Import failed -- see the log.");
    }
    free(j->job);
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
        ap_status_progress_finish(j->status_id, 0);
        free(j);
    } else if (it->run == export_job_run) {
        export_job *j = (export_job *)it;
        if (j->from_coord) {
            ap_export_coord_encode_done(app, j->rgba_bytes);
        } else {
            if (app->export_inflight > 0) app->export_inflight--;
            ap_status_progress_finish(j->status_id, 0);
        }
        free(j->rgba);
        free(j);
    } else if (it->run == thumb_encode_job_run) {
        thumb_encode_job *j = (thumb_encode_job *)it;
        free(j->rgba);
        free(j->jpeg);
        free(j);
    } else if (it->run == import_job_run) {
        import_job *j = (import_job *)it;
        app->import_inflight = false;
        app->import_job_id   = 0;
        ap_job_finish(j->job, AP_JOB_CANCELED);
        free(j->job);
        free(j);
    } else if (it->run == selection_edit_job_run) {
        selection_edit_job *j = (selection_edit_job *)it;
        app->selection_edit_inflight = false;
        ap_job_finish(j->job, AP_JOB_CANCELED);
        free(j->indices);
        free(j->job);
        free(j);
    } else if (it->run == ap_update_check_run) {
        ap_update_check_job *j = (ap_update_check_job *)it;
        app->update.check_inflight = false;
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
    } else if (it->run == import_job_run) {
        handle_import_complete(app, (import_job *)it);
    } else if (it->run == selection_edit_job_run) {
        handle_selection_edit_complete(app, (selection_edit_job *)it);
    } else if (it->run == ap_update_check_run) {
        handle_update_check_complete(app, (ap_update_check_job *)it);
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
        ap_grid_set_thumbnail(app->grid, c, VK_NULL_HANDLE, VK_NULL_HANDLE, 0, 0);
    }
}

void submit_import_job(ap_app *app, const char *lib_root, const char *src_dir,
                       const ap_import_settings *settings)
{
    if (!app || !app->workers || !lib_root || !src_dir || !settings) return;
    if (app->import_inflight) return;

    import_job *j = calloc(1, sizeof(*j));
    if (!j) {
        AP_ERROR("import: job alloc failed");
        return;
    }
    j->base.run = import_job_run;
    snprintf(j->lib_root, sizeof(j->lib_root), "%s", lib_root);
    snprintf(j->db_path,  sizeof(j->db_path),
             "%s/library.db", lib_root);
    snprintf(j->src_dir,  sizeof(j->src_dir),  "%s", src_dir);
    j->settings  = *settings;
    j->job = ap_job_begin(AP_JOB_KIND_IMPORT, "Importing photos...", 0, NULL);
    if (!j->job) {
        AP_ERROR("import: job control block alloc failed");
        free(j);
        return;
    }

    app->import_inflight  = true;
    app->import_status[0] = '\0';
    app->import_job_id    = j->job->id;
    memset(&app->import_report, 0, sizeof(app->import_report));
    ap_worker_pool_submit(app->workers, &j->base);
}

selection_edit_job *build_selection_edit_job(ap_app *app, ap_sel_edit_op op,
                                             bool skip_open_photo)
{
    if (!app || !app->library || !app->grid || !app->workers) return NULL;
    // Single-flight: never run two selection-edit batches at once, or
    // two workers could write the same photo's sidecar concurrently.
    if (app->selection_edit_inflight) {
        ap_status_notify(AP_STATUS_INFO,
                         "A selection edit is already running.");
        return NULL;
    }

    int sel = ap_grid_selection_count(app->grid);
    if (sel <= 0) return NULL;

    selection_edit_job *j = calloc(1, sizeof(*j));
    if (!j) {
        AP_ERROR("selection edit: job alloc failed");
        return NULL;
    }
    j->indices = malloc((size_t)sel * sizeof(int));
    if (!j->indices) {
        AP_ERROR("selection edit: index snapshot alloc failed");
        free(j);
        return NULL;
    }

    int count = 0;
    for (int c = 0; c < app->grid_map_count && count < sel; c++) {
        if (!ap_grid_is_selected(app->grid, c)) continue;
        int i = app->grid_map[c];
        // Ops that rewrite the edit stack skip the open photo: doing so
        // would leave its in-memory stack stale until close + reopen.
        if (skip_open_photo && app->photo && i == app->photo_library_idx)
            continue;
        j->indices[count++] = i;
    }
    if (count == 0) {
        free(j->indices);
        free(j);
        return NULL;
    }

    j->base.run   = selection_edit_job_run;
    j->library    = app->library;
    j->op         = op;
    j->count      = count;
    j->thumb_gen  = app->thumb_load_gen;
    return j;
}

int commit_selection_edit_job(ap_app *app, selection_edit_job *j,
                              const char *label)
{
    if (!app || !j) return -1;
    j->job = ap_job_begin(AP_JOB_KIND_SELECTION_EDIT, label, j->count, NULL);
    if (!j->job) {
        AP_ERROR("selection edit: job control block alloc failed");
        free(j->indices);
        free(j);
        return -1;
    }
    int count = j->count;
    app->selection_edit_inflight = true;
    ap_worker_pool_submit(app->workers, &j->base);
    return count;
}
