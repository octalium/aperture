#define _GNU_SOURCE

#include "app.h"

#include "app/layout_profiles.h"
#include "core/log.h"
#include "core/worker.h"
#include "gpu/canvas.h"
#include "gpu/gpu.h"
#include "gpu/grid.h"
#include "gpu/pipeline_graph.h"
#include "edit/viewport.h"
#include "library/import.h"
#include "library/library.h"
#include "modules/wb.h"
#include "output/export.h"
#include "output/jpeg.h"
#include "output/png.h"
#include "output/tiff.h"
#include "panels/panels.h"
#include "photo/photo.h"
#include "photo/thumbnail.h"
#include "ui/file_dialog.h"
#include "ui/imgui.h"
#include "ui/toast.h"

#include "cimgui.h"

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

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

// Encode-on-worker job for an interactive export. Main thread does the
// GPU readback (fast), then submits this job with the framed RGBA
// buffer + output path + format settings. Worker writes the file in
// the configured format and sets ok. `format` selects which encoder
// runs; the per-format fields are read only when they apply.
typedef struct {
    ap_work_item base;
    uint8_t     *rgba;
    int          width, height;
    int          format;          // ap_export_format
    int          jpeg_quality;    // JPEG
    int          png_depth;       // PNG  (ap_png_depth)
    int          tiff_depth;      // TIFF (ap_tiff_depth)
    int          tiff_compress;   // TIFF (ap_tiff_compress)
    char         out_path[4096];
    int          ok;
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

// Forward decls - used before their definitions.
static void refresh_window_title(ap_app *app);
static void delete_edit_photo(ap_app *app);

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

    ap_library_sort  sort;                     // current photo list order
    char             search_buf[256];          // substring filter; empty = show all
    ap_culling_filter culling_filter;          // rating / flag / color filter

    // Library-grid rubber-band marquee: active while the user drags on
    // empty grid space. Coords are window-absolute screen pixels.
    bool             marquee_active;
    float            marquee_x0, marquee_y0;  // drag start

    // Library-grid thumbnail drag: active while dragging over selected
    // cells. The ImGui drag-drop payload ("AP_THUMB_DRAG") carries a
    // single int (always 1, payload is a presence signal only).
    bool             thumb_drag_active;

    // Click-vs-drag disambiguation: when the user presses the mouse
    // button over an already-selected cell with no modifier we defer
    // the select-only until mouse-up. If a drag fires first we keep
    // the full selection and start the thumb drag instead. -1 when
    // no deferred select is pending.
    int              deferred_select_cell;

    ap_edit_stack      edit_clipboard;       // copy/paste edits between photos
    bool               edit_clipboard_valid; // true once copy has been called

    bool               compare_original;    // hold-to-bypass: skip all user
                                           // edit stages, show the unedited
                                           // demosaic'd image.

    bool               import_modal;        // File -> Import
    char               import_source[4096];
    ap_import_settings import_settings;
    char               import_status[160];

    // Export mode: the contextual panels read and write this struct;
    // it is loaded from the library on entering the mode and saved
    // back after a successful export.
    ap_export_settings export_settings;
    bool             rename_library_modal; // Library indicator -> Rename
    char             rename_library_input[128];
    bool             save_layout_modal;    // View -> Layout -> Save Current As
    char             save_layout_input[AP_LAYOUT_NAME_LEN];
    bool             delete_modal;         // Delete key -> confirm prompt (grid)
    bool             delete_edit_modal;    // Delete key -> confirm prompt (edit view)
    bool             quit_requested;

    // Interactive canvas tool — armed by a module's config window,
    // driven by the photo-mode canvas-input handler. canvas_tool_entry
    // is the edit-stack entry index the tool writes to. crop_drag_*
    // track an in-progress crop-handle or straighten drag; see
    // drive_crop_tool.
    ap_canvas_tool   canvas_tool;
    int              canvas_tool_entry;
    int              crop_drag_handle;     // crop handle enum, or -1 idle
    bool             crop_aspect_locked;
    float            crop_aspect_ratio;    // width / height when locked
    float            crop_drag_u0, crop_drag_v0; // straighten line start
};

// Crop-overlay interaction handle: the eight rectangle handles, a
// whole-rect move, and the straighten drag. CROP_HANDLE_NONE marks an
// idle overlay (no drag in progress).
enum {
    CROP_HANDLE_NONE       = -1,
    CROP_HANDLE_TL         = 0,
    CROP_HANDLE_TR         = 1,
    CROP_HANDLE_BL         = 2,
    CROP_HANDLE_BR         = 3,
    CROP_HANDLE_L          = 4,
    CROP_HANDLE_R          = 5,
    CROP_HANDLE_T          = 6,
    CROP_HANDLE_B          = 7,
    CROP_HANDLE_MOVE       = 8,
    CROP_HANDLE_STRAIGHTEN = 9,
};

#define THUMB_MAX_INFLIGHT 8

// Multiplicative zoom factor per discrete Ctrl+wheel tick, shared by
// drive_canvas_view and drive_grid_input so both modes feel identical.
#define ZOOM_FACTOR 0.10f

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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

// Optional panels remember their open / closed state across sessions
// in the app-wide settings, keyed by panel name. Window positions are
// handled separately by ImGui's own ini.
static void load_panel_visibility(void)
{
    for (const ap_panel *const *p = ap_panel_registry; *p; p++) {
        const ap_panel *panel = *p;
        if (!panel->visible || !panel->name) continue;
        char key[128];
        char val[8];
        snprintf(key, sizeof(key), "panel.%s.visible", panel->name);
        if (ap_settings_get(key, val, sizeof(val)) == 0) {
            *panel->visible = (atoi(val) != 0);
        }
    }
}

