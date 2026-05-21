#define _GNU_SOURCE

#include "app.h"

#include "app/layout_profiles.h"
#include "core/log.h"
#include "core/worker.h"
#include "gpu/canvas.h"
#include "gpu/gpu.h"
#include "gpu/grid.h"
#include "gpu/pipeline_graph.h"
#include "library/import.h"
#include "library/library.h"
#include "output/jpeg.h"
#include "panels/panels.h"
#include "photo/photo.h"
#include "photo/thumbnail.h"
#include "ui/file_dialog.h"
#include "ui/imgui.h"

#include "cimgui.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// Decode-on-worker job for one thumbnail. Submitter (main thread)
// fills `path` + `idx`, and — when the library has a fresh
// edit-render blob for this photo — `cache_jpeg` / `cache_jpeg_size`.
// The worker decodes the cache blob if present (preferred), else the
// camera's embedded preview from `path`. It fills `rgba` / `w` / `h`
// / `ok`; the main thread polls completed jobs, uploads to GPU, and
// frees both buffers.
typedef struct {
    ap_work_item   base;
    char           path[4096];
    int            idx;
    unsigned char *cache_jpeg;       // edit-render blob, or NULL
    size_t         cache_jpeg_size;
    uint64_t       gen;              // app->thumb_load_gen at submit;
                                     // stale jobs (e.g. after a library
                                     // thumbnail-mode toggle) are
                                     // discarded on completion.
    uint8_t       *rgba;
    int            w, h;
    int            ok;
} thumb_job;

// Raw-load-on-worker job for an interactive photo open. Submitter
// fills `path` + `gen`; worker fills `raw` + `ok`; main thread polls,
// builds the photo around the raw if generations still match.
typedef struct {
    ap_work_item base;
    char         path[4096];
    ap_raw_image raw;
    uint64_t     gen;
    int          ok;
} photo_open_job;

// JPEG-encode-on-worker job for an interactive export. Main thread
// does the GPU readback (fast), then submits this job with the RGBA
// buffer + output path + quality. Worker writes the file.
typedef struct {
    ap_work_item base;
    uint8_t     *rgba;
    int          width, height;
    int          quality;
    char         out_path[4096];
} export_job;

// Edit-render thumbnail encode job. Main thread does the sync GPU
// readback in ap_app_close_photo, then submits this so the downsample
// + libjpeg encode happen off the GPU thread - ESC returns to the
// grid immediately. The completion handler stores the JPEG blob to
// the library db and invalidates the affected grid cell so the
// thumbnail pump re-decodes the new render.
typedef struct {
    ap_work_item   base;
    uint8_t       *rgba;        // owned by the job, freed in handler
    int            width, height;
    int            idx;         // library index of the closed photo
    unsigned char *jpeg;        // filled by run; freed in handler
    size_t         jpeg_size;
    int            ok;
} thumb_encode_job;

// Forward decls of run-fn pointers (used to dispatch on completion).
static void thumb_job_run(ap_work_item *self);
static void thumb_encode_job_run(ap_work_item *self);
static void photo_open_job_run(ap_work_item *self);
static void export_job_run(ap_work_item *self);

// Forward decl - used by open/close before its definition.
static void refresh_window_title(ap_app *app);

struct ap_app {
    ap_gpu          *gpu;
    ap_canvas       *canvas;
    ap_grid         *grid;
    ap_mode          mode;
    ap_photo        *photo;
    int              photo_library_idx;   // library index of the photo
                                          // currently shown in photo
                                          // mode, or -1. Lets photo-mode
                                          // arrow nav walk the library
                                          // without mutating grid state.
    ap_library      *library;
    ap_worker_pool  *workers;
    int              thumb_inflight;
    bool             photo_loading;
    char             loading_path[4096];
    uint64_t         photo_load_gen;
    uint64_t         thumb_load_gen;       // bumped on library
                                           // thumbnail-mode toggle so
                                           // in-flight thumb_jobs
                                           // submitted in the prior
                                           // mode are discarded on
                                           // completion.
    int              export_inflight;

    // Workspace chrome
    bool             show_panels;          // Tab to toggle
    bool             show_rendered_thumbnails; // library grid: edit
                                               // renders (default) vs
                                               // camera-embedded
                                               // previews.

    // Library-grid group filter and the cell -> library-photo map it
    // produces: grid_map[cell] is the library photo shown in that
    // cell, grid_map_count the number of visible cells. With the ALL
    // filter the map is the identity.
    int              group_filter_kind;        // ap_group_filter
    char             group_filter_name[AP_GROUP_NAME_LEN];
    int             *grid_map;
    int              grid_map_count;
    int              grid_map_cap;

    bool               import_modal;        // File -> Import
    char               import_source[4096];
    ap_import_settings import_settings;
    char               import_status[160];
    bool             rename_library_modal; // Library indicator -> Rename
    char             rename_library_input[128];
    bool             save_layout_modal;    // View -> Layout -> Save Current As
    char             save_layout_input[AP_LAYOUT_NAME_LEN];
    bool             quit_requested;
};

#define THUMB_MAX_INFLIGHT 8

// Set by SIGTERM / SIGINT; polled by ap_app_should_run so the main
// loop exits cleanly (running per-photo save-on-close, library
// teardown, etc.) instead of dying mid-frame.
static volatile sig_atomic_t g_quit_requested = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_quit_requested = 1;
}

