#ifndef APERTURE_COLOR_ICC_H
#define APERTURE_COLOR_ICC_H

#ifdef __cplusplus
extern "C" {
#endif

// Edge length of the baked colour LUT. 33 is the de-facto standard
// grid size for camera / display profiles (Adobe uses it); trilinear
// interpolation of a 33^3 grid reproduces a matrix transform exactly
// and resolves a cLUT profile's curvature well.
#define AP_ICC_LUT_DIM 33

// Bake an ICC profile into a 3D colour LUT.
//
// `path` is a filesystem path to a `.icc` profile (matrix/shaper or
// cLUT — both are handled). The profile's camera-RGB -> linear-sRGB
// transform is sampled on a regular AP_ICC_LUT_DIM^3 grid over the
// [0,1]^3 device cube.
//
// `out` must point to AP_ICC_LUT_DIM^3 * 4 floats. It receives RGBA
// texels (alpha = 1), laid out red-fastest:
//   out[(((b * DIM) + g) * DIM + r) * 4 + channel]
// which is exactly the row-major content of a DIM-wide,
// DIM*DIM-tall 2D image — the form the pipeline graph uploads.
//
// Returns 0 on success. Returns non-zero — and leaves `out`
// untouched — when the file is missing, unreadable, or not an RGB
// profile. Callers treat a non-zero return as "profile not applied".
//
// This is not cheap (DIM^3 colour-managed conversions); it is meant
// to be called once when the pipeline graph is built, not per frame.
int ap_icc_bake_lut(const char *path, float *out);

#ifdef __cplusplus
}
#endif

#endif
