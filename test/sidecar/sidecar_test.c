// Sidecar IO round-trip. Write a populated `.aperture` sidecar to a
// tmpdir, parse it back, assert every field survives. Catches any
// metadata override / culling / groups / keywords / edit-param that
// the writer drops or the reader misreads.

#define _GNU_SOURCE

#include "aptest.h"
#include "aptest_tmpdir.h"

#include "edit/stack.h"
#include "photo/culling.h"
#include "photo/groups.h"
#include "photo/keywords.h"
#include "photo/metadata.h"
#include "sidecar/sidecar.h"

#include <stdio.h>
#include <string.h>

// Build a non-trivial sidecar fixture: every kind of overrideable
// field is populated, so any silent drop on the round-trip surfaces
// as a mismatch.
static void fill_inputs(ap_edit_stack *stack,
                        ap_photo_metadata *meta,
                        bool meta_set[AP_META_FIELD_COUNT],
                        ap_photo_culling *culling,
                        ap_photo_groups *groups,
                        ap_photo_keywords *keywords)
{
    ap_edit_stack_init(stack);
    int idx = ap_edit_stack_add(stack, "color_grade");
    AP_TEST_ASSERT(idx == 0, "color_grade add idx=%d", idx);
    ap_edit_entry *e = ap_edit_stack_at(stack, idx);
    e->params[0] = 0.10f;  // lift_r
    e->params[1] = 0.20f;  // lift_g
    e->params[2] = -0.05f; // lift_b
    e->params[3] = 1.10f;  // gamma_r
    e->params[4] = 1.05f;  // gamma_g
    e->params[5] = 0.95f;  // gamma_b
    e->params[6] = 1.00f;  // gain_r
    e->params[7] = 0.90f;  // gain_g
    e->params[8] = 1.20f;  // gain_b
    e->enabled = true;
    snprintf(e->display_name, sizeof(e->display_name), "warm grade");

    idx = ap_edit_stack_add(stack, "lens_correction");
    AP_TEST_ASSERT(idx == 1, "lens add idx=%d", idx);
    e = ap_edit_stack_at(stack, idx);
    e->params[0] = 35.0f;  // focal_mm
    e->params[1] = 1.8f;   // aperture
    e->params[2] = 1.0f;   // do_distortion
    e->params[3] = 0.0f;   // do_vignetting
    e->enabled = false;    // disabled — must round-trip
    snprintf(e->str_params[0], AP_EDIT_STR_LEN, "Canon EOS R5");
    snprintf(e->str_params[1], AP_EDIT_STR_LEN, "RF 35mm F1.8 IS Macro");

    ap_photo_metadata_clear(meta);
    for (int i = 0; i < AP_META_FIELD_COUNT; i++) meta_set[i] = false;
    ap_photo_metadata_set(meta, AP_META_ARTIST, "Test Author");
    meta_set[AP_META_ARTIST] = true;
    ap_photo_metadata_set(meta, AP_META_DESCRIPTION,
                          "quote-\"and\\-backslash test");
    meta_set[AP_META_DESCRIPTION] = true;
    ap_photo_metadata_set(meta, AP_META_LENS_MODEL, "");  // deliberate blank
    meta_set[AP_META_LENS_MODEL] = true;

    ap_photo_culling_clear(culling);
    culling->rating = 4;
    culling->flag   = AP_FLAG_PICK;
    culling->color  = AP_COLOR_GREEN;

    groups->count = 0;
    snprintf(groups->names[0], AP_GROUP_NAME_LEN, "portraits");
    snprintf(groups->names[1], AP_GROUP_NAME_LEN, "client-acme");
    groups->count = 2;

    ap_photo_keywords_clear(keywords);
    ap_photo_keywords_add(keywords, "outdoor");
    ap_photo_keywords_add(keywords, "studio|softbox");
    ap_photo_keywords_add(keywords, "people");
}