static void install_signal_handlers(void)
{
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

ap_app *ap_app_create(int width, int height, const char *title)
{
    ap_app *app = calloc(1, sizeof(*app));
    if (!app) {
        AP_ERROR("ap_app_create: out of memory");
        return NULL;
    }
    app->mode = AP_MODE_LIBRARY;
    app->show_panels = true;
    app->show_rendered_thumbnails = true;
    app->photo_library_idx = -1;

    app->gpu = ap_gpu_create(width, height, title);
    if (!app->gpu) {
        free(app);
        return NULL;
    }

    app->canvas = ap_canvas_create(app->gpu);
    if (!app->canvas) {
        ap_gpu_destroy(app->gpu);
        free(app);
        return NULL;
    }

    app->grid = ap_grid_create(app->gpu);
    if (!app->grid) {
        ap_canvas_destroy(app->canvas);
        ap_gpu_destroy(app->gpu);
        free(app);
        return NULL;
    }

    app->workers = ap_worker_pool_create(0);
    if (!app->workers) {
        ap_grid_destroy(app->grid);
        ap_canvas_destroy(app->canvas);
        ap_gpu_destroy(app->gpu);
        free(app);
        return NULL;
    }

    // Restore persisted preferences.
    {
        char buf[32];
        if (ap_settings_get("fullscreen", buf, sizeof(buf)) == 0
            && atoi(buf) != 0) {
            ap_gpu_toggle_fullscreen(app->gpu);
        }
    }

    // Load the active layout profile, if any. ImGui's IniFilename
    // is NULL (see imgui_bridge.cpp); this is the explicit load.
    ap_layout_init();

    install_signal_handlers();
    return app;
}

static void toggle_and_persist_fullscreen(ap_app *app)
{
    ap_gpu_toggle_fullscreen(app->gpu);
    ap_settings_set("fullscreen", ap_gpu_is_fullscreen(app->gpu) ? "1" : "0");
}

// Free a completed work item without acting on its result. Used at
// teardown and when the result's target (library, photo) is gone.
static void discard_completed_item(ap_app *app, ap_work_item *it)
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

// Wait for all submitted work to land, then drain (without acting).
static void drain_all_workers(ap_app *app)
{
    if (!app->workers) return;
    ap_worker_pool_wait_idle(app->workers);
    for (;;) {
        ap_work_item *it = ap_worker_pool_poll(app->workers);
        if (!it) break;
        discard_completed_item(app, it);
    }
}

// Wait + drain only the thumbnail jobs (and discard any other
// stragglers). Used when the library these jobs were decoded for is
// about to be torn down.
static void discard_completed_thumb_jobs(ap_app *app)
{
    drain_all_workers(app);
}

void ap_app_destroy(ap_app *app)
{
    if (!app) return;

    drain_all_workers(app);
    ap_app_wait_idle(app);
    ap_app_close_photo(app);
    ap_app_close_library(app);
    ap_gpu_set_canvas(app->gpu, NULL);
    ap_gpu_set_grid(app->gpu, NULL);
    if (app->workers) {
        ap_worker_pool_destroy(app->workers);
        app->workers = NULL;
    }
    if (app->grid) {
        ap_grid_destroy(app->grid);
        app->grid = NULL;
    }
    free(app->grid_map);
    app->grid_map = NULL;
    if (app->canvas) {
        ap_canvas_destroy(app->canvas);
        app->canvas = NULL;
    }
    if (app->gpu) {
        ap_gpu_destroy(app->gpu);
        app->gpu = NULL;
    }
    free(app);
}

bool ap_app_should_run(ap_app *app)
{
    if (!app) return false;
    if (g_quit_requested) return false;
    if (app->quit_requested) return false;
    return ap_gpu_should_run(app->gpu);
}

void ap_app_wait_idle(ap_app *app)
{
    if (app && app->gpu) {
        ap_gpu_wait_idle(app->gpu);
    }
}

ap_mode ap_app_mode(const ap_app *app)
{
    return app ? app->mode : AP_MODE_LIBRARY;
}

static void bind_mode_view(ap_app *app)
{
    if (!app || !app->gpu) return;
    if (app->mode == AP_MODE_PHOTO) {
        // Canvas binds whenever we are in photo mode, even before the
        // photo's pipeline is built. With no input view, ap_canvas_record
        // early-returns and draw_loading_overlay covers the gap.
        ap_gpu_set_grid(app->gpu, NULL);
        ap_gpu_set_canvas(app->gpu, app->canvas);
    } else {
        ap_gpu_set_canvas(app->gpu, NULL);
        ap_gpu_set_grid(app->gpu, app->library ? app->grid : NULL);
    }
}

void ap_app_set_mode(ap_app *app, ap_mode mode)
{
    if (!app) return;
    app->mode = mode;
    bind_mode_view(app);
}

static void photo_open_job_run(ap_work_item *self)
{
    photo_open_job *j = (photo_open_job *)self;
    j->ok = (ap_raw_load(j->path, &j->raw) == 0);
}

// Release the currently-open photo's GPU resources and free it,
// without touching navigation state (mode, library index). Both the
// user-facing close and the prev/next photo swap go through this; the
// swap path must keep photo_library_idx so navigation can continue.
static void release_photo(ap_app *app)
{
    if (!app->photo) return;
    ap_app_wait_idle(app);
    ap_canvas_set_input(app->canvas, VK_NULL_HANDLE, VK_NULL_HANDLE, 0, 0);
    ap_gpu_set_graph(app->gpu, NULL);
    ap_photo_close(app->photo);
    app->photo = NULL;
}

static void install_loaded_photo(ap_app *app, photo_open_job *j)
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
    // Fresh photo - reset the canvas view. set_input itself preserves
    // zoom/pan now so edits don't snap the view back.
    ap_canvas_reset_view(app->canvas);
    app->mode = AP_MODE_PHOTO;
    bind_mode_view(app);
}

int ap_app_open_photo(ap_app *app, const char *path)
{
    if (!app || !path) {
        return -1;
    }

    photo_open_job *j = calloc(1, sizeof(*j));
    if (!j) {
        AP_ERROR("ap_app_open_photo: oom");
        return -1;
    }
    j->base.run = photo_open_job_run;
    snprintf(j->path, sizeof(j->path), "%s", path);

    app->photo_load_gen++;
    j->gen = app->photo_load_gen;
    snprintf(app->loading_path, sizeof(app->loading_path), "%s", path);
    app->photo_loading = true;

    // Flip into photo mode synchronously so the workspace appears
    // instantly. The canvas binds with no input until the worker lands
    // and install_loaded_photo rebinds it to the freshly built pipeline.
    // draw_loading_overlay covers the gap. If we are already in photo
    // mode (prev/next nav), the previous photo stays visible until the
    // new one is installed.
    if (app->mode != AP_MODE_PHOTO) {
        app->mode = AP_MODE_PHOTO;
        bind_mode_view(app);
    }

    ap_worker_pool_submit(app->workers, &j->base);
    return 0;
}

void ap_app_close_photo(ap_app *app)
{
    if (!app) return;

    // Invalidate any in-flight async open so its result is discarded
    // when it lands.
    if (app->photo_loading) {
        app->photo_load_gen++;
        app->photo_loading = false;
        app->loading_path[0] = '\0';
    }

    int closed_idx = app->photo_library_idx;

    // Sync GPU readback of the rendered output while the graph is
    // still alive. The downsample + libjpeg encode + db store happen
    // on a worker so the return to library mode is immediate; the
    // affected grid cell refreshes when the worker completes.
    uint8_t *thumb_rgba = NULL;
    int      thumb_w = 0, thumb_h = 0;
    bool have_rgba = (closed_idx >= 0 && app->photo && app->library &&
                      ap_photo_readback_rgba(app->photo,
                                             &thumb_rgba,
                                             &thumb_w, &thumb_h) == 0);

    // Frame the grid thumbnail through the photo's viewport so a
    // cropped / rotated / flipped photo shows its framed result in
    // the library grid, not the full rendered frame. Done before
    // release_photo — the viewport lives on the photo's edit stack.
    if (have_rgba) {
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
        // On OOM keep the un-framed thumb — better than no thumbnail.
    }

    release_photo(app);
    app->photo_library_idx = -1;
    app->mode  = AP_MODE_LIBRARY;
    bind_mode_view(app);

    if (have_rgba) {
        thumb_encode_job *j = calloc(1, sizeof(*j));
        if (j) {
            j->base.run = thumb_encode_job_run;
            j->rgba     = thumb_rgba;
            j->width    = thumb_w;
            j->height   = thumb_h;
            j->idx      = closed_idx;
            thumb_rgba  = NULL;
            ap_worker_pool_submit(app->workers, &j->base);
        } else {
            AP_ERROR("close_photo: thumb_encode job alloc failed");
        }
    }
    free(thumb_rgba); // NULL after submit; non-NULL only on alloc failure
}

ap_photo *ap_app_photo(ap_app *app)
{
    return app ? app->photo : NULL;
}

ap_canvas *ap_app_canvas(ap_app *app)
{
    return app ? app->canvas : NULL;
}

void ap_app_rebuild_photo_graph(ap_app *app)
{
    if (!app || !app->photo) return;

    // Idle the device first - the old graph's images may still be in
    // flight. ap_photo_rebuild_graph then destroys the old graph and
    // builds a new one from the current edit stack.
    ap_app_wait_idle(app);
    if (ap_photo_rebuild_graph(app->photo) != 0) {
        AP_ERROR("app: photo graph rebuild failed");
        return;
    }

    // Both the GPU's current-graph pointer and the canvas binding
    // referenced the *old* graph that rebuild just freed. Re-point
    // both at the new one - missing either is a use-after-free the
    // next time a frame is recorded.
    ap_pipeline_graph *graph = ap_photo_graph(app->photo);
    ap_gpu_set_graph(app->gpu, graph);
    ap_canvas_set_input(app->canvas,
                        ap_pipeline_graph_output_view(graph),
                        ap_pipeline_graph_output_sampler(graph),
                        ap_pipeline_graph_output_width(graph),
                        ap_pipeline_graph_output_height(graph));
}

bool ap_app_photo_loading(const ap_app *app)
{
    return app ? app->photo_loading : false;
}

