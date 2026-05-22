#ifndef APERTURE_OUTPUT_TIFF_H
#define APERTURE_OUTPUT_TIFF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// TIFF sample format. Selects both the bit depth and the numeric type
// written into the TIFF SampleFormat tag.
typedef enum {
    AP_TIFF_UINT8  = 0,   // 8-bit unsigned integer (display-ready)
    AP_TIFF_UINT16 = 1,   // 16-bit unsigned integer (full pipeline range)
    AP_TIFF_FLOAT32 = 2,  // 32-bit IEEE-754 float   (linear scene-linear)
} ap_tiff_depth;

// TIFF strip compression scheme.
typedef enum {
    AP_TIFF_COMPRESS_NONE = 0,  // no compression (fastest, largest)
    AP_TIFF_COMPRESS_LZW  = 1,  // lossless LZW
    AP_TIFF_COMPRESS_DEFLATE = 2, // lossless Deflate (zip)
} ap_tiff_compress;

// Encode a raw RGBA buffer as an RGB TIFF file. The alpha channel is
// dropped on output. `rgba_f32` holds linear-float pixels for
// AP_TIFF_FLOAT32; for 8- and 16-bit output pass a uint8 RGBA buffer
// in `rgba_u8` (the other pointer must be NULL). The pixel values are
// expected to be in the output colour space selected by the pipeline's
// output-transfer stage.
//
// `icc_data` / `icc_size` carry an optional ICC profile blob to embed;
// pass NULL / 0 to skip.
//
// Returns 0 on success.
int ap_export_tiff(const uint8_t *rgba_u8, const float *rgba_f32,
                   int width, int height,
                   ap_tiff_depth depth, ap_tiff_compress compress,
                   const uint8_t *icc_data, size_t icc_size,
                   const char *path);

#ifdef __cplusplus
}
#endif

#endif
