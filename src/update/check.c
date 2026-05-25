#include "update/check.h"

#include "core/log.h"
#include "core/version.h"

#include <curl/curl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AP_UPDATE_MANIFEST_MAX_BYTES (256u * 1024u)
#define AP_UPDATE_FETCH_TIMEOUT_S    15L

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
    bool   overflow;
} fetch_buf;

static size_t fetch_write(char *data, size_t size, size_t nmemb, void *userp)
{
    fetch_buf *b = (fetch_buf *)userp;
    size_t add = size * nmemb;
    if (b->overflow) return 0;
    if (b->len + add + 1 > AP_UPDATE_MANIFEST_MAX_BYTES) {
        b->overflow = true;
        return 0;
    }
    if (b->len + add + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + add + 1) cap *= 2;
        if (cap > AP_UPDATE_MANIFEST_MAX_BYTES) cap = AP_UPDATE_MANIFEST_MAX_BYTES;
        char *p = realloc(b->buf, cap);
        if (!p) return 0;
        b->buf = p;
        b->cap = cap;
    }
    memcpy(b->buf + b->len, data, add);
    b->len += add;
    b->buf[b->len] = '\0';
    return add;
}

void ap_update_check_run(ap_work_item *self)
{
    ap_update_check_job *j = (ap_update_check_job *)self;
    j->ok    = false;
    j->newer = false;
    j->error[0] = '\0';

    CURL *curl = curl_easy_init();
    if (!curl) {
        snprintf(j->error, sizeof(j->error), "curl init failed");
        return;
    }

    fetch_buf buf = {0};
    char user_agent[128];
    snprintf(user_agent, sizeof(user_agent),
             "aperture/%s update-check", j->current_version);

    curl_easy_setopt(curl, CURLOPT_URL,             j->url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,  1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS,       5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,         AP_UPDATE_FETCH_TIMEOUT_S);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,  AP_UPDATE_FETCH_TIMEOUT_S);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,       user_agent);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,   fetch_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,       &buf);

    CURLcode rc = curl_easy_perform(curl);
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        snprintf(j->error, sizeof(j->error),
                 "fetch failed: %s", curl_easy_strerror(rc));
        free(buf.buf);
        return;
    }
    if (http_status < 200 || http_status >= 300) {
        snprintf(j->error, sizeof(j->error),
                 "fetch returned HTTP %ld", http_status);
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
