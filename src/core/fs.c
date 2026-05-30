#include "core/fs.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <errno.h>
#include <stdlib.h>

// convert a UTF-8 path to a heap UTF-16 string. returns NULL on
// failure; caller frees with free().
static wchar_t *to_wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = malloc((size_t)n * sizeof(wchar_t));
    if (!w) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) {
        free(w);
        return NULL;
    }
    return w;
}

int ap_rename_replace(const char *src, const char *dst)
{
    wchar_t *wsrc = to_wide(src);
    wchar_t *wdst = to_wide(dst);
    if (!wsrc || !wdst) {
        free(wsrc);
        free(wdst);
        errno = ENOMEM;
        return -1;
    }

    // REPLACE_EXISTING gives POSIX rename's overwrite; WRITE_THROUGH
    // flushes the rename to disk so the atomic-write pattern survives a
    // crash. cross-volume replaces are not atomic and are rejected,
    // surfaced as EXDEV so callers fall back to copy + unlink.
    BOOL ok = MoveFileExW(wsrc, wdst,
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    DWORD err = ok ? 0 : GetLastError();
    free(wsrc);
    free(wdst);
    if (ok) return 0;
    errno = (err == ERROR_NOT_SAME_DEVICE) ? EXDEV : EIO;
    return -1;
}

#else

#include <stdio.h>

int ap_rename_replace(const char *src, const char *dst)
{
    return rename(src, dst);
}

#endif
