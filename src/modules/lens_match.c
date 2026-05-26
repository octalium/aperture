#include "lens_match.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

// Global lensfun database — loaded once, shared across all modules.
// The DB is thread-safe for reads; we only ever read after lf_db_load.
static lfDatabase *g_lf_db;
static bool        g_lf_db_loaded;

// Round-robin LRU. Sized to cover both lens_correction and
// chromatic_aberration each resolving a live STR_LENS plus an EXIF
// baseline in the same frame without eviction churn.
#define LENS_MATCH_CACHE_SLOTS 4
static ap_lens_match g_match_cache[LENS_MATCH_CACHE_SLOTS];
static int           g_match_cache_next;

bool ap_lens_match_ensure_db(void)
{
    if (g_lf_db_loaded) return g_lf_db != NULL;
    g_lf_db_loaded = true;
    g_lf_db = lf_db_new();
    if (!g_lf_db) return false;
    lfError err = lf_db_load(g_lf_db);
    if (err != LF_NO_ERROR) {
        lf_db_destroy(g_lf_db);
        g_lf_db = NULL;
        return false;
    }
    return true;
}

static void compose_name(const char *maker, const char *model,
                         char *out, size_t len)
{
    if (!maker) maker = "";
    if (!model) model = "";
    if (maker[0] && model[0]) {
        snprintf(out, len, "%s %s", maker, model);
    } else if (model[0]) {
        snprintf(out, len, "%s", model);
    } else {
        snprintf(out, len, "%s", maker);
    }
}

// Quantize a float hint so cache equality survives minor drift between
// callers that re-derive the same value from the same upstream.
static int quant(float v)
{
    if (v <= 0.0f) return 0;
    return (int)lrintf(v * 100.0f);
}

static bool hints_equal(const ap_lens_match_hints *a,
                        const ap_lens_match_hints *b)
{
    return quant(a->shot_focal_mm)     == quant(b->shot_focal_mm)
        && quant(a->shot_aperture)     == quant(b->shot_aperture)
        && quant(a->lens_min_focal)    == quant(b->lens_min_focal)
        && quant(a->lens_max_focal)    == quant(b->lens_max_focal)
        && quant(a->lens_min_aperture) == quant(b->lens_min_aperture);
}

// True when `[min_v, max_v]` brackets `shot` within ±1%. Zero on either
// side of the bracket is neutral (no opinion); same for a zero shot.
static bool brackets(float min_v, float max_v, float shot)
{
    if (shot <= 0.0f || min_v <= 0.0f || max_v <= 0.0f) return true;
    return shot >= min_v * 0.99f && shot <= max_v * 1.01f;
}

// ±1% ratio agreement, neutral when either side is zero. Mirrors
// Lensfun's own _lf_compare_num so this narrowing aligns with the
// MatchScore check the DB would apply if we could pass the range there.
static bool ratio_ok(float pat, float cand)
{
    if (pat <= 0.0f || cand <= 0.0f) return true;
    float r = pat / cand;
    return r > 0.99f && r < 1.01f;
}

// True when `cand` survives narrowing by `h`. Each axis is independent
// and neutral when zero on either side.
static bool candidate_passes_hints(const lfLens *cand,
                                   const ap_lens_match_hints *h)
{
    if (!h) return true;
    if (!brackets(cand->MinFocal, cand->MaxFocal, h->shot_focal_mm))
        return false;
    // Shot aperture must be reachable — the candidate has to open at
    // least as wide as the shot (smaller f-number = wider). A 1% margin
    // keeps rounding from rejecting on-the-line shots.
    if (h->shot_aperture > 0.0f && cand->MinAperture > 0.0f
        && h->shot_aperture < cand->MinAperture * 0.99f) {
        return false;
    }
    if (!ratio_ok(h->lens_min_focal,    cand->MinFocal))    return false;
    if (!ratio_ok(h->lens_max_focal,    cand->MaxFocal))    return false;
    if (!ratio_ok(h->lens_min_aperture, cand->MinAperture)) return false;
    return true;
}

