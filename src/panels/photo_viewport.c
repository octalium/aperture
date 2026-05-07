#include "panels.h"

#include "photo/photo.h"

#include "cimgui.h"

static void photo_viewport_draw(ap_app *app)
{
    ap_photo *photo = ap_app_photo(app);
    if (!photo) {
        return;
    }

    int img_w = ap_photo_width(photo);
    int img_h = ap_photo_height(photo);

    ImVec2_c default_size = { 800.0f, 600.0f };
    igSetNextWindowSize(default_size, ImGuiCond_FirstUseEver);

    if (igBegin("image", NULL, 0)) {
        ImVec2_c avail = igGetContentRegionAvail();
        if (avail.x > 0.0f && avail.y > 0.0f && img_w > 0 && img_h > 0) {
            float src_aspect = (float)img_w / (float)img_h;
            float dst_aspect = avail.x / avail.y;
            ImVec2_c size;
            if (src_aspect > dst_aspect) {
                size.x = avail.x;
                size.y = avail.x / src_aspect;
            } else {
                size.y = avail.y;
                size.x = avail.y * src_aspect;
            }
            ImVec2_c cursor = igGetCursorPos();
            cursor.x += (avail.x - size.x) * 0.5f;
            cursor.y += (avail.y - size.y) * 0.5f;
            igSetCursorPos(cursor);

            ImTextureRef_c tex_ref = {
                ._TexData = NULL,
                ._TexID   = ap_photo_tex_id(photo),
            };
            ImVec2_c uv0 = { 0.0f, 0.0f };
            ImVec2_c uv1 = { 1.0f, 1.0f };
            igImage(tex_ref, size, uv0, uv1);
        }
    }
    igEnd();
}

const ap_panel panel_photo_viewport = {
    .name = "photo_viewport",
    .mode = AP_MODE_PHOTO,
    .draw = photo_viewport_draw,
};
