#define _GNU_SOURCE

#include "output/export.h"

#include "core/log.h"
#include "library/library.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KEY_FORMAT      "export.format"
#define KEY_JPEG_Q      "export.jpeg_quality"
#define KEY_PNG_DEPTH   "export.png_depth"
#define KEY_TIFF_DEPTH  "export.tiff_depth"
#define KEY_TIFF_COMP   "export.tiff_compress"
#define KEY_NAMING      "export.naming"
#define KEY_PATTERN     "export.pattern"
#define KEY_DEST        "export.destination"
#define KEY_DEST_SUBDIR "export.dest_subdir"
#define KEY_DEST_DIR    "export.dest_dir"
#define KEY_COLLISION   "export.collision"

#define KEY_QE_FORMAT   "quick_export.format"
#define KEY_QE_JPEG_Q   "quick_export.jpeg_quality"
#define KEY_QE_DEST     "quick_export.destination"

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void ap_export_settings_load(const ap_library *lib, ap_export_settings *out)
{
    if (!out) return;

    out->format        = AP_EXPORT_FORMAT_JPEG;
    out->jpeg_quality  = 90;
    out->png_depth     = AP_PNG_UINT8;
    out->tiff_depth    = AP_TIFF_UINT8;
    out->tiff_compress = AP_TIFF_COMPRESS_LZW;
    out->naming        = AP_EXPORT_NAME_KEEP;
    snprintf(out->pattern, sizeof(out->pattern),
             "{ORIG}_{YYYY}{MM}{DD}");
    out->destination   = AP_EXPORT_DEST_SUBDIR;
    snprintf(out->dest_subdir, sizeof(out->dest_subdir), "export");
    out->dest_dir[0]   = '\0';
    out->collision     = AP_EXPORT_COLLIDE_SUFFIX;
    if (!lib) return;

    char buf[AP_EXPORT_DEST_LEN];
    if (ap_library_setting_get(lib, KEY_FORMAT, buf, sizeof(buf)) == 0) {
        out->format = clamp_int(atoi(buf), AP_EXPORT_FORMAT_JPEG,
                                AP_EXPORT_FORMAT_PNG);
    }
    if (ap_library_setting_get(lib, KEY_JPEG_Q, buf, sizeof(buf)) == 0) {
        out->jpeg_quality = clamp_int(atoi(buf), 1, 100);
    }
    if (ap_library_setting_get(lib, KEY_PNG_DEPTH, buf, sizeof(buf)) == 0) {
        out->png_depth = clamp_int(atoi(buf), AP_PNG_UINT8, AP_PNG_UINT16);
    }
    if (ap_library_setting_get(lib, KEY_TIFF_DEPTH, buf, sizeof(buf)) == 0) {
        out->tiff_depth = clamp_int(atoi(buf), AP_TIFF_UINT8, AP_TIFF_UINT16);
    }
    if (ap_library_setting_get(lib, KEY_TIFF_COMP, buf, sizeof(buf)) == 0) {
        out->tiff_compress = clamp_int(atoi(buf), AP_TIFF_COMPRESS_NONE,
                                       AP_TIFF_COMPRESS_DEFLATE);
    }
    if (ap_library_setting_get(lib, KEY_NAMING, buf, sizeof(buf)) == 0) {
        out->naming = (atoi(buf) == AP_EXPORT_NAME_PATTERN)
                          ? AP_EXPORT_NAME_PATTERN : AP_EXPORT_NAME_KEEP;
    }
    char pat[AP_EXPORT_PATTERN_LEN];
    if (ap_library_setting_get(lib, KEY_PATTERN, pat, sizeof(pat)) == 0
        && pat[0]) {
        snprintf(out->pattern, sizeof(out->pattern), "%s", pat);
    }
    if (ap_library_setting_get(lib, KEY_DEST, buf, sizeof(buf)) == 0) {
        out->destination = clamp_int(atoi(buf), AP_EXPORT_DEST_BESIDE,
                                     AP_EXPORT_DEST_CUSTOM);
    }
    if (ap_library_setting_get(lib, KEY_DEST_SUBDIR, buf, sizeof(buf)) == 0
        && buf[0]) {
        snprintf(out->dest_subdir, sizeof(out->dest_subdir), "%s", buf);
    }
    if (ap_library_setting_get(lib, KEY_DEST_DIR, buf, sizeof(buf)) == 0) {
        snprintf(out->dest_dir, sizeof(out->dest_dir), "%s", buf);
    }
    if (ap_library_setting_get(lib, KEY_COLLISION, buf, sizeof(buf)) == 0) {
        out->collision = clamp_int(atoi(buf), AP_EXPORT_COLLIDE_OVERWRITE,
                                   AP_EXPORT_COLLIDE_SKIP);
    }
}