int ap_app_request_jpeg_export(ap_app *app, ap_photo *photo,
                               const char *out_path, int quality)
{
    if (!app || !photo || !out_path) return -1;
    int w = ap_photo_width(photo);
    int h = ap_photo_height(photo);
    if (w <= 0 || h <= 0) return -1;

    size_t bytes = (size_t)w * (size_t)h * 4u;
    uint8_t *rgba = malloc(bytes);
    if (!rgba) {
        AP_ERROR("export: out of memory (%zu bytes)", bytes);
        return -1;
    }
    if (ap_pipeline_graph_readback(ap_photo_graph(photo), rgba, bytes) != 0) {
        free(rgba);
        return -1;
    }

    // Apply the viewport (crop / rotation / flip / scale) — the
    // pipeline rendered the full frame; the export rasterizes the
    // framed result. ap_viewport_resample_rgba8 mirrors the canvas
    // shader so the file matches what's on screen.
    {
        ap_viewport vp = ap_photo_viewport(photo);
        int framed_w = 0, framed_h = 0;
        uint8_t *framed = ap_viewport_resample_rgba8(&vp, rgba, w, h,
                                                     &framed_w, &framed_h);
        if (!framed) {
            AP_ERROR("export: out of memory framing the export");
            free(rgba);
            return -1;
        }
        free(rgba);
        rgba = framed;
        w = framed_w;
        h = framed_h;
    }

    export_job *j = calloc(1, sizeof(*j));
    if (!j) {
        AP_ERROR("export: job alloc failed");
        free(rgba);
        return -1;
    }
    j->base.run = export_job_run;
    j->rgba    = rgba;
    j->width   = w;
    j->height  = h;
    j->quality = quality;
    snprintf(j->out_path, sizeof(j->out_path), "%s", out_path);

    app->export_inflight++;
    AP_INFO("export: queued %s (%dx%d, q=%d)", j->out_path, w, h, quality);
    ap_worker_pool_submit(app->workers, &j->base);
    return 0;
}

// Rebuild the cell -> library-photo map from the active group filter,
// resize the grid to the visible count, and re-bind each visible
// cell's thumbnail from the library cache (the cells have just been
// reassigned, so their bound textures are stale).
static void rebuild_grid_map(ap_app *app)
{
    app->grid_map_count = 0;
    if (!app->library) {
        if (app->grid) ap_grid_set_photo_count(app->grid, 0);
        return;
    }

    int n = ap_library_photo_count(app->library);
    if (n > app->grid_map_cap) {
        int *m = realloc(app->grid_map, (size_t)n * sizeof(int));
        if (!m) {
            AP_ERROR("app: grid map allocation failed");
            return;
        }
        app->grid_map     = m;
        app->grid_map_cap = n;
    }

    for (int i = 0; i < n; i++) {
        bool show = true;
        if (app->group_filter_kind != AP_GROUP_FILTER_ALL) {
            const ap_photo_groups *g =
                ap_library_photo_groups(app->library, i);
            int gc = g ? g->count : 0;
            if (app->group_filter_kind == AP_GROUP_FILTER_UNGROUPED) {
                show = (gc == 0);
            } else {
                show = false;
                for (int k = 0; k < gc; k++) {
                    if (strcmp(g->names[k], app->group_filter_name) == 0) {
                        show = true;
                        break;
                    }
                }
            }
        }
        if (show) {
            app->grid_map[app->grid_map_count++] = i;
        }
    }

    if (!app->grid) return;
    ap_grid_set_photo_count(app->grid, app->grid_map_count);
    ap_grid_set_selected(app->grid, 0);
    for (int c = 0; c < app->grid_map_count; c++) {
        ap_thumbnail *t = ap_library_thumbnail(app->library,
                                               app->grid_map[c]);
        if (t) {
            ap_grid_set_thumbnail(app->grid, c, ap_thumbnail_view(t),
                                  ap_thumbnail_sampler(t));
        } else {
            ap_grid_set_thumbnail(app->grid, c,
                                  VK_NULL_HANDLE, VK_NULL_HANDLE);
        }
    }
}

// Grid cell currently showing library photo `photo_idx`, or -1 when
// that photo is filtered out of the visible grid.
static int cell_for_photo(const ap_app *app, int photo_idx)
{
    for (int c = 0; c < app->grid_map_count; c++) {
        if (app->grid_map[c] == photo_idx) return c;
    }
    return -1;
}

int ap_app_open_library(ap_app *app, const char *path)
{
    if (!app || !path) return -1;

    ap_app_close_photo(app);
    ap_app_close_library(app);

    app->library = ap_library_open(path);
    if (!app->library) {
        return -1;
    }
    app->group_filter_kind    = AP_GROUP_FILTER_ALL;
    app->group_filter_name[0] = '\0';
    rebuild_grid_map(app);
    app->mode = AP_MODE_LIBRARY;
    bind_mode_view(app);
    refresh_window_title(app);
    return 0;
}

void ap_app_close_library(ap_app *app)
{
    if (!app || !app->library) return;

    // Wait for outstanding decode work to land in the completed queue
    // and toss the buffers - they belong to the library that's going
    // away. Then drop GPU references before the textures vanish.
    ap_worker_pool_wait_idle(app->workers);
    discard_completed_thumb_jobs(app);
    ap_app_wait_idle(app);
    ap_grid_set_photo_count(app->grid, 0);
    app->grid_map_count = 0;
    ap_library_close(app->library);
    app->library = NULL;
    bind_mode_view(app);
    refresh_window_title(app);
}

ap_library *ap_app_library(ap_app *app)
{
    return app ? app->library : NULL;
}

int ap_app_apply_pipeline_to_selection(ap_app *app, int64_t pipeline_id)
{
    if (!app || !app->library || !app->grid) return -1;

    int wrote = 0;
    for (int c = 0; c < app->grid_map_count; c++) {
        if (!ap_grid_is_selected(app->grid, c)) continue;
        int i = app->grid_map[c];
        // Skip the open photo: rewriting its sidecar would leave the
        // in-memory stack stale until the user closes + reopens.
        if (app->photo && i == app->photo_library_idx) continue;
        if (ap_library_apply_pipeline_to_photo(app->library, i,
                                               pipeline_id) == 0) {
            wrote++;
            // Drop the cached thumbnail so the grid re-decodes against
            // the new stack on the next pump cycle. The stored
            // edit-render blob's freshness check already handles the
            // sidecar-mtime side.
            ap_library_invalidate_thumbnail(app->library, i);
        }
    }
    return wrote;
}

int ap_app_apply_metadata_to_selection(ap_app *app,
                                       const ap_photo_metadata *patch,
                                       const bool patch_set[AP_META_FIELD_COUNT])
{
    if (!app || !patch || !patch_set) return -1;
    if (!app->library || !app->grid)  return -1;

    int wrote = 0;
    for (int c = 0; c < app->grid_map_count; c++) {
        if (!ap_grid_is_selected(app->grid, c)) continue;
        int i = app->grid_map[c];
        if (ap_library_apply_metadata_patch(app->library, i,
                                            patch, patch_set) == 0) {
            wrote++;
        }
    }
    return wrote;
}

void ap_app_set_group_filter(ap_app *app, int kind, const char *name)
{
    if (!app) return;
    app->group_filter_kind = kind;
    if (kind == AP_GROUP_FILTER_GROUP && name) {
        snprintf(app->group_filter_name, sizeof(app->group_filter_name),
                 "%s", name);
    } else {
        app->group_filter_name[0] = '\0';
    }
    rebuild_grid_map(app);
}

int ap_app_group_filter_kind(const ap_app *app)
{
    return app ? app->group_filter_kind : AP_GROUP_FILTER_ALL;
}

const char *ap_app_group_filter_name(const ap_app *app)
{
    return app ? app->group_filter_name : "";
}

int ap_app_grid_selection_count(const ap_app *app)
{
    if (!app || !app->grid) return 0;
    return ap_grid_selection_count(app->grid);
}

int ap_app_assign_selection_to_group(ap_app *app, const char *group, bool add)
{
    if (!app || !app->library || !app->grid || !group || !*group) {
        return -1;
    }
    int wrote = 0;
    for (int c = 0; c < app->grid_map_count; c++) {
        if (!ap_grid_is_selected(app->grid, c)) continue;
        if (ap_library_set_photo_group(app->library, app->grid_map[c],
                                       group, add) == 0) {
            wrote++;
        }
    }
    // Membership changed — a group filter's visible set may have moved.
    rebuild_grid_map(app);
    return wrote;
}

static void navigate_library_relative(ap_app *app, int dir)
{
    // Walks the library list while staying in photo mode. The
    // library's grid selection is intentionally untouched - it
    // represents the user's earlier intent in library mode and
    // should survive this navigation so backing out (Esc) puts
    // them where they were.
    if (!app->library) return;
    int n = ap_library_photo_count(app->library);
    if (n <= 0 || app->photo_library_idx < 0) return;
    int new_idx = app->photo_library_idx + dir;
    if (new_idx < 0 || new_idx >= n) return;

    char abs[4096];
    if (ap_library_photo_absolute_path(app->library, new_idx,
                                       abs, sizeof(abs)) != 0) return;
    app->photo_library_idx = new_idx;
    ap_app_open_photo(app, abs);
}

