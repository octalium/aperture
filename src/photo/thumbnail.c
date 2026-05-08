#include "thumbnail.h"

#include "core/log.h"
#include "gpu/texture.h"

#include <libraw/libraw.h>

#include <jpeglib.h>
#include <setjmp.h>

#include <stdlib.h>
#include <string.h>

struct ap_thumbnail {
    ap_texture *tex;
};

struct jpeg_error_jmp {
    struct jpeg_error_mgr base;
    jmp_buf jump;
};

static void on_jpeg_error_exit(j_common_ptr cinfo)
{
    char msg[JMSG_LENGTH_MAX];
    (*cinfo->err->format_message)(cinfo, msg);
    AP_ERROR("thumb jpeg: %s", msg);
    struct jpeg_error_jmp *err = (struct jpeg_error_jmp *)cinfo->err;
    longjmp(err->jump, 1);
}

// Decode a JPEG byte buffer to a freshly malloc'd RGBA8 buffer.
// Caller frees `*out_pixels`.
static int decode_jpeg_to_rgba(const uint8_t *jpeg, size_t jpeg_size,
                               uint8_t **out_pixels, int *out_w, int *out_h)
{
    struct jpeg_decompress_struct cinfo = {0};
    struct jpeg_error_jmp err;
    cinfo.err = jpeg_std_error(&err.base);
    err.base.error_exit = on_jpeg_error_exit;

    uint8_t *rgba = NULL;
    uint8_t *row  = NULL;
    int rc = -1;

    if (setjmp(err.jump) != 0) {
        goto done;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, jpeg, jpeg_size);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        AP_ERROR("thumb: jpeg_read_header failed");
        goto done;
    }
    cinfo.out_color_space = JCS_RGB;
    if (!jpeg_start_decompress(&cinfo)) {
        AP_ERROR("thumb: jpeg_start_decompress failed");
        goto done;
    }

    int w  = (int)cinfo.output_width;
    int h  = (int)cinfo.output_height;
    int nc = cinfo.output_components;
    if (w <= 0 || h <= 0 || nc != 3) {
        AP_ERROR("thumb: unexpected jpeg dims %dx%d ch=%d", w, h, nc);
        goto done;
    }

    rgba = malloc((size_t)w * (size_t)h * 4u);
    row  = malloc((size_t)w * 3u);
    if (!rgba || !row) {
        AP_ERROR("thumb: scanline alloc failed");
        goto done;
    }

    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW p = row;
        if (jpeg_read_scanlines(&cinfo, &p, 1) != 1) {
            AP_ERROR("thumb: jpeg_read_scanlines short");
            goto done;
        }
        size_t y = cinfo.output_scanline - 1;
        uint8_t *dst = rgba + y * (size_t)w * 4u;
        for (int x = 0; x < w; x++) {
            dst[x * 4 + 0] = row[x * 3 + 0];
            dst[x * 4 + 1] = row[x * 3 + 1];
            dst[x * 4 + 2] = row[x * 3 + 2];
            dst[x * 4 + 3] = 0xFFu;
        }
    }

    jpeg_finish_decompress(&cinfo);
    *out_pixels = rgba;
    *out_w = w;
    *out_h = h;
    rgba = NULL;
    rc = 0;

done:
    free(row);
    free(rgba);
    jpeg_destroy_decompress(&cinfo);
    return rc;
}

