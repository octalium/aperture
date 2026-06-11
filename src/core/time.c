#include "core/time.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

double ap_time_now(void)
{
    // the frequency is fixed at boot; the racy first-call init is
    // benign (idempotent write of the same value).
    static LARGE_INTEGER freq;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
}

#else
#include <time.h>

double ap_time_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

#endif
