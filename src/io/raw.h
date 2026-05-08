#ifndef APERTURE_IO_RAW_H
#define APERTURE_IO_RAW_H

#include <stdint.h>

// Per-image metadata that downstream stages (demosaic, future WB
// sliders, lens correction, color profile selection) consume
// independently of the CPU-side raw buffer.
typedef struct {
    // Bayer pattern at the visible top-left as a 2x2 tile:
    // channel_map[(y & 1) * 2 + (x & 1)] is the CFA channel sampled at
    // pixel (x, y). Values: 0 = R, 1 = G, 2 = B, 3 = G2 (treated as G).
    int   channel_map[4];

    // Per-channel black levels, in the same uint16 scale as the raw
    // pixels. R, G, B, G2.
    float black_level[4];

    // White level (effective max sensor value, same scale).
    float white_level;

    // Daylight WB multipliers per CFA channel (R, G, B, G2),
    // normalized so the green multiplier is 1.0.
    float wb_mul[4];

    // Camera-RGB → linear sRGB-primaries 3x3 matrix, row-major.
    // Composed on the CPU as SRGB_FROM_XYZ · cam_xyz.
    float cam_to_srgb[3][3];
} ap_raw_metadata;

typedef struct {
    uint16_t       *bayer;     // visible-region single-channel Bayer pixels
    int             width;     // visible width
    int             height;    // visible height
    ap_raw_metadata meta;
} ap_raw_image;

int  ap_raw_load(const char *path, ap_raw_image *out);
void ap_raw_image_free(ap_raw_image *img);

#endif
