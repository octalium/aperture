#include "panels.h"

#include "photo/photo.h"

#include "cimgui.h"

static void photo_edit_draw(ap_app *app)
{
    ap_photo *photo = ap_app_photo(app);
    if (!photo) {
        return;
    }
    ap_edit_state *edit = ap_photo_edit(photo);

    if (igBegin("edit", NULL, 0)) {
        igSliderFloat("Exposure (EV)", &edit->exposure_ev,
                      -5.0f,  5.0f, "%.2f", 0);
        igSliderFloat("Contrast",      &edit->tone_contrast,
                       0.5f,  4.0f, "%.2f", 0);
        igSliderFloat("Pivot",         &edit->tone_pivot,
                       0.05f, 0.5f, "%.3f", 0);
    }
    igEnd();
}

const ap_panel panel_photo_edit = {
    .name = "photo_edit",
    .mode = AP_MODE_PHOTO,
    .draw = photo_edit_draw,
};
