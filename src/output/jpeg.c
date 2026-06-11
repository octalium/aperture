#include "jpeg.h"

#include "core/fs.h"
#include "core/log.h"
#include "output/jpeg_error.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include <jpeglib.h>
#include <setjmp.h>

// Drive compression for a cinfo whose destination has already been
// wired up (stdio or memory). RGBA in, RGB JPEG out. `row` is a caller
// owned scratch buffer of width * 3 bytes; fatal libjpeg errors longjmp
// out to the caller's trampoline, so nothing here may own resources.
static void encode_rgba(struct jpeg_compress_struct *cinfo,
                        const uint8_t *rgba, int width, int height,
                        int quality, uint8_t *row)
{
    cinfo->image_width      = (JDIMENSION)width;
    cinfo->image_height     = (JDIMENSION)height;
    cinfo->input_components = 3;
    cinfo->in_color_space   = JCS_RGB;

    jpeg_set_defaults(cinfo);
    jpeg_set_quality(cinfo, quality, TRUE);
    jpeg_start_compress(cinfo, TRUE);

    while (cinfo->next_scanline < cinfo->image_height) {
        const uint8_t *src = rgba + (size_t)cinfo->next_scanline
                                  * (size_t)width * 4;
        for (int x = 0; x < width; x++) {
            row[x * 3 + 0] = src[x * 4 + 0];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 2];
        }
        JSAMPROW p = row;
        jpeg_write_scanlines(cinfo, &p, 1);
    }
    jpeg_finish_compress(cinfo);
}

// Run one full compression with the setjmp error trampoline installed.
// Writes to stdio `f` when non-NULL, otherwise to a malloc'd memory
// buffer via `buf`/`size` (which live in the caller, so their values
// stay determinate after a longjmp). Owns the row scratch buffer: it is
// allocated before setjmp and freed on both the success and error path.
static int run_compress(const uint8_t *rgba, int width, int height,
                        int quality, FILE *f,
                        unsigned char **buf, unsigned long *size)
{
    uint8_t *row = malloc((size_t)width * 3);
    if (!row) {
        AP_ERROR("jpeg: row buffer alloc failed");
        return -1;
    }

    struct jpeg_compress_struct cinfo = {0};
    ap_jpeg_error err;
    cinfo.err = ap_jpeg_error_install(&err, "export");

    volatile int rc = -1;
    volatile int mem_ready = 0;
    if (setjmp(err.jump) == 0) {
        jpeg_create_compress(&cinfo);
        if (f) {
            jpeg_stdio_dest(&cinfo, f);
        } else {
            jpeg_mem_dest(&cinfo, buf, size);
            mem_ready = 1;
        }
        encode_rgba(&cinfo, rgba, width, height, quality, row);
        rc = 0;
    }
    // on a mem-dest error the encoder may have regrown its buffer:
    // empty_mem_output_buffer frees the block *buf points at, and only
    // term_destination re-syncs *buf/*size (jpeg_destroy_compress does
    // not call it), so without this the caller would free a dangling
    // pointer and leak the live buffer. term_mem_destination merely
    // assigns and cannot error; the stdio term flushes and can, so it
    // must never be invoked here. mem_ready guards against a longjmp
    // from inside jpeg_mem_dest leaving the destination uninitialized.
    if (rc != 0 && mem_ready) {
        (*cinfo.dest->term_destination)(&cinfo);
    }
    jpeg_destroy_compress(&cinfo);
    free(row);
    return rc;
}

int ap_export_jpeg(const uint8_t *rgba, int width, int height,
                   const char *path, int quality)
{
    if (!rgba || !path || width <= 0 || height <= 0) {
        AP_ERROR("ap_export_jpeg: invalid args");
        return -1;
    }
    if (quality < 0)   quality = 0;
    if (quality > 100) quality = 100;

    ap_atomic *a = ap_atomic_open(path);
    if (!a) {
        AP_ERROR("ap_export_jpeg: open(%s): %m", path);
        return -1;
    }
    FILE *f = ap_atomic_file(a);

    int rc = run_compress(rgba, width, height, quality, f, NULL, NULL);
    if (rc == 0) {
        if (ap_atomic_commit(a) != 0) rc = -1;
    } else {
        ap_atomic_abort(a);
    }
    if (rc == 0) {
        AP_INFO("exported jpeg: %s (%dx%d, q=%d)", path, width, height, quality);
    }
    return rc;
}

int ap_export_jpeg_mem(const uint8_t *rgba, int width, int height,
                       int quality, uint8_t **out, size_t *out_size)
{
    if (!rgba || !out || !out_size || width <= 0 || height <= 0) {
        AP_ERROR("ap_export_jpeg_mem: invalid args");
        return -1;
    }
    if (quality < 0)   quality = 0;
    if (quality > 100) quality = 100;

    // libjpeg-turbo malloc's this buffer and grows it as needed; the
    // caller owns it after a successful return.
    unsigned char *buf = NULL;
    unsigned long  size = 0;

    int rc = run_compress(rgba, width, height, quality, NULL, &buf, &size);
    if (rc == 0 && buf && size > 0) {
        *out      = buf;
        *out_size = (size_t)size;
        return 0;
    }
    free(buf);
    return -1;
}
