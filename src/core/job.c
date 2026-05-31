#include "job.h"

#include "core/thread.h"

#include <stdio.h>
#include <stdlib.h>

static ap_mutex g_job_mu   = AP_MUTEX_INITIALIZER;
static ap_job  *g_job_head = NULL;
static uint64_t g_job_next_id = 1;

ap_job *ap_job_begin(ap_job_kind kind, const char *label, int total,
                     void (*on_cancel)(ap_job *self))
{
    ap_job *j = calloc(1, sizeof(*j));
    if (!j) return NULL;

    j->kind = kind;
    snprintf(j->label, sizeof(j->label), "%s", label ? label : "");
    atomic_store(&j->done,  0);
    atomic_store(&j->total, total);
    atomic_store(&j->cancel, 0);
    atomic_store(&j->state, AP_JOB_RUNNING);
    j->on_cancel = on_cancel;
    j->status_id = ap_status_progress_begin(label, total);

    ap_mutex_lock(&g_job_mu);
    j->id = g_job_next_id++;
    if (g_job_next_id == 0) g_job_next_id = 1;
    j->next    = g_job_head;
    g_job_head = j;
    ap_mutex_unlock(&g_job_mu);

    return j;
}

void ap_job_progress(ap_job *j, int done, int total)
{
    if (!j) return;
    atomic_store(&j->done, done);
    if (total > 0) atomic_store(&j->total, total);
    ap_status_progress_update(j->status_id, done, total);
}

void ap_job_set_state(ap_job *j, ap_job_state s)
{
    if (!j) return;
    atomic_store(&j->state, (int)s);
}

void ap_job_finish(ap_job *j, ap_job_state terminal)
{
    if (!j) return;
    atomic_store(&j->state, (int)terminal);
    ap_status_progress_finish(j->status_id, terminal == AP_JOB_DONE);

    ap_mutex_lock(&g_job_mu);
    ap_job **link = &g_job_head;
    while (*link) {
        if (*link == j) {
            *link = j->next;
            break;
        }
        link = &(*link)->next;
    }
    j->next = NULL;
    ap_mutex_unlock(&g_job_mu);
}

void ap_job_request_cancel(ap_job *j)
{
    if (!j) return;
    atomic_store(&j->cancel, 1);
    atomic_store(&j->state, AP_JOB_CANCELING);
}

bool ap_job_cancel_requested(const ap_job *j)
{
    if (!j) return false;
    return atomic_load(&j->cancel) != 0;
}

void ap_job_request_cancel_by_id(uint64_t id)
{
    if (id == 0) return;
    ap_mutex_lock(&g_job_mu);
    for (ap_job *j = g_job_head; j; j = j->next) {
        if (j->id == id) {
            atomic_store(&j->cancel, 1);
            atomic_store(&j->state, AP_JOB_CANCELING);
            break;
        }
    }
    ap_mutex_unlock(&g_job_mu);
}

int ap_job_snapshot(ap_job_view *out, int max)
{
    if (!out || max <= 0) return 0;
    int n = 0;
    ap_mutex_lock(&g_job_mu);
    for (ap_job *j = g_job_head; j && n < max; j = j->next) {
        ap_job_view *v = &out[n++];
        v->id    = j->id;
        v->kind  = j->kind;
        snprintf(v->label, sizeof(v->label), "%s", j->label);
        v->done  = atomic_load(&j->done);
        v->total = atomic_load(&j->total);
        v->state = (ap_job_state)atomic_load(&j->state);
    }
    ap_mutex_unlock(&g_job_mu);
    return n;
}
