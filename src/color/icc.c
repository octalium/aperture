#include "color/icc.h"

#include "core/log.h"

#include <lcms2.h>

#include <stdlib.h>

// ICC -> 3D colour LUT.
//
// The profile's camera-RGB -> linear-sRGB transform is sampled on a
// regular AP_ICC_LUT_DIM^3 grid. LittleCMS does the full colour
// pipeline — TRC linearisation, the colorant matrix or the cLUT
// AToB table, white-point / chromatic adaptation, rendering intent —
// so a single grid sweep handles matrix and cLUT profiles uniformly.
// The destination is a linear-TRC sRGB profile, matching the camera-
// native matrix's convention: this stage outputs linear sRGB and the
// downstream output-transfer stage applies the display EOTF.

int ap_icc_bake_lut(const char *path, float *out)
{
    if (!path || !path[0] || !out) {
        return -1;
    }

    const cmsCIExyY d65 = { 0.3127, 0.3290, 1.0 };
    const cmsCIExyYTRIPLE prim = {
        { 0.6400, 0.3300, 1.0 },   // sRGB / Rec.709 red
        { 0.3000, 0.6000, 1.0 },   // green
        { 0.1500, 0.0600, 1.0 },   // blue
    };

    cmsHPROFILE   in   = NULL;
    cmsHPROFILE   dst  = NULL;
    cmsToneCurve *lin  = NULL;
    cmsHTRANSFORM xf   = NULL;
    double       *grid = NULL;
    double       *res  = NULL;
    int rc = -1;

    in = cmsOpenProfileFromFile(path, "r");
    if (!in) {
        AP_WARN("icc: cannot open profile: %s", path);
        goto done;
    }
    if (cmsGetColorSpace(in) != cmsSigRgbData) {
        AP_WARN("icc: not an RGB profile: %s", path);
        goto done;
    }

    lin = cmsBuildGamma(NULL, 1.0);
    if (!lin) {
        AP_WARN("icc: tone curve allocation failed");
        goto done;
    }
    cmsToneCurve *curves[3] = { lin, lin, lin };
    dst = cmsCreateRGBProfile(&d65, &prim, curves);
    if (!dst) {
        AP_WARN("icc: linear-sRGB profile allocation failed");
        goto done;
    }

    xf = cmsCreateTransform(in, TYPE_RGB_DBL, dst, TYPE_RGB_DBL,
                            INTENT_RELATIVE_COLORIMETRIC,
                            cmsFLAGS_NOOPTIMIZE | cmsFLAGS_HIGHRESPRECALC);
    if (!xf) {
        AP_WARN("icc: could not build transform for: %s", path);
        goto done;
    }

    const int dim = AP_ICC_LUT_DIM;
    const int n   = dim * dim * dim;
    grid = malloc((size_t)n * 3 * sizeof(double));
    res  = malloc((size_t)n * 3 * sizeof(double));
    if (!grid || !res) {
        AP_WARN("icc: LUT scratch allocation failed");
        goto done;
    }

    // Regular grid over the [0,1]^3 device cube, red varying fastest.
    int idx = 0;
    for (int b = 0; b < dim; b++) {
        for (int g = 0; g < dim; g++) {
            for (int r = 0; r < dim; r++) {
                grid[idx * 3 + 0] = (double)r / (double)(dim - 1);
                grid[idx * 3 + 1] = (double)g / (double)(dim - 1);
                grid[idx * 3 + 2] = (double)b / (double)(dim - 1);
                idx++;
            }
        }
    }

    cmsDoTransform(xf, grid, res, (cmsUInt32Number)n);

    for (int i = 0; i < n; i++) {
        out[i * 4 + 0] = (float)res[i * 3 + 0];
        out[i * 4 + 1] = (float)res[i * 3 + 1];
        out[i * 4 + 2] = (float)res[i * 3 + 2];
        out[i * 4 + 3] = 1.0f;
    }
    rc = 0;
    AP_INFO("icc: baked %d^3 LUT from %s", dim, path);

done:
    free(res);
    free(grid);
    if (xf)  cmsDeleteTransform(xf);
    if (dst) cmsCloseProfile(dst);
    if (lin) cmsFreeToneCurve(lin);
    if (in)  cmsCloseProfile(in);
    return rc;
}
