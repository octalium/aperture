#include "update/check.h"

#include "core/log.h"
#include "core/version.h"
#include "net/http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AP_UPDATE_MANIFEST_MAX_BYTES (256u * 1024u)
#define AP_UPDATE_FETCH_TIMEOUT_S    15
#define AP_UPDATE_MAX_REDIRECTS      5

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
    bool   overflow;
} fetch_buf;

static size_t fetch_write(const char *data, size_t len, void *userp)
{
    fetch_buf *b = (fetch_buf *)userp;
    if (b->overflow) return 0;
    if (b->len + len + 1 > AP_UPDATE_MANIFEST_MAX_BYTES) {
        b->overflow = true;
        return 0;
    }
    if (b->len + len + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + len + 1) cap *= 2;
        if (cap > AP_UPDATE_MANIFEST_MAX_BYTES) cap = AP_UPDATE_MANIFEST_MAX_BYTES;
        char *p = realloc(b->buf, cap);
        if (!p) return 0;
        b->buf = p;
        b->cap = cap;
    }
    memcpy(b->buf + b->len, data, len);
    b->len += len;
    b->buf[b->len] = '\0';
    return len;
}

void ap_update_check_run(ap_work_item *self)
{
    ap_update_check_job *j = (ap_update_check_job *)self;
    j->ok    = false;
    j->newer = false;
    j->error[0] = '\0';

    fetch_buf buf = {0};
    char user_agent[128];
    snprintf(user_agent, sizeof(user_agent),
             "aperture/%s update-check", j->current_version);

    ap_http_request req = {
        .url           = j->url,
        .user_agent    = user_agent,
        .timeout_s     = AP_UPDATE_FETCH_TIMEOUT_S,
        .max_redirects = AP_UPDATE_MAX_REDIRECTS,
        .write_fn      = fetch_write,
        .write_user    = &buf,
    };
    ap_http_response resp;
    ap_http_get(&req, &resp);

    if (!resp.ok) {
        snprintf(j->error, sizeof(j->error), "fetch failed: %s", resp.error);
        free(buf.buf);
        return;
    }
    if (resp.http_status < 200 || resp.http_status >= 300) {
        snprintf(j->error, sizeof(j->error),
                 "fetch returned HTTP %ld", resp.http_status);
        free(buf.buf);
        return;
    }
    if (buf.overflow) {
        snprintf(j->error, sizeof(j->error),
                 "manifest exceeds %u bytes", AP_UPDATE_MANIFEST_MAX_BYTES);
        free(buf.buf);
        return;
    }
    if (!buf.buf || buf.len == 0) {
        snprintf(j->error, sizeof(j->error), "manifest was empty");
        free(buf.buf);
        return;
    }

    if (ap_manifest_parse(buf.buf, buf.len, &j->manifest) != 0) {
        snprintf(j->error, sizeof(j->error), "manifest parse failed");
        free(buf.buf);
        return;
    }
    free(buf.buf);

    j->ok    = true;
    j->newer = ap_version_compare(j->manifest.latest, j->current_version) > 0;
}

int ap_update_check_submit(ap_worker_pool *pool,
                           const char     *url,
                           const char     *current_version)
{
    if (!pool) return -1;
    ap_update_check_job *j = calloc(1, sizeof(*j));
    if (!j) {
        AP_ERROR("update: check-job alloc failed");
        return -1;
    }
    j->base.run = ap_update_check_run;
    const char *u = (url && *url) ? url : AP_UPDATE_DEFAULT_MANIFEST_URL;
    snprintf(j->url, sizeof(j->url), "%s", u);
    snprintf(j->current_version, sizeof(j->current_version), "%s",
             (current_version && *current_version) ? current_version
                                                   : AP_VERSION_STRING);
    ap_worker_pool_submit(pool, &j->base);
    return 0;
}
