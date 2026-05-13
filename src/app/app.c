#define _GNU_SOURCE

#include "app.h"

#include "core/log.h"
#include "core/worker.h"
#include "gpu/canvas.h"
#include "gpu/gpu.h"
#include "gpu/grid.h"
#include "gpu/pipeline_graph.h"
#include "library/library.h"
#include "output/jpeg.h"
#include "panels/panels.h"
#include "photo/photo.h"
#include "photo/thumbnail.h"
#include "ui/imgui.h"

#include "cimgui.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>

// Decode-on-worker job for one thumbnail. Submitter (main thread)
// allocates and fills `path` + `idx`; worker fills `rgba` / `w` / `h`
// / `ok`; main thread polls completed jobs, uploads to GPU, frees.
typedef struct {
    ap_work_item base;
    char         path[4096];
    int          idx;
    uint8_t     *rgba;
    int          w, h;
    int          ok;
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

// Forward decls of run-fn pointers (used to dispatch on completion).
static void thumb_job_run(ap_work_item *self);
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
    ap_library      *library;
    ap_worker_pool  *workers;
    int              thumb_inflight;
    bool             photo_loading;
    char             loading_path[4096];
    uint64_t         photo_load_gen;
    int              export_inflight;

    // Workspace chrome
    bool             show_panels;          // Tab to toggle
    bool             open_library_modal;   // File -> Open Library
    char             open_library_input[4096];
    bool             rename_library_modal; // Library indicator -> Rename
    char             rename_library_input[128];
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

    install_signal_handlers();
    return app;
}

