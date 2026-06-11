#ifndef APERTURE_APP_JOBS_H
#define APERTURE_APP_JOBS_H

/*
 * Worker-job types and the run/completion/drain helpers used by app.c.
 * All symbols are internal to src/app; this header is not part of the
 * public API.
 */

#include "app_priv.h"
#include "core/worker.h"

#include <stdatomic.h>
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
    // When set, this is a background raw decode owned by the export
    // coordinator (not an interactive open): completion hands the decoded
    // raw to the coordinator (ap_export_coord_decode_complete) instead of
    // installing it as app->photo. coord_item is the export item index.
    bool         from_coord;
    int          coord_item;
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
    // When true, the export coordinator owns this encode: completion
    // accounts rgba_bytes back to it (and drives no standalone status
    // bar — the coordinator's job owns the progress surface).
    bool         from_coord;
    size_t       rgba_bytes;
} export_job;

typedef struct {
    ap_work_item   base;
    uint8_t       *rgba;
    int            width, height;
    int            idx;
    uint64_t       gen;        // thumb_load_gen at submit; stale => skip store
    unsigned char *jpeg;
    size_t         jpeg_size;
    int            ok;
} thumb_encode_job;

typedef struct import_job {
    ap_work_item        base;
    char                lib_root[4096];
    char                db_path[4096];
    char                src_dir[4096];
    ap_import_settings  settings;
    ap_import_report    report;
    int                 ok;
    // The unified job control block (label / progress / cancel). The
    // worker polls job->cancel between files via the progress callback;
    // the main thread flips it through ap_job_request_cancel_by_id.
    ap_job             *job;
} import_job;

// Which worker-safe sidecar op a selection_edit_job runs. Each maps to
// a pure path-based ap_library_* per-item function (no GPU, no in-memory
// library cache mutation, no per-library db handle). PIPELINE is
// resolved to a concrete edit stack at submit on the main thread and
// runs as STACK on the worker. The only sqlite a worker can reach is
// the shared registry connection, via the default-stack seed for photos
// without a sidecar — safe because that handle is opened serialized
// (FULLMUTEX, see registry_get in library.c).
typedef enum {
    AP_SEL_EDIT_PIPELINE = 0,  // apply a pipeline to each photo's stack
    AP_SEL_EDIT_STACK,         // write a copied edit stack to each photo
    AP_SEL_EDIT_METADATA,      // merge a metadata patch into each sidecar
    AP_SEL_EDIT_LENS,          // attach a lens override to matching photos
    AP_SEL_EDIT_CULLING,       // write a per-photo culling struct to each sidecar
    AP_SEL_EDIT_GROUP,         // add/remove one group on each photo's sidecar
} ap_sel_edit_op;

// Whether an op changes the rendered pixels (so its thumbnails must be
// re-decoded at completion). Culling + group are metadata-only.
static inline bool ap_sel_edit_changes_render(ap_sel_edit_op op)
{
    return op != AP_SEL_EDIT_CULLING && op != AP_SEL_EDIT_GROUP;
}

// Background batch over a snapshot of the selected photos. Both the
// library indices and their resolved absolute paths are snapshotted on
// the main thread at submit; the worker runs the per-item sidecar op
// against `paths` ONLY — it never dereferences the library, so a
// concurrent main-thread library reload (e.g. the import-completion
// rescan, which does not drain the pool) cannot free state out from
// under it. The indices are kept solely for the main-thread completion
// handler, which invalidates the touched thumbnails (re-validating
// index + generation first).
typedef struct {
    ap_work_item    base;
    ap_job         *job;
    ap_sel_edit_op  op;
    int            *indices;       // snapshot of selected library indices
    char          (*paths)[4096];  // resolved abs path per index; worker reads these
    bool           *ok;            // per-photo write success; completion
                                   // reconciles cache/db only where set
    int             count;
    _Atomic int     wrote;         // photos successfully written
    _Atomic int     processed;     // photos the worker iterated (< count on cancel)
    uint64_t        thumb_gen;     // generation captured at submit

    // op payloads (only the active op's fields are meaningful)
    ap_edit_stack   stack;                       // STACK + resolved PIPELINE
    ap_photo_metadata patch;                     // METADATA
    bool            patch_set[AP_META_FIELD_COUNT]; // METADATA
    char            match_exif_lens[AP_META_VALUE_LEN]; // LENS
    char            override_lens[AP_META_VALUE_LEN];   // LENS
    int             lens_slot;                          // LENS str-param slot
    ap_photo_culling *cull;                      // CULLING: per-photo, count entries
    char            group_name[AP_GROUP_NAME_LEN]; // GROUP
    bool            group_add;                     // GROUP
} selection_edit_job;

