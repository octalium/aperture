#define _GNU_SOURCE  // localtime_r under -std=c17

#include "log.h"

#include "core/compat.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static const char *level_names[] = {
    [AP_LOG_INFO]  = "info",
    [AP_LOG_WARN]  = "warn",
    [AP_LOG_ERROR] = "error",
    [AP_LOG_FATAL] = "fatal",
};

void ap_log(ap_log_level level, const char *fmt, ...)
{
    FILE *out = (level <= AP_LOG_INFO) ? stdout : stderr;

    struct timespec ts = {0};
    struct tm tm = {0};
    timespec_get(&ts, TIME_UTC);
    localtime_r(&ts.tv_sec, &tm);

    // format the whole line into one buffer and emit it with a single
    // fwrite so concurrent logging never interleaves mid-line.
    char line[2048];
    size_t end = (size_t)snprintf(line, sizeof(line),
                                  "%02d:%02d:%02d.%03ld [%s] ",
                                  tm.tm_hour, tm.tm_min, tm.tm_sec,
                                  ts.tv_nsec / 1000000L,
                                  level_names[level]);

    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(line + end, sizeof(line) - end, fmt, args);
    va_end(args);
    if (n > 0) end += (size_t)n;
    if (end > sizeof(line) - 1) end = sizeof(line) - 1;
    line[end++] = '\n';

    fwrite(line, 1, end, out);
    fflush(out);

    if (level == AP_LOG_FATAL) {
        abort();
    }
}