const ap_lens_match *ap_lens_match_resolve(const char *cam_in,
                                           const char *lens_in,
                                           const ap_lens_match_hints *hints)
{
    if (!cam_in)  cam_in  = "";
    if (!lens_in) lens_in = "";

    ap_lens_match_hints zero_hints = {0};
    if (!hints) hints = &zero_hints;

    for (int i = 0; i < LENS_MATCH_CACHE_SLOTS; i++) {
        const ap_lens_match *e = &g_match_cache[i];
        if (e->valid &&
            strncmp(e->cam_in,  cam_in,  AP_EDIT_STR_LEN) == 0 &&
            strncmp(e->lens_in, lens_in, AP_EDIT_STR_LEN) == 0 &&
            hints_equal(&e->hints_in, hints)) {
            return e;
        }
    }

    ap_lens_match *m = &g_match_cache[g_match_cache_next];
    g_match_cache_next = (g_match_cache_next + 1) % LENS_MATCH_CACHE_SLOTS;
    memset(m, 0, sizeof *m);
    m->valid    = true;
    m->hints_in = *hints;
    strncpy(m->cam_in,  cam_in,  AP_EDIT_STR_LEN - 1);
    strncpy(m->lens_in, lens_in, AP_EDIT_STR_LEN - 1);

    if (!ap_lens_match_ensure_db()) return m;

    if (cam_in[0]) {
        // cameras matched loosely — EXIF make/model word order varies
        // between vendors but the body identifier is usually unambiguous.
        const lfCamera **cams = lf_db_find_cameras_ext(g_lf_db, NULL, cam_in,
                                                       LF_SEARCH_LOOSE);
        if (cams && cams[0]) {
            m->cam       = cams[0];
            m->cam_score = cams[0]->Score;
            compose_name(cams[0]->Maker, cams[0]->Model,
                         m->cam_name, sizeof m->cam_name);
        }
        lf_free(cams);
    }

    if (lens_in[0]) {
        const lfLens **lenses = lf_db_find_lenses_hd(g_lf_db, m->cam,
                                                     NULL, lens_in, 0);
        if (lenses && lenses[0]) {
            // Record the pre-narrowing best for diagnostic UI text,
            // even when narrowing later rejects every candidate.
            m->lens_score = lenses[0]->Score;
            compose_name(lenses[0]->Maker, lenses[0]->Model,
                         m->lens_name, sizeof m->lens_name);

            int hint_pruned = 0;
            for (int i = 0;
                 lenses[i] &&
                 m->candidate_count < AP_LENS_MATCH_MAX_CANDIDATES &&
                 lenses[i]->Score >= AP_LENS_MATCH_MIN_SCORE;
                 i++) {
                if (!candidate_passes_hints(lenses[i], hints)) {
                    hint_pruned++;
                    continue;
                }
                m->candidates[m->candidate_count].lens  = lenses[i];
                m->candidates[m->candidate_count].score = lenses[i]->Score;
                compose_name(lenses[i]->Maker, lenses[i]->Model,
                             m->candidates[m->candidate_count].name,
                             sizeof m->candidates[m->candidate_count].name);
                m->candidate_count++;
            }
            if (m->candidate_count > 0) {
                m->lens       = m->candidates[0].lens;
                m->lens_score = m->candidates[0].score;
                compose_name(m->candidates[0].lens->Maker,
                             m->candidates[0].lens->Model,
                             m->lens_name, sizeof m->lens_name);
            } else {
                // Either the best candidate's score was below the
                // threshold, or every qualifying candidate was pruned
                // by hint narrowing. `lens_pruned_by_hints` distinguishes
                // the two so consumer UI can explain the actual cause.
                m->lens_rejected       = true;
                m->lens_pruned_by_hints = hint_pruned > 0;
            }
        }
        lf_free(lenses);
    }
    return m;
}
