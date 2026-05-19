#include "edit/stack_toml.h"

#include "modules/module.h"

#include <stdint.h>
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
        // Trailing blank line between entries keeps the document
        // readable; doesn't change parser behavior.
        if (i + 1 < count) {
            if (fputc('\n', f) == EOF) return -1;
        }
    }
    return 0;
}
