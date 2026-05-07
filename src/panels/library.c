#include "panels.h"

#include "library/library.h"

#include "cimgui.h"

static void library_draw(ap_app *app)
{
    ap_library *lib = ap_app_library(app);

    if (igBegin("Library", NULL, 0)) {
        if (!lib) {
            igTextWrapped("No library open. Run aperture <directory> to "
                          "scan a folder of raws into a library.");
            igEnd();
            return;
        }

        igText("Root: %s", ap_library_root(lib));
        int n = ap_library_photo_count(lib);
        igText("Photos: %d", n);
        igSeparator();

        for (int i = 0; i < n; i++) {
            const char *rel = ap_library_photo_relative_path(lib, i);
            if (!rel) continue;
            if (igButton(rel, (ImVec2_c){ 0.0f, 0.0f })) {
                char abs[4096];
                if (ap_library_photo_absolute_path(lib, i,
                                                   abs, sizeof(abs)) == 0) {
                    ap_app_open_photo(app, abs);
                }
            }
        }
    }
    igEnd();
}

const ap_panel panel_library = {
    .name = "library",
    .mode = AP_MODE_LIBRARY,
    .draw = library_draw,
};
