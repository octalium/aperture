// Edit-stack pack/unpack round-trip for the param-heavy modules.
// Asserts every per-instance float and string param survives a TOML
// write -> read cycle for color_grade, lens_correction, and
// chromatic_aberration. Uses the fixture module registry — no shaders,
// no GPU — so the test isolates the param-name <-> slot mapping.

#define _GNU_SOURCE

#include "aptest.h"

#include "core/memstream.h"
#include "edit/stack.h"
#include "edit/stack_toml.h"
#include "modules/module.h"

#include <toml.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void assert_entries_equal(const ap_edit_entry *a,
                                 const ap_edit_entry *b)
{
    AP_TEST_ASSERT(strcmp(a->module_name, b->module_name) == 0,
                   "module mismatch: a=%s b=%s",
                   a->module_name, b->module_name);
    AP_TEST_ASSERT(a->enabled == b->enabled,
                   "enabled mismatch on %s: a=%d b=%d",
                   a->module_name, a->enabled, b->enabled);
    AP_TEST_ASSERT(strcmp(a->display_name, b->display_name) == 0,
                   "display_name mismatch on %s: a='%s' b='%s'",
                   a->module_name, a->display_name, b->display_name);

    const ap_module *m = ap_module_find(a->module_name);
    AP_TEST_ASSERT(m != NULL, "module '%s' not in registry", a->module_name);
    for (int s = 0; s < m->params_count; s++) {
        AP_TEST_ASSERT(a->params[s] == b->params[s],
                       "%s.%s mismatch: a=%g b=%g",
                       a->module_name, m->params_names[s],
                       (double)a->params[s], (double)b->params[s]);
    }
    for (int s = 0; s < m->str_params_count; s++) {
        AP_TEST_ASSERT(strcmp(a->str_params[s], b->str_params[s]) == 0,
                       "%s.%s mismatch: a='%s' b='%s'",
                       a->module_name, m->str_params_names[s],
                       a->str_params[s], b->str_params[s]);
    }
}

// pack `in` to TOML, parse it back into `out`. asserts on writer / parser
// failure. caller owns nothing on the heap; the parsed doc is freed here.
static void round_trip_stack(const ap_edit_stack *in, ap_edit_stack *out)
{
    ap_memstream *ms = ap_memstream_open();
    AP_TEST_ASSERT(ms != NULL, "open_memstream");
    AP_TEST_ASSERT(ap_edit_stack_write_toml(in, ap_memstream_file(ms)) == 0,
                   "write_toml failed");
    char  *buf = NULL;
    size_t len = 0;
    AP_TEST_ASSERT(ap_memstream_close(ms, &buf, &len) == 0, "memstream close");

    char errbuf[256] = {0};
    toml_table_t *root = toml_parse(buf, errbuf, sizeof(errbuf));
    AP_TEST_ASSERT(root != NULL, "toml_parse: %s\n--- buffer (%zu B) ---\n%s",
                   errbuf, len, buf ? buf : "(null)");
    toml_array_t *arr = toml_array_in(root, "edit");
    AP_TEST_ASSERT(arr != NULL, "no [[edit]] array in serialized output");
    AP_TEST_ASSERT(ap_edit_stack_read_toml_array(arr, out) == 0,
                   "read_toml_array failed");

    toml_free(root);
    free(buf);
}

static void test_color_grade(void)
{
    ap_edit_stack in;
    ap_edit_stack_init(&in);
    int idx = ap_edit_stack_add(&in, "color_grade");
    AP_TEST_ASSERT(idx == 0, "add color_grade");
    ap_edit_entry *e = ap_edit_stack_at(&in, idx);
    e->enabled = false;
    snprintf(e->display_name, sizeof(e->display_name), "moody");
    for (int s = 0; s < 9; s++) {
        e->params[s] = -0.5f + 0.125f * (float)s;
    }

    ap_edit_stack out;
    ap_edit_stack_init(&out);
    round_trip_stack(&in, &out);
    AP_TEST_ASSERT(out.count == 1, "out.count=%d", out.count);
    assert_entries_equal(&in.entries[0], &out.entries[0]);
}

