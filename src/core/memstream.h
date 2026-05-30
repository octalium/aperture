#ifndef APERTURE_CORE_MEMSTREAM_H
#define APERTURE_CORE_MEMSTREAM_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// portable in-memory output stream, replacing POSIX open_memstream
// (which MSVC lacks). open a stream, write to its FILE* with the usual
// stdio calls, then close to recover the accumulated bytes as a single
// heap buffer. on unix this is a thin wrapper over open_memstream; on
// windows it accumulates through a temp file and slurps it back on
// close.
typedef struct ap_memstream ap_memstream;

// open a new in-memory output stream. returns NULL on failure.
ap_memstream *ap_memstream_open(void);

// the writable stdio stream. valid until ap_memstream_close. never NULL
// for a stream returned by ap_memstream_open.
FILE *ap_memstream_file(ap_memstream *m);

// flush + close the stream, hand back the written bytes. on success
// `*out_buf` points to a heap buffer (NUL-terminated; caller frees with
// free()) and `*out_len` holds its length excluding the NUL. returns 0
// on success, -1 on failure (the stream is still released; `*out_buf`
// is set to NULL). `m` is freed regardless.
int ap_memstream_close(ap_memstream *m, char **out_buf, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif
