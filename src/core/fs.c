#define _GNU_SOURCE   // expose fileno()/fsync() under strict c17

#include "core/fs.h"

#include "core/compat.h"   // fsync / fileno under their POSIX names on every target
#include "core/log.h"
#include "core/random.h"
#include "core/winutf8.h"  // ap_utf8_to_wide on windows

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

int ap_rename_replace(const char *src, const char *dst)
{
    wchar_t *wsrc = ap_utf8_to_wide(src);
    wchar_t *wdst = ap_utf8_to_wide(dst);
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

// the durable-rename above (MoveFileEx WRITE_THROUGH) already forces the
// directory entry to disk, so a separate directory sync is unnecessary.
static int fsync_dir(const char *path) { (void)path; return 0; }

#else

#include <fcntl.h>
#include <unistd.h>

int ap_rename_replace(const char *src, const char *dst)
{
    return rename(src, dst);
}

// fsync the directory holding `path` so a create/rename within it is
// durable (the file's own fsync does not cover the directory entry).
static int fsync_dir(const char *path)
{
    char dir[4096];
    int n = snprintf(dir, sizeof(dir), "%s", path);
    if (n < 0 || (size_t)n >= sizeof(dir)) return -1;
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    else { dir[0] = '.'; dir[1] = '\0'; }
    int fd = open(dir, O_RDONLY);
    if (fd < 0) return -1;
    int rc = fsync(fd);
    close(fd);
    return rc;
}

#endif

// ---- atomic write (shared across platforms) ----

struct ap_atomic {
    FILE *f;
    char  temp[4096];
    char  final[4096];
};

// build "<final>.tmp.<16 hex>" into `out`. the random suffix makes a
// collision (with a concurrent writer or a stale temp) effectively
// impossible. returns 0, or -1 if it doesn't fit / entropy failed.
static int make_temp_path(const char *final_path, char *out, size_t out_size)
{
    uint8_t r[8];
    if (ap_random_bytes(r, sizeof(r)) != 0) return -1;
    static const char hexd[] = "0123456789abcdef";
    char hex[17];
    for (int i = 0; i < 8; i++) {
        hex[i * 2]     = hexd[r[i] >> 4];
        hex[i * 2 + 1] = hexd[r[i] & 0x0f];
    }
    hex[16] = '\0';
    int n = snprintf(out, out_size, "%s.tmp.%s", final_path, hex);
    if (n < 0 || (size_t)n >= out_size) return -1;
    return 0;
}

int ap_atomic_temp_path(const char *final_path, char *out, size_t out_size)
{
    if (!final_path || !out) return -1;
    return make_temp_path(final_path, out, out_size);
}

ap_atomic *ap_atomic_open(const char *final_path)
{
    if (!final_path) return NULL;
    ap_atomic *a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    int n = snprintf(a->final, sizeof(a->final), "%s", final_path);
    if (n < 0 || (size_t)n >= sizeof(a->final)) { free(a); return NULL; }
    if (make_temp_path(final_path, a->temp, sizeof(a->temp)) != 0) {
        free(a);
        return NULL;
    }
    a->f = fopen(a->temp, "wb");
    if (!a->f) {
        AP_WARN("fs: create %s: %s", a->temp, strerror(errno));
        free(a);
        return NULL;
    }
    return a;
}

FILE *ap_atomic_file(ap_atomic *a) { return a ? a->f : NULL; }

int ap_atomic_commit(ap_atomic *a)
{
    if (!a) return -1;
    int rc = 0;
    if (fflush(a->f) != 0 || fsync(fileno(a->f)) != 0) {
        AP_ERROR("fs: fsync %s: %s", a->temp, strerror(errno));
        rc = -1;
    }
    if (fclose(a->f) != 0) rc = -1;
    a->f = NULL;
    if (rc == 0 && ap_rename_replace(a->temp, a->final) != 0) {
        AP_ERROR("fs: rename %s -> %s: %s", a->temp, a->final, strerror(errno));
        rc = -1;
    }
    if (rc == 0) fsync_dir(a->final);
    if (rc != 0) remove(a->temp);
    free(a);
    return rc;
}

void ap_atomic_abort(ap_atomic *a)
{
    if (!a) return;
    if (a->f) fclose(a->f);
    remove(a->temp);
    free(a);
}

int ap_atomic_commit_temp(const char *temp_path, const char *final_path)
{
    if (!temp_path || !final_path) return -1;
    // the caller already wrote + closed the temp; reopen to fsync its
    // contents to disk before the rename.
    int rc = 0;
    FILE *f = fopen(temp_path, "rb");
    if (!f) {
        AP_ERROR("fs: reopen %s: %s", temp_path, strerror(errno));
        rc = -1;
    } else {
        if (fsync(fileno(f)) != 0) rc = -1;
        fclose(f);
    }
    if (rc == 0 && ap_rename_replace(temp_path, final_path) != 0) {
        AP_ERROR("fs: rename %s -> %s: %s", temp_path, final_path, strerror(errno));
        rc = -1;
    }
    if (rc == 0) fsync_dir(final_path);
    if (rc != 0) remove(temp_path);
    return rc;
}

void ap_atomic_discard_temp(const char *temp_path)
{
    if (temp_path) remove(temp_path);
}