static void test_lens_correction(void)
{
    ap_edit_stack in;
    ap_edit_stack_init(&in);
    int idx = ap_edit_stack_add(&in, "lens_correction");
    AP_TEST_ASSERT(idx == 0, "add lens_correction");
    ap_edit_entry *e = ap_edit_stack_at(&in, idx);
    e->enabled = true;
    e->params[0] = 24.0f;  // focal_mm
    e->params[1] = 1.4f;   // aperture
    e->params[2] = 1.0f;   // do_distortion
    e->params[3] = 0.0f;   // do_vignetting
    snprintf(e->str_params[0], AP_EDIT_STR_LEN, "NIKON CORPORATION NIKON Z 9");
    snprintf(e->str_params[1], AP_EDIT_STR_LEN, "NIKKOR Z 24-70mm f/2.8 S");

    ap_edit_stack out;
    ap_edit_stack_init(&out);
    round_trip_stack(&in, &out);
    AP_TEST_ASSERT(out.count == 1, "out.count=%d", out.count);
    assert_entries_equal(&in.entries[0], &out.entries[0]);
}

static void test_chromatic_aberration(void)
{
    ap_edit_stack in;
    ap_edit_stack_init(&in);
    int idx = ap_edit_stack_add(&in, "chromatic_aberration");
    AP_TEST_ASSERT(idx == 0, "add chromatic_aberration");
    ap_edit_entry *e = ap_edit_stack_at(&in, idx);
    e->enabled = true;
    e->params[0] = 1.0f;    // mode
    e->params[1] = 70.0f;   // focal_mm
    e->params[2] = 1.0007f; // r_scale
    e->params[3] = 0.9992f; // b_scale
    e->params[4] = 0.013f;  // r_offset
    e->params[5] = -0.020f; // b_offset
    snprintf(e->str_params[0], AP_EDIT_STR_LEN, "Canon EOS R5");
    snprintf(e->str_params[1], AP_EDIT_STR_LEN, "RF 70-200mm F2.8 L IS USM");

    ap_edit_stack out;
    ap_edit_stack_init(&out);
    round_trip_stack(&in, &out);
    AP_TEST_ASSERT(out.count == 1, "out.count=%d", out.count);
    assert_entries_equal(&in.entries[0], &out.entries[0]);
}

// Mixed stack: write all three modules in one document, ensure order
// and per-entry state survive together (catches accidental
// cross-module bleed in the parser).
static void test_mixed_stack(void)
{
    ap_edit_stack in;
    ap_edit_stack_init(&in);
    ap_edit_stack_add(&in, "lens_correction");
    ap_edit_stack_add(&in, "color_grade");
    ap_edit_stack_add(&in, "chromatic_aberration");
    AP_TEST_ASSERT(in.count == 3, "expected 3 entries, got %d", in.count);

    in.entries[0].params[0] = 35.0f;  // lens_correction.focal_mm
    in.entries[1].params[6] = 1.5f;   // color_grade.gain_r
    in.entries[2].params[2] = 1.0009f; // chromatic_aberration.r_scale
    in.entries[1].enabled = false;
    snprintf(in.entries[2].display_name, sizeof(in.entries[2].display_name),
             "CA pass");

    ap_edit_stack out;
    ap_edit_stack_init(&out);
    round_trip_stack(&in, &out);
    AP_TEST_ASSERT(out.count == in.count,
                   "out.count=%d in.count=%d", out.count, in.count);
    for (int i = 0; i < in.count; i++) {
        assert_entries_equal(&in.entries[i], &out.entries[i]);
    }
}

int main(void)
{
    test_color_grade();
    test_lens_correction();
    test_chromatic_aberration();
    test_mixed_stack();
    printf("modules/stack_pack: OK\n");
    return 0;
}
