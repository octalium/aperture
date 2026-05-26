#ifndef APERTURE_CORE_VERSION_H
#define APERTURE_CORE_VERSION_H

#ifdef __cplusplus
extern "C" {
#endif

// Build-time aperture version, e.g. "0.0.1". Defined via meson from
// meson.project_version() (see -DAP_VERSION_STRING in the root
// meson.build). Falls back to "unknown" if a translation unit is
// somehow compiled without it.
#ifndef AP_VERSION_STRING
#define AP_VERSION_STRING "unknown"
#endif

// Parsed semantic version comparison. Returns negative when `a < b`,
// zero when equal, positive when `a > b`. Both inputs are dotted
// numeric strings (e.g. "0.2.0" vs "0.1.4"); any non-numeric suffix
// (e.g. "-rc1") is parsed as a separator and stops the comparison at
// the preceding component. NULL is treated as the empty string and
// compares less than any non-empty version.
int ap_version_compare(const char *a, const char *b);

#ifdef __cplusplus
}
#endif

#endif
