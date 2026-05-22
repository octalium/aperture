#ifndef APERTURE_MODULE_WB_H
#define APERTURE_MODULE_WB_H

#ifdef __cplusplus
extern "C" {
#endif

// White-balance eyedropper solver.
//
// `params` is a White Balance edit entry's parameter slot array.
// `sample_r/g/b` are the rendered (displayed) pixel the user clicked,
// as 8-bit sRGB components in [0,255] — i.e. the bytes the canvas
// readback produced.
//
// Rewrites `params` so the clicked pixel reads neutral: the entry is
// switched to the Manual variant and its R/G/B multipliers are scaled
// by the per-channel correction that equalises the sample (linearised
// before the ratio so the solve is photometrically correct, green
// anchored so overall exposure is preserved). A near-black sample
// carries no chroma to correct from and is left as a no-op.
//
// Returns true when the params were updated, false when the sample
// was too dark to solve a meaningful correction.
bool ap_wb_apply_neutral_pick(float *params,
                              float sample_r, float sample_g, float sample_b);

#ifdef __cplusplus
}
#endif

#endif
