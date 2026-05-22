#include "tiff.h"

#include "core/log.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <tiffio.h>

// libtiff error / warning redirected through the aperture log so that
// they use the same format as the rest of the codebase.
static void on_tiff_error(const char *module, const char *fmt, va_list ap)
{
    char msg[512];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    AP_ERROR("tiff(%s): %s", module ? module : "?", msg);
}

static void on_tiff_warning(const char *module, const char *fmt, va_list ap)
{
    char msg[512];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    AP_WARN("tiff(%s): %s", module ? module : "?", msg);
}

// Write TIFF tags common to all depth variants, then write strips.
// On entry `tif` is open for writing, tags not yet set.
static int write_tiff(TIFF *tif,
                      const uint8_t *rgba_u8, const float *rgba_f32,
                      int width, int height,
                      ap_tiff_depth depth, ap_tiff_compress compress,
                      const uint8_t *icc_data, size_t icc_size)
{
    uint16_t bits_per_sample;
    uint16_t sample_format;

    switch (depth) {
    case AP_TIFF_UINT8:
        bits_per_sample = 8;
        sample_format   = SAMPLEFORMAT_UINT;
        break;
    case AP_TIFF_UINT16:
        bits_per_sample = 16;
        sample_format   = SAMPLEFORMAT_UINT;
        break;
    case AP_TIFF_FLOAT32:
        bits_per_sample = 32;
        sample_format   = SAMPLEFORMAT_IEEEFP;
        break;
    default:
        AP_ERROR("ap_export_tiff: unknown depth %d", (int)depth);
        return -1;
    }

    uint16_t compression;
    switch (compress) {
    case AP_TIFF_COMPRESS_NONE:    compression = COMPRESSION_NONE;    break;
    case AP_TIFF_COMPRESS_LZW:     compression = COMPRESSION_LZW;     break;
    case AP_TIFF_COMPRESS_DEFLATE: compression = COMPRESSION_DEFLATE; break;
    default:
        AP_ERROR("ap_export_tiff: unknown compression %d", (int)compress);
        return -1;
    }

    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH,      (uint32_t)width);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH,     (uint32_t)height);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, (uint16_t)3);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE,   bits_per_sample);
    TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT,    sample_format);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC,     PHOTOMETRIC_RGB);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG,    PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_ORIENTATION,     ORIENTATION_TOPLEFT);
    TIFFSetField(tif, TIFFTAG_COMPRESSION,     compression);

    // LZW predictor improves compression for 8- and 16-bit integer data.
    if (compress == AP_TIFF_COMPRESS_LZW ||
        compress == AP_TIFF_COMPRESS_DEFLATE) {
        if (depth != AP_TIFF_FLOAT32) {
            TIFFSetField(tif, TIFFTAG_PREDICTOR, PREDICTOR_HORIZONTAL);
        }
    }

    // Use strips of 32 rows — conventional default that keeps each
    // strip under ~4 MB for typical photo widths.
    uint32_t rows_per_strip = 32;
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, rows_per_strip);

    if (icc_data && icc_size > 0) {
        TIFFSetField(tif, TIFFTAG_ICCPROFILE, (uint32_t)icc_size, icc_data);
    }

    size_t bytes_per_sample = (size_t)(bits_per_sample / 8);
    size_t row_bytes = (size_t)width * 3 * bytes_per_sample;
    uint8_t *row = malloc(row_bytes);
    if (!row) {
        AP_ERROR("ap_export_tiff: row buffer alloc failed");
        return -1;
    }

    int rc = 0;
    for (int y = 0; y < height; y++) {
        if (depth == AP_TIFF_UINT8) {
            const uint8_t *src = rgba_u8 + (size_t)y * (size_t)width * 4;
            uint8_t *dst = row;
            for (int x = 0; x < width; x++) {
                dst[x * 3 + 0] = src[x * 4 + 0];
                dst[x * 3 + 1] = src[x * 4 + 1];
                dst[x * 3 + 2] = src[x * 4 + 2];
            }
        } else if (depth == AP_TIFF_UINT16) {
            const uint8_t *src = rgba_u8 + (size_t)y * (size_t)width * 4;
            uint16_t *dst = (uint16_t *)row;
            for (int x = 0; x < width; x++) {
                // Expand 8-bit input to full 16-bit range (0x00→0x0000,
                // 0xFF→0xFFFF) by replicating the byte in both halves.
                uint8_t r = src[x * 4 + 0];
                uint8_t g = src[x * 4 + 1];
                uint8_t b = src[x * 4 + 2];
                dst[x * 3 + 0] = (uint16_t)((r << 8) | r);
                dst[x * 3 + 1] = (uint16_t)((g << 8) | g);
                dst[x * 3 + 2] = (uint16_t)((b << 8) | b);
            }
        } else {
            const float *src = rgba_f32 + (size_t)y * (size_t)width * 4;
            float *dst = (float *)row;
            for (int x = 0; x < width; x++) {
                dst[x * 3 + 0] = src[x * 4 + 0];
                dst[x * 3 + 1] = src[x * 4 + 1];
                dst[x * 3 + 2] = src[x * 4 + 2];
            }
        }
        if (TIFFWriteScanline(tif, row, (uint32_t)y, 0) < 0) {
            AP_ERROR("ap_export_tiff: TIFFWriteScanline failed at row %d", y);
            rc = -1;
            break;
        }
    }

    free(row);
    return rc;
}

int ap_export_tiff(const uint8_t *rgba_u8, const float *rgba_f32,
                   int width, int height,
                   ap_tiff_depth depth, ap_tiff_compress compress,
                   const uint8_t *icc_data, size_t icc_size,
                   const char *path)
{
    if (!path || width <= 0 || height <= 0) {
        AP_ERROR("ap_export_tiff: invalid args");
        return -1;
    }
    if (depth == AP_TIFF_FLOAT32 && !rgba_f32) {
        AP_ERROR("ap_export_tiff: rgba_f32 required for FLOAT32 depth");
        return -1;
    }
    if (depth != AP_TIFF_FLOAT32 && !rgba_u8) {
        AP_ERROR("ap_export_tiff: rgba_u8 required for integer depth");
        return -1;
    }

    TIFFSetErrorHandler(on_tiff_error);
    TIFFSetWarningHandler(on_tiff_warning);

    TIFF *tif = TIFFOpen(path, "w");
    if (!tif) {
        AP_ERROR("ap_export_tiff: TIFFOpen(%s) failed", path);
        return -1;
    }

    int rc = write_tiff(tif, rgba_u8, rgba_f32,
                        width, height, depth, compress,
                        icc_data, icc_size);
    TIFFClose(tif);

    if (rc == 0) {
        const char *depth_str  = depth == AP_TIFF_UINT8   ? "uint8"
                               : depth == AP_TIFF_UINT16  ? "uint16"
                               :                            "float32";
        const char *comp_str   = compress == AP_TIFF_COMPRESS_NONE    ? "none"
                               : compress == AP_TIFF_COMPRESS_LZW     ? "lzw"
                               :                                         "deflate";
        AP_INFO("exported tiff: %s (%dx%d, %s, compress=%s)",
                path, width, height, depth_str, comp_str);
    }
    return rc;
}
