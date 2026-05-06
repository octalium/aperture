#include "raw.h"

#include "core/log.h"

#include <libraw/libraw.h>

#include <stdlib.h>
#include <string.h>

int ap_raw_load(const char *path, ap_raw_image *out)
{
    libraw_data_t *raw = libraw_init(0);
    if (!raw) {
        AP_ERROR("libraw_init failed");
        return -1;
    }

    int err = libraw_open_file(raw, path);
    if (err != LIBRAW_SUCCESS) {
        AP_ERROR("libraw_open_file(%s) -> %s", path, libraw_strerror(err));
        libraw_close(raw);
        return -1;
    }

    err = libraw_unpack(raw);
    if (err != LIBRAW_SUCCESS) {
        AP_ERROR("libraw_unpack(%s) -> %s", path, libraw_strerror(err));
        libraw_close(raw);
        return -1;
    }

    err = libraw_dcraw_process(raw);
    if (err != LIBRAW_SUCCESS) {
        AP_ERROR("libraw_dcraw_process(%s) -> %s", path, libraw_strerror(err));
        libraw_close(raw);
        return -1;
    }

    libraw_processed_image_t *img = libraw_dcraw_make_mem_image(raw, &err);
    if (!img) {
        AP_ERROR("libraw_dcraw_make_mem_image(%s) -> %s", path, libraw_strerror(err));
        libraw_close(raw);
        return -1;
    }

    if (img->type != LIBRAW_IMAGE_BITMAP || img->bits != 8 || img->colors != 3) {
        AP_ERROR("unexpected processed image format: type=%d bits=%d colors=%d",
                 img->type, img->bits, img->colors);
        libraw_dcraw_clear_mem(img);
        libraw_close(raw);
        return -1;
    }

    int w = img->width;
    int h = img->height;
    size_t rgba_size = (size_t)w * (size_t)h * 4;

    uint8_t *rgba = malloc(rgba_size);
    if (!rgba) {
        AP_ERROR("rgba buffer alloc failed (%zu bytes)", rgba_size);
        libraw_dcraw_clear_mem(img);
        libraw_close(raw);
        return -1;
    }

    for (int y = 0; y < h; y++) {
        const uint8_t *src = img->data + (size_t)y * (size_t)w * 3;
        uint8_t       *dst = rgba      + (size_t)y * (size_t)w * 4;
        for (int x = 0; x < w; x++) {
            dst[x * 4 + 0] = src[x * 3 + 0];
            dst[x * 4 + 1] = src[x * 3 + 1];
            dst[x * 4 + 2] = src[x * 3 + 2];
            dst[x * 4 + 3] = 255;
        }
    }

    libraw_dcraw_clear_mem(img);
    libraw_close(raw);

    out->pixels = rgba;
    out->width  = w;
    out->height = h;

    AP_INFO("loaded raw: %s (%dx%d)", path, w, h);
    return 0;
}

void ap_raw_image_free(ap_raw_image *img)
{
    if (!img) return;
    free(img->pixels);
    img->pixels = NULL;
    img->width  = 0;
    img->height = 0;
}
