#define _GNU_SOURCE

#include "io/exif.h"

#include "core/compat.h"
#include "core/log.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Read enough of the source to cover TIFF IFD0 + the ExifIFD for every
// TIFF-based RAW we touch. 256 KB also covers the CR3 'moov' box's
// CMT1 child near file start for the bodies we have shipped tests for.
#define EXIF_READ_BYTES (256u * 1024u)

static uint16_t rd_u16(const uint8_t *p, bool le)
{
    return le ? (uint16_t)(p[0] | (p[1] << 8))
              : (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t rd_u32(const uint8_t *p, bool le)
{
    return le ? (uint32_t)(p[0] |
                           ((uint32_t)p[1] <<  8) |
                           ((uint32_t)p[2] << 16) |
                           ((uint32_t)p[3] << 24))
              : (uint32_t)(((uint32_t)p[0] << 24) |
                           ((uint32_t)p[1] << 16) |
                           ((uint32_t)p[2] <<  8) |
                            (uint32_t)p[3]);
}

// EXIF tags of interest.
#define TAG_MAKE               0x010F
#define TAG_MODEL              0x0110
#define TAG_DATETIME_ORIGINAL  0x9003
#define TAG_DATETIME           0x0132
#define TAG_SUBSEC_ORIGINAL    0x9291
#define TAG_BODY_SERIAL        0xA431
#define TAG_IMAGE_UNIQUE_ID    0xA420
#define TAG_EXIF_IFD_POINTER   0x8769

// "YYYY:MM:DD HH:MM:SS" → time_t, interpreting the wall-clock fields
// as UTC. EXIF DateTimeOriginal carries no timezone, so for identity
// purposes we pick a fixed reference (UTC) to keep the resulting
// timestamp independent of the host's TZ. Returns 0 on parse failure;
// cameras never emit a 1970-01-01 capture so 0 is a safe "absent"
// sentinel.
static time_t parse_exif_datetime(const char *s, size_t len)
{
    if (!s || len < 19) return 0;
    int y, mo, d, h, mi, se;
    char buf[20];
    size_t n = len < 19 ? len : 19;
    memcpy(buf, s, n);
    buf[n] = '\0';
    if (sscanf(buf, "%4d:%2d:%2d %2d:%2d:%2d",
               &y, &mo, &d, &h, &mi, &se) != 6) {
        return 0;
    }
    struct tm tm = {0};
    tm.tm_year  = y - 1900;
    tm.tm_mon   = mo - 1;
    tm.tm_mday  = d;
    tm.tm_hour  = h;
    tm.tm_min   = mi;
    tm.tm_sec   = se;
    return timegm(&tm);
}

// Copy an ASCII EXIF value (NUL-terminated in the file) into `out`,
// stripping trailing whitespace and the terminator. `src_len` is the
// declared count of bytes in the IFD entry.
static void copy_ascii(const uint8_t *src, size_t src_len,
                       char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!src || src_len == 0) return;
    size_t n = src_len < out_len - 1 ? src_len : out_len - 1;
    memcpy(out, src, n);
    out[n] = '\0';
    // Drop the trailing NUL counted in `src_len` and any padding.
    size_t real = 0;
    while (real < n && out[real] != '\0') real++;
    out[real] = '\0';
    while (real > 0 && (out[real - 1] == ' ' || out[real - 1] == '\t')) {
        out[--real] = '\0';
    }
}

// Format counts (bytes per IFD value type). 0 marks types we ignore.
static const uint8_t TYPE_SIZE[13] = {
    0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8,
};

typedef struct {
    const uint8_t *buf;
    size_t         len;
    bool           le;
    ap_exif_fields fields;
    int            populated;
} exif_ctx;

static void apply_tag(exif_ctx *ctx, uint16_t tag, uint16_t type,
                      uint32_t count, const uint8_t *value_ptr,
                      uint32_t value_offset)
{
    // Locate the value bytes. The 4 inline bytes live in `value_ptr`;
    // anything larger lives at `value_offset` from the TIFF header.
    uint32_t type_size = (type < sizeof(TYPE_SIZE)) ? TYPE_SIZE[type] : 0;
    if (type_size == 0) return;
    uint64_t total = (uint64_t)type_size * count;
    const uint8_t *vp;
    if (total <= 4) {
        vp = value_ptr;
    } else {
        if ((uint64_t)value_offset + total > ctx->len) return;
        vp = ctx->buf + value_offset;
    }

    switch (tag) {
    case TAG_MAKE: {
        if (type != 2) return;
        if (ctx->fields.make[0]) return;
        copy_ascii(vp, count, ctx->fields.make,
                   sizeof(ctx->fields.make));
        if (ctx->fields.make[0]) ctx->populated++;
        break;
    }
    case TAG_MODEL: {
        if (type != 2) return;
        if (ctx->fields.model[0]) return;
        copy_ascii(vp, count, ctx->fields.model,
                   sizeof(ctx->fields.model));
        if (ctx->fields.model[0]) ctx->populated++;
        break;
    }
    case TAG_DATETIME_ORIGINAL:
    case TAG_DATETIME: {
        if (type != 2 /* ASCII */) return;
        time_t t = parse_exif_datetime((const char *)vp, count);
        if (t != 0 && ctx->fields.capture_time == 0) {
            ctx->fields.capture_time = t;
            ctx->populated++;
        }
        break;
    }
    case TAG_SUBSEC_ORIGINAL: {
        if (type != 2) return;
        if (ctx->fields.subsec_original[0]) return;
        copy_ascii(vp, count, ctx->fields.subsec_original,
                   sizeof(ctx->fields.subsec_original));
        if (ctx->fields.subsec_original[0]) ctx->populated++;
        break;
    }
    case TAG_BODY_SERIAL: {
        if (type != 2) return;
        if (ctx->fields.body_serial[0]) return;
        copy_ascii(vp, count, ctx->fields.body_serial,
                   sizeof(ctx->fields.body_serial));
        if (ctx->fields.body_serial[0]) ctx->populated++;
        break;
    }
    case TAG_IMAGE_UNIQUE_ID: {
        if (type != 2) return;
        if (ctx->fields.image_unique_id[0]) return;
        copy_ascii(vp, count, ctx->fields.image_unique_id,
                   sizeof(ctx->fields.image_unique_id));
        if (ctx->fields.image_unique_id[0]) ctx->populated++;
        break;
    }
    default:
        break;
    }
}

// Walk one IFD starting at `ifd_off` (offset from TIFF header). For
// each entry, applies tags of interest and recurses into the Exif
// SubIFD when found. Returns the file offset of the next IFD in the
// chain (0 when none).
static uint32_t walk_ifd(exif_ctx *ctx, uint32_t ifd_off, int depth)
{
    if (depth > 4) return 0;
    if (ifd_off + 2 > ctx->len) return 0;
    uint16_t count = rd_u16(ctx->buf + ifd_off, ctx->le);
    uint32_t entries_off = ifd_off + 2;
    if ((uint64_t)entries_off + (uint64_t)count * 12 + 4 > ctx->len) return 0;

    for (uint16_t i = 0; i < count; i++) {
        const uint8_t *e = ctx->buf + entries_off + i * 12;
        uint16_t tag   = rd_u16(e + 0, ctx->le);
        uint16_t type  = rd_u16(e + 2, ctx->le);
        uint32_t cnt   = rd_u32(e + 4, ctx->le);
        uint32_t vofs  = rd_u32(e + 8, ctx->le);

        if (tag == TAG_EXIF_IFD_POINTER && type == 4 /* LONG */ && cnt == 1) {
            walk_ifd(ctx, vofs, depth + 1);
            continue;
        }
        apply_tag(ctx, tag, type, cnt, e + 8, vofs);
    }
    return rd_u32(ctx->buf + entries_off + count * 12, ctx->le);
}

// Parse a TIFF header in `buf` (length `len`) and walk every IFD in
// IFD0's chain. Caller has already verified that `buf` looks like a
// TIFF block.
static void parse_tiff(exif_ctx *ctx)
{
    if (ctx->len < 8) return;
    uint8_t b0 = ctx->buf[0], b1 = ctx->buf[1];
    if (b0 == 'I' && b1 == 'I')      ctx->le = true;
    else if (b0 == 'M' && b1 == 'M') ctx->le = false;
    else return;

    uint16_t magic = rd_u16(ctx->buf + 2, ctx->le);
    if (magic != 42) return;
    uint32_t ifd0 = rd_u32(ctx->buf + 4, ctx->le);
    int chain = 0;
    while (ifd0 != 0 && chain < 8) {
        ifd0 = walk_ifd(ctx, ifd0, 0);
        chain++;
    }
}

// CR3 wraps EXIF inside a `CMT1` box that contains a TIFF blob. The
// path is moov.uuid(85c0b687...).CMT1. We scan the file head for the
// `CMT1` four-byte marker — the box header (size + name) sits right
// before, and the TIFF block starts immediately after. Robust enough
// for the small set of CR3 bodies we test with; if a CR3 ever lacks a
// recoverable CMT1 in the read window, ap_exif_read returns -1 and
// the caller falls back to mtime.
static bool find_cr3_exif(const uint8_t *buf, size_t len,
                          const uint8_t **tiff_out, size_t *tiff_len_out)
{
    if (len < 16) return false;
    for (size_t i = 0; i + 8 <= len; i++) {
        if (buf[i] == 'C' && buf[i + 1] == 'M' &&
            buf[i + 2] == 'T' && buf[i + 3] == '1') {
            // box header is 8 bytes (size + name) immediately before
            // the `CMT1` name. The TIFF blob begins right after.
            if (i < 4) continue;
            uint32_t box_size = ((uint32_t)buf[i - 4] << 24) |
                                ((uint32_t)buf[i - 3] << 16) |
                                ((uint32_t)buf[i - 2] <<  8) |
                                 (uint32_t)buf[i - 1];
            size_t   payload_off = i + 4;
            if (payload_off >= len) return false;
            size_t   payload_max = len - payload_off;
            size_t   payload_len = box_size > 8 ? box_size - 8 : payload_max;
            if (payload_len > payload_max) payload_len = payload_max;
            *tiff_out     = buf + payload_off;
            *tiff_len_out = payload_len;
            return true;
        }
    }
    return false;
}

// True when the first bytes look like an ISO BMFF file ("ftyp" at the
// canonical offset 4..7).
static bool looks_like_bmff(const uint8_t *buf, size_t len)
{
    if (len < 12) return false;
    return buf[4] == 'f' && buf[5] == 't' && buf[6] == 'y' && buf[7] == 'p';
}

int ap_exif_read_buf(const unsigned char *buf, size_t len,
                     ap_exif_fields *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!buf || len < 8) return -1;

    exif_ctx ctx = { .buf = buf, .len = len };
    if (looks_like_bmff(buf, len)) {
        const uint8_t *tiff = NULL;
        size_t         tlen = 0;
        if (find_cr3_exif(buf, len, &tiff, &tlen)) {
            ctx.buf = tiff;
            ctx.len = tlen;
            parse_tiff(&ctx);
        }
    } else {
        parse_tiff(&ctx);
    }

    *out = ctx.fields;
    return ctx.populated > 0 ? 0 : -1;
}

