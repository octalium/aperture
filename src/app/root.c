#define _GNU_SOURCE

#include "root.h"

#include "core/log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char g_root[4096] = {0};

const char *ap_app_root_path(void)
{
    if (g_root[0]) return g_root;

    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && xdg[0]) {
        snprintf(g_root, sizeof(g_root), "%s/aperture", xdg);
        return g_root;
    }
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        AP_ERROR("ap_app_root_path: neither XDG_DATA_HOME nor HOME is set");
        g_root[0] = '\0';
        return NULL;
    }
    snprintf(g_root, sizeof(g_root), "%s/.local/share/aperture", home);
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
    if (len > 0 && tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }

    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
            AP_ERROR("mkdir(%s): %s", tmp, strerror(errno));
            return -1;
        }
        *p = '/';
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
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
    int n = snprintf(buf, buflen, "%s/%s", root, sub);
    if (n < 0 || (size_t)n >= buflen) return -1;
    return 0;
}
