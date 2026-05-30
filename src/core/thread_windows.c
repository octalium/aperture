#include "core/thread.h"

#include <stdlib.h>

// SRWLOCK and CONDITION_VARIABLE are zero-initialised lightweight
// objects with no OS handle to release, so init just clears them and
// destroy is a no-op. exclusive (write) acquisition gives the same
// mutual-exclusion semantics aperture's pthread mutexes had.

int ap_mutex_init(ap_mutex *m)
{
    InitializeSRWLock(m);
    return 0;
}

void ap_mutex_destroy(ap_mutex *m)
{
    (void)m;
}

void ap_mutex_lock(ap_mutex *m)
{
    AcquireSRWLockExclusive(m);
}

void ap_mutex_unlock(ap_mutex *m)
{
    ReleaseSRWLockExclusive(m);
}

int ap_cond_init(ap_cond *c)
{
    InitializeConditionVariable(c);
    return 0;
}

void ap_cond_destroy(ap_cond *c)
{
    (void)c;
}

void ap_cond_wait(ap_cond *c, ap_mutex *m)
{
    // INFINITE timeout matches pthread_cond_wait; the SRW flag pairs
    // with the exclusive acquisition used by ap_mutex_lock. spurious
    // wakeups are handled by the callers' predicate loops.
    SleepConditionVariableSRW(c, m, INFINITE, 0);
}

void ap_cond_signal(ap_cond *c)
{
    WakeConditionVariable(c);
}

void ap_cond_broadcast(ap_cond *c)
{
    WakeAllConditionVariable(c);
}

// Win32 thread procs return DWORD via __stdcall, but aperture's entry
// points use the pthread-shaped void *(*)(void *). a small heap-owned
// trampoline adapts the signature; the thread frees it on entry.
typedef struct {
    ap_thread_fn fn;
    void        *arg;
} thread_trampoline;

static DWORD WINAPI thread_entry(LPVOID param)
{
    thread_trampoline *t = param;
    ap_thread_fn fn  = t->fn;
    void        *arg = t->arg;
    free(t);
    fn(arg);
    return 0;
}

int ap_thread_create(ap_thread *t, ap_thread_fn fn, void *arg)
{
    thread_trampoline *tr = malloc(sizeof(*tr));
    if (!tr) return -1;
    tr->fn  = fn;
    tr->arg = arg;
    HANDLE h = CreateThread(NULL, 0, thread_entry, tr, 0, NULL);
    if (!h) {
        free(tr);
        return -1;
    }
    *t = h;
    return 0;
}

void ap_thread_join(ap_thread t)
{
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}