void ap_export_settings_save(ap_library *lib, const ap_export_settings *s)
{
    if (!lib || !s) return;
    char num[16];
    snprintf(num, sizeof(num), "%d", s->format);
    ap_library_setting_set(lib, KEY_FORMAT, num);
    snprintf(num, sizeof(num), "%d", s->jpeg_quality);
    ap_library_setting_set(lib, KEY_JPEG_Q, num);
    snprintf(num, sizeof(num), "%d", s->png_depth);
    ap_library_setting_set(lib, KEY_PNG_DEPTH, num);
    snprintf(num, sizeof(num), "%d", s->tiff_depth);
    ap_library_setting_set(lib, KEY_TIFF_DEPTH, num);
    snprintf(num, sizeof(num), "%d", s->tiff_compress);
    ap_library_setting_set(lib, KEY_TIFF_COMP, num);
    snprintf(num, sizeof(num), "%d", s->naming);
    ap_library_setting_set(lib, KEY_NAMING, num);
    ap_library_setting_set(lib, KEY_PATTERN, s->pattern);
    snprintf(num, sizeof(num), "%d", s->destination);
    ap_library_setting_set(lib, KEY_DEST, num);
    ap_library_setting_set(lib, KEY_DEST_SUBDIR, s->dest_subdir);
    ap_library_setting_set(lib, KEY_DEST_DIR, s->dest_dir);
    snprintf(num, sizeof(num), "%d", s->collision);
    ap_library_setting_set(lib, KEY_COLLISION, num);
}

const char *ap_export_format_extension(int format)
{
    switch (format) {
    case AP_EXPORT_FORMAT_TIFF: return "tiff";
    case AP_EXPORT_FORMAT_PNG:  return "png";
    case AP_EXPORT_FORMAT_JPEG:
    default:                    return "jpg";
    }
}

void ap_export_format_stem(const ap_export_settings *s,
                           const char *src_stem, time_t when, int seq,
                           char *out, size_t out_len)
{
    if (!s || !out || out_len == 0) return;
    if (!src_stem) src_stem = "";

    if (s->naming != AP_EXPORT_NAME_PATTERN) {
        snprintf(out, out_len, "%s", src_stem);
        return;
    }

    struct tm tm;
    localtime_r(&when, &tm);

    size_t o = 0;
    for (const char *p = s->pattern; *p && o + 1 < out_len; ) {
        if (*p != '{') {
            out[o++] = *p++;
            continue;
        }
        const char *end = strchr(p, '}');
        if (!end) {
            out[o++] = *p++;
            continue;
        }
        char tok[16];
        size_t tlen = (size_t)(end - p - 1);
        char rep[64];
        rep[0] = '\0';
        if (tlen < sizeof(tok)) {
            memcpy(tok, p + 1, tlen);
            tok[tlen] = '\0';
            if      (strcmp(tok, "ORIG") == 0) snprintf(rep, sizeof(rep), "%s", src_stem);
            else if (strcmp(tok, "YYYY") == 0) snprintf(rep, sizeof(rep), "%04d", tm.tm_year + 1900);
            else if (strcmp(tok, "MM")   == 0) snprintf(rep, sizeof(rep), "%02d", tm.tm_mon + 1);
            else if (strcmp(tok, "DD")   == 0) snprintf(rep, sizeof(rep), "%02d", tm.tm_mday);
            else if (strcmp(tok, "HH")   == 0) snprintf(rep, sizeof(rep), "%02d", tm.tm_hour);
            else if (strcmp(tok, "MIN")  == 0) snprintf(rep, sizeof(rep), "%02d", tm.tm_min);
            else if (strcmp(tok, "SEC")  == 0) snprintf(rep, sizeof(rep), "%02d", tm.tm_sec);
            else if (strcmp(tok, "SEQ")  == 0) snprintf(rep, sizeof(rep), "%04d", seq);
            // An unrecognised token expands to nothing.
        }
        for (const char *r = rep; *r && o + 1 < out_len; r++) {
            out[o++] = *r;
        }
        p = end + 1;
    }
    out[o] = '\0';

    // An empty pattern (or one of only unknown tokens) would yield a
    // bare extension; fall back to the source stem so the file is
    // always identifiable.
    if (out[0] == '\0') {
        snprintf(out, out_len, "%s", src_stem);
    }
}

