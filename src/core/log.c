#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static const char *level_names[] = {
    [AP_LOG_INFO]  = "info",
    [AP_LOG_WARN]  = "warn",
    [AP_LOG_ERROR] = "error",
    [AP_LOG_FATAL] = "fatal",
};

void ap_log(ap_log_level level, const char *fmt, ...)
{
    FILE *out = (level <= AP_LOG_INFO) ? stdout : stderr;

    fprintf(out, "[%s] ", level_names[level]);

    va_list args;
    va_start(args, fmt);
    vfprintf(out, fmt, args);
    va_end(args);

    fputc('\n', out);
    fflush(out);

    if (level == AP_LOG_FATAL) {
        abort();
    }
}