static void drive_canvas_input(ap_app *app)
{
    if (!app->canvas || !app->photo) return;

    ImGuiIO *io = igGetIO_Nil();
    if (!io) return;

    if (igIsKeyPressed_Bool(ImGuiKey_Escape, false)) {
        ap_app_close_photo(app);
        return;
    }

    // Prev/next photo. Gate on WantTextInput, not WantCaptureKeyboard:
    // the always-present docked panels keep WantCaptureKeyboard true,
    // which would block navigation entirely. WantTextInput is only
    // set while an actual text field (e.g. the rename box) is active,
    // which is the one case where arrows should be left to ImGui.
    if (!io->WantTextInput) {
        if (igIsKeyPressed_Bool(ImGuiKey_RightArrow, true)) {
            navigate_library_relative(app, +1);
            return;
        }
        if (igIsKeyPressed_Bool(ImGuiKey_LeftArrow, true)) {
            navigate_library_relative(app, -1);
            return;
        }
    }

    if (io->WantCaptureMouse) return;

    int win_w = (int)io->DisplaySize.x;
    int win_h = (int)io->DisplaySize.y;

    if (io->MouseDown[0] && (io->MouseDelta.x != 0.0f || io->MouseDelta.y != 0.0f)) {
        ap_canvas_pan(app->canvas, io->MouseDelta.x, io->MouseDelta.y);
    }
    // Wheel semantics:
    //   plain wheel  → pan (vertical + horizontal). Trackpad
    //                  two-finger scroll lands here and feels native.
    //   Ctrl + wheel → zoom-at-cursor. Compositors translate trackpad
    //                  pinch into Ctrl+vertical-scroll, so pinch-to-
    //                  zoom works without GLFW gesture plumbing.
    if (io->MouseWheel != 0.0f || io->MouseWheelH != 0.0f) {
        if (io->KeyCtrl && io->MouseWheel != 0.0f) {
            float factor = io->MouseWheel > 0.0f
                ? 1.0f + 0.10f * io->MouseWheel
                : 1.0f / (1.0f - 0.10f * io->MouseWheel);
            ap_canvas_zoom_at(app->canvas, factor,
                              io->MousePos.x, io->MousePos.y,
                              win_w, win_h);
        } else {
            const float pan_step_px = 40.0f;
            // Sign matches "natural scrolling": wheel-up moves the
            // image up, wheel-right moves it right. Flip a sign here
            // if it ever feels inverted on a different input setup.
            ap_canvas_pan(app->canvas,
                          io->MouseWheelH * pan_step_px,
                          -io->MouseWheel  * pan_step_px);
        }
    }

    if (igIsKeyPressed_Bool(ImGuiKey_F, false) ||
        igIsKeyPressed_Bool(ImGuiKey_0, false)) {
        ap_canvas_reset_view(app->canvas);
    } else if (igIsKeyPressed_Bool(ImGuiKey_1, false)) {
        ap_canvas_set_zoom(app->canvas, 1.0f, win_w, win_h);
    }
}

static void open_selected_photo(ap_app *app)
{
    if (!app->library || !app->grid) return;
    int cell = ap_grid_selected(app->grid);
    if (cell < 0 || cell >= app->grid_map_count) return;
    int idx = app->grid_map[cell];

    char abs[4096];
    if (ap_library_photo_absolute_path(app->library, idx, abs, sizeof(abs)) != 0) {
        AP_ERROR("library: photo path overflow at idx %d", idx);
        return;
    }
    app->photo_library_idx = idx;
    if (ap_app_open_photo(app, abs) != 0) {
        AP_ERROR("library: failed to open %s", abs);
    }
}

static void drive_grid_input(ap_app *app)
{
    if (!app->library || !app->grid) return;
    int n = app->grid_map_count;
    if (n <= 0) return;

    ImGuiIO *io = igGetIO_Nil();
    if (!io) return;

    int win_w = (int)io->DisplaySize.x;
    int win_h = (int)io->DisplaySize.y;

    if (!io->WantCaptureMouse) {
        if (io->MouseWheel != 0.0f) {
            if (io->KeyCtrl) {
                int cur  = ap_grid_cell_size(app->grid);
                int step = 16;
                int next = cur + (int)(io->MouseWheel) * step;
                ap_grid_set_cell_size(app->grid, next);
            } else {
                const float wheel_step_px = 60.0f;
                ap_grid_scroll(app->grid, -io->MouseWheel * wheel_step_px,
                               win_w, win_h);
            }
        }
        if (igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left)) {
            int hit = ap_grid_hit_test(app->grid,
                                       io->MousePos.x, io->MousePos.y,
                                       win_w, win_h);
            if (hit >= 0) {
                ap_grid_select_only(app->grid, hit);
                open_selected_photo(app);
                return;
            }
        } else if (igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) {
            int hit = ap_grid_hit_test(app->grid,
                                       io->MousePos.x, io->MousePos.y,
                                       win_w, win_h);
            if (hit >= 0) {
                int anchor = ap_grid_selected(app->grid);
                if (io->KeyShift) {
                    ap_grid_select_range(app->grid, anchor, hit);
                } else if (io->KeyCtrl) {
                    ap_grid_select_toggle(app->grid, hit);
                } else {
                    ap_grid_select_only(app->grid, hit);
                }
            }
        }
    }

    int sel = ap_grid_selected(app->grid);
    int new_sel = sel;
    int cpr = ap_grid_cells_per_row(app->grid, win_w, win_h);
    if      (igIsKeyPressed_Bool(ImGuiKey_RightArrow, true)) new_sel = sel + 1;
    else if (igIsKeyPressed_Bool(ImGuiKey_LeftArrow,  true)) new_sel = sel - 1;
    else if (igIsKeyPressed_Bool(ImGuiKey_DownArrow, true))  new_sel = sel + cpr;
    else if (igIsKeyPressed_Bool(ImGuiKey_UpArrow,   true))  new_sel = sel - cpr;
    if (new_sel != sel) {
        if (io->KeyShift) {
            ap_grid_select_range(app->grid, sel, new_sel);
        } else {
            ap_grid_select_only(app->grid, new_sel);
        }
        ap_grid_ensure_visible(app->grid, ap_grid_selected(app->grid),
                               win_w, win_h);
    }

    if (!io->KeyCtrl && (igIsKeyPressed_Bool(ImGuiKey_Enter, false) ||
                         igIsKeyPressed_Bool(ImGuiKey_Space, false))) {
        open_selected_photo(app);
    }
}

static void draw_selection_overlay(ap_app *app)
{
    if (!app->library || !app->grid) return;
    int n = ap_library_photo_count(app->library);
    if (n <= 0) return;
    int sel_count = ap_grid_selection_count(app->grid);
    if (sel_count <= 1) return;   // focus highlight alone is enough

    ImGuiIO *io = igGetIO_Nil();
    if (!io) return;
    int win_w = (int)io->DisplaySize.x;
    int win_h = (int)io->DisplaySize.y;
    ImDrawList *dl = igGetForegroundDrawList_ViewportPtr(NULL);
    if (!dl) return;

    int focus = ap_grid_selected(app->grid);
    for (int i = 0; i < n; i++) {
        if (i == focus) continue;
        if (!ap_grid_is_selected(app->grid, i)) continue;
        float cx, cy, cw, ch;
        if (ap_grid_cell_rect(app->grid, i, win_w, win_h,
                              &cx, &cy, &cw, &ch) != 0) continue;
        ImVec2_c tl = { cx,      cy      };
        ImVec2_c br = { cx + cw, cy + ch };
        ImDrawList_AddRect(dl, tl, br, 0xFFB8C4D9, 0.0f, 0, 2.0f);
    }
}

