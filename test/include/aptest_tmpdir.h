#ifndef APERTURE_TEST_APTEST_TMPDIR_H
#define APERTURE_TEST_APTEST_TMPDIR_H

// Tiny tmpdir helper for tests that need on-disk state (library db,
// sidecars, ...). Build a unique directory under $TMPDIR (or /tmp),
// hand back its absolute path, and rm -rf it on cleanup.

#define _GNU_SOURCE

#include "aptest.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// fill `out` with a freshly-created tmpdir path. asserts on failure.
static inline void aptest_tmpdir_make(char *out, size_t out_len)
{
    const char *base = getenv("TMPDIR");
    if (!base || !*base) base = "/tmp";
    int n = snprintf(out, out_len, "%s/aptest.XXXXXX", base);
    AP_TEST_ASSERT(n > 0 && (size_t)n < out_len, "tmpdir path too long");
    AP_TEST_ASSERT(mkdtemp(out) != NULL, "mkdtemp(%s): %s",
                   out, strerror(errno));
}

// recursively remove `path`. tolerates missing files; aborts on real errors.
static inline void aptest_tmpdir_rm(const char *path)
{
    if (!path || !*path) return;
    DIR *d = opendir(path);
    if (!d) {
        if (errno == ENOENT) return;
        AP_TEST_ASSERT(0, "opendir(%s): %s", path, strerror(errno));
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        char child[4096];
        int n = snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        AP_TEST_ASSERT(n > 0 && (size_t)n < sizeof(child),
                       "child path too long under %s", path);
        struct stat st;
        AP_TEST_ASSERT(lstat(child, &st) == 0, "lstat(%s): %s",
                       child, strerror(errno));
        if (S_ISDIR(st.st_mode)) {
            aptest_tmpdir_rm(child);
        } else {
            AP_TEST_ASSERT(unlink(child) == 0 || errno == ENOENT,
                           "unlink(%s): %s", child, strerror(errno));
        }
    }
    closedir(d);
    AP_TEST_ASSERT(rmdir(path) == 0 || errno == ENOENT,
                   "rmdir(%s): %s", path, strerror(errno));
}

#endif
