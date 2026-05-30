#ifndef APERTURE_UI_TOAST_H
#define APERTURE_UI_TOAST_H

#include "core/compat.h"  // AP_PRINTF_FMT

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AP_TOAST_INFO  = 0,
    AP_TOAST_ERROR = 1,
} ap_toast_kind;

// Push a transient notification onto the toast queue. Messages are
// shown as fading cards in the bottom-right corner of the window.
// Silently discards the oldest entry when the queue is full.
void ap_toast_push(ap_toast_kind kind, const char *fmt, ...)
    AP_PRINTF_FMT(2, 3);

// Render the current toast stack.  Call once per frame from
// ap_app_run_frame, after all panels have been drawn.
void ap_toast_draw(void);

#ifdef __cplusplus
}
#endif

#endif
