#ifndef APERTURE_CORE_JOB_H
#define APERTURE_CORE_JOB_H

/*
 * ap_job — the unified background-job descriptor + process-global
 * registry. A job is the unit the user sees and cancels; it is
 * separate from ap_work_item (the unit the worker pool runs), because
 * one job may map to many work items (the export coordinator submits
 * one encode item per photo but is one user-visible job) or to none at
 * a given instant (a main-thread coordinator between frames).
 *
 * The registry mirrors ap_status's locking idiom: a process-global
 * mutex-guarded linked list. The UI never holds a registry pointer —
 * it reads value-copied snapshots (ap_job_snapshot) and cancels by id
 * (ap_job_request_cancel_by_id), so a job freed between snapshot and
 * click is a safe no-op.
 */

#include "ui/status.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Upper bound on jobs a single ap_job_snapshot call copies out. The
// registry list itself is unbounded; this only sizes the UI's stack
// buffer.
#define AP_JOB_MAX 64

// Lifecycle state of a job. Advisory for display; the authoritative
// terminal transition happens once, in the completion handler on the
// main thread.
typedef enum {
    AP_JOB_PENDING   = 0,  // registered, not yet started
    AP_JOB_RUNNING   = 1,
    AP_JOB_CANCELING = 2,  // cancel requested, not yet acknowledged/drained
    AP_JOB_DONE      = 3,  // finished ok
    AP_JOB_FAILED    = 4,
    AP_JOB_CANCELED  = 5,  // finished after cancel
} ap_job_state;

// Coarse classification of a job, used by the UI for grouping/labels.
typedef enum {
    AP_JOB_KIND_IMPORT = 0,
    AP_JOB_KIND_EXPORT,         // GPU-coordinated export (single/batch/quick)
    AP_JOB_KIND_PHOTO_OPEN,
    AP_JOB_KIND_SELECTION_EDIT, // sidecar-batch ops over a selection
    AP_JOB_KIND_LIBRARY,        // sort / rescan / reload / delete
    AP_JOB_KIND_THUMB,          // aggregate thumbnail backlog (optional)
} ap_job_kind;

typedef struct ap_job ap_job;

// The job control block. The owning subsystem frees it in its
// completion handler after ap_job_finish unlinks it from the registry;
// the registry never frees a job. Progress/cancel/state fields are
// atomics so workers update them through a back-pointer without locking.
struct ap_job {
    uint64_t        id;            // monotonic, registry-assigned, never 0
    ap_job_kind     kind;
    char            label[128];    // user-facing, mirrors into ap_status

    _Atomic int     done;          // progress numerator (worker writes)
    _Atomic int     total;         // 0 => indeterminate (worker writes)
    _Atomic int     cancel;        // 0/1; the ONE cancel flag for all kinds
    _Atomic int     state;         // ap_job_state

    ap_status_id    status_id;     // the visual bar this job drives
    void          (*on_cancel)(ap_job *self); // optional extra teardown hook
    ap_job         *next;          // registry linked-list link (registry-owned)
};

// Flat value-copy of a job for the UI. Holds no pointer into the
// registry, so it is safe to retain across frames.
typedef struct {
    uint64_t     id;
    ap_job_kind  kind;
    char         label[128];
    int          done;
    int          total;
    ap_job_state state;
} ap_job_view;

// Allocate + register a job, assign it a non-zero id, open a status bar
// (label/total mirror ap_status_progress_begin), and return it in the
// RUNNING state. `on_cancel` is optional extra teardown run by the
// completion handler on cancel. Returns NULL on allocation failure.
ap_job *ap_job_begin(ap_job_kind kind, const char *label, int total,
                     void (*on_cancel)(ap_job *self));

// Store the progress atomics and forward to the job's status bar.
// `total <= 0` leaves the stored total unchanged. Thread-safe (worker
// side); no lock taken.
void ap_job_progress(ap_job *j, int done, int total);

// Set the job's advisory state atomic. Thread-safe.
void ap_job_set_state(ap_job *j, ap_job_state s);

// Mark the job terminal: finish its status bar (DONE => green,
// CANCELED/FAILED => red) and unlink it from the registry so snapshots
// stop showing it. The block itself is NOT freed — the owning
// subsystem frees it after this call. Main thread only.
void ap_job_finish(ap_job *j, ap_job_state terminal);

// Request cancellation: set the cancel atomic and move the job to
// CANCELING. Thread-safe.
void ap_job_request_cancel(ap_job *j);

// Worker-side poll of the cancel flag. Thread-safe.
bool ap_job_cancel_requested(const ap_job *j);

// Look up a job by id under the registry lock and request cancel. A
// no-op if the id is no longer present (job already finished/freed),
// so the UI may hold a stale id across frames safely. Main thread.
void ap_job_request_cancel_by_id(uint64_t id);

// Value-copy up to `max` live jobs into `out` under the registry lock.
// Returns the number written. The UI never holds a registry pointer.
int ap_job_snapshot(ap_job_view *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* APERTURE_CORE_JOB_H */
