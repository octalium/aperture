#include "png.h"

#include "core/log.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

#include <png.h>

// libpng error / warning redirected through the aperture log.
static void on_png_error(png_structp png, png_const_charp msg)
{
    AP_ERROR("png: %s", msg);
    // libpng requires a longjmp back to the caller's setjmp on error.
    longjmp(png_jmpbuf(png), 1);
}

static void on_png_warning(png_structp png, png_const_charp msg)
{
    (void)png;
    AP_WARN("png: %s", msg);
}

int ap_export_png(const uint8_t *rgba, int width, int height,
                  ap_png_depth depth,
                  const uint8_t *icc_data, size_t icc_size,
                  const char *path)
{
    if (!rgba || !path || width <= 0 || height <= 0) {
        AP_ERROR("ap_export_png: invalid args");
        return -1;
    }
    if (depth != AP_PNG_UINT8 && depth != AP_PNG_UINT16) {
        AP_ERROR("ap_export_png: unknown depth %d", (int)depth);
        return -1;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        AP_ERROR("ap_export_png: fopen(%s): %m", path);
        return -1;
    }

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING,
                                              NULL,
                                              on_png_error, on_png_warning);
    if (!png) {
        AP_ERROR("ap_export_png: png_create_write_struct failed");
        fclose(f);
        return -1;
    }

    png_infop info = png_create_info_struct(png);
    if (!info) {
        AP_ERROR("ap_export_png: png_create_info_struct failed");
        png_destroy_write_struct(&png, NULL);
        fclose(f);
        return -1;
    }

    int rc = -1;

    if (setjmp(png_jmpbuf(png)) != 0) {
        goto done;
    }

    png_init_io(png, f);

    int bit_depth = (depth == AP_PNG_UINT16) ? 16 : 8;
    png_set_IHDR(png, info,
                 (png_uint_32)width, (png_uint_32)height,
                 bit_depth,
                 PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);

    if (icc_data && icc_size > 0) {
        // The cast to (png_bytep) is required by the libpng API on all
        // versions; the data is not modified.
        png_set_iCCP(png, info, "ICC Profile", PNG_COMPRESSION_TYPE_BASE,
                     (png_const_bytep)icc_data, (png_uint_32)icc_size);
    }

    png_write_info(png, info);

    // libpng on big-endian writes 16-bit values byte-swapped relative to
    // how we pack them; png_set_swap corrects this on little-endian hosts.
    if (bit_depth == 16) {
        png_set_swap(png);
    }

    // Allocate one output row (RGB, no alpha).
    size_t row_bytes = (size_t)width * 3 * (size_t)(bit_depth / 8);
    uint8_t *row = malloc(row_bytes);
    if (!row) {
        AP_ERROR("ap_export_png: row buffer alloc failed");
        goto done;
    }

    for (int y = 0; y < height; y++) {
        const uint8_t *src = rgba + (size_t)y * (size_t)width * 4;
        if (depth == AP_PNG_UINT8) {
            uint8_t *dst = row;
            for (int x = 0; x < width; x++) {
                dst[x * 3 + 0] = src[x * 4 + 0];
                dst[x * 3 + 1] = src[x * 4 + 1];
                dst[x * 3 + 2] = src[x * 4 + 2];
            }
        } else {
            uint16_t *dst = (uint16_t *)row;
            for (int x = 0; x < width; x++) {
                uint8_t r = src[x * 4 + 0];
                uint8_t g = src[x * 4 + 1];
                uint8_t b = src[x * 4 + 2];
                dst[x * 3 + 0] = (uint16_t)((r << 8) | r);
                dst[x * 3 + 1] = (uint16_t)((g << 8) | g);
                dst[x * 3 + 2] = (uint16_t)((b << 8) | b);
            }
        }
        png_write_row(png, row);
    }
    free(row);

    png_write_end(png, info);
    rc = 0;

done:
    png_destroy_write_struct(&png, &info);
    fclose(f);
    if (rc == 0) {
        const char *depth_str = (depth == AP_PNG_UINT16) ? "uint16" : "uint8";
        AP_INFO("exported png: %s (%dx%d, %s)", path, width, height, depth_str);
    }
    return rc;
}
