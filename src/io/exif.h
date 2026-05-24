#ifndef APERTURE_IO_EXIF_H
#define APERTURE_IO_EXIF_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#define AP_EXIF_STR_LEN 64

// Small bundle of EXIF fields the importer needs: enough to build a
// physically-unique identity for a shot and to honour pattern naming.
// All strings are NUL-terminated; an empty string means the file did
// not carry that tag (or the parser could not recover it).
typedef struct {
    // BodySerialNumber (Exif tag 0xA431). Empty when absent.
    char body_serial[AP_EXIF_STR_LEN];
    // ImageUniqueID (Exif tag 0xA420). Empty when absent.
    char image_unique_id[AP_EXIF_STR_LEN];
    // SubSecTimeOriginal (Exif tag 0x9291). Empty when absent.
    char subsec_original[AP_EXIF_STR_LEN];
    // DateTimeOriginal (Exif tag 0x9003) as a local-time time_t. Zero
    // when the tag was absent or unparseable.
    time_t capture_time;
} ap_exif_fields;

// Parse just the fields above from `path`. Reads only the first chunk
// of the file (TIFF IFD0 + ExifIFD for TIFF-based RAW; the CMT1 box
// for CR3). Returns 0 when at least one field was populated, -1 when
// the file could not be read or no recognisable EXIF was found. On a
// -1 return `out` is zeroed.
int ap_exif_read(const char *path, ap_exif_fields *out);

// Compose the importer's dedupe identity string from `f` into `out`
// (NUL-terminated, truncated to `out_len`). Format:
//   "<serial>-<unix_ts>.<subsec>-<unique_id>"
// where any missing field is left empty (its slot collapses). Returns
// the number of populated fields. When no field is populated the
// identity is the empty string and the caller should not use it for
// dedupe.
int  ap_exif_identity(const ap_exif_fields *f, char *out, size_t out_len);

#endif
