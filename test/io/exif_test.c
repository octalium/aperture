// EXIF parser test suite. Builds synthesized TIFF and CR3 byte blobs
// in memory and feeds them through ap_exif_read_buf. No disk I/O.

#define _GNU_SOURCE

#include "aptest.h"
#include "io/exif.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <unistd.h>
#endif

// Small append-only buffer helper. Tests assemble TIFF and BMFF blobs
// here and feed the result to the parser.

typedef struct {
    unsigned char data[8192];
    size_t        len;
} buf_t;

static void buf_reset(buf_t *b) { b->len = 0; memset(b->data, 0, sizeof(b->data)); }

static void buf_u8(buf_t *b, uint8_t v)
{
    AP_TEST_ASSERT(b->len + 1 <= sizeof(b->data), "buf overflow");
    b->data[b->len++] = v;
}

static void buf_u16le(buf_t *b, uint16_t v)
{
    buf_u8(b, (uint8_t)(v & 0xff));
    buf_u8(b, (uint8_t)((v >> 8) & 0xff));
}

static void buf_u16be(buf_t *b, uint16_t v)
{
    buf_u8(b, (uint8_t)((v >> 8) & 0xff));
    buf_u8(b, (uint8_t)(v & 0xff));
}

static void buf_u32le(buf_t *b, uint32_t v)
{
    buf_u8(b, (uint8_t)(v & 0xff));
    buf_u8(b, (uint8_t)((v >> 8) & 0xff));
    buf_u8(b, (uint8_t)((v >> 16) & 0xff));
    buf_u8(b, (uint8_t)((v >> 24) & 0xff));
}

static void buf_u32be(buf_t *b, uint32_t v)
{
    buf_u8(b, (uint8_t)((v >> 24) & 0xff));
    buf_u8(b, (uint8_t)((v >> 16) & 0xff));
    buf_u8(b, (uint8_t)((v >> 8) & 0xff));
    buf_u8(b, (uint8_t)(v & 0xff));
}

static void buf_bytes(buf_t *b, const void *src, size_t n)
{
    AP_TEST_ASSERT(b->len + n <= sizeof(b->data), "buf overflow");
    memcpy(b->data + b->len, src, n);
    b->len += n;
}

static void buf_pad_to(buf_t *b, size_t pos)
{
    AP_TEST_ASSERT(pos <= sizeof(b->data), "pad past buffer");
    if (pos > b->len) {
        memset(b->data + b->len, 0, pos - b->len);
        b->len = pos;
    }
}

// Write a single 12-byte IFD entry. For ASCII values whose total
// payload is <=4 bytes, the TIFF spec stores the bytes inline in the
// last 4 bytes of the entry — caller passes `inline_bytes` non-NULL
// in that case (`value_offset` is ignored). Otherwise the value
// lives at `value_offset` from the TIFF header start.

static void ifd_entry(buf_t *b, int le, uint16_t tag, uint16_t type,
                      uint32_t count, uint32_t value_offset,
                      const unsigned char *inline_bytes,
                      size_t inline_len)
{
    if (le) {
        buf_u16le(b, tag);
        buf_u16le(b, type);
        buf_u32le(b, count);
    } else {
        buf_u16be(b, tag);
        buf_u16be(b, type);
        buf_u32be(b, count);
    }
    if (inline_bytes) {
        unsigned char payload[4] = {0, 0, 0, 0};
        size_t n = inline_len < 4 ? inline_len : 4;
        memcpy(payload, inline_bytes, n);
        buf_bytes(b, payload, 4);
    } else if (le) {
        buf_u32le(b, value_offset);
    } else {
        buf_u32be(b, value_offset);
    }
}

// Pack an ASCII value either inline (<=4 bytes) or as an offset.
// `str` includes the trailing NUL.
static void ifd_ascii(buf_t *b, int le, uint16_t tag, const char *str,
                      size_t total_len, uint32_t offset_if_extern)
{
    if (total_len <= 4) {
        ifd_entry(b, le, tag, 2, (uint32_t)total_len, 0,
                  (const unsigned char *)str, total_len);
    } else {
        ifd_entry(b, le, tag, 2, (uint32_t)total_len, offset_if_extern,
                  NULL, 0);
    }
}

