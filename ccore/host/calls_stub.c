/* The calls with no model open: the fixed state of a host started without a file, with the stub
 * answers of docs/research/c-core-spec.md §1. Model-backed calls replace this file later.
 *
 * Key order: every object here is a `serde_json::json!` map in Rust, and preserve_order is off
 * (spec §3, "Canonical JSON"), so keys are written in byte order. Each producer is cited. */
#include <string.h>

#include "calls.h"
#include "json_w.h"

#define CC_VERSION "0.1.0-ccore" /* `version` is the C core's own (spec §1, deviations) */

/* calls.rs:65-71 `model`: the error of a call that needs an editable model. */
static const char READ_ONLY[] = "this file is read only: import it to edit"; /* viewer/lib.rs:65 */
static const char NO_MODEL[] = "no model open"; /* calls.rs:552 */

static const char *const OPS[] = { /* calls.rs:27-41 `dispatch_op` */
    "apply", "undo", "redo", "select", "query", "action", "verb",
    "dismiss_belongs", "standards_action", "set_placing", "tab",
};

int cc_known_op(const char *op) {
    for (size_t i = 0; i < sizeof OPS / sizeof OPS[0]; i++)
        if (!strcmp(op, OPS[i])) return 1;
    return 0;
}

/* The 54 ribbon actions in palette order (ribbon.rs:130-185 ALL_ACTIONS): name (ribbon.rs:252),
 * label (ribbon.rs:188, English: the Rust labels are not translated; LinkDxf/LinkPdf as built with
 * the default feature "reference-plans", bimble-viewer/Cargo.toml:9), tab (`home`, ribbon.rs:338),
 * shortcut (`keys`, ribbon.rs:319). enabled: the spec's set (save, undo, redo, place_*,
 * export_ifc4, home). pressed: `action_on` (ribbon.rs:616) with the defaults: edges and hatching
 * on (bimble-render/src/lib.rs:1431-1432), the four plan annotations on
 * (bimble-drawing/src/template.rs:222-226), everything else off. */
typedef struct {
    const char *name, *label, *tab, *shortcut;
    char enabled, pressed;
} ActionRow;

