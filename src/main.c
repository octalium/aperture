#include "app/app.h"
#include "core/log.h"
#include "library/library.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifndef APERTURE_VERSION
#error "APERTURE_VERSION must be defined at compile time (set via meson)"
#endif

static int open_argument(ap_app *app, const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        AP_ERROR("cannot stat %s: %s", path, strerror(errno));
        return -1;
    }
    if (S_ISDIR(st.st_mode)) {
        return ap_app_open_library(app, path);
    }
    if (S_ISREG(st.st_mode)) {
        return ap_app_open_photo(app, path);
    }
    AP_ERROR("%s is neither a file nor a directory", path);
    return -1;
}

int main(int argc, char **argv)
{
    srand((unsigned int)time(NULL));
    AP_INFO("aperture %s", APERTURE_VERSION);

    ap_app *app = ap_app_create(1280, 720, "Aperture");
    if (!app) {
        return 1;
    }

    if (argc > 1) {
        if (open_argument(app, argv[1]) != 0) {
            ap_app_destroy(app);
            return 1;
        }
    } else {
        // No CLI arg - resume the most-recent library if one exists.
        ap_registry_entry rows[1];
        if (ap_registry_list(rows, 1) > 0) {
            AP_INFO("resuming most-recent library: %s", rows[0].path);
            // Failure here is non-fatal - start with no library open.
            ap_app_open_library(app, rows[0].path);
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
