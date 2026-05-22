#ifndef APERTURE_OUTPUT_PNG_H
#define APERTURE_OUTPUT_PNG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// PNG bit depth. 8-bit is display-ready; 16-bit retains more precision
// for archival or round-trip use.
typedef enum {
    AP_PNG_UINT8  = 0,  // 8 bits per channel
    AP_PNG_UINT16 = 1,  // 16 bits per channel
} ap_png_depth;

// Encode a raw RGBA8 buffer as an RGB PNG file. The alpha channel is
// dropped on output. For AP_PNG_UINT16 the 8-bit input values are
// expanded to the 16-bit range by bit replication so that black stays
// 0x0000 and white stays 0xFFFF.
//
// `icc_data` / `icc_size` carry an optional ICC profile blob to embed
// as an iCCP chunk; pass NULL / 0 to skip.
//
// Returns 0 on success.
int ap_export_png(const uint8_t *rgba, int width, int height,
                  ap_png_depth depth,
                  const uint8_t *icc_data, size_t icc_size,
                  const char *path);

#ifdef __cplusplus
}
#endif

#endif