static const ActionRow ACTIONS[] = {
    {"new", "New", "File", "Ctrl N", 0, 0},
    {"open", "Open…", "File", 0, 0, 0},
    {"import_ifc", "Import IFC…", "File", 0, 0, 0},
    {"convert_open", "Convert to .bimble (editable)…", "File", 0, 0, 0},
    {"share_for_viewing", "Share for viewing… (.bimbleview)", "File", 0, 0, 0},
    {"export_ifc4", "Export IFC4…", "File", 0, 1, 0},
    {"export_ifc2x3", "Export IFC2x3…", "File", 0, 0, 0},
    {"save", "Save", "File", "Ctrl S", 1, 0},
    {"undo", "Undo", "Quick access", "Ctrl Z", 1, 0},
    {"redo", "Redo", "Quick access", "Ctrl Y", 1, 0},
    {"place_column", "Place: Column", "Model", 0, 1, 0},
    {"place_beam", "Place: Beam", "Model", 0, 1, 0},
    {"place_wall", "Place: Wall", "Model", 0, 1, 0},
    {"place_slab", "Place: Slab", "Model", 0, 1, 0},
    {"measure", "Measure", "Model", "M", 0, 0},
    {"sheet_tool_select", "Sheet: select", "Annotate", 0, 0, 0},
    {"sheet_tool_text", "Sheet: text", "Annotate", 0, 0, 0},
    {"sheet_tool_dimension", "Sheet: dimension", "Annotate", 0, 0, 0},
    {"section_labels", "Section labels on plans", "Annotate", 0, 0, 1},
    {"openings", "Opening sizes on plans", "Annotate", 0, 0, 1},
    {"section_marks", "Section marks on plans", "Annotate", 0, 0, 1},
    {"slab_levels", "Slab levels on plans", "Annotate", 0, 0, 1},
    {"edges", "Edges", "View", "E", 0, 1},
    {"hatching", "Hatching", "View", "H", 0, 1},
    {"orthographic", "Orthographic", "View", "O", 0, 0},
    {"section_box", "Section box", "View", "B", 0, 0},
    {"box_to_selection", "Section box to selection", "View", 0, 0, 0},
    {"fit", "Fit all", "View", "F", 0, 0},
    {"top", "Top view", "View", "T", 0, 0},
    {"on_paper", "Export view… (PDF, SVG)", "Sheets", 0, 0, 0},
    {"new_sheet", "New sheet", "Sheets", 0, 0, 0},
    {"close_sheet", "Close the sheet", "Sheets", 0, 0, 0},
    {"link_model", "Link IFC…", "Collaborate", 0, 0, 0},
    {"link_dxf", "Link DXF…", "Collaborate", 0, 0, 0},
    {"link_pdf", "Link PDF…", "Collaborate", 0, 0, 0},
    {"manage_links", "Manage links…", "Collaborate", 0, 0, 0},
    {"hide_now", "Hide for now", "View", 0, 0, 0},
    {"isolate_now", "Isolate for now", "View", 0, 0, 0},
    {"hide_in_view", "Hide in view", "View", 0, 0, 0},
    {"isolate_in_view", "Isolate in view", "View", 0, 0, 0},
    {"unhide", "Unhide in view", "View", 0, 0, 0},
    {"reveal_hidden", "Reveal hidden", "View", 0, 0, 0},
    {"shared_folder", "Shared folder…", "Collaborate", 0, 0, 0},
    {"synchronize", "Synchronize", "Collaborate", 0, 0, 0},
    {"home", "Home", "File", 0, 1, 0},
    {"standards", "Graphic standards", "Manage", 0, 0, 0},
    {"project", "Project information", "Project", 0, 0, 0},
    {"materials", "Materials", "Manage", 0, 0, 0},
    {"history", "History…", "Manage", 0, 0, 0},
    {"settings", "Settings…", "File", 0, 0, 0},
    {"about", "About Bimble…", "File", 0, 0, 0},
    {"paper_view", "On paper", "View", 0, 0, 0},
    {"clear_temporary", "Show all hidden for now", "View", 0, 0, 0},
    {"show_all_in_view", "Show all in view", "View", 0, 0, 0},
};
_Static_assert(sizeof ACTIONS / sizeof ACTIONS[0] == 54, "ribbon.rs:130 has 54 actions");

static int done(JsonW *w) {
    return jw_finish(w) == JW_OK ? CC_OK : (w->b->oom = 1, CC_ERR);
}

static void err(cc_buf *out, const char *a, const char *b, const char *c) {
    buf_puts(out, a);
    if (b) buf_puts(out, b);
    if (c) buf_puts(out, c);
}

/* Rust `{:?}` of a str, for the error texts (escapes quote, backslash and control bytes). */
static void debug_str(cc_buf *out, const char *s) {
    buf_putc(out, '"');
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') buf_putc(out, '\\'), buf_putc(out, (char)ch);
        else if (ch == '\n') buf_puts(out, "\\n");
        else if (ch == '\r') buf_puts(out, "\\r");
        else if (ch == '\t') buf_puts(out, "\\t");
        else if (ch < 0x20 || ch == 0x7f) {
            static const char hex[] = "0123456789abcdef";
            buf_puts(out, "\\u{");
            if (ch >= 16) buf_putc(out, hex[ch >> 4]);
            buf_putc(out, hex[ch & 15]);
            buf_putc(out, '}');
        } else buf_putc(out, (char)ch);
    }
    buf_putc(out, '"');
}

static int q_state(JsonW *w) { /* calls.rs:239-258 */
    jw_obj_begin(w);
    jw_key(w, "editable"), jw_bool(w, 0);
    jw_key(w, "file"), jw_null(w);
    jw_key(w, "schema"), jw_null(w);
    jw_key(w, "unsaved"), jw_bool(w, 0);
    jw_key(w, "user"), jw_null(w);
    jw_key(w, "version"), jw_str(w, CC_VERSION);
    jw_obj_end(w);
    return done(w);
}

