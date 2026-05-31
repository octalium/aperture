#include "core/dir.h"

#include "core/winutf8.h"  // ap_utf8_to_wide + windows.h

#include <errno.h>
#include <stdlib.h>
#include <wchar.h>

// readdir yields entry names; FindFirstFileW yields the first match up
// front and FindNextFileW the rest. we cache the first hit and hand it
// out on the initial ap_dir_read so the cursor semantics line up. the
// name is converted from UTF-16 to UTF-8 into a fixed buffer that
// stays valid until the next read, matching the d_name lifetime.
struct ap_dir {
    HANDLE          h;
    WIN32_FIND_DATAW data;
    bool            have_pending;  // data holds an unread entry
    char            name[MAX_PATH * 4];  // worst-case UTF-8 expansion
};

static _Thread_local int g_last_open_errno;

// convert a UTF-8 path into a heap UTF-16 string with `\*` appended so
// FindFirstFileW enumerates the directory's contents. returns NULL on
// allocation or conversion failure.
static wchar_t *make_search_glob(const char *path)
{
    wchar_t *wide = ap_utf8_to_wide(path);
    if (!wide) return NULL;
    size_t n = wcslen(wide);  // length without the terminator
    // grow to fit a separator, '*' and the terminator past the path.
    wchar_t *glob = realloc(wide, (n + 3) * sizeof(wchar_t));
    if (!glob) {
        free(wide);
        return NULL;
    }
    wchar_t last = n ? glob[n - 1] : L'\0';
    if (last != L'\\' && last != L'/') {
        glob[n++] = L'\\';
    }
    glob[n++] = L'*';
    glob[n]   = L'\0';
    return glob;
}

// convert the current entry's UTF-16 name into d->name. returns false if
// the conversion fails, so the caller can skip the entry rather than emit
// an empty name (which would alias the parent directory in path joins).
static bool store_name(ap_dir *d)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, d->data.cFileName, -1,
                                d->name, (int)sizeof(d->name), NULL, NULL);
    return n > 0;
}

ap_dir *ap_dir_open(const char *path)
{
    wchar_t *glob = make_search_glob(path);
    if (!glob) {
        g_last_open_errno = ENOMEM;
        return NULL;
    }

    ap_dir *d = calloc(1, sizeof(*d));
    if (!d) {
        g_last_open_errno = ENOMEM;
        free(glob);
        return NULL;
    }

    d->h = FindFirstFileW(glob, &d->data);
    free(glob);
    if (d->h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        g_last_open_errno = (err == ERROR_FILE_NOT_FOUND
                             || err == ERROR_PATH_NOT_FOUND)
                                ? ENOENT
                                : EIO;
        free(d);
        return NULL;
    }
    d->have_pending = true;
    return d;
}

int ap_dir_open_errno(void)
{
    return g_last_open_errno;
}

const char *ap_dir_read(ap_dir *d)
{
    if (!d) return NULL;
    for (;;) {
        if (d->have_pending) {
            d->have_pending = false;
        } else if (!FindNextFileW(d->h, &d->data)) {
            return NULL;
        }
        // skip any entry whose name can't be represented in UTF-8 rather
        // than handing back an empty string.
        if (store_name(d)) return d->name;
    }
}

void ap_dir_close(ap_dir *d)
{
    if (!d) return;
    if (d->h != INVALID_HANDLE_VALUE) FindClose(d->h);
    free(d);
}
