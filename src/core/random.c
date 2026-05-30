#include "core/random.h"

#include "core/log.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

int ap_random_bytes(void *buf, size_t len)
{
    // BCRYPT_USE_SYSTEM_PREFERRED_RNG draws from the system CNG pool
    // without opening an algorithm provider handle — the windows
    // equivalent of reading /dev/urandom.
    NTSTATUS st = BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)len,
                                  BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (st < 0) {
        AP_ERROR("ap_random_bytes: BCryptGenRandom failed (0x%08lx)",
                 (unsigned long)st);
        return -1;
    }
    return 0;
}

#else

#include <errno.h>
#include <stdio.h>
#include <string.h>

int ap_random_bytes(void *buf, size_t len)
{
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) {
        AP_ERROR("ap_random_bytes: cannot open /dev/urandom: %s",
                 strerror(errno));
        return -1;
    }
    size_t got = fread(buf, 1, len, f);
    fclose(f);
    if (got != len) {
        AP_ERROR("ap_random_bytes: short read from /dev/urandom");
        return -1;
    }
    return 0;
}

#endif
