#include "update/settings.h"

#include "library/library.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KEY_CHECK_ON_LAUNCH "update.check_on_launch"
#define KEY_MANIFEST_URL    "update.manifest_url"

void ap_update_settings_load(ap_update_settings *out)
{
    if (!out) return;
    out->check_on_launch = true;
    out->manifest_url[0] = '\0';

    char buf[1024];
    if (ap_settings_get(KEY_CHECK_ON_LAUNCH, buf, sizeof(buf)) == 0) {
        out->check_on_launch = (atoi(buf) != 0);
    }
    if (ap_settings_get(KEY_MANIFEST_URL, buf, sizeof(buf)) == 0) {
        snprintf(out->manifest_url, sizeof(out->manifest_url), "%s", buf);
    }
}

void ap_update_settings_save(const ap_update_settings *s)
{
    if (!s) return;
    ap_settings_set(KEY_CHECK_ON_LAUNCH, s->check_on_launch ? "1" : "0");
    ap_settings_set(KEY_MANIFEST_URL, s->manifest_url);
}
