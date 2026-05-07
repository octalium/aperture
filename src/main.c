#include "app/app.h"
#include "core/log.h"

#ifndef APERTURE_VERSION
#error "APERTURE_VERSION must be defined at compile time (set via meson)"
#endif

int main(int argc, char **argv)
{
    AP_INFO("aperture %s", APERTURE_VERSION);

    ap_app *app = ap_app_create(1280, 720, "aperture");
    if (!app) {
        return 1;
    }

    if (argc > 1) {
        if (ap_app_open_photo(app, argv[1]) != 0) {
            ap_app_destroy(app);
            return 1;
        }
    }

    int rc = 0;
    while (ap_app_should_run(app)) {
        if (ap_app_run_frame(app) < 0) {
            rc = 1;
            break;
        }
    }

    ap_app_destroy(app);
    return rc;
}
