#include "module.h"

#include "lens_correction_comp_spv.h"
#include "lens_match.h"

#include "app/app.h"
#include "photo/metadata.h"
#include "ui/toast.h"

#include "cimgui.h"

#include <lensfun/lensfun.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Lens Correction — geometric distortion + vignetting using the
// Lensfun database.  The module looks up camera and lens by EXIF
// make/model strings, interpolates distortion and vignetting
// coefficients for the shot's focal length and aperture, then
// dispatches a compute shader that performs the backward-warp and
// intensity correction in one pass.
//
// Parameter layout
// ----------------
// Float params:
//   SLOT_FOCAL_MM     (0)  Focal length in mm (0 = unset, show hint only)
//   SLOT_APERTURE     (1)  f-number          (0 = unset, show hint only)
//   SLOT_DO_DISTORT   (2)  1.0 = apply distortion correction
//   SLOT_DO_VIGNETTE  (3)  1.0 = apply vignetting correction
//
// String params (AP_EDIT_STR_SLOTS == 2):
//   STR_CAMERA    (0)  "Make Model" override; empty → use EXIF
//   STR_LENS      (1)  "Lens model" override; empty → use EXIF

typedef struct {
    float dist_a;
    float dist_b;
    float dist_c;
    int   dist_model;

    float vig_k1;
    float vig_k2;
    float vig_k3;
    int   do_vignetting;

    float center_x;
    float center_y;
    float _pad0;
    float _pad1;
} lens_push_t;

enum {
    SLOT_FOCAL_MM    = 0,
    SLOT_APERTURE    = 1,
    SLOT_DO_DISTORT  = 2,
    SLOT_DO_VIGNETTE = 3,
    SLOT_COUNT       = 4,
};

enum {
    STR_CAMERA = 0,
    STR_LENS   = 1,
};

// dist_model values — must match lens_correction.comp
enum {
    DIST_MODEL_NONE   = 0,
    DIST_MODEL_POLY3  = 1,
    DIST_MODEL_POLY5  = 2,
    DIST_MODEL_PTLENS = 3,
};

static const float       lens_defaults[] = {
    /* focal_mm */    0.0f,
    /* aperture */    0.0f,
    /* do_distort */  1.0f,
    /* do_vignette */ 1.0f,
};
static const char *const lens_param_names[] = {
    "focal_mm", "aperture", "do_distortion", "do_vignetting",
};
static const char *const lens_str_names[] = {
    "camera_override", "lens_override",
};

// Parse a formatted focal-length string like "24mm" or "24" into mm.
// Returns 0.0 if the string is empty or unparseable.
static float parse_focal(const char *s)
{
    if (!s || !s[0]) return 0.0f;
    float v = 0.0f;
    if (sscanf(s, "%f", &v) != 1) return 0.0f;
    return v;
}

// Parse a formatted aperture string like "f/2.8" or "2.8".
// Returns 0.0 when unset/unparseable.
static float parse_aperture(const char *s)
{
    if (!s || !s[0]) return 0.0f;
    float v = 0.0f;
    if (sscanf(s, "f/%f", &v) == 1) return v;
    if (sscanf(s, "%f", &v) == 1) return v;
    return 0.0f;
}

// Look up camera and lens in the Lensfun DB and fill the push constant
// with interpolated coefficients.  Returns 0 on success (correction
// found and packed), nonzero to skip the module this frame.
static int lookup_and_pack(const float *params,
                           const char (*str_params)[AP_EDIT_STR_LEN],
                           const ap_raw_metadata *meta,
                           lens_push_t *pc)
{
    if (!ap_lens_match_ensure_db()) return -1;

    const char *cam_str  = str_params ? str_params[STR_CAMERA] : "";
    const char *lens_str = str_params ? str_params[STR_LENS]   : "";

    float shot_focal    = params ? params[SLOT_FOCAL_MM] : 0.0f;
    float shot_aperture = params ? params[SLOT_APERTURE] : 0.0f;

    const ap_lens_match_hints hints = {
        .shot_focal_mm     = shot_focal,
        .shot_aperture     = shot_aperture,
        .lens_min_focal    = meta ? meta->lens_min_focal    : 0.0f,
        .lens_max_focal    = meta ? meta->lens_max_focal    : 0.0f,
        .lens_min_aperture = meta ? meta->lens_min_aperture : 0.0f,
    };

    const ap_lens_match *match = ap_lens_match_resolve(cam_str, lens_str,
                                                       &hints);
    const lfLens        *lens  = match->lens;

    float focal    = shot_focal;
    float aperture = shot_aperture;
    if (focal <= 0.0f) focal = 50.0f;    // fallback
    if (aperture <= 0.0f) aperture = 8.0f; // safe fallback

    // Center shift defaults to zero when there's no lens data.
    pc->center_x = lens ? lens->CenterX : 0.0f;
    pc->center_y = lens ? lens->CenterY : 0.0f;
    pc->_pad0    = 0.0f;
    pc->_pad1    = 0.0f;

    // Distortion
    pc->dist_model = DIST_MODEL_NONE;
    pc->dist_a = pc->dist_b = pc->dist_c = 0.0f;

    int do_distort = (params && params[SLOT_DO_DISTORT] != 0.0f);
    if (do_distort && lens) {
        lfLensCalibDistortion dist = {0};
        if (lf_lens_interpolate_distortion(lens, focal, &dist)) {
            switch (dist.Model) {
            case LF_DIST_MODEL_POLY3:
                pc->dist_model = DIST_MODEL_POLY3;
                pc->dist_c = dist.Terms[0];  // k1
                break;
            case LF_DIST_MODEL_POLY5:
                pc->dist_model = DIST_MODEL_POLY5;
                pc->dist_c = dist.Terms[0];  // k1
                pc->dist_b = dist.Terms[1];  // k2
                break;
            case LF_DIST_MODEL_PTLENS:
                pc->dist_model = DIST_MODEL_PTLENS;
                pc->dist_a = dist.Terms[0];  // a
                pc->dist_b = dist.Terms[1];  // b
                pc->dist_c = dist.Terms[2];  // c
                break;
            default:
                break;
            }
        }
    }

    // Vignetting
    pc->vig_k1 = pc->vig_k2 = pc->vig_k3 = 0.0f;
    pc->do_vignetting = 0;

    int do_vig = (params && params[SLOT_DO_VIGNETTE] != 0.0f);
    if (do_vig && lens) {
        lfLensCalibVignetting vig = {0};
        float distance = 10.0f; // typical focus distance fallback
        if (lf_lens_interpolate_vignetting(lens, focal, aperture,
                                           distance, &vig)) {
            if (vig.Model == LF_VIGNETTING_MODEL_PA) {
                pc->vig_k1    = vig.Terms[0];
                pc->vig_k2    = vig.Terms[1];
                pc->vig_k3    = vig.Terms[2];
                pc->do_vignetting = 1;
            }
        }
    }

    // Skip the stage if nothing to do and both corrections are disabled.
    if (pc->dist_model == DIST_MODEL_NONE && !pc->do_vignetting) return -1;
    return 0;
}

static int lens_pack_push(const ap_module *self,
                          const float *params,
                          const char (*str_params)[AP_EDIT_STR_LEN],
                          const ap_raw_metadata *meta,
                          void *push_out)
{
    (void)self;
    lens_push_t *pc = push_out;
    memset(pc, 0, sizeof(*pc));
    return lookup_and_pack(params, str_params, meta, pc);
}

// Build the camera identification string from EXIF fields.
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

// Build the lens identification string from EXIF fields.
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

// Collect the EXIF-derived narrowing hints from `fm` and the focused-
// edit's current `params`. Mirrors the pack_push view so render-side
// resolve calls hit the same cache entry.
static ap_lens_match_hints render_hints(const ap_photo_metadata *fm,
                                        const float *params)
{
    ap_lens_match_hints h = {0};
    if (params) {
        h.shot_focal_mm = params[SLOT_FOCAL_MM];
        h.shot_aperture = params[SLOT_APERTURE];
    }
    if (!fm) return h;
    h.lens_min_focal    = parse_focal(
        ap_photo_metadata_get(fm, AP_META_LENS_MIN_FOCAL));
    h.lens_max_focal    = parse_focal(
        ap_photo_metadata_get(fm, AP_META_LENS_MAX_FOCAL));
    h.lens_min_aperture = parse_aperture(
        ap_photo_metadata_get(fm, AP_META_LENS_MIN_APERTURE));
    return h;
}

static void lens_render(const ap_module *self, float *params,
                        const ap_module_render_ctx *ctx)
{
    if (!params) return;

    const ap_photo_metadata *fm = ctx->file_meta;

    if (!ap_lens_match_ensure_db()) {
        igTextColored((ImVec4_c){ 1.0f, 0.4f, 0.4f, 1.0f },
                      "Lensfun database not found");
    }

    // Camera string — auto-fill from EXIF on first use (empty slot),
    // then editable by the user if EXIF matching fails.
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

    // Lens string — same pattern.
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

    // Lensfun match status. Re-uses the cache populated by pack_push
    // (and refreshed in-place when inputs change), so the panel pays
    // at most one Lensfun query per (camera, lens, hints) tuple.
    ap_lens_match_hints hints = render_hints(fm, params);
    const ap_lens_match *match = ap_lens_match_resolve(ctx->str_params[STR_CAMERA],
                                                       ctx->str_params[STR_LENS],
                                                       &hints);

    // Candidate set keyed on the *EXIF* lens string, not the current
    // (possibly-overridden) STR_LENS. The chooser always offers the
    // alternatives the original EXIF string yields, so an override can
    // be undone or swapped without round-tripping through "Clear Lens".
    // Falls back to STR_LENS when no EXIF lens model is available.
    const char *baseline_lens =
        (exif_lens[0]) ? exif_lens : ctx->str_params[STR_LENS];
    const ap_lens_match *baseline =
        ap_lens_match_resolve(ctx->str_params[STR_CAMERA], baseline_lens,
                              &hints);

    bool is_override = (exif_lens[0] &&
                        strcmp(ctx->str_params[STR_LENS], exif_lens) != 0);
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
        igTextColored(ok_col, "\xe2\x9c\x93 Lens: %s (score %d)",
                      match->lens_name, match->lens_score);
        if (is_override) {
            igTextColored(warn_col,
                          "\xe2\x86\xb3 overridden \xe2\x80\x94 EXIF: %s",
                          exif_lens);
        } else if (baseline->candidate_count > 1) {
            igTextColored(warn_col,
                          "\xe2\x86\xb3 auto-picked from %d candidates",
                          baseline->candidate_count);
        }
    } else if (match->lens_rejected) {
        if (match->lens_pruned_by_hints) {
            igTextColored(bad_col,
                          "\xe2\x9a\xa0 Lens metadata mismatch (best: %s, "
                          "focal/aperture outside the lens's declared range) "
                          "\xe2\x80\x94 stage skipped",
                          match->lens_name);
        } else {
            igTextColored(bad_col,
                          "\xe2\x9a\xa0 Lens match too weak (best: %s, score %d/%d) "
                          "\xe2\x80\x94 stage skipped",
                          match->lens_name, match->lens_score,
                          AP_LENS_MATCH_MIN_SCORE);
        }
    } else if (ctx->str_params[STR_LENS][0]) {
        igTextColored(bad_col,
                      "\xe2\x9a\xa0 No lens match \xe2\x80\x94 stage skipped");
    }

    // Override chooser: surfaced whenever the EXIF-string lookup yields
    // more than one candidate. The combo's selection reflects which
    // candidate the *current* STR_LENS resolves to; when none match
    // (typically a manually typed override) the preview shows the
    // typed value tagged as custom so the displayed state stays honest.
    if (baseline->candidate_count > 1) {
        int  selected = -1;
        for (int i = 0; i < baseline->candidate_count; i++) {
            if (strcmp(baseline->candidates[i].name,
                       ctx->str_params[STR_LENS]) == 0) {
                selected = i;
                break;
            }
        }
        char custom_preview[AP_EDIT_STR_LEN + 16];
        const char *preview;
        if (selected >= 0) {
            preview = baseline->candidates[selected].name;
        } else {
            snprintf(custom_preview, sizeof(custom_preview),
                     "(custom: %s)", ctx->str_params[STR_LENS]);
            preview = custom_preview;
        }
        if (igBeginCombo("Choose lens", preview, 0)) {
            for (int i = 0; i < baseline->candidate_count; i++) {
                char label[AP_META_VALUE_LEN * 2 + 32];
                snprintf(label, sizeof(label), "%s (score %d)",
                         baseline->candidates[i].name,
                         baseline->candidates[i].score);
                bool is_sel = (i == selected);
                if (igSelectable_Bool(label, is_sel, 0,
                                      (ImVec2_c){0.0f, 0.0f})) {
                    snprintf(ctx->str_params[STR_LENS], AP_EDIT_STR_LEN,
                             "%s", baseline->candidates[i].name);
                    *ctx->request_rebuild    = true;
                    *ctx->snapshot_requested = true;
                }
                if (is_sel) igSetItemDefaultFocus();
            }
            igEndCombo();
        }

        // Bulk-apply: write the current STR_LENS as an override onto
        // every selected photo whose EXIF lens model exactly equals
        // this photo's EXIF lens model.
        igSameLine(0.0f, 4.0f);
        if (igSmallButton("Apply to selection")) {
            int applied = 0, skipped = 0;
            if (ap_app_apply_lens_override_to_selection(
                    ctx->app, exif_lens, ctx->str_params[STR_LENS],
                    &applied, &skipped) == 0) {
                ap_toast_push(AP_TOAST_INFO,
                              "Lens override: applied to %d photo%s; "
                              "skipped %d (different lens or no module).",
                              applied, applied == 1 ? "" : "s", skipped);
            } else {
                ap_toast_push(AP_TOAST_ERROR,
                              "Lens override: no library / selection.");
            }
        }
    }

    igSeparator();

    // Focal length — auto-populate from EXIF when slot is zero.
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
    if (igIsItemActivated() && ctx->snapshot_requested) {
        *ctx->snapshot_requested = true;
    }
    if (exif_focal_s && exif_focal_s[0]) {
        igSameLine(0.0f, 4.0f);
        igTextDisabled("EXIF: %s", exif_focal_s);
    }

    // Aperture — auto-populate from EXIF when slot is zero.
    const char *exif_ap_s = fm ? ap_photo_metadata_get(fm, AP_META_APERTURE) : "";
    if (params[SLOT_APERTURE] <= 0.0f && exif_ap_s && exif_ap_s[0]) {
        float parsed = parse_aperture(exif_ap_s);
        if (parsed > 0.0f) {
            params[SLOT_APERTURE] = parsed;
            *ctx->request_rebuild = true;
        }
    }
    float aperture = params[SLOT_APERTURE];
    if (igInputFloat("Aperture (f/)", &aperture, 0.1f, 1.0f, "%.1f", 0)) {
        if (aperture < 0.7f) aperture = 0.7f;
        params[SLOT_APERTURE] = aperture;
    }
    if (igIsItemActivated() && ctx->snapshot_requested) {
        *ctx->snapshot_requested = true;
    }
    if (exif_ap_s && exif_ap_s[0]) {
        igSameLine(0.0f, 4.0f);
        igTextDisabled("EXIF: %s", exif_ap_s);
    }

    igSeparator();

    bool do_distort = (params[SLOT_DO_DISTORT] != 0.0f);
    if (igCheckbox("Distortion correction", &do_distort)) {
        params[SLOT_DO_DISTORT]  = do_distort ? 1.0f : 0.0f;
        *ctx->request_rebuild    = true;
        *ctx->snapshot_requested = true;
    }

    bool do_vig = (params[SLOT_DO_VIGNETTE] != 0.0f);
    if (igCheckbox("Vignetting correction", &do_vig)) {
        params[SLOT_DO_VIGNETTE] = do_vig ? 1.0f : 0.0f;
        *ctx->request_rebuild    = true;
        *ctx->snapshot_requested = true;
    }

    igTextDisabled("Clear Camera/Lens to re-detect from EXIF.");

    (void)self;
}

const ap_module module_lens_correction = {
    .name             = "lens_correction",
    .display_name     = "Lens Correction",
    .category         = AP_MODULE_GEOMETRIC,
    .user_visible     = true,
    .spv_data         = lens_correction_comp_spv,
    .spv_size         = lens_correction_comp_spv_size,
    .push_size        = sizeof(lens_push_t),
    .pack_push        = lens_pack_push,
    .params_count     = SLOT_COUNT,
    .params_default   = lens_defaults,
    .params_names     = lens_param_names,
    .render_params    = lens_render,
    .str_params_count = 2,
    .str_params_names = lens_str_names,
};
