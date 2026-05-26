#ifndef APERTURE_CORE_WORKER_H
#define APERTURE_CORE_WORKER_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ap_worker_pool ap_worker_pool;
typedef struct ap_work_item   ap_work_item;

// One unit of work. Subtypes embed this as their first member and
// cast back inside `run`. Heap-allocated by the submitter; ownership
// transfers to the pool until ap_worker_pool_poll returns it.
//
// `run` executes on a worker thread - must NOT touch the GPU, the
// main-thread state of any subsystem, or other shared state without
// its own synchronization. Output goes into fields on the embedding
// subtype, which the main thread reads after polling.
struct ap_work_item {
    void          (*run)(ap_work_item *self);
    ap_work_item   *next;  // internal - pool's linked-list link
};

// Create a pool of `n_threads` worker threads. If n_threads <= 0 the
// pool sizes itself to min(4, hardware_concurrency).
ap_worker_pool *ap_worker_pool_create(int n_threads);

// Wait for in-flight items to finish, drain queues (any items
// remaining are leaked - caller must drain via poll first), and join
// threads.
void            ap_worker_pool_destroy(ap_worker_pool *pool);

// Submit an item. Thread-safe. Pool takes ownership until poll.
void            ap_worker_pool_submit(ap_worker_pool *pool, ap_work_item *item);

// Pop one completed item, or NULL if none ready. Non-blocking. The
// caller owns the returned pointer and is responsible for freeing
// (or recycling) it.
ap_work_item   *ap_worker_pool_poll(ap_worker_pool *pool);

// Block until every submitted item has finished running. Items stay
// on the completed queue - the caller still needs to drain them via
// poll. Use this before tearing down state any in-flight items might
// observe (even though `run` itself shouldn't touch shared state, the
// caller may have other invariants to preserve).
void            ap_worker_pool_wait_idle(ap_worker_pool *pool);

#ifdef __cplusplus
}
#endif

#endif
