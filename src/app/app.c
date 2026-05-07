#include "app.h"

#include "core/log.h"
#include "gpu/gpu.h"
#include "library/library.h"
#include "panels/panels.h"
#include "photo/photo.h"
#include "ui/imgui.h"

#include <stdlib.h>

struct ap_app {
    ap_gpu     *gpu;
    ap_mode     mode;
    ap_photo   *photo;
    ap_library *library;
};

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
    return app;
}

void ap_app_destroy(ap_app *app)
{
    if (!app) return;

    ap_app_wait_idle(app);
    ap_app_close_photo(app);
    ap_app_close_library(app);
    if (app->gpu) {
        ap_gpu_destroy(app->gpu);
        app->gpu = NULL;
    }
    free(app);
}

bool ap_app_should_run(ap_app *app)
{
    return app && ap_gpu_should_run(app->gpu);
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
    ap_gpu_set_graph(app->gpu, ap_photo_graph(app->photo));
    app->mode = AP_MODE_PHOTO;
    return 0;
}

void ap_app_close_photo(ap_app *app)
{
    if (!app || !app->photo) return;

    ap_app_wait_idle(app);
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

    const ap_edit_state *edit = NULL;
    if (app->photo) {
        edit = ap_photo_edit(app->photo);
    }
    return ap_gpu_render_frame(app->gpu, edit);
}
