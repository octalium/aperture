#include "core/dir.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>

struct ap_dir {
    DIR *d;
};

static _Thread_local int g_last_open_errno;

ap_dir *ap_dir_open(const char *path)
{
    DIR *raw = opendir(path);
    if (!raw) {
        g_last_open_errno = errno;
        return NULL;
    }
    ap_dir *d = calloc(1, sizeof(*d));
    if (!d) {
        g_last_open_errno = ENOMEM;
        closedir(raw);
        return NULL;
    }
    d->d = raw;
    return d;
}

int ap_dir_open_errno(void)
{
    return g_last_open_errno;
}

const char *ap_dir_read(ap_dir *d)
{
    if (!d) return NULL;
    struct dirent *e = readdir(d->d);
    return e ? e->d_name : NULL;
}

void ap_dir_close(ap_dir *d)
{
    if (!d) return;
    closedir(d->d);
    free(d);
}
