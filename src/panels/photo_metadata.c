#include "panels.h"

#include "photo/metadata.h"
#include "photo/photo.h"

#include "cimgui.h"

#include <stdio.h>

// Metadata window: per-photo EXIF + camera/lens/GPS pane, docked to
// the left. Each field shows the user's override when set, otherwise
// the value the loader extracted from the raw file. Edits go to the
// per-photo sidecar's [metadata] table; the file is never touched.
// Reset (only present when the field has an override) clears the
// override and reverts to the file value.

// Per-field edit buffers. ImGui's InputText mutates its buffer
// in-place each frame; refilling from the model every frame would
// overwrite mid-typing edits. So sync model -> buffer only on photo
// change (tracked by pointer; the photo's address is stable for its
// lifetime) and let InputText own the buffer between syncs.
static const ap_photo *g_last_photo = NULL;
static char g_buffers[AP_META_FIELD_COUNT][AP_META_VALUE_LEN];

static void sync_buffers_from_photo(const ap_photo *photo)
{
    for (int i = 0; i < AP_META_FIELD_COUNT; i++) {
        snprintf(g_buffers[i], AP_META_VALUE_LEN, "%s",
                 ap_photo_metadata_value(photo, (ap_meta_field)i));
    }
}

static void photo_metadata_draw(ap_app *app)
{
    ap_photo *photo = ap_app_photo(app);
    if (!photo) {
        g_last_photo = NULL;
        return;
    }

    if (photo != g_last_photo) {
        sync_buffers_from_photo(photo);
        g_last_photo = photo;
    }

    if (!igBegin("Metadata", NULL, 0)) {
        igEnd();
        return;
    }

    igTextDisabled("edits persist in this photo's sidecar; the raw file is never touched");
    igSeparator();

    for (int i = 0; i < AP_META_FIELD_COUNT; i++) {
        ap_meta_field f = (ap_meta_field)i;
        igPushID_Int(i);

        igText("%s", ap_meta_field_label(f));

        bool is_user = ap_photo_metadata_is_user(photo, f);

        // Reserve space on the right for the Reset button when one
        // will be drawn. Layout stays consistent whether or not the
        // field has an override.
        float avail = igGetContentRegionAvail().x;
        float reset_w = 60.0f;
        float input_w = is_user ? (avail - reset_w - 8.0f) : avail;
        if (input_w < 80.0f) input_w = 80.0f;
        igSetNextItemWidth(input_w);

        if (igInputText("##value", g_buffers[i], AP_META_VALUE_LEN,
                        0, NULL, NULL)) {
            ap_photo_metadata_set_user(photo, f, g_buffers[i]);
        }

        if (is_user) {
            igSameLine(0.0f, -1.0f);
            if (igButton("Reset", (ImVec2_c){ reset_w, 0.0f })) {
                ap_photo_metadata_reset(photo, f);
                // Refill the buffer with the now-effective (file)
                // value so the InputText snaps back.
                snprintf(g_buffers[i], AP_META_VALUE_LEN, "%s",
                         ap_photo_metadata_value(photo, f));
            }
        }

        igPopID();
    }

    igEnd();
}

const ap_panel panel_photo_metadata = {
    .name = "photo_metadata",
    .mode = AP_MODE_PHOTO,
    .draw = photo_metadata_draw,
};
