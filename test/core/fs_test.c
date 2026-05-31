// Atomic-write helper tests. Exercises ap_atomic_* on a real tmpdir:
// commit replaces in place, abort leaves nothing, the temp is always
// cleaned up, and the path-based variant round-trips.

#define _GNU_SOURCE

#include "aptest.h"
#include "aptest_tmpdir.h"

#include "core/dir.h"
#include "core/fs.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

// count directory entries excluding "." / "..". used to assert that a
// commit/abort leaves exactly the expected files and no stray temp.
static int count_entries(const char *dir)
{
    ap_dir *d = ap_dir_open(dir);
    AP_TEST_ASSERT(d != NULL, "ap_dir_open(%s)", dir);
    int c = 0;
    const char *name;
    while ((name = ap_dir_read(d)) != NULL) {
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        c++;
    }
    ap_dir_close(d);
    return c;
}

static void read_file(const char *path, char *out, size_t out_len)
{
    FILE *f = fopen(path, "rb");
    AP_TEST_ASSERT(f != NULL, "open(%s)", path);
    size_t n = fread(out, 1, out_len - 1, f);
    out[n] = '\0';
    fclose(f);
}

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static void test_commit(const char *dir)
{
    char path[4096];
    int pn = snprintf(path, sizeof(path), "%s/file.txt", dir);
    AP_TEST_ASSERT(pn > 0 && (size_t)pn < sizeof(path), "path too long");

    ap_atomic *a = ap_atomic_open(path);
    AP_TEST_ASSERT(a != NULL, "ap_atomic_open");
    fputs("hello atomic", ap_atomic_file(a));
    AP_TEST_ASSERT(ap_atomic_commit(a) == 0, "commit");

    char buf[64];
    read_file(path, buf, sizeof(buf));
    AP_TEST_ASSERT(strcmp(buf, "hello atomic") == 0, "content=%s", buf);
    AP_TEST_ASSERT(count_entries(dir) == 1, "commit must leave only the final file");

    // a second commit replaces the file in place.
    a = ap_atomic_open(path);
    fputs("second", ap_atomic_file(a));
    AP_TEST_ASSERT(ap_atomic_commit(a) == 0, "commit overwrite");
    read_file(path, buf, sizeof(buf));
    AP_TEST_ASSERT(strcmp(buf, "second") == 0, "overwrite content=%s", buf);
    AP_TEST_ASSERT(count_entries(dir) == 1, "overwrite must not leave a temp");

    unlink(path);
}

static void test_abort(const char *dir)
{
    char path[4096];
    int pn = snprintf(path, sizeof(path), "%s/aborted.txt", dir);
    AP_TEST_ASSERT(pn > 0 && (size_t)pn < sizeof(path), "path too long");

    ap_atomic *a = ap_atomic_open(path);
    AP_TEST_ASSERT(a != NULL, "ap_atomic_open");
    fputs("junk that must vanish", ap_atomic_file(a));
    ap_atomic_abort(a);

    AP_TEST_ASSERT(!file_exists(path), "abort must not create the final file");
    AP_TEST_ASSERT(count_entries(dir) == 0, "abort must leave no temp");
}

static void test_path_variant(const char *dir)
{
    char path[4096];
    int pn = snprintf(path, sizeof(path), "%s/viapath.txt", dir);
    AP_TEST_ASSERT(pn > 0 && (size_t)pn < sizeof(path), "path too long");

    char tmp[4096];
    AP_TEST_ASSERT(ap_atomic_temp_path(path, tmp, sizeof(tmp)) == 0, "temp_path");
    FILE *f = fopen(tmp, "wb");
    AP_TEST_ASSERT(f != NULL, "open temp");
    fputs("written by path", f);
    fclose(f);
    AP_TEST_ASSERT(ap_atomic_commit_temp(tmp, path) == 0, "commit_temp");

    char buf[64];
    read_file(path, buf, sizeof(buf));
    AP_TEST_ASSERT(strcmp(buf, "written by path") == 0, "path content=%s", buf);
    AP_TEST_ASSERT(count_entries(dir) == 1, "commit_temp must not leave a temp");

    unlink(path);
}

int main(void)
{
    char dir[4096];
    aptest_tmpdir_make(dir, sizeof(dir));

    test_commit(dir);
    test_abort(dir);
    test_path_variant(dir);

    aptest_tmpdir_rm(dir);
    printf("core/fs: OK\n");
    return 0;
}
