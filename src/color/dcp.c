#include "color/dcp.h"

#include "core/log.h"

#include <tiffio.h>

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// DCP tag IDs (DNG 1.4 spec, Appendix B).
#define DCP_TAG_COLOR_MATRIX_1     0xC621u
#define DCP_TAG_COLOR_MATRIX_2     0xC622u
#define DCP_TAG_FORWARD_MATRIX_1   0xC714u
#define DCP_TAG_FORWARD_MATRIX_2   0xC715u
#define DCP_TAG_CALIB_ILLUMINANT_1 0xC65Au
#define DCP_TAG_CALIB_ILLUMINANT_2 0xC65Bu
#define DCP_TAG_PROFILE_NAME       0xC6F4u

// EXIF LightSource enumeration values used as DCP illuminant IDs.
// (EXIF 2.32 spec, Table 8)
#define EXIF_LIGHT_DAYLIGHT         1u
#define EXIF_LIGHT_FLUORESCENT      2u
#define EXIF_LIGHT_TUNGSTEN         3u
#define EXIF_LIGHT_FLASH            4u
#define EXIF_LIGHT_FINE_WEATHER     9u
#define EXIF_LIGHT_CLOUDY_WEATHER  10u
#define EXIF_LIGHT_SHADE           11u
#define EXIF_LIGHT_DAY_FLUOR       12u
#define EXIF_LIGHT_DAYWHITE_FLUOR  13u
#define EXIF_LIGHT_COOLWHITE_FLUOR 14u
#define EXIF_LIGHT_WHITE_FLUOR     15u
#define EXIF_LIGHT_STD_A           17u
#define EXIF_LIGHT_STD_B           18u
#define EXIF_LIGHT_STD_C           19u
#define EXIF_LIGHT_D55             20u
#define EXIF_LIGHT_D65             21u
#define EXIF_LIGHT_D75             23u

// Return the correlated colour temperature in Kelvin for a known EXIF
// LightSource enum value, or 0.0f when the value is unrecognised.
static float illuminant_temp(uint16_t illuminant)
{
    switch (illuminant) {
    case EXIF_LIGHT_TUNGSTEN:         return 2850.0f;
    case EXIF_LIGHT_STD_A:            return 2856.0f;
    case EXIF_LIGHT_FLUORESCENT:      return 3800.0f;
    case EXIF_LIGHT_WHITE_FLUOR:      return 3450.0f;
    case EXIF_LIGHT_COOLWHITE_FLUOR:  return 4150.0f;
    case EXIF_LIGHT_STD_B:            return 4874.0f;
    case EXIF_LIGHT_DAYWHITE_FLUOR:   return 5000.0f;
    case EXIF_LIGHT_FINE_WEATHER:     return 5500.0f;
    case EXIF_LIGHT_FLASH:            return 5500.0f;
    case EXIF_LIGHT_DAYLIGHT:         return 5500.0f;
    case EXIF_LIGHT_D55:              return 5503.0f;
    case EXIF_LIGHT_STD_C:            return 6774.0f;
    case EXIF_LIGHT_D65:              return 6504.0f;
    case EXIF_LIGHT_DAY_FLUOR:        return 6430.0f;
    case EXIF_LIGHT_CLOUDY_WEATHER:   return 6500.0f;
    case EXIF_LIGHT_SHADE:            return 7500.0f;
    case EXIF_LIGHT_D75:              return 7504.0f;
    default:                          return 0.0f;
    }
}

// Estimate a correlated colour temperature (CCT) in Kelvin from the
// scene white-balance multipliers. The multipliers are normalised so
// green = 1.0; the red-to-blue ratio drives the CCT estimate.
//
// Camera WB multipliers are proportional to the gain needed to correct
// the scene illuminant to neutral. The red-to-blue ratio wb_r/wb_b
// is inversely related to scene colour temperature:
//   - warm scene (low CCT, tungsten-ish): sensor needs less red boost
//     and more blue boost, so wb_r < wb_b, giving wb_r/wb_b < 1
//   - cool scene (high CCT, shade-ish): sensor needs more red boost
//     and less blue boost, so wb_r > wb_b, giving wb_r/wb_b > 1
//
// We calibrate against D65 (6504 K) as the neutral reference where
// wb_r = wb_b = 1.0 for a daylight-balanced sensor:
//   CCT_approx = 6504 * (wb_r / wb_b)
//
// This first-order approximation is accurate enough for two-illuminant
// interpolation: an error of +/-500 K is negligible when interpolating
// between illuminants 3500 K apart.
static float wb_to_cct(float wb_r, float wb_g, float wb_b)
{
    (void)wb_g;
    const float eps = 1e-4f;
    float r = (wb_r > eps) ? wb_r : 1.0f;
    float b = (wb_b > eps) ? wb_b : 1.0f;
    float cct = 6504.0f * (r / b);
    if (cct < 1000.0f)  cct = 1000.0f;
    if (cct > 20000.0f) cct = 20000.0f;
    return cct;
}

