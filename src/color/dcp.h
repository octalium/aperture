#ifndef APERTURE_COLOR_DCP_H
#define APERTURE_COLOR_DCP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "color/icc.h"  // AP_ICC_LUT_DIM

// Parse a DCP file and bake it into a 3D colour LUT.
//
// DCP (Adobe DNG Camera Profile) is a TIFF container that carries
// per-camera colour matrices mapping raw camera RGB to the PCS
// (Profile Connection Space, XYZ D50). Two-illuminant profiles store
// a pair of 3×3 matrices — ColorMatrix1/2 and optionally
// ForwardMatrix1/2 — calibrated at two reference illuminants
// (typically Standard Illuminant A / tungsten and D65). This function
// interpolates between the two matrices based on the scene's
// correlated colour temperature, derived from the raw metadata's
// white-balance multipliers.
//
// The resulting matrix is baked into a AP_ICC_LUT_DIM^3 colour LUT
// using the same layout as ap_icc_bake_lut (RGBA, red varying fastest,
// alpha = 1.0). The DCP matrix maps camera RGB → linear sRGB, so the
// LUT output is directly compatible with the profile_lut shader.
//
// `path`     - filesystem path to the .dcp file.
// `wb_r`, `wb_g`, `wb_b` - white-balance multipliers (normalised so
//              green = 1.0), from ap_raw_metadata::wb_mul. Used to
//              derive the scene colour temperature for illuminant
//              interpolation. When both multipliers are zero the
//              function falls back to illuminant-2 (D65).
// `out`      - pointer to AP_ICC_LUT_DIM^3 * 4 floats. Receives the
//              baked RGBA LUT; untouched on failure.
//
// Returns 0 on success. Returns non-zero when the file is missing,
// not a valid DCP, or contains no usable colour matrices.
int ap_dcp_bake_lut(const char *path,
                    float wb_r, float wb_g, float wb_b,
                    float *out);

#ifdef __cplusplus
}
#endif

#endif
