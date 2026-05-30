#define _GNU_SOURCE  // open_memstream on glibc

#include "core/memstream.h"

#include <stdlib.h>

#ifdef _WIN32

// windows has no open_memstream and no fopencookie/funopen to build one
// from. accumulate writes through a binary temp file, then read the
// whole file back into a heap buffer on close. tmpfile() places the
// file in the per-process temp area and unlinks it on close.
struct ap_memstream {
    FILE *fp;
};

ap_memstream *ap_memstream_open(void)
{
    ap_memstream *m = malloc(sizeof(*m));
    if (!m) return NULL;
    m->fp = tmpfile();
    if (!m->fp) {
        free(m);
        return NULL;
    }
    return m;
}

FILE *ap_memstream_file(ap_memstream *m)
{
    return m->fp;
}

int ap_memstream_close(ap_memstream *m, char **out_buf, size_t *out_len)
{
    *out_buf = NULL;
    *out_len = 0;

    FILE *fp = m->fp;
    int rc = -1;
    char *buf = NULL;

    if (fflush(fp) == 0 && fseek(fp, 0, SEEK_END) == 0) {
        long size = ftell(fp);
        if (size >= 0 && fseek(fp, 0, SEEK_SET) == 0) {
            buf = malloc((size_t)size + 1);
            if (buf) {
                size_t got = fread(buf, 1, (size_t)size, fp);
                if (got == (size_t)size) {
                    buf[size] = '\0';
                    *out_buf = buf;
                    *out_len = (size_t)size;
                    rc = 0;
                } else {
                    free(buf);
                }
            }
        }
    }

    fclose(fp);
    free(m);
    return rc;
}

#else

#include <stdio.h>

// the POSIX backend owns the buf/len cells that open_memstream writes
// through on flush/close, then hands the buffer to the caller.
struct ap_memstream {
    FILE  *fp;
    char  *buf;
    size_t len;
};

ap_memstream *ap_memstream_open(void)
{
    ap_memstream *m = malloc(sizeof(*m));
    if (!m) return NULL;
    m->buf = NULL;
    m->len = 0;
    m->fp  = open_memstream(&m->buf, &m->len);
    if (!m->fp) {
        free(m);
        return NULL;
    }
    return m;
}

FILE *ap_memstream_file(ap_memstream *m)
{
    return m->fp;
}

int ap_memstream_close(ap_memstream *m, char **out_buf, size_t *out_len)
{
    *out_buf = NULL;
    *out_len = 0;

    int rc = -1;
    if (fclose(m->fp) == 0) {
        *out_buf = m->buf;   // open_memstream NUL-terminates; caller frees
        *out_len = m->len;
        rc = 0;
    } else {
        free(m->buf);
    }
    free(m);
    return rc;
}

#endif