// Compute the illuminant interpolation weight.
// Returns t in [0,1]: 0 = pure illuminant-1, 1 = pure illuminant-2.
// Interpolation is performed in mired space (M = 1e6/T) so the spacing
// is perceptually uniform along the Planckian locus (Adobe DNG spec).
static float illuminant_weight(float scene_cct, float t1_K, float t2_K)
{
    const float eps = 1.0f;
    if (t1_K < eps || t2_K < eps) return 1.0f;
    float inv_scene = 1.0f / scene_cct;
    float inv_t1    = 1.0f / t1_K;
    float inv_t2    = 1.0f / t2_K;
    float denom = inv_t2 - inv_t1;
    if (fabsf(denom) < 1e-12f) return 1.0f;
    float w = (inv_scene - inv_t1) / denom;
    if (w < 0.0f) w = 0.0f;
    if (w > 1.0f) w = 1.0f;
    return w;
}

// XYZ D50 -> linear sRGB via Bradford D50->D65 adaptation followed by
// the XYZ D65 -> sRGB primary matrix. Adobe DCP ColorMatrix and
// ForwardMatrix both use XYZ D50 as the PCS (DNG spec ss6.3).
//
// Product of:
//   Bradford D50->D65:
//     | 0.9555766 -0.0230393  0.0631636 |
//     |-0.0282895  1.0099416  0.0210077 |
//     | 0.0122982 -0.0204830  1.3299098 |
//   XYZ D65 -> linear sRGB (IEC 61966-2-1 inverse):
//     | 3.2404542 -1.5371385 -0.4985314 |
//     |-0.9692660  1.8760108  0.0415560 |
//     | 0.0556434 -0.2040259  1.0572252 |
static const float k_xyz_d50_to_srgb[3][3] = {
    {  3.1338561f, -1.6168667f, -0.4906146f },
    { -0.9787684f,  1.9161415f,  0.0334540f },
    {  0.0719453f, -0.2289914f,  1.4052427f },
};

// 3x3 matrix multiply: out = A * B (all row-major).
static void mat33_mul(const float A[3][3], const float B[3][3], float out[3][3])
{
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            float s = 0.0f;
            for (int k = 0; k < 3; k++) s += A[i][k] * B[k][j];
            out[i][j] = s;
        }
    }
}

// 3x3 matrix inverse via cofactor expansion.
// Returns 0 on success, -1 when the matrix is singular.
static int mat33_inv(const float M[3][3], float inv[3][3])
{
    float c00 = M[1][1]*M[2][2] - M[1][2]*M[2][1];
    float c01 = M[1][2]*M[2][0] - M[1][0]*M[2][2];
    float c02 = M[1][0]*M[2][1] - M[1][1]*M[2][0];
    float det = M[0][0]*c00 + M[0][1]*c01 + M[0][2]*c02;
    if (fabsf(det) < 1e-10f) return -1;
    float id = 1.0f / det;
    inv[0][0] = c00 * id;
    inv[0][1] = (M[0][2]*M[2][1] - M[0][1]*M[2][2]) * id;
    inv[0][2] = (M[0][1]*M[1][2] - M[0][2]*M[1][1]) * id;
    inv[1][0] = c01 * id;
    inv[1][1] = (M[0][0]*M[2][2] - M[0][2]*M[2][0]) * id;
    inv[1][2] = (M[0][2]*M[1][0] - M[0][0]*M[1][2]) * id;
    inv[2][0] = c02 * id;
    inv[2][1] = (M[0][1]*M[2][0] - M[0][0]*M[2][1]) * id;
    inv[2][2] = (M[0][0]*M[1][1] - M[0][1]*M[1][0]) * id;
    return 0;
}

// Normalise each row of a 3x3 matrix so its elements sum to 1.0.
// Required by the DNG spec before inverting a ColorMatrix: row
// normalisation ensures the camera neutral (1,1,1) maps to the D50
// white point after inversion.
static void mat33_normalise_rows(float M[3][3])
{
    for (int i = 0; i < 3; i++) {
        float s = M[i][0] + M[i][1] + M[i][2];
        if (fabsf(s) < 1e-10f) continue;
        float inv_s = 1.0f / s;
        M[i][0] *= inv_s;
        M[i][1] *= inv_s;
        M[i][2] *= inv_s;
    }
}

