#include "raw.h"

#include "core/log.h"
#include "photo/metadata.h"

#include <libraw/libraw.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

// Format `value` as %g into the metadata slot. Suffix appended when
// the value is finite and nonzero; otherwise the slot is left empty
// so the UI shows "not in file" instead of "0mm" / "f/0" / etc.
static void set_numeric(ap_photo_metadata *m, ap_meta_field f,
                        double value, const char *suffix)
{
    if (!isfinite(value) || value <= 0.0) return;
    char buf[AP_META_VALUE_LEN];
    snprintf(buf, sizeof(buf), "%g%s", value, suffix ? suffix : "");
    ap_photo_metadata_set(m, f, buf);
}

// Shutter in seconds -> conventional form. <1s renders as the
// reciprocal fraction ("1/250"), >=1s as a plain seconds string
// ("2s"). Cameras don't actually shoot 0.997s, but if a third-party
// raw reports it the printf below keeps two sig figs.
static void set_shutter(ap_photo_metadata *m, double seconds)
{
    if (!isfinite(seconds) || seconds <= 0.0) return;
    char buf[AP_META_VALUE_LEN];
    if (seconds < 1.0) {
        snprintf(buf, sizeof(buf), "1/%g", 1.0 / seconds);
    } else {
        snprintf(buf, sizeof(buf), "%gs", seconds);
    }
    ap_photo_metadata_set(m, AP_META_SHUTTER, buf);
}

// LibRaw gives GPS as Deg/Min/Sec floats + a hemisphere ref char
// ('N'/'S'/'E'/'W'). Render as signed decimal degrees with the
// ref appended for unambiguous parsing.
static void set_gps_degree(ap_photo_metadata *m, ap_meta_field f,
                           const float dms[3], char ref)
{
    double d = dms[0] + dms[1] / 60.0 + dms[2] / 3600.0;
    if (!isfinite(d) || d == 0.0) return;
    char buf[AP_META_VALUE_LEN];
    snprintf(buf, sizeof(buf), "%.6f %c", d, ref ? ref : '?');
    ap_photo_metadata_set(m, f, buf);
}

// Resolve LibRaw's lens-model string with the MakerNote fallback chain.
// Some Nikon / Sony bodies (and many adapted lenses) leave the unified
// `lens.Lens` field blank because the lens model lives only in MakerNote
// tags. Returns 0 and writes a NUL-terminated string into `out` when a
// usable name was assembled; returns -1 otherwise (out is left as "").
static int compose_lens_model(const libraw_data_t *raw,
                              char *out, size_t out_len)
{
    if (out_len == 0) return -1;
    out[0] = '\0';
    const char *m = raw->lens.Lens;
    if (!m || !m[0]) m = raw->lens.makernotes.Lens;
    if (m && m[0]) {
        snprintf(out, out_len, "%s", m);
        return 0;
    }
    const char *pre = raw->lens.makernotes.LensFeatures_pre;
    const char *suf = raw->lens.makernotes.LensFeatures_suf;
    if ((pre && pre[0]) || (suf && suf[0])) {
        snprintf(out, out_len, "%s%s%s",
                 pre ? pre : "",
                 (pre && pre[0] && suf && suf[0]) ? " " : "",
                 suf ? suf : "");
        return 0;
    }
    return -1;
}

