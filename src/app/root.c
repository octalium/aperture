#define _GNU_SOURCE

#include "root.h"

#include "core/log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <direct.h>
#  define AP_PATH_SEP        '\\'
#  define AP_PATH_SEP_STR    "\\"
#  define AP_MKDIR(p)        _mkdir(p)
#else
#  include <sys/stat.h>
#  define AP_PATH_SEP        '/'
#  define AP_PATH_SEP_STR    "/"
#  define AP_MKDIR(p)        mkdir((p), 0755)
#endif

static char g_root[4096] = {0};

static int resolve_root(char *out, size_t out_len)
{
#if defined(_WIN32)
    const char *appdata = getenv("APPDATA");
    if (!appdata || !appdata[0]) {
        AP_ERROR("ap_app_root_path: %%APPDATA%% is not set");
        return -1;
    }
    int n = snprintf(out, out_len, "%s%caperture", appdata, AP_PATH_SEP);
    return (n < 0 || (size_t)n >= out_len) ? -1 : 0;

#elif defined(__APPLE__)
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        AP_ERROR("ap_app_root_path: $HOME is not set");
        return -1;
    }
    int n = snprintf(out, out_len, "%s/Library/Application Support/aperture", home);
    return (n < 0 || (size_t)n >= out_len) ? -1 : 0;

#else
    // Linux / *BSD / other XDG-style systems.
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && xdg[0]) {
        int n = snprintf(out, out_len, "%s/aperture", xdg);
        return (n < 0 || (size_t)n >= out_len) ? -1 : 0;
    }
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        AP_ERROR("ap_app_root_path: neither XDG_DATA_HOME nor HOME is set");
        return -1;
    }
    int n = snprintf(out, out_len, "%s/.local/share/aperture", home);
    return (n < 0 || (size_t)n >= out_len) ? -1 : 0;
#endif
}

const char *ap_app_root_path(void)
{
    if (g_root[0]) return g_root;
    if (resolve_root(g_root, sizeof(g_root)) < 0) {
        g_root[0] = '\0';
        return NULL;
    }
    return g_root;
}

static int mkdir_p(const char *path)
{
    char tmp[4096];
    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp)) {
        AP_ERROR("mkdir_p: path too long");
        return -1;
    }
    size_t len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == AP_PATH_SEP) {
        tmp[len - 1] = '\0';
    }

    // Walk separators, creating each intermediate component. Skip the
    // first character so we don't try to mkdir "/" or "C:" itself.
    for (char *p = tmp + 1; *p; p++) {
        if (*p != AP_PATH_SEP) continue;
        *p = '\0';
        if (AP_MKDIR(tmp) != 0 && errno != EEXIST) {
            AP_ERROR("mkdir(%s): %s", tmp, strerror(errno));
            return -1;
        }
        *p = AP_PATH_SEP;
    }
    if (AP_MKDIR(tmp) != 0 && errno != EEXIST) {
        AP_ERROR("mkdir(%s): %s", tmp, strerror(errno));
        return -1;
    }
    return 0;
}

int ap_app_root_ensure(void)
{
    const char *root = ap_app_root_path();
    if (!root) return -1;

    if (mkdir_p(root) < 0) return -1;

    char libs[4096];
    if (ap_app_root_join("libraries", libs, sizeof(libs)) < 0) return -1;
    if (mkdir_p(libs) < 0) return -1;

    return 0;
}

int ap_app_root_join(const char *sub, char *buf, size_t buflen)
{
    const char *root = ap_app_root_path();
    if (!root || !buf || !sub) return -1;
    int n = snprintf(buf, buflen, "%s%s%s", root, AP_PATH_SEP_STR, sub);
    if (n < 0 || (size_t)n >= buflen) return -1;
    return 0;
}