// Reserve `n` bytes of payload space when total > 4 (extern), or 0
// when it fits inline. Lets the layout pass be agnostic about the
// caller's chosen string lengths.
static size_t extern_size(size_t total_len)
{
    return total_len > 4 ? total_len : 0;
}

// Build a single-IFD0 TIFF with Make/Model + DateTime in IFD0 and
// DateTimeOriginal + SubSecTimeOriginal in an Exif SubIFD. Returns
// total byte length. Layout is computed from the supplied string
// lengths so the test can vary them freely (incl. <=4-byte strings
// that pack inline in the IFD entry).
static size_t build_tiff(buf_t *b, int le, const char *make,
                         const char *model, const char *dto,
                         const char *subsec)
{
    buf_reset(b);

    if (le) {
        buf_u8(b, 'I'); buf_u8(b, 'I');
        buf_u16le(b, 42);
        buf_u32le(b, 8);
    } else {
        buf_u8(b, 'M'); buf_u8(b, 'M');
        buf_u16be(b, 42);
        buf_u32be(b, 8);
    }

    size_t make_len   = strlen(make)   + 1;
    size_t model_len  = strlen(model)  + 1;
    size_t dto_len    = strlen(dto)    + 1;
    size_t subsec_len = strlen(subsec) + 1;

    size_t ifd0_end   = 8 + 2 + 4 * 12 + 4;
    size_t make_off   = ifd0_end;
    size_t model_off  = make_off  + extern_size(make_len);
    size_t subifd_off = model_off + extern_size(model_len);
    size_t subifd_end = subifd_off + 2 + 2 * 12 + 4;
    size_t dto_off    = subifd_end;
    size_t subsec_off = dto_off + extern_size(dto_len);

    if (le) buf_u16le(b, 4); else buf_u16be(b, 4);
    ifd_ascii(b, le, 0x010F, make,  make_len,  (uint32_t)make_off);
    ifd_ascii(b, le, 0x0110, model, model_len, (uint32_t)model_off);
    ifd_ascii(b, le, 0x0132, dto,   dto_len,   (uint32_t)dto_off);
    // 0x8769 ExifIFDPointer, LONG (always 4 bytes -> fits inline).
    ifd_entry(b, le, 0x8769, 4, 1, (uint32_t)subifd_off, NULL, 0);
    if (le) buf_u32le(b, 0); else buf_u32be(b, 0);

    buf_pad_to(b, make_off);
    if (extern_size(make_len))  buf_bytes(b, make,  make_len);
    buf_pad_to(b, model_off);
    if (extern_size(model_len)) buf_bytes(b, model, model_len);

    buf_pad_to(b, subifd_off);
    if (le) buf_u16le(b, 2); else buf_u16be(b, 2);
    ifd_ascii(b, le, 0x9003, dto,    dto_len,    (uint32_t)dto_off);
    ifd_ascii(b, le, 0x9291, subsec, subsec_len, (uint32_t)subsec_off);
    if (le) buf_u32le(b, 0); else buf_u32be(b, 0);

    buf_pad_to(b, dto_off);
    if (extern_size(dto_len))    buf_bytes(b, dto,    dto_len);
    buf_pad_to(b, subsec_off);
    if (extern_size(subsec_len)) buf_bytes(b, subsec, subsec_len);

    return b->len;
}

// Compute the UTC time_t for "2024:06:15 12:34:56" — pinned because
// the parser uses timegm, so the expected value is timezone-independent.
static time_t expected_dto_utc(void)
{
    struct tm tm = {0};
    tm.tm_year = 2024 - 1900;
    tm.tm_mon  = 6 - 1;
    tm.tm_mday = 15;
    tm.tm_hour = 12;
    tm.tm_min  = 34;
    tm.tm_sec  = 56;
#if defined(_WIN32)
    return _mkgmtime(&tm);
#else
    return timegm(&tm);
#endif
}

static void test_tiff_little_endian(void)
{
    buf_t b;
    size_t n = build_tiff(&b, 1, "Canon", "EOS R5",
                          "2024:06:15 12:34:56", "123");
    ap_exif_fields f;
    int rc = ap_exif_read_buf(b.data, n, &f);
    AP_TEST_ASSERT(rc == 0, "expected parse success on little-endian fixture");
    AP_TEST_ASSERT(strcmp(f.make,  "Canon")  == 0, "make=%s", f.make);
    AP_TEST_ASSERT(strcmp(f.model, "EOS R5") == 0, "model=%s", f.model);
    AP_TEST_ASSERT(strcmp(f.subsec_original, "123") == 0,
                   "subsec=%s", f.subsec_original);
    AP_TEST_ASSERT(f.capture_time == expected_dto_utc(),
                   "capture_time=%lld", (long long)f.capture_time);
    AP_TEST_ASSERT(ap_exif_identity_is_unique(&f),
                   "all four identity-tuple fields should be populated");

    char ident[128];
    int populated = ap_exif_identity(&f, ident, sizeof(ident));
    AP_TEST_ASSERT(populated == 4, "populated=%d", populated);
    AP_TEST_ASSERT(strstr(ident, "Canon|EOS R5|") != NULL,
                   "identity prefix wrong: %s", ident);
    AP_TEST_ASSERT(strstr(ident, "|123") != NULL,
                   "identity subsec missing: %s", ident);
}

static void test_tiff_big_endian(void)
{
    buf_t b;
    size_t n = build_tiff(&b, 0, "NIKON CORPORATION", "NIKON Z 9",
                          "2024:06:15 12:34:56", "47");
    ap_exif_fields f;
    int rc = ap_exif_read_buf(b.data, n, &f);
    AP_TEST_ASSERT(rc == 0, "expected parse success on big-endian fixture");
    AP_TEST_ASSERT(strcmp(f.make,  "NIKON CORPORATION") == 0,
                   "make=%s", f.make);
    AP_TEST_ASSERT(strcmp(f.model, "NIKON Z 9") == 0, "model=%s", f.model);
    AP_TEST_ASSERT(strcmp(f.subsec_original, "47") == 0,
                   "subsec=%s", f.subsec_original);
    AP_TEST_ASSERT(f.capture_time == expected_dto_utc(),
                   "capture_time=%lld", (long long)f.capture_time);
    AP_TEST_ASSERT(ap_exif_identity_is_unique(&f),
                   "all four identity-tuple fields should be populated");
}

// Wrap a TIFF block in the minimal BMFF envelope the parser scans for:
// an ftyp header followed by a CMT1 box whose payload is the TIFF blob.
static size_t build_cr3(buf_t *out, const unsigned char *tiff, size_t tiff_len)
{
    buf_reset(out);

    // ftyp box (24 bytes). Real CR3 uses 'crx ' / 'isom'; the parser
    // only checks for the 'ftyp' name at offset 4..7.
    buf_u32be(out, 24);
    buf_u8(out, 'f'); buf_u8(out, 't'); buf_u8(out, 'y'); buf_u8(out, 'p');
    buf_u8(out, 'c'); buf_u8(out, 'r'); buf_u8(out, 'x'); buf_u8(out, ' ');
    buf_u32be(out, 0);
    buf_u8(out, 'c'); buf_u8(out, 'r'); buf_u8(out, 'x'); buf_u8(out, ' ');
    buf_u8(out, 'i'); buf_u8(out, 's'); buf_u8(out, 'o'); buf_u8(out, 'm');

    // CMT1 box: 4 bytes size, 4 bytes name 'CMT1', then payload.
    uint32_t cmt1_size = (uint32_t)(8 + tiff_len);
    buf_u32be(out, cmt1_size);
    buf_u8(out, 'C'); buf_u8(out, 'M'); buf_u8(out, 'T'); buf_u8(out, '1');
    buf_bytes(out, tiff, tiff_len);

    return out->len;
}