// Free a completed work item without acting on its result. Used at
// teardown and when the result's target (library, photo) is gone.
static void discard_completed_item(ap_app *app, ap_work_item *it)
{
    if (it->run == thumb_job_run) {
        thumb_job *j = (thumb_job *)it;
        if (app->thumb_inflight > 0) app->thumb_inflight--;
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
    if (app->mode == AP_MODE_PHOTO && app->photo) {
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

static void install_loaded_photo(ap_app *app, photo_open_job *j)
{
    ap_app_close_photo(app);
    app->photo = ap_photo_open_with_raw(app->gpu, j->path, &j->raw);
    if (!app->photo) {
        AP_ERROR("photo: build from raw failed for %s", j->path);
        return;
    }
    ap_pipeline_graph *graph = ap_photo_graph(app->photo);
    ap_gpu_set_graph(app->gpu, graph);
    ap_canvas_set_input(app->canvas,
                        ap_pipeline_graph_output_view(graph),
                        ap_pipeline_graph_output_sampler(graph),
                        ap_pipeline_graph_output_width(graph),
                        ap_pipeline_graph_output_height(graph));
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

    if (!app->photo) {
        if (app->mode == AP_MODE_PHOTO) app->mode = AP_MODE_LIBRARY;
        bind_mode_view(app);
        return;
    }

    ap_app_wait_idle(app);
    ap_canvas_set_input(app->canvas, VK_NULL_HANDLE, VK_NULL_HANDLE, 0, 0);
    ap_gpu_set_graph(app->gpu, NULL);
    ap_photo_close(app->photo);
    app->photo = NULL;
    app->mode  = AP_MODE_LIBRARY;
    bind_mode_view(app);
}

ap_photo *ap_app_photo(ap_app *app)
{
    return app ? app->photo : NULL;
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

int ap_app_open_library(ap_app *app, const char *path)
{
    if (!app || !path) return -1;

    ap_app_close_photo(app);
    ap_app_close_library(app);

    app->library = ap_library_open(path);
    if (!app->library) {
        return -1;
    }
    ap_grid_set_photo_count(app->grid, ap_library_photo_count(app->library));
    ap_grid_set_selected(app->grid, 0);
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
    ap_library_close(app->library);
    app->library = NULL;
    bind_mode_view(app);
    refresh_window_title(app);
}

ap_library *ap_app_library(ap_app *app)
{
    return app ? app->library : NULL;
}

static void navigate_library_relative(ap_app *app, int dir)
{
    if (!app->library || !app->grid) return;
    int n = ap_library_photo_count(app->library);
    if (n <= 0) return;
    int sel = ap_grid_selected(app->grid);
    int new_sel = sel + dir;
    if (new_sel < 0 || new_sel >= n) return;
    ap_grid_set_selected(app->grid, new_sel);

    char abs[4096];
    if (ap_library_photo_absolute_path(app->library, new_sel,
                                       abs, sizeof(abs)) == 0) {
        ap_app_open_photo(app, abs);
    }
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

    if (!io->WantCaptureKeyboard) {
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
    if (io->MouseWheel != 0.0f) {
        float factor = io->MouseWheel > 0.0f
            ? 1.0f + 0.10f * io->MouseWheel
            : 1.0f / (1.0f - 0.10f * io->MouseWheel);
        ap_canvas_zoom_at(app->canvas, factor,
                          io->MousePos.x, io->MousePos.y,
                          win_w, win_h);
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
    int idx = ap_grid_selected(app->grid);
    if (idx < 0 || idx >= ap_library_photo_count(app->library)) return;

    char abs[4096];
    if (ap_library_photo_absolute_path(app->library, idx, abs, sizeof(abs)) != 0) {
        AP_ERROR("library: photo path overflow at idx %d", idx);
        return;
    }
    if (ap_app_open_photo(app, abs) != 0) {
        AP_ERROR("library: failed to open %s", abs);
    }
}

static void drive_grid_input(ap_app *app)
{
    if (!app->library || !app->grid) return;
    int n = ap_library_photo_count(app->library);
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

    if (igIsKeyPressed_Bool(ImGuiKey_Enter, false) ||
        igIsKeyPressed_Bool(ImGuiKey_Space, false)) {
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
    int n = ap_library_photo_count(app->library);
    if (n <= 0) return;

    ImGuiIO *io = igGetIO_Nil();
    if (!io) return;
    int win_w = (int)io->DisplaySize.x;
    int win_h = (int)io->DisplaySize.y;

    ImDrawList *dl = igGetForegroundDrawList_ViewportPtr(NULL);
    if (!dl) return;

    const float band_h = 18.0f;
    for (int i = 0; i < n; i++) {
        const char *rel = ap_library_photo_relative_path(app->library, i);
        if (!rel) continue;
        float cx, cy, cw, ch;
        if (ap_grid_cell_rect(app->grid, i, win_w, win_h, &cx, &cy, &cw, &ch) != 0) {
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

        // Don't paint the band when the cell is too small to read it.
        if (fit_h < band_h * 2.0f) continue;

        ImVec2_c band_tl = { fit_x,         fit_y + fit_h - band_h };
        ImVec2_c band_br = { fit_x + fit_w, fit_y + fit_h          };
        ImDrawList_AddRectFilled(dl, band_tl, band_br, 0xB8000000, 0.0f, 0);
        ImVec2_c text_pos = { fit_x + 4.0f, fit_y + fit_h - band_h + 2.0f };
        ImDrawList_AddText_Vec2(dl, text_pos, 0xFFEEEEEE, rel, NULL);
    }
}

static void thumb_job_run(ap_work_item *self)
{
    thumb_job *j = (thumb_job *)self;
    j->ok = (ap_thumbnail_decode_cpu(j->path, &j->rgba, &j->w, &j->h) == 0);
}

// Submit decode jobs while the pool has headroom and the library
// still has un-decoded photos. Each submission heap-allocates a
// thumb_job that the worker fills in.
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
        if (ap_library_photo_absolute_path(app->library, idx,
                                           j->path, sizeof(j->path)) != 0) {
            free(j);
            continue;
        }
        ap_worker_pool_submit(app->workers, &j->base);
        app->thumb_inflight++;
    }
}

static void handle_thumb_complete(ap_app *app, thumb_job *j)
{
    if (app->thumb_inflight > 0) app->thumb_inflight--;
    if (app->library && app->grid && j->ok && j->rgba
        && j->idx >= 0 && j->idx < ap_library_photo_count(app->library))
    {
        ap_thumbnail *t = ap_thumbnail_upload(app->gpu, j->rgba, j->w, j->h);
        if (t) {
            ap_library_set_thumbnail(app->library, j->idx, t);
            ap_grid_set_thumbnail(app->grid, j->idx,
                                  ap_thumbnail_view(t),
                                  ap_thumbnail_sampler(t));
        }
    }
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
    } else {
        AP_WARN("worker: unknown completed run-fn, leaking item");
    }
}

// ---- menubar + global hotkeys ----------------------------------------

static void trigger_quick_export(ap_app *app)
{
    if (!app->photo) return;
    const char *src = ap_photo_path(app->photo);
    char out[4096];
    int n = snprintf(out, sizeof(out), "%s.jpg", src);
    if (n > 0 && (size_t)n < sizeof(out)) {
        ap_app_request_jpeg_export(app, app->photo, out, 90);
    } else {
        AP_ERROR("export: path too long for %s", src);
    }
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
            const char *root = app->library ? ap_library_root(app->library) : "";
            snprintf(app->open_library_input,
                     sizeof(app->open_library_input), "%s", root ? root : "");
            app->open_library_modal = true;
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

    if (igBeginMenu("View", true)) {
        bool show = app->show_panels;
        if (igMenuItem_BoolPtr("Show Panels", "Tab", &show, true)) {
            app->show_panels = show;
        }
        if (igMenuItem_Bool("Fullscreen", "F11",
                            ap_gpu_is_fullscreen(app->gpu), true)) {
            ap_gpu_toggle_fullscreen(app->gpu);
        }
        igSeparator();
        if (igMenuItem_Bool("Reset Cell Zoom", "Ctrl+0", false, true)) {
            ap_grid_reset_cell_size(app->grid);
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

static void draw_open_library_modal(ap_app *app)
{
    if (app->open_library_modal) {
        igOpenPopup_Str("Open Library", 0);
        app->open_library_modal = false;
    }
    if (!igBeginPopupModal("Open Library", NULL, 0)) return;

    igText("Path to a directory holding raw photos:");
    igInputText("##path", app->open_library_input,
                sizeof(app->open_library_input), 0, NULL, NULL);

    bool submit = igButton("Open", (ImVec2_c){ 120.0f, 0.0f });
    igSameLine(0.0f, -1.0f);
    bool cancel = igButton("Cancel", (ImVec2_c){ 120.0f, 0.0f });

    if (submit && app->open_library_input[0]) {
        if (ap_app_open_library(app, app->open_library_input) == 0) {
            app->open_library_input[0] = '\0';
            igCloseCurrentPopup();
        }
    } else if (cancel) {
        igCloseCurrentPopup();
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

static void drive_global_hotkeys(ap_app *app)
{
    ImGuiIO *io = igGetIO_Nil();
    if (!io) return;

    if (igIsKeyPressed_Bool(ImGuiKey_Tab, false) && !io->WantCaptureKeyboard) {
        app->show_panels = !app->show_panels;
    }
    if (igIsKeyPressed_Bool(ImGuiKey_F11, false)) {
        ap_gpu_toggle_fullscreen(app->gpu);
    }
    if (io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_Q, false)) {
        app->quit_requested = true;
    }
    if (io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_E, false) && app->photo) {
        trigger_quick_export(app);
    }
    if (io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_0, false)) {
        ap_grid_reset_cell_size(app->grid);
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
    draw_open_library_modal(app);
    draw_rename_library_modal(app);
    drive_global_hotkeys(app);

    if (app->show_panels) {
        for (const ap_panel *const *p = ap_panel_registry; *p; p++) {
            const ap_panel *panel = *p;
            if (panel->mode == AP_MODE_ANY || panel->mode == app->mode) {
                if (panel->draw) {
                    panel->draw(app);
                }
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

    const ap_edit_state *edit = NULL;
    if (app->photo) {
        edit = ap_photo_edit(app->photo);
    }
    return ap_gpu_render_frame(app->gpu, edit);
}
