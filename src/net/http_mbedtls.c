#include "net/http.h"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// minimal HTTPS GET over mbedTLS. supports follow-redirect (3xx with
// Location:), streaming response body to a caller callback, and
// strict server-cert verification against a system CA bundle.

#define AP_HTTP_REQ_CAP  4096
#define AP_HTTP_HDR_CAP  8192

// common CA bundle locations across linux distros, in priority order.
// the first readable entry wins. document any additions inline.
static const char *const ca_bundle_paths[] = {
    "/etc/ssl/certs/ca-certificates.crt",   // debian/ubuntu, alpine, arch (ca-certificates)
    "/etc/pki/tls/certs/ca-bundle.crt",     // fedora, rhel, centos, amazon linux
    "/etc/ssl/ca-bundle.pem",               // opensuse
    "/etc/ssl/cert.pem",                    // some arch setups, alpine fallback, *bsd
    "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",   // fedora alternate
    NULL,
};

typedef struct {
    char        scheme[8];
    char        host[256];
    char        port[8];
    char        path[1024];
} url_parts;

static int parse_url(const char *url, url_parts *p)
{
    if (!url) return -1;
    memset(p, 0, sizeof(*p));

    const char *sep = strstr(url, "://");
    if (!sep) return -1;
    size_t scheme_len = (size_t)(sep - url);
    if (scheme_len == 0 || scheme_len >= sizeof(p->scheme)) return -1;
    memcpy(p->scheme, url, scheme_len);
    p->scheme[scheme_len] = '\0';
    for (size_t i = 0; i < scheme_len; ++i) {
        p->scheme[i] = (char)tolower((unsigned char)p->scheme[i]);
    }

    const char *host_start = sep + 3;
    const char *path_start = strchr(host_start, '/');
    const char *host_end   = path_start ? path_start : host_start + strlen(host_start);
    const char *colon      = NULL;
    for (const char *c = host_start; c < host_end; ++c) {
        if (*c == ':') { colon = c; break; }
    }
    const char *host_stop = colon ? colon : host_end;
    size_t host_len = (size_t)(host_stop - host_start);
    if (host_len == 0 || host_len >= sizeof(p->host)) return -1;
    memcpy(p->host, host_start, host_len);
    p->host[host_len] = '\0';

    if (colon) {
        size_t port_len = (size_t)(host_end - colon - 1);
        if (port_len == 0 || port_len >= sizeof(p->port)) return -1;
        memcpy(p->port, colon + 1, port_len);
        p->port[port_len] = '\0';
    } else {
        snprintf(p->port, sizeof(p->port),
                 (strcmp(p->scheme, "https") == 0) ? "443" : "80");
    }

    if (path_start) {
        size_t path_len = strlen(path_start);
        if (path_len >= sizeof(p->path)) return -1;
        memcpy(p->path, path_start, path_len + 1);
    } else {
        p->path[0] = '/';
        p->path[1] = '\0';
    }
    return 0;
}

// resolve a Location: value against a base URL. supports absolute URLs
// (https://...) and absolute-path references (/foo/bar). returns 0 on
// success and writes the resolved URL into `out`.
static int resolve_location(const url_parts *base,
                            const char      *loc,
                            size_t           loc_len,
                            char            *out,
                            size_t           out_cap)
{
    while (loc_len && (loc[0] == ' ' || loc[0] == '\t')) { loc++; loc_len--; }
    while (loc_len && (loc[loc_len - 1] == ' '
                       || loc[loc_len - 1] == '\t'
                       || loc[loc_len - 1] == '\r')) {
        loc_len--;
    }
    if (loc_len == 0) return -1;
    if (loc_len > 6 && (strncasecmp(loc, "http://", 7) == 0
                        || strncasecmp(loc, "https://", 8) == 0)) {
        if (loc_len + 1 > out_cap) return -1;
        memcpy(out, loc, loc_len);
        out[loc_len] = '\0';
        return 0;
    }
    if (loc[0] == '/') {
        int n = snprintf(out, out_cap, "%s://%s%s%s%.*s",
                         base->scheme, base->host,
                         (base->port[0] && strcmp(base->port,
                             strcmp(base->scheme, "https") == 0 ? "443" : "80") != 0)
                             ? ":" : "",
                         (base->port[0] && strcmp(base->port,
                             strcmp(base->scheme, "https") == 0 ? "443" : "80") != 0)
                             ? base->port : "",
                         (int)loc_len, loc);
        if (n < 0 || (size_t)n >= out_cap) return -1;
        return 0;
    }
    return -1;
}

