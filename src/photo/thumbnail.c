#include "thumbnail.h"

#include "core/log.h"
#include "gpu/texture.h"
#include "output/jpeg.h"

#include <libraw/libraw.h>

#include <jpeglib.h>
#include <setjmp.h>

#include <stdint.h>
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

// Embedded NEF/CR2/ARW previews are commonly the full-size camera
// JPEG (e.g. 7360×4912 for D800E). Decoding that wholesale and
// uploading it as a thumbnail texture eats minutes of GPU memory for
// no win. Use libjpeg's DCT scale to keep decode cheap, then CPU
// box-resample to a small target size before upload.
#define THUMB_TARGET_MAX_DIM 384

static void target_dims_for(int src_w, int src_h, int *out_w, int *out_h)
{
    int longest = src_w > src_h ? src_w : src_h;
    if (longest <= THUMB_TARGET_MAX_DIM) {
        *out_w = src_w;
        *out_h = src_h;
        return;
    }
    double s = (double)THUMB_TARGET_MAX_DIM / (double)longest;
    int dw = (int)(src_w * s + 0.5);
    int dh = (int)(src_h * s + 0.5);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    *out_w = dw;
    *out_h = dh;
}

// Rotate an RGBA8 buffer to honor a LibRaw flip code.
//   0 = identity (no allocation, ownership transferred via pass-through)
//   3 = 180 degrees
//   5 = 90 CCW (dims swap)
//   6 = 90 CW (dims swap)
// On non-identity flips, allocates a fresh `*out_pixels` and frees the
// caller's `src` (so the caller can pass ownership in and forget).
// Returns 0 on success.
static int rotate_rgba(uint8_t *src, int src_w, int src_h, int flip,
                       uint8_t **out_pixels, int *out_w, int *out_h)
{
    if (flip == 0) {
        *out_pixels = src;
        *out_w = src_w;
        *out_h = src_h;
        return 0;
    }

    int dst_w, dst_h;
    if (flip == 5 || flip == 6) { dst_w = src_h; dst_h = src_w; }
    else                        { dst_w = src_w; dst_h = src_h; }

    uint8_t *dst = malloc((size_t)dst_w * (size_t)dst_h * 4u);
    if (!dst) {
        AP_ERROR("thumb: rotate alloc failed");
        return -1;
    }

    for (int dy = 0; dy < dst_h; dy++) {
        for (int dx = 0; dx < dst_w; dx++) {
            int sx, sy;
            switch (flip) {
                case 3: sx = src_w - 1 - dx;     sy = src_h - 1 - dy;     break;
                case 5: sx = src_w - 1 - dy;     sy = dx;                 break;
                case 6: sx = dy;                 sy = src_h - 1 - dx;     break;
                default: sx = dx;                sy = dy;                 break;
            }
            const uint8_t *s = src + ((size_t)sy * src_w + sx) * 4u;
            uint8_t       *d = dst + ((size_t)dy * dst_w + dx) * 4u;
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
        }
    }

    free(src);
    *out_pixels = dst;
    *out_w = dst_w;
    *out_h = dst_h;
    return 0;
}