// Background structural library op: sort (RELOAD) / rescan / delete.
// The worker builds a full replacement ap_library_cache off its own
// sqlite connection (ap_library_cache_build); the main-thread
// completion drains the pool, waits for GPU idle, then atomically swaps
// it into the library and rebuilds the grid. DELETE carries the rel
// paths to remove + the grid cell to land on afterward.
typedef struct {
    ap_work_item      base;
    ap_job           *job;
    ap_library_op     op;
    ap_library_sort   sort;
    char              root[4096];
    char            (*del_rel)[4096];  // DELETE: rel paths (owned), del_count
    int               del_count;
    int               anchor_cell;     // DELETE: grid cell to select at done
    ap_library_cache *result;          // built cache; NULL on cancel/fail
    _Atomic int       ok;
} library_job;

void submit_import_job(ap_app *app, const char *lib_root, const char *src_dir,
                       const ap_import_settings *settings);

// Begin + submit a background library job. Single-flight against both
// library jobs and selection-edit jobs (a 2nd is rejected with a toast,
// so structural rebuilds never overlap a sidecar batch). Takes
// ownership of `del_rel` (freed on submit failure / at completion).
// Returns 0 on submit, -1 on rejection or allocation failure.
int submit_library_job(ap_app *app, ap_library_op op, ap_library_sort sort,
                       char (*del_rel)[4096], int del_count, int anchor_cell,
                       const char *label);

// Allocate a selection_edit_job and snapshot the current grid
// selection's library indices + resolved absolute paths into it. When
// `skip_open_photo` is true the currently-open photo is excluded (ops
// that rewrite the edit stack must skip it, else its in-memory stack
// goes stale). Sets op + thumb generation. Returns NULL on no library /
// empty selection / allocation failure / single-flight rejection; when
// `out_busy` is non-NULL it is set true only for the single-flight
// rejection, so callers can distinguish "already running" from "nothing
// to do". The job is NOT yet submitted: the caller fills the op-specific
// payload, then calls commit_selection_edit_job, which begins the ap_job
// and submits. This two-step keeps the worker from observing partial
// payload.
selection_edit_job *build_selection_edit_job(ap_app *app, ap_sel_edit_op op,
                                             bool skip_open_photo,
                                             bool *out_busy);

// Free a built-but-not-committed selection_edit_job (its ap_job has not
// been begun, so the single-flight guard is still clear). Use this when
// a caller fails to fill the op payload after build_selection_edit_job.
void abandon_selection_edit_job(selection_edit_job *j);

// Begin the job's ap_job (label/progress/cancel) and submit it to the
// worker pool. Takes ownership; on ap_job_begin failure it frees the
// job and returns -1. Returns the queued photo count on success.
int commit_selection_edit_job(ap_app *app, selection_edit_job *j,
                              const char *label);

void discard_completed_item(ap_app *app, ap_work_item *it);
void drain_all_workers(ap_app *app);

// Retire completed worker items on the main thread, looping until the
// completion queue is empty or a per-frame wall-time budget (~3ms) is
// hit. A library-swap completion ends the drain early: it stalls the
// GPU and invalidates the thumbnail generation, so anything still
// queued is re-validated next frame. Call once per frame.
void drain_completed_jobs(ap_app *app);
void submit_pending_thumbs(ap_app *app);
// Read back the open photo's rendered pixels and refresh its library
// thumbnail on a worker. Resolves the library index from the photo's
// path at submit time (a cached index can be stale mid-navigation).
void submit_thumb_refresh(ap_app *app);
void toggle_rendered_thumbnails(ap_app *app);

#endif /* APERTURE_APP_JOBS_H */