// Convert a libraw bitmap (RGB or RGBA, 8 or 16 bit) into a fresh
// RGBA8 buffer. Caller frees.
static int bitmap_to_rgba(const libraw_processed_image_t *img,
                          uint8_t **out_pixels, int *out_w, int *out_h)
{
    if (img->bits != 8) {
        AP_ERROR("thumb: bitmap bits=%u unsupported (only 8)", img->bits);
        return -1;
    }
    int w = img->width;
    int h = img->height;
    int c = img->colors;
    if (w <= 0 || h <= 0 || (c != 3 && c != 4)) {
        AP_ERROR("thumb: bitmap dims/channels %dx%d c=%d", w, h, c);
        return -1;
    }
    uint8_t *rgba = malloc((size_t)w * (size_t)h * 4u);
    if (!rgba) {
        AP_ERROR("thumb: rgba alloc failed");
        return -1;
    }
    for (int y = 0; y < h; y++) {
        const uint8_t *src = img->data + (size_t)y * (size_t)w * (size_t)c;
        uint8_t       *dst = rgba      + (size_t)y * (size_t)w * 4u;
        for (int x = 0; x < w; x++) {
            dst[x * 4 + 0] = src[x * c + 0];
            dst[x * 4 + 1] = src[x * c + 1];
            dst[x * 4 + 2] = src[x * c + 2];
            dst[x * 4 + 3] = c == 4 ? src[x * c + 3] : 0xFFu;
        }
    }
    *out_pixels = rgba;
    *out_w = w;
    *out_h = h;
    return 0;
}

ap_thumbnail *ap_thumbnail_create(ap_gpu *g, const char *path)
{
    if (!g || !path) {
        AP_ERROR("ap_thumbnail_create: invalid args");
        return NULL;
    }

    libraw_data_t *raw = libraw_init(0);
    if (!raw) {
        AP_ERROR("thumb: libraw_init failed");
        return NULL;
    }

    int err = libraw_open_file(raw, path);
    if (err != LIBRAW_SUCCESS) {
        AP_ERROR("thumb: libraw_open_file(%s) -> %s", path, libraw_strerror(err));
        libraw_close(raw);
        return NULL;
    }
    err = libraw_unpack_thumb(raw);
    if (err != LIBRAW_SUCCESS) {
        AP_ERROR("thumb: libraw_unpack_thumb(%s) -> %s", path, libraw_strerror(err));
        libraw_close(raw);
        return NULL;
    }

    int errc = 0;
    libraw_processed_image_t *img = libraw_dcraw_make_mem_thumb(raw, &errc);
    if (!img || errc != LIBRAW_SUCCESS) {
        AP_ERROR("thumb: libraw_dcraw_make_mem_thumb(%s) errc=%d", path, errc);
        if (img) libraw_dcraw_clear_mem(img);
        libraw_close(raw);
        return NULL;
    }

    uint8_t *rgba = NULL;
    int w = 0, h = 0;
    int rc;
    if (img->type == LIBRAW_IMAGE_JPEG) {
        rc = decode_jpeg_to_rgba(img->data, img->data_size, &rgba, &w, &h);
    } else if (img->type == LIBRAW_IMAGE_BITMAP) {
        rc = bitmap_to_rgba(img, &rgba, &w, &h);
    } else {
        AP_ERROR("thumb: %s embedded preview type %d unsupported",
                 path, (int)img->type);
        rc = -1;
    }
    libraw_dcraw_clear_mem(img);
    libraw_close(raw);
    if (rc != 0) {
        return NULL;
    }

    ap_thumbnail *t = calloc(1, sizeof(*t));
    if (!t) {
        AP_ERROR("ap_thumbnail_create: out of memory");
        free(rgba);
        return NULL;
    }
    t->tex = ap_texture_create_rgba8_srgb(g, rgba, w, h);
    free(rgba);
    if (!t->tex) {
        free(t);
        return NULL;
    }
    return t;
}

void ap_thumbnail_destroy(ap_thumbnail *t)
{
    if (!t) return;
    if (t->tex) ap_texture_destroy(t->tex);
    free(t);
}

VkImageView ap_thumbnail_view(const ap_thumbnail *t)
{
    return t ? ap_texture_view(t->tex) : VK_NULL_HANDLE;
}

VkSampler ap_thumbnail_sampler(const ap_thumbnail *t)
{
    return t ? ap_texture_sampler(t->tex) : VK_NULL_HANDLE;
}

int ap_thumbnail_width(const ap_thumbnail *t)
{
    return t ? ap_texture_width(t->tex) : 0;
}

int ap_thumbnail_height(const ap_thumbnail *t)
{
    return t ? ap_texture_height(t->tex) : 0;
}
