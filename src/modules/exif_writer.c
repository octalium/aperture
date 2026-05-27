#include "exif_writer.h"

#include "core/log.h"

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Dynamic byte buffer
typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} buf_t;

static int buf_grow(buf_t *b, size_t need)
{
    if (b->len + need <= b->cap) return 0;
    size_t cap = b->cap ? b->cap * 2 : 256;
    while (cap < b->len + need) cap *= 2;
    uint8_t *p = realloc(b->data, cap);
    if (!p) return -1;
    b->data = p;
    b->cap  = cap;
    return 0;
}

static int buf_append(buf_t *b, const void *src, size_t n)
{
    if (buf_grow(b, n) < 0) return -1;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

static int buf_append_u8(buf_t *b, uint8_t v)
{
    return buf_append(b, &v, 1);
}

static int buf_append_u16le(buf_t *b, uint16_t v)
{
    uint8_t bytes[2] = { (uint8_t)(v), (uint8_t)(v >> 8) };
    return buf_append(b, bytes, 2);
}

static int buf_append_u32le(buf_t *b, uint32_t v)
{
    uint8_t bytes[4] = { (uint8_t)(v), (uint8_t)(v >> 8),
                         (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    return buf_append(b, bytes, 4);
}

// TIFF / EXIF constants

// TIFF tag types
#define TIFF_TYPE_BYTE      1
#define TIFF_TYPE_ASCII     2
#define TIFF_TYPE_SHORT     3
#define TIFF_TYPE_LONG      4
#define TIFF_TYPE_RATIONAL  5   // two LONGs: numerator / denominator
#define TIFF_TYPE_SBYTE     6
#define TIFF_TYPE_SRATIONAL 10  // two SLONGs

// IFD0 tags
#define TAG_IMAGE_DESC      0x010E
#define TAG_MAKE            0x010F
#define TAG_MODEL           0x0110
#define TAG_ORIENTATION     0x0112
#define TAG_DATETIME        0x0132
#define TAG_ARTIST          0x013B
#define TAG_EXIF_IFD        0x8769  // offset to Exif sub-IFD
#define TAG_GPS_IFD         0x8825  // offset to GPS sub-IFD

// Exif sub-IFD tags
#define TAG_EXPOSURE_TIME   0x829A
#define TAG_FNUMBER         0x829D
#define TAG_ISO             0x8827
#define TAG_DATETIME_ORIG   0x9003
#define TAG_FOCAL_LEN       0x920A
#define TAG_LENS_MAKE       0xA433
#define TAG_LENS_MODEL      0xA434

// GPS sub-IFD tags
#define TAG_GPS_LAT_REF     0x0001
#define TAG_GPS_LAT         0x0002
#define TAG_GPS_LON_REF     0x0003
#define TAG_GPS_LON         0x0004
#define TAG_GPS_ALT_REF     0x0005
#define TAG_GPS_ALT         0x0006

// IFD entry builder

// An IFD entry record before its value is resolved to an offset.
typedef struct {
    uint16_t tag;
    uint16_t type;
    uint32_t count;
    // When the value fits in 4 bytes it is stored inline (inline_val).
    // Otherwise it is an offset into the value-data area.
    int      has_offset;  // 0 = inline, 1 = offset into vdata
    uint32_t inline_val;
    size_t   vdata_off;   // byte offset within the vdata buf
    size_t   vdata_len;
} ifd_entry;

#define MAX_IFD_ENTRIES 32

typedef struct {
    ifd_entry entries[MAX_IFD_ENTRIES];
    int       count;
    buf_t     vdata;  // value data area for out-of-line values
} ifd_builder;

static void ifd_init(ifd_builder *b)
{
    memset(b, 0, sizeof(*b));
}

static void ifd_free(ifd_builder *b)
{
    free(b->vdata.data);
}

// Add an ASCII entry. `str` must be non-NULL and non-empty.
static int ifd_add_ascii(ifd_builder *b, uint16_t tag, const char *str)
{
    if (!str || *str == '\0') return 0;
    if (b->count >= MAX_IFD_ENTRIES) return -1;
    size_t len = strlen(str) + 1; // include NUL
    ifd_entry *e = &b->entries[b->count++];
    e->tag   = tag;
    e->type  = TIFF_TYPE_ASCII;
    e->count = (uint32_t)len;
    if (len <= 4) {
        e->has_offset = 0;
        memcpy(&e->inline_val, str, len);
    } else {
        e->has_offset = 1;
        e->vdata_off  = b->vdata.len;
        e->vdata_len  = len;
        if (buf_append(&b->vdata, str, len) < 0) return -1;
    }
    return 0;
}

// Add a SHORT entry (single 16-bit unsigned value).
static int ifd_add_short(ifd_builder *b, uint16_t tag, uint16_t val)
{
    if (b->count >= MAX_IFD_ENTRIES) return -1;
    ifd_entry *e = &b->entries[b->count++];
    e->tag        = tag;
    e->type       = TIFF_TYPE_SHORT;
    e->count      = 1;
    e->has_offset = 0;
    e->inline_val = val;   // little-endian: 16-bit value padded to 32
    return 0;
}

// Add a LONG entry (single 32-bit unsigned value, inline).
static int ifd_add_long(ifd_builder *b, uint16_t tag, uint32_t val)
{
    if (b->count >= MAX_IFD_ENTRIES) return -1;
    ifd_entry *e = &b->entries[b->count++];
    e->tag        = tag;
    e->type       = TIFF_TYPE_LONG;
    e->count      = 1;
    e->has_offset = 0;
    e->inline_val = val;
    return 0;
}

// Add a RATIONAL entry (numerator / denominator, always out-of-line).
static int ifd_add_rational(ifd_builder *b, uint16_t tag,
                            uint32_t numer, uint32_t denom)
{
    if (b->count >= MAX_IFD_ENTRIES) return -1;
    ifd_entry *e = &b->entries[b->count++];
    e->tag        = tag;
    e->type       = TIFF_TYPE_RATIONAL;
    e->count      = 1;
    e->has_offset = 1;
    e->vdata_off  = b->vdata.len;
    e->vdata_len  = 8;
    if (buf_append_u32le(&b->vdata, numer) < 0) return -1;
    if (buf_append_u32le(&b->vdata, denom) < 0) return -1;
    return 0;
}

// Add a RATIONAL[3] entry for GPS degrees-minutes-seconds.
static int ifd_add_gps_coord(ifd_builder *b, uint16_t tag,
                              uint32_t deg_n, uint32_t deg_d,
                              uint32_t min_n, uint32_t min_d,
                              uint32_t sec_n, uint32_t sec_d)
{
    if (b->count >= MAX_IFD_ENTRIES) return -1;
    ifd_entry *e = &b->entries[b->count++];
    e->tag        = tag;
    e->type       = TIFF_TYPE_RATIONAL;
    e->count      = 3;
    e->has_offset = 1;
    e->vdata_off  = b->vdata.len;
    e->vdata_len  = 24;
    if (buf_append_u32le(&b->vdata, deg_n) < 0) return -1;
    if (buf_append_u32le(&b->vdata, deg_d) < 0) return -1;
    if (buf_append_u32le(&b->vdata, min_n) < 0) return -1;
    if (buf_append_u32le(&b->vdata, min_d) < 0) return -1;
    if (buf_append_u32le(&b->vdata, sec_n) < 0) return -1;
    if (buf_append_u32le(&b->vdata, sec_d) < 0) return -1;
    return 0;
}

// Add a BYTE entry for a single 1-byte value (used for GPS sub-tags).
static int ifd_add_byte(ifd_builder *b, uint16_t tag, uint8_t val)
{
    if (b->count >= MAX_IFD_ENTRIES) return -1;
    ifd_entry *e = &b->entries[b->count++];
    e->tag        = tag;
    e->type       = TIFF_TYPE_BYTE;
    e->count      = 1;
    e->has_offset = 0;
    e->inline_val = val;
    return 0;
}

// Serialize the IFD into `out` at the current write position. `ifd_start`
// is the absolute byte offset from the TIFF header start where this IFD
// begins; used to compute correct value offsets. `vdata_offset` is the
// absolute offset where this IFD's out-of-line value data will be placed.
// After the IFD directory entries (and optional next-IFD pointer) the
// vdata is appended directly. The function writes into `out` and returns
// the total bytes written (IFD dir + vdata), or -1 on error.
static int ifd_emit(const ifd_builder *b, buf_t *out,
                    uint32_t next_ifd_offset)
{
    // Sort entries by tag (TIFF spec requires ascending tag order).
    // Copy into a local array for sorting to avoid modifying the builder.
    ifd_entry sorted[MAX_IFD_ENTRIES];
    memcpy(sorted, b->entries, (size_t)b->count * sizeof(ifd_entry));
    for (int i = 0; i < b->count - 1; i++) {
        for (int j = i + 1; j < b->count; j++) {
            if (sorted[j].tag < sorted[i].tag) {
                ifd_entry tmp = sorted[i];
                sorted[i] = sorted[j];
                sorted[j] = tmp;
            }
        }
    }

    // The IFD directory starts here in `out`.
    size_t dir_start = out->len;

    // IFD entry count.
    if (buf_append_u16le(out, (uint16_t)b->count) < 0) return -1;

    // Each entry is 12 bytes. Emit with placeholder offsets; we'll patch
    // them after computing where vdata lands.
    // Directory size: 2 (count) + count * 12 + 4 (next IFD offset).
    size_t dir_end = dir_start + 2 + (size_t)b->count * 12 + 4;
    // vdata starts immediately after the directory.
    size_t vdata_abs = dir_end;

    for (int i = 0; i < b->count; i++) {
        const ifd_entry *e = &sorted[i];
        if (buf_append_u16le(out, e->tag)   < 0) return -1;
        if (buf_append_u16le(out, e->type)  < 0) return -1;
        if (buf_append_u32le(out, e->count) < 0) return -1;
        if (!e->has_offset) {
            if (buf_append_u32le(out, e->inline_val) < 0) return -1;
        } else {
            uint32_t off = (uint32_t)(vdata_abs + e->vdata_off);
            if (buf_append_u32le(out, off) < 0) return -1;
        }
    }

    // Next IFD offset.
    if (buf_append_u32le(out, next_ifd_offset) < 0) return -1;

    // Append vdata.
    if (b->vdata.len > 0) {
        if (buf_append(out, b->vdata.data, b->vdata.len) < 0) return -1;
    }

    return 0;
}

// Metadata field parsers

// Parse a rational string of the form "n/d" or a plain float.
// Returns true on success; numer and denom are set to a reduced fraction.
static int parse_rational(const char *s, uint32_t *numer, uint32_t *denom)
{
    if (!s || *s == '\0') return 0;
    // Try "n/d" form first.
    unsigned long n = 0, d = 0;
    if (sscanf(s, "%lu/%lu", &n, &d) == 2 && d > 0) {
        *numer = (uint32_t)n;
        *denom = (uint32_t)d;
        return 1;
    }
    // Try plain float (e.g. "50.0", "2.8").
    double v = 0.0;
    if (sscanf(s, "%lf", &v) == 1 && v >= 0.0) {
        // Represent as N/1000 to preserve two decimal places.
        *numer = (uint32_t)llround(v * 1000.0);
        *denom = 1000;
        return 1;
    }
    return 0;
}

// Parse "f/2.8" or "2.8" → FNumber rational.
static int parse_fnumber(const char *s, uint32_t *numer, uint32_t *denom)
{
    if (!s || *s == '\0') return 0;
    const char *p = s;
    if (*p == 'f' || *p == 'F') {
        p++;
        if (*p == '/') p++;
    }
    return parse_rational(p, numer, denom);
}

// Parse ISO "200" → uint16.
static int parse_iso(const char *s, uint16_t *out)
{
    if (!s || *s == '\0') return 0;
    unsigned long v = 0;
    if (sscanf(s, "%lu", &v) != 1 || v == 0) return 0;
    *out = (uint16_t)(v > 65535 ? 65535 : v);
    return 1;
}

// Parse "200mm" or "200" → focal length rational (in mm).
static int parse_focal(const char *s, uint32_t *numer, uint32_t *denom)
{
    if (!s || *s == '\0') return 0;
    double v = 0.0;
    if (sscanf(s, "%lf", &v) != 1 || v <= 0.0) return 0;
    *numer = (uint32_t)llround(v * 10.0);
    *denom = 10;
    return 1;
}

// Parse a GPS coordinate string in decimal degrees, e.g. "48.858222" or
// "-2.294524". Produces degrees/minutes/seconds rationals with 1/1000
// second precision. The sign indicates hemisphere — caller must pass abs.
static int parse_gps_decimal(const char *s, double *deg_out)
{
    if (!s || *s == '\0') return 0;
    double v = 0.0;
    if (sscanf(s, "%lf", &v) != 1) return 0;
    *deg_out = v;
    return 1;
}

// Convert decimal degrees to DMS rationals (all positive; sign handled
// by the Ref tag).
static void deg_to_dms(double abs_deg,
                       uint32_t *deg_n, uint32_t *deg_d,
                       uint32_t *min_n, uint32_t *min_d,
                       uint32_t *sec_n, uint32_t *sec_d)
{
    int d = (int)abs_deg;
    double rem_m = (abs_deg - d) * 60.0;
    int    m     = (int)rem_m;
    double rem_s = (rem_m - m) * 60.0;
    uint32_t s   = (uint32_t)llround(rem_s * 1000.0);
    *deg_n = (uint32_t)d; *deg_d = 1;
    *min_n = (uint32_t)m; *min_d = 1;
    *sec_n = s;           *sec_d = 1000;
}

// Parse GPS altitude "123.4m" or "123.4". Returns decimal meters.
static int parse_gps_alt(const char *s, double *out)
{
    if (!s || *s == '\0') return 0;
    double v = 0.0;
    if (sscanf(s, "%lf", &v) != 1) return 0;
    *out = v;
    return 1;
}

// Main builder

int ap_exif_build(const ap_photo_metadata *meta,
                  uint8_t **out, size_t *out_size)
{
    if (!meta || !out || !out_size) return -1;

    const char *datetime    = ap_photo_metadata_get(meta, AP_META_DATETIME);
    const char *make        = ap_photo_metadata_get(meta, AP_META_CAMERA_MAKE);
    const char *model       = ap_photo_metadata_get(meta, AP_META_CAMERA_MODEL);
    const char *lens_make   = ap_photo_metadata_get(meta, AP_META_LENS_MAKE);
    const char *lens_model  = ap_photo_metadata_get(meta, AP_META_LENS_MODEL);
    const char *focal_str   = ap_photo_metadata_get(meta, AP_META_FOCAL_LEN);
    const char *aperture    = ap_photo_metadata_get(meta, AP_META_APERTURE);
    const char *shutter_str = ap_photo_metadata_get(meta, AP_META_SHUTTER);
    const char *iso_str     = ap_photo_metadata_get(meta, AP_META_ISO);
    const char *gps_lat_str = ap_photo_metadata_get(meta, AP_META_GPS_LAT);
    const char *gps_lon_str = ap_photo_metadata_get(meta, AP_META_GPS_LON);
    const char *gps_alt_str = ap_photo_metadata_get(meta, AP_META_GPS_ALT);
    const char *artist      = ap_photo_metadata_get(meta, AP_META_ARTIST);
    const char *desc        = ap_photo_metadata_get(meta, AP_META_DESCRIPTION);

    // Determine what GPS data is usable.
    double lat_deg = 0.0, lon_deg = 0.0, alt_m = 0.0;
    int has_lat  = *gps_lat_str  && parse_gps_decimal(gps_lat_str,  &lat_deg);
    int has_lon  = *gps_lon_str  && parse_gps_decimal(gps_lon_str,  &lon_deg);
    int has_alt  = *gps_alt_str  && parse_gps_alt(gps_alt_str, &alt_m);
    int has_gps  = has_lat && has_lon;

    // Build Exif sub-IFD.
    ifd_builder exif_ifd;
    ifd_init(&exif_ifd);
    int rc = 0;

    if (*datetime) {
        rc |= ifd_add_ascii(&exif_ifd, TAG_DATETIME_ORIG, datetime);
    }
    {
        uint32_t numer, denom;
        if (*shutter_str && parse_rational(shutter_str, &numer, &denom)) {
            rc |= ifd_add_rational(&exif_ifd, TAG_EXPOSURE_TIME, numer, denom);
        }
    }
    {
        uint32_t numer, denom;
        if (*aperture && parse_fnumber(aperture, &numer, &denom)) {
            rc |= ifd_add_rational(&exif_ifd, TAG_FNUMBER, numer, denom);
        }
    }
    {
        uint16_t iso;
        if (*iso_str && parse_iso(iso_str, &iso)) {
            rc |= ifd_add_short(&exif_ifd, TAG_ISO, iso);
        }
    }
    {
        uint32_t numer, denom;
        if (*focal_str && parse_focal(focal_str, &numer, &denom)) {
            rc |= ifd_add_rational(&exif_ifd, TAG_FOCAL_LEN, numer, denom);
        }
    }
    if (*lens_make)  rc |= ifd_add_ascii(&exif_ifd, TAG_LENS_MAKE,  lens_make);
    if (*lens_model) rc |= ifd_add_ascii(&exif_ifd, TAG_LENS_MODEL, lens_model);

    // Build GPS sub-IFD (only when lat+lon are present).
    ifd_builder gps_ifd;
    ifd_init(&gps_ifd);

    if (has_gps) {
        double abs_lat = fabs(lat_deg);
        double abs_lon = fabs(lon_deg);
        uint32_t deg_n, deg_d, min_n, min_d, sec_n, sec_d;

        const char *lat_ref = (lat_deg >= 0.0) ? "N" : "S";
        const char *lon_ref = (lon_deg >= 0.0) ? "E" : "W";
        rc |= ifd_add_ascii(&gps_ifd, TAG_GPS_LAT_REF, lat_ref);

        deg_to_dms(abs_lat, &deg_n, &deg_d, &min_n, &min_d, &sec_n, &sec_d);
        rc |= ifd_add_gps_coord(&gps_ifd, TAG_GPS_LAT,
                                deg_n, deg_d, min_n, min_d, sec_n, sec_d);

        rc |= ifd_add_ascii(&gps_ifd, TAG_GPS_LON_REF, lon_ref);

        deg_to_dms(abs_lon, &deg_n, &deg_d, &min_n, &min_d, &sec_n, &sec_d);
        rc |= ifd_add_gps_coord(&gps_ifd, TAG_GPS_LON,
                                deg_n, deg_d, min_n, min_d, sec_n, sec_d);

        if (has_alt) {
            uint8_t alt_ref  = (alt_m >= 0.0) ? 0 : 1; // 0=above, 1=below sea
            double  abs_alt  = fabs(alt_m);
            uint32_t alt_n   = (uint32_t)llround(abs_alt * 100.0);
            rc |= ifd_add_byte(&gps_ifd, TAG_GPS_ALT_REF, alt_ref);
            rc |= ifd_add_rational(&gps_ifd, TAG_GPS_ALT, alt_n, 100);
        }
    }

    if (rc < 0) {
        ifd_free(&exif_ifd);
        ifd_free(&gps_ifd);
        AP_ERROR("ap_exif_build: IFD construction failed");
        return -1;
    }

    // Build IFD0.
    ifd_builder ifd0;
    ifd_init(&ifd0);

    if (*desc)   rc |= ifd_add_ascii(&ifd0, TAG_IMAGE_DESC, desc);
    if (*make)   rc |= ifd_add_ascii(&ifd0, TAG_MAKE,       make);
    if (*model)  rc |= ifd_add_ascii(&ifd0, TAG_MODEL,      model);
    if (*datetime) rc |= ifd_add_ascii(&ifd0, TAG_DATETIME, datetime);
    if (*artist) rc |= ifd_add_ascii(&ifd0, TAG_ARTIST,     artist);


    // Reserve entries for sub-IFD pointers (always add if the sub-IFD
    // has any entries, even if we patch the value below).
    if (exif_ifd.count > 0) rc |= ifd_add_long(&ifd0, TAG_EXIF_IFD, 0);
    if (has_gps && gps_ifd.count > 0)
                             rc |= ifd_add_long(&ifd0, TAG_GPS_IFD,  0);

    if (rc < 0) {
        ifd_free(&exif_ifd);
        ifd_free(&gps_ifd);
        ifd_free(&ifd0);
        AP_ERROR("ap_exif_build: IFD0 construction failed");
        return -1;
    }

    // Layout pass: compute byte offsets.
    // The TIFF blob layout is:
    //   [0]  TIFF header (8 bytes)
    //   [8]  IFD0 directory + IFD0 vdata
    //   ...  Exif sub-IFD directory + its vdata
    //   ...  GPS sub-IFD directory + its vdata (if any)

    // IFD directory size = 2 (count) + N*12 + 4 (next-IFD offset).
    size_t ifd0_dir_size  = 2 + (size_t)ifd0.count  * 12 + 4;
    size_t exif_dir_size  = 2 + (size_t)exif_ifd.count * 12 + 4;
    size_t gps_dir_size   = (has_gps && gps_ifd.count > 0)
                          ? (2 + (size_t)gps_ifd.count * 12 + 4)
                          : 0;

    size_t header_size    = 8;
    size_t ifd0_start     = header_size;
    size_t ifd0_vdata_off = ifd0_start  + ifd0_dir_size;
    size_t exif_ifd_start = ifd0_vdata_off + ifd0.vdata.len;
    size_t exif_vdata_off = exif_ifd_start + exif_dir_size;
    size_t gps_ifd_start  = exif_vdata_off + exif_ifd.vdata.len;
    size_t gps_vdata_off  = gps_ifd_start  + gps_dir_size;
    size_t total_size     = gps_vdata_off  +
                            ((has_gps && gps_ifd.count > 0) ? gps_ifd.vdata.len : 0);

    // Patch sub-IFD offset entries in IFD0.
    // Walk sorted IFD0 entries (matching ifd_emit sort order) to find them.
    // We know TAG_EXIF_IFD < TAG_GPS_IFD so sort is deterministic. The
    // inline_val fields in the ifd0.entries[] array hold the placeholder
    // values; update them before emitting.
    for (int i = 0; i < ifd0.count; i++) {
        if (ifd0.entries[i].tag == TAG_EXIF_IFD && exif_ifd.count > 0) {
            ifd0.entries[i].inline_val = (uint32_t)exif_ifd_start;
        }
        if (ifd0.entries[i].tag == TAG_GPS_IFD && has_gps && gps_ifd.count > 0) {
            ifd0.entries[i].inline_val = (uint32_t)gps_ifd_start;
        }
    }

    // Allocate output buffer and serialize.
    buf_t out_buf = {0};
    if (buf_grow(&out_buf, total_size + 64) < 0) {
        ifd_free(&exif_ifd);
        ifd_free(&gps_ifd);
        ifd_free(&ifd0);
        return -1;
    }

    // TIFF header: "II" (little-endian), magic 42, IFD0 offset = 8.
    buf_append_u8(&out_buf, 'I');
    buf_append_u8(&out_buf, 'I');
    buf_append_u16le(&out_buf, 42);
    buf_append_u32le(&out_buf, (uint32_t)ifd0_start);

    // IFD0: we need the vdata offsets relative to TIFF header start.
    // ifd_emit places vdata immediately after the directory in `out_buf`,
    // but the offsets in each entry must point to vdata_abs relative to
    // the TIFF header. Since ifd_emit computes vdata_abs as
    // `dir_end = out->len after dir`, and `out->len` == ifd0_start before
    // the call, the dir_end == ifd0_start + ifd0_dir_size == ifd0_vdata_off
    // which matches our layout. So ifd_emit's internal offset computation
    // is correct: it uses `out->len` (== ifd0_start) + dir_size as the
    // vdata base.
    if (ifd_emit(&ifd0, &out_buf, 0) < 0) goto fail;

    // Exif sub-IFD.
    if (exif_ifd.count > 0) {
        if (ifd_emit(&exif_ifd, &out_buf, 0) < 0) goto fail;
    }

    // GPS sub-IFD.
    if (has_gps && gps_ifd.count > 0) {
        if (ifd_emit(&gps_ifd, &out_buf, 0) < 0) goto fail;
    }

    ifd_free(&exif_ifd);
    ifd_free(&gps_ifd);
    ifd_free(&ifd0);

    *out      = out_buf.data;
    *out_size = out_buf.len;
    return 0;

fail:
    free(out_buf.data);
    ifd_free(&exif_ifd);
    ifd_free(&gps_ifd);
    ifd_free(&ifd0);
    AP_ERROR("ap_exif_build: serialization failed");
    return -1;
}

int ap_exif_build_app1(const ap_photo_metadata *meta,
                       uint8_t **out, size_t *out_size)
{
    uint8_t *exif = NULL;
    size_t   exif_len = 0;
    if (ap_exif_build(meta, &exif, &exif_len) < 0) return -1;

    // APP1 layout: 0xFF 0xE1 + 2-byte big-endian segment length
    // (length counts itself but not the 0xFF 0xE1 prefix) + "Exif\0\0" +
    // EXIF blob.
    size_t header_len   = 6; // "Exif\0\0"
    size_t segment_len  = 2 + header_len + exif_len; // includes the 2-byte length field
    size_t total        = 2 + segment_len;

    if (segment_len > 0xFFFF) {
        // JPEG APP1 segment length is a 16-bit field; this would overflow.
        AP_ERROR("ap_exif_build_app1: EXIF blob too large (%zu bytes)", exif_len);
        free(exif);
        return -1;
    }

    uint8_t *app1 = malloc(total);
    if (!app1) {
        free(exif);
        return -1;
    }

    size_t pos = 0;
    app1[pos++] = 0xFF;
    app1[pos++] = 0xE1;
    // Big-endian segment length.
    app1[pos++] = (uint8_t)(segment_len >> 8);
    app1[pos++] = (uint8_t)(segment_len);
    memcpy(app1 + pos, "Exif\0\0", 6); pos += 6;
    memcpy(app1 + pos, exif, exif_len); pos += exif_len;

    free(exif);

    *out      = app1;
    *out_size = pos;
    return 0;
}
