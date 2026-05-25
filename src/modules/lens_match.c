#include "lens_match.h"

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

const ap_lens_match *ap_lens_match_resolve(const char *cam_in,
                                           const char *lens_in)
{
    if (!cam_in)  cam_in  = "";
    if (!lens_in) lens_in = "";

    for (int i = 0; i < LENS_MATCH_CACHE_SLOTS; i++) {
        const ap_lens_match *e = &g_match_cache[i];
        if (e->valid &&
            strncmp(e->cam_in,  cam_in,  AP_EDIT_STR_LEN) == 0 &&
            strncmp(e->lens_in, lens_in, AP_EDIT_STR_LEN) == 0) {
            return e;
        }
    }

    ap_lens_match *m = &g_match_cache[g_match_cache_next];
    g_match_cache_next = (g_match_cache_next + 1) % LENS_MATCH_CACHE_SLOTS;
    memset(m, 0, sizeof *m);
    m->valid = true;
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
            m->lens_score = lenses[0]->Score;
            compose_name(lenses[0]->Maker, lenses[0]->Model,
                         m->lens_name, sizeof m->lens_name);
            if (lenses[0]->Score >= AP_LENS_MATCH_MIN_SCORE) {
                m->lens = lenses[0];
                for (int i = 0;
                     lenses[i] &&
                     i < AP_LENS_MATCH_MAX_CANDIDATES &&
                     lenses[i]->Score >= AP_LENS_MATCH_MIN_SCORE;
                     i++) {
                    m->candidates[m->candidate_count].lens  = lenses[i];
                    m->candidates[m->candidate_count].score = lenses[i]->Score;
                    compose_name(lenses[i]->Maker, lenses[i]->Model,
                                 m->candidates[m->candidate_count].name,
                                 sizeof m->candidates[m->candidate_count].name);
                    m->candidate_count++;
                }
            } else {
                m->lens_rejected = true;
            }
        }
        lf_free(lenses);
    }
    return m;
}
