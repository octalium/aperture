#define _GNU_SOURCE

#include "app.h"

#include "core/log.h"
#include "gpu/canvas.h"
#include "gpu/gpu.h"
#include "gpu/pipeline_graph.h"
#include "library/library.h"
#include "panels/panels.h"
#include "photo/photo.h"
#include "ui/imgui.h"

#include "cimgui.h"

#include <signal.h>
#include <stdlib.h>

struct ap_app {
    ap_gpu     *gpu;
    ap_canvas  *canvas;
    ap_mode     mode;
    ap_photo   *photo;
    ap_library *library;
};

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
    ap_gpu_set_canvas(app->gpu, app->canvas);

    install_signal_handlers();
    return app;
}

void ap_app_destroy(ap_app *app)
{
    if (!app) return;

    ap_app_wait_idle(app);
    ap_app_close_photo(app);
    ap_app_close_library(app);
    if (app->canvas) {
        ap_gpu_set_canvas(app->gpu, NULL);
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

void ap_app_set_mode(ap_app *app, ap_mode mode)
{
    if (app) {
        app->mode = mode;
    }
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
    app->mode = AP_MODE_LIBRARY;
    return 0;
}

void ap_app_close_library(ap_app *app)
{
    if (!app || !app->library) return;
    ap_library_close(app->library);
    app->library = NULL;
}

ap_library *ap_app_library(ap_app *app)
{
    return app ? app->library : NULL;
}

static void drive_canvas_input(ap_app *app)
{
    if (!app->canvas || !app->photo) return;

    ImGuiIO *io = igGetIO_Nil();
    if (!io || io->WantCaptureMouse) return;

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

    drive_canvas_input(app);

    const ap_edit_state *edit = NULL;
    if (app->photo) {
        edit = ap_photo_edit(app->photo);
    }
    return ap_gpu_render_frame(app->gpu, edit);
}
