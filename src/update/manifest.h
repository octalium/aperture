#ifndef APERTURE_UPDATE_MANIFEST_H
#define APERTURE_UPDATE_MANIFEST_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Parsed release manifest. Mirrors the JSON schema hosted alongside
// each release at e.g.
//   https://github.com/octalium/aperture/releases/download/latest/releases.json
//
// Example JSON:
//   {
//     "latest":   "0.2.0",
//     "released": "2026-06-01T00:00:00Z",
//     "channels": {
//       "appimage": { "url": "...", "zsync": "...", "sha256": "..." },
//       "dmg":      { "url": "...", "sha256": "..." },
//       "msi":      { "url": "...", "sha256": "..." },
//       "flatpak":  { "ref": "io.github.octalium.aperture/x86_64/stable" }
//     },
//     "notes": "..."
//   }
//
// All channel records are optional; only the channel matching the
// running platform is consumed by the updater. String fields that are
// missing from the JSON come back as empty strings, not NULL — every
// member is a fixed buffer so the parser can fail closed without
// dynamic allocation.

#define AP_MANIFEST_VERSION_LEN  64
#define AP_MANIFEST_DATE_LEN     32
#define AP_MANIFEST_URL_LEN      1024
#define AP_MANIFEST_HASH_LEN     128
#define AP_MANIFEST_REF_LEN      256
#define AP_MANIFEST_NOTES_LEN    4096

typedef struct {
    char url[AP_MANIFEST_URL_LEN];
    char zsync[AP_MANIFEST_URL_LEN];
    char sha256[AP_MANIFEST_HASH_LEN];
} ap_manifest_appimage;

typedef struct {
    char url[AP_MANIFEST_URL_LEN];
    char sha256[AP_MANIFEST_HASH_LEN];
} ap_manifest_artifact;

typedef struct {
    char ref[AP_MANIFEST_REF_LEN];
} ap_manifest_flatpak;

typedef struct {
    char                 latest[AP_MANIFEST_VERSION_LEN];
    char                 released[AP_MANIFEST_DATE_LEN];
    ap_manifest_appimage appimage;
    ap_manifest_artifact dmg;
    ap_manifest_artifact msi;
    ap_manifest_flatpak  flatpak;
    char                 notes[AP_MANIFEST_NOTES_LEN];
} ap_manifest;

// Parse `json` (NUL-terminated, length `len`) into `out`. Zeros
// `out` first. Returns 0 on success, -1 on a parse error or if the
// required `latest` field is missing. Logs the failure on -1.
int ap_manifest_parse(const char *json, size_t len, ap_manifest *out);

#ifdef __cplusplus
}
#endif

#endif
