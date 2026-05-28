#include "net/http.h"

#import <Foundation/Foundation.h>

#include <stdio.h>
#include <string.h>

// HTTPS GET via NSURLSession. cert verification and redirect handling
// are driven by the platform default policy. blocks until completion
// via a dispatch semaphore so the caller sees synchronous semantics.

void ap_http_get(const ap_http_request *req, ap_http_response *out)
{
    memset(out, 0, sizeof(*out));
    if (!req || !req->url || !req->write_fn) {
        snprintf(out->error, sizeof(out->error), "invalid request");
        return;
    }

    @autoreleasepool {
        NSString *url_str = [NSString stringWithUTF8String:req->url];
        NSURL    *url     = [NSURL URLWithString:url_str];
        if (!url) {
            snprintf(out->error, sizeof(out->error),
                     "invalid url: %s", req->url);
            return;
        }

        NSMutableURLRequest *r = [NSMutableURLRequest requestWithURL:url];
        r.HTTPMethod = @"GET";
        if (req->user_agent && *req->user_agent) {
            NSString *ua = [NSString stringWithUTF8String:req->user_agent];
            [r setValue:ua forHTTPHeaderField:@"User-Agent"];
        }
        if (req->timeout_s > 0) {
            r.timeoutInterval = (NSTimeInterval)req->timeout_s;
        }

        NSURLSessionConfiguration *cfg =
            [NSURLSessionConfiguration ephemeralSessionConfiguration];
        if (req->timeout_s > 0) {
            cfg.timeoutIntervalForRequest  = (NSTimeInterval)req->timeout_s;
            cfg.timeoutIntervalForResource = (NSTimeInterval)req->timeout_s;
        }
        cfg.HTTPShouldUsePipelining = NO;

        NSURLSession *session =
            [NSURLSession sessionWithConfiguration:cfg];

        __block long      status      = 0;
        __block BOOL      ok          = NO;
        __block NSData   *body        = nil;
        __block NSString *err_msg     = nil;

        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        NSURLSessionDataTask *task = [session
            dataTaskWithRequest:r
              completionHandler:^(NSData * _Nullable data,
                                  NSURLResponse * _Nullable response,
                                  NSError * _Nullable error) {
                if (error) {
                    err_msg = error.localizedDescription;
                } else if ([response isKindOfClass:[NSHTTPURLResponse class]]) {
                    NSHTTPURLResponse *http = (NSHTTPURLResponse *)response;
                    status = (long)http.statusCode;
                    body   = data;
                    ok     = YES;
                } else {
                    err_msg = @"unexpected non-HTTP response";
                }
                dispatch_semaphore_signal(sem);
            }];
        [task resume];
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
        [session finishTasksAndInvalidate];

        if (!ok) {
            const char *cstr = err_msg ? err_msg.UTF8String : "unknown error";
            snprintf(out->error, sizeof(out->error),
                     "nsurlsession: %s", cstr ? cstr : "unknown");
            return;
        }
        out->http_status = status;

        const char *bytes = body ? (const char *)body.bytes : NULL;
        size_t      len   = body ? (size_t)body.length      : 0;
        if (len > 0 && bytes) {
            size_t consumed = req->write_fn(bytes, len, req->write_user);
            if (consumed != len) {
                snprintf(out->error, sizeof(out->error),
                         "write callback rejected %zu/%zu bytes",
                         consumed, len);
                return;
            }
        }
        out->ok = true;
    }
}
