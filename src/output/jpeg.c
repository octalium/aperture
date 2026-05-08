#include "jpeg.h"

#include "core/log.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include <jpeglib.h>
#include <setjmp.h>

struct jpeg_error_jmp {
    struct jpeg_error_mgr base;
    jmp_buf jump;
};

static void on_jpeg_error_exit(j_common_ptr cinfo)
{
    char msg[JMSG_LENGTH_MAX];
    (*cinfo->err->format_message)(cinfo, msg);
    AP_ERROR("jpeg: %s", msg);
    struct jpeg_error_jmp *err = (struct jpeg_error_jmp *)cinfo->err;
    longjmp(err->jump, 1);
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
    struct jpeg_error_jmp err;
    cinfo.err = jpeg_std_error(&err.base);
    err.base.error_exit = on_jpeg_error_exit;

    uint8_t *row = NULL;
    int rc = -1;

    if (setjmp(err.jump) != 0) {
        // Error path — libjpeg long-jumped here.
        goto done;
    }

    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, f);

    cinfo.image_width      = (JDIMENSION)width;
    cinfo.image_height     = (JDIMENSION)height;
    cinfo.input_components = 3;
    cinfo.in_color_space   = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);

    jpeg_start_compress(&cinfo, TRUE);

    row = malloc((size_t)width * 3);
    if (!row) {
        AP_ERROR("ap_export_jpeg: row buffer alloc failed");
        goto done;
    }

    while (cinfo.next_scanline < cinfo.image_height) {
        const uint8_t *src = rgba + (size_t)cinfo.next_scanline * (size_t)width * 4;
        for (int x = 0; x < width; x++) {
            row[x * 3 + 0] = src[x * 4 + 0];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 2];
        }
        JSAMPROW p = row;
        jpeg_write_scanlines(&cinfo, &p, 1);
    }

    jpeg_finish_compress(&cinfo);
    rc = 0;

done:
    free(row);
    jpeg_destroy_compress(&cinfo);
    fclose(f);
    if (rc == 0) {
        AP_INFO("exported jpeg: %s (%dx%d, q=%d)", path, width, height, quality);
    }
    return rc;
}
