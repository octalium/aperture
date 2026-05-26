#include "update/updater.h"

#include "core/log.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

// Flatpak distribution updater.
//
// Flatpak owns its own update path: users install updates through
// their software center (GNOME Software, KDE Discover, ...) or via
// `flatpak update` on the command line. The sandbox blocks the app
// from invoking `flatpak update` on itself, so apply() does NOT
// install — it surfaces the update to the user by opening their
// software center on aperture's AppStream page. The detection half
// (manifest fetch + newer-version flag) is shared with every other
// platform via src/update/check.{c,h}; this TU only overrides apply().

#define AP_FLATPAK_APP_ID         "io.github.octalium.aperture"
#define AP_FLATPAK_APPSTREAM_URI  "appstream://" AP_FLATPAK_APP_ID
#define AP_FLATPAK_FLATHUB_URL    "https://flathub.org/apps/" AP_FLATPAK_APP_ID

static int xdg_open(const char *target)
{
    // target is a compile-time constant in every call site below.
    // Do NOT extend this to accept caller-supplied strings without
    // shell-quoting first.
    char cmd[512];
    int  n = snprintf(cmd, sizeof(cmd),
                      "xdg-open '%s' >/dev/null 2>&1", target);
    if (n < 0 || (size_t)n >= sizeof(cmd)) return -1;
    return system(cmd);
}

static int flatpak_apply(const char **err_msg)
{
    if (xdg_open(AP_FLATPAK_APPSTREAM_URI) == 0) return 0;

    AP_WARN("update: appstream:// handler failed; "
            "falling back to Flathub web page");
    if (xdg_open(AP_FLATPAK_FLATHUB_URL) == 0) return 0;

    AP_WARN("update: could not open software center or Flathub page");
    if (err_msg) {
        *err_msg = "Could not open software center. Run "
                   "'flatpak update " AP_FLATPAK_APP_ID
                   "' in a terminal to update.";
    }
    return -1;
}

const ap_updater *ap_updater_flatpak_get(void)
{
    static ap_updater self;
    static bool       init = false;
    if (!init) {
        self       = *ap_updater_default();
        self.apply = flatpak_apply;
        init       = true;
    }
    return &self;
}
