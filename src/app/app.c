#define _GNU_SOURCE

#include "app.h"

#include "core/log.h"
#include "core/worker.h"
#include "gpu/canvas.h"
#include "gpu/gpu.h"
#include "gpu/grid.h"
#include "gpu/pipeline_graph.h"
#include "library/library.h"
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

struct ap_app {
    ap_gpu          *gpu;
    ap_canvas       *canvas;
    ap_grid         *grid;
    ap_mode          mode;
    ap_photo        *photo;
    ap_library      *library;
    ap_worker_pool  *workers;
    int              thumb_inflight;
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

// Drain completed thumbnail jobs without GPU-uploading them. Used
// when the library these jobs belong to is going away. Caller is
// expected to have first waited for in-flight jobs to drop into the
// completed queue (ap_worker_pool_wait_idle).
static void discard_completed_thumb_jobs(ap_app *app)
{
    if (!app->workers) return;
    for (;;) {
        ap_work_item *it = ap_worker_pool_poll(app->workers);
        if (!it) break;
        thumb_job *j = (thumb_job *)it;
        free(j->rgba);
        free(j);
        if (app->thumb_inflight > 0) app->thumb_inflight--;
    }
}

void ap_app_destroy(ap_app *app)
{
    if (!app) return;

    ap_app_wait_idle(app);
    ap_app_close_photo(app);
    ap_app_close_library(app);
    ap_gpu_set_canvas(app->gpu, NULL);
    ap_gpu_set_grid(app->gpu, NULL);
    if (app->workers) {
        ap_worker_pool_destroy(app->workers);
        app->workers = NULL;
        // Pool destroy waits for in-flight workers; nothing left to drain.
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

int ap_app_open_photo(ap_app *app, const char *path)
{
    if (!app || !path) {
        return -1;
    }

    ap_app_close_photo(app);

    app->photo = ap_photo_open(app->gpu, path);
    if (!app->photo) {
        return -1;
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
    return 0;
}

void ap_app_close_photo(ap_app *app)
{
    if (!app || !app->photo) return;

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
    return 0;
}

void ap_app_close_library(ap_app *app)
{
    if (!app || !app->library) return;

    // Wait for outstanding decode work to land in the completed queue
    // and toss the buffers — they belong to the library that's going
    // away. Then drop GPU references before the textures vanish.
    ap_worker_pool_wait_idle(app->workers);
    discard_completed_thumb_jobs(app);
    ap_app_wait_idle(app);
    ap_grid_set_photo_count(app->grid, 0);
    ap_library_close(app->library);
    app->library = NULL;
    bind_mode_view(app);
}

ap_library *ap_app_library(ap_app *app)
{
    return app ? app->library : NULL;
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
            const float wheel_step_px = 60.0f;
            ap_grid_scroll(app->grid, -io->MouseWheel * wheel_step_px,
                           win_w, win_h);
        }
        if (igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) {
            int hit = ap_grid_hit_test(app->grid,
                                       io->MousePos.x, io->MousePos.y,
                                       win_w, win_h);
            if (hit >= 0) {
                ap_grid_set_selected(app->grid, hit);
                open_selected_photo(app);
                return;
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
        ap_grid_set_selected(app->grid, new_sel);
        ap_grid_ensure_visible(app->grid, ap_grid_selected(app->grid),
                               win_w, win_h);
    }

    if (igIsKeyPressed_Bool(ImGuiKey_Enter, false) ||
        igIsKeyPressed_Bool(ImGuiKey_Space, false)) {
        open_selected_photo(app);
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

    for (int i = 0; i < n; i++) {
        const char *rel = ap_library_photo_relative_path(app->library, i);
        if (!rel) continue;
        float cx, cy, cw, ch;
        if (ap_grid_cell_rect(app->grid, i, win_w, win_h, &cx, &cy, &cw, &ch) != 0) {
            continue;
        }
        ImVec2_c pos = { cx + 6.0f, cy + ch - 18.0f };
        ImDrawList_AddText_Vec2(dl, pos, 0xFFEEEEEE, rel, NULL);
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

// Drain one completed decode job per frame: GPU-upload + bind to the
// grid. One per frame keeps GPU upload latency bounded; the worker
// keeps producing while we're idle here.
static void drain_one_completed_thumb(ap_app *app)
{
    if (!app->workers || !app->library || !app->grid) return;
    ap_work_item *it = ap_worker_pool_poll(app->workers);
    if (!it) return;
    thumb_job *j = (thumb_job *)it;
    if (app->thumb_inflight > 0) app->thumb_inflight--;

    if (j->ok && j->rgba && j->idx >= 0
        && j->idx < ap_library_photo_count(app->library))
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

int ap_app_run_frame(ap_app *app)
{
    if (!app) return -1;

    ap_imgui_new_frame();

    for (const ap_panel *const *p = ap_panel_registry; *p; p++) {
        const ap_panel *panel = *p;
        if (panel->mode == AP_MODE_ANY || panel->mode == app->mode) {
            if (panel->draw) {
                panel->draw(app);
            }
        }
    }

    if (app->mode == AP_MODE_PHOTO) {
        drive_canvas_input(app);
    } else if (app->mode == AP_MODE_LIBRARY) {
        drive_grid_input(app);
        draw_grid_labels(app);
        submit_pending_thumbs(app);
        drain_one_completed_thumb(app);
    }

    const ap_edit_state *edit = NULL;
    if (app->photo) {
        edit = ap_photo_edit(app->photo);
    }
    return ap_gpu_render_frame(app->gpu, edit);
}