static void extract_metadata(libraw_data_t *raw, ap_photo_metadata *m)
{
    ap_photo_metadata_clear(m);

    // ISO 8601-ish "YYYY-MM-DD HH:MM:SS" (local time as recorded).
    if (raw->other.timestamp != 0) {
        struct tm tm_v;
        time_t ts = raw->other.timestamp;
        if (gmtime_r(&ts, &tm_v)) {
            char buf[AP_META_VALUE_LEN];
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_v);
            ap_photo_metadata_set(m, AP_META_DATETIME, buf);
        }
    }

    ap_photo_metadata_set(m, AP_META_CAMERA_MAKE,  raw->idata.make);
    ap_photo_metadata_set(m, AP_META_CAMERA_MODEL, raw->idata.model);
    ap_photo_metadata_set(m, AP_META_LENS_MAKE,    raw->lens.LensMake);

    char lens_buf[AP_META_VALUE_LEN];
    if (compose_lens_model(raw, lens_buf, sizeof(lens_buf)) == 0) {
        ap_photo_metadata_set(m, AP_META_LENS_MODEL, lens_buf);
    }

    set_numeric(m, AP_META_FOCAL_LEN, raw->other.focal_len,   "mm");
    set_numeric(m, AP_META_APERTURE,  raw->other.aperture,    "");
    set_shutter(m, raw->other.shutter);
    set_numeric(m, AP_META_ISO,       raw->other.iso_speed,   "");

    // Aperture conventionally rendered with the "f/" prefix.
    const char *ap_str = ap_photo_metadata_get(m, AP_META_APERTURE);
    if (ap_str && ap_str[0]) {
        char buf[AP_META_VALUE_LEN];
        snprintf(buf, sizeof(buf), "f/%s", ap_str);
        ap_photo_metadata_set(m, AP_META_APERTURE, buf);
    }

    if (raw->other.parsed_gps.gpsparsed) {
        set_gps_degree(m, AP_META_GPS_LAT,
                       raw->other.parsed_gps.latitude,
                       raw->other.parsed_gps.latref);
        set_gps_degree(m, AP_META_GPS_LON,
                       raw->other.parsed_gps.longitude,
                       raw->other.parsed_gps.longref);
        set_numeric(m, AP_META_GPS_ALT,
                    raw->other.parsed_gps.altitude, "m");
    }

    ap_photo_metadata_set(m, AP_META_ARTIST,      raw->other.artist);
    ap_photo_metadata_set(m, AP_META_DESCRIPTION, raw->other.desc);
}

static const char *const RAW_EXTENSIONS[] = {
    ".nef", ".cr2", ".cr3", ".raf", ".arw",
    ".dng", ".orf", ".rw2", ".pef", ".srw",
    NULL,
};

bool ap_raw_is_raw_path(const char *path)
{
    if (!path) {
        return false;
    }
    const char *dot = strrchr(path, '.');
    if (!dot) {
        return false;
    }
    for (int i = 0; RAW_EXTENSIONS[i]; i++) {
        if (strcasecmp(dot, RAW_EXTENSIONS[i]) == 0) {
            return true;
        }
    }
    return false;
}

int ap_raw_capture_time(const char *path, time_t *out)
{
    if (!path || !out) {
        return -1;
    }
    libraw_data_t *raw = libraw_init(0);
    if (!raw) {
        return -1;
    }
    // open_file parses the metadata (incl. other.timestamp); no
    // libraw_unpack — the pixel data isn't needed here.
    int err = libraw_open_file(raw, path);
    if (err != LIBRAW_SUCCESS) {
        libraw_close(raw);
        return -1;
    }
    time_t ts = raw->other.timestamp;
    libraw_close(raw);
    if (ts == 0) {
        return -1;
    }
    *out = ts;
    return 0;
}

