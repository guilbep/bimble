/* The plan annotation toggles (ticket KEY1): the ribbon's Annotate › On plans actions
 * section_labels, openings, section_marks, slab_levels (ribbon.rs:729-744). Rust keeps them in the
 * viewer's plan template (`self.plan.template`: PlanTemplate::element_labels, opening_dimensions,
 * section_markers, slab_levels, template.rs:222-226, all on), session state: not the model, not a
 * view, not saved. Every plan the session draws on paper (sheet_svg, sheet_pdf, view_pdf; Rust:
 * layout.rs:263, viewing.rs:603, calls.rs:502) follows them; the screen's own overlays do not.
 * Enabled with a model (ribbon.rs:612 `_ => loaded`), pressed while on (ribbon.rs:631-634). */
#ifndef CCORE_SESSION_TOGGLES_H
#define CCORE_SESSION_TOGGLES_H
#include "draw_plan.h"
#include "session.h"

/* 0..3 for the four action names, else -1. */
int cc_toggle_index(const char *name);
/* Whether toggle `i` is on (the plan template's flag). */
int cc_toggle_on(const cc_session *s, int i);
/* Turns toggle `i` (ribbon.rs:729 `^= true`; no history step, no model change). */
void cc_toggle_flip(cc_session *s, int i);
/* The session's toggles onto a plan template (after PlanTemplate::default). */
void cc_toggles_apply(const cc_session *s, cc_plan_template *t);
#endif