static void save_panel_visibility(void)
{
    for (const ap_panel *const *p = ap_panel_registry; *p; p++) {
        const ap_panel *panel = *p;
        if (!panel->visible || !panel->name) continue;
        char key[128];
        snprintf(key, sizeof(key), "panel.%s.visible", panel->name);
        ap_settings_set(key, *panel->visible ? "1" : "0");
    }
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
    app->canvas_tool = AP_CANVAS_TOOL_NONE;
    app->canvas_tool_entry = -1;
    app->crop_drag_handle = CROP_HANDLE_NONE;
    app->deferred_select_cell = -1;
    app->crop_aspect_ratio = 1.0f;

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

    // Restore the last-active layout profile and which optional panels
    // were open last session. ImGui auto-persists window positions
    // itself via its ini (see imgui_bridge.cpp).
    ap_layout_init();
    load_panel_visibility();

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

void ap_app_destroy(ap_app *app)
{
    if (!app) return;

    save_panel_visibility();
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
    if (app->mode == AP_MODE_PHOTO || app->mode == AP_MODE_EXPORT) {
        // Canvas binds in photo and export mode, even before the
        // photo's pipeline is built. With no input view, ap_canvas_record
        // early-returns and draw_loading_overlay covers the gap. Export
        // mode reuses the canvas as a live preview of the open photo.
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
    app->compare_original = false;
    // An armed canvas tool refers to an edit-stack entry of the photo
    // being torn down — drop it so the next photo starts clean.
    app->canvas_tool       = AP_CANVAS_TOOL_NONE;
    app->canvas_tool_entry = -1;
    app->crop_drag_handle  = CROP_HANDLE_NONE;
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

// Sync GPU readback of the current photo's rendered output, frame it
// through the photo's viewport, and queue a worker job that downsamples,
// JPEG-encodes, stores the result to the library db, and invalidates the
// grid cell at `idx`. Must be called while `app->photo` is still live.
// No-op when there is no photo, no library, or the readback fails.
static void submit_thumb_refresh(ap_app *app, int idx)
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
    submit_thumb_refresh(app, closed_idx);

    release_photo(app);
    app->photo_library_idx = -1;
    app->mode  = AP_MODE_LIBRARY;
    bind_mode_view(app);
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

    // If the before/after compare was active when the graph was rebuilt,
    // re-apply the bypass to the new graph. The rebuild produced fresh
    // stages with skip flags matching the entries' enabled state; we
    // need to override them again.
    if (app->compare_original) {
        app->compare_original = false;   // clear so set_ sees a change
        ap_app_set_compare_original(app, true);
    }
}

bool ap_app_photo_loading(const ap_app *app)
{
    return app ? app->photo_loading : false;
}

bool ap_app_compare_original(const ap_app *app)
{
    return app ? app->compare_original : false;
}

void ap_app_set_compare_original(ap_app *app, bool on)
{
    if (!app || !app->photo) return;
    if (app->compare_original == on) return;
    app->compare_original = on;

    ap_pipeline_graph *graph = ap_photo_graph(app->photo);
    if (!graph) return;

    // Skip (or restore) every stage that belongs to a user-edit stack
    // entry. Transport stages (demosaic, raw_passthrough, output_transfer)
    // have entry_idx == -1 and are left untouched so the display pipeline
    // stays coherent.
    //
    // When restoring (on = false) we reinstate the entry's own enabled
    // flag rather than force-enabling everything: a disabled entry must
    // stay skipped even after the compare ends.
    const ap_edit_stack *stack = ap_photo_stack(app->photo);
    int n = stack ? ap_edit_stack_count(stack) : 0;
    for (int i = 0; i < n; i++) {
        bool want_skip;
        if (on) {
            want_skip = true;
        } else {
            const ap_edit_entry *e = ap_edit_stack_at_const(stack, i);
            want_skip = e ? !e->enabled : false;
        }
        ap_pipeline_graph_set_stage_skip(graph, i, want_skip);
    }
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
    j->base.run     = export_job_run;
    j->rgba         = rgba;
    j->width        = w;
    j->height       = h;
    j->format       = AP_EXPORT_FORMAT_JPEG;
    j->jpeg_quality = quality;
    snprintf(j->out_path, sizeof(j->out_path), "%s", out_path);

    app->export_inflight++;
    AP_INFO("export: queued %s (%dx%d, q=%d)", j->out_path, w, h, quality);
    ap_worker_pool_submit(app->workers, &j->base);
    return 0;
}

// Read back the open photo's rendered output, frame it through its
// viewport, and queue an encode job in the configured format. Shared
// by ap_app_run_export; the caller owns the collision-policy and
// directory-creation decisions and passes a final `out_path`.
static int queue_export_job(ap_app *app, ap_photo *photo,
                            const ap_export_settings *s,
                            const char *out_path)
{
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
    j->base.run       = export_job_run;
    j->rgba           = rgba;
    j->width          = w;
    j->height         = h;
    j->format         = s->format;
    j->jpeg_quality   = s->jpeg_quality;
    j->png_depth      = s->png_depth;
    j->tiff_depth     = s->tiff_depth;
    j->tiff_compress  = s->tiff_compress;
    snprintf(j->out_path, sizeof(j->out_path), "%s", out_path);

    app->export_inflight++;
    AP_INFO("export: queued %s (%dx%d, format=%d)",
            j->out_path, w, h, s->format);
    ap_worker_pool_submit(app->workers, &j->base);
    return 0;
}

ap_export_settings *ap_app_export_settings(ap_app *app)
{
    return app ? &app->export_settings : NULL;
}

void ap_app_enter_export(ap_app *app)
{
    if (!app || !app->photo) return;
    ap_export_settings_load(app->library, &app->export_settings);
    app->mode = AP_MODE_EXPORT;
    bind_mode_view(app);
}

// Build the output path for `src` under `s` using `seq` as the {SEQ}
// token, honouring the collision policy. Returns 0 on success and
// writes the path into `out` / `out_len`; returns 1 when the file
// was skipped by the SKIP policy; returns -1 on a hard error.
static int resolve_output_path(const ap_export_settings *s,
                               const char *src,
                               const char *library_root,
                               int seq,
                               char *out, size_t out_len)
{
    char dir[4096];
    if (ap_export_resolve_dir(s, src, library_root, dir, sizeof(dir)) != 0)
        return -1;
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        AP_ERROR("export: mkdir(%s): %s", dir, strerror(errno));
        return -1;
    }

    const char *slash = strrchr(src, '/');
    const char *base  = slash ? slash + 1 : src;
    char src_stem[1024];
    snprintf(src_stem, sizeof(src_stem), "%s", base);
    char *dot = strrchr(src_stem, '.');
    if (dot) *dot = '\0';

    time_t when = time(NULL);
    {
        struct stat st;
        if (stat(src, &st) == 0) when = st.st_mtime;
    }

    char stem[1024];
    ap_export_format_stem(s, src_stem, when, seq, stem, sizeof(stem));
    const char *ext = ap_export_format_extension(s->format);

    int n = snprintf(out, out_len, "%s/%s.%s", dir, stem, ext);
    if (n <= 0 || (size_t)n >= out_len) {
        AP_ERROR("export: output path too long");
        return -1;
    }

    struct stat st;
    if (stat(out, &st) == 0) {
        if (s->collision == AP_EXPORT_COLLIDE_SKIP)
            return 1;
        if (s->collision == AP_EXPORT_COLLIDE_SUFFIX) {
            int suffix = 1;
            do {
                n = snprintf(out, out_len, "%s/%s_%d.%s",
                             dir, stem, suffix, ext);
                if (n <= 0 || (size_t)n >= out_len) {
                    AP_ERROR("export: suffixed path too long");
                    return -1;
                }
                suffix++;
            } while (stat(out, &st) == 0 && suffix < 10000);
        }
    }
    return 0;
}

int ap_app_run_export(ap_app *app)
{
    if (!app || !app->photo) return -1;
    const ap_export_settings *s = &app->export_settings;
    const char *src = ap_photo_path(app->photo);

    char out[4096];
    int pr = resolve_output_path(s, src,
                                 app->library ? ap_library_root(app->library)
                                              : NULL,
                                 1, out, sizeof(out));
    if (pr < 0) {
        ap_toast_push(AP_TOAST_ERROR, "Export: bad destination.");
        return -1;
    }
    if (pr == 1) {
        ap_toast_push(AP_TOAST_INFO, "Export skipped — file exists.");
        return 1;
    }

    if (queue_export_job(app, app->photo, s, out) != 0) {
        ap_toast_push(AP_TOAST_ERROR, "Export failed — see the log.");
        return -1;
    }

    // Persist the settings now that the user has committed to them.
    ap_export_settings_save(app->library, &app->export_settings);
    return 0;
}

int ap_app_batch_export_selection(ap_app *app, const ap_export_settings *s,
                                  int *out_queued, int *out_skipped)
{
    if (!app || !app->library || !app->grid || !s) return -1;

    if (out_queued)  *out_queued  = 0;
    if (out_skipped) *out_skipped = 0;

    const char *lib_root = ap_library_root(app->library);

    int seq     = 1;
    int queued  = 0;
    int skipped = 0;

    for (int c = 0; c < app->grid_map_count; c++) {
        if (!ap_grid_is_selected(app->grid, c)) continue;
        int i = app->grid_map[c];

        // Skip the currently-open photo: it may have unsaved in-memory
        // edits that differ from the sidecar; the user should close it
        // first if they want to include it.
        if (app->photo && i == app->photo_library_idx) continue;

        char abs[4096];
        if (ap_library_photo_absolute_path(app->library, i,
                                           abs, sizeof(abs)) != 0) {
            AP_WARN("batch export: cannot build path for index %d", i);
            continue;
        }

        char out_path[4096];
        int pr = resolve_output_path(s, abs, lib_root, seq,
                                     out_path, sizeof(out_path));
        if (pr < 0) {
            AP_ERROR("batch export: cannot resolve output path for %s", abs);
            continue;
        }
        if (pr == 1) {
            skipped++;
            seq++;
            continue;
        }

        // Open the photo synchronously on the main thread: decode raw +
        // build GPU pipeline. We operate an entirely separate ap_photo
        // so the canvas / open-photo state is untouched. The readback
        // inside queue_export_job calls vkDeviceWaitIdle on its own
        // graph — no need to touch gpu->current_graph here.
        ap_photo *tmp = ap_photo_open(app->gpu, abs);
        if (!tmp) {
            AP_WARN("batch export: cannot open %s — skipping", abs);
            continue;
        }

        if (queue_export_job(app, tmp, s, out_path) == 0) {
            queued++;
        } else {
            AP_WARN("batch export: encode queue failed for %s", abs);
        }

        // Close without persisting: we did not touch the sidecar and
        // the thumbnail is unchanged. The pipeline readback already
        // called vkDeviceWaitIdle, so the graph's images are idle.
        ap_photo_close(tmp);

        seq++;
    }

    if (out_queued)  *out_queued  = queued;
    if (out_skipped) *out_skipped = skipped;
    return 0;
}

// Rebuild the cell -> library-photo map from the active group filter,
// resize the grid to the visible count, and re-bind each visible
// cell's thumbnail from the library cache (the cells have just been
// reassigned, so their bound textures are stale). The selection and
// focus are preserved: photos that remain visible keep their selected
// state; photos filtered out simply drop from the selection.
static void rebuild_grid_map(ap_app *app)
{
    if (!app->library) {
        app->grid_map_count = 0;
        if (app->grid) ap_grid_set_photo_count(app->grid, 0);
        return;
    }

    int n = ap_library_photo_count(app->library);

    // Snapshot which library indices are selected and which is focused
    // before touching anything, so we can restore them afterward.
    int   old_focus_lib = -1;
    bool *lib_was_sel   = NULL;
    if (app->grid && app->grid_map_count > 0 && n > 0) {
        lib_was_sel = calloc((size_t)n, sizeof(bool));
        if (lib_was_sel) {
            int fc = ap_grid_selected(app->grid);
            if (fc >= 0 && fc < app->grid_map_count)
                old_focus_lib = app->grid_map[fc];
            for (int c = 0; c < app->grid_map_count; c++) {
                if (ap_grid_is_selected(app->grid, c))
                    lib_was_sel[app->grid_map[c]] = true;
            }
        }
    }

    app->grid_map_count = 0;
    if (n > app->grid_map_cap) {
        int *m = realloc(app->grid_map, (size_t)n * sizeof(int));
        if (!m) {
            AP_ERROR("app: grid map allocation failed");
            free(lib_was_sel);
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
        if (show && app->search_buf[0]) {
            const char *rel = ap_library_photo_relative_path(app->library, i);
            if (!rel || !strcasestr(rel, app->search_buf)) {
                show = false;
            }
        }
        if (show) {
            const ap_culling_filter *cf = &app->culling_filter;
            if (cf->rating_min > 0 || cf->flag != AP_FLAG_NONE ||
                    cf->color != AP_COLOR_NONE) {
                ap_photo_culling c = ap_library_photo_culling(app->library, i);
                if (cf->rating_min > 0 && c.rating < cf->rating_min)
                    show = false;
                if (show && cf->flag != AP_FLAG_NONE && c.flag != cf->flag)
                    show = false;
                if (show && cf->color != AP_COLOR_NONE && c.color != cf->color)
                    show = false;
            }
        }
        if (show) {
            app->grid_map[app->grid_map_count++] = i;
        }
    }

    if (!app->grid) {
        free(lib_was_sel);
        return;
    }
    ap_grid_set_photo_count(app->grid, app->grid_map_count);

    if (lib_was_sel && app->grid_map_count > 0) {
        // Restore focus to the cell showing the previously-focused photo,
        // falling back to cell 0 if that photo is no longer visible.
        int new_focus = 0;
        if (old_focus_lib >= 0 && old_focus_lib < n) {
            for (int c = 0; c < app->grid_map_count; c++) {
                if (app->grid_map[c] == old_focus_lib) {
                    new_focus = c;
                    break;
                }
            }
        }
        ap_grid_select_only(app->grid, new_focus);
        for (int c = 0; c < app->grid_map_count; c++) {
            if (c == new_focus) continue;
            if (lib_was_sel[app->grid_map[c]])
                ap_grid_select_toggle(app->grid, c);
        }
    } else {
        ap_grid_set_selected(app->grid, 0);
    }

    free(lib_was_sel);

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
    app->sort                 = AP_SORT_PATH;
    app->search_buf[0]        = '\0';
    app->culling_filter       = (ap_culling_filter){ 0, AP_FLAG_NONE, AP_COLOR_NONE };
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
    drain_all_workers(app);
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

void ap_app_request_import(ap_app *app)
{
    if (app && app->library) app->import_modal = true;
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

int ap_app_copy_edits(ap_app *app)
{
    if (!app || !app->photo) return -1;
    const ap_edit_stack *stack = ap_photo_stack(app->photo);
    if (!stack) return -1;
    app->edit_clipboard       = *stack;
    app->edit_clipboard_valid = true;
    return 0;
}

int ap_app_paste_edits(ap_app *app)
{
    if (!app || !app->photo || !app->edit_clipboard_valid) return -1;
    ap_edit_stack *stack = ap_photo_stack(app->photo);
    if (!stack) return -1;
    ap_photo_edit_snapshot(app->photo);
    *stack = app->edit_clipboard;
    ap_app_rebuild_photo_graph(app);
    return 0;
}

bool ap_app_has_edit_clipboard(const ap_app *app)
{
    return app && app->edit_clipboard_valid;
}

void ap_app_edit_snapshot(ap_app *app)
{
    if (!app || !app->photo) return;
    ap_photo_edit_snapshot(app->photo);
}

bool ap_app_undo(ap_app *app)
{
    if (!app || !app->photo) return false;
    if (!ap_photo_undo(app->photo)) return false;
    ap_app_rebuild_photo_graph(app);
    return true;
}

bool ap_app_redo(ap_app *app)
{
    if (!app || !app->photo) return false;
    if (!ap_photo_redo(app->photo)) return false;
    ap_app_rebuild_photo_graph(app);
    return true;
}

int ap_app_sync_edits_to_selection(ap_app *app)
{
    if (!app || !app->library || !app->grid) return -1;
    if (!app->edit_clipboard_valid) return -1;

    int wrote = 0;
    for (int c = 0; c < app->grid_map_count; c++) {
        if (!ap_grid_is_selected(app->grid, c)) continue;
        int i = app->grid_map[c];
        if (app->photo && i == app->photo_library_idx) continue;
        if (ap_library_apply_stack_to_photo(app->library, i,
                                            &app->edit_clipboard) == 0) {
            wrote++;
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

// Culling field to mutate across the selection. Each enumerator
// targets exactly one ap_photo_culling member.
typedef enum {
    AP_CULL_RATING = 0,
    AP_CULL_FLAG   = 1,
    AP_CULL_COLOR  = 2,
} ap_cull_field;

// Apply a single culling-field change to every selected grid photo.
// `value` is interpreted per `field`: a rating int, an ap_flag, or an
// ap_color_label. Returns the number of photos written, or -1 on a
// missing library / grid.
static int apply_culling_to_selection(ap_app *app, ap_cull_field field,
                                      int value)
{
    if (!app || !app->library || !app->grid) return -1;

    int wrote = 0;
    for (int c = 0; c < app->grid_map_count; c++) {
        if (!ap_grid_is_selected(app->grid, c)) continue;
        int i = app->grid_map[c];
        ap_photo_culling cull = ap_library_photo_culling(app->library, i);
        switch (field) {
        case AP_CULL_RATING: cull.rating = value;                  break;
        case AP_CULL_FLAG:   cull.flag   = (ap_flag)value;         break;
        case AP_CULL_COLOR:  cull.color  = (ap_color_label)value;  break;
        }
        if (ap_library_set_photo_culling(app->library, i, cull) == 0) {
            wrote++;
        }
    }
    return wrote;
}

int ap_app_set_selection_rating(ap_app *app, int rating)
{
    return apply_culling_to_selection(app, AP_CULL_RATING,
                                      ap_rating_clamp(rating));
}

int ap_app_set_selection_flag(ap_app *app, ap_flag flag)
{
    return apply_culling_to_selection(app, AP_CULL_FLAG, (int)flag);
}

int ap_app_set_selection_color(ap_app *app, ap_color_label color)
{
    return apply_culling_to_selection(app, AP_CULL_COLOR, (int)color);
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

void ap_app_set_sort(ap_app *app, ap_library_sort sort)
{
    if (!app || !app->library) return;
    if (app->sort == sort) return;
    app->sort = sort;
    // Drain in-flight thumbnail jobs before reordering: jobs carry a
    // library index and the reload shifts every index.
    drain_all_workers(app);
    ap_app_wait_idle(app);
    ap_library_reload_sorted(app->library, sort);
    // Clear grid thumbnails: the thumbnail cache (thumbs[]) was reset
    // by the reload, and grid cells still hold stale VkImageViews.
    if (app->grid) {
        int c;
        for (c = 0; c < app->grid_map_count; c++) {
            ap_grid_set_thumbnail(app->grid, c, VK_NULL_HANDLE, VK_NULL_HANDLE);
        }
    }
    rebuild_grid_map(app);
}

ap_library_sort ap_app_sort(const ap_app *app)
{
    return app ? app->sort : AP_SORT_PATH;
}

void ap_app_set_search(ap_app *app, const char *query)
{
    if (!app) return;
    snprintf(app->search_buf, sizeof(app->search_buf), "%s", query ? query : "");
    rebuild_grid_map(app);
}

const char *ap_app_search(const ap_app *app)
{
    return app ? app->search_buf : "";
}

ap_culling_filter ap_app_culling_filter(const ap_app *app)
{
    if (!app) return (ap_culling_filter){ 0, AP_FLAG_NONE, AP_COLOR_NONE };
    return app->culling_filter;
}

void ap_app_set_culling_filter(ap_app *app, ap_culling_filter filter)
{
    if (!app) return;
    app->culling_filter = filter;
    rebuild_grid_map(app);
}

void ap_app_set_canvas_tool(ap_app *app, ap_canvas_tool tool, int entry_idx)
{
    if (!app) return;
    // Re-arming the same tool on the same entry toggles it off, so the
    // config-window button reads as a press-on / press-off control.
    if (tool != AP_CANVAS_TOOL_NONE &&
        app->canvas_tool == tool && app->canvas_tool_entry == entry_idx) {
        app->canvas_tool       = AP_CANVAS_TOOL_NONE;
        app->canvas_tool_entry = -1;
    } else {
        app->canvas_tool       = tool;
        app->canvas_tool_entry = (tool == AP_CANVAS_TOOL_NONE) ? -1
                                                               : entry_idx;
    }
    app->crop_drag_handle = CROP_HANDLE_NONE;
}

ap_canvas_tool ap_app_canvas_tool(const ap_app *app)
{
    return app ? app->canvas_tool : AP_CANVAS_TOOL_NONE;
}

int ap_app_canvas_tool_entry(const ap_app *app)
{
    return app ? app->canvas_tool_entry : -1;
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

    // Refresh the outgoing photo's thumbnail before the async open
    // replaces app->photo — the readback must happen while the graph
    // is still live.
    submit_thumb_refresh(app, app->photo_library_idx);

    app->photo_library_idx = new_idx;
    ap_app_open_photo(app, abs);
}

// ---- interactive canvas tools ----------------------------------------

// Resolve the edit-stack entry an armed canvas tool drives, verifying
// it is still the module the tool expects. A stack reorder / removal
// can shift indices out from under the stored binding; when the entry
// no longer matches, the tool is disarmed and NULL returned rather
// than writing into the wrong module's params.
static ap_edit_entry *canvas_tool_entry(ap_app *app, const char *module)
{
    if (!app->photo || app->canvas_tool_entry < 0) return NULL;
    ap_edit_stack *stack = ap_photo_stack(app->photo);
    if (!stack) return NULL;
    ap_edit_entry *e = ap_edit_stack_at(stack, app->canvas_tool_entry);
    if (!e || strcmp(e->module_name, module) != 0) {
        app->canvas_tool       = AP_CANVAS_TOOL_NONE;
        app->canvas_tool_entry = -1;
        return NULL;
    }
    return e;
}

// White-balance eyedropper: on a left-click inside the image, sample
// the clicked pixel from the rendered output and solve the bound White
// Balance entry's multipliers to neutralise it. The rendered image is
// read back in full (synchronous, one-shot on click) and the source
// pixel under the cursor is sampled. Returns true when the click was
// consumed so the caller skips view panning this frame.
static bool drive_wb_eyedropper(ap_app *app, ImGuiIO *io)
{
    ap_edit_entry *e = canvas_tool_entry(app, "wb");
    if (!e) return false;

    igSetMouseCursor(ImGuiMouseCursor_Hand);

    if (io->WantCaptureMouse) return false;
    if (!igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) return false;

    int win_w = (int)io->DisplaySize.x;
    int win_h = (int)io->DisplaySize.y;

    float u = 0.0f, v = 0.0f;
    if (!ap_canvas_screen_to_source_uv(app->canvas, io->MousePos.x,
                                       io->MousePos.y, win_w, win_h,
                                       &u, &v)) {
        return false;   // clicked outside the image — ignore
    }

    ap_pipeline_graph *graph = ap_photo_graph(app->photo);
    if (!graph) return false;
    int gw = ap_pipeline_graph_output_width(graph);
    int gh = ap_pipeline_graph_output_height(graph);
    if (gw <= 0 || gh <= 0) return false;

    size_t bytes = (size_t)gw * (size_t)gh * 4u;
    uint8_t *rgba = malloc(bytes);
    if (!rgba) {
        AP_ERROR("eyedropper: out of memory (%zu bytes)", bytes);
        return true;
    }
    if (ap_pipeline_graph_readback(graph, rgba, bytes) != 0) {
        free(rgba);
        ap_toast_push(AP_TOAST_ERROR, "Eyedropper: pixel readback failed.");
        return true;
    }

    int px = (int)(u * (float)gw);
    int py = (int)(v * (float)gh);
    if (px < 0) px = 0; else if (px >= gw) px = gw - 1;
    if (py < 0) py = 0; else if (py >= gh) py = gh - 1;
    const uint8_t *p = &rgba[((size_t)py * (size_t)gw + (size_t)px) * 4u];
    float sr = (float)p[0], sg = (float)p[1], sb = (float)p[2];
    free(rgba);

    ap_app_edit_snapshot(app);
    if (ap_wb_apply_neutral_pick(e->params, sr, sg, sb)) {
        ap_app_rebuild_photo_graph(app);
        ap_toast_push(AP_TOAST_INFO, "White balance set from picked pixel.");
    } else {
        ap_toast_push(AP_TOAST_INFO,
                      "Eyedropper: pixel too dark — pick a brighter area.");
    }
    return true;
}

// Crop-overlay geometry: the eight handle positions of the crop rect
// in framed-output [0,1] space. The crop rect is itself a [0,1] sub-
// rect of the framed output. Order matches the CROP_HANDLE_* enum for
// the corner / edge handles (0..7).
static void crop_handle_uv(const ap_viewport *vp, int handle,
                           float *u, float *v)
{
    float x0 = vp->crop_x0, y0 = vp->crop_y0;
    float x1 = vp->crop_x1, y1 = vp->crop_y1;
    float mx = 0.5f * (x0 + x1);
    float my = 0.5f * (y0 + y1);
    switch (handle) {
    case CROP_HANDLE_TL: *u = x0; *v = y0; break;
    case CROP_HANDLE_TR: *u = x1; *v = y0; break;
    case CROP_HANDLE_BL: *u = x0; *v = y1; break;
    case CROP_HANDLE_BR: *u = x1; *v = y1; break;
    case CROP_HANDLE_L:  *u = x0; *v = my; break;
    case CROP_HANDLE_R:  *u = x1; *v = my; break;
    case CROP_HANDLE_T:  *u = mx; *v = y0; break;
    case CROP_HANDLE_B:  *u = mx; *v = y1; break;
    default:             *u = mx; *v = my; break;
    }
}

// While the crop tool is armed the app feeds the canvas a viewport
// with an identity crop (full frame visible, see crop_tool_viewport),
// so the canvas's framed-output [0,1] space is the whole rotated
// frame. The crop rect's crop_x0..y1 are normalized over that same
// frame, so the overlay handles live directly in framed-output space
// — no extra crop-relative remap is needed.

// Re-clamp a crop rect to stay inside [0,1], keep a minimum size, and
// keep x0<x1 / y0<y1.
static void crop_rect_sanitize(ap_viewport *vp)
{
    const float min_sz = 0.02f;
    if (vp->crop_x0 < 0.0f) vp->crop_x0 = 0.0f;
    if (vp->crop_y0 < 0.0f) vp->crop_y0 = 0.0f;
    if (vp->crop_x1 > 1.0f) vp->crop_x1 = 1.0f;
    if (vp->crop_y1 > 1.0f) vp->crop_y1 = 1.0f;
    if (vp->crop_x1 - vp->crop_x0 < min_sz) {
        if (vp->crop_x0 + min_sz <= 1.0f) vp->crop_x1 = vp->crop_x0 + min_sz;
        else vp->crop_x0 = vp->crop_x1 - min_sz;
    }
    if (vp->crop_y1 - vp->crop_y0 < min_sz) {
        if (vp->crop_y0 + min_sz <= 1.0f) vp->crop_y1 = vp->crop_y0 + min_sz;
        else vp->crop_y0 = vp->crop_y1 - min_sz;
    }
}

// Apply a drag at framed-output coordinate (u,v) to the crop rect for
// the given handle. When the aspect lock is on, edge / corner drags
// hold app->crop_aspect_ratio (width/height of the framed image's crop
// in pixels). The straighten handle is handled separately.
static void crop_apply_drag(ap_app *app, ap_viewport *vp, int handle,
                            float u, float v, int img_w, int img_h)
{
    if (u < 0.0f) u = 0.0f; else if (u > 1.0f) u = 1.0f;
    if (v < 0.0f) v = 0.0f; else if (v > 1.0f) v = 1.0f;

    if (handle == CROP_HANDLE_MOVE) {
        float w  = vp->crop_x1 - vp->crop_x0;
        float h  = vp->crop_y1 - vp->crop_y0;
        float nx = u - w * 0.5f;
        float ny = v - h * 0.5f;
        if (nx < 0.0f) nx = 0.0f;
        if (ny < 0.0f) ny = 0.0f;
        if (nx + w > 1.0f) nx = 1.0f - w;
        if (ny + h > 1.0f) ny = 1.0f - h;
        vp->crop_x0 = nx; vp->crop_y0 = ny;
        vp->crop_x1 = nx + w; vp->crop_y1 = ny + h;
        return;
    }

    switch (handle) {
    case CROP_HANDLE_TL: vp->crop_x0 = u; vp->crop_y0 = v; break;
    case CROP_HANDLE_TR: vp->crop_x1 = u; vp->crop_y0 = v; break;
    case CROP_HANDLE_BL: vp->crop_x0 = u; vp->crop_y1 = v; break;
    case CROP_HANDLE_BR: vp->crop_x1 = u; vp->crop_y1 = v; break;
    case CROP_HANDLE_L:  vp->crop_x0 = u; break;
    case CROP_HANDLE_R:  vp->crop_x1 = u; break;
    case CROP_HANDLE_T:  vp->crop_y0 = v; break;
    case CROP_HANDLE_B:  vp->crop_y1 = v; break;
    default: break;
    }

    // Keep ordering before the aspect step so width / height are signed
    // correctly.
    if (vp->crop_x1 < vp->crop_x0) {
        float t = vp->crop_x0; vp->crop_x0 = vp->crop_x1; vp->crop_x1 = t;
    }
    if (vp->crop_y1 < vp->crop_y0) {
        float t = vp->crop_y0; vp->crop_y0 = vp->crop_y1; vp->crop_y1 = t;
    }

    // Aspect lock: after a free drag, recover the locked ratio by
    // pulling the axis that was *not* dragged. Ratio is in pixels, so
    // convert through the framed image dimensions.
    if (app->crop_aspect_locked && app->crop_aspect_ratio > 0.0f &&
        img_w > 0 && img_h > 0) {
        float ratio_norm = app->crop_aspect_ratio
                         * (float)img_h / (float)img_w;  // norm-w / norm-h
        bool x_edge = (handle == CROP_HANDLE_L || handle == CROP_HANDLE_R);
        bool y_edge = (handle == CROP_HANDLE_T || handle == CROP_HANDLE_B);
        float cw = vp->crop_x1 - vp->crop_x0;
        float ch = vp->crop_y1 - vp->crop_y0;
        if (y_edge) {
            cw = ch * ratio_norm;   // height changed -> match width
        } else if (x_edge) {
            ch = cw / ratio_norm;   // width changed -> match height
        } else {
            // Corner: keep width, derive height.
            ch = cw / ratio_norm;
        }
        // Re-anchor on the fixed corner so the dragged handle leads.
        switch (handle) {
        case CROP_HANDLE_TL:
            vp->crop_x0 = vp->crop_x1 - cw;
            vp->crop_y0 = vp->crop_y1 - ch; break;
        case CROP_HANDLE_TR:
            vp->crop_x1 = vp->crop_x0 + cw;
            vp->crop_y0 = vp->crop_y1 - ch; break;
        case CROP_HANDLE_BL:
            vp->crop_x0 = vp->crop_x1 - cw;
            vp->crop_y1 = vp->crop_y0 + ch; break;
        case CROP_HANDLE_BR:
            vp->crop_x1 = vp->crop_x0 + cw;
            vp->crop_y1 = vp->crop_y0 + ch; break;
        case CROP_HANDLE_L: case CROP_HANDLE_R: {
            float mid = 0.5f * (vp->crop_y0 + vp->crop_y1);
            vp->crop_y0 = mid - ch * 0.5f;
            vp->crop_y1 = mid + ch * 0.5f; break;
        }
        case CROP_HANDLE_T: case CROP_HANDLE_B: {
            float mid = 0.5f * (vp->crop_x0 + vp->crop_x1);
            vp->crop_x0 = mid - cw * 0.5f;
            vp->crop_x1 = mid + cw * 0.5f; break;
        }
        default: break;
        }
    }

    crop_rect_sanitize(vp);
}

// Interactive crop / straighten overlay. Drives the bound Transform
// entry's crop + rotation slots from drag handles. While the tool is
// armed the canvas is fed an identity-crop viewport (full frame
// visible) so the user always sees the whole image to crop within;
// drive_canvas_view's pan is suppressed by the caller. Returns nothing
// — the drawing half is draw_crop_overlay.
static void drive_crop_tool(ap_app *app, ImGuiIO *io)
{
    ap_edit_entry *e = canvas_tool_entry(app, "transform");
    if (!e) return;

    int win_w = (int)io->DisplaySize.x;
    int win_h = (int)io->DisplaySize.y;
    int img_w = ap_photo_width(app->photo);
    int img_h = ap_photo_height(app->photo);

    ap_viewport vp = ap_transform_viewport(e->params);

    // Hit-test radius for handles, in screen pixels.
    const float grab_r = 11.0f;

    if (io->WantCaptureMouse && app->crop_drag_handle == CROP_HANDLE_NONE) {
        return;
    }

    // Begin a drag: pick the closest handle under the cursor. The
    // straighten handle (a separate gesture) starts on a Shift-click
    // anywhere inside the crop rect; a plain click inside grabs MOVE.
    if (igIsMouseClicked_Bool(ImGuiMouseButton_Left, false) &&
        app->crop_drag_handle == CROP_HANDLE_NONE) {
        float best_d2 = grab_r * grab_r;
        int   best    = CROP_HANDLE_NONE;
        for (int h = CROP_HANDLE_TL; h <= CROP_HANDLE_B; h++) {
            float hu, hv, sx, sy;
            crop_handle_uv(&vp, h, &hu, &hv);
            ap_canvas_framed_uv_to_screen(app->canvas, hu, hv,
                                          win_w, win_h, &sx, &sy);
            float dx = sx - io->MousePos.x;
            float dy = sy - io->MousePos.y;
            float d2 = dx * dx + dy * dy;
            if (d2 < best_d2) { best_d2 = d2; best = h; }
        }
        if (best == CROP_HANDLE_NONE) {
            // Inside the crop rect? -> straighten (Shift) or move.
            float fu, fv;
            ap_canvas_screen_to_framed_uv(app->canvas, io->MousePos.x,
                                          io->MousePos.y, win_w, win_h,
                                          &fu, &fv);
            if (fu >= vp.crop_x0 && fu <= vp.crop_x1 &&
                fv >= vp.crop_y0 && fv <= vp.crop_y1) {
                if (io->KeyShift) {
                    best = CROP_HANDLE_STRAIGHTEN;
                    app->crop_drag_u0 = fu;
                    app->crop_drag_v0 = fv;
                } else {
                    best = CROP_HANDLE_MOVE;
                }
            }
        }
        if (best != CROP_HANDLE_NONE) {
            ap_app_edit_snapshot(app);
            app->crop_drag_handle = best;
        }
    }

    // Continue an active drag.
    if (app->crop_drag_handle != CROP_HANDLE_NONE) {
        if (!igIsMouseDown_Nil(ImGuiMouseButton_Left)) {
            app->crop_drag_handle = CROP_HANDLE_NONE;
        } else if (app->crop_drag_handle == CROP_HANDLE_STRAIGHTEN) {
            // Straighten: the drag vector's angle off horizontal is the
            // correction; add it to the current rotation. Converting to
            // screen space keeps the angle visually true under the
            // canvas zoom (uniform scale -> angle preserved).
            float su0, sv0, su1, sv1;
            ap_canvas_framed_uv_to_screen(app->canvas, app->crop_drag_u0,
                                          app->crop_drag_v0, win_w, win_h,
                                          &su0, &sv0);
            su1 = io->MousePos.x;
            sv1 = io->MousePos.y;
            float dx = su1 - su0;
            float dy = sv1 - sv0;
            if (dx * dx + dy * dy > 4.0f) {
                float ang = atan2f(dy, dx) * (float)(180.0 / M_PI);
                // Snap the drawn line to the nearest of horizontal /
                // vertical: a near-vertical drag means "this should be
                // vertical".
                if (ang > 90.0f)       ang -= 180.0f;
                else if (ang < -90.0f) ang += 180.0f;
                if (ang > 45.0f)       ang -= 90.0f;
                else if (ang < -45.0f) ang += 90.0f;
                float rot = vp.rotation_deg - ang;
                if (rot > 180.0f)  rot -= 360.0f;
                if (rot < -180.0f) rot += 360.0f;
                if (rot > 180.0f)  rot = 180.0f;
                if (rot < -180.0f) rot = -180.0f;
                vp.rotation_deg = rot;
            }
        } else {
            float fu, fv;
            ap_canvas_screen_to_framed_uv(app->canvas, io->MousePos.x,
                                          io->MousePos.y, win_w, win_h,
                                          &fu, &fv);
            crop_apply_drag(app, &vp, app->crop_drag_handle, fu, fv,
                            img_w, img_h);
        }
        ap_transform_set_viewport(e->params, &vp);
    }
}

// Pan / zoom the canvas from the mouse and the F / 0 / 1 view keys.
// Shared by photo mode and export mode — both present a single photo
// on the canvas with the same manipulation feel. Skips entirely while
// ImGui owns the mouse so panel drags don't pan the image.
static void drive_canvas_view(ap_app *app, ImGuiIO *io)
{
    if (io->WantCaptureMouse) return;

    int win_w = (int)io->DisplaySize.x;
    int win_h = (int)io->DisplaySize.y;

    if (io->MouseDown[0] && (io->MouseDelta.x != 0.0f || io->MouseDelta.y != 0.0f)) {
        ap_canvas_pan(app->canvas, io->MouseDelta.x, io->MouseDelta.y,
                      win_w, win_h);
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
                ? 1.0f + ZOOM_FACTOR * io->MouseWheel
                : 1.0f / (1.0f - ZOOM_FACTOR * io->MouseWheel);
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
                          -io->MouseWheel  * pan_step_px,
                          win_w, win_h);
        }
    }

    if (igIsKeyPressed_Bool(ImGuiKey_F, false) ||
        igIsKeyPressed_Bool(ImGuiKey_0, false)) {
        ap_canvas_reset_view(app->canvas);
    } else if (igIsKeyPressed_Bool(ImGuiKey_1, false)) {
        ap_canvas_set_zoom(app->canvas, 1.0f, win_w, win_h);
    }
}

static void drive_canvas_input(ap_app *app)
{
    if (!app->canvas || !app->photo) return;

    ImGuiIO *io = igGetIO_Nil();
    if (!io) return;

    if (igIsKeyPressed_Bool(ImGuiKey_Escape, false)) {
        // Escape disarms an active canvas tool first; only with no tool
        // armed does it close the photo.
        if (app->canvas_tool != AP_CANVAS_TOOL_NONE) {
            ap_app_set_canvas_tool(app, AP_CANVAS_TOOL_NONE, -1);
        } else {
            ap_app_close_photo(app);
        }
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
        if (igIsKeyPressed_Bool(ImGuiKey_Delete, false) &&
            app->photo && app->photo_library_idx >= 0) {
            if (io->KeyShift) {
                delete_edit_photo(app);
            } else {
                app->delete_edit_modal = true;
            }
            return;
        }
    }

    // Interactive canvas tools take the mouse when armed. The
    // eyedropper consumes a click but otherwise leaves the view free
    // to pan / zoom between picks; the crop tool owns mouse drags
    // entirely, so view panning is suppressed while it is armed.
    switch (app->canvas_tool) {
    case AP_CANVAS_TOOL_WB_EYEDROPPER:
        if (drive_wb_eyedropper(app, io)) return;
        break;
    case AP_CANVAS_TOOL_CROP:
        drive_crop_tool(app, io);
        return;
    case AP_CANVAS_TOOL_NONE:
        break;
    }

    drive_canvas_view(app, io);
}

// Export-mode canvas input. The same pan / zoom feel as photo mode,
// but Esc backs out to photo mode rather than closing the photo —
// the photo is the export subject and stays open.
static void drive_export_input(ap_app *app)
{
    if (!app->canvas || !app->photo) return;

    ImGuiIO *io = igGetIO_Nil();
    if (!io) return;

    if (!io->WantTextInput && igIsKeyPressed_Bool(ImGuiKey_Escape, false)) {
        app->mode = AP_MODE_PHOTO;
        bind_mode_view(app);
        return;
    }

    drive_canvas_view(app, io);
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

// De-index and delete every selected library photo from disk. Drains
// worker + GPU work first so no in-flight job acts on a library index
// this removal is about to shift.
static void delete_grid_selection(ap_app *app)
{
    if (!app->library || !app->grid) return;
    if (ap_grid_selection_count(app->grid) <= 0) return;

    drain_all_workers(app);
    ap_app_wait_idle(app);

    int anchor_cell = -1;   // smallest selected cell — the new focus
    int removed     = 0;

    // grid_map is ascending in library index, so walking cells
    // high-to-low removes photos from the back — the indices of the
    // photos still pending stay valid as we go. The descending walk
    // also leaves anchor_cell holding the smallest selected cell.
    for (int c = app->grid_map_count - 1; c >= 0; c--) {
        if (!ap_grid_is_selected(app->grid, c)) continue;
        anchor_cell = c;
        int i = app->grid_map[c];
        // Leave the open photo — deleting its file out from under the
        // edit view would strand it. It can be deleted after closing.
        if (app->photo && i == app->photo_library_idx) continue;
        if (ap_library_photo_remove(app->library, i) != 0) continue;
        if (app->photo && i < app->photo_library_idx) {
            app->photo_library_idx--;
        }
        removed++;
    }
    if (removed == 0) return;

    rebuild_grid_map(app);
    if (app->grid_map_count > 0) {
        // Land on the photo that slid into the lowest deleted cell —
        // the nearest remaining neighbour — and scroll to it, since
        // rebuild_grid_map reset the grid's scroll to the top.
        int target = anchor_cell;
        if (target >= app->grid_map_count) {
            target = app->grid_map_count - 1;
        }
        if (target < 0) target = 0;
        ap_grid_select_only(app->grid, target);

        ImGuiIO *io = igGetIO_Nil();
        if (io) {
            ap_grid_ensure_visible(app->grid, target,
                                   (int)io->DisplaySize.x,
                                   (int)io->DisplaySize.y);
        }
    }
    AP_INFO("library: deleted %d photo(s)", removed);
}

// Close the currently-open photo, de-index it, delete its files from
// disk, then navigate to an adjacent photo (next if available, else
// previous, else return to the library grid).
static void delete_edit_photo(ap_app *app)
{
    if (!app->library || !app->photo) return;

    int idx = app->photo_library_idx;
    if (idx < 0) return;

    drain_all_workers(app);
    ap_app_wait_idle(app);

    // Release the photo before removal so its files are not open.
    release_photo(app);
    app->photo_library_idx = -1;

    if (ap_library_photo_remove(app->library, idx) != 0) {
        // Removal failed — back to library so the user isn't stranded.
        app->mode = AP_MODE_LIBRARY;
        bind_mode_view(app);
        rebuild_grid_map(app);
        return;
    }

    int n_after = ap_library_photo_count(app->library);

    if (n_after <= 0) {
        app->mode = AP_MODE_LIBRARY;
        bind_mode_view(app);
        rebuild_grid_map(app);
        AP_INFO("library: deleted photo (library now empty)");
        return;
    }

    // Try the photo that slid into `idx` (was the next one); if `idx`
    // is now out of range fall back to the last photo.
    int next_idx = (idx < n_after) ? idx : (n_after - 1);

    char abs[4096];
    if (ap_library_photo_absolute_path(app->library, next_idx,
                                       abs, sizeof(abs)) != 0) {
        app->mode = AP_MODE_LIBRARY;
        bind_mode_view(app);
        rebuild_grid_map(app);
        return;
    }

    // Update grid selection so backing out (Esc) lands on the right cell.
    rebuild_grid_map(app);
    {
        int cell = cell_for_photo(app, next_idx);
        if (cell >= 0) ap_grid_select_only(app->grid, cell);
    }

    app->photo_library_idx = next_idx;
    ap_app_open_photo(app, abs);
    AP_INFO("library: deleted photo (navigated to idx %d)", next_idx);
}

// Culling keyboard shortcuts, applied to the whole grid selection:
//
//   0 - 5            set the star rating (0 clears it)
//   P                pick flag
//   X                reject flag
//   U                clear the flag
//   Shift + 1 - 5    colour label: red / yellow / green / blue / purple
//   Shift + 0        clear the colour label
//
// All are gated on !WantTextInput so they don't fire while a panel
// text field is focused, and on !KeyCtrl so they don't collide with
// any chorded shortcut. Shift selects the colour-label variant, so a
// plain digit is unambiguously a rating change.
static void drive_grid_culling_input(ap_app *app, ImGuiIO *io)
{
    if (!app->library || !app->grid) return;
    if (io->WantTextInput || io->KeyCtrl) return;
    if (ap_grid_selection_count(app->grid) <= 0) return;

    for (int d = 0; d <= 5; d++) {
        if (!igIsKeyPressed_Bool((ImGuiKey)(ImGuiKey_0 + d), false)) continue;
        if (io->KeyShift) {
            // Shift + digit -> colour label (digit maps onto the enum
            // value; 0 is AP_COLOR_NONE).
            ap_app_set_selection_color(app, (ap_color_label)d);
        } else {
            ap_app_set_selection_rating(app, d);
        }
        return;
    }

    if (io->KeyShift) return;   // remaining keys are unshifted

    if (igIsKeyPressed_Bool(ImGuiKey_P, false)) {
        ap_app_set_selection_flag(app, AP_FLAG_PICK);
    } else if (igIsKeyPressed_Bool(ImGuiKey_X, false)) {
        ap_app_set_selection_flag(app, AP_FLAG_REJECT);
    } else if (igIsKeyPressed_Bool(ImGuiKey_U, false)) {
        ap_app_set_selection_flag(app, AP_FLAG_NONE);
    }
}

static void draw_grid_context_menu(ap_app *app)
{
    if (!igBeginPopup("##grid_ctx", 0)) return;

    if (!app->library || !app->grid || app->grid_map_count <= 0) {
        igEndPopup();
        return;
    }

    int sel_count   = ap_grid_selection_count(app->grid);
    int focus_cell  = ap_grid_selected(app->grid);

    if (igMenuItem_Bool("Open", NULL, false,
                        focus_cell >= 0 && focus_cell < app->grid_map_count)) {
        open_selected_photo(app);
        igCloseCurrentPopup();
    }

    igSeparator();

    if (igBeginMenu("Set Rating", sel_count > 0)) {
        static const char *const rating_labels[] = {
            "0 (None)", "1 Star", "2 Stars", "3 Stars", "4 Stars", "5 Stars"
        };
        for (int r = 0; r <= 5; r++) {
            if (igMenuItem_Bool(rating_labels[r], NULL, false, true)) {
                ap_app_set_selection_rating(app, r);
            }
        }
        igEndMenu();
    }

    if (igMenuItem_Bool("Pick", "P", false, sel_count > 0)) {
        ap_app_set_selection_flag(app, AP_FLAG_PICK);
    }
    if (igMenuItem_Bool("Reject", "X", false, sel_count > 0)) {
        ap_app_set_selection_flag(app, AP_FLAG_REJECT);
    }
    if (igMenuItem_Bool("Clear Flag", "U", false, sel_count > 0)) {
        ap_app_set_selection_flag(app, AP_FLAG_NONE);
    }

    igSeparator();

    if (igBeginMenu("Set Color Label", sel_count > 0)) {
        static const char *const color_labels[] = {
            "None", "Red", "Yellow", "Green", "Blue", "Purple"
        };
        for (int c = 0; c <= 5; c++) {
            if (igMenuItem_Bool(color_labels[c], NULL, false, true)) {
                ap_app_set_selection_color(app, (ap_color_label)c);
            }
        }
        igEndMenu();
    }

    igSeparator();

    {
        char gnames[256][AP_GROUP_NAME_LEN];
        int gn = ap_library_group_list(app->library, gnames, 256);
        bool has_groups = (gn > 0);
        if (igBeginMenu("Add to Group", has_groups && sel_count > 0)) {
            for (int i = 0; i < gn; i++) {
                if (igMenuItem_Bool(gnames[i], NULL, false, true)) {
                    ap_app_assign_selection_to_group(app, gnames[i], true);
                }
            }
            igEndMenu();
        }
        if (igBeginMenu("Remove from Group", has_groups && sel_count > 0)) {
            for (int i = 0; i < gn; i++) {
                if (igMenuItem_Bool(gnames[i], NULL, false, true)) {
                    ap_app_assign_selection_to_group(app, gnames[i], false);
                }
            }
            igEndMenu();
        }
    }

    igSeparator();

    if (igMenuItem_Bool("Delete", NULL, false, sel_count > 0)) {
        app->delete_modal = true;
        igCloseCurrentPopup();
    }

    igEndPopup();
}

static void drive_grid_input(ap_app *app)
{
    if (!app->library || !app->grid) return;
    int n = app->grid_map_count;
    if (n <= 0) return;

    ImGuiIO *io = igGetIO_Nil();
    if (!io) return;

    // A popup (confirm modal, context menu) owns input while open —
    // don't also drive the grid behind it.
    if (igIsPopupOpen_Str(NULL, ImGuiPopupFlags_AnyPopup)) {
        ap_grid_set_hover(app->grid, -1);
        return;
    }

    int win_w = (int)io->DisplaySize.x;
    int win_h = (int)io->DisplaySize.y;

    // Update the hover index every frame so the shader highlight and
    // filename tooltip stay current. Clear to -1 when ImGui owns the
    // mouse (a panel is active) so panels don't leave a stale highlight.
    {
        int hover = io->WantCaptureMouse ? -1
                  : ap_grid_hit_test(app->grid,
                                     io->MousePos.x, io->MousePos.y,
                                     win_w, win_h);
        ap_grid_set_hover(app->grid, hover);
        if (hover >= 0 && hover < app->grid_map_count) {
            int i = app->grid_map[hover];
            const char *rel = ap_library_photo_relative_path(app->library, i);
            if (rel) igSetTooltip("%s", rel);
            igSetMouseCursor(ImGuiMouseCursor_Hand);
        }
    }

    if (!io->WantCaptureMouse) {
        if (igIsMouseClicked_Bool(ImGuiMouseButton_Right, false)) {
            int hit = ap_grid_hit_test(app->grid,
                                       io->MousePos.x, io->MousePos.y,
                                       win_w, win_h);
            if (hit >= 0) {
                if (!ap_grid_is_selected(app->grid, hit)) {
                    ap_grid_select_only(app->grid, hit);
                }
                igOpenPopup_Str("##grid_ctx", 0);
            }
        }
        if (io->MouseWheel != 0.0f) {
            if (io->KeyCtrl) {
                int cur = ap_grid_cell_size(app->grid);
                float factor = io->MouseWheel > 0.0f
                    ? 1.0f + ZOOM_FACTOR * io->MouseWheel
                    : 1.0f / (1.0f - ZOOM_FACTOR * io->MouseWheel);
                int next = (int)((float)cur * factor + 0.5f);
                ap_grid_zoom_at(app->grid, next,
                                io->MousePos.x, io->MousePos.y,
                                win_w, win_h);
            } else {
                const float wheel_step_px = 60.0f;
                ap_grid_scroll(app->grid, -io->MouseWheel * wheel_step_px,
                               win_w, win_h);
            }
        }
        if (igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left)) {
            app->marquee_active   = false;
            app->thumb_drag_active = false;
            int hit = ap_grid_hit_test(app->grid,
                                       io->MousePos.x, io->MousePos.y,
                                       win_w, win_h);
            if (hit >= 0) {
                ap_grid_select_only(app->grid, hit);
                open_selected_photo(app);
                return;
            }
        } else if (igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) {
            // Record click origin for marquee / thumb-drag detection.
            app->marquee_x0 = io->MousePos.x;
            app->marquee_y0 = io->MousePos.y;
            // Any new press cancels a pending deferred select from a
            // previous down stroke.
            app->deferred_select_cell = -1;
            int hit = ap_grid_hit_test(app->grid,
                                       io->MousePos.x, io->MousePos.y,
                                       win_w, win_h);
            if (hit >= 0) {
                int anchor = ap_grid_selected(app->grid);
                if (io->KeyShift) {
                    ap_grid_select_range(app->grid, anchor, hit);
                } else if (io->KeyCtrl) {
                    ap_grid_select_toggle(app->grid, hit);
                } else if (ap_grid_is_selected(app->grid, hit) &&
                           ap_grid_selection_count(app->grid) > 1) {
                    // Down on an already-selected cell in a multi-selection
                    // with no modifier — defer select-only until mouse-up so
                    // a drag keeps the whole selection.
                    app->deferred_select_cell = hit;
                } else {
                    ap_grid_select_only(app->grid, hit);
                }
            } else {
                // Down on empty space — potential marquee start.
                app->marquee_active = true;
            }
        }

        // Deferred select-only: if the mouse was released without dragging,
        // apply the select-only that was held back on mouse-down.
        if (app->deferred_select_cell >= 0 &&
            !igIsMouseDown_Nil(ImGuiMouseButton_Left) &&
            !igIsMouseDragging(ImGuiMouseButton_Left, -1.0f)) {
            ap_grid_select_only(app->grid, app->deferred_select_cell);
            app->deferred_select_cell = -1;
        }

        // Rubber-band marquee: drag began on empty space. Cancelled if
        // the mouse leaves the grid area (WantCaptureMouse becomes true).
        if (app->marquee_active) {
            if (!igIsMouseDown_Nil(ImGuiMouseButton_Left)) {
                app->marquee_active = false;
            } else if (igIsMouseDragging(ImGuiMouseButton_Left, -1.0f)) {
                ap_grid_select_rect(app->grid,
                                    app->marquee_x0, app->marquee_y0,
                                    io->MousePos.x,  io->MousePos.y,
                                    win_w, win_h);
            }
        }

        // Thumbnail drag initiation: drag began on a selected cell.
        // When a deferred select is pending the mouse went down on an
        // already-selected cell — keep the full selection and start the
        // drag without collapsing it.
        if (!app->marquee_active && !app->thumb_drag_active) {
            if (igIsMouseDragging(ImGuiMouseButton_Left, -1.0f)) {
                if (app->deferred_select_cell >= 0) {
                    app->deferred_select_cell = -1;
                    app->thumb_drag_active = true;
                } else {
                    int hit = ap_grid_hit_test(app->grid,
                                               app->marquee_x0, app->marquee_y0,
                                               win_w, win_h);
                    if (hit >= 0 && ap_grid_is_selected(app->grid, hit) &&
                        ap_grid_selection_count(app->grid) > 0) {
                        app->thumb_drag_active = true;
                    }
                }
            }
        }
    } else {
        // ImGui owns the mouse — cancel the marquee (it can't extend into
        // panels) and any pending deferred select. The thumb drag persists:
        // it was initiated on the grid and must reach the Groups panel to
        // deliver its payload.
        app->marquee_active = false;
        app->deferred_select_cell = -1;
    }

    // Thumbnail drag-drop source: runs regardless of WantCaptureMouse so
    // the payload stays live while the user hovers over the Groups panel.
    // Uses SourceExtern to emit without a prior ImGui widget interaction.
    if (app->thumb_drag_active) {
        if (!igIsMouseDown_Nil(ImGuiMouseButton_Left)) {
            app->thumb_drag_active = false;
        } else if (igBeginDragDropSource(ImGuiDragDropFlags_SourceExtern)) {
            int sel = ap_grid_selection_count(app->grid);
            igSetDragDropPayload("AP_THUMB_DRAG", &sel, sizeof(int),
                                 ImGuiCond_Once);
            igText("%d photo(s)", sel);
            igEndDragDropSource();
        }
    }

    int sel = ap_grid_selected(app->grid);
    int new_sel = sel;
    int cpr = ap_grid_cells_per_row(app->grid, win_w, win_h);

    // Rows-per-page: how many full rows fit in the render rect height.
    // Used by PageUp / PageDown to advance exactly one viewport of rows.
    int rows_per_page = ap_grid_rows_per_page(app->grid, win_w, win_h);

    // Gate keyboard nav on WantTextInput so arrows / Home / End /
    // PageUp / PageDown don't walk the grid while a panel text field
    // (search box, rename field) has focus.
    if (!io->WantTextInput) {
        if      (igIsKeyPressed_Bool(ImGuiKey_RightArrow, true)) new_sel = sel + 1;
        else if (igIsKeyPressed_Bool(ImGuiKey_LeftArrow,  true)) new_sel = sel - 1;
        else if (igIsKeyPressed_Bool(ImGuiKey_DownArrow,  true)) new_sel = sel + cpr;
        else if (igIsKeyPressed_Bool(ImGuiKey_UpArrow,    true)) new_sel = sel - cpr;
        else if (igIsKeyPressed_Bool(ImGuiKey_Home,  false))     new_sel = 0;
        else if (igIsKeyPressed_Bool(ImGuiKey_End,   false))     new_sel = n - 1;
        else if (igIsKeyPressed_Bool(ImGuiKey_PageDown, true))   new_sel = sel + rows_per_page * cpr;
        else if (igIsKeyPressed_Bool(ImGuiKey_PageUp,   true))   new_sel = sel - rows_per_page * cpr;
    }
    if (new_sel != sel) {
        if (io->KeyShift) {
            ap_grid_select_range(app->grid, sel, new_sel);
        } else {
            ap_grid_select_only(app->grid, new_sel);
        }
        ap_grid_ensure_visible(app->grid, ap_grid_selected(app->grid),
                               win_w, win_h);
    }

    if (!io->KeyCtrl && !io->WantTextInput &&
        (igIsKeyPressed_Bool(ImGuiKey_Enter, false) ||
         igIsKeyPressed_Bool(ImGuiKey_Space, false))) {
        open_selected_photo(app);
    }

    // Delete: confirm-then-remove the selection; Shift+Delete skips
    // the prompt. Gated on WantTextInput so it doesn't fire while a
    // panel text field (e.g. group rename) is focused.
    if (!io->WantTextInput &&
        igIsKeyPressed_Bool(ImGuiKey_Delete, false) &&
        ap_grid_selection_count(app->grid) > 0) {
        if (io->KeyShift) {
            delete_grid_selection(app);
        } else {
            app->delete_modal = true;
        }
    }

    drive_grid_culling_input(app, io);
}

static void draw_selection_overlay(ap_app *app)
{
    if (!app->library || !app->grid) return;
    int n = app->grid_map_count;
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
        // cimgui's AddRect is (..., rounding, thickness, flags) — the
        // last two are swapped vs upstream Dear ImGui's C++ signature.
        ImDrawList_AddRect(dl, tl, br, 0xFFB8C4D9, 0.0f, 2.0f, 0);
    }
}

static void draw_marquee_overlay(ap_app *app)
{
    if (!app->marquee_active) return;
    ImGuiIO *io = igGetIO_Nil();
    if (!io) return;
    if (!igIsMouseDragging(ImGuiMouseButton_Left, -1.0f)) return;

    ImDrawList *dl = igGetForegroundDrawList_ViewportPtr(NULL);
    if (!dl) return;

    ImVec2_c tl = { app->marquee_x0 < io->MousePos.x
                        ? app->marquee_x0 : io->MousePos.x,
                    app->marquee_y0 < io->MousePos.y
                        ? app->marquee_y0 : io->MousePos.y };
    ImVec2_c br = { app->marquee_x0 < io->MousePos.x
                        ? io->MousePos.x : app->marquee_x0,
                    app->marquee_y0 < io->MousePos.y
                        ? io->MousePos.y : app->marquee_y0 };

    ImDrawList_AddRectFilled(dl, tl, br, 0x26B8C4D9, 0.0f, 0);
    ImDrawList_AddRect(dl, tl, br, 0xCCB8C4D9, 0.0f, 1.0f, 0);
}

// Paint the culling overlay for one grid cell: a colour-label strip
// across the top of the fitted image, a pick / reject flag badge in
// the top-left corner, and the star rating as filled dots right-
// aligned inside the filename band. `fit_*` is the letterboxed image
// rect; `band_top` / `band_h` describe the filename band beneath it.
static void draw_cell_culling(ImDrawList *dl, const ap_photo_culling *cull,
                              float fit_x, float fit_y, float fit_w,
                              float band_top, float band_h)
{
    if (ap_photo_culling_is_empty(cull)) return;

    // Colour-label strip along the top edge of the image.
    if (cull->color != AP_COLOR_NONE) {
        const float strip_h = 4.0f;
        ImVec2_c s_tl = { fit_x,          fit_y           };
        ImVec2_c s_br = { fit_x + fit_w,  fit_y + strip_h };
        ImDrawList_AddRectFilled(dl, s_tl, s_br,
                                 ap_color_label_rgba(cull->color), 0.0f, 0);
    }

    // Pick / reject badge: a filled circle in the top-left corner.
    if (cull->flag != AP_FLAG_NONE) {
        const float r = 5.0f;
        ImVec2_c centre = { fit_x + r + 3.0f, fit_y + r + 3.0f };
        // Green pick, red reject — packed 0xAABBGGRR.
        unsigned fill = (cull->flag == AP_FLAG_PICK)
                            ? 0xFF4CB752u : 0xFF4242E5u;
        ImDrawList_AddCircleFilled(dl, centre, r, fill, 0);
        ImDrawList_AddCircle(dl, centre, r, 0xFF101010u, 0, 1.5f);
    }

    // Rating dots, right-aligned inside the filename band.
    if (cull->rating > 0) {
        const float dot_r  = 2.5f;
        const float dot_gap = 3.0f;
        const float pitch  = dot_r * 2.0f + dot_gap;
        float total = pitch * (float)cull->rating - dot_gap;
        float cy_dot = band_top + band_h * 0.5f;
        float x = fit_x + fit_w - 4.0f - total + dot_r;
        for (int s = 0; s < cull->rating; s++) {
            ImVec2_c centre = { x, cy_dot };
            ImDrawList_AddCircleFilled(dl, centre, dot_r, 0xFF55D6F2u, 0);
            x += pitch;
        }
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
        const char *slash = strrchr(rel, '/');
        const char *label = slash ? slash + 1 : rel;
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

        ap_photo_culling cull = ap_library_photo_culling(app->library, i);

        // Reserve room on the band's right edge for the rating dots so
        // a long filename never paints over them.
        float text_right = fit_x + fit_w - 4.0f;
        if (cull.rating > 0) {
            const float pitch = 2.5f * 2.0f + 3.0f;
            text_right -= pitch * (float)cull.rating - 3.0f + 4.0f;
        }
        ImVec2_c text_pos = { fit_x + 4.0f, band_top + 2.0f };
        ImVec2_c clip_tl  = { fit_x,      band_top          };
        ImVec2_c clip_br  = { text_right, band_top + band_h };
        ImDrawList_PushClipRect(dl, clip_tl, clip_br, true);
        ImDrawList_AddText_Vec2(dl, text_pos, 0xFFEEEEEE, label, NULL);
        ImDrawList_PopClipRect(dl);

        draw_cell_culling(dl, &cull, fit_x, fit_y, fit_w,
                          band_top, band_h);
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
    int rc;
    switch (j->format) {
    case AP_EXPORT_FORMAT_TIFF:
        // The GPU readback is 8-bit sRGB RGBA; a UINT16 TIFF still
        // gets a wider container (libtiff expands the 8-bit samples)
        // even though no extra precision is recovered from the
        // readback. No ICC blob is embedded yet — see issue #206.
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

static void handle_export_complete(ap_app *app, export_job *j)
{
    if (app->export_inflight > 0) app->export_inflight--;
    if (j->ok) {
        // Show only the filename component — paths can be long and the
        // toast card is narrow.
        const char *slash = strrchr(j->out_path, '/');
        const char *name  = slash ? slash + 1 : j->out_path;
        ap_toast_push(AP_TOAST_INFO, "Saved %s", name);
    } else {
        ap_toast_push(AP_TOAST_ERROR, "Export failed — see the log.");
    }
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

        if (igMenuItem_Bool("Close Library", NULL,
                            false, app->library != NULL)) {
            ap_app_close_library(app);
        }

        igSeparator();

        if (igMenuItem_Bool("Quit", "Ctrl+Q", false, true)) {
            app->quit_requested = true;
        }
        igEndMenu();
    }

    if (igBeginMenu("Photo", app->photo != NULL)) {
        if (igMenuItem_Bool("Close", "Esc", false, app->photo != NULL)) {
            ap_app_close_photo(app);
        }
        if (igMenuItem_Bool("Delete", NULL, false,
                            app->photo != NULL &&
                            app->photo_library_idx >= 0)) {
            app->delete_edit_modal = true;
        }

        igSeparator();

        if (igMenuItem_Bool("Quick Export", "Ctrl+E",
                            false, app->photo != NULL)) {
            trigger_quick_export(app);
        }
        if (igMenuItem_Bool("Export...", NULL,
                            app->mode == AP_MODE_EXPORT,
                            app->photo != NULL)) {
            ap_app_enter_export(app);
        }
        igEndMenu();
    }

    if (igBeginMenu("View", true)) {
        bool show = app->show_panels;
        const char *panels_sc =
            (app->mode == AP_MODE_PHOTO || app->mode == AP_MODE_EXPORT)
                ? "Space" : NULL;
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
            if (igMenuItem_Bool("View Raw", NULL, view_raw, true)) {
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

    // Panels menu: visibility toggles for every registered panel that
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
    if (any_optional && igBeginMenu("Panels", true)) {
        for (const ap_panel *const *p = ap_panel_registry; *p; p++) {
            const ap_panel *panel = *p;
            if (!panel->menu_label || !panel->visible) continue;
            if (panel->mode != AP_MODE_ANY && panel->mode != app->mode) continue;
            igMenuItem_BoolPtr(panel->menu_label, NULL, panel->visible, true);
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

    if (app->export_inflight > 0) {
        // Right-align the in-flight indicator.  Build the label first so
        // we know its width, then back-track the cursor to place it flush
        // with the right edge of the menubar.
        char exporting[40];
        if (app->export_inflight == 1) {
            snprintf(exporting, sizeof(exporting), "Exporting...");
        } else {
            snprintf(exporting, sizeof(exporting),
                     "Exporting (%d)...", app->export_inflight);
        }
        ImVec2_c label_sz = igCalcTextSize(exporting, NULL, false, -1.0f);
        ImGuiStyle *style = igGetStyle();
        float right_x = igGetWindowWidth()
                        - label_sz.x
                        - style->FramePadding.x * 2.0f
                        - style->ItemSpacing.x;
        igSetCursorPosX(right_x);
        ImVec4_c dim = { 0.75f, 0.75f, 0.75f, 1.0f };
        igTextColored(dim, "%s", exporting);
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

static void draw_delete_modal(ap_app *app)
{
    if (app->delete_modal) {
        igOpenPopup_Str("Delete Photos", 0);
        app->delete_modal = false;
    }
    if (!igBeginPopupModal("Delete Photos", NULL,
                           ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    int n = (app->library && app->grid)
                ? ap_grid_selection_count(app->grid) : 0;
    if (n <= 0) {
        igCloseCurrentPopup();
        igEndPopup();
        return;
    }

    const char *s = (n == 1) ? "" : "s";
    igText("Delete %d photo%s from disk?", n, s);
    igTextDisabled("The raw file%s and sidecar%s will be removed. "
                   "This cannot be undone.", s, s);
    igSeparator();

    bool confirm = igButton("Delete", (ImVec2_c){ 120.0f, 0.0f });
    igSameLine(0.0f, -1.0f);
    bool cancel = igButton("Cancel", (ImVec2_c){ 120.0f, 0.0f });

    if (confirm) {
        delete_grid_selection(app);
        igCloseCurrentPopup();
    } else if (cancel) {
        igCloseCurrentPopup();
    }
    igEndPopup();
}

static void draw_delete_edit_modal(ap_app *app)
{
    if (app->delete_edit_modal) {
        igOpenPopup_Str("Delete Photo", 0);
        app->delete_edit_modal = false;
    }
    if (!igBeginPopupModal("Delete Photo", NULL,
                           ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    if (!app->photo || app->photo_library_idx < 0) {
        igCloseCurrentPopup();
        igEndPopup();
        return;
    }

    const char *rel = ap_library_photo_relative_path(app->library,
                                                     app->photo_library_idx);
    igText("Delete this photo from disk?");
    if (rel) igTextDisabled("%s", rel);
    igTextDisabled("The raw file and sidecar will be removed. "
                   "This cannot be undone.");
    igSeparator();

    bool confirm = igButton("Delete", (ImVec2_c){ 120.0f, 0.0f });
    igSameLine(0.0f, -1.0f);
    bool cancel = igButton("Cancel", (ImVec2_c){ 120.0f, 0.0f });

    if (confirm) {
        delete_edit_photo(app);
        igCloseCurrentPopup();
    } else if (cancel) {
        igCloseCurrentPopup();
    }
    igEndPopup();
}

static void drive_global_hotkeys(ap_app *app)
{
    ImGuiIO *io = igGetIO_Nil();
    if (!io) return;

    // Bare Space in photo / export mode toggles panel visibility. The
    // canvas owns the keyboard there and Space is otherwise idle.
    // Library mode has no shortcut - bare Space there is "open
    // selected photo" and the View menu still has a clickable toggle.
    // The WantTextInput guard keeps text fields (the rename box, etc.)
    // typeable.
    if ((app->mode == AP_MODE_PHOTO || app->mode == AP_MODE_EXPORT)
        && !io->WantTextInput
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
    if (io->KeyCtrl && !io->WantTextInput
        && igIsKeyPressed_Bool(ImGuiKey_C, false) && app->photo) {
        ap_app_copy_edits(app);
    }
    if (io->KeyCtrl && !io->WantTextInput
        && igIsKeyPressed_Bool(ImGuiKey_V, false) && app->photo
        && app->edit_clipboard_valid) {
        ap_app_paste_edits(app);
    }
    if (io->KeyCtrl && !io->WantTextInput
        && igIsKeyPressed_Bool(ImGuiKey_Z, false) && app->photo) {
        if (io->KeyShift) {
            ap_app_redo(app);
        } else {
            ap_app_undo(app);
        }
    }
    if (io->KeyCtrl && !io->WantTextInput
        && igIsKeyPressed_Bool(ImGuiKey_Y, false) && app->photo) {
        ap_app_redo(app);
    }
    if (!io->WantTextInput) {
        // ` (grave accent) — two contexts:
        //   Photo mode : hold to show the unedited original (before/after
        //                compare). Skips all user-edit stages cheaply via
        //                set_stage_skip; no graph rebuild. Released when
        //                the key comes up.
        //   Library mode: toggle between rendered and camera-preview
        //                 thumbnails (unchanged behaviour).
        if (app->photo) {
            bool held = igIsKeyDown_Nil(ImGuiKey_GraveAccent);
            if (held != app->compare_original) {
                ap_app_set_compare_original(app, held);
            }
        } else if (app->mode == AP_MODE_LIBRARY && app->library) {
            if (igIsKeyPressed_Bool(ImGuiKey_GraveAccent, false)) {
                toggle_rendered_thumbnails(app);
            }
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
            int win_w = (int)io->DisplaySize.x;
            int win_h = (int)io->DisplaySize.y;
            ap_grid_zoom_at(app->grid,
                            ap_grid_cell_size(app->grid) + 16,
                            win_w * 0.5f, win_h * 0.5f, win_w, win_h);
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
            int win_w = (int)io->DisplaySize.x;
            int win_h = (int)io->DisplaySize.y;
            ap_grid_zoom_at(app->grid,
                            ap_grid_cell_size(app->grid) - 16,
                            win_w * 0.5f, win_h * 0.5f, win_w, win_h);
        }
    }
}

// ----------------------------------------------------------------------

// Zoom-level readout drawn over the canvas corner: "Fit" at the
// default view, "100%" when the effective scale is 1:1, else "N%".
// The label uses the foreground draw list so it paints over the canvas
// but under any ImGui windows.
static void draw_canvas_zoom_overlay(ap_app *app)
{
    if (!app->photo || !app->canvas) return;
    ImGuiIO *io = igGetIO_Nil();
    if (!io) return;

    int win_w = (int)io->DisplaySize.x;
    int win_h = (int)io->DisplaySize.y;
    if (win_w <= 0 || win_h <= 0) return;

    float scale = ap_canvas_effective_scale(app->canvas, win_w, win_h);
    if (scale <= 0.0f) return;

    char label[32];
    float zoom = ap_canvas_zoom(app->canvas);
    if (zoom >= AP_CANVAS_DEFAULT_ZOOM - 0.001f &&
        zoom <= AP_CANVAS_DEFAULT_ZOOM + 0.001f) {
        snprintf(label, sizeof(label), "Fit");
    } else if (scale >= 0.995f && scale <= 1.005f) {
        snprintf(label, sizeof(label), "100%%");
    } else {
        snprintf(label, sizeof(label), "%d%%", (int)(scale * 100.0f + 0.5f));
    }

    ImDrawList *dl = igGetForegroundDrawList_ViewportPtr(NULL);
    if (!dl) return;

    ImVec2_c text_sz = igCalcTextSize(label, NULL, false, -1.0f);
    const float pad   = 8.0f;
    // Bottom-right corner of the canvas, inset by pad.
    float x = (float)win_w - text_sz.x - pad;
    float y = (float)win_h - text_sz.y - pad;
    ImVec2_c bg_tl = { x - pad * 0.5f, y - pad * 0.5f };
    ImVec2_c bg_br = { x + text_sz.x + pad * 0.5f,
                       y + text_sz.y + pad * 0.5f };
    ImDrawList_AddRectFilled(dl, bg_tl, bg_br, 0xB8000000, 4.0f, 0);
    ImDrawList_AddText_Vec2(dl, (ImVec2_c){ x, y }, 0xFFEEEEEE, label, NULL);
}

// "ORIGINAL" badge drawn over the top-left corner of the canvas while
// the before/after compare bypass is active. Gives the user clear visual
// feedback that edits are bypassed.
static void draw_compare_overlay(ap_app *app)
{
    if (!app->compare_original || !app->photo) return;
    ImGuiIO *io = igGetIO_Nil();
    if (!io) return;
    ImDrawList *dl = igGetForegroundDrawList_ViewportPtr(NULL);
    if (!dl) return;

    const char *label = "ORIGINAL";
    ImVec2_c text_sz = igCalcTextSize(label, NULL, false, -1.0f);
    const float pad = 8.0f;
    float x = pad;
    float y = pad + igGetFrameHeight();  // below the menubar
    ImVec2_c bg_tl = { x - pad * 0.5f, y - pad * 0.5f };
    ImVec2_c bg_br = { x + text_sz.x + pad * 0.5f,
                       y + text_sz.y + pad * 0.5f };
    ImDrawList_AddRectFilled(dl, bg_tl, bg_br, 0xCC000000, 4.0f, 0);
    ImDrawList_AddText_Vec2(dl, (ImVec2_c){ x, y }, 0xFFEEEEEE, label, NULL);
}

// The crop overlay: dimmed margin outside the crop rect, a rule-of-
// thirds grid inside it, the rect outline, and eight grab handles.
// Drawn on the foreground draw list so it sits over the canvas but
// under ImGui windows. Only runs while the crop tool is armed; the
// canvas is showing the full frame (see the viewport push).
static void draw_crop_overlay(ap_app *app)
{
    if (app->canvas_tool != AP_CANVAS_TOOL_CROP) return;
    ap_edit_entry *e = canvas_tool_entry(app, "transform");
    if (!e) return;

    ImGuiIO *io = igGetIO_Nil();
    if (!io) return;
    int win_w = (int)io->DisplaySize.x;
    int win_h = (int)io->DisplaySize.y;

    ImDrawList *dl = igGetForegroundDrawList_ViewportPtr(NULL);
    if (!dl) return;

    ap_viewport vp = ap_transform_viewport(e->params);

    // The four crop-rect corners in screen pixels.
    float x0s, y0s, x1s, y1s;
    ap_canvas_framed_uv_to_screen(app->canvas, vp.crop_x0, vp.crop_y0,
                                  win_w, win_h, &x0s, &y0s);
    ap_canvas_framed_uv_to_screen(app->canvas, vp.crop_x1, vp.crop_y1,
                                  win_w, win_h, &x1s, &y1s);

    // The full framed image rect in screen pixels — the dim mask
    // covers the framed image outside the crop, not the whole window.
    float fx0, fy0, fx1, fy1;
    ap_canvas_framed_uv_to_screen(app->canvas, 0.0f, 0.0f,
                                  win_w, win_h, &fx0, &fy0);
    ap_canvas_framed_uv_to_screen(app->canvas, 1.0f, 1.0f,
                                  win_w, win_h, &fx1, &fy1);

    // Dim the four margin bands between the framed image and the crop.
    const unsigned dim = 0x99000000u;
    ImDrawList_AddRectFilled(dl, (ImVec2_c){ fx0, fy0 },
                             (ImVec2_c){ fx1, y0s }, dim, 0.0f, 0);
    ImDrawList_AddRectFilled(dl, (ImVec2_c){ fx0, y1s },
                             (ImVec2_c){ fx1, fy1 }, dim, 0.0f, 0);
    ImDrawList_AddRectFilled(dl, (ImVec2_c){ fx0, y0s },
                             (ImVec2_c){ x0s, y1s }, dim, 0.0f, 0);
    ImDrawList_AddRectFilled(dl, (ImVec2_c){ x1s, y0s },
                             (ImVec2_c){ fx1, y1s }, dim, 0.0f, 0);

    // Rule-of-thirds grid inside the crop rect.
    const unsigned grid_col = 0x66FFFFFFu;
    for (int i = 1; i <= 2; i++) {
        float t = (float)i / 3.0f;
        float gx = x0s + (x1s - x0s) * t;
        float gy = y0s + (y1s - y0s) * t;
        ImDrawList_AddLine(dl, (ImVec2_c){ gx, y0s },
                           (ImVec2_c){ gx, y1s }, grid_col, 1.0f);
        ImDrawList_AddLine(dl, (ImVec2_c){ x0s, gy },
                           (ImVec2_c){ x1s, gy }, grid_col, 1.0f);
    }

    // Crop rect outline.
    ImDrawList_AddRect(dl, (ImVec2_c){ x0s, y0s }, (ImVec2_c){ x1s, y1s },
                       0xFFEEEEEE, 0.0f, 2.0f, 0);

    // Handles — eight filled squares.
    const float hs = 5.0f;
    for (int h = CROP_HANDLE_TL; h <= CROP_HANDLE_B; h++) {
        float hu, hv, sx, sy;
        crop_handle_uv(&vp, h, &hu, &hv);
        ap_canvas_framed_uv_to_screen(app->canvas, hu, hv,
                                      win_w, win_h, &sx, &sy);
        unsigned col = (h == app->crop_drag_handle) ? 0xFF55D6F2u
                                                    : 0xFFEEEEEEu;
        ImDrawList_AddRectFilled(dl, (ImVec2_c){ sx - hs, sy - hs },
                                 (ImVec2_c){ sx + hs, sy + hs }, col,
                                 0.0f, 0);
        ImDrawList_AddRect(dl, (ImVec2_c){ sx - hs, sy - hs },
                           (ImVec2_c){ sx + hs, sy + hs }, 0xFF101010u,
                           0.0f, 1.5f, 0);
    }

    // Straighten guide: while a straighten drag is active draw the
    // line the user is dragging.
    if (app->crop_drag_handle == CROP_HANDLE_STRAIGHTEN) {
        float su0, sv0;
        ap_canvas_framed_uv_to_screen(app->canvas, app->crop_drag_u0,
                                      app->crop_drag_v0, win_w, win_h,
                                      &su0, &sv0);
        ImDrawList_AddLine(dl, (ImVec2_c){ su0, sv0 },
                           io->MousePos, 0xFF55D6F2u, 2.0f);
    }
}

// Aspect-ratio presets for the crop tool toolbar. "Free" disables the
// lock; the rest lock the crop to a fixed width:height.
typedef struct {
    const char *label;
    float       w, h;       // 0,0 for Free
} crop_aspect_preset;

static const crop_aspect_preset crop_aspect_presets[] = {
    { "Free",  0.0f, 0.0f },
    { "1:1",   1.0f, 1.0f },
    { "3:2",   3.0f, 2.0f },
    { "4:3",   4.0f, 3.0f },
    { "16:9", 16.0f, 9.0f },
    { "2:3",   2.0f, 3.0f },
    { "3:4",   3.0f, 4.0f },
    { "9:16",  9.0f, 16.0f },
};

// Floating toolbar shown while the crop tool is armed: aspect-ratio
// lock, reset, and a Done button. Aspect lock and straighten state
// live on the app; the toolbar is their UI surface.
static void draw_crop_toolbar(ap_app *app)
{
    if (app->canvas_tool != AP_CANVAS_TOOL_CROP) return;
    ap_edit_entry *e = canvas_tool_entry(app, "transform");
    if (!e) return;

    ImGuiIO *io = igGetIO_Nil();
    if (!io) return;

    igSetNextWindowBgAlpha(0.85f);
    igSetNextWindowPos((ImVec2_c){ io->DisplaySize.x * 0.5f, 36.0f },
                       ImGuiCond_Appearing, (ImVec2_c){ 0.5f, 0.0f });
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse
                           | ImGuiWindowFlags_AlwaysAutoResize
                           | ImGuiWindowFlags_NoDocking;
    if (!igBegin("Crop##overlay", NULL, flags)) {
        igEnd();
        return;
    }

    int n_presets = (int)(sizeof(crop_aspect_presets) /
                          sizeof(crop_aspect_presets[0]));

    igText("Aspect");
    igSameLine(0.0f, -1.0f);
    igSetNextItemWidth(110.0f);
    const char *cur = "Free";
    if (app->crop_aspect_locked) {
        for (int i = 1; i < n_presets; i++) {
            float r = crop_aspect_presets[i].w / crop_aspect_presets[i].h;
            if (fabsf(r - app->crop_aspect_ratio) < 0.001f) {
                cur = crop_aspect_presets[i].label;
                break;
            }
        }
    }
    if (igBeginCombo("##aspect", cur, 0)) {
        for (int i = 0; i < n_presets; i++) {
            const crop_aspect_preset *p = &crop_aspect_presets[i];
            bool selected = (i == 0) ? !app->crop_aspect_locked
                : (app->crop_aspect_locked &&
                   strcmp(cur, p->label) == 0);
            if (igSelectable_Bool(p->label, selected, 0,
                                  (ImVec2_c){ 0.0f, 0.0f })) {
                if (i == 0) {
                    app->crop_aspect_locked = false;
                } else {
                    app->crop_aspect_locked = true;
                    app->crop_aspect_ratio  = p->w / p->h;
                    // Re-shape the current crop to the new ratio about
                    // its centre so the lock takes effect immediately.
                    ap_viewport vp = ap_transform_viewport(e->params);
                    int iw = ap_photo_width(app->photo);
                    int ih = ap_photo_height(app->photo);
                    if (iw > 0 && ih > 0) {
                        float ratio_norm = app->crop_aspect_ratio
                                         * (float)ih / (float)iw;
                        float cx = 0.5f * (vp.crop_x0 + vp.crop_x1);
                        float cy = 0.5f * (vp.crop_y0 + vp.crop_y1);
                        float cw = vp.crop_x1 - vp.crop_x0;
                        float ch = cw / ratio_norm;
                        if (ch > 1.0f) { ch = 1.0f; cw = ch * ratio_norm; }
                        ap_app_edit_snapshot(app);
                        vp.crop_x0 = cx - cw * 0.5f;
                        vp.crop_x1 = cx + cw * 0.5f;
                        vp.crop_y0 = cy - ch * 0.5f;
                        vp.crop_y1 = cy + ch * 0.5f;
                        crop_rect_sanitize(&vp);
                        ap_transform_set_viewport(e->params, &vp);
                    }
                }
            }
        }
        igEndCombo();
    }

    igSameLine(0.0f, 16.0f);
    if (igButton("Reset crop", (ImVec2_c){ 0.0f, 0.0f })) {
        ap_viewport vp = ap_transform_viewport(e->params);
        ap_app_edit_snapshot(app);
        vp.crop_x0 = 0.0f; vp.crop_y0 = 0.0f;
        vp.crop_x1 = 1.0f; vp.crop_y1 = 1.0f;
        vp.rotation_deg = 0.0f;
        ap_transform_set_viewport(e->params, &vp);
    }

    igSameLine(0.0f, 8.0f);
    if (igButton("Done", (ImVec2_c){ 0.0f, 0.0f })) {
        ap_app_set_canvas_tool(app, AP_CANVAS_TOOL_NONE, -1);
    }

    igSeparator();
    igTextDisabled("drag handles to crop  ·  shift-drag inside to straighten");

    igEnd();
}

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
    draw_delete_modal(app);
    draw_delete_edit_modal(app);
    drive_global_hotkeys(app);

    // Full-viewport invisible host window owns the dockspace that
    // every panel docks into. PassthruCentralNode keeps the middle
    // area transparent so the canvas / grid render path stays
    // visible underneath. Default layout (Image left, Edits + Tools
    // right) is built once on first launch; ImGui's .ini handles
    // every subsequent run.
    ImGuiDockNode *dock_central = NULL;
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
            igDockBuilderDockWindow("Metadata##library",      right_bot);
            igDockBuilderDockWindow("Pipelines##library",     right_bot);
            igDockBuilderDockWindow("Groups##library",        right_bot);
            igDockBuilderDockWindow("Sort & Search##library", right_bot);
            // Export-mode panels: Format / Quality / Naming up the
            // right column, Destination in the bottom-right slot with
            // the action button. Mode-gated like the library panels.
            igDockBuilderDockWindow("Format##export",      right_top);
            igDockBuilderDockWindow("Quality##export",     right_mid);
            igDockBuilderDockWindow("Naming##export",      right_mid);
            igDockBuilderDockWindow("Destination##export", right_bot);
            igDockBuilderFinish(dockspace_id);
        }
        igDockSpace(dockspace_id,
                    (ImVec2_c){ 0.0f, 0.0f },
                    ImGuiDockNodeFlags_PassthruCentralNode, NULL);
        // Capture the central node here, inside the dock-host window,
        // where dockspace_id was hashed in the right ImGui ID scope.
        dock_central = igDockBuilderGetCentralNode(dockspace_id);
        igEnd();
    }

    // Confine the grid and the canvas to the dockspace central node so
    // docked panels don't paint over them — and so the canvas fits the
    // visible area, not the whole window. dock_central was captured
    // inside the dock-host window (correct ID scope) and stays NULL
    // when show_panels is off — both cases fall through to full-window.
    {
        bool have_central = dock_central &&
            dock_central->Size.x > 0.0f && dock_central->Size.y > 0.0f;
        int cx = have_central ? (int)dock_central->Pos.x  : 0;
        int cy = have_central ? (int)dock_central->Pos.y  : 0;
        int cw = have_central ? (int)dock_central->Size.x : 0;
        int ch = have_central ? (int)dock_central->Size.y : 0;
        if (app->grid)   ap_grid_set_render_rect(app->grid, cx, cy, cw, ch);
        if (app->canvas) ap_canvas_set_render_rect(app->canvas, cx, cy, cw, ch);
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

    if (app->grid) {
        ImGuiIO *io = igGetIO_Nil();
        ap_grid_update(app->grid, io ? io->DeltaTime : 0.0f);
    }

    // Push the active viewport to the canvas before the mode-input
    // handlers run — the pipeline renders full-frame, the canvas
    // applies the Transform module's crop / rotation / flip / scale at
    // presentation. The panels above may have changed it this frame,
    // and the crop tool's coordinate mapping (drive_crop_tool) reads
    // the canvas viewport, so it must be current here, not pushed at
    // end of frame.
    //
    // While the interactive crop tool is armed the canvas shows the
    // *full* rotated frame (crop reset to the whole frame) so the user
    // can drag the crop rect over the entire image; the crop overlay
    // draws the rect on top. Rotation / flip / scale stay applied so
    // straighten previews live.
    if (app->canvas) {
        if (app->photo) {
            ap_viewport vp = ap_photo_viewport(app->photo);
            if (app->canvas_tool == AP_CANVAS_TOOL_CROP) {
                vp.crop_x0 = 0.0f; vp.crop_y0 = 0.0f;
                vp.crop_x1 = 1.0f; vp.crop_y1 = 1.0f;
            }
            ap_canvas_set_viewport(app->canvas, &vp);
        } else {
            ap_canvas_set_viewport(app->canvas, NULL);
        }
    }

    if (app->mode == AP_MODE_PHOTO && !app->photo_loading) {
        drive_canvas_input(app);
        draw_crop_overlay(app);
        draw_crop_toolbar(app);
        draw_canvas_zoom_overlay(app);
        draw_compare_overlay(app);
    } else if (app->mode == AP_MODE_EXPORT && !app->photo_loading) {
        drive_export_input(app);
        draw_canvas_zoom_overlay(app);
    } else if (app->mode == AP_MODE_LIBRARY && !app->photo_loading) {
        drive_grid_input(app);
        draw_grid_context_menu(app);
        draw_grid_labels(app);
        draw_selection_overlay(app);
        draw_marquee_overlay(app);
        submit_pending_thumbs(app);
    }
    drain_one_completed_job(app);
    draw_loading_overlay(app);
    ap_toast_draw();

    const ap_edit_stack *stack = NULL;
    if (app->photo) {
        stack = ap_photo_stack(app->photo);
    }
    return ap_gpu_render_frame(app->gpu, stack);
}
