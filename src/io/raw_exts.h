#ifndef APERTURE_IO_RAW_EXTS_H
#define APERTURE_IO_RAW_EXTS_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>

// Single source of truth for the file extensions Aperture treats as raw
// photos. Production code reaches it through `io/raw.h`; the library
// test stub in test/library/library_test_stubs.c includes this header
// directly so it can't drift from the production list.
static const char *const ap_raw_extensions[] = {
    ".nef", ".cr2", ".cr3", ".raf", ".arw",
    ".dng", ".orf", ".rw2", ".pef", ".srw",
    NULL,
};

// True when `path` (or a bare filename) ends in a supported raw-file
// extension. Case-insensitive on the extension only.
static inline bool ap_raw_is_raw_path(const char *path)
{
    if (!path) return false;
    const char *dot = strrchr(path, '.');
    if (!dot) return false;
    for (size_t i = 0; ap_raw_extensions[i]; i++) {
        if (strcasecmp(dot, ap_raw_extensions[i]) == 0) return true;
    }
    return false;
}

#endif