static void test_cr3_bmff(void)
{
    buf_t tiff;
    size_t tlen = build_tiff(&tiff, 1, "Canon", "Canon EOS R5",
                             "2024:06:15 12:34:56", "987");
    buf_t cr3;
    size_t clen = build_cr3(&cr3, tiff.data, tlen);

    ap_exif_fields f;
    int rc = ap_exif_read_buf(cr3.data, clen, &f);
    AP_TEST_ASSERT(rc == 0, "expected CR3 parse success");
    AP_TEST_ASSERT(strcmp(f.make,  "Canon") == 0, "make=%s", f.make);
    AP_TEST_ASSERT(strcmp(f.model, "Canon EOS R5") == 0, "model=%s", f.model);
    AP_TEST_ASSERT(strcmp(f.subsec_original, "987") == 0,
                   "subsec=%s", f.subsec_original);
    AP_TEST_ASSERT(f.capture_time == expected_dto_utc(),
                   "capture_time=%lld", (long long)f.capture_time);
}

static void test_truncated(void)
{
    unsigned char tiny[8] = { 'I', 'I', 42, 0, 0, 0, 0, 0 };
    ap_exif_fields f;
    memset(&f, 0xaa, sizeof(f));
    int rc = ap_exif_read_buf(tiny, sizeof(tiny), &f);
    AP_TEST_ASSERT(rc == -1, "truncated fixture should fail");
    AP_TEST_ASSERT(f.make[0] == '\0', "out.make should be zeroed on failure");
    AP_TEST_ASSERT(f.capture_time == 0,
                   "out.capture_time should be zeroed on failure");

    int rc2 = ap_exif_read_buf(NULL, 0, &f);
    AP_TEST_ASSERT(rc2 == -1, "null buf should fail");

    unsigned char two_bytes[2] = { 'I', 'I' };
    int rc3 = ap_exif_read_buf(two_bytes, sizeof(two_bytes), &f);
    AP_TEST_ASSERT(rc3 == -1, "under-minimum buf should fail");
}

static void test_malformed_header(void)
{
    unsigned char bad_magic[16] = { 'I', 'I', 99, 0, 0, 0, 0, 0,
                                    0, 0, 0, 0, 0, 0, 0, 0 };
    ap_exif_fields f;
    int rc = ap_exif_read_buf(bad_magic, sizeof(bad_magic), &f);
    AP_TEST_ASSERT(rc == -1, "bad TIFF magic should fail");

    unsigned char bad_endian[16] = { 'Z', 'Z', 42, 0, 0, 0, 0, 0,
                                     0, 0, 0, 0, 0, 0, 0, 0 };
    rc = ap_exif_read_buf(bad_endian, sizeof(bad_endian), &f);
    AP_TEST_ASSERT(rc == -1, "bad byte-order marker should fail");
}

static void test_overflowing_ifd(void)
{
    // Valid TIFF header, IFD0 claims an absurd entry count whose
    // entries would walk off the end of the buffer.
    unsigned char buf[32];
    memset(buf, 0, sizeof(buf));
    buf[0] = 'I'; buf[1] = 'I';
    buf[2] = 42; buf[3] = 0;
    buf[4] = 8;  buf[5] = 0; buf[6] = 0; buf[7] = 0;
    // IFD0 at offset 8: 65535 entries claimed. 65535*12 dwarfs the
    // buffer; the parser must reject without crashing.
    buf[8] = 0xff; buf[9] = 0xff;

    ap_exif_fields f;
    int rc = ap_exif_read_buf(buf, sizeof(buf), &f);
    AP_TEST_ASSERT(rc == -1, "overflowing IFD should fail (got rc=%d)", rc);
}

static void test_input_immutability(void)
{
    buf_t b;
    size_t n = build_tiff(&b, 1, "Canon", "EOS R5",
                          "2024:06:15 12:34:56", "123");

    unsigned char copy[8192];
    memcpy(copy, b.data, n);

    ap_exif_fields f;
    int rc = ap_exif_read_buf(b.data, n, &f);
    AP_TEST_ASSERT(rc == 0, "parse should succeed");
    AP_TEST_ASSERT(memcmp(copy, b.data, n) == 0,
                   "parser must not mutate the input buffer");
}

int main(void)
{
    test_tiff_little_endian();
    test_tiff_big_endian();
    test_cr3_bmff();
    test_truncated();
    test_malformed_header();
    test_overflowing_ifd();
    test_input_immutability();
    printf("io/exif: OK\n");
    return 0;
}
