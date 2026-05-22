#include "panels.h"

#include "app/app.h"
#include "output/export.h"
#include "output/png.h"
#include "output/tiff.h"

#include "cimgui.h"

// Export-mode Quality panel: the format-dependent encoder controls.
// JPEG shows a quality slider; PNG shows a bit-depth selector; TIFF
// shows bit-depth plus a compression scheme. The panel reads the
// active format from the shared ap_export_settings and renders only
// the controls that apply.

static void export_quality_draw(ap_app *app)
{
    if (!app) return;
    ap_export_settings *s = ap_app_export_settings(app);
    if (!s) return;

    if (!igBegin("Quality##export", NULL, 0)) {
        igEnd();
        return;
    }

    switch (s->format) {
    case AP_EXPORT_FORMAT_JPEG:
        igText("JPEG quality");
        igSetNextItemWidth(-1.0f);
        igSliderInt("##jpeg_quality", &s->jpeg_quality, 1, 100, "%d", 0);
        igTextDisabled("higher is better quality and a larger file");
        break;

    case AP_EXPORT_FORMAT_TIFF: {
        igText("Bit depth");
        static const char *const tiff_depth_items[] = {
            "8-bit integer", "16-bit integer",
        };
        igSetNextItemWidth(-1.0f);
        igCombo_Str_arr("##tiff_depth", &s->tiff_depth,
                        tiff_depth_items, 2, -1);

        igSpacing();
        igText("Compression");
        static const char *const tiff_comp_items[] = {
            "None", "LZW", "Deflate (ZIP)",
        };
        igSetNextItemWidth(-1.0f);
        igCombo_Str_arr("##tiff_compress", &s->tiff_compress,
                        tiff_comp_items, 3, -1);
        igTextDisabled("LZW and Deflate are lossless");

        igSpacing();
        igTextWrapped("The render pipeline reads back as 8-bit; a "
                      "16-bit TIFF widens the container but recovers "
                      "no extra precision yet.");
        break;
    }

    case AP_EXPORT_FORMAT_PNG: {
        igText("Bit depth");
        static const char *const png_depth_items[] = {
            "8-bit", "16-bit",
        };
        igSetNextItemWidth(-1.0f);
        igCombo_Str_arr("##png_depth", &s->png_depth,
                        png_depth_items, 2, -1);
        igTextDisabled("PNG is always lossless");
        break;
    }

    default:
        igTextDisabled("(no format selected)");
        break;
    }

    igEnd();
}

const ap_panel panel_export_quality = {
    .name = "export_quality",
    .mode = AP_MODE_EXPORT,
    .draw = export_quality_draw,
};