// Read a DCP 3x3 rational-array tag from the open TIFF.
// DCP stores 3x3 matrices as 9 SRATIONAL values in row-major order.
// libtiff reads unknown (anonymous) SRATIONAL tags as TIFF_SETGET_C32_FLOAT:
//   TIFFGetField(tif, tag, &uint32_count, &float_ptr)
// where each SRATIONAL numerator/denominator pair has been converted
// to a float. Returns 0 on success, -1 when the tag is absent or
// has a count other than 9.
static int read_matrix_tag(TIFF *tif, uint32_t tag, float mat[3][3])
{
    uint32_t count = 0;
    float   *vals  = NULL;
    if (!TIFFGetField(tif, tag, &count, &vals)) return -1;
    if (count != 9 || !vals) return -1;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            mat[i][j] = vals[i * 3 + j];
    return 0;
}

// Read a single SHORT tag (CalibrationIlluminant1/2).
// Anonymous SHORT arrays use TIFF_SETGET_C32_UINT16:
//   TIFFGetField(tif, tag, &uint32_count, &uint16_ptr)
static int read_short_tag(TIFF *tif, uint32_t tag, uint16_t *out)
{
    uint32_t  count = 0;
    uint16_t *vals  = NULL;
    if (!TIFFGetField(tif, tag, &count, &vals)) return -1;
    if (count < 1 || !vals) return -1;
    *out = vals[0];
    return 0;
}

// Read a variable-length ASCII tag (ProfileName).
// Anonymous ASCII tags use TIFF_SETGET_C32_ASCII:
//   TIFFGetField(tif, tag, &uint32_count, &char_ptr)
static int read_ascii_tag(TIFF *tif, uint32_t tag, char *buf, size_t bufsz)
{
    uint32_t count = 0;
    char    *s     = NULL;
    if (!TIFFGetField(tif, tag, &count, &s)) return -1;
    if (!s) return -1;
    size_t n = (count < bufsz) ? count : bufsz - 1;
    memcpy(buf, s, n);
    buf[n] = '\0';
    return 0;
}

static void dcp_tiff_error(const char *module, const char *fmt, va_list ap)
{
    char msg[512];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    AP_ERROR("dcp/tiff(%s): %s", module ? module : "?", msg);
}

static void dcp_tiff_warning(const char *module, const char *fmt, va_list ap)
{
    char msg[512];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    AP_WARN("dcp/tiff(%s): %s", module ? module : "?", msg);
}