static void test_round_trip(void)
{
    char tmp[4096];
    aptest_tmpdir_make(tmp, sizeof(tmp));

    char raw_path[4200];
    snprintf(raw_path, sizeof(raw_path), "%s/photo.cr3", tmp);
    FILE *touch = fopen(raw_path, "wb");
    AP_TEST_ASSERT(touch != NULL, "create %s", raw_path);
    fclose(touch);

    ap_edit_stack     stack_in;
    ap_photo_metadata meta_in;
    bool              meta_set_in[AP_META_FIELD_COUNT];
    ap_photo_culling  culling_in;
    ap_photo_groups   groups_in;
    ap_photo_keywords keywords_in;
    fill_inputs(&stack_in, &meta_in, meta_set_in, &culling_in,
                &groups_in, &keywords_in);

    bool respect_orientation_in = false;
    int rc = ap_sidecar_save(raw_path, &stack_in, respect_orientation_in,
                             &meta_in, meta_set_in, &culling_in,
                             &groups_in, &keywords_in);
    AP_TEST_ASSERT(rc == 0, "ap_sidecar_save: rc=%d", rc);

    ap_edit_stack     stack_out;
    ap_photo_metadata meta_out;
    bool              meta_set_out[AP_META_FIELD_COUNT];
    ap_photo_culling  culling_out;
    ap_photo_groups   groups_out;
    ap_photo_keywords keywords_out;
    bool              respect_orientation_out = true;
    ap_edit_stack_init(&stack_out);

    rc = ap_sidecar_load(raw_path, &stack_out, &respect_orientation_out,
                         &meta_out, meta_set_out, &culling_out,
                         &groups_out, &keywords_out);
    AP_TEST_ASSERT(rc == 0, "ap_sidecar_load: rc=%d", rc);

    AP_TEST_ASSERT(respect_orientation_out == respect_orientation_in,
                   "orientation: in=%d out=%d",
                   respect_orientation_in, respect_orientation_out);

    AP_TEST_ASSERT(stack_out.count == stack_in.count,
                   "stack count: in=%d out=%d",
                   stack_in.count, stack_out.count);
    for (int i = 0; i < stack_in.count; i++) {
        const ap_edit_entry *a = &stack_in.entries[i];
        const ap_edit_entry *b = &stack_out.entries[i];
        AP_TEST_ASSERT(strcmp(a->module_name, b->module_name) == 0,
                       "entry %d module: in=%s out=%s",
                       i, a->module_name, b->module_name);
        AP_TEST_ASSERT(a->enabled == b->enabled,
                       "entry %d enabled: in=%d out=%d",
                       i, a->enabled, b->enabled);
        AP_TEST_ASSERT(strcmp(a->display_name, b->display_name) == 0,
                       "entry %d display: in='%s' out='%s'",
                       i, a->display_name, b->display_name);
        for (int s = 0; s < AP_EDIT_PARAMS_SLOTS; s++) {
            AP_TEST_ASSERT(a->params[s] == b->params[s],
                           "entry %d param[%d]: in=%g out=%g",
                           i, s, (double)a->params[s], (double)b->params[s]);
        }
        for (int s = 0; s < AP_EDIT_STR_SLOTS; s++) {
            AP_TEST_ASSERT(strcmp(a->str_params[s], b->str_params[s]) == 0,
                           "entry %d str_params[%d]: in='%s' out='%s'",
                           i, s, a->str_params[s], b->str_params[s]);
        }
    }

    for (int i = 0; i < AP_META_FIELD_COUNT; i++) {
        AP_TEST_ASSERT(meta_set_in[i] == meta_set_out[i],
                       "meta_set[%d]: in=%d out=%d",
                       i, meta_set_in[i], meta_set_out[i]);
        if (!meta_set_in[i]) continue;
        const char *a = ap_photo_metadata_get(&meta_in,  (ap_meta_field)i);
        const char *b = ap_photo_metadata_get(&meta_out, (ap_meta_field)i);
        AP_TEST_ASSERT(strcmp(a, b) == 0,
                       "meta[%d]: in='%s' out='%s'", i, a, b);
    }

    AP_TEST_ASSERT(culling_in.rating == culling_out.rating,
                   "rating: in=%d out=%d",
                   culling_in.rating, culling_out.rating);
    AP_TEST_ASSERT(culling_in.flag == culling_out.flag,
                   "flag: in=%d out=%d", culling_in.flag, culling_out.flag);
    AP_TEST_ASSERT(culling_in.color == culling_out.color,
                   "color: in=%d out=%d",
                   culling_in.color, culling_out.color);

    AP_TEST_ASSERT(groups_in.count == groups_out.count,
                   "groups count: in=%d out=%d",
                   groups_in.count, groups_out.count);
    for (int i = 0; i < groups_in.count; i++) {
        AP_TEST_ASSERT(strcmp(groups_in.names[i], groups_out.names[i]) == 0,
                       "groups[%d]: in='%s' out='%s'",
                       i, groups_in.names[i], groups_out.names[i]);
    }

    AP_TEST_ASSERT(keywords_in.count == keywords_out.count,
                   "keywords count: in=%d out=%d",
                   keywords_in.count, keywords_out.count);
    for (int i = 0; i < keywords_in.count; i++) {
        AP_TEST_ASSERT(strcmp(keywords_in.kw[i], keywords_out.kw[i]) == 0,
                       "keywords[%d]: in='%s' out='%s'",
                       i, keywords_in.kw[i], keywords_out.kw[i]);
    }

    aptest_tmpdir_rm(tmp);
}

