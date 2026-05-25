#include "module.h"

#include "chromatic_aberration_comp_spv.h"
#include "lens_match.h"

#include "photo/metadata.h"

#include "cimgui.h"

#include <lensfun/lensfun.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

// Chromatic Aberration — lateral CA removal via per-channel resample.
//
// Operates after lens_correction in the pipeline order: distortion +
// vignetting have already corrected the geometry, so the channel-wise
// magnification difference is what's left for this module to undo.
//
// Two modes share a single compute shader. Auto pulls the lfTCAModel
// terms from a Lensfun match keyed on (camera, lens) using the shared
// ap_lens_match cache; Manual layers user-supplied scale and radial
// offset sliders over an identity model. The mode default flips to
// Auto whenever the current (camera, lens) yields a real Lensfun TCA
// calibration and Manual otherwise.
//
// Parameter layout
// ----------------
// Float params:
//   SLOT_MODE      (0)  0 = Auto, 1 = Manual
//   SLOT_FOCAL_MM  (1)  Focal length in mm; 0 = take from EXIF
//   SLOT_R_SCALE   (2)  Manual: red scale     (default 1.0)
//   SLOT_B_SCALE   (3)  Manual: blue scale    (default 1.0)
//   SLOT_R_OFFSET  (4)  Manual: red radial offset    (default 0.0)
//   SLOT_B_OFFSET  (5)  Manual: blue radial offset   (default 0.0)
//
// String params (AP_EDIT_STR_SLOTS == 2):
//   STR_CAMERA  (0)  "Make Model" override; empty → use EXIF
//   STR_LENS    (1)  "Lens model" override; empty → use EXIF

typedef struct {
    int    tca_model;       // 0 = none / manual, 1 = linear, 2 = poly3
    int    _pad0;
    int    _pad1;
    int    _pad2;

    float  red_terms[4];    // linear: kr in [0]; poly3: br, cr, vr
    float  blue_terms[4];   // linear: kb in [0]; poly3: bb, cb, vb

    float  r_scale;
    float  b_scale;
    float  r_offset;
    float  b_offset;

    float  center_x;
    float  center_y;
    float  _pad3;
    float  _pad4;
} ca_push_t;

enum {
    SLOT_MODE     = 0,
    SLOT_FOCAL_MM = 1,
    SLOT_R_SCALE  = 2,
    SLOT_B_SCALE  = 3,
    SLOT_R_OFFSET = 4,
    SLOT_B_OFFSET = 5,
    SLOT_COUNT    = 6,
};

enum {
    STR_CAMERA = 0,
    STR_LENS   = 1,
};

enum {
    MODE_AUTO   = 0,
    MODE_MANUAL = 1,
};

// tca_model values must match shaders/chromatic_aberration.comp.
enum {
    SHADER_TCA_NONE   = 0,
    SHADER_TCA_LINEAR = 1,
    SHADER_TCA_POLY3  = 2,
};

static const float       ca_defaults[] = {
    /* mode */     0.0f,
    /* focal_mm */ 0.0f,
    /* r_scale */  1.0f,
    /* b_scale */  1.0f,
    /* r_offset */ 0.0f,
    /* b_offset */ 0.0f,
};
static const char *const ca_param_names[] = {
    "mode", "focal_mm", "r_scale", "b_scale", "r_offset", "b_offset",
};
static const char *const ca_str_names[] = {
    "camera_override", "lens_override",
};

static float parse_focal(const char *s)
{
    if (!s || !s[0]) return 0.0f;
    float v = 0.0f;
    if (sscanf(s, "%f", &v) != 1) return 0.0f;
    return v;
}

// Accepts either "f/2.8" or a bare "2.8". Returns 0 on parse failure.
static float parse_aperture(const char *s)
{
    if (!s || !s[0]) return 0.0f;
    float v = 0.0f;
    if (sscanf(s, "f/%f", &v) == 1) return v;
    if (sscanf(s, "%f", &v) == 1) return v;
    return 0.0f;
}

// Fill `pc` with the per-channel TCA terms for the matched lens at
// `focal`. Returns true when Lensfun supplied a usable, finite model
// and the shader should run in auto mode; false when no calibration is
// available or any required coefficient interpolated to NaN/Inf
// (caller skips the stage rather than dispatching garbage).
//
// Lensfun's POLY3 Terms[] is interleaved per-channel rather than
// concatenated: Terms[0..5] = (vr, vb, cr, cb, br, bb). Source:
// lensfun/libs/lensfun/database.cpp attribute parser.
static bool fill_auto_terms(const lfLens *lens, float focal, ca_push_t *pc)
{
    if (!lens) return false;

    lfLensCalibTCA tca = {0};
    if (!lf_lens_interpolate_tca(lens, focal, &tca)) return false;

    switch (tca.Model) {
    case LF_TCA_MODEL_LINEAR:
        if (!isfinite(tca.Terms[0]) || !isfinite(tca.Terms[1])) return false;
        pc->tca_model     = SHADER_TCA_LINEAR;
        pc->red_terms[0]  = tca.Terms[0]; // kr
        pc->blue_terms[0] = tca.Terms[1]; // kb
        return true;
    case LF_TCA_MODEL_POLY3:
        for (int i = 0; i < 6; i++) {
            if (!isfinite(tca.Terms[i])) return false;
        }
        pc->tca_model     = SHADER_TCA_POLY3;
        pc->red_terms[0]  = tca.Terms[4]; // br
        pc->red_terms[1]  = tca.Terms[2]; // cr
        pc->red_terms[2]  = tca.Terms[0]; // vr
        pc->blue_terms[0] = tca.Terms[5]; // bb
        pc->blue_terms[1] = tca.Terms[3]; // cb
        pc->blue_terms[2] = tca.Terms[1]; // vb
        return true;
    default:
        return false;
    }
}

// Cached resolve helper restricted to the module's STR slots. Returns
// the cached match for the configured camera+lens strings, narrowed by
// the EXIF hints when supplied — used by both pack_push and
// render_params so they observe the same state.
static const ap_lens_match *resolve_module_match(
    const char (*str_params)[AP_EDIT_STR_LEN],
    const ap_lens_match_hints *hints)
{
    const char *cam_str  = str_params ? str_params[STR_CAMERA] : "";
    const char *lens_str = str_params ? str_params[STR_LENS]   : "";
    return ap_lens_match_resolve(cam_str, lens_str, hints);
}

static int ca_pack_push(const ap_module *self,
                        const float *params,
                        const char (*str_params)[AP_EDIT_STR_LEN],
                        const ap_raw_metadata *meta,
                        void *push_out)
{
    (void)self;

    ca_push_t *pc = push_out;
    memset(pc, 0, sizeof(*pc));

    // identity defaults: shader skips its model branch and uses manual
    // values which themselves default to scale=1, offset=0.
    pc->tca_model     = SHADER_TCA_NONE;
    pc->red_terms[0]  = 1.0f;
    pc->blue_terms[0] = 1.0f;

    int   mode  = params ? (int)params[SLOT_MODE]     : MODE_AUTO;
    float focal = params ? params[SLOT_FOCAL_MM]      : 0.0f;
    pc->r_scale  = params ? params[SLOT_R_SCALE]  : 1.0f;
    pc->b_scale  = params ? params[SLOT_B_SCALE]  : 1.0f;
    pc->r_offset = params ? params[SLOT_R_OFFSET] : 0.0f;
    pc->b_offset = params ? params[SLOT_B_OFFSET] : 0.0f;

    if (mode == MODE_AUTO) {
        if (!ap_lens_match_ensure_db()) return -1;
        const ap_lens_match_hints hints = {
            .shot_focal_mm     = focal,
            .lens_min_focal    = meta ? meta->lens_min_focal    : 0.0f,
            .lens_max_focal    = meta ? meta->lens_max_focal    : 0.0f,
            .lens_min_aperture = meta ? meta->lens_min_aperture : 0.0f,
        };
        const ap_lens_match *match = resolve_module_match(str_params, &hints);
        if (!match->lens) return -1;
        if (focal <= 0.0f) focal = 50.0f;
        if (!fill_auto_terms(match->lens, focal, pc)) return -1;
        pc->center_x = match->lens->CenterX;
        pc->center_y = match->lens->CenterY;
        // auto path ignores manual sliders — keep the layered values
        // at identity so the shader matches Lensfun's calibration.
        pc->r_scale  = 1.0f;
        pc->b_scale  = 1.0f;
        pc->r_offset = 0.0f;
        pc->b_offset = 0.0f;
        return 0;
    }

    // manual path: skip when nothing would shift.
    if (pc->r_scale == 1.0f && pc->b_scale == 1.0f
        && pc->r_offset == 0.0f && pc->b_offset == 0.0f) {
        return -1;
    }
    return 0;
}

static void build_exif_cam(const ap_photo_metadata *fm, char *out, size_t len)
{
    out[0] = '\0';
    if (!fm) return;
    const char *make  = ap_photo_metadata_get(fm, AP_META_CAMERA_MAKE);
    const char *model = ap_photo_metadata_get(fm, AP_META_CAMERA_MODEL);
    if (make && make[0] && model && model[0]) {
        snprintf(out, len, "%s %s", make, model);
    } else if (model && model[0]) {
        snprintf(out, len, "%s", model);
    }
}

static void build_exif_lens(const ap_photo_metadata *fm, char *out, size_t len)
{
    out[0] = '\0';
    if (!fm) return;
    const char *make  = ap_photo_metadata_get(fm, AP_META_LENS_MAKE);
    const char *model = ap_photo_metadata_get(fm, AP_META_LENS_MODEL);
    if (make && make[0] && model && model[0]) {
        snprintf(out, len, "%s %s", make, model);
    } else if (model && model[0]) {
        snprintf(out, len, "%s", model);
    }
}

static void ca_render(const ap_module *self, float *params,
                      const ap_module_render_ctx *ctx)
{
    if (!params) return;

    const ap_photo_metadata *fm = ctx->file_meta;
    bool db_ok = ap_lens_match_ensure_db();
    if (!db_ok) {
        igTextColored((ImVec4_c){ 1.0f, 0.4f, 0.4f, 1.0f },
                      "Lensfun database not found");
    }

    char exif_cam[AP_META_VALUE_LEN * 2 + 2];
    build_exif_cam(fm, exif_cam, sizeof(exif_cam));
    if (!ctx->str_params[STR_CAMERA][0] && exif_cam[0]) {
        strncpy(ctx->str_params[STR_CAMERA], exif_cam, AP_EDIT_STR_LEN - 1);
        ctx->str_params[STR_CAMERA][AP_EDIT_STR_LEN - 1] = '\0';
        *ctx->request_rebuild = true;
    }
    igInputText("Camera", ctx->str_params[STR_CAMERA], AP_EDIT_STR_LEN, 0, NULL, NULL);
    if (igIsItemDeactivatedAfterEdit()) {
        *ctx->request_rebuild    = true;
        *ctx->snapshot_requested = true;
    }
    if (exif_cam[0]) {
        igSameLine(0.0f, 4.0f);
        igTextDisabled("EXIF: %s", exif_cam);
    }

    char exif_lens[AP_META_VALUE_LEN * 2 + 2];
    build_exif_lens(fm, exif_lens, sizeof(exif_lens));
    if (!ctx->str_params[STR_LENS][0] && exif_lens[0]) {
        strncpy(ctx->str_params[STR_LENS], exif_lens, AP_EDIT_STR_LEN - 1);
        ctx->str_params[STR_LENS][AP_EDIT_STR_LEN - 1] = '\0';
        *ctx->request_rebuild = true;
    }
    igInputText("Lens", ctx->str_params[STR_LENS], AP_EDIT_STR_LEN, 0, NULL, NULL);
    if (igIsItemDeactivatedAfterEdit()) {
        *ctx->request_rebuild    = true;
        *ctx->snapshot_requested = true;
    }
    if (exif_lens[0]) {
        igSameLine(0.0f, 4.0f);
        igTextDisabled("EXIF: %s", exif_lens);
    }

    // Build EXIF-derived narrowing hints from file_meta so the render-
    // side resolve hits the same cache entry as pack_push.
    ap_lens_match_hints hints = {
        .shot_focal_mm     = params[SLOT_FOCAL_MM],
        .lens_min_focal    = fm ? parse_focal(
            ap_photo_metadata_get(fm, AP_META_LENS_MIN_FOCAL)) : 0.0f,
        .lens_max_focal    = fm ? parse_focal(
            ap_photo_metadata_get(fm, AP_META_LENS_MAX_FOCAL)) : 0.0f,
        .lens_min_aperture = fm ? parse_aperture(
            ap_photo_metadata_get(fm, AP_META_LENS_MIN_APERTURE)) : 0.0f,
    };
    const ap_lens_match *match = ap_lens_match_resolve(
        ctx->str_params[STR_CAMERA], ctx->str_params[STR_LENS], &hints);

    // probe whether the matched lens carries TCA calibration at the
    // currently-set focal length — this drives the Auto/Manual default
    // and the inline match status text.
    float probe_focal = params[SLOT_FOCAL_MM];
    if (probe_focal <= 0.0f) {
        const char *exif_focal_s = fm ? ap_photo_metadata_get(fm, AP_META_FOCAL_LEN) : "";
        probe_focal = parse_focal(exif_focal_s);
    }
    if (probe_focal <= 0.0f) probe_focal = 50.0f;

    bool has_tca         = false;
    bool tca_unsupported = false;
    if (match->lens) {
        lfLensCalibTCA probe = {0};
        if (lf_lens_interpolate_tca(match->lens, probe_focal, &probe)
            && probe.Model != LF_TCA_MODEL_NONE) {
            has_tca = true;
            // shader only implements LINEAR + POLY3; anything else
            // (Lensfun 0.3.95+ adds ACM = Adobe Camera Model) falls
            // through fill_auto_terms and silently disables Auto.
            tca_unsupported = (probe.Model != LF_TCA_MODEL_LINEAR
                               && probe.Model != LF_TCA_MODEL_POLY3);
        }
    }

    const ImVec4_c ok_col   = { 0.4f, 1.0f, 0.5f, 1.0f };
    const ImVec4_c warn_col = { 1.0f, 0.8f, 0.4f, 1.0f };
    const ImVec4_c bad_col  = { 1.0f, 0.5f, 0.5f, 1.0f };
    if (match->cam) {
        igTextColored(ok_col, "\xe2\x9c\x93 Camera: %s (score %d)",
                      match->cam_name, match->cam_score);
    } else if (ctx->str_params[STR_CAMERA][0]) {
        igTextColored(warn_col, "\xe2\x9a\xa0 Camera not found in Lensfun DB");
    }
    if (match->lens) {
        if (tca_unsupported) {
            igTextColored(warn_col,
                          "\xe2\x9a\xa0 Lens: %s (TCA model not yet supported, likely ACM — use Manual)",
                          match->lens_name);
        } else if (has_tca) {
            igTextColored(ok_col, "\xe2\x9c\x93 Lens: %s (TCA available)",
                          match->lens_name);
        } else {
            igTextColored(warn_col,
                          "\xe2\x9a\xa0 Lens: %s (no TCA data — use Manual)",
                          match->lens_name);
        }
    } else if (match->lens_rejected) {
        igTextColored(bad_col,
                      "\xe2\x9a\xa0 Lens match too weak (best: %s, score %d/%d)",
                      match->lens_name, match->lens_score,
                      AP_LENS_MATCH_MIN_SCORE);
    } else if (ctx->str_params[STR_LENS][0]) {
        igTextColored(bad_col, "\xe2\x9a\xa0 No lens match");
    }

    igSeparator();

    const char *mode_items = "Auto\0Manual\0";
    int mode = (int)params[SLOT_MODE];
    if (mode < 0) mode = 0;
    if (mode > 1) mode = 1;
    if (igCombo_Str("Mode", &mode, mode_items, -1)) {
        params[SLOT_MODE]        = (float)mode;
        *ctx->request_rebuild    = true;
        *ctx->snapshot_requested = true;
    }
    igTextDisabled(mode == MODE_AUTO
                   ? "Lensfun TCA calibration drives the correction."
                   : "Manual scale and radial offset bypass Lensfun.");

    if (mode == MODE_AUTO) {
        const char *exif_focal_s = fm ? ap_photo_metadata_get(fm, AP_META_FOCAL_LEN) : "";
        if (params[SLOT_FOCAL_MM] <= 0.0f && exif_focal_s && exif_focal_s[0]) {
            float parsed = parse_focal(exif_focal_s);
            if (parsed > 0.0f) {
                params[SLOT_FOCAL_MM] = parsed;
                *ctx->request_rebuild = true;
            }
        }
        float focal = params[SLOT_FOCAL_MM];
        if (igInputFloat("Focal length (mm)", &focal, 1.0f, 10.0f, "%.1f", 0)) {
            if (focal < 1.0f) focal = 1.0f;
            params[SLOT_FOCAL_MM] = focal;
        }
        if (igIsItemActivated()) {
            *ctx->snapshot_requested = true;
        }
        if (exif_focal_s && exif_focal_s[0]) {
            igSameLine(0.0f, 4.0f);
            igTextDisabled("EXIF: %s", exif_focal_s);
        }
    } else {
        ap_module_slider_reset(self, params, "Red scale",
                               SLOT_R_SCALE,  0.995f, 1.005f, "%.4f");
        ap_module_slider_reset(self, params, "Blue scale",
                               SLOT_B_SCALE,  0.995f, 1.005f, "%.4f");
        ap_module_slider_reset(self, params, "Red radial offset",
                               SLOT_R_OFFSET, -0.005f, 0.005f, "%.4f");
        ap_module_slider_reset(self, params, "Blue radial offset",
                               SLOT_B_OFFSET, -0.005f, 0.005f, "%.4f");
        igTextDisabled("Tune at the corners — defaults are identity.");
    }

    igTextDisabled("Clear Camera/Lens to re-detect from EXIF.");
}

const ap_module module_chromatic_aberration = {
    .name             = "chromatic_aberration",
    .display_name     = "Chromatic Aberration",
    .category         = AP_MODULE_GEOMETRIC,
    .user_visible     = true,
    .spv_data         = chromatic_aberration_comp_spv,
    .spv_size         = chromatic_aberration_comp_spv_size,
    .push_size        = sizeof(ca_push_t),
    .pack_push        = ca_pack_push,
    .params_count     = SLOT_COUNT,
    .params_default   = ca_defaults,
    .params_names     = ca_param_names,
    .render_params    = ca_render,
    .str_params_count = 2,
    .str_params_names = ca_str_names,
};
