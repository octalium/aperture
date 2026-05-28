#include "net/http.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <stdio.h>
#include <string.h>
#include <wchar.h>

// HTTPS GET via WinHTTP. uses the windows trust store via the default
// cert chain validation (WINHTTP_FLAG_SECURE + no override flags).
// redirects are followed by the stack itself up to the configured cap.

static void utf8_to_wide(const char *s, wchar_t *out, size_t out_cap)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0 || (size_t)n > out_cap) {
        if (out_cap) out[0] = L'\0';
        return;
    }
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out, (int)out_cap);
}

static void describe_error(DWORD code, char *out, size_t cap)
{
    LPWSTR msg = NULL;
    DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER
            | FORMAT_MESSAGE_FROM_HMODULE
            | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        GetModuleHandleW(L"winhttp.dll"),
        code,
        0,
        (LPWSTR)&msg,
        0, NULL);
    if (n == 0 || !msg) {
        snprintf(out, cap, "winhttp error %lu", (unsigned long)code);
        return;
    }
    char  utf8[256] = {0};
    int   wn = WideCharToMultiByte(CP_UTF8, 0, msg, -1,
                                   utf8, sizeof(utf8) - 1, NULL, NULL);
    LocalFree(msg);
    if (wn <= 0) {
        snprintf(out, cap, "winhttp error %lu", (unsigned long)code);
        return;
    }
    for (char *p = utf8; *p; ++p) {
        if (*p == '\r' || *p == '\n') *p = ' ';
    }
    snprintf(out, cap, "winhttp: %s (%lu)", utf8, (unsigned long)code);
}

void ap_http_get(const ap_http_request *req, ap_http_response *out)
{
    memset(out, 0, sizeof(*out));
    if (!req || !req->url || !req->write_fn) {
        snprintf(out->error, sizeof(out->error), "invalid request");
        return;
    }

    wchar_t wurl[2048];
    utf8_to_wide(req->url, wurl, sizeof(wurl) / sizeof(wurl[0]));
    if (wurl[0] == L'\0') {
        snprintf(out->error, sizeof(out->error), "url conversion failed");
        return;
    }

    URL_COMPONENTSW uc;
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize      = sizeof(uc);
    wchar_t host[256] = {0};
    wchar_t path[1024] = {0};
    uc.lpszHostName      = host;
    uc.dwHostNameLength  = sizeof(host) / sizeof(host[0]);
    uc.lpszUrlPath       = path;
    uc.dwUrlPathLength   = sizeof(path) / sizeof(path[0]);

    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) {
        describe_error(GetLastError(), out->error, sizeof(out->error));
        return;
    }
    if (uc.nScheme != INTERNET_SCHEME_HTTPS) {
        snprintf(out->error, sizeof(out->error),
                 "unsupported scheme (only https)");
        return;
    }

    wchar_t ua[128] = L"aperture-http";
    if (req->user_agent && *req->user_agent) {
        utf8_to_wide(req->user_agent, ua, sizeof(ua) / sizeof(ua[0]));
    }

    HINTERNET session = WinHttpOpen(ua,
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS,
                                    0);
    if (!session) {
        describe_error(GetLastError(), out->error, sizeof(out->error));
        return;
    }
    if (req->timeout_s > 0) {
        DWORD t_ms = (DWORD)req->timeout_s * 1000u;
        WinHttpSetTimeouts(session,
                           (int)t_ms,   // resolve
                           (int)t_ms,   // connect
                           (int)t_ms,   // send
                           (int)t_ms);  // receive
    }

    HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0);
    if (!conn) {
        describe_error(GetLastError(), out->error, sizeof(out->error));
        WinHttpCloseHandle(session);
        return;
    }

    HINTERNET request = WinHttpOpenRequest(
        conn, L"GET", path, NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!request) {
        describe_error(GetLastError(), out->error, sizeof(out->error));
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return;
    }

    DWORD redirect_cap = (DWORD)(req->max_redirects > 0
                                 ? req->max_redirects : 0);
    WinHttpSetOption(request,
                     WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS,
                     &redirect_cap, sizeof(redirect_cap));
    DWORD redirect_policy = req->max_redirects > 0
        ? WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS
        : WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(request,
                     WINHTTP_OPTION_REDIRECT_POLICY,
                     &redirect_policy, sizeof(redirect_policy));

    if (!WinHttpSendRequest(request,
                            WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0,
                            0, 0)
        || !WinHttpReceiveResponse(request, NULL)) {
        describe_error(GetLastError(), out->error, sizeof(out->error));
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return;
    }

    DWORD status      = 0;
    DWORD status_sz   = sizeof(status);
    if (!WinHttpQueryHeaders(request,
                             WINHTTP_QUERY_STATUS_CODE
                                 | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             &status, &status_sz, WINHTTP_NO_HEADER_INDEX)) {
        describe_error(GetLastError(), out->error, sizeof(out->error));
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return;
    }
    out->http_status = (long)status;

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request, &avail)) {
            describe_error(GetLastError(), out->error, sizeof(out->error));
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(conn);
            WinHttpCloseHandle(session);
            return;
        }
        if (avail == 0) break;
        char *chunk = (char *)malloc(avail);
        if (!chunk) {
            snprintf(out->error, sizeof(out->error),
                     "out of memory reading body");
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(conn);
            WinHttpCloseHandle(session);
            return;
        }
        DWORD got = 0;
        if (!WinHttpReadData(request, chunk, avail, &got)) {
            describe_error(GetLastError(), out->error, sizeof(out->error));
            free(chunk);
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(conn);
            WinHttpCloseHandle(session);
            return;
        }
        if (got == 0) { free(chunk); break; }
        size_t consumed = req->write_fn(chunk, (size_t)got, req->write_user);
        free(chunk);
        if (consumed != (size_t)got) {
            snprintf(out->error, sizeof(out->error),
                     "write callback rejected %zu/%lu bytes",
                     consumed, (unsigned long)got);
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(conn);
            WinHttpCloseHandle(session);
            return;
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    out->ok = true;
}
