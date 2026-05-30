#ifndef APERTURE_CORE_RANDOM_H
#define APERTURE_CORE_RANDOM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// fill `buf` with `len` cryptographically-strong random bytes. backed
// by /dev/urandom on unix and BCryptGenRandom (CNG) on windows. returns
// 0 on success, -1 on failure (and logs the cause).
int ap_random_bytes(void *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif
