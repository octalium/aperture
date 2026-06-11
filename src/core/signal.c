#define _GNU_SOURCE  // SA_RESTART under -std=c17

#include "core/signal.h"

#include <signal.h>
#include <stddef.h>

void ap_signal_install(int sig, void (*handler)(int))
{
#ifdef _WIN32
    // MSVC's CRT has only C signal(); SIGINT / SIGTERM exist and feed
    // the same handler.
    signal(sig, handler);
#else
    struct sigaction sa = {0};
    sa.sa_handler = handler;
    // restart interrupted syscalls so the shutdown signal doesn't fail
    // an in-flight blocking read/write on a worker thread.
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
#endif
}
