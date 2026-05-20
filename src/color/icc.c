#include "color/icc.h"

#include "core/log.h"

#include <lcms2.h>

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// ICC -> camera matrix.
//
// For a matrix/shaper profile the camera-RGB -> linear-sRGB transform
// is a pure 3x3 (the colorant matrix). We recover it by running the
// three basis vectors through a LittleCMS transform whose destination
// is a linear-TRC sRGB profile: lcms2 does the full colour pipeline
// (white-point / chromatic adaptation, rendering intent), and because
// a matrix/shaper profile's tone curves are normalised — TRC(0)=0,
// TRC(1)=1 — the unit inputs pass the colorant matrix through
// unchanged. The (0,0,0) sample captures any offset (nominally zero).
//
// cLUT profiles cannot be reduced to a 3x3; ap_icc_camera_matrix
// rejects them so the caller falls back to the camera-native matrix.

#define ICC_CACHE_SLOTS 4
#define ICC_PATH_MAX    1024

// Derive the matrix from the profile at `path`. Returns 0 on success.
static int icc_derive_matrix(const char *path, float out[9])
{
    const cmsCIExyY d65 = { 0.3127, 0.3290, 1.0 };
    const cmsCIExyYTRIPLE prim = {
        { 0.6400, 0.3300, 1.0 },   // sRGB / Rec.709 red
        { 0.3000, 0.6000, 1.0 },   // green
        { 0.1500, 0.0600, 1.0 },   // blue
    };

    cmsHPROFILE   in  = NULL;
    cmsHPROFILE   dst = NULL;
    cmsToneCurve *lin = NULL;
    cmsHTRANSFORM xf  = NULL;
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
    if (!cmsIsMatrixShaper(in)) {
        AP_WARN("icc: '%s' is a cLUT profile; matrix extraction needs "
                "the 3D-LUT path", path);
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
                            cmsFLAGS_NOOPTIMIZE);
    if (!xf) {
        AP_WARN("icc: could not build transform for: %s", path);
        goto done;
    }

    const double src_px[4][3] = {
        { 0.0, 0.0, 0.0 },
        { 1.0, 0.0, 0.0 },
        { 0.0, 1.0, 0.0 },
        { 0.0, 0.0, 1.0 },
    };
    double dst_px[4][3];
    cmsDoTransform(xf, src_px, dst_px, 4);

    // Column c of the matrix = transform(e_c) - transform(0).
    for (int col = 0; col < 3; col++) {
        for (int row = 0; row < 3; row++) {
            out[row * 3 + col] =
                (float)(dst_px[1 + col][row] - dst_px[0][row]);
        }
    }
    rc = 0;
    AP_INFO("icc: loaded matrix profile %s", path);

done:
    if (xf)  cmsDeleteTransform(xf);
    if (dst) cmsCloseProfile(dst);
    if (lin) cmsFreeToneCurve(lin);
    if (in)  cmsCloseProfile(in);
    return rc;
}

// Path-keyed cache. pack_push calls this every frame; the profile is
// parsed once per distinct path. Failures are cached too, so a bad
// path is not re-opened (and re-logged) every frame.
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct icc_cache_slot {
    char  path[ICC_PATH_MAX];
    float matrix[9];
    int   rc;
    bool  valid;
} g_cache[ICC_CACHE_SLOTS];
static int g_cache_next;

int ap_icc_camera_matrix(const char *path, float out[9])
{
    if (!path || !path[0] || !out) {
        return -1;
    }

    pthread_mutex_lock(&g_lock);

    for (int i = 0; i < ICC_CACHE_SLOTS; i++) {
        if (g_cache[i].valid && strcmp(g_cache[i].path, path) == 0) {
            int rc = g_cache[i].rc;
            if (rc == 0) {
                memcpy(out, g_cache[i].matrix, sizeof(float) * 9);
            }
            pthread_mutex_unlock(&g_lock);
            return rc;
        }
    }

    float matrix[9];
    int rc = icc_derive_matrix(path, matrix);

    struct icc_cache_slot *slot = &g_cache[g_cache_next];
    g_cache_next = (g_cache_next + 1) % ICC_CACHE_SLOTS;
    snprintf(slot->path, sizeof(slot->path), "%s", path);
    slot->rc    = rc;
    slot->valid = true;
    if (rc == 0) {
        memcpy(slot->matrix, matrix, sizeof(matrix));
        memcpy(out, matrix, sizeof(matrix));
    }

    pthread_mutex_unlock(&g_lock);
    return rc;
}
