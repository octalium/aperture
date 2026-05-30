#include "core/thread.h"

int ap_mutex_init(ap_mutex *m)
{
    return pthread_mutex_init(m, NULL);
}

void ap_mutex_destroy(ap_mutex *m)
{
    pthread_mutex_destroy(m);
}

void ap_mutex_lock(ap_mutex *m)
{
    pthread_mutex_lock(m);
}

void ap_mutex_unlock(ap_mutex *m)
{
    pthread_mutex_unlock(m);
}

int ap_cond_init(ap_cond *c)
{
    return pthread_cond_init(c, NULL);
}

void ap_cond_destroy(ap_cond *c)
{
    pthread_cond_destroy(c);
}

void ap_cond_wait(ap_cond *c, ap_mutex *m)
{
    pthread_cond_wait(c, m);
}

void ap_cond_signal(ap_cond *c)
{
    pthread_cond_signal(c);
}

void ap_cond_broadcast(ap_cond *c)
{
    pthread_cond_broadcast(c);
}

int ap_thread_create(ap_thread *t, ap_thread_fn fn, void *arg)
{
    return pthread_create(t, NULL, fn, arg);
}

void ap_thread_join(ap_thread t)
{
    pthread_join(t, NULL);
}
