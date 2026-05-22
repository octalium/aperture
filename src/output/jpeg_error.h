#ifndef AP_OUTPUT_JPEG_ERROR_H
#define AP_OUTPUT_JPEG_ERROR_H

// Shared libjpeg error-handling glue: a jpeg_error_mgr extended with a
// setjmp target so a libjpeg fatal error unwinds to the caller instead
// of calling exit(). Used by both the JPEG encoder (output/jpeg.c) and
// the thumbnail decoder (photo/thumbnail.c).

#include "core/log.h"

#include <stdio.h>      // jpeglib.h references FILE without including it
#include <jpeglib.h>
#include <setjmp.h>

typedef struct {
    struct jpeg_error_mgr base;
    jmp_buf               jump;
    const char           *what;   // log-prefix context, e.g. "export"
} ap_jpeg_error;

// libjpeg error_exit replacement: log the formatted message and longjmp
// back to the caller's setjmp point instead of aborting the process.
static inline void ap_jpeg_error_exit(j_common_ptr cinfo)
{
    char msg[JMSG_LENGTH_MAX];
    (*cinfo->err->format_message)(cinfo, msg);
    ap_jpeg_error *err = (ap_jpeg_error *)cinfo->err;
    AP_ERROR("jpeg (%s): %s", err->what, msg);
    longjmp(err->jump, 1);
}

// Wire `err` as a cinfo's error manager with the setjmp-based exit
// handler. `what` is a short context string for diagnostics. Returns
// the manager pointer to assign to `cinfo.err`.
static inline struct jpeg_error_mgr *ap_jpeg_error_install(
    ap_jpeg_error *err, const char *what)
{
    struct jpeg_error_mgr *mgr = jpeg_std_error(&err->base);
    err->base.error_exit = ap_jpeg_error_exit;
    err->what            = what;
    return mgr;
}

#endif
