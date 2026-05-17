#ifndef APERTURE_OUTPUT_JPEG_H
#define APERTURE_OUTPUT_JPEG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Encode a raw RGBA8 buffer as a baseline JPEG. The alpha channel is
// dropped on the way out (JPEG is RGB only). Pixels are expected to be
// in sRGB (the encode shader's output). `quality` is 0–100.
//
// Returns 0 on success.
int ap_export_jpeg(const uint8_t *rgba, int width, int height,
                   const char *path, int quality);

// Same, but encodes into a freshly malloc'd in-memory buffer instead
// of a file. On success `*out` holds the JPEG bytes (caller frees
// with free()) and `*out_size` its length. Returns 0 on success.
int ap_export_jpeg_mem(const uint8_t *rgba, int width, int height,
                       int quality, uint8_t **out, size_t *out_size);

#ifdef __cplusplus
}
#endif

#endif
