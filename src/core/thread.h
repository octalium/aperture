#ifndef APERTURE_CORE_THREAD_H
#define APERTURE_CORE_THREAD_H

// portable threading: mutex, condition variable, and joinable thread.
// the POSIX backend is a thin pthreads wrapper (byte-identical to the
// code aperture shipped before the port); the windows backend uses
// SRWLOCK + CONDITION_VARIABLE + CreateThread. the surface is exactly
// what the thread pool (src/core/worker.c) and the status overlay
// (src/ui/status.c) need — no timed waits, no recursive locks.

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
typedef SRWLOCK            ap_mutex;
typedef CONDITION_VARIABLE ap_cond;
typedef HANDLE             ap_thread;
// SRWLOCK supports static init, so a file-scope mutex needs no runtime
// setup (mirrors pthread's PTHREAD_MUTEX_INITIALIZER).
#define AP_MUTEX_INITIALIZER SRWLOCK_INIT
#else
typedef pthread_mutex_t ap_mutex;
typedef pthread_cond_t  ap_cond;
typedef pthread_t       ap_thread;
#define AP_MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER
#endif

// thread entry point. matches the pthread/Win32 shape closely enough
// that both backends adapt to it without an extra trampoline alloc.
typedef void *(*ap_thread_fn)(void *arg);

// mutex. init/destroy are required for heap-allocated mutexes; a
// file-scope mutex declared with AP_MUTEX_INITIALIZER may skip both.
// returns 0 on success, non-zero on failure.
int  ap_mutex_init(ap_mutex *m);
void ap_mutex_destroy(ap_mutex *m);
void ap_mutex_lock(ap_mutex *m);
void ap_mutex_unlock(ap_mutex *m);

// condition variable, always paired with a held mutex. returns 0 on
// success, non-zero on failure for init.
int  ap_cond_init(ap_cond *c);
void ap_cond_destroy(ap_cond *c);
// atomically release `m`, block until signalled, then re-acquire `m`.
void ap_cond_wait(ap_cond *c, ap_mutex *m);
void ap_cond_signal(ap_cond *c);
void ap_cond_broadcast(ap_cond *c);

// spawn a joinable thread running `fn(arg)`. writes the handle into
// `*t`. returns 0 on success, non-zero on failure.
int  ap_thread_create(ap_thread *t, ap_thread_fn fn, void *arg);
// block until `t` finishes; its return value is discarded.
void ap_thread_join(ap_thread t);

#ifdef __cplusplus
}
#endif

#endif