int ap_dcp_bake_lut(const char *path,
                    float wb_r, float wb_g, float wb_b,
                    float *out)
{
    if (!path || !path[0] || !out) return -1;

    TIFFSetErrorHandler(dcp_tiff_error);
    TIFFSetWarningHandler(dcp_tiff_warning);

    TIFF *tif = TIFFOpen(path, "r");
    if (!tif) {
        AP_WARN("dcp: cannot open file: %s", path);
        return -1;
    }

    // Illuminant identifiers. Default to Standard A / D65, the most
    // common DCP pairing, when the tags are absent.
    uint16_t illum1 = (uint16_t)EXIF_LIGHT_STD_A;
    uint16_t illum2 = (uint16_t)EXIF_LIGHT_D65;
    read_short_tag(tif, DCP_TAG_CALIB_ILLUMINANT_1, &illum1);
    read_short_tag(tif, DCP_TAG_CALIB_ILLUMINANT_2, &illum2);

    float t1_K = illuminant_temp(illum1);
    float t2_K = illuminant_temp(illum2);
    if (t1_K < 1.0f) t1_K = 2856.0f;
    if (t2_K < 1.0f) t2_K = 6504.0f;

    // Colour matrices. ColorMatrix1 is mandatory; ColorMatrix2 is
    // optional (single-illuminant profiles carry only CM1).
    float cm1[3][3];
    float cm2[3][3];
    bool  has_cm1 = (read_matrix_tag(tif, DCP_TAG_COLOR_MATRIX_1, cm1) == 0);
    bool  has_cm2 = (read_matrix_tag(tif, DCP_TAG_COLOR_MATRIX_2, cm2) == 0);

    float fm1[3][3];
    float fm2[3][3];
    bool  has_fm1 = (read_matrix_tag(tif, DCP_TAG_FORWARD_MATRIX_1, fm1) == 0);
    bool  has_fm2 = (read_matrix_tag(tif, DCP_TAG_FORWARD_MATRIX_2, fm2) == 0);

    char profile_name[256] = { 0 };
    read_ascii_tag(tif, DCP_TAG_PROFILE_NAME, profile_name, sizeof(profile_name));

    TIFFClose(tif);

    if (!has_cm1) {
        AP_WARN("dcp: no ColorMatrix1 in: %s", path);
        return -1;
    }

    // Derive scene CCT from WB multipliers and compute illuminant weight.
    const float wb_r_n = (wb_r > 1e-4f) ? wb_r : 1.0f;
    const float wb_g_n = (wb_g > 1e-4f) ? wb_g : 1.0f;
    const float wb_b_n = (wb_b > 1e-4f) ? wb_b : 1.0f;
    float scene_cct = wb_to_cct(wb_r_n, wb_g_n, wb_b_n);
    float weight    = has_cm2 ? illuminant_weight(scene_cct, t1_K, t2_K)
                              : 0.0f;

    // Build the interpolated camera -> XYZ D50 matrix.
    // When ForwardMatrices are present, use them; they map camera -> XYZ
    // D50 directly. When only ColorMatrices are available, invert the
    // interpolated ColorMatrix (XYZ D50 -> camera) to get camera -> XYZ D50.
    float cam_to_xyz[3][3];

    bool use_forward = (has_fm1 || has_fm2)
                    && (has_fm1 || weight >= 0.5f)
                    && (has_fm2 || weight <  0.5f);

    if (use_forward) {
        if (has_fm1 && has_fm2) {
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++)
                    cam_to_xyz[i][j] = fm1[i][j] * (1.0f - weight)
                                     + fm2[i][j] * weight;
        } else if (has_fm1) {
            memcpy(cam_to_xyz, fm1, sizeof(fm1));
        } else {
            memcpy(cam_to_xyz, fm2, sizeof(fm2));
        }
    } else {
        // Interpolate ColorMatrix pair, normalise rows (DNG spec ss6.3),
        // then invert to get camera -> XYZ D50.
        float cm_interp[3][3];
        if (has_cm2) {
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++)
                    cm_interp[i][j] = cm1[i][j] * (1.0f - weight)
                                    + cm2[i][j] * weight;
        } else {
            memcpy(cm_interp, cm1, sizeof(cm1));
        }
        mat33_normalise_rows(cm_interp);
        if (mat33_inv(cm_interp, cam_to_xyz) != 0) {
            AP_WARN("dcp: ColorMatrix is singular in: %s", path);
            return -1;
        }
    }

    // Chain: cam -> XYZ_D50 -> linear sRGB.
    float cam_to_srgb[3][3];
    mat33_mul(k_xyz_d50_to_srgb, cam_to_xyz, cam_to_srgb);

    // Bake the 3x3 matrix into a AP_ICC_LUT_DIM^3 colour LUT.
    // Layout: red varies fastest; alpha channel is always 1.0.
    const int dim = AP_ICC_LUT_DIM;
    for (int b = 0; b < dim; b++) {
        for (int g = 0; g < dim; g++) {
            for (int r = 0; r < dim; r++) {
                float fr = (float)r / (float)(dim - 1);
                float fg = (float)g / (float)(dim - 1);
                float fb = (float)b / (float)(dim - 1);

                int idx = ((b * dim) + g) * dim + r;
                out[idx * 4 + 0] = cam_to_srgb[0][0] * fr
                                 + cam_to_srgb[0][1] * fg
                                 + cam_to_srgb[0][2] * fb;
                out[idx * 4 + 1] = cam_to_srgb[1][0] * fr
                                 + cam_to_srgb[1][1] * fg
                                 + cam_to_srgb[1][2] * fb;
                out[idx * 4 + 2] = cam_to_srgb[2][0] * fr
                                 + cam_to_srgb[2][1] * fg
                                 + cam_to_srgb[2][2] * fb;
                out[idx * 4 + 3] = 1.0f;
            }
        }
    }

    if (profile_name[0]) {
        AP_INFO("dcp: baked %d^3 LUT from \"%s\" (%s, %.0f K, t=%.3f)",
                dim, profile_name, path, scene_cct, weight);
    } else {
        AP_INFO("dcp: baked %d^3 LUT from %s (%.0f K, t=%.3f)",
                dim, path, scene_cct, weight);
    }
    return 0;
}
