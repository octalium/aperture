#ifndef APERTURE_IO_RAW_H
#define APERTURE_IO_RAW_H

#include <stdint.h>

typedef struct {
    uint8_t *pixels;
    int      width;
    int      height;
} ap_raw_image;

int  ap_raw_load(const char *path, ap_raw_image *out);
void ap_raw_image_free(ap_raw_image *img);

#endif
