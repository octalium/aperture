#include "panels.h"

#include "cimgui.h"

#ifndef APERTURE_VERSION
#error "APERTURE_VERSION must be defined at compile time (set via meson)"
#endif

static void photo_info_draw(ap_app *app)
{
    (void)app;
    ImGuiIO *io = igGetIO_Nil();
    if (igBegin("aperture", NULL, 0)) {
        igText("aperture %s", APERTURE_VERSION);
        igSeparator();
        igText("FPS: %.1f", io->Framerate);
        igText("Frame time: %.3f ms", 1000.0f / io->Framerate);
    }
    igEnd();
}

const ap_panel panel_photo_info = {
    .name = "photo_info",
    .mode = AP_MODE_ANY,
    .draw = photo_info_draw,
};
