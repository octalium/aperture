#ifndef APERTURE_LENS_MATCH_H
#define APERTURE_LENS_MATCH_H

#include "edit/stack.h"
#include "photo/metadata.h"

#include <lensfun/lensfun.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Minimum Lensfun confidence score (0..100) we require before applying
// lens-derived corrections. Weak matches that survive Lensfun's fuzzy
// scoring are rejected here — e.g. "AF-S 50mm f/1.8" matching every
// "50mm" prime when the user's exact lens isn't in the database.
#define AP_LENS_MATCH_MIN_SCORE 50

// Maximum candidate lenses surfaced to the chooser. Strict matches for
// an ambiguous string rarely return more than a handful that clear the
// score threshold; the cap keeps the chooser usable and bounds the
// per-result memory footprint.
#define AP_LENS_MATCH_MAX_CANDIDATES 16

// One candidate lens from the Lensfun query. `lens` is borrowed from
// the global database — valid for the lifetime of that database.
typedef struct {
    const lfLens *lens;
    int           score;
    char          name[AP_META_VALUE_LEN * 2]; // "Maker Model"
} ap_lens_candidate;

// Result of a (camera_str, lens_str) lookup against the Lensfun
// database. Cached across frames so multiple modules can read match
// state every frame without re-running the queries. `cam` / `lens` are
// borrowed pointers owned by the global database.
typedef struct {
    char            cam_in[AP_EDIT_STR_LEN];
    char            lens_in[AP_EDIT_STR_LEN];
    bool            valid;
    const lfCamera *cam;
    const lfLens   *lens;          // NULL when no acceptable match
    int             cam_score;
    int             lens_score;    // best candidate score even if rejected
    char            cam_name[AP_META_VALUE_LEN * 2];
    char            lens_name[AP_META_VALUE_LEN * 2];   // best candidate name
    bool            lens_rejected; // a candidate existed but scored too low

    ap_lens_candidate candidates[AP_LENS_MATCH_MAX_CANDIDATES];
    int               candidate_count;
} ap_lens_match;

// Ensure the global Lensfun database is loaded. Idempotent — repeated
// calls are no-ops. Returns true when the database is available after
// the call, false otherwise. The database is owned process-wide and
// never destroyed; load failures latch for the session.
bool ap_lens_match_ensure_db(void);

// Strict cached lookup of (camera_str, lens_str). Returns a pointer to
// a module-shared cache entry; never NULL. Result fields are zeroed
// when inputs are empty or the database is unavailable. The lens query
// is strict (no LF_SEARCH_LOOSE); weak hits are filtered by
// AP_LENS_MATCH_MIN_SCORE. When more than one candidate clears the
// threshold, `lens` points at the top-scoring one and
// `candidates[0..candidate_count)` lists them all so a chooser UI can
// offer manual override.
const ap_lens_match *ap_lens_match_resolve(const char *cam_str,
                                           const char *lens_str);

#ifdef __cplusplus
}
#endif

#endif