// A sidecar with no overrides should write the bare skeleton and read
// back to all-defaults — exercises the "everything empty" path through
// the writer's conditional-section logic.
static void test_empty_round_trip(void)
{
    char tmp[4096];
    aptest_tmpdir_make(tmp, sizeof(tmp));
    char raw[4200];
    snprintf(raw, sizeof(raw), "%s/photo.cr3", tmp);
    FILE *touch = fopen(raw, "wb");
    AP_TEST_ASSERT(touch != NULL, "touch %s", raw);
    fclose(touch);

    ap_edit_stack stack;
    ap_edit_stack_init(&stack);
    ap_photo_metadata meta;
    ap_photo_metadata_clear(&meta);
    bool meta_set[AP_META_FIELD_COUNT] = {0};
    ap_photo_culling culling;
    ap_photo_culling_clear(&culling);
    ap_photo_groups groups; groups.count = 0;
    ap_photo_keywords keywords;
    ap_photo_keywords_clear(&keywords);

    int rc = ap_sidecar_save(raw, &stack, true, &meta, meta_set,
                             &culling, &groups, &keywords);
    AP_TEST_ASSERT(rc == 0, "save empty: rc=%d", rc);

    ap_edit_stack stack2;
    ap_edit_stack_init(&stack2);
    bool ro = false;
    ap_photo_metadata meta2;
    bool meta_set2[AP_META_FIELD_COUNT];
    ap_photo_culling culling2;
    ap_photo_groups groups2;
    ap_photo_keywords keywords2;
    rc = ap_sidecar_load(raw, &stack2, &ro, &meta2, meta_set2, &culling2,
                         &groups2, &keywords2);
    AP_TEST_ASSERT(rc == 0, "load empty: rc=%d", rc);
    AP_TEST_ASSERT(ro == true, "orientation default should round-trip true");
    AP_TEST_ASSERT(stack2.count == 0, "empty stack count=%d", stack2.count);
    AP_TEST_ASSERT(ap_photo_culling_is_empty(&culling2),
                   "culling should be empty after empty round-trip");
    AP_TEST_ASSERT(groups2.count == 0,
                   "groups count=%d", groups2.count);
    AP_TEST_ASSERT(ap_photo_keywords_is_empty(&keywords2),
                   "keywords should be empty after empty round-trip");
    for (int i = 0; i < AP_META_FIELD_COUNT; i++) {
        AP_TEST_ASSERT(!meta_set2[i],
                       "meta_set[%d] should be false after empty round-trip",
                       i);
    }

    aptest_tmpdir_rm(tmp);
}

// load_culling / load_groups must see the values a full save wrote —
// they're the fast paths the library uses to build its caches and have
// historically been a place where the lightweight parsers and the full
// parser fell out of sync.
static void test_load_culling_groups(void)
{
    char tmp[4096];
    aptest_tmpdir_make(tmp, sizeof(tmp));
    char raw[4200];
    snprintf(raw, sizeof(raw), "%s/photo.cr3", tmp);
    FILE *touch = fopen(raw, "wb");
    AP_TEST_ASSERT(touch != NULL, "touch %s", raw);
    fclose(touch);

    ap_edit_stack stack; ap_edit_stack_init(&stack);
    ap_photo_metadata meta; ap_photo_metadata_clear(&meta);
    bool meta_set[AP_META_FIELD_COUNT] = {0};
    ap_photo_culling c; ap_photo_culling_clear(&c);
    c.rating = 3;
    c.flag   = AP_FLAG_REJECT;
    c.color  = AP_COLOR_RED;
    ap_photo_groups g; g.count = 1;
    snprintf(g.names[0], AP_GROUP_NAME_LEN, "rejects");
    ap_photo_keywords k; ap_photo_keywords_clear(&k);

    int rc = ap_sidecar_save(raw, &stack, true, &meta, meta_set, &c, &g, &k);
    AP_TEST_ASSERT(rc == 0, "save: rc=%d", rc);

    ap_photo_culling cc; ap_photo_culling_clear(&cc);
    rc = ap_sidecar_load_culling(raw, &cc);
    AP_TEST_ASSERT(rc == 0, "load_culling: rc=%d", rc);
    AP_TEST_ASSERT(cc.rating == 3 && cc.flag == AP_FLAG_REJECT &&
                       cc.color == AP_COLOR_RED,
                   "culling fast-path mismatch r=%d f=%d c=%d",
                   cc.rating, (int)cc.flag, (int)cc.color);

    ap_photo_groups gg; gg.count = 0;
    rc = ap_sidecar_load_groups(raw, &gg);
    AP_TEST_ASSERT(rc == 0, "load_groups: rc=%d", rc);
    AP_TEST_ASSERT(gg.count == 1, "groups count=%d", gg.count);
    AP_TEST_ASSERT(strcmp(gg.names[0], "rejects") == 0,
                   "groups[0]='%s'", gg.names[0]);

    AP_TEST_ASSERT(ap_sidecar_remove(raw) == 0, "sidecar_remove");
    aptest_tmpdir_rm(tmp);
}

int main(void)
{
    test_round_trip();
    test_empty_round_trip();
    test_load_culling_groups();
    printf("sidecar/sidecar: OK\n");
    return 0;
}
