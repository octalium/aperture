#ifndef APERTURE_CORE_WINUTF8_H
#define APERTURE_CORE_WINUTF8_H

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdlib.h>

// convert a UTF-8 string to a heap-allocated UTF-16 string. returns
// NULL on allocation or conversion failure (including an invalid UTF-8
// sequence under CP_UTF8); the caller frees the result with free().
static inline wchar_t *ap_utf8_to_wide(const char *s)
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

#endif // _WIN32

#endif
