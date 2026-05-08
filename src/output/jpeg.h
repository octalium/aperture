#ifndef APERTURE_OUTPUT_JPEG_H
#define APERTURE_OUTPUT_JPEG_H

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

#ifdef __cplusplus
}
#endif

#endif
