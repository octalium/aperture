#include "update/updater.h"

#include "core/log.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define AP_UPDATE_RELEASES_URL "https://github.com/octalium/aperture/releases"

static ap_manifest g_pending;
static bool        g_pending_set = false;
static bool        g_newer       = false;

static void default_check(void)
{
    // The startup-check path in src/app/app.c submits the worker job
    // directly via ap_update_check_submit. Manual "Check for updates"
    // from the About modal does the same. The platform-stub default
    // keeps no extra state, so this is a no-op.
}

// Hardcoded compile-time URL above is the only argument; do NOT
// extend this to interpolate user-controlled strings without
// shell-quoting first.
static int open_releases_page(void)
{
#if defined(__linux__)
    int rc = system("xdg-open '" AP_UPDATE_RELEASES_URL "' >/dev/null 2>&1");
#elif defined(__APPLE__)
    int rc = system("open '" AP_UPDATE_RELEASES_URL "' >/dev/null 2>&1");
#elif defined(_WIN32)
    int rc = system("start \"\" \"" AP_UPDATE_RELEASES_URL "\"");
#else
    int rc = -1;
#endif
    return rc;
}

static int default_apply(const char **err_msg)
{
    int rc = open_releases_page();
    if (rc != 0) {
        AP_WARN("update: could not launch browser for %s",
                AP_UPDATE_RELEASES_URL);
        if (err_msg) {
            *err_msg = "Could not launch browser. Visit "
                       AP_UPDATE_RELEASES_URL " manually.";
        }
        return rc;
    }
    return 0;
}

static bool default_available(void)
{
    return g_pending_set && g_newer;
}

static const ap_updater g_default = {
    .check     = default_check,
    .apply     = default_apply,
    .available = default_available,
};

const ap_updater *ap_updater_get(void)
{
    return &g_default;
}

void ap_updater_set_pending(const ap_manifest *manifest, bool newer)
{
    if (!manifest) {
        g_pending_set = false;
        g_newer       = false;
        return;
    }
    g_pending     = *manifest;
    g_pending_set = true;
    g_newer       = newer;
}

const ap_manifest *ap_updater_pending(void)
{
    return g_pending_set ? &g_pending : NULL;
}