static void draw_grid_labels(ap_app *app)
{
    if (!app->library || !app->grid) return;
    int n = app->grid_map_count;
    if (n <= 0) return;

    ImGuiIO *io = igGetIO_Nil();
    if (!io) return;
    int win_w = (int)io->DisplaySize.x;
    int win_h = (int)io->DisplaySize.y;

    // Background draw list — labels belong to the grid layer, behind
    // any docked panels. Foreground would paint over the panel chrome.
    ImDrawList *dl = igGetBackgroundDrawList(NULL);
    if (!dl) return;

    const float band_h = 18.0f;
    for (int c = 0; c < n; c++) {
        int i = app->grid_map[c];
        const char *rel = ap_library_photo_relative_path(app->library, i);
        if (!rel) continue;
        float cx, cy, cw, ch;
        if (ap_grid_cell_rect(app->grid, c, win_w, win_h, &cx, &cy, &cw, &ch) != 0) {
            continue;
        }

        // Letterbox the label band to the actual image rect inside the
        // cell, mirroring the shader's aspect-fit math. Portrait
        // thumbnails leave space at the sides; landscape leaves space
        // top and bottom. Without this, labels sit on top of the image
        // itself instead of riding cleanly under it.
        float fit_x = cx, fit_y = cy, fit_w = cw, fit_h = ch;
        ap_thumbnail *t = ap_library_thumbnail(app->library, i);
        if (t) {
            int tw = ap_thumbnail_width(t);
            int th = ap_thumbnail_height(t);
            if (tw > 0 && th > 0) {
                float s = cw / (float)tw;
                float sy = ch / (float)th;
                if (sy < s) s = sy;
                fit_w = (float)tw * s;
                fit_h = (float)th * s;
                fit_x = cx + (cw - fit_w) * 0.5f;
                fit_y = cy + (ch - fit_h) * 0.5f;
            }
        }

        // Skip only when the *cell* genuinely can't carry a band —
        // not when the fitted image is short. A heavily cropped photo
        // has a wide, short thumbnail; gating on fit_h dropped its
        // title entirely.
        if (ch < band_h) continue;

        // Band rides under the fitted image, clamped to stay inside
        // the cell so a short thumbnail never carries it off-cell.
        float band_top = fit_y + fit_h - band_h;
        if (band_top < cy)                  band_top = cy;
        if (band_top + band_h > cy + ch)    band_top = cy + ch - band_h;

        ImVec2_c band_tl = { fit_x,         band_top          };
        ImVec2_c band_br = { fit_x + fit_w, band_top + band_h };
        ImDrawList_AddRectFilled(dl, band_tl, band_br, 0xB8000000, 0.0f, 0);
        ImVec2_c text_pos = { fit_x + 4.0f, band_top + 2.0f };
        ImDrawList_AddText_Vec2(dl, text_pos, 0xFFEEEEEE, rel, NULL);
    }
}

static void thumb_job_run(ap_work_item *self)
{
    thumb_job *j = (thumb_job *)self;
    if (j->cache_jpeg) {
        // Edit-render blob from the library db — what the photo
        // actually looks like through its stack.
        j->ok = (ap_thumbnail_decode_jpeg(j->cache_jpeg, j->cache_jpeg_size,
                                          &j->rgba, &j->w, &j->h) == 0);
    } else {
        // Fallback: camera's embedded preview.
        j->ok = (ap_thumbnail_decode_cpu(j->path, &j->rgba, &j->w, &j->h) == 0);
    }
}

// Submit decode jobs while the pool has headroom and the library
// still has un-decoded photos. Each submission heap-allocates a
// thumb_job that the worker fills in. The library db is consulted
// (cheap indexed SELECT, main thread) for a fresh edit-render blob;
// when present it's handed to the worker, otherwise the worker
// falls back to the embedded preview.
static void submit_pending_thumbs(ap_app *app)
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
        // In rendered mode prefer the persisted edit-render blob; NULL
        // out-params on miss so the worker uses the embedded-preview
        // path. In camera-preview mode skip the lookup entirely so the
        // worker always uses the embedded preview.
        if (app->show_rendered_thumbnails) {
            ap_library_thumbnail_blob(app->library, idx,
                                      &j->cache_jpeg, &j->cache_jpeg_size);
        }
        ap_worker_pool_submit(app->workers, &j->base);
        app->thumb_inflight++;
    }
}

// Flip the library grid between rendered (edit-render db blobs, with
// embedded-preview fallback) and camera-embedded previews. Bumps the
// thumb load gen so any thumb_jobs still in flight from the previous
// mode are discarded on completion, then drops every cached thumbnail
// so the pump re-decodes the whole grid under the new mode.
static void toggle_rendered_thumbnails(ap_app *app)
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

static void handle_thumb_complete(ap_app *app, thumb_job *j)
{
    if (app->thumb_inflight > 0) app->thumb_inflight--;
    // Discard jobs submitted before the last thumbnail-mode toggle:
    // their pixels reflect the old mode (rendered vs camera preview).
    bool stale = (j->gen != app->thumb_load_gen);
    if (!stale && app->library && app->grid && j->ok && j->rgba
        && j->idx >= 0 && j->idx < ap_library_photo_count(app->library))
    {
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
    }
    free(j->cache_jpeg);
    free(j->rgba);
    free(j);
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
            // ap_app_open_photo flipped us to photo mode. With no photo
            // to install, revert to the library so the user isn't
            // stranded on a blank canvas. If a previous photo is still
            // bound (prev/next nav with the new open failing), keep it
            // visible.
            if (!app->photo && app->mode == AP_MODE_PHOTO) {
                app->mode = AP_MODE_LIBRARY;
                bind_mode_view(app);
            }
        }
    }
    free(j);
}

static void export_job_run(ap_work_item *self)
{
    export_job *j = (export_job *)self;
    if (ap_export_jpeg(j->rgba, j->width, j->height,
                       j->out_path, j->quality) != 0) {
        AP_ERROR("export: failed to write %s", j->out_path);
    }
}

static void handle_export_complete(ap_app *app, export_job *j)
{
    if (app->export_inflight > 0) app->export_inflight--;
    free(j->rgba);
    free(j);
}

static void thumb_encode_job_run(ap_work_item *self)
{
    thumb_encode_job *j = (thumb_encode_job *)self;
    j->ok = (ap_thumbnail_encode_jpeg(j->rgba, j->width, j->height,
                                      &j->jpeg, &j->jpeg_size) == 0);
}

