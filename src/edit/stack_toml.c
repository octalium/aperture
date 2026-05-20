#include "edit/stack_toml.h"

#include "modules/module.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_double(toml_table_t *t, const char *key, double *out)
{
    toml_datum_t v = toml_double_in(t, key);
    if (!v.ok) return -1;
    *out = v.u.d;
    return 0;
}

static int read_int(toml_table_t *t, const char *key, int64_t *out)
{
    toml_datum_t v = toml_int_in(t, key);
    if (!v.ok) return -1;
    *out = v.u.i;
    return 0;
}

static const char *read_string(toml_table_t *t, const char *key)
{
    toml_datum_t v = toml_string_in(t, key);
    return v.ok ? v.u.s : NULL;
}

static void load_one(toml_table_t *t, ap_edit_stack *stack)
{
    const char *module_name = read_string(t, "module");
    if (!module_name) return;
    int idx = ap_edit_stack_add(stack, module_name);
    if (idx < 0) return;
    ap_edit_entry *e = ap_edit_stack_at(stack, idx);
    const ap_module *m = ap_module_find(module_name);

    int64_t i = 0;
    if (read_int(t, "enabled", &i) == 0) e->enabled = i != 0;
    const char *display = read_string(t, "name");
    if (display && *display) {
        snprintf(e->display_name, sizeof(e->display_name), "%s", display);
    }

    if (m && m->params_names) {
        for (int s = 0; s < m->params_count; s++) {
            const char *name = m->params_names[s];
            if (!name) continue;
            double d = 0.0;
            if (read_double(t, name, &d) == 0) {
                e->params[s] = (float)d;
            }
        }
    }

    if (m && m->str_params_names) {
        int ns = m->str_params_count;
        if (ns > AP_EDIT_STR_SLOTS) ns = AP_EDIT_STR_SLOTS;
        for (int s = 0; s < ns; s++) {
            const char *name = m->str_params_names[s];
            if (!name) continue;
            toml_datum_t v = toml_string_in(t, name);
            if (v.ok) {
                snprintf(e->str_params[s], AP_EDIT_STR_LEN, "%s", v.u.s);
                free(v.u.s);   // toml_string_in hands ownership to us
            }
        }
    }
}

int ap_edit_stack_read_toml_array(toml_array_t *arr, ap_edit_stack *out)
{
    if (!out) return -1;
    ap_edit_stack_init(out);
    if (!arr) return 0;
    int n = toml_array_nelem(arr);
    for (int r = 0; r < n; r++) {
        toml_table_t *t = toml_table_at(arr, r);
        if (t) load_one(t, out);
    }
    return 0;
}

// Emit `key = "value"` with the value escaped as a TOML basic string.
// Paths can carry backslashes and quotes, so they need real escaping
// (unlike the identifier-shaped module names written raw above).
static int write_toml_string(FILE *f, const char *key, const char *val)
{
    if (fprintf(f, "%-9s = \"", key) < 0) return -1;
    for (const char *p = val; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\\' || c == '"') {
            if (fputc('\\', f) == EOF || fputc(c, f) == EOF) return -1;
        } else if (c == '\n') {
            if (fputs("\\n", f) == EOF) return -1;
        } else if (c == '\r') {
            if (fputs("\\r", f) == EOF) return -1;
        } else if (c == '\t') {
            if (fputs("\\t", f) == EOF) return -1;
        } else if (c < 0x20) {
            // Drop other control chars rather than emit invalid TOML.
        } else {
            if (fputc((int)c, f) == EOF) return -1;
        }
    }
    if (fputs("\"\n", f) == EOF) return -1;
    return 0;
}

int ap_edit_stack_write_toml(const ap_edit_stack *stack, FILE *f)
{
    if (!stack || !f) return -1;
    int count = ap_edit_stack_count(stack);
    for (int i = 0; i < count; i++) {
        const ap_edit_entry *e = ap_edit_stack_at_const(stack, i);
        if (!e) continue;
        const ap_module *m = ap_module_find(e->module_name);
        if (fprintf(f,
            "[[edit]]\n"
            "module  = \"%s\"\n"
            "enabled = %d\n",
            e->module_name, e->enabled ? 1 : 0) < 0) return -1;
        if (e->display_name[0]) {
            if (fprintf(f, "name    = \"%s\"\n", e->display_name) < 0) return -1;
        }
        if (m && m->params_names) {
            for (int s = 0; s < m->params_count; s++) {
                const char *name = m->params_names[s];
                if (!name) continue;
                if (fprintf(f, "%-9s = %g\n", name, (double)e->params[s]) < 0) {
                    return -1;
                }
            }
        }
        if (m && m->str_params_names) {
            int ns = m->str_params_count;
            if (ns > AP_EDIT_STR_SLOTS) ns = AP_EDIT_STR_SLOTS;
            for (int s = 0; s < ns; s++) {
                const char *name = m->str_params_names[s];
                if (!name || !e->str_params[s][0]) continue;
                if (write_toml_string(f, name, e->str_params[s]) != 0) {
                    return -1;
                }
            }
        }
        // Trailing blank line between entries keeps the document
        // readable; doesn't change parser behavior.
        if (i + 1 < count) {
            if (fputc('\n', f) == EOF) return -1;
        }
    }
    return 0;
}
