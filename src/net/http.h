#ifndef APERTURE_NET_HTTP_H
#define APERTURE_NET_HTTP_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// streaming response callback. invoked zero or more times during
// ap_http_get with chunks of the response body. return the number of
// bytes consumed; returning anything other than `len` aborts the
// transfer and surfaces as an error from ap_http_get.
typedef size_t (*ap_http_write_fn)(const char *data, size_t len, void *user);

// request options. all string fields are caller-owned, must outlive
// the ap_http_get call. user_agent may be NULL (no UA header set).
// timeout_s applies to both connect and overall transfer.
typedef struct {
    const char       *url;
    const char       *user_agent;
    int               timeout_s;
    int               max_redirects;
    ap_http_write_fn  write_fn;
    void             *write_user;
} ap_http_request;

// result of a completed ap_http_get call.
typedef struct {
    long  http_status;     // server-reported status code (0 if transfer failed)
    char  error[256];      // human-readable error string; empty on success
    bool  ok;              // true iff transfer completed without transport error
} ap_http_response;

// perform a blocking HTTPS GET. follows up to req->max_redirects
// redirects. streams the response body into req->write_fn. on success
// `out->ok` is true and `out->http_status` reflects the final status;
// caller must still inspect http_status for the 2xx outcome. on
// transport failure `out->ok` is false and `out->error` describes why.
//
// thread-safety: callable from any thread. each call uses its own
// per-platform handle; no global state to coordinate.
void ap_http_get(const ap_http_request *req, ap_http_response *out);

#ifdef __cplusplus
}
#endif

#endif
