#ifndef APERTURE_OUTPUT_EXPORT_H
#define APERTURE_OUTPUT_EXPORT_H

#include "output/png.h"
#include "output/tiff.h"

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ap_library ap_library;

#define AP_EXPORT_DEST_LEN    4096
#define AP_EXPORT_PATTERN_LEN 256

// Output container format. The encoder for each lives in src/output/.
typedef enum {
    AP_EXPORT_FORMAT_JPEG = 0,
    AP_EXPORT_FORMAT_TIFF = 1,
    AP_EXPORT_FORMAT_PNG  = 2,
} ap_export_format;

// Where the output file lands.
//   BESIDE     - next to the source raw, in the same directory
//   SUBDIR     - <library_root>/<dest_subdir>/
//   CUSTOM     - the absolute directory in `dest_dir`
typedef enum {
    AP_EXPORT_DEST_BESIDE = 0,
    AP_EXPORT_DEST_SUBDIR = 1,
    AP_EXPORT_DEST_CUSTOM = 2,
} ap_export_destination;

// What to do when the output filename already exists.
typedef enum {
    AP_EXPORT_COLLIDE_OVERWRITE = 0,  // replace the existing file
    AP_EXPORT_COLLIDE_SUFFIX    = 1,  // append _1, _2, ... to the stem
    AP_EXPORT_COLLIDE_SKIP      = 2,  // leave the existing file, skip
} ap_export_collision;

// Full export configuration. Persisted per-library; the Export-mode
// panels read and write this struct in place.
//
// `pattern` expands these tokens, with the format's extension appended
// afterwards:
//   {ORIG} {YYYY} {MM} {DD} {HH} {MIN} {SEC} {SEQ}
// Date / time come from the photo's EXIF capture time, falling back to
// the source file's modification time. {SEQ} is a 4-digit counter.
typedef struct {
    int  format;                          // ap_export_format

    int  jpeg_quality;                    // 1..100, JPEG only
    int  png_depth;                       // ap_png_depth, PNG only
    int  tiff_depth;                      // ap_tiff_depth, TIFF only
    int  tiff_compress;                   // ap_tiff_compress, TIFF only

    int  naming;                          // 0 keep stem, 1 rename by pattern
    char pattern[AP_EXPORT_PATTERN_LEN];  // rename pattern

    int  destination;                     // ap_export_destination
    char dest_subdir[AP_EXPORT_DEST_LEN]; // SUBDIR: relative to library root
    char dest_dir[AP_EXPORT_DEST_LEN];    // CUSTOM: absolute directory

    int  collision;                       // ap_export_collision
} ap_export_settings;

// Filename mode for `naming`.
typedef enum {
    AP_EXPORT_NAME_KEEP    = 0,  // keep the source stem
    AP_EXPORT_NAME_PATTERN = 1,  // rename via `pattern`
} ap_export_naming;

// Fill `out` with defaults, then overlay any values persisted on the
// library. Defaults: JPEG quality 90, keep original stem, export to
// <root>/export/, auto-suffix on collision.
void ap_export_settings_load(const ap_library *lib, ap_export_settings *out);

// Persist the export settings on the library. No-op when `lib` is NULL.
void ap_export_settings_save(ap_library *lib, const ap_export_settings *s);

// Lowercase file extension (no dot) for the configured format:
// "jpg", "tiff", or "png".
const char *ap_export_format_extension(int format);

// Build the output filename stem for one photo. `src_stem` is the
// source basename without its extension. With AP_EXPORT_NAME_KEEP the
// stem is copied verbatim; with AP_EXPORT_NAME_PATTERN the pattern is
// expanded against `when` (capture / mtime) and `seq`. The extension
// is not included — the caller appends it.
void ap_export_format_stem(const ap_export_settings *s,
                           const char *src_stem, time_t when, int seq,
                           char *out, size_t out_len);

// Resolve the absolute output directory for one photo.
//   BESIDE - the directory component of `src_path`
//   SUBDIR - <library_root>/<dest_subdir>
//   CUSTOM - `dest_dir`
// `library_root` may be NULL (BESIDE still works; SUBDIR then fails).
// Returns 0 on success, -1 when the directory cannot be resolved.
int ap_export_resolve_dir(const ap_export_settings *s,
                          const char *src_path, const char *library_root,
                          char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif
