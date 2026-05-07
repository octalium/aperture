#include "panels.h"

#include "cimgui.h"

static void library_draw(ap_app *app)
{
    (void)app;
    if (igBegin("Library", NULL, 0)) {
        igText("Library mode");
        igSeparator();
        igTextWrapped("Browseable thumbnail grid + group filter arrives "
                      "with the SQLite library index. For now, open a "
                      "raw via the command line.");
    }
    igEnd();
}

const ap_panel panel_library = {
    .name = "library",
    .mode = AP_MODE_LIBRARY,
    .draw = library_draw,
};