static void handle_thumb_encode_complete(ap_app *app, thumb_encode_job *j)
{
    if (j->ok && j->jpeg && j->jpeg_size > 0 && app->library
        && j->idx >= 0 && j->idx < ap_library_photo_count(app->library))
    {
        ap_library_store_thumbnail(app->library, j->idx, j->jpeg, j->jpeg_size);
        // Drop the grid cell back to the placeholder and invalidate
        // the in-memory cache so the pump re-decodes from the new
        // db blob.
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

// Pop one completed work item per frame and dispatch on its run-fn.
// Pool poll is non-blocking - when nothing's ready, this is a no-op.
static void drain_one_completed_job(ap_app *app)
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

// ---- menubar + global hotkeys ----------------------------------------

static void trigger_quick_export(ap_app *app)
{
    if (!app->photo) return;
    const char *src = ap_photo_path(app->photo);

    // Export filename mimics the source: its basename with the raw
    // extension swapped for .jpg.
    const char *slash = strrchr(src, '/');
    const char *base  = slash ? slash + 1 : src;
    char stem[1024];
    snprintf(stem, sizeof(stem), "%s", base);
    char *dot = strrchr(stem, '.');
    if (dot) *dot = '\0';

    char out[4096];
    if (app->library) {
        // Default destination: <lib_root>/export/, created on demand.
        char dir[4096];
        int dn = snprintf(dir, sizeof(dir), "%s/export",
                          ap_library_root(app->library));
        if (dn <= 0 || (size_t)dn >= sizeof(dir)) {
            AP_ERROR("export: directory path too long");
            return;
        }
        if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
            AP_ERROR("export: mkdir(%s): %s", dir, strerror(errno));
            return;
        }
        int n = snprintf(out, sizeof(out), "%s/%s.jpg", dir, stem);
        if (n <= 0 || (size_t)n >= sizeof(out)) {
            AP_ERROR("export: path too long for %s", stem);
            return;
        }
    } else {
        // No library open — fall back to beside the source file.
        int n = snprintf(out, sizeof(out), "%s.jpg", src);
        if (n <= 0 || (size_t)n >= sizeof(out)) {
            AP_ERROR("export: path too long for %s", src);
            return;
        }
    }

    ap_app_request_jpeg_export(app, app->photo, out, 90);
}

static const char *library_display_label(ap_library *lib)
{
    if (!lib) return "(no library)";
    const char *name = ap_library_name(lib);
    if (name && *name) return name;
    return ap_library_root(lib);
}

static void refresh_window_title(ap_app *app)
{
    if (!app || !app->gpu) return;
    if (!app->library) {
        ap_gpu_set_window_title(app->gpu, "Aperture");
        return;
    }
    char title[5120];
    snprintf(title, sizeof(title), "Aperture: %s",
             library_display_label(app->library));
    ap_gpu_set_window_title(app->gpu, title);
}

static void draw_menubar(ap_app *app)
{
    if (!igBeginMainMenuBar()) return;

    if (igBeginMenu("File", true)) {
        if (igMenuItem_Bool("Open Library", NULL, false, true)) {
            const char *root = app->library ? ap_library_root(app->library)
                                            : NULL;
            char picked[4096];
            if (ap_file_dialog_pick_folder(picked, sizeof(picked), root)) {
                ap_app_open_library(app, picked);
            }
        }

        if (igMenuItem_Bool("Import...", NULL, false, app->library != NULL)) {
            ap_import_settings_load(app->library, &app->import_settings);
            app->import_source[0] = '\0';
            app->import_status[0] = '\0';
            app->import_modal     = true;
        }

        igSeparator();

        if (igMenuItem_Bool("Close Photo", "Esc",
                            false, app->photo != NULL)) {
            ap_app_close_photo(app);
        }
        if (igMenuItem_Bool("Close Library", NULL,
                            false, app->library != NULL)) {
            ap_app_close_library(app);
        }

        igSeparator();

        if (igMenuItem_Bool("Export", "Ctrl+E",
                            false, app->photo != NULL)) {
            trigger_quick_export(app);
        }

        igSeparator();

        if (igMenuItem_Bool("Quit", "Ctrl+Q", false, true)) {
            app->quit_requested = true;
        }
        igEndMenu();
    }

    // Edit menu: visibility toggles for every registered panel that
    // opted into the optional-panel pattern (panels.h: `visible` +
    // `menu_label`). Shown only when at least one matching panel
    // exists for the current mode — otherwise we'd render an empty
    // dropdown.
    bool any_optional = false;
    for (const ap_panel *const *p = ap_panel_registry; *p; p++) {
        const ap_panel *panel = *p;
        if (!panel->menu_label || !panel->visible) continue;
        if (panel->mode != AP_MODE_ANY && panel->mode != app->mode) continue;
        any_optional = true;
        break;
    }
    if (any_optional && igBeginMenu("Edit", true)) {
        for (const ap_panel *const *p = ap_panel_registry; *p; p++) {
            const ap_panel *panel = *p;
            if (!panel->menu_label || !panel->visible) continue;
            if (panel->mode != AP_MODE_ANY && panel->mode != app->mode) continue;
            igMenuItem_BoolPtr(panel->menu_label, NULL, panel->visible, true);
        }
        igEndMenu();
    }

    if (igBeginMenu("View", true)) {
        bool show = app->show_panels;
        const char *panels_sc =
            (app->mode == AP_MODE_PHOTO) ? "Space" : NULL;
        if (igMenuItem_BoolPtr("Show Panels", panels_sc, &show, true)) {
            app->show_panels = show;
        }
        if (igMenuItem_Bool("Fullscreen", "F11",
                            ap_gpu_is_fullscreen(app->gpu), true)) {
            toggle_and_persist_fullscreen(app);
        }
        igSeparator();
        if (igMenuItem_Bool("Reset Cell Zoom", "Ctrl+0", false, true)) {
            ap_grid_reset_cell_size(app->grid);
        }
        if (app->photo) {
            bool view_raw = ap_photo_view_raw(app->photo);
            if (igMenuItem_Bool("View Raw", "`", view_raw, true)) {
                ap_photo_set_view_raw(app->photo, !view_raw);
                ap_app_rebuild_photo_graph(app);
            }
        }
        if (app->mode == AP_MODE_LIBRARY && app->library) {
            bool rendered = app->show_rendered_thumbnails;
            if (igMenuItem_BoolPtr("Show Rendered Thumbnails", "`",
                                   &rendered, true)) {
                toggle_rendered_thumbnails(app);
            }
        }
        igSeparator();

        // Layout submenu: pick / save / reset named profiles.
        // ImGui auto-persist is off; this is the explicit surface.
        if (igBeginMenu("Layout", true)) {
            const char *active = ap_layout_active_name();
            char names[AP_LAYOUT_MAX_ENUM][AP_LAYOUT_NAME_LEN];
            int n = ap_layout_list(names, AP_LAYOUT_MAX_ENUM);
            if (n <= 0) {
                igMenuItem_Bool("(no saved layouts)", NULL, false, false);
            } else {
                for (int i = 0; i < n; i++) {
                    bool checked = (strcmp(active, names[i]) == 0);
                    if (igMenuItem_Bool(names[i], NULL, checked, true)
                        && !checked) {
                        ap_layout_set_active(names[i]);
                    }
                }
            }
            igSeparator();
            if (igMenuItem_Bool("Save Current As...", NULL, false, true)) {
                app->save_layout_modal = true;
                app->save_layout_input[0] = '\0';
            }
            if (igMenuItem_Bool("Reset to Active Profile", NULL,
                                false, active && active[0])) {
                ap_layout_reload_active();
            }
            if (igMenuItem_Bool("Reset to Defaults", NULL, false, true)) {
                ap_layout_reset_to_default();
            }
            igEndMenu();
        }

        igEndMenu();
    }

    // Library indicator + quick switcher. The menu label is the
    // library's user-set name when set, the full path otherwise, or
    // "(no library)" when none is open. Centered in the menubar so
    // it reads as a status indicator independent of the File/View
    // dropdowns on the left.
    const char *lib_label = library_display_label(app->library);
    {
        ImVec2_c label_size = igCalcTextSize(lib_label, NULL, false, -1.0f);
        ImGuiStyle *style = igGetStyle();
        float item_w   = label_size.x + style->FramePadding.x * 2.0f;
        float center_x = (igGetWindowWidth() - item_w) * 0.5f;
        // Don't backtrack - leave a gap after View if the label is
        // somehow huge.
        igSetCursorPosX(center_x);
    }
    if (igBeginMenu(lib_label, true)) {
        if (app->library) {
            igText("%s", ap_library_root(app->library));
            igText("%d photos", ap_library_photo_count(app->library));
            if (igMenuItem_Bool("Rename", NULL, false, true)) {
                snprintf(app->rename_library_input,
                         sizeof(app->rename_library_input), "%s",
                         ap_library_name(app->library));
                app->rename_library_modal = true;
            }
            igSeparator();
        }
        ap_registry_entry rows[16];
        int n = ap_registry_list(rows, 16);
        if (n <= 0) {
            igMenuItem_Bool("(no recent libraries)", NULL, false, false);
        } else {
            for (int i = 0; i < n; i++) {
                bool current = app->library
                    && strcmp(rows[i].path, ap_library_root(app->library)) == 0;
                // Menu shows the name (or path when unnamed). Hover
                // gets the full path as a tooltip - keeps the menu
                // narrow and the buffer manageable.
                const char *label_name = rows[i].name[0] ? rows[i].name
                                                         : rows[i].path;
                if (igMenuItem_Bool(label_name, NULL, current, true) && !current) {
                    ap_app_open_library(app, rows[i].path);
                }
                if (igIsItemHovered(0)) {
                    igSetTooltip("%s", rows[i].path);
                }
            }
        }
        igEndMenu();
    }

    igEndMainMenuBar();
}

static void draw_import_modal(ap_app *app)
{
    if (app->import_modal) {
        igOpenPopup_Str("Import Photos", 0);
        app->import_modal = false;
    }
    if (!igBeginPopupModal("Import Photos", NULL,
                           ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    if (!app->library) {
        igText("No library open.");
        if (igButton("Close", (ImVec2_c){ 120.0f, 0.0f })) {
            igCloseCurrentPopup();
        }
        igEndPopup();
        return;
    }

    ap_import_settings *s = &app->import_settings;

    igText("Source folder:");
    igTextDisabled("%s", app->import_source[0] ? app->import_source
                                               : "(none chosen)");
    if (igButton("Choose Source...", (ImVec2_c){ 0.0f, 0.0f })) {
        ap_file_dialog_pick_folder(app->import_source,
                                   sizeof(app->import_source), NULL);
    }

    igSeparator();

    igInputText("Destination subdir", s->subdir, sizeof(s->subdir),
                0, NULL, NULL);

    static const char *const naming_items[] = {
        "Keep original names", "Rename by pattern",
    };
    igCombo_Str_arr("Naming", &s->naming, naming_items, 2, -1);
    if (s->naming == AP_IMPORT_NAME_PATTERN) {
        igInputText("Pattern", s->pattern, sizeof(s->pattern), 0, NULL, NULL);
        igTextDisabled("tokens: {ORIG} {YYYY} {MM} {DD} {HH} {MIN} {SEC} {SEQ}");
    }

    static const char *const collide_items[] = {
        "Skip", "Overwrite", "Auto-suffix",
    };
    igCombo_Str_arr("On name collision", &s->collision, collide_items, 3, -1);

    igSeparator();

    bool can_import = app->import_source[0] != '\0';
    if (!can_import) igBeginDisabled(true);
    if (igButton("Import", (ImVec2_c){ 120.0f, 0.0f })) {
        ap_import_settings_save(app->library, s);
        char root[4096];
        snprintf(root, sizeof(root), "%s", ap_library_root(app->library));
        int n = 0;
        if (ap_import_run(app->library, app->import_source, s, &n) == 0) {
            // Re-scan by reopening the library so the new files appear.
            ap_app_open_library(app, root);
            snprintf(app->import_status, sizeof(app->import_status),
                     "Imported %d photo%s.", n, n == 1 ? "" : "s");
        } else {
            snprintf(app->import_status, sizeof(app->import_status),
                     "Import failed - see the log.");
        }
    }
    if (!can_import) igEndDisabled();
    igSameLine(0.0f, -1.0f);
    if (igButton("Close", (ImVec2_c){ 120.0f, 0.0f })) {
        igCloseCurrentPopup();
    }

    if (app->import_status[0]) {
        igTextWrapped("%s", app->import_status);
    }

    igEndPopup();
}

static void draw_rename_library_modal(ap_app *app)
{
    if (app->rename_library_modal) {
        igOpenPopup_Str("Rename Library", 0);
        app->rename_library_modal = false;
    }
    if (!igBeginPopupModal("Rename Library", NULL, 0)) return;

    if (!app->library) {
        igCloseCurrentPopup();
        igEndPopup();
        return;
    }

    igText("Display name for this library:");
    igText("(leave blank to clear and show the path)");
    igInputText("##name", app->rename_library_input,
                sizeof(app->rename_library_input), 0, NULL, NULL);

    bool submit = igButton("Save", (ImVec2_c){ 120.0f, 0.0f });
    igSameLine(0.0f, -1.0f);
    bool cancel = igButton("Cancel", (ImVec2_c){ 120.0f, 0.0f });

    if (submit) {
        ap_library_set_name(app->library, app->rename_library_input);
        refresh_window_title(app);
        igCloseCurrentPopup();
    } else if (cancel) {
        igCloseCurrentPopup();
    }
    igEndPopup();
}

static void draw_save_layout_modal(ap_app *app)
{
    if (app->save_layout_modal) {
        igOpenPopup_Str("Save Layout As", 0);
        app->save_layout_modal = false;
    }
    if (!igBeginPopupModal("Save Layout As", NULL, 0)) return;

    igText("Name for the layout:");
    igSetNextItemWidth(260.0f);
    igInputText("##layout_name", app->save_layout_input,
                sizeof(app->save_layout_input), 0, NULL, NULL);

    bool name_ok = app->save_layout_input[0] != '\0';
    if (!name_ok) igBeginDisabled(true);
    bool submit = igButton("Save", (ImVec2_c){ 120.0f, 0.0f });
    if (!name_ok) igEndDisabled();
    igSameLine(0.0f, -1.0f);
    bool cancel = igButton("Cancel", (ImVec2_c){ 120.0f, 0.0f });

    if (submit && name_ok) {
        if (ap_layout_save_current_as(app->save_layout_input) == 0) {
            igCloseCurrentPopup();
        }
    } else if (cancel) {
        igCloseCurrentPopup();
    }
    igEndPopup();
}

static void drive_global_hotkeys(ap_app *app)
{
    ImGuiIO *io = igGetIO_Nil();
    if (!io) return;

    // Bare Space in photo mode toggles panel visibility. The canvas
    // owns the keyboard there and Space is otherwise idle. Library
    // mode has no shortcut - bare Space there is "open selected
    // photo" and the View menu still has a clickable toggle. The
    // WantTextInput guard keeps text fields (the rename box, etc.)
    // typeable.
    if (app->mode == AP_MODE_PHOTO && !io->WantTextInput
        && igIsKeyPressed_Bool(ImGuiKey_Space, false)) {
        app->show_panels = !app->show_panels;
    }
    if (igIsKeyPressed_Bool(ImGuiKey_F11, false)) {
        toggle_and_persist_fullscreen(app);
    }
    if (io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_Q, false)) {
        app->quit_requested = true;
    }
    if (io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_E, false) && app->photo) {
        trigger_quick_export(app);
    }
    if (igIsKeyPressed_Bool(ImGuiKey_GraveAccent, false)
        && !io->WantTextInput) {
        // Same key, two contexts: photo mode toggles the per-photo
        // View Raw bypass; library mode toggles between rendered and
        // camera-preview grid thumbnails. Photo mode wins when a
        // photo is open.
        if (app->photo) {
            ap_photo_set_view_raw(app->photo, !ap_photo_view_raw(app->photo));
            ap_app_rebuild_photo_graph(app);
        } else if (app->mode == AP_MODE_LIBRARY && app->library) {
            toggle_rendered_thumbnails(app);
        }
    }
    if (io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_0, false)) {
        if (app->mode == AP_MODE_PHOTO) {
            ap_canvas_reset_view(app->canvas);
        } else {
            ap_grid_reset_cell_size(app->grid);
        }
    }

    if (io->KeyCtrl && (igIsKeyPressed_Bool(ImGuiKey_Equal, true) ||
                        igIsKeyPressed_Bool(ImGuiKey_KeypadAdd, true))) {
        if (app->mode == AP_MODE_PHOTO) {
            int win_w = (int)io->DisplaySize.x;
            int win_h = (int)io->DisplaySize.y;
            ap_canvas_zoom_at(app->canvas, 1.15f,
                              win_w * 0.5f, win_h * 0.5f, win_w, win_h);
        } else {
            ap_grid_set_cell_size(app->grid,
                                  ap_grid_cell_size(app->grid) + 16);
        }
    }
    if (io->KeyCtrl && (igIsKeyPressed_Bool(ImGuiKey_Minus, true) ||
                        igIsKeyPressed_Bool(ImGuiKey_KeypadSubtract, true))) {
        if (app->mode == AP_MODE_PHOTO) {
            int win_w = (int)io->DisplaySize.x;
            int win_h = (int)io->DisplaySize.y;
            ap_canvas_zoom_at(app->canvas, 1.0f / 1.15f,
                              win_w * 0.5f, win_h * 0.5f, win_w, win_h);
        } else {
            ap_grid_set_cell_size(app->grid,
                                  ap_grid_cell_size(app->grid) - 16);
        }
    }
}

