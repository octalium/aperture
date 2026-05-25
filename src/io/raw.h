#ifndef APERTURE_IO_RAW_H
#define APERTURE_IO_RAW_H

#include "photo/metadata.h"

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

// Per-image metadata that downstream stages consume independently of the
// CPU-side raw buffer.
typedef struct {
    // Bayer pattern at the sensor's top-left as a 2x2 tile:
    // channel_map[(y & 1) * 2 + (x & 1)] is the CFA channel sampled at
    // sensor pixel (x, y). Values: 0=R, 1=G, 2=B, 3=G2 (treated as G).
    int   channel_map[4];

    // Per-channel black levels, in the same uint16 scale as the raw pixels.
    float black_level[4];
    // White level (effective max sensor value, same scale).
    float white_level;
    // White-balance multipliers per CFA channel (R, G, B, G2),
    // normalized so green = 1.0.
    float wb_mul[4];
    // Camera-RGB → linear sRGB-primaries 3x3 matrix, row-major.
    float cam_to_srgb[3][3];

    // EXIF / dcraw orientation flip code:
    //   0 = no rotation, 3 = 180°, 5 = 90° CCW, 6 = 90° CW.
    // Mirror-and-rotate values (2/4/5/7 in EXIF terms) are clamped to
    // the closest pure-rotation case at v1.
    int   flip;

    // Sensor dimensions of the bayer plane. The demosaic shader needs
    // these to map output (display-oriented) coordinates back into the
    // sensor-oriented Bayer texture for each lookup.
    int   sensor_width;
    int   sensor_height;

    // Lens nominal range, derived by libraw from EXIF LensInfo (0xA432).
    // Zero when the source did not carry LensInfo. Consumed by lens_match
    // to narrow the Lensfun candidate set when the EXIF lens model string
    // is ambiguous.
    float lens_min_focal;
    float lens_max_focal;
    float lens_min_aperture;
} ap_raw_metadata;

typedef struct {
    uint16_t          *bayer;        // raw Bayer plane, bayer_width × bayer_height
    int                bayer_width;  // sensor visible region (matches `bayer`)
    int                bayer_height;
    int                width;        // display dimensions (post-orientation)
    int                height;
    ap_raw_metadata    meta;         // pipeline-side metadata (CFA, WB, ...)
    ap_photo_metadata  file_meta;    // display-side metadata extracted from
                                     // the file (camera, lens, exposure, ...)
} ap_raw_image;

int  ap_raw_load(const char *path, ap_raw_image *out);
void ap_raw_image_free(ap_raw_image *img);

// Read just the capture timestamp from a raw file's metadata — parses
// the file header without decoding pixels. Writes *out and returns 0
// on success; returns non-zero when the file can't be read or carries
// no timestamp.
int  ap_raw_capture_time(const char *path, time_t *out);

// Read the EXIF lens model from a raw file's header — same
// header-only path as ap_raw_capture_time, so bulk operations over a
// selection don't pay the full pixel-decode cost. Writes a
// NUL-terminated string into `out` (truncated to fit) and returns 0
// on success. Returns non-zero when the file can't be read or carries
// no usable lens-model string; `out` is set to "" in that case.
int  ap_raw_lens_model(const char *path, char *out, size_t out_len);

// True when `path` (or a bare filename) ends in a supported raw-file
// extension. The single source of truth for which formats the app
// treats as raw photos.
bool ap_raw_is_raw_path(const char *path);

#endif