int ap_exif_read(const char *path, ap_exif_fields *out)
{
    if (!path || !out) return -1;
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t *buf = malloc(EXIF_READ_BYTES);
    if (!buf) { fclose(f); return -1; }
    size_t n = fread(buf, 1, EXIF_READ_BYTES, f);
    fclose(f);
    if (n < 8) { free(buf); return -1; }

    int rc = ap_exif_read_buf(buf, n, out);
    free(buf);
    return rc;
}

int ap_exif_identity(const ap_exif_fields *f, char *out, size_t out_len)
{
    if (!out || out_len == 0) return 0;
    out[0] = '\0';
    if (!f) return 0;

    int populated = 0;
    if (f->make[0])            populated++;
    if (f->model[0])           populated++;
    if (f->capture_time != 0)  populated++;
    if (f->subsec_original[0]) populated++;
    if (populated == 0) return 0;

    char ts[32];
    ts[0] = '\0';
    if (f->capture_time != 0) {
        snprintf(ts, sizeof(ts), "%lld", (long long)f->capture_time);
    }
    snprintf(out, out_len, "%s|%s|%s|%s",
             f->make,
             f->model,
             ts,
             f->subsec_original);
    return populated;
}

bool ap_exif_identity_is_unique(const ap_exif_fields *f)
{
    if (!f) return false;
    return f->make[0]
        && f->model[0]
        && f->capture_time != 0
        && f->subsec_original[0];
}
