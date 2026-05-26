#ifndef APERTURE_IO_EXIF_H
#define APERTURE_IO_EXIF_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#define AP_EXIF_STR_LEN 64

// Small bundle of EXIF fields the importer needs: enough to compose a
// per-shot identity for dedupe and to honour pattern naming. All
// strings are NUL-terminated; an empty string means the file did not
// carry that tag (or the parser could not recover it).
typedef struct {
    // Make (IFD0 tag 0x010F). Empty when absent.
    char make[AP_EXIF_STR_LEN];
    // Model (IFD0 tag 0x0110). Empty when absent.
    char model[AP_EXIF_STR_LEN];
    // BodySerialNumber (Exif SubIFD tag 0xA431). Parsed for possible
    // diagnostic use; not part of the identity tuple because many
    // vendors only emit it inside MakerNote subdirs.
    char body_serial[AP_EXIF_STR_LEN];
    // ImageUniqueID (Exif SubIFD tag 0xA420). Same caveat as body_serial.
    char image_unique_id[AP_EXIF_STR_LEN];
    // SubSecTimeOriginal (Exif SubIFD tag 0x9291). Empty when absent.
    char subsec_original[AP_EXIF_STR_LEN];
    // DateTimeOriginal (Exif SubIFD tag 0x9003) as a time_t. The EXIF
    // wall-clock fields are interpreted as UTC (via timegm) so the
    // value is deterministic across host timezones — required for the
    // identity tuple to dedupe correctly across machines. Zero when
    // the tag was absent or unparseable.
    time_t capture_time;
} ap_exif_fields;

// Parse just the fields above from `path`. Reads only the first chunk
// of the file (TIFF IFD0 + ExifIFD for TIFF-based RAW; the CMT1 box
// for CR3). Returns 0 when at least one field was populated, -1 when
// the file could not be read or no recognisable EXIF was found. On a
// -1 return `out` is zeroed.
int ap_exif_read(const char *path, ap_exif_fields *out);

// Same as ap_exif_read but parses from an in-memory byte blob (the
// first 256 KB of a RAW is enough; see ap_exif_read). `buf` is read-
// only and never mutated. Returns 0 when at least one field was
// populated, -1 otherwise (and zeros `out`).
int ap_exif_read_buf(const unsigned char *buf, size_t len,
                     ap_exif_fields *out);

// Compose the importer's dedupe identity string from `f` into `out`
// (NUL-terminated, truncated to `out_len`). Format:
//   "<make>|<model>|<dto>|<subsec>"
// Returns the number of identity-tuple fields that were populated.
// Callers should only consult the resulting string when
// `ap_exif_identity_is_unique` returns true; the format is otherwise
// stable but not collision-free.
int  ap_exif_identity(const ap_exif_fields *f, char *out, size_t out_len);

// True when `f` carries all four identity-tuple fields:
//   make, model, capture_time, subsec_original
// Rationale: same-camera bursts disambiguate via SubSec; two same-
// model bodies firing in the same sub-second is vanishingly rare and
// the accepted residual collision. Older bodies without SubSec, or
// any source missing make/model, must not dedupe — they fall through
// to a copy (or get skipped, depending on the import's strict_identity
// setting).
bool ap_exif_identity_is_unique(const ap_exif_fields *f);

#endif
