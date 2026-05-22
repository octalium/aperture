#include "stack.h"

#include "core/log.h"
#include "modules/module.h"

#include <stdio.h>
#include <string.h>

void ap_edit_stack_init(ap_edit_stack *s)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->focus = -1;
}

int ap_edit_stack_add(ap_edit_stack *s, const char *module_name)
{
    if (!s || !module_name) return -1;
    if (s->count >= AP_EDIT_STACK_MAX) {
        AP_ERROR("edit_stack: full (max %d entries)", AP_EDIT_STACK_MAX);
        return -1;
    }
    const ap_module *m = ap_module_find(module_name);
    if (!m) {
        AP_ERROR("edit_stack: unknown module '%s'", module_name);
        return -1;
    }

    int idx = s->count++;
    ap_edit_entry *e = &s->entries[idx];
    memset(e, 0, sizeof(*e));
    snprintf(e->module_name, sizeof(e->module_name), "%s", module_name);
    e->enabled = true;
    e->id = ++s->next_id;

    int n = m->params_count;
    if (n > AP_EDIT_PARAMS_SLOTS) n = AP_EDIT_PARAMS_SLOTS;
    if (m->params_default && n > 0) {
        memcpy(e->params, m->params_default, (size_t)n * sizeof(float));
    }
    return idx;
}

void ap_edit_stack_remove(ap_edit_stack *s, int idx)
{
    if (!s || idx < 0 || idx >= s->count) return;
    for (int i = idx; i + 1 < s->count; i++) {
        s->entries[i] = s->entries[i + 1];
    }
    s->count--;
    memset(&s->entries[s->count], 0, sizeof(s->entries[0]));

    if (s->count == 0)               s->focus = -1;
    else if (s->focus >= s->count)   s->focus = s->count - 1;
    else if (s->focus > idx)         s->focus--;
    else if (s->focus == idx)        s->focus = idx > 0 ? idx - 1 : 0;
}

void ap_edit_stack_reorder(ap_edit_stack *s, int src, int dst)
{
    if (!s) return;
    if (src < 0 || src >= s->count) return;
    if (dst < 0 || dst >= s->count) return;
    if (src == dst) return;

    ap_edit_entry tmp = s->entries[src];
    if (src < dst) {
        for (int i = src; i < dst; i++) s->entries[i] = s->entries[i + 1];
    } else {
        for (int i = src; i > dst; i--) s->entries[i] = s->entries[i - 1];
    }
    s->entries[dst] = tmp;

    int f = s->focus;
    if      (f == src)               s->focus = dst;
    else if (src < dst && f > src && f <= dst) s->focus = f - 1;
    else if (src > dst && f < src && f >= dst) s->focus = f + 1;
}

void ap_edit_stack_set_enabled(ap_edit_stack *s, int idx, bool enabled)
{
    if (!s || idx < 0 || idx >= s->count) return;
    s->entries[idx].enabled = enabled;
}

int ap_edit_stack_count(const ap_edit_stack *s) { return s ? s->count : 0; }

ap_edit_entry *ap_edit_stack_at(ap_edit_stack *s, int idx)
{
    if (!s || idx < 0 || idx >= s->count) return NULL;
    return &s->entries[idx];
}

const ap_edit_entry *ap_edit_stack_at_const(const ap_edit_stack *s, int idx)
{
    if (!s || idx < 0 || idx >= s->count) return NULL;
    return &s->entries[idx];
}

void ap_edit_stack_set_focus(ap_edit_stack *s, int idx)
{
    if (!s) return;
    if (idx < -1 || idx >= s->count) return;
    s->focus = idx;
}

int ap_edit_stack_focus(const ap_edit_stack *s)
{
    return s ? s->focus : -1;
}

int ap_edit_stack_reset(ap_edit_stack *s, int idx)
{
    if (!s || idx < 0 || idx >= s->count) return -1;
    ap_edit_entry *e = &s->entries[idx];
    const ap_module *m = ap_module_find(e->module_name);
    if (!m) return -1;
    memset(e->params, 0, sizeof(e->params));
    memset(e->str_params, 0, sizeof(e->str_params));
    int n = m->params_count;
    if (n > AP_EDIT_PARAMS_SLOTS) n = AP_EDIT_PARAMS_SLOTS;
    if (m->params_default && n > 0) {
        memcpy(e->params, m->params_default, (size_t)n * sizeof(float));
    }
    return 0;
}

const char *ap_edit_stack_label_at(const ap_edit_stack *s, int idx,
                                   char *buf, size_t buflen)
{
    if (!s || idx < 0 || idx >= s->count || !buf || buflen == 0) {
        if (buf && buflen > 0) buf[0] = '\0';
        return buf;
    }
    const ap_edit_entry *e = &s->entries[idx];
    if (e->display_name[0]) {
        snprintf(buf, buflen, "%s", e->display_name);
        return buf;
    }
    const ap_module *m = ap_module_find(e->module_name);
    const char *base = m ? m->display_name : e->module_name;

    // Auto-suffix when the module appears more than once on the stack.
    int n_before = 0;
    int n_total  = 0;
    for (int i = 0; i < s->count; i++) {
        if (s->entries[i].display_name[0]) continue;
        if (strcmp(s->entries[i].module_name, e->module_name) == 0) {
            if (i < idx) n_before++;
            n_total++;
        }
    }
    if (n_total <= 1) {
        snprintf(buf, buflen, "%s", base);
    } else {
        snprintf(buf, buflen, "%s %d", base, n_before + 1);
    }
    return buf;
}

bool ap_edit_stack_name_unique(const ap_edit_stack *s, const char *candidate,
                               int ignore_idx)
{
    if (!s || !candidate || !*candidate) return false;
    char buf[AP_EDIT_DISPLAY_LEN];
    for (int i = 0; i < s->count; i++) {
        if (i == ignore_idx) continue;
        const char *label = ap_edit_stack_label_at(s, i, buf, sizeof(buf));
        if (label && strcmp(label, candidate) == 0) return false;
    }
    return true;
}