// ----------------------------------------------------------------------

static void draw_loading_overlay(ap_app *app)
{
    if (!app->photo_loading) return;
    ImGuiIO *io = igGetIO_Nil();
    if (!io) return;
    ImDrawList *dl = igGetForegroundDrawList_ViewportPtr(NULL);
    if (!dl) return;

    ImVec2_c center = { io->DisplaySize.x * 0.5f, io->DisplaySize.y * 0.5f };
    char msg[5120];
    snprintf(msg, sizeof(msg), "loading %s", app->loading_path);
    ImVec2_c pos = { center.x - 200.0f, center.y - 8.0f };
    ImDrawList_AddText_Vec2(dl, pos, 0xFFEEEEEE, msg, NULL);
}

int ap_app_run_frame(ap_app *app)
{
    if (!app) return -1;

    ap_imgui_new_frame();

    draw_menubar(app);
    draw_import_modal(app);
    draw_rename_library_modal(app);
    draw_save_layout_modal(app);
    drive_global_hotkeys(app);

    // Full-viewport invisible host window owns the dockspace that
    // every panel docks into. PassthruCentralNode keeps the middle
    // area transparent so the canvas / grid render path stays
    // visible underneath. Default layout (Image left, Edits + Tools
    // right) is built once on first launch; ImGui's .ini handles
    // every subsequent run.
    if (app->show_panels) {
        ImGuiViewport *vp = igGetMainViewport();
        igSetNextWindowPos(vp->WorkPos, ImGuiCond_Always,
                           (ImVec2_c){ 0.0f, 0.0f });
        igSetNextWindowSize(vp->WorkSize, ImGuiCond_Always);
        igSetNextWindowViewport(vp->ID);

        igPushStyleVar_Float(ImGuiStyleVar_WindowRounding,   0.0f);
        igPushStyleVar_Float(ImGuiStyleVar_WindowBorderSize, 0.0f);
        igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding,
                            (ImVec2_c){ 0.0f, 0.0f });

        ImGuiWindowFlags host_flags = ImGuiWindowFlags_NoTitleBar
                                    | ImGuiWindowFlags_NoCollapse
                                    | ImGuiWindowFlags_NoResize
                                    | ImGuiWindowFlags_NoMove
                                    | ImGuiWindowFlags_NoBringToFrontOnFocus
                                    | ImGuiWindowFlags_NoNavFocus
                                    | ImGuiWindowFlags_NoBackground
                                    | ImGuiWindowFlags_NoDocking;

        igBegin("##aperture_dockhost", NULL, host_flags);
        igPopStyleVar(3);

        ImGuiID dockspace_id = igGetID_Str("aperture_dockspace");
        static bool dock_layout_built = false;
        // Reset-to-defaults trips the rebuild flag in layout_profiles;
        // pull it down here so the builder runs again on the next
        // frame and produces the default layout from scratch.
        if (ap_layout_consume_rebuild_request()) {
            if (igDockBuilderGetNode(dockspace_id)) {
                igDockBuilderRemoveNode(dockspace_id);
            }
            dock_layout_built = false;
        }
        if (!dock_layout_built &&
            igDockBuilderGetNode(dockspace_id) == NULL) {
            dock_layout_built = true;
            igDockBuilderAddNode(dockspace_id,
                                 ImGuiDockNodeFlags_DockSpace);
            igDockBuilderSetNodeSize(dockspace_id, vp->WorkSize);

            // Default layout. The principle: left column is "what
            // this is" (per-photo info / library navigation), right
            // column is "what to do with it" (active controls). Both
            // generalize across modes — photo-mode panels populate
            // their slots when photo is open, library-mode panels
            // populate the same column shapes when in library mode.
            // ImGui's .ini owns everything past first launch, so the
            // user can rearrange, float, or close panels freely;
            // empty dock nodes collapse and the central node grows.
            //
            // Left column:
            //   top    — Image (per-photo settings)
            //   bottom — Metadata (per-photo, photo mode)
            // Right column:
            //   top    — Histogram
            //   middle — Tools
            //   bottom — Edits + Metadata##library (tabbed; only one
            //            draws at a time because they're mode-gated)
            // Center stays empty so the canvas / grid render through.
            ImGuiID center = 0;
            ImGuiID left   = igDockBuilderSplitNode(dockspace_id,
                                                    ImGuiDir_Left, 0.18f,
                                                    NULL, &center);
            // Pull the right column off the post-left remainder. 0.27
            // of the remainder ≈ 0.22 of the full window, matching the
            // hand-tuned width in the user's saved .ini.
            ImGuiID right  = igDockBuilderSplitNode(center,
                                                    ImGuiDir_Right, 0.27f,
                                                    NULL, &center);

            ImGuiID left_bot = 0;
            ImGuiID left_top = igDockBuilderSplitNode(left,
                                                      ImGuiDir_Up, 0.50f,
                                                      NULL, &left_bot);

            // Right column splits twice: first off Edits at the bottom,
            // then within the upper half split Histogram (top) /
            // Tools (middle).
            ImGuiID right_bot   = 0;
            ImGuiID right_upper = igDockBuilderSplitNode(right,
                                                          ImGuiDir_Up, 0.62f,
                                                          NULL, &right_bot);
            ImGuiID right_mid = 0;
            ImGuiID right_top = igDockBuilderSplitNode(right_upper,
                                                       ImGuiDir_Up, 0.45f,
                                                       NULL, &right_mid);

            igDockBuilderDockWindow("Image",             left_top);
            igDockBuilderDockWindow("Metadata",          left_bot);
            igDockBuilderDockWindow("Histogram",         right_top);
            igDockBuilderDockWindow("Tools",             right_mid);
            igDockBuilderDockWindow("Edits",             right_bot);
            // Library-mode bulk panels share the bottom-right slot
            // with Edits. They're mode-gated, so only one tab renders
            // at a time and the slot reads cleanly in either mode.
            igDockBuilderDockWindow("Metadata##library",  right_bot);
            igDockBuilderDockWindow("Pipelines##library", right_bot);
            igDockBuilderFinish(dockspace_id);
        }
        igDockSpace(dockspace_id,
                    (ImVec2_c){ 0.0f, 0.0f },
                    ImGuiDockNodeFlags_PassthruCentralNode, NULL);
        igEnd();
    }

    // Track the dockspace central node so the grid renders + lays out
    // within whatever the user's panel arrangement leaves behind. With
    // no panels docked the central node fills the whole viewport;
    // with the default left/right split it's the slot in between.
    // Cleared to "full window" when show_panels is off so the grid
    // reverts to full-bleed rendering.
    if (app->grid) {
        if (app->show_panels) {
            ImGuiID dockspace_id = igGetID_Str("aperture_dockspace");
            ImGuiDockNode *central = igDockBuilderGetCentralNode(dockspace_id);
            if (central && central->Size.x > 0.0f && central->Size.y > 0.0f) {
                ap_grid_set_render_rect(app->grid,
                                        (int)central->Pos.x,
                                        (int)central->Pos.y,
                                        (int)central->Size.x,
                                        (int)central->Size.y);
            } else {
                ap_grid_set_render_rect(app->grid, 0, 0, 0, 0);
            }
        } else {
            ap_grid_set_render_rect(app->grid, 0, 0, 0, 0);
        }
    }

    if (app->show_panels) {
        for (const ap_panel *const *p = ap_panel_registry; *p; p++) {
            const ap_panel *panel = *p;
            if (panel->mode != AP_MODE_ANY && panel->mode != app->mode) continue;
            if (panel->visible && !*panel->visible) continue;
            if (panel->draw) {
                panel->draw(app);
            }
        }
    }

    if (app->mode == AP_MODE_PHOTO && !app->photo_loading) {
        drive_canvas_input(app);
    } else if (app->mode == AP_MODE_LIBRARY && !app->photo_loading) {
        drive_grid_input(app);
        draw_grid_labels(app);
        draw_selection_overlay(app);
        submit_pending_thumbs(app);
    }
    drain_one_completed_job(app);
    draw_loading_overlay(app);

    // Push the active viewport to the canvas every frame — the
    // pipeline renders full-frame, the canvas applies the Transform
    // module's crop / rotation / flip / scale at presentation. Cheap;
    // the Transform config window may have changed it this frame.
    if (app->canvas) {
        if (app->photo) {
            ap_viewport vp = ap_photo_viewport(app->photo);
            ap_canvas_set_viewport(app->canvas, &vp);
        } else {
            ap_canvas_set_viewport(app->canvas, NULL);
        }
    }

    const ap_edit_stack *stack = NULL;
    if (app->photo) {
        stack = ap_photo_stack(app->photo);
    }
    return ap_gpu_render_frame(app->gpu, stack);
}