// build a HTTP/1.1 GET request line + headers + blank line. returns
// the byte length written, or -1 on overflow.
static int build_request(char       *buf,
                         size_t      cap,
                         const char *host,
                         const char *port,
                         const char *path,
                         const char *user_agent)
{
    int default_port = (strcmp(port, "443") == 0 || strcmp(port, "80") == 0);
    int n;
    if (default_port) {
        n = snprintf(buf, cap,
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "User-Agent: %s\r\n"
                     "Accept: */*\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     path, host, user_agent ? user_agent : "aperture-http");
    } else {
        n = snprintf(buf, cap,
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s:%s\r\n"
                     "User-Agent: %s\r\n"
                     "Accept: */*\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     path, host, port, user_agent ? user_agent : "aperture-http");
    }
    if (n < 0 || (size_t)n >= cap) return -1;
    return n;
}

typedef struct {
    long status;
    char location[2048];
    bool have_location;
    bool chunked;
    long content_length;
    bool have_content_length;
} resp_headers;

// parse the status line + headers from the front of `buf`. returns the
// byte offset just past the terminating CRLFCRLF, or -1 if the headers
// aren't fully present (caller should read more) or are malformed.
static int parse_headers(const char *buf, size_t len, resp_headers *h)
{
    memset(h, 0, sizeof(*h));
    h->content_length = -1;

    const char *eoh = NULL;
    for (size_t i = 0; i + 3 < len; ++i) {
        if (buf[i] == '\r' && buf[i+1] == '\n'
            && buf[i+2] == '\r' && buf[i+3] == '\n') {
            eoh = buf + i + 4;
            break;
        }
    }
    if (!eoh) return -1;

    if (len < 12 || strncmp(buf, "HTTP/", 5) != 0) return -1;
    const char *sp = strchr(buf, ' ');
    if (!sp) return -1;
    h->status = strtol(sp + 1, NULL, 10);

    const char *line = strstr(buf, "\r\n");
    if (!line) return -1;
    line += 2;
    while (line < eoh - 2) {
        const char *eol = strstr(line, "\r\n");
        if (!eol || eol == line) break;
        const char *colon = memchr(line, ':', (size_t)(eol - line));
        if (colon) {
            size_t name_len = (size_t)(colon - line);
            const char *val = colon + 1;
            while (val < eol && (*val == ' ' || *val == '\t')) val++;
            size_t val_len = (size_t)(eol - val);
            if (name_len == 8 && strncasecmp(line, "Location", 8) == 0) {
                if (val_len < sizeof(h->location)) {
                    memcpy(h->location, val, val_len);
                    h->location[val_len] = '\0';
                    h->have_location = true;
                }
            } else if (name_len == 14
                       && strncasecmp(line, "Content-Length", 14) == 0) {
                char numbuf[32];
                if (val_len < sizeof(numbuf)) {
                    memcpy(numbuf, val, val_len);
                    numbuf[val_len] = '\0';
                    h->content_length = strtol(numbuf, NULL, 10);
                    h->have_content_length = true;
                }
            } else if (name_len == 17
                       && strncasecmp(line, "Transfer-Encoding", 17) == 0) {
                if (val_len >= 7 && strncasecmp(val, "chunked", 7) == 0) {
                    h->chunked = true;
                }
            }
        }
        line = eol + 2;
    }
    return (int)(eoh - buf);
}

typedef struct {
    mbedtls_net_context       net;
    mbedtls_ssl_context       ssl;
    mbedtls_ssl_config        conf;
    mbedtls_x509_crt          cacert;
    mbedtls_entropy_context   entropy;
    mbedtls_ctr_drbg_context  ctr_drbg;
    bool                      net_init;
    bool                      ssl_init;
    bool                      conf_init;
    bool                      cacert_init;
    bool                      entropy_init;
    bool                      ctr_drbg_init;
} tls_ctx;

static void tls_ctx_free(tls_ctx *t)
{
    if (t->ssl_init)      mbedtls_ssl_free(&t->ssl);
    if (t->conf_init)     mbedtls_ssl_config_free(&t->conf);
    if (t->cacert_init)   mbedtls_x509_crt_free(&t->cacert);
    if (t->ctr_drbg_init) mbedtls_ctr_drbg_free(&t->ctr_drbg);
    if (t->entropy_init)  mbedtls_entropy_free(&t->entropy);
    if (t->net_init)      mbedtls_net_free(&t->net);
    memset(t, 0, sizeof(*t));
}

static void mbed_error(int rc, char *out, size_t cap)
{
    char detail[160];
    mbedtls_strerror(rc, detail, sizeof(detail));
    snprintf(out, cap, "mbedtls: %s (-0x%04x)", detail, (unsigned)-rc);
}

// load the first readable CA bundle from the well-known paths above.
// returns 0 on success and sets *path_out to the chosen path; returns
// non-zero with an error string in `err` if none could be loaded.
static int load_system_ca(mbedtls_x509_crt *cacert,
                          const char      **path_out,
                          char             *err,
                          size_t            err_cap)
{
    for (size_t i = 0; ca_bundle_paths[i]; ++i) {
        FILE *f = fopen(ca_bundle_paths[i], "rb");
        if (!f) continue;
        fclose(f);
        int rc = mbedtls_x509_crt_parse_file(cacert, ca_bundle_paths[i]);
        if (rc < 0) {
            mbed_error(rc, err, err_cap);
            return -1;
        }
        // parse_file returns 0 on success or the number of failed certs
        // for partial success; either means we have a usable trust set
        // for at least some authorities. accept and move on.
        *path_out = ca_bundle_paths[i];
        return 0;
    }
    snprintf(err, err_cap,
             "no system CA bundle found "
             "(checked /etc/ssl/certs/ca-certificates.crt and 4 others)");
    return -1;
}

static int read_all_headers(tls_ctx        *t,
                            char           *buf,
                            size_t          cap,
                            size_t         *len_out,
                            resp_headers   *h,
                            int            *body_off_out,
                            char           *err,
                            size_t          err_cap)
{
    size_t len = 0;
    for (;;) {
        if (len >= cap) {
            snprintf(err, err_cap, "response headers exceed %zu bytes", cap);
            return -1;
        }
        int rc = mbedtls_ssl_read(&t->ssl,
                                  (unsigned char *)buf + len,
                                  cap - len);
        if (rc == MBEDTLS_ERR_SSL_WANT_READ
            || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (rc == 0 || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            snprintf(err, err_cap, "connection closed before headers complete");
            return -1;
        }
        if (rc < 0) {
            mbed_error(rc, err, err_cap);
            return -1;
        }
        len += (size_t)rc;
        int body_off = parse_headers(buf, len, h);
        if (body_off > 0) {
            *len_out = len;
            *body_off_out = body_off;
            return 0;
        }
        if (body_off == -1 && len >= cap) {
            snprintf(err, err_cap, "response headers exceed buffer");
            return -1;
        }
    }
}

static int feed_body(ap_http_write_fn fn,
                     void            *user,
                     const char      *data,
                     size_t           len,
                     char            *err,
                     size_t           err_cap)
{
    if (len == 0) return 0;
    size_t n = fn(data, len, user);
    if (n != len) {
        snprintf(err, err_cap, "write callback rejected %zu/%zu bytes", n, len);
        return -1;
    }
    return 0;
}

static int read_body_identity(tls_ctx          *t,
                              long              content_length,
                              const char       *prefix,
                              size_t            prefix_len,
                              ap_http_write_fn  fn,
                              void             *user,
                              char             *err,
                              size_t            err_cap)
{
    if (feed_body(fn, user, prefix, prefix_len, err, err_cap) < 0) return -1;
    size_t total = prefix_len;
    char   chunk[4096];
    for (;;) {
        if (content_length >= 0 && (long)total >= content_length) break;
        int rc = mbedtls_ssl_read(&t->ssl,
                                  (unsigned char *)chunk, sizeof(chunk));
        if (rc == MBEDTLS_ERR_SSL_WANT_READ
            || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (rc == 0 || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) break;
        if (rc < 0) {
            mbed_error(rc, err, err_cap);
            return -1;
        }
        if (feed_body(fn, user, chunk, (size_t)rc, err, err_cap) < 0) return -1;
        total += (size_t)rc;
    }
    return 0;
}

// pull more bytes into the rolling buffer when the chunked decoder
// needs them. shifts already-consumed data out so we don't grow.
static int chunk_refill(tls_ctx *t,
                        char    *buf,
                        size_t   cap,
                        size_t  *pos,
                        size_t  *len,
                        char    *err,
                        size_t   err_cap)
{
    if (*pos > 0) {
        memmove(buf, buf + *pos, *len - *pos);
        *len -= *pos;
        *pos = 0;
    }
    if (*len >= cap) {
        snprintf(err, err_cap, "chunked stream: refill buffer full");
        return -1;
    }
    int rc = mbedtls_ssl_read(&t->ssl,
                              (unsigned char *)buf + *len,
                              cap - *len);
    if (rc == MBEDTLS_ERR_SSL_WANT_READ
        || rc == MBEDTLS_ERR_SSL_WANT_WRITE) return 1;
    if (rc == 0 || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
        snprintf(err, err_cap, "chunked stream: connection closed mid-chunk");
        return -1;
    }
    if (rc < 0) {
        mbed_error(rc, err, err_cap);
        return -1;
    }
    *len += (size_t)rc;
    return 0;
}

static int read_body_chunked(tls_ctx          *t,
                             const char       *prefix,
                             size_t            prefix_len,
                             ap_http_write_fn  fn,
                             void             *user,
                             char             *err,
                             size_t            err_cap)
{
    enum { BUF_CAP = 16384 };
    char  *buf = malloc(BUF_CAP);
    if (!buf) {
        snprintf(err, err_cap, "chunked decode: out of memory");
        return -1;
    }
    size_t len = prefix_len;
    if (len > BUF_CAP) {
        free(buf);
        snprintf(err, err_cap, "chunked decode: prefix exceeds buffer");
        return -1;
    }
    if (len) memcpy(buf, prefix, len);
    size_t pos = 0;

    int ret = 0;
    for (;;) {
        const char *eol = NULL;
        for (size_t i = pos; i + 1 < len; ++i) {
            if (buf[i] == '\r' && buf[i+1] == '\n') {
                eol = buf + i;
                break;
            }
        }
        if (!eol) {
            int rc = chunk_refill(t, buf, BUF_CAP, &pos, &len, err, err_cap);
            if (rc < 0) { ret = -1; goto done; }
            continue;
        }
        char size_str[32];
        size_t size_len = (size_t)(eol - (buf + pos));
        const char *semi = memchr(buf + pos, ';', size_len);
        if (semi) size_len = (size_t)(semi - (buf + pos));
        if (size_len >= sizeof(size_str)) {
            snprintf(err, err_cap, "chunked decode: chunk size header too long");
            ret = -1; goto done;
        }
        memcpy(size_str, buf + pos, size_len);
        size_str[size_len] = '\0';
        char *end = NULL;
        unsigned long chunk_size = strtoul(size_str, &end, 16);
        if (end == size_str) {
            snprintf(err, err_cap, "chunked decode: invalid chunk size");
            ret = -1; goto done;
        }
        pos = (size_t)(eol - buf) + 2;
        if (chunk_size == 0) {
            // trailer headers may follow; we ignore them, just drain to
            // the terminating CRLF after the zero-length chunk.
            for (;;) {
                bool found = false;
                for (size_t i = pos; i + 1 < len; ++i) {
                    if (buf[i] == '\r' && buf[i+1] == '\n') {
                        if (i == pos) { found = true; pos = i + 2; break; }
                        pos = i + 2;
                        break;
                    }
                }
                if (found) break;
                if (pos >= len) {
                    int rc = chunk_refill(t, buf, BUF_CAP, &pos, &len,
                                          err, err_cap);
                    if (rc < 0) { ret = -1; goto done; }
                    if (rc > 0) continue;
                }
                if (pos >= len) break;
            }
            break;
        }
        size_t remaining = chunk_size;
        while (remaining > 0) {
            size_t avail = len - pos;
            if (avail == 0) {
                int rc = chunk_refill(t, buf, BUF_CAP, &pos, &len,
                                      err, err_cap);
                if (rc < 0) { ret = -1; goto done; }
                continue;
            }
            size_t take = avail < remaining ? avail : remaining;
            if (feed_body(fn, user, buf + pos, take, err, err_cap) < 0) {
                ret = -1; goto done;
            }
            pos += take;
            remaining -= take;
        }
        while (len - pos < 2) {
            int rc = chunk_refill(t, buf, BUF_CAP, &pos, &len, err, err_cap);
            if (rc < 0) { ret = -1; goto done; }
        }
        if (!(buf[pos] == '\r' && buf[pos+1] == '\n')) {
            snprintf(err, err_cap, "chunked decode: missing CRLF after chunk");
            ret = -1; goto done;
        }
        pos += 2;
    }

done:
    free(buf);
    return ret;
}

static int do_get_once(const url_parts        *url,
                       const ap_http_request  *req,
                       resp_headers           *headers_out,
                       bool                    follow_only,
                       ap_http_response       *out)
{
    tls_ctx t;
    memset(&t, 0, sizeof(t));

    if (strcmp(url->scheme, "https") != 0) {
        snprintf(out->error, sizeof(out->error),
                 "unsupported scheme: %s", url->scheme);
        return -1;
    }

    mbedtls_net_init(&t.net);           t.net_init      = true;
    mbedtls_ssl_init(&t.ssl);           t.ssl_init      = true;
    mbedtls_ssl_config_init(&t.conf);   t.conf_init     = true;
    mbedtls_x509_crt_init(&t.cacert);   t.cacert_init   = true;
    mbedtls_entropy_init(&t.entropy);   t.entropy_init  = true;
    mbedtls_ctr_drbg_init(&t.ctr_drbg); t.ctr_drbg_init = true;

    const char *seed = "aperture-update";
    int rc = mbedtls_ctr_drbg_seed(&t.ctr_drbg,
                                   mbedtls_entropy_func, &t.entropy,
                                   (const unsigned char *)seed,
                                   strlen(seed));
    if (rc != 0) { mbed_error(rc, out->error, sizeof(out->error));
                   tls_ctx_free(&t); return -1; }

    const char *ca_path = NULL;
    if (load_system_ca(&t.cacert, &ca_path,
                       out->error, sizeof(out->error)) != 0) {
        tls_ctx_free(&t);
        return -1;
    }

    rc = mbedtls_net_connect(&t.net, url->host, url->port,
                             MBEDTLS_NET_PROTO_TCP);
    if (rc != 0) {
        mbed_error(rc, out->error, sizeof(out->error));
        tls_ctx_free(&t);
        return -1;
    }

    if (req->timeout_s > 0) {
        // mbedtls_net_set_block + recv_timeout. set_recv_timeout requires
        // mbedtls_ssl_set_bio with recv_timeout variant, configured below.
        mbedtls_net_set_block(&t.net);
    }

    rc = mbedtls_ssl_config_defaults(&t.conf,
                                     MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) { mbed_error(rc, out->error, sizeof(out->error));
                   tls_ctx_free(&t); return -1; }

    mbedtls_ssl_conf_authmode(&t.conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&t.conf, &t.cacert, NULL);
    mbedtls_ssl_conf_rng(&t.conf, mbedtls_ctr_drbg_random, &t.ctr_drbg);
    if (req->timeout_s > 0) {
        mbedtls_ssl_conf_read_timeout(&t.conf,
                                      (uint32_t)req->timeout_s * 1000u);
    }

    rc = mbedtls_ssl_setup(&t.ssl, &t.conf);
    if (rc != 0) { mbed_error(rc, out->error, sizeof(out->error));
                   tls_ctx_free(&t); return -1; }
    rc = mbedtls_ssl_set_hostname(&t.ssl, url->host);
    if (rc != 0) { mbed_error(rc, out->error, sizeof(out->error));
                   tls_ctx_free(&t); return -1; }
    mbedtls_ssl_set_bio(&t.ssl, &t.net,
                        mbedtls_net_send,
                        NULL,
                        mbedtls_net_recv_timeout);

    for (;;) {
        rc = mbedtls_ssl_handshake(&t.ssl);
        if (rc == 0) break;
        if (rc != MBEDTLS_ERR_SSL_WANT_READ
            && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            mbed_error(rc, out->error, sizeof(out->error));
            tls_ctx_free(&t);
            return -1;
        }
    }

    uint32_t flags = mbedtls_ssl_get_verify_result(&t.ssl);
    if (flags != 0) {
        char buf[200];
        mbedtls_x509_crt_verify_info(buf, sizeof(buf), "", flags);
        snprintf(out->error, sizeof(out->error),
                 "cert verify failed: %s", buf);
        tls_ctx_free(&t);
        return -1;
    }

    char req_buf[AP_HTTP_REQ_CAP];
    int req_len = build_request(req_buf, sizeof(req_buf),
                                url->host, url->port, url->path,
                                req->user_agent);
    if (req_len < 0) {
        snprintf(out->error, sizeof(out->error),
                 "request line exceeds %d bytes", AP_HTTP_REQ_CAP);
        tls_ctx_free(&t);
        return -1;
    }
    size_t sent = 0;
    while (sent < (size_t)req_len) {
        rc = mbedtls_ssl_write(&t.ssl,
                               (const unsigned char *)req_buf + sent,
                               (size_t)req_len - sent);
        if (rc == MBEDTLS_ERR_SSL_WANT_READ
            || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (rc < 0) {
            mbed_error(rc, out->error, sizeof(out->error));
            tls_ctx_free(&t);
            return -1;
        }
        sent += (size_t)rc;
    }

    char         hdr_buf[AP_HTTP_HDR_CAP];
    size_t       hdr_len   = 0;
    int          body_off  = 0;
    resp_headers h;
    if (read_all_headers(&t, hdr_buf, sizeof(hdr_buf),
                         &hdr_len, &h, &body_off,
                         out->error, sizeof(out->error)) != 0) {
        tls_ctx_free(&t);
        return -1;
    }
    *headers_out = h;
    out->http_status = h.status;

    if (follow_only && h.status >= 300 && h.status < 400 && h.have_location) {
        // redirect with no body of interest; close and let the caller follow.
        mbedtls_ssl_close_notify(&t.ssl);
        tls_ctx_free(&t);
        return 0;
    }

    int body_rc;
    if (h.chunked) {
        body_rc = read_body_chunked(&t,
                                    hdr_buf + body_off,
                                    hdr_len - (size_t)body_off,
                                    req->write_fn, req->write_user,
                                    out->error, sizeof(out->error));
    } else {
        body_rc = read_body_identity(&t, h.content_length,
                                     hdr_buf + body_off,
                                     hdr_len - (size_t)body_off,
                                     req->write_fn, req->write_user,
                                     out->error, sizeof(out->error));
    }
    mbedtls_ssl_close_notify(&t.ssl);
    tls_ctx_free(&t);
    return body_rc;
}

void ap_http_get(const ap_http_request *req, ap_http_response *out)
{
    memset(out, 0, sizeof(*out));
    if (!req || !req->url || !req->write_fn) {
        snprintf(out->error, sizeof(out->error), "invalid request");
        return;
    }
    char       current[2048];
    size_t     url_len = strlen(req->url);
    if (url_len + 1 > sizeof(current)) {
        snprintf(out->error, sizeof(out->error), "url too long");
        return;
    }
    memcpy(current, req->url, url_len + 1);

    int max_redirects = req->max_redirects > 0 ? req->max_redirects : 0;
    for (int hop = 0; hop <= max_redirects; ++hop) {
        url_parts url;
        if (parse_url(current, &url) != 0) {
            snprintf(out->error, sizeof(out->error),
                     "invalid url: %.200s", current);
            return;
        }
        bool maybe_redirect = (hop < max_redirects);
        resp_headers h;
        int rc = do_get_once(&url, req, &h, maybe_redirect, out);
        if (rc != 0) return;
        if (maybe_redirect && h.status >= 300 && h.status < 400
            && h.have_location) {
            char next[2048];
            if (resolve_location(&url, h.location, strlen(h.location),
                                 next, sizeof(next)) != 0) {
                snprintf(out->error, sizeof(out->error),
                         "could not resolve redirect target: %.200s",
                         h.location);
                return;
            }
            memcpy(current, next, strlen(next) + 1);
            continue;
        }
        out->ok = true;
        return;
    }
    snprintf(out->error, sizeof(out->error),
             "exceeded %d redirects", max_redirects);
}
