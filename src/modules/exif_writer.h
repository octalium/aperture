#ifndef APERTURE_MODULES_EXIF_WRITER_H
#define APERTURE_MODULES_EXIF_WRITER_H

#include "photo/metadata.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Build a raw EXIF blob from the supplied photo metadata. The blob is a
// TIFF-structured byte stream beginning with the TIFF header (byte-order
// mark, magic 42, IFD0 offset); it does NOT include the "Exif\0\0"
// prefix used in JPEG APP1 segments — callers prepend that themselves.
//
// On success, *out receives a malloc'd buffer of *out_size bytes. The
// caller owns the buffer and must free() it when done. Empty or missing
// metadata fields are silently skipped so a sparse ap_photo_metadata is
// always safe to pass.
//
// Returns 0 on success, -1 on allocation failure.
int ap_exif_build(const ap_photo_metadata *meta,
                  uint8_t **out, size_t *out_size);

// Convenience: build a complete JPEG APP1 segment (0xFF 0xE1 + 2-byte
// big-endian length + "Exif\0\0" + EXIF blob) suitable for inserting
// immediately after the SOI marker when constructing a JPEG file.
//
// On success, *out receives a malloc'd buffer of *out_size bytes.
// Returns 0 on success, -1 on failure.
int ap_exif_build_app1(const ap_photo_metadata *meta,
                       uint8_t **out, size_t *out_size);

#ifdef __cplusplus
}
#endif

#endif
