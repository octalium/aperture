#ifndef APERTURE_CORE_TIME_H
#define APERTURE_CORE_TIME_H

#ifdef __cplusplus
extern "C" {
#endif

// Monotonic clock in seconds from an arbitrary epoch. Thread-safe,
// unaffected by wall-clock adjustments — for measuring intervals
// (frame budgets, profiling), never absolute time.
double ap_time_now(void);

#ifdef __cplusplus
}
#endif

#endif /* APERTURE_CORE_TIME_H */
