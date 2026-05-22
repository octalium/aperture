#include "panels.h"

#include "app/app.h"
#include "output/export.h"

#include "cimgui.h"

// Export-mode Format panel: picks the output container format. The
// other export panels (Quality, Naming, Destination) read the chosen
// format back from the shared ap_export_settings to show the
// format-appropriate controls.

static void export_format_draw(ap_app *app)
{
    if (!app) return;
    ap_export_settings *s = ap_app_export_settings(app);
    if (!s) return;

    if (!igBegin("Format##export", NULL, 0)) {
        igEnd();
        return;
    }

    igTextDisabled("output container for the exported file");
    igSeparator();

    igRadioButton_IntPtr("JPEG", &s->format, AP_EXPORT_FORMAT_JPEG);
    igSameLine(0.0f, 16.0f);
    igRadioButton_IntPtr("TIFF", &s->format, AP_EXPORT_FORMAT_TIFF);
    igSameLine(0.0f, 16.0f);
    igRadioButton_IntPtr("PNG", &s->format, AP_EXPORT_FORMAT_PNG);

    igSpacing();
    switch (s->format) {
    case AP_EXPORT_FORMAT_JPEG:
        igTextWrapped("Lossy, 8-bit. Smallest files; ideal for web "
                      "and sharing.");
        break;
    case AP_EXPORT_FORMAT_TIFF:
        igTextWrapped("Lossless, 8- or 16-bit, optional compression. "
                      "Best for print and archival.");
        break;
    case AP_EXPORT_FORMAT_PNG:
        igTextWrapped("Lossless, 8- or 16-bit. Good for screenshots "
                      "and graphics with hard edges.");
        break;
    default:
        break;
    }

    igEnd();
}

const ap_panel panel_export_format = {
    .name = "export_format",
    .mode = AP_MODE_EXPORT,
    .draw = export_format_draw,
};
