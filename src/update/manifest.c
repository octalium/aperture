#include "update/manifest.h"

#include "core/log.h"

#include "cJSON.h"

#include <stdio.h>
#include <string.h>

static void copy_str(char *dst, size_t cap, const cJSON *node, const char *key)
{
    if (!dst || cap == 0) return;
    dst[0] = '\0';
    if (!node) return;
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(node, key);
    if (!cJSON_IsString(v) || !v->valuestring) return;
    snprintf(dst, cap, "%s", v->valuestring);
}

int ap_manifest_parse(const char *json, size_t len, ap_manifest *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!json || len == 0) {
        AP_ERROR("manifest: empty input");
        return -1;
    }

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        const char *err = cJSON_GetErrorPtr();
        AP_ERROR("manifest: parse failed near '%.40s'",
                 err ? err : "<unknown>");
        return -1;
    }
    if (!cJSON_IsObject(root)) {
        AP_ERROR("manifest: root is not an object");
        cJSON_Delete(root);
        return -1;
    }

    copy_str(out->latest,   sizeof(out->latest),   root, "latest");
    copy_str(out->released, sizeof(out->released), root, "released");
    copy_str(out->notes,    sizeof(out->notes),    root, "notes");

    if (out->latest[0] == '\0') {
        AP_ERROR("manifest: required field 'latest' is missing");
        cJSON_Delete(root);
        return -1;
    }

    const cJSON *channels = cJSON_GetObjectItemCaseSensitive(root, "channels");
    if (cJSON_IsObject(channels)) {
        const cJSON *dmg =
            cJSON_GetObjectItemCaseSensitive(channels, "dmg");
        if (cJSON_IsObject(dmg)) {
            copy_str(out->dmg.url,    sizeof(out->dmg.url),    dmg, "url");
            copy_str(out->dmg.sha256, sizeof(out->dmg.sha256), dmg, "sha256");
        }
        const cJSON *msi =
            cJSON_GetObjectItemCaseSensitive(channels, "msi");
        if (cJSON_IsObject(msi)) {
            copy_str(out->msi.url,    sizeof(out->msi.url),    msi, "url");
            copy_str(out->msi.sha256, sizeof(out->msi.sha256), msi, "sha256");
        }
        const cJSON *flatpak =
            cJSON_GetObjectItemCaseSensitive(channels, "flatpak");
        if (cJSON_IsObject(flatpak)) {
            copy_str(out->flatpak.ref, sizeof(out->flatpak.ref),
                     flatpak, "ref");
        }
    }

    cJSON_Delete(root);
    return 0;
}
