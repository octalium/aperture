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
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
#endif
}
