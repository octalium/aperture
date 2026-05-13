#include "panels.h"

#include "photo/photo.h"

#include "cimgui.h"

#include <stdio.h>

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

        bool respect = edit->respect_orientation != 0;
        if (igCheckbox("Respect EXIF orientation", &respect)) {
            edit->respect_orientation = respect ? 1 : 0;
            // Pipeline graph dims and demosaic flip are baked at
            // photo-open time, so apply by reopening on the same path.
            // The current `photo` pointer becomes stale once we open —
            // bail out of the rest of the panel for this frame.
            char path_copy[4096];
            snprintf(path_copy, sizeof(path_copy), "%s", ap_photo_path(photo));
            ap_app_open_photo(app, path_copy);
            igEnd();
            return;
        }
    }
    igEnd();
}

const ap_panel panel_photo_edit = {
    .name = "photo_edit",
    .mode = AP_MODE_PHOTO,
    .draw = photo_edit_draw,
};