// Box-filter downsample of an RGBA8 buffer. Allocates a fresh
// `*out_pixels`; caller frees. Returns 0 on success.
static int downsample_rgba(const uint8_t *src, int src_w, int src_h,
                           uint8_t **out_pixels, int *out_w, int *out_h)
{
    int dw, dh;
    target_dims_for(src_w, src_h, &dw, &dh);
    if (dw == src_w && dh == src_h) {
        size_t bytes = (size_t)src_w * (size_t)src_h * 4u;
        uint8_t *copy = malloc(bytes);
        if (!copy) return -1;
        memcpy(copy, src, bytes);
        *out_pixels = copy;
        *out_w = src_w;
        *out_h = src_h;
        return 0;
    }
    uint8_t *dst = malloc((size_t)dw * (size_t)dh * 4u);
    if (!dst) {
        AP_ERROR("thumb: downsample alloc failed");
        return -1;
    }

    for (int dy = 0; dy < dh; dy++) {
        int y0 = (int)((int64_t)dy       * src_h / dh);
        int y1 = (int)((int64_t)(dy + 1) * src_h / dh);
        if (y1 <= y0) y1 = y0 + 1;
        for (int dx = 0; dx < dw; dx++) {
            int x0 = (int)((int64_t)dx       * src_w / dw);
            int x1 = (int)((int64_t)(dx + 1) * src_w / dw);
            if (x1 <= x0) x1 = x0 + 1;

            uint32_t r = 0, g = 0, b = 0;
            uint32_t n = 0;
            for (int y = y0; y < y1; y++) {
                const uint8_t *row = src + ((size_t)y * src_w + x0) * 4u;
                for (int x = x0; x < x1; x++) {
                    r += *row++;
                    g += *row++;
                    b += *row++;
                    row++;          // skip A
                    n++;
                }
            }
            uint8_t *p = dst + ((size_t)dy * dw + dx) * 4u;
            p[0] = (uint8_t)(r / n);
            p[1] = (uint8_t)(g / n);
            p[2] = (uint8_t)(b / n);
            p[3] = 0xFFu;
        }
    }
    *out_pixels = dst;
    *out_w = dw;
    *out_h = dh;
    return 0;
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
    // Pick a DCT scale denominator (1, 2, 4, 8 for libjpeg-turbo) that
    // brings the longest side under THUMB_TARGET_MAX_DIM*2 - we'll
    // box-resample the rest of the way after decoding.
    cinfo.scale_num   = 1;
    cinfo.scale_denom = 1;
    {
        int longest = (int)(cinfo.image_width > cinfo.image_height
                            ? cinfo.image_width : cinfo.image_height);
        int target  = THUMB_TARGET_MAX_DIM * 2;
        for (int den = 8; den >= 2; den /= 2) {
            if (longest / den >= target) { cinfo.scale_denom = (unsigned)den; break; }
        }
    }
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

int ap_thumbnail_decode_jpeg(const uint8_t *jpeg, size_t jpeg_size,
                             uint8_t **out_pixels, int *out_w, int *out_h)
{
    if (!jpeg || jpeg_size == 0 || !out_pixels || !out_w || !out_h) {
        AP_ERROR("ap_thumbnail_decode_jpeg: invalid args");
        return -1;
    }
    // The blob is already at (or near) thumbnail size, so the DCT
    // scale stays at 1; the box-resample is a safety net for the
    // rare case it's slightly over the target.
    uint8_t *rgba = NULL;
    int w = 0, h = 0;
    if (decode_jpeg_to_rgba(jpeg, jpeg_size, &rgba, &w, &h) != 0) return -1;

    uint8_t *small = NULL;
    int sw = 0, sh = 0;
    int rc = downsample_rgba(rgba, w, h, &small, &sw, &sh);
    free(rgba);
    if (rc != 0) return -1;

    *out_pixels = small;
    *out_w = sw;
    *out_h = sh;
    return 0;
}

int ap_thumbnail_encode_jpeg(const uint8_t *rgba, int width, int height,
                             uint8_t **out_jpeg, size_t *out_size)
{
    if (!rgba || width <= 0 || height <= 0 || !out_jpeg || !out_size) {
        AP_ERROR("ap_thumbnail_encode_jpeg: invalid args");
        return -1;
    }
    uint8_t *small = NULL;
    int sw = 0, sh = 0;
    if (downsample_rgba(rgba, width, height, &small, &sw, &sh) != 0) {
        return -1;
    }
    int rc = ap_export_jpeg_mem(small, sw, sh, 88, out_jpeg, out_size);
    free(small);
    return rc;
}

int ap_thumbnail_decode_cpu(const char *path,
                            uint8_t **out_pixels, int *out_w, int *out_h)
{
    if (!path || !out_pixels || !out_w || !out_h) {
        AP_ERROR("ap_thumbnail_decode_cpu: invalid args");
        return -1;
    }

    libraw_data_t *raw = libraw_init(0);
    if (!raw) {
        AP_ERROR("thumb: libraw_init failed");
        return -1;
    }

    int err = libraw_open_file(raw, path);
    if (err != LIBRAW_SUCCESS) {
        AP_ERROR("thumb: libraw_open_file(%s) -> %s", path, libraw_strerror(err));
        libraw_close(raw);
        return -1;
    }
    int flip = raw->sizes.flip;
    if (flip != 0 && flip != 3 && flip != 5 && flip != 6) flip = 0;
    err = libraw_unpack_thumb(raw);
    if (err != LIBRAW_SUCCESS) {
        AP_ERROR("thumb: libraw_unpack_thumb(%s) -> %s", path, libraw_strerror(err));
        libraw_close(raw);
        return -1;
    }

    int errc = 0;
    libraw_processed_image_t *img = libraw_dcraw_make_mem_thumb(raw, &errc);
    if (!img || errc != LIBRAW_SUCCESS) {
        AP_ERROR("thumb: libraw_dcraw_make_mem_thumb(%s) errc=%d", path, errc);
        if (img) libraw_dcraw_clear_mem(img);
        libraw_close(raw);
        return -1;
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
    if (rc != 0) return -1;

    uint8_t *small = NULL;
    int sw = 0, sh = 0;
    rc = downsample_rgba(rgba, w, h, &small, &sw, &sh);
    free(rgba);
    if (rc != 0) return -1;

    // Apply EXIF orientation last (cheapest after downsampling). On
    // identity the buffer is passed through; otherwise rotate_rgba
    // owns and frees `small`.
    uint8_t *oriented = NULL;
    int     ow = 0, oh = 0;
    if (rotate_rgba(small, sw, sh, flip, &oriented, &ow, &oh) != 0) {
        free(small);
        return -1;
    }

    *out_pixels = oriented;
    *out_w = ow;
    *out_h = oh;
    return 0;
}

ap_thumbnail *ap_thumbnail_upload(ap_gpu *g, const uint8_t *rgba,
                                  int width, int height)
{
    if (!g || !rgba || width <= 0 || height <= 0) {
        AP_ERROR("ap_thumbnail_upload: invalid args");
        return NULL;
    }
    ap_thumbnail *t = calloc(1, sizeof(*t));
    if (!t) {
        AP_ERROR("ap_thumbnail_upload: out of memory");
        return NULL;
    }
    t->tex = ap_texture_create_rgba8_srgb(g, rgba, width, height);
    if (!t->tex) {
        free(t);
        return NULL;
    }
    return t;
}

ap_thumbnail *ap_thumbnail_create(ap_gpu *g, const char *path)
{
    uint8_t *rgba = NULL;
    int w = 0, h = 0;
    if (ap_thumbnail_decode_cpu(path, &rgba, &w, &h) != 0) {
        return NULL;
    }
    ap_thumbnail *t = ap_thumbnail_upload(g, rgba, w, h);
    free(rgba);
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