int ap_export_resolve_dir(const ap_export_settings *s,
                          const char *src_path, const char *library_root,
                          char *out, size_t out_len)
{
    if (!s || !out || out_len == 0) return -1;

    int n = -1;
    switch (s->destination) {
    case AP_EXPORT_DEST_BESIDE: {
        if (!src_path) return -1;
        const char *slash = strrchr(src_path, '/');
        if (!slash) {
            n = snprintf(out, out_len, ".");
        } else {
            size_t dir_len = (size_t)(slash - src_path);
            if (dir_len == 0) dir_len = 1;  // root "/"
            if (dir_len >= out_len) return -1;
            memcpy(out, src_path, dir_len);
            out[dir_len] = '\0';
            n = (int)dir_len;
        }
        break;
    }
    case AP_EXPORT_DEST_SUBDIR:
        if (!library_root) {
            AP_ERROR("export: subdir destination needs an open library");
            return -1;
        }
        n = snprintf(out, out_len, "%s/%s", library_root, s->dest_subdir);
        break;
    case AP_EXPORT_DEST_CUSTOM:
        if (!s->dest_dir[0]) {
            AP_ERROR("export: no custom destination directory chosen");
            return -1;
        }
        n = snprintf(out, out_len, "%s", s->dest_dir);
        break;
    default:
        return -1;
    }

    if (n <= 0 || (size_t)n >= out_len) {
        AP_ERROR("export: destination path too long");
        return -1;
    }
    return 0;
}

void ap_quick_export_load(ap_quick_export_settings *out)
{
    if (!out) return;

    out->format         = AP_EXPORT_FORMAT_JPEG;
    out->jpeg_quality   = 90;
    out->destination[0] = '\0';

    char buf[AP_EXPORT_DEST_LEN];
    if (ap_settings_get(KEY_QE_FORMAT, buf, sizeof(buf)) == 0) {
        out->format = clamp_int(atoi(buf), AP_EXPORT_FORMAT_JPEG,
                                AP_EXPORT_FORMAT_PNG);
    }
    if (ap_settings_get(KEY_QE_JPEG_Q, buf, sizeof(buf)) == 0) {
        out->jpeg_quality = clamp_int(atoi(buf), 1, 100);
    }
    if (ap_settings_get(KEY_QE_DEST, buf, sizeof(buf)) == 0) {
        snprintf(out->destination, sizeof(out->destination), "%s", buf);
    }
}

void ap_quick_export_save(const ap_quick_export_settings *s)
{
    if (!s) return;
    char num[16];
    snprintf(num, sizeof(num), "%d", s->format);
    ap_settings_set(KEY_QE_FORMAT, num);
    snprintf(num, sizeof(num), "%d", s->jpeg_quality);
    ap_settings_set(KEY_QE_JPEG_Q, num);
    ap_settings_set(KEY_QE_DEST, s->destination);
}

int ap_quick_export_resolve_dir(const ap_quick_export_settings *s,
                                const char *library_root,
                                char *out, size_t out_len)
{
    if (!s || !out || out_len == 0) return -1;
    if (s->destination[0]) {
        int n = snprintf(out, out_len, "%s", s->destination);
        if (n <= 0 || (size_t)n >= out_len) {
            AP_ERROR("quick export: destination path too long");
            return -1;
        }
        return 0;
    }
    if (!library_root || !library_root[0]) return -1;
    int n = snprintf(out, out_len, "%s/export", library_root);
    if (n <= 0 || (size_t)n >= out_len) {
        AP_ERROR("quick export: default path too long");
        return -1;
    }
    return 0;
}