static int q_actions(JsonW *w) { /* calls.rs:325-339 */
    jw_arr_begin(w);
    for (size_t i = 0; i < sizeof ACTIONS / sizeof ACTIONS[0]; i++) {
        const ActionRow *a = &ACTIONS[i];
        jw_obj_begin(w);
        jw_key(w, "enabled"), jw_bool(w, a->enabled);
        jw_key(w, "label"), jw_str(w, a->label);
        jw_key(w, "name"), jw_str(w, a->name);
        jw_key(w, "pressed"), jw_bool(w, a->pressed);
        jw_key(w, "shortcut");
        if (a->shortcut) jw_str(w, a->shortcut);
        else jw_null(w);
        jw_key(w, "tab"), jw_str(w, a->tab);
        jw_obj_end(w);
    }
    jw_arr_end(w);
    return done(w);
}

static int q_history(JsonW *w) { /* calls.rs:346-359 */
    jw_obj_begin(w);
    jw_key(w, "redo"), jw_arr_begin(w), jw_arr_end(w);
    jw_key(w, "undo"), jw_arr_begin(w), jw_arr_end(w);
    jw_obj_end(w);
    return done(w);
}

static void capped_empty(JsonW *w) { /* calls.rs:57-63 `capped` */
    jw_obj_begin(w);
    jw_key(w, "count"), jw_u64(w, 0);
    jw_key(w, "ids"), jw_arr_begin(w), jw_arr_end(w);
    jw_obj_end(w);
}

static int q_visibility(JsonW *w) { /* calls.rs:442-460; no model: origin_mm null */
    jw_obj_begin(w);
    jw_key(w, "hidden_in_view"), capped_empty(w);
    jw_key(w, "hidden_now"), capped_empty(w);
    jw_key(w, "isolated_in_view"), capped_empty(w);
    jw_key(w, "isolated_now"), capped_empty(w);
    jw_key(w, "section_box");
    jw_obj_begin(w);
    jw_key(w, "box"), jw_null(w);
    jw_key(w, "on"), jw_bool(w, 0);
    jw_key(w, "origin_mm"), jw_null(w);
    jw_obj_end(w);
    jw_obj_end(w);
    return done(w);
}

static int q_open_views(JsonW *w) { /* calls.rs:362-397; no model: no view, no tab (Home) */
    jw_obj_begin(w);
    jw_key(w, "active"), jw_null(w);
    jw_key(w, "current"), jw_null(w);
    jw_key(w, "paper");
    jw_obj_begin(w);
    jw_key(w, "offered"), jw_bool(w, 0);
    jw_key(w, "on"), jw_bool(w, 0);
    jw_obj_end(w);
    jw_key(w, "tabs"), jw_arr_begin(w), jw_arr_end(w);
    jw_key(w, "views"), jw_arr_begin(w), jw_arr_end(w);
    jw_obj_end(w);
    return done(w);
}

static int q_collab(JsonW *w) { /* collab.rs:289-297 (json!, sorted): the lab's NO_COLLAB */
    jw_obj_begin(w);
    jw_key(w, "belongs"), jw_null(w);
    jw_key(w, "borrowed_by_me"), jw_u64(w, 0);
    jw_key(w, "borrowed_by_others"), jw_arr_begin(w), jw_arr_end(w);
    jw_key(w, "folder"), jw_null(w);
    jw_key(w, "last_sync"), jw_null(w);
    jw_key(w, "right"), jw_null(w);
    jw_key(w, "user"), jw_null(w);
    jw_obj_end(w);
    return done(w);
}

