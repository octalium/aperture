#ifndef APERTURE_COLOR_ICC_H
#define APERTURE_COLOR_ICC_H

#ifdef __cplusplus
extern "C" {
#endif

// Derive the camera-RGB -> linear-sRGB 3x3 colour matrix from a
// matrix/shaper ICC profile.
//
// `path` is a filesystem path to a `.icc` profile. `out` receives the
// 3x3 matrix as 9 floats, row-major (out[row*3 + col]); it maps a
// linear camera-RGB triple to linear sRGB, the same convention the
// camera-native matrix in ap_raw_metadata uses, so the result is a
// drop-in replacement for the Color Profile push constant.
//
// Returns 0 on success. Returns non-zero — and leaves `out`
// untouched — when the file is missing or unreadable, or the profile
// is not an RGB matrix/shaper profile (cLUT profiles carry a colour
// lookup table that a 3x3 cannot represent; those are handled by the
// 3D-LUT path, not here). Callers should fall back to the camera-
// native matrix on a non-zero return.
//
// Results are cached by path, so calling this every frame from a
// module's pack_push is cheap after the first lookup.
int ap_icc_camera_matrix(const char *path, float out[9]);

#ifdef __cplusplus
}
#endif

#endif
