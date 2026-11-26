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
    {"about", YES},            /* SET1: action about, query about (settings.h) */
    {"base_point_move", NO},   /* SETOUT: moving the project base point by clicks (the model's coordinates stay: ADR 0075) */
    {"cfc", NO},               /* QTY1: Assign CFC, CFC catalogue (ticket QTY2: the catalogue's licence first) */
    {"collaboration", NO},     /* calls.c q_collab: a constant "nobody" (H2c) */
    {"export_dxf", FULL_BUILD}, /* sheet_svg.h view_dxf, sheet_dxf (DXF1, ADR 0082) */
    {"export_ifc", FULL_BUILD},
    {"export_pdf", FULL_BUILD}, /* sheet_svg.h sheet_pdf, view_pdf (D7) */
    {"export_svg", FULL_BUILD}, /* sheet_svg.h sheet_svg (D5) */
    {"export_view", FULL_BUILD}, /* action on_paper: Export view (view_pdf, view_svg; PAPER1) */
    {"georef", YES},           /* query north, coordinates (ADR 0045); G2's setting-out panel */
    {"grids", YES},            /* query grids, grid_axes (S1); G2 */
    {"history", YES},          /* query history: the undo list (calls.rs:346) */
    {"history_write", NATIVE}, /* H2a: action commit, branch_create, branch_switch; H2b: merge (versions.h) */
    {"lca", NO},
    {"levels", YES},           /* query storeys; G2 */
    {"materials", YES},        /* the materials browser (MAT1): query materials_page, verbs material_* */
    {"measure", YES},          /* the measure tool (T1) */
    {"palette", YES},          /* query palette_match (Q2) */
    {"paper_view", FULL_BUILD}, /* action paper_view, the On paper switch (view_svg; PAPER1) */
    {"phases", NO},
    {"plan_annotations", YES},  /* actions section_labels, openings, section_marks, slab_levels (KEY1) */
    {"project_info", YES},     /* query project, verb set_project_field */
    {"quantities", FULL_BUILD}, /* schedules.h schedule_rows, schedule_csv (Q2) */
    {"rebar", NO},
    {"recent_files", NATIVE},  /* FS1: settings_file.h, the native host reads and opens paths (the browser cannot) */
    {"reference_planes", YES}, /* WAVE1B: named work planes (ADR 0075 decisions 9-11): query reference_planes, tool work_plane */
    {"references", YES},       /* links.h, links_ref.h */
    {"schedules", FULL_BUILD},
    {"section_catalogue", NO},
    {"setting_out", YES},      /* SETOUT: query setting_out, actions show_base_point, show_survey_point, view_true_north */
    {"settings", FULL_BUILD},  /* SET1: query settings, action set_settings (settings.h; not in the viewer-only build) */
    {"sheet_tools", NO},       /* actions sheet_tool_* */
    {"sheets", YES},           /* query sheets, sheet_layout (SH1) */
    {"site", NO},
    {"standards", NO},         /* query standards: null (STD1) */
    {"steel", NO},
    {"subprojects", NO},
    {"user", FULL_BUILD},      /* SET1: state.user from the settings (null until a name is set, as Rust) */
    {"users", NO},
    {"versions", NATIVE},      /* versions.h: the history store next to the file (H1) */
    {"views", FULL_BUILD},     /* SEC1: verbs view_add, view_remove, view_edit; sections shown live */
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
