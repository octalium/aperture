#include "jpeg.h"

#include "core/log.h"
#include "output/jpeg_error.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include <jpeglib.h>
#include <setjmp.h>

// Drive compression for a cinfo whose destination has already been
// wired up (stdio or memory). RGBA in, RGB JPEG out.
static int encode_rgba(struct jpeg_compress_struct *cinfo,
                       const uint8_t *rgba, int width, int height,
                       int quality)
{
    cinfo->image_width      = (JDIMENSION)width;
    cinfo->image_height     = (JDIMENSION)height;
    cinfo->input_components = 3;
    cinfo->in_color_space   = JCS_RGB;

    jpeg_set_defaults(cinfo);
    jpeg_set_quality(cinfo, quality, TRUE);
    jpeg_start_compress(cinfo, TRUE);

    uint8_t *row = malloc((size_t)width * 3);
    if (!row) {
        AP_ERROR("jpeg: row buffer alloc failed");
        return -1;
    }
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
    free(row);
    return 0;
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

    FILE *f = fopen(path, "wb");
    if (!f) {
        AP_ERROR("ap_export_jpeg: fopen(%s): %m", path);
        return -1;
    }

    struct jpeg_compress_struct cinfo = {0};
    ap_jpeg_error err;
    cinfo.err = ap_jpeg_error_install(&err, "export");

    int rc = -1;
    if (setjmp(err.jump) != 0) {
        goto done;
    }
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, f);
    rc = encode_rgba(&cinfo, rgba, width, height, quality);

done:
    jpeg_destroy_compress(&cinfo);
    fclose(f);
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

    struct jpeg_compress_struct cinfo = {0};
    ap_jpeg_error err;
    cinfo.err = ap_jpeg_error_install(&err, "export");

    // libjpeg-turbo malloc's this buffer and grows it as needed; the
    // caller owns it after a successful return.
    unsigned char *buf = NULL;
    unsigned long  size = 0;

    int rc = -1;
    if (setjmp(err.jump) != 0) {
        goto done;
    }
    jpeg_create_compress(&cinfo);
    jpeg_mem_dest(&cinfo, &buf, &size);
    rc = encode_rgba(&cinfo, rgba, width, height, quality);

done:
    jpeg_destroy_compress(&cinfo);
    if (rc == 0 && buf && size > 0) {
        *out      = buf;
        *out_size = (size_t)size;
        return 0;
    }
    free(buf);
    return -1;
}
