#ifndef APERTURE_APP_JOBS_H
#define APERTURE_APP_JOBS_H

/*
 * Worker-job types and the run/completion/drain helpers used by app.c.
 * All symbols are internal to src/app; this header is not part of the
 * public API.
 */

#include "app_priv.h"
#include "core/worker.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    ap_work_item   base;
    char           path[4096];
    int            idx;
    unsigned char *cache_jpeg;
    size_t         cache_jpeg_size;
    uint64_t       gen;
    uint8_t       *rgba;
    int            w, h;
    int            ok;
} thumb_job;

typedef struct photo_open_job {
    ap_work_item base;
    char         path[4096];
    ap_raw_image raw;
    uint64_t     gen;
    ap_status_id status_id;
    int          ok;
} photo_open_job;

typedef struct {
    ap_work_item base;
    uint8_t     *rgba;
    int          width, height;
    int          format;
    int          jpeg_quality;
    int          png_depth;
    int          tiff_depth;
    int          tiff_compress;
    char         out_path[4096];
    ap_status_id status_id;
    int          ok;
} export_job;

typedef struct {
    ap_work_item   base;
    uint8_t       *rgba;
    int            width, height;
    int            idx;
    unsigned char *jpeg;
    size_t         jpeg_size;
    int            ok;
} thumb_encode_job;

typedef struct {
    ap_work_item        base;
    char                lib_root[4096];
    char                src_dir[4096];
    ap_import_settings  settings;
    ap_status_id        status_id;
    int                 imported;
    int                 ok;
} import_job;

void submit_import_job(ap_app *app, const char *lib_root, const char *src_dir,
                       const ap_import_settings *settings);

void discard_completed_item(ap_app *app, ap_work_item *it);
void drain_all_workers(ap_app *app);
void drain_one_completed_job(ap_app *app);
void submit_pending_thumbs(ap_app *app);
void submit_thumb_refresh(ap_app *app, int idx);
void toggle_rendered_thumbnails(ap_app *app);

#endif /* APERTURE_APP_JOBS_H */
