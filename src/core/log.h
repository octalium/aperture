#ifndef APERTURE_CORE_LOG_H
#define APERTURE_CORE_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AP_LOG_INFO,
    AP_LOG_WARN,
    AP_LOG_ERROR,
    AP_LOG_FATAL,
} ap_log_level;

void ap_log(ap_log_level level, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#define AP_INFO(...)  ap_log(AP_LOG_INFO,  __VA_ARGS__)
#define AP_WARN(...)  ap_log(AP_LOG_WARN,  __VA_ARGS__)
#define AP_ERROR(...) ap_log(AP_LOG_ERROR, __VA_ARGS__)
#define AP_FATAL(...) ap_log(AP_LOG_FATAL, __VA_ARGS__)

#endif
