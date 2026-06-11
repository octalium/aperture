#ifdef __linux__
#define _GNU_SOURCE   // sched_getaffinity
#endif

#include "worker.h"

#include "core/log.h"
#include "core/thread.h"

#include <stdbool.h>
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>  // GetSystemInfo for the default thread count
#else
#include <unistd.h>   // sysconf(_SC_NPROCESSORS_ONLN)
#ifdef __linux__
#include <sched.h>    // sched_getaffinity, CPU_COUNT
#endif
#endif

// fallback core count when the OS query fails; otherwise the pool
// auto-sizes to cores - 1 (decode/encode scale with cores, one core
// stays free for the main thread).
#define WORKER_FALLBACK_CORES 4
#define WORKER_MIN_THREADS    2

struct ap_worker_pool {
    ap_thread      *threads;
    int             n_threads;

    ap_mutex        mu;
    ap_cond         pending_cv;
    ap_cond         idle_cv;

    ap_work_item   *pending_head;
    ap_work_item   *pending_tail;

    ap_work_item   *completed_head;
    ap_work_item   *completed_tail;

    int             active_count;  // pending + running
    bool            shutdown;
};

// number of logical CPUs available to this process, or 0 if it cannot
// be determined. on linux the affinity mask is queried first so a
// taskset/cgroup-restricted process doesn't oversubscribe its quota.
static int cpu_count(void)
{
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
#else
#ifdef __linux__
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        int n = CPU_COUNT(&set);
        if (n > 0) return n;
    }
#endif
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    return online > 0 ? (int)online : 0;
#endif
}

static void enqueue(ap_work_item **head, ap_work_item **tail, ap_work_item *item)
{
    item->next = NULL;
    if (*tail) {
        (*tail)->next = item;
        *tail = item;
    } else {
        *head = *tail = item;
    }
}

static ap_work_item *dequeue(ap_work_item **head, ap_work_item **tail)
{
    ap_work_item *item = *head;
    if (!item) return NULL;
    *head = item->next;
    if (!*head) *tail = NULL;
    item->next = NULL;
    return item;
}

static void *worker_main(void *arg)
{
    ap_worker_pool *p = arg;
    for (;;) {
        ap_mutex_lock(&p->mu);
        while (!p->shutdown && !p->pending_head) {
            ap_cond_wait(&p->pending_cv, &p->mu);
        }
        if (p->shutdown && !p->pending_head) {
            ap_mutex_unlock(&p->mu);
            return NULL;
        }
        ap_work_item *item = dequeue(&p->pending_head, &p->pending_tail);
        ap_mutex_unlock(&p->mu);

        if (item && item->run) {
            item->run(item);
        }

        ap_mutex_lock(&p->mu);
        enqueue(&p->completed_head, &p->completed_tail, item);
        if (--p->active_count == 0) {
            ap_cond_broadcast(&p->idle_cv);
        }
        ap_mutex_unlock(&p->mu);
    }
}

ap_worker_pool *ap_worker_pool_create(int n_threads)
{
    if (n_threads <= 0) {
        int online = cpu_count();
        if (online <= 0) online = WORKER_FALLBACK_CORES;
        n_threads = online - 1;
        if (n_threads < WORKER_MIN_THREADS) n_threads = WORKER_MIN_THREADS;
    }

    ap_worker_pool *p = calloc(1, sizeof(*p));
    if (!p) {
        AP_ERROR("worker: out of memory");
        return NULL;
    }
    p->n_threads = n_threads;
    p->threads   = calloc((size_t)n_threads, sizeof(*p->threads));
    if (!p->threads) {
        AP_ERROR("worker: thread array alloc failed");
        free(p);
        return NULL;
    }

    if (ap_mutex_init(&p->mu) != 0) {
        AP_ERROR("worker: mutex init failed");
        free(p->threads);
        free(p);
        return NULL;
    }
    if (ap_cond_init(&p->pending_cv) != 0) {
        AP_ERROR("worker: pending cond init failed");
        ap_mutex_destroy(&p->mu);
        free(p->threads);
        free(p);
        return NULL;
    }
    if (ap_cond_init(&p->idle_cv) != 0) {
        AP_ERROR("worker: idle cond init failed");
        ap_cond_destroy(&p->pending_cv);
        ap_mutex_destroy(&p->mu);
        free(p->threads);
        free(p);
        return NULL;
    }

    for (int i = 0; i < n_threads; i++) {
        if (ap_thread_create(&p->threads[i], worker_main, p) != 0) {
            AP_ERROR("worker: thread create failed at idx %d", i);
            // Best-effort tear-down: signal shutdown, join the threads
            // that did spawn.
            ap_mutex_lock(&p->mu);
            p->shutdown = true;
            ap_cond_broadcast(&p->pending_cv);
            ap_mutex_unlock(&p->mu);
            for (int j = 0; j < i; j++) ap_thread_join(p->threads[j]);
            ap_cond_destroy(&p->idle_cv);
            ap_cond_destroy(&p->pending_cv);
            ap_mutex_destroy(&p->mu);
            free(p->threads);
            free(p);
            return NULL;
        }
    }
    return p;
}

void ap_worker_pool_destroy(ap_worker_pool *p)
{
    if (!p) return;
    ap_mutex_lock(&p->mu);
    p->shutdown = true;
    ap_cond_broadcast(&p->pending_cv);
    ap_mutex_unlock(&p->mu);
    for (int i = 0; i < p->n_threads; i++) ap_thread_join(p->threads[i]);
    ap_cond_destroy(&p->idle_cv);
    ap_cond_destroy(&p->pending_cv);
    ap_mutex_destroy(&p->mu);
    free(p->threads);
    free(p);
}

int ap_worker_pool_thread_count(const ap_worker_pool *p)
{
    return p ? p->n_threads : 0;
}

void ap_worker_pool_submit(ap_worker_pool *p, ap_work_item *item)
{
    if (!p || !item) return;
    ap_mutex_lock(&p->mu);
    p->active_count++;
    enqueue(&p->pending_head, &p->pending_tail, item);
    ap_cond_signal(&p->pending_cv);
    ap_mutex_unlock(&p->mu);
}

ap_work_item *ap_worker_pool_poll(ap_worker_pool *p)
{
    if (!p) return NULL;
    ap_mutex_lock(&p->mu);
    ap_work_item *item = dequeue(&p->completed_head, &p->completed_tail);
    ap_mutex_unlock(&p->mu);
    return item;
}

void ap_worker_pool_wait_idle(ap_worker_pool *p)
{
    if (!p) return;
    ap_mutex_lock(&p->mu);
    while (p->active_count > 0) {
        ap_cond_wait(&p->idle_cv, &p->mu);
    }
    ap_mutex_unlock(&p->mu);
}
