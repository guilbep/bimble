/* The plan annotation toggles (ticket KEY1): see toggles.h. */
#include "toggles.h"
#include <string.h>

static const char *const NAMES[] = {"section_labels", "openings", "section_marks", "slab_levels"};

int cc_toggle_index(const char *name) {
    for (int i = 0; i < 4; i++)
        if (!strcmp(name, NAMES[i])) return i;
    return -1;
}

/* plan_off: bit i set = toggle i off (0, all on, is PlanTemplate::default()). */
int cc_toggle_on(const cc_session *s, int i) { return i >= 0 && i < 4 && !(s->plan_off >> i & 1u); }

void cc_toggle_flip(cc_session *s, int i) {
    if (i >= 0 && i < 4) s->plan_off ^= 1u << i;
}

void cc_toggles_apply(const cc_session *s, cc_plan_template *t) { /* ribbon.rs:729-744 */
    t->element_labels = cc_toggle_on(s, 0);
    t->opening_dimensions = cc_toggle_on(s, 1);
    t->section_markers = cc_toggle_on(s, 2);
    t->slab_levels = cc_toggle_on(s, 3);
}
