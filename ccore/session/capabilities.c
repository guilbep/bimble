/* query capabilities (ticket FAKE1): see capabilities.h. */
#include <string.h>
#include "capabilities.h"
#include "calls.h"
#include "json_w.h"
#include "session.h"

#ifdef CC_VIEW_ONLY
#define FULL 0 /* build.sh VIEW_DENY: no verbs, IFC writer, drawings on paper, PDF writer, schedules */
#else
#define FULL 1
#endif

enum { NO, YES, FULL_BUILD, NATIVE };

/* Sorted by name (the answer's key order; capabilities_test checks it). */
static const struct {
    const char *name;
    int when;
} CAPS[] = {
    {"about", NO},             /* SET1 */
    {"collaboration", NO},     /* calls.c q_collab: a constant "nobody" (H2c) */
    {"export_ifc", FULL_BUILD},
    {"export_pdf", FULL_BUILD}, /* sheet_svg.h sheet_pdf, view_pdf (D7) */
    {"export_svg", FULL_BUILD}, /* sheet_svg.h sheet_svg (D5) */
    {"export_view", NO},       /* action on_paper (PAPER1) */
    {"georef", YES},           /* query north, coordinates (ADR 0045); G2's setting-out panel */
    {"grids", YES},            /* query grids, grid_axes (S1); G2 */
    {"history", YES},          /* query history: the undo list (calls.rs:346) */
    {"history_write", NO},     /* commit, branch (H2a) */
    {"lca", NO},
    {"levels", YES},           /* query storeys; G2 */
    {"materials", NO},         /* the materials browser (MAT1); the query materials is a list only */
    {"measure", YES},          /* the measure tool (T1) */
    {"palette", YES},          /* query palette_match (Q2) */
    {"paper_view", NO},        /* action paper_view, the On paper switch (PAPER1) */
    {"phases", NO},
    {"plan_annotations", NO},  /* actions section_labels, openings, section_marks, slab_levels (KEY1) */
    {"project_info", YES},     /* query project, verb set_project_field */
    {"quantities", FULL_BUILD}, /* schedules.h schedule_rows, schedule_csv (Q2) */
    {"rebar", NO},
    {"recent_files", NO},      /* calls.c EMPTY: a stub [] (FS1) */
    {"references", YES},       /* links.h, links_ref.h */
    {"schedules", FULL_BUILD},
    {"section_catalogue", NO},
    {"settings", NO},          /* SET1 */
    {"sheet_tools", NO},       /* actions sheet_tool_* */
    {"sheets", YES},           /* query sheets, sheet_layout (SH1) */
    {"site", NO},
    {"standards", NO},         /* query standards: null (STD1) */
    {"steel", NO},
    {"subprojects", NO},
    {"user", NO},              /* query state: user null (SET1) */
    {"users", NO},
    {"versions", NATIVE},      /* versions.h: the history store next to the file (H1) */
};

int cc_capabilities_json(const cc_session *s, Buf *out) {
    JsonW w;
    jw_init(&w, out, 0);
    jw_obj_begin(&w);
    for (size_t i = 0; i < sizeof CAPS / sizeof CAPS[0]; i++) {
        int on = CAPS[i].when == YES || (CAPS[i].when == FULL_BUILD && FULL) ||
                 (CAPS[i].when == NATIVE && FULL && s->pf && s->pf->files);
        jw_key(&w, CAPS[i].name), jw_bool(&w, on);
    }
    jw_obj_end(&w);
    return jw_finish(&w) == JW_OK ? CC_OK : (out->oom = 1, CC_ERR);
}
