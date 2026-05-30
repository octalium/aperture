#ifndef APERTURE_TEST_APTEST_TMPDIR_H
#define APERTURE_TEST_APTEST_TMPDIR_H

// Tiny tmpdir helper for tests that need on-disk state (library db,
// sidecars, ...). Build a unique directory under $TMPDIR (or the
// platform temp dir), hand back its absolute path, and rm -rf it on
// cleanup. Directory iteration goes through the portable ap_dir API so
// the helper builds on every target.

#define _GNU_SOURCE

#include "aptest.h"

#include "core/compat.h"
#include "core/dir.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
// MSVC has no mkdtemp; synthesise it from _mktemp_s + _mkdir. the
// template's trailing XXXXXX is rewritten in place, matching POSIX.
static inline char *aptest_mkdtemp(char *tmpl)
{
    if (_mktemp_s(tmpl, strlen(tmpl) + 1) != 0) return NULL;
    return (_mkdir(tmpl) == 0) ? tmpl : NULL;
}
#define mkdtemp(t) aptest_mkdtemp(t)
#endif

// fill `out` with a freshly-created tmpdir path. asserts on failure.
static inline void aptest_tmpdir_make(char *out, size_t out_len)
{
    const char *base = getenv("TMPDIR");
#ifdef _WIN32
    if (!base || !*base) base = getenv("TEMP");
    if (!base || !*base) base = ".";
#else
    if (!base || !*base) base = "/tmp";
#endif
    int n = snprintf(out, out_len, "%s/aptest.XXXXXX", base);
    AP_TEST_ASSERT(n > 0 && (size_t)n < out_len, "tmpdir path too long");
    AP_TEST_ASSERT(mkdtemp(out) != NULL, "mkdtemp(%s): %s",
                   out, strerror(errno));
}

// recursively remove `path`. tolerates missing files; aborts on real errors.
static inline void aptest_tmpdir_rm(const char *path)
{
    if (!path || !*path) return;
    ap_dir *d = ap_dir_open(path);
    if (!d) {
        if (ap_dir_open_errno() == ENOENT) return;
        AP_TEST_ASSERT(0, "opendir(%s): %s", path, strerror(ap_dir_open_errno()));
    }
    const char *name;
    while ((name = ap_dir_read(d)) != NULL) {
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }
        char child[4096];
        int n = snprintf(child, sizeof(child), "%s/%s", path, name);
        AP_TEST_ASSERT(n > 0 && (size_t)n < sizeof(child),
                       "child path too long under %s", path);
        struct stat st;
        AP_TEST_ASSERT(stat(child, &st) == 0, "stat(%s): %s",
                       child, strerror(errno));
        if (S_ISDIR(st.st_mode)) {
            aptest_tmpdir_rm(child);
        } else {
            AP_TEST_ASSERT(unlink(child) == 0 || errno == ENOENT,
                           "unlink(%s): %s", child, strerror(errno));
        }
    }
    ap_dir_close(d);
    AP_TEST_ASSERT(rmdir(path) == 0 || errno == ENOENT,
                   "rmdir(%s): %s", path, strerror(errno));
}

#endif