static int call_query(const char *name, cc_buf *out) {
    JsonW w;
    jw_init(&w, out, 0);
    if (!strcmp(name, "state")) return q_state(&w);
    if (!strcmp(name, "actions")) return q_actions(&w);
    if (!strcmp(name, "history")) return q_history(&w);
    if (!strcmp(name, "visibility")) return q_visibility(&w);
    if (!strcmp(name, "open_views")) return q_open_views(&w);
    if (!strcmp(name, "collab")) return q_collab(&w);
    /* null without a model: status (calls.rs:427, lib.rs:4620), placing (calls.rs:408),
     * pointer (calls.rs:403, stubbed null, spec §1); standards stubbed null (spec §1). */
    if (!strcmp(name, "status") || !strcmp(name, "placing") || !strcmp(name, "pointer") ||
        !strcmp(name, "standards")) {
        buf_puts(out, "null");
        return CC_OK;
    }
    static const char *const EMPTY[] = {"recent_files", "links", "measurements", "schedule_list", "sheets",
                                        "views", "schedules", "recent_actions"};
    for (size_t i = 0; i < sizeof EMPTY / sizeof EMPTY[0]; i++)
        if (!strcmp(name, EMPTY[i])) { /* spec §1: stub [] */
            buf_puts(out, "[]");
            return CC_OK;
        }
    /* Model queries: calls.rs `model(v)?` fails without a model. */
    static const char *const MODEL[] = {"storeys", "materials", "elements", "element", "project"};
    for (size_t i = 0; i < sizeof MODEL / sizeof MODEL[0]; i++)
        if (!strcmp(name, MODEL[i])) {
            buf_puts(out, READ_ONLY);
            return CC_ERR;
        }
    static const char *const CUT[] = {"palette_match", "verbs", "describe", "schedule_rows", "schedule_csv",
                                      "sheet_svg"};
    for (size_t i = 0; i < sizeof CUT / sizeof CUT[0]; i++)
        if (!strcmp(name, CUT[i])) {
            err(out, "query ", name, " is not in the C core");
            return CC_ERR;
        }
    /* bimble-api dispatch.rs:784 */
    buf_puts(out, "unknown query ");
    debug_str(out, name);
    buf_puts(out, "; known: storeys, materials, elements, element, project, views, sheets, schedules");
    return CC_ERR;
}

static const ActionRow *find_action(const char *name) {
    for (size_t i = 0; i < sizeof ACTIONS / sizeof ACTIONS[0]; i++)
        if (!strcmp(ACTIONS[i].name, name)) return &ACTIONS[i];
    return 0;
}

int cc_call(const char *op, const char *name, const char *body, size_t n, cc_buf *out) {
    (void)body;
    (void)n;
    if (!strcmp(op, "query")) return call_query(name, out);
    if (!strcmp(op, "action")) { /* calls.rs:538-547 */
        const ActionRow *a = find_action(name);
        if (!a) {
            buf_puts(out, "unknown action ");
            debug_str(out, name);
            return CC_ERR;
        }
        if (!a->enabled) {
            buf_puts(out, "action ");
            debug_str(out, name);
            buf_puts(out, " is not enabled now");
            return CC_ERR;
        }
        if (!strcmp(name, "home")) return CC_OK; /* the lab shows its own Home */
        buf_puts(out, NO_MODEL);
        return CC_ERR;
    }
    if (!strcmp(op, "dismiss_belongs")) return CC_OK; /* calls.rs:108-114: a no-op without a model */
    if (!strcmp(op, "standards_action")) { /* calls.rs:172-187; spec §1: no-op */
        if (!strcmp(name, "revert") || !strcmp(name, "reset") || !strcmp(name, "save")) return CC_OK;
        buf_puts(out, "unknown standards action ");
        debug_str(out, name);
        return CC_ERR;
    }
    if (!strcmp(op, "select") || !strcmp(op, "tab")) { /* calls.rs:552 "no model open"; no tab open */
        buf_puts(out, !strcmp(op, "tab") ? "no such open tab" : NO_MODEL);
        return CC_ERR;
    }
    /* apply, undo, redo, verb, set_placing: calls.rs `model(v)?` / READ_ONLY without a model. */
    buf_puts(out, READ_ONLY);
    return CC_ERR;
}

void cc_change_json(cc_buf *out) { /* calls.rs:134-167 (json!, sorted keys) */
    JsonW w;
    jw_init(&w, out, 0);
    jw_obj_begin(&w);
    jw_key(&w, "file"), jw_null(&w);
    jw_key(&w, "placing"), jw_null(&w);
    jw_key(&w, "rev"), jw_u64(&w, 0);
    jw_key(&w, "selection"), jw_arr_begin(&w), jw_arr_end(&w);
    jw_key(&w, "tab"), jw_null(&w);
    jw_key(&w, "unsaved"), jw_bool(&w, 0);
    jw_key(&w, "view"), jw_arr_begin(&w);
    for (int i = 0; i < 8; i++) jw_u64(&w, 0); /* no model: nothing shown, no tab open */
    jw_arr_end(&w);
    jw_obj_end(&w);
    jw_finish(&w);
}