int ap_raw_lens_model(const char *path, char *out, size_t out_len)
{
    if (!path || !out || out_len == 0) {
        return -1;
    }
    out[0] = '\0';
    libraw_data_t *raw = libraw_init(0);
    if (!raw) {
        return -1;
    }
    int err = libraw_open_file(raw, path);
    if (err != LIBRAW_SUCCESS) {
        libraw_close(raw);
        return -1;
    }
    int rc = compose_lens_model(raw, out, out_len);
    libraw_close(raw);
    return rc;
}

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

    if (!raw->rawdata.raw_image) {
        AP_ERROR("%s: not a Bayer raw (no raw_image plane); X-Trans / Foveon "
                 "are not yet supported", path);
        libraw_close(raw);
        return -1;
    }

    int raw_w = raw->sizes.raw_width;
    int raw_h = raw->sizes.raw_height;
    int vis_w = raw->sizes.width;
    int vis_h = raw->sizes.height;
    int left  = raw->sizes.left_margin;
    int top   = raw->sizes.top_margin;

    if (vis_w <= 0 || vis_h <= 0 || left + vis_w > raw_w || top + vis_h > raw_h) {
        AP_ERROR("%s: implausible visible region (%dx%d at %d,%d in raw %dx%d)",
                 path, vis_w, vis_h, left, top, raw_w, raw_h);
        libraw_close(raw);
        return -1;
    }

    uint16_t *bayer = malloc((size_t)vis_w * (size_t)vis_h * sizeof(*bayer));
    if (!bayer) {
        AP_ERROR("ap_raw_load: bayer buffer alloc failed (%zu bytes)",
                 (size_t)vis_w * (size_t)vis_h * sizeof(*bayer));
        libraw_close(raw);
        return -1;
    }

    for (int y = 0; y < vis_h; y++) {
        const uint16_t *src = raw->rawdata.raw_image + (size_t)(top + y) * raw_w + left;
        memcpy(bayer + (size_t)y * vis_w, src, (size_t)vis_w * sizeof(*src));
    }

    out->bayer        = bayer;
    out->bayer_width  = vis_w;
    out->bayer_height = vis_h;

    int flip = raw->sizes.flip;
    // Clamp non-rotation EXIF orientations (mirror+rotate) to the
    // closest pure rotation. Rare in practice; surface and refine if
    // someone hits one.
    if (flip != 0 && flip != 3 && flip != 5 && flip != 6) {
        AP_WARN("ap_raw_load: %s has unsupported flip=%d, treating as 0",
                path, flip);
        flip = 0;
    }
    out->meta.flip          = flip;
    out->meta.sensor_width  = vis_w;
    out->meta.sensor_height = vis_h;

    if (flip == 5 || flip == 6) {
        out->width  = vis_h;
        out->height = vis_w;
    } else {
        out->width  = vis_w;
        out->height = vis_h;
    }

    // Channel map at the visible top-left.
    for (int dy = 0; dy < 2; dy++) {
        for (int dx = 0; dx < 2; dx++) {
            out->meta.channel_map[dy * 2 + dx] =
                libraw_COLOR(raw, top + dy, left + dx);
        }
    }

    // Black levels: per-channel cblack[0..3] plus the global offset.
    for (int i = 0; i < 4; i++) {
        out->meta.black_level[i] = (float)raw->color.cblack[i] + (float)raw->color.black;
    }
    out->meta.white_level = (float)raw->color.maximum;

    // Use LibRaw's pre-computed rgb_cam (post-WB camera → sRGB-linear)
    // and pre_mul (the WB that pairs with it). These are derived inside
    // LibRaw from cam_xyz the same way dcraw does - using LibRaw's
    // values directly is the reliable path. rgb_cam is [3][4]; for
    // Bayer we use the leading [3][3].
    for (int i = 0; i < 3; i++) {
        for (int r = 0; r < 3; r++) {
            out->meta.cam_to_srgb[i][r] = raw->color.rgb_cam[i][r];
        }
    }

    for (int i = 0; i < 4; i++) {
        out->meta.wb_mul[i] = raw->color.pre_mul[i];
    }
    if (out->meta.wb_mul[3] == 0.0f) {
        out->meta.wb_mul[3] = out->meta.wb_mul[1];
    }
    // Normalize so the green multiplier is 1.0 (matches the shader's
    // assumption and keeps render values in a stable scale).
    if (out->meta.wb_mul[1] != 0.0f) {
        float g = out->meta.wb_mul[1];
        for (int i = 0; i < 4; i++) {
            out->meta.wb_mul[i] /= g;
        }
        // rgb_cam is calibrated against unnormalized pre_mul; rescale
        // the matrix by the inverse of the green factor we just divided
        // out so the shader's WB step cancels properly.
        for (int i = 0; i < 3; i++) {
            for (int r = 0; r < 3; r++) {
                out->meta.cam_to_srgb[i][r] *= g;
            }
        }
    }

    extract_metadata(raw, &out->file_meta);

    AP_INFO("loaded raw: %s (%dx%d, %s %s)",
            path, vis_w, vis_h,
            raw->idata.make, raw->idata.model);

    libraw_close(raw);
    return 0;
}

void ap_raw_image_free(ap_raw_image *img)
{
    if (!img) return;
    free(img->bayer);
    img->bayer        = NULL;
    img->bayer_width  = 0;
    img->bayer_height = 0;
    img->width        = 0;
    img->height       = 0;
}
