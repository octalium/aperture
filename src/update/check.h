#ifndef APERTURE_UPDATE_CHECK_H
#define APERTURE_UPDATE_CHECK_H

#include "core/worker.h"
#include "update/manifest.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Async version-check job. Submitted to the worker pool by
// ap_update_check_submit; the run function fetches and parses the
// manifest off the main thread. The main thread inspects `ok`,
// `manifest`, and `newer` after the job lands via the standard
// drain path.
typedef struct {
    ap_work_item base;
    char         url[1024];
    char         current_version[64];
    ap_manifest  manifest;
    bool         ok;
    bool         newer;
    char         error[256];
} ap_update_check_job;

// Default manifest URL: the always-latest tag's `releases.json` asset
// on github.com/octalium/aperture. Override at runtime via the
// `update.manifest_url` app setting if desired.
#define AP_UPDATE_DEFAULT_MANIFEST_URL \
    "https://github.com/octalium/aperture/releases/download/latest/releases.json"

// Allocate and submit a check job. `url` may be NULL to use the
// default; `current_version` is the running build version (typically
// AP_VERSION_STRING). The job is dispatched on `pool` and its
// completion is picked up by the standard worker-pool poll path.
// Returns 0 on success, -1 on allocation failure.
int ap_update_check_submit(ap_worker_pool *pool,
                           const char     *url,
                           const char     *current_version);

// Run function — bound to ap_work_item->run when the job is dispatched.
// Exposed so the app's completion dispatcher can recognise the job type.
void ap_update_check_run(ap_work_item *self);

#ifdef __cplusplus
}
#endif

#endif
