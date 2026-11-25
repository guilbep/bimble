/* The calls of the native host, answered from the open model (session.h): `Viewer::call` of
 * crates/bimble-viewer/src/calls.rs. Without a model (the host started without a file) the
 * answers are those of a viewer with nothing loaded. With a read-only scene (an IFC or a
 * .bimbleview opened as it is, session.h `view`) they are a read-only file's: the scene is shown
 * and selectable, and every call that needs the model answers READ_ONLY (calls.rs:65 `model`).
 *
 * Key order: every object here is a `serde_json::json!` map in Rust, and preserve_order is off
 * (spec §3, "Canonical JSON"), so keys are written in byte order. Each producer is cited.
 * Deviations from Rust: docs/research/c-core-spec.md §1 (version) and the
 * ones marked "Deviation" below. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "calls.h"
#include "file.h"
#include "guid.h"
#include "ifc_out.h"
#include "json_r.h"
#include "json_w.h"
#include "library.h"
#include "links.h"
#include "links_ref.h" /* L1 */
#include "mesh.h"
#include "sheet_svg.h"
#include "query.h"
#include "scene.h"
#include "session.h"
#include "verb_table.h"
#include "verbs.h"
#include "viewfile.h"
#include "views.h"
#include "sheets.h" /* ticket SH1 */
#include "view_only.h"
#include "annot.h"
#include "measure.h" /* ticket T1 */
#include "versions.h" /* ticket H1 */

#define CC_VERSION "0.1.0-ccore" /* `version` is the C core's own (spec §1, deviations) */

static const char READ_ONLY[] = "this file is read only: import it to edit"; /* viewer/lib.rs:65 */
static const char NO_MODEL[] = "no model open";                              /* calls.rs:552 */
#define CC_SIZE_MIN 10.0     /* verbs.rs:12 SIZE_MM */
#define CC_SIZE_MAX 100000.0

static const char *const OPS[] = { /* calls.rs:27-41 `dispatch_op` */
    "apply", "undo", "redo", "select", "query", "action", "verb",
    "dismiss_belongs", "standards_action", "set_placing", "tab",
    "tool", /* a C addition: the place tool (calls.h) */
};

int cc_known_op(const char *op) {
    for (size_t i = 0; i < sizeof OPS / sizeof OPS[0]; i++)
        if (!strcmp(op, OPS[i])) return 1;
    return 0;
}

/* The 54 ribbon actions in palette order (ribbon.rs:130-185 ALL_ACTIONS): name (ribbon.rs:252),
 * label (ribbon.rs:188, English: the Rust labels are not translated; LinkDxf/LinkPdf as built with
 * the default feature "reference-plans", bimble-viewer/Cargo.toml:9), tab (`home`, ribbon.rs:338),
 * shortcut (`keys`, ribbon.rs:319). `pressed`: `action_on` (ribbon.rs:616) with the defaults: edges
 * and hatching on (bimble-render/src/lib.rs:1431-1432), the four plan annotations on
 * (bimble-drawing/src/template.rs:222-226), place_<kind> while placing that kind, else off.
 * `enabled`: action_enabled (ribbon.rs:555-612) for the actions the C core runs (new, open,
 * import_ifc, convert_open, share_for_viewing, save, undo, redo, place_*, home, export_ifc4,
 * export_ifc2x3); every other one is off (spec §1). */
typedef struct {
    const char *name, *label, *tab, *shortcut;
    char pressed;
} ActionRow;

static const ActionRow ACTIONS[] = {
    {"new", "New", "File", "Ctrl N", 0},
    {"open", "Open…", "File", 0, 0},
    {"import_ifc", "Import IFC…", "File", 0, 0},
    {"convert_open", "Convert to .bimble (editable)…", "File", 0, 0},
    {"share_for_viewing", "Share for viewing… (.bimbleview)", "File", 0, 0},
    {"export_ifc4", "Export IFC4…", "File", 0, 0},
    {"export_ifc2x3", "Export IFC2x3…", "File", 0, 0},
    {"save", "Save", "File", "Ctrl S", 0},
    {"undo", "Undo", "Quick access", "Ctrl Z", 0},
    {"redo", "Redo", "Quick access", "Ctrl Y", 0},
    {"place_column", "Place: Column", "Model", 0, 0},
    {"place_beam", "Place: Beam", "Model", 0, 0},
    {"place_wall", "Place: Wall", "Model", 0, 0},
    {"place_slab", "Place: Slab", "Model", 0, 0},
    {"measure", "Measure", "Model", "M", 0},
    {"sheet_tool_select", "Sheet: select", "Annotate", 0, 0},
    {"sheet_tool_text", "Sheet: text", "Annotate", 0, 0},
    {"sheet_tool_dimension", "Sheet: dimension", "Annotate", 0, 0},
    {"section_labels", "Section labels on plans", "Annotate", 0, 1},
    {"openings", "Opening sizes on plans", "Annotate", 0, 1},
    {"section_marks", "Section marks on plans", "Annotate", 0, 1},
    {"slab_levels", "Slab levels on plans", "Annotate", 0, 1},
    {"edges", "Edges", "View", "E", 1},
    {"hatching", "Hatching", "View", "H", 1},
    {"orthographic", "Orthographic", "View", "O", 0},
    {"section_box", "Section box", "View", "B", 0},
    {"box_to_selection", "Section box to selection", "View", 0, 0},
    {"fit", "Fit all", "View", "F", 0},
    {"top", "Top view", "View", "T", 0},
    {"on_paper", "Export view… (PDF, SVG)", "Sheets", 0, 0},
    {"new_sheet", "New sheet", "Sheets", 0, 0},
    {"close_sheet", "Close the sheet", "Sheets", 0, 0},
    {"link_model", "Link IFC…", "Collaborate", 0, 0},
    {"link_dxf", "Link DXF…", "Collaborate", 0, 0},
    {"link_pdf", "Link PDF…", "Collaborate", 0, 0},
    {"manage_links", "Manage links…", "Collaborate", 0, 0},
    {"hide_now", "Hide for now", "View", 0, 0},
    {"isolate_now", "Isolate for now", "View", 0, 0},
    {"hide_in_view", "Hide in view", "View", 0, 0},
    {"isolate_in_view", "Isolate in view", "View", 0, 0},
    {"unhide", "Unhide in view", "View", 0, 0},
    {"reveal_hidden", "Reveal hidden", "View", 0, 0},
    {"shared_folder", "Shared folder…", "Collaborate", 0, 0},
    {"synchronize", "Synchronize", "Collaborate", 0, 0},
    {"home", "Home", "File", 0, 0},
    {"standards", "Graphic standards", "Manage", 0, 0},
    {"project", "Project information", "Project", 0, 0},
    {"materials", "Materials", "Manage", 0, 0},
    {"history", "History…", "Manage", 0, 0},
    {"settings", "Settings…", "File", 0, 0},
    {"about", "About Bimble…", "File", 0, 0},
    {"paper_view", "On paper", "View", 0, 0},
    {"clear_temporary", "Show all hidden for now", "View", 0, 0},
    {"show_all_in_view", "Show all in view", "View", 0, 0},
};
_Static_assert(sizeof ACTIONS / sizeof ACTIONS[0] == 54, "ribbon.rs:130 has 54 actions");

static const char *const KIND_NAME[] = {"column", "beam", "wall", "slab"}; /* verbs.rs:28 snake_case */
static const char *const SIZE_KEY[] = {"column", "beam_width", "beam_height", "wall", "slab"}; /* verbs.rs:78 */
/* Sizes serialized through json! (a sorted map): beam_height, beam_width, column, slab, wall. */
static const int SIZE_SORTED[] = {2, 1, 0, 4, 3};

static int done(JsonW *w) {
    return jw_finish(w) == JW_OK ? CC_OK : (w->b->oom = 1, CC_ERR);
}

/* Rust `{:?}` of a str, for the error texts. */
static void debug_str(cc_buf *out, const char *s) { cc_buf_debug_str(out, s, strlen(s)); }

/* Something is shown: an editable model or a read-only scene (Rust's `loaded.is_some()`). */
static int loaded(const cc_session *s) { return s->open || s->view; }
static int tool_json(cc_session *s, const cc_gid *placed, cc_buf *out); /* the place tool, below */
static int annot_json(cc_session *s, const cc_gid *added, cc_buf *out); /* the annotate tool, below */

/* ---- views and tabs (views.h) ---- */

static void tab_json(JsonW *w, const cc_session *s) { /* the current tab (ribbon.rs:406) */
    cc_sv_tab_json(w, &s->active);
}

/* ---- the scene, built once per rev (status counts, visibility origin) ---- */

static const cc_scene *scene_now(cc_session *s) {
    if (s->view == CC_VIEW_IFC) return &s->ifc->scene;
    if (s->view == CC_VIEW_FILE) return &s->vscene;
    if (!s->open) return NULL;
    if (s->scene_rev != s->rev) {
        if (s->scene_rev) cc_scene_free(&s->scene);
        cc_error e;
        s->scene_ok = cc_scene_build(&s->scene, &s->model, &e) == 0;
        if (!s->scene_ok) cc_log(s->pf, "bimble-ccore: scene: %s", e.msg);
        s->scene_rev = s->rev;
    }
    return s->scene_ok ? &s->scene : NULL;
}

const cc_scene *cc_session_scene(cc_session *s) { return scene_now(s); }

/* ---- history step ids and selection (lib.rs:1725-1805) ---- */

static int push_id(uint64_t **v, size_t *n, size_t *cap, uint64_t id) {
    if (*n == *cap) {
        size_t c = *cap ? 2 * *cap : 16;
        uint64_t *p = realloc(*v, c * sizeof *p);
        if (!p) return -1;
        *v = p, *cap = c;
    }
    (*v)[(*n)++] = id;
    return 0;
}

/* lib.rs:1767-1805 `rebuild_scene`: rev + 1; the primary selection kept by GlobalId, the others
 * dropped. */
static void rebuild(cc_session *s) {
    s->rev++;
    if (s->has_primary && !cc_model_element(&s->model, &s->primary)) s->has_primary = 0;
    s->nalso = 0;
}

/* lib.rs:1725 `try_apply` through edit.rs:233 `Editor::apply` (no commands: no step, but the
 * scene is rebuilt). */
static int try_apply(cc_session *s, const cc_command *cmds, size_t n, cc_buf *out) {
    if (!s->open) {
        buf_puts(out, READ_ONLY);
        return CC_ERR;
    }
    if (n) {
        if (cc_history_apply(&s->hist, &s->model, cmds, n, out)) return CC_ERR;
        if (push_id(&s->undo_ids, &s->nundo_ids, &s->cap_undo_ids, s->next_step++)) {
            buf_puts(out, "out of memory");
            return CC_ERR;
        }
        s->nredo_ids = 0;
        cc_session_note_apply(s, cmds, n); /* the session file logs the step with these */
    }
    rebuild(s);
    return CC_OK;
}

/* lib.rs:1744 `undo_redo`: nothing to take back is no change. */
static void undo_redo(cc_session *s, int undo) {
    if (!s->open) return;
    if (undo ? cc_history_undo(&s->hist, &s->model) : cc_history_redo(&s->hist, &s->model)) {
        if (undo && s->nundo_ids) {
            if (!push_id(&s->redo_ids, &s->nredo_ids, &s->cap_redo_ids, s->undo_ids[s->nundo_ids - 1])) s->nundo_ids--;
        } else if (!undo && s->nredo_ids) {
            if (!push_id(&s->undo_ids, &s->nundo_ids, &s->cap_undo_ids, s->redo_ids[s->nredo_ids - 1])) s->nredo_ids--;
        }
        rebuild(s);
    }
}

/* ---- placing (edit.rs:387-460) ---- */

static const char *hint(const cc_session *s) { /* edit.rs:84-95 `Placing::hint` */
    if (s->placing_kind == CC_KIND_SLAB && s->polygon) return "Click the next corner of the slab.";
    if (s->placing_kind == CC_KIND_COLUMN) return "Click where the column goes (on the storey level). Esc stops.";
    if (!s->tool.npoints) return s->placing_kind == CC_KIND_SLAB ? "Click a corner of the slab." : "Click the start point.";
    return s->placing_kind == CC_KIND_SLAB ? "Click the opposite corner." : "Click the end point.";
}

static void placing_json(cc_session *s, JsonW *w) { /* edit.rs:387-408, json! sorted */
    cc_kind kind = s->placing ? s->placing_kind : s->kind;
    const cc_storey *st = s->has_storey ? cc_model_storey(&s->model, &s->storey) : NULL;
    jw_obj_begin(w);
    jw_key(w, "active"), jw_bool(w, s->placing);
    jw_key(w, "clicks_needed"), jw_u64(w, kind == CC_KIND_COLUMN ? 1 : 2);
    jw_key(w, "hint");
    if (s->placing) jw_str(w, hint(s));
    else jw_null(w);
    jw_key(w, "kind"), jw_str(w, KIND_NAME[kind]);
    jw_key(w, "material");
    if (s->material) jw_str(w, s->material);
    else jw_null(w);
    jw_key(w, "offset_mm"), jw_arr_begin(w);
    for (int i = 0; i < 3; i++) jw_f64(w, s->offset[i]);
    jw_arr_end(w);
    jw_key(w, "points"), jw_arr_begin(w); /* the place tool's clicks (op tool) */
    for (size_t i = 0; s->placing && i < s->tool.npoints; i++)
        jw_arr_begin(w), jw_f64(w, s->tool.points[i][0]), jw_f64(w, s->tool.points[i][1]), jw_arr_end(w);
    jw_arr_end(w);
    jw_key(w, "shape"), jw_str(w, s->polygon ? "polygon" : "rectangle");
    jw_key(w, "sizes"), jw_obj_begin(w);
    for (int i = 0; i < 5; i++) jw_key(w, SIZE_KEY[SIZE_SORTED[i]]), jw_f64(w, s->sizes[SIZE_SORTED[i]]);
    jw_obj_end(w);
    jw_key(w, "storey");
    if (st) jw_str_n(w, st->id.c, 22);
    else jw_null(w);
    jw_key(w, "storey_name");
    if (st) jw_str_n(w, st->name.p, st->name.n);
    else jw_null(w);
    jw_key(w, "top_offset_mm"), jw_f64(w, s->top_offset_mm);
    jw_obj_end(w);
}

/* ribbon.rs:676-690 Action::Place(k): toggles placing that kind. */
static void place_action(cc_session *s, cc_kind k) {
    int on = s->placing && s->placing_kind == k;
    s->placing = !on;
    cc_tool_clear(&s->tool);
    if (!on) s->placing_kind = k, s->kind = k;
}

/* serde_json Value's Display: compact JSON (numbers as their token). */
static void value_text(cc_buf *out, const JrVal *v) {
    switch (v->type) {
    case JR_NULL: buf_puts(out, "null"); break;
    case JR_BOOL: buf_puts(out, v->b ? "true" : "false"); break;
    case JR_NUM: buf_append(out, v->s, v->n); break;
    case JR_STR: json_escape(out, v->s, v->n); break;
    case JR_ARR:
        buf_putc(out, '[');
        for (size_t i = 0; i < v->n; i++) {
            if (i) buf_putc(out, ',');
            value_text(out, &v->items[i]);
        }
        buf_putc(out, ']');
        break;
    default:
        buf_putc(out, '{');
        for (size_t i = 0; i < v->n; i++) {
            if (i) buf_putc(out, ',');
            json_escape(out, v->members[i].key, v->members[i].key_n);
            buf_putc(out, ':');
            value_text(out, &v->members[i].val);
        }
        buf_putc(out, '}');
    }
}

static const char *type_word(const JrVal *v) { /* serde's Unexpected, roughly */
    switch (v->type) {
    case JR_NULL: return "null";
    case JR_BOOL: return "boolean";
    case JR_NUM: return "number";
    case JR_STR: return "string";
    case JR_ARR: return "sequence";
    default: return "map";
    }
}

/* Members of an object sorted by key bytes, the last duplicate kept (serde_json::Map). */
static int member_cmp(const void *a, const void *b) {
    const JrMember *x = *(const JrMember *const *)a, *y = *(const JrMember *const *)b;
    size_t n = x->key_n < y->key_n ? x->key_n : y->key_n;
    int c = memcmp(x->key, y->key, n);
    if (c) return c;
    if (x->key_n != y->key_n) return x->key_n < y->key_n ? -1 : 1;
    return x < y ? -1 : x > y; /* stable: document order */
}
static size_t sorted_members(const JrVal *obj, const JrMember **out) {
    size_t n = 0;
    for (size_t i = 0; i < obj->n; i++) out[n++] = &obj->members[i];
    qsort(out, n, sizeof *out, member_cmp);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) { /* keep the last of equal keys */
        if (i + 1 < n && out[i]->key_n == out[i + 1]->key_n && !memcmp(out[i]->key, out[i + 1]->key, out[i]->key_n))
            continue;
        out[k++] = out[i];
    }
    return k;
}

/* edit.rs:113-140 PlacingPatch::parse, then calls.rs:190-202 and edit.rs:413-460. */
static int call_set_placing(cc_session *s, const char *body, size_t n, cc_buf *out) {
    Arena a;
    arena_init(&a, 0);
    JrError je;
    int rc = CC_ERR;
    const JrVal *v = jr_parse(&a, body, n, &je);
    if (!v) {
        buf_puts(out, je.msg);
        goto end;
    }
    if (v->type != JR_OBJ) {
        buf_puts(out, "invalid type: "), buf_puts(out, type_word(v)), buf_puts(out, ", expected a map");
        goto end;
    }
    const JrMember **ms = arena_alloc(&a, (v->n + 1) * sizeof *ms, 8);
    if (!ms) {
        buf_puts(out, "out of memory");
        goto end;
    }
    size_t nm = sorted_members(v, ms);
    const JrVal *kind = 0, *storey = 0, *material = 0, *sizes = 0, *top = 0;
    int has_polygon = 0, polygon = 0;
    cc_kind k = CC_KIND_COLUMN;
    cc_gid sid;
    for (size_t i = 0; i < nm; i++) {
        const JrMember *m = ms[i];
        const JrVal *x = &m->val;
        if (jr_key_is(m, "kind")) {
            int found = -1;
            if (x->type == JR_STR)
                for (int j = 0; j < 4; j++)
                    if (strlen(KIND_NAME[j]) == x->n && !memcmp(KIND_NAME[j], x->s, x->n)) found = j;
            if (found < 0) {
                buf_puts(out, "kind: ");
                if (x->type == JR_STR) {
                    buf_puts(out, "unknown variant `");
                    buf_append(out, x->s, x->n);
                    buf_puts(out, "`, expected one of `column`, `beam`, `wall`, `slab`");
                } else buf_puts(out, "invalid type: "), buf_puts(out, type_word(x)), buf_puts(out, ", expected a Kind");
                goto end;
            }
            kind = x, k = (cc_kind)found;
        } else if (jr_key_is(m, "storey")) {
            cc_guid g;
            const char *ge = 0;
            if (x->type != JR_STR || cc_guid_parse(x->s, x->n, &g, &ge)) {
                buf_puts(out, "storey: ");
                buf_puts(out, x->type == JR_STR && ge ? ge : "invalid type, expected a GlobalId string");
                goto end;
            }
            memcpy(sid.c, g.c, 22);
            storey = x;
        } else if (jr_key_is(m, "material")) {
            if (x->type != JR_STR) {
                buf_puts(out, "material: invalid type: "), buf_puts(out, type_word(x)), buf_puts(out, ", expected a string");
                goto end;
            }
            material = x;
        } else if (jr_key_is(m, "sizes")) {
            if (x->type != JR_OBJ) {
                buf_puts(out, "sizes: invalid type: "), buf_puts(out, type_word(x)), buf_puts(out, ", expected a map");
                goto end;
            }
            sizes = x;
        } else if (jr_key_is(m, "top_offset_mm")) {
            if (x->type != JR_NUM) {
                buf_puts(out, "top_offset_mm: invalid type: "), buf_puts(out, type_word(x)), buf_puts(out, ", expected f64");
                goto end;
            }
            top = x;
        } else if (jr_key_is(m, "shape")) {
            if (x->type == JR_STR && x->n == 9 && !memcmp(x->s, "rectangle", 9)) polygon = 0;
            else if (x->type == JR_STR && x->n == 7 && !memcmp(x->s, "polygon", 7)) polygon = 1;
            else {
                buf_puts(out, "shape: "), value_text(out, x), buf_puts(out, " is not \"rectangle\" or \"polygon\"");
                goto end;
            }
            has_polygon = 1;
        } else {
            buf_puts(out, "unknown field: "), buf_append(out, m->key, m->key_n);
            goto end;
        }
    }
    if (!s->open) {
        buf_puts(out, READ_ONLY);
        goto end;
    }
    /* edit.rs:413 Editor::set_placing: all checks first, then the changes. */
    if (storey && !cc_model_storey(&s->model, &sid)) {
        buf_puts(out, "unknown storey: "), buf_append(out, sid.c, 22);
        goto end;
    }
    if (material) { /* verbs.rs:129 `material(m, Some(name))` */
        int in_model = 0;
        for (size_t i = 0; i < s->model.nmaterials && !in_model; i++)
            in_model = s->model.materials[i].name.n == material->n && !memcmp(s->model.materials[i].name.p, material->s, material->n);
        if (!in_model) {
            const cc_library_material *lm = cc_library_lookup(material->s, material->n);
            if (!lm) {
                buf_puts(out, "unknown material: "), buf_append(out, material->s, material->n);
                goto end;
            }
            int reuse = 0;
            for (size_t i = 0; i < s->model.nmaterials && !reuse; i++) {
                const cc_material *mm = &s->model.materials[i];
                reuse = mm->category == lm->category && mm->name.n == strlen(lm->name) && !memcmp(mm->name.p, lm->name, mm->name.n);
            }
            if (!reuse) (void)cc_guid_new(); /* library::to_material makes a GlobalId: keep seeded runs in step */
        }
    }
    double ns[5];
    memcpy(ns, s->sizes, sizeof ns);
    if (sizes) {
        const JrMember **sm = arena_alloc(&a, (sizes->n + 1) * sizeof *sm, 8);
        if (!sm) {
            buf_puts(out, "out of memory");
            goto end;
        }
        size_t nsm = sorted_members(sizes, sm);
        for (size_t i = 0; i < nsm; i++) {
            int j = -1;
            for (int q = 0; q < 5; q++)
                if (jr_key_is(sm[i], SIZE_KEY[q])) j = q;
            if (j < 0) {
                buf_puts(out, "unknown size: "), buf_append(out, sm[i]->key, sm[i]->key_n);
                goto end;
            }
            const JrVal *x = &sm[i]->val;
            if (x->type != JR_NUM) {
                buf_puts(out, SIZE_KEY[j]), buf_puts(out, ": "), value_text(out, x), buf_puts(out, " is not a number");
                goto end;
            }
            if (!(x->num >= CC_SIZE_MIN && x->num <= CC_SIZE_MAX)) { /* verbs.rs:15 `size` */
                buf_puts(out, SIZE_KEY[j]), buf_puts(out, ": ");
                cc_buf_display_f64(out, x->num);
                buf_puts(out, " is not a size within [10, 100000] mm");
                goto end;
            }
            ns[j] = x->num;
        }
    }
    if (top && !(top->num >= -CC_MAX_MM && top->num <= CC_MAX_MM)) {
        buf_puts(out, "top_offset_mm: ");
        cc_buf_display_f64(out, top->num);
        buf_puts(out, " is not a finite value within ±10000000 mm");
        goto end;
    }
    memcpy(s->sizes, ns, sizeof ns);
    if (has_polygon && polygon != s->polygon) s->polygon = polygon, s->tool.npoints = 0; /* points cleared */
    if (top) s->top_offset_mm = top->num + 0.0; /* no -0.0 */
    if (storey) s->has_storey = 1, s->storey = sid;
    if (material) {
        char *p = malloc(material->n + 1);
        if (!p) {
            buf_puts(out, "out of memory");
            goto end;
        }
        memcpy(p, material->s, material->n), p[material->n] = 0;
        free(s->material), s->material = p;
    }
    if (kind) {
        s->kind = k;
        if (s->placing) s->placing_kind = k, s->tool.npoints = 0; /* a new kind drops the points */
        else place_action(s, k);                /* calls.rs:198: `place_<kind>`, enabled with a model */
    }
    rc = CC_OK;
end:
    arena_free(&a);
    return rc;
}

/* ---- queries ---- */

static int q_state(cc_session *s, JsonW *w) { /* calls.rs:239-258 */
    jw_obj_begin(w);
    jw_key(w, "editable"), jw_bool(w, s->open);
    jw_key(w, "file");
    if (loaded(s)) jw_str(w, s->file);
    else jw_null(w);
    jw_key(w, "schema");
    if (s->open && s->schema_bimble) jw_str(w, ".bimble");
    else if (s->view == CC_VIEW_IFC && s->ifc->schema.n) jw_str_n(w, s->ifc->schema.p, s->ifc->schema.n);
    else jw_null(w); /* a model's scene and a view file's have no IFC schema */
    jw_key(w, "unsaved"), jw_bool(w, cc_session_unsaved(s));
    jw_key(w, "user"), jw_null(w);
    jw_key(w, "version"), jw_str(w, CC_VERSION);
    jw_obj_end(w);
    return done(w);
}

static int action_enabled(cc_session *s, const char *name) { /* ribbon.rs:555-612, the C core's subset */
    if (!strcmp(name, "new") || !strcmp(name, "open") || !strcmp(name, "import_ifc")) return 1; /* ribbon.rs:559 */
    if (!strcmp(name, "save_as")) return s->open; /* not ribbon actions (file_action) */
    if (!strcmp(name, "close")) return loaded(s);
    /* ribbon.rs:575: Rust shares any scene shown: a model's, an IFC read as it is, a view file. */
    if (!strcmp(name, "share_for_viewing")) return s->open || s->view != CC_VIEW_NONE;
    /* ribbon.rs:564-572: an IFC open read-only, natively (the browser keeps no path to read it
     * again), with something to edit (Deviation: any element, not structural_count). */
    if (!strcmp(name, "convert_open")) return s->view == CC_VIEW_IFC && s->pf->files && s->ifc->scene.nelements > 0;
    if (!strcmp(name, "save")) return cc_session_unsaved(s);
    if (!strcmp(name, "undo")) return s->open && s->hist.nundo > 0;
    if (!strcmp(name, "redo")) return s->open && s->hist.nredo > 0;
    if (!strncmp(name, "place_", 6)) return s->open;
    if (!strcmp(name, "export_ifc4") || !strcmp(name, "export_ifc2x3")) return s->open; /* ribbon.rs:577 */
    if (!strcmp(name, "home")) return 1;
    /* Links (ticket B5, links.h): ribbon.rs:596 ManageLinks `loaded`, LinkModel `_ => loaded`. */
    if (!strcmp(name, "link_model") || !strcmp(name, "manage_links")) return loaded(s);
    if (!strcmp(name, "measure")) return loaded(s); /* ribbon.rs:612 `_ => loaded` (ticket T1) */
    /* ---- L1: reference plans (links_ref.h): ribbon.rs `_ => loaded`; stored in a model's view ---- */
    if (!strcmp(name, "link_pdf")) return s->open;
    /* Visibility and the section box (ribbon.rs:592-611; the C core has only the 3D view: never flat). */
    if (!strcmp(name, "section_box") || !strcmp(name, "reveal_hidden")) return loaded(s);
    if (!strcmp(name, "box_to_selection")) return loaded(s) && s->vis.section_on;
    if (!strcmp(name, "clear_temporary")) return loaded(s) && s->vis.has_temp;
    if (!strcmp(name, "show_all_in_view")) return loaded(s) && cc_vis_hidden_in_view(&s->vis);
    if (!strcmp(name, "hide_now") || !strcmp(name, "isolate_now") || !strcmp(name, "hide_in_view") || !strcmp(name, "isolate_in_view"))
        return loaded(s) && s->has_primary;
    if (!strcmp(name, "unhide")) {
        if (!loaded(s) || !s->has_primary) return 0;
        if (cc_vis_hidden(&s->vis, &s->primary)) return 1;
        for (size_t i = 0; i < s->nalso; i++)
            if (cc_vis_hidden(&s->vis, &s->also[i])) return 1;
        return 0;
    }
    return 0;
}

static int action_pressed(cc_session *s, const ActionRow *a) {
    if (!strncmp(a->name, "place_", 6)) return s->open && s->placing && !strcmp(a->name + 6, KIND_NAME[s->placing_kind]);
    if (!strcmp(a->name, "section_box")) return s->vis.section_on; /* ribbon.rs:629 */
    if (!strcmp(a->name, "reveal_hidden")) return s->vis.reveal;   /* ribbon.rs:643 */
    if (!strcmp(a->name, "measure")) return loaded(s) && s->measure.on; /* ribbon.rs:625 (ticket T1) */
    return a->pressed;
}

static int q_actions(cc_session *s, JsonW *w) { /* calls.rs:325-339 */
    jw_arr_begin(w);
    for (size_t i = 0; i < sizeof ACTIONS / sizeof ACTIONS[0]; i++) {
        const ActionRow *a = &ACTIONS[i];
        jw_obj_begin(w);
        jw_key(w, "enabled"), jw_bool(w, action_enabled(s, a->name));
        jw_key(w, "label"), jw_str(w, a->label);
        jw_key(w, "name"), jw_str(w, a->name);
        jw_key(w, "pressed"), jw_bool(w, action_pressed(s, a));
        jw_key(w, "shortcut");
        if (a->shortcut) jw_str(w, a->shortcut);
        else jw_null(w);
        jw_key(w, "tab"), jw_str(w, a->tab);
        jw_obj_end(w);
    }
    jw_arr_end(w);
    return done(w);
}

static int q_visibility(cc_session *s, JsonW *w) { /* calls.rs:442-460 (visibility.h) */
    const cc_scene *sc = loaded(s) ? scene_now(s) : NULL;
    cc_vis_json(&s->vis, w, sc ? sc->origin : NULL);
    return done(w);
}

/* calls.rs:362-397 (views.h): the views shown (the defaults made from the storeys, stored ones
 * over them), the active one, the open tabs in order, the current one (null: Home). A ViewSummary
 * through to_value is a sorted map (api/query.rs:164-178, `type` flattened), plus `auto_name`
 * (viewing.rs:17-38). paper: offered for a plan or section shown (ribbon.rs:1478 two_pictures);
 * never on (the C core has no On paper picture). */
static int q_open_views(cc_session *s, JsonW *w) {
    if (cc_sv_sync(s)) return w->b->oom = 1, CC_ERR;
    Arena a;
    arena_init(&a, 0);
    cc_view *v = NULL;
    size_t n = 0;
    if (cc_sv_list(s, &a, &v, &n)) {
        arena_free(&a);
        return w->b->oom = 1, CC_ERR;
    }
    int tab = loaded(s) && s->tab_open && s->has_active, twod = 0;
    for (size_t i = 0; tab && i < n; i++)
        if (!cc_gid_cmp(&v[i].id, &s->active)) twod = v[i].kind != CC_VIEW_THREE_D;
    jw_obj_begin(w);
    jw_key(w, "active");
    if (loaded(s) && s->has_active) jw_str_n(w, s->active.c, 22);
    else jw_null(w);
    jw_key(w, "current");
    if (tab) tab_json(w, s);
    else jw_null(w);
    jw_key(w, "paper");
    jw_obj_begin(w);
    jw_key(w, "offered"), jw_bool(w, twod);
    jw_key(w, "on"), jw_bool(w, 0);
    jw_obj_end(w);
    jw_key(w, "tabs"), jw_arr_begin(w);
    for (size_t i = 0; loaded(s) && i < s->ntabs; i++) cc_sv_tab_json(w, &s->tabs[i]);
    jw_arr_end(w);
    jw_key(w, "views"), jw_arr_begin(w);
    for (size_t i = 0; i < n; i++) cc_sv_summary_json(w, s->open ? &s->model : NULL, &v[i]);
    jw_arr_end(w);
    jw_obj_end(w);
    arena_free(&a);
    return done(w);
}

/* The box a plan shows (views.rs view_clip): the scene's bounds, z cut to the plan's range. */
static void plan_box(const cc_scene *sc, const cc_view *v, const double z[4], cc_vis *box) {
    cc_vis_fit(box, sc, NULL);
    if (!box->has_box) return;
    double o = sc->origin[2];
    double lo = v->look == CC_LOOK_DOWN ? z[CC_RANGE_DEPTH] : z[CC_RANGE_CUT];
    double hi = v->look == CC_LOOK_DOWN ? z[CC_RANGE_CUT] : z[CC_RANGE_DEPTH];
    if (lo == lo && lo > -1e300 && lo < 1e300 && (float)(lo - o) > box->min[2]) box->min[2] = (float)(lo - o);
    if (hi == hi && hi > -1e300 && hi < 1e300 && (float)(hi - o) < box->max[2]) box->max[2] = (float)(hi - o);
}

/* A C addition (views.h, ticket C1): what the viewport draws for the active view.
 *   {"id", "type": "three_d"|"plan"|"section", "look": "down"|"up"|null, "storey": its id or null,
 *    "elevation": the storey's (world mm) or null, "cut": the cut plane (world mm) or null,
 *    "clip": {"min":[x,y,z],"max":[x,y,z]} (scene coordinates, f32 as ClipBox) or null}
 * clip: views.rs view_clip for a plan: the model's bounds (all elements, ClipBox::around), z cut
 * to what the plan shows (ResolvedRange::visible: depth to cut looking down, cut to depth looking
 * up). null for a 3D view (and for a section: not drawn by the C core yet). Nothing loaded: null. */
static int q_view_clip(cc_session *s, JsonW *w) {
    Arena a;
    arena_init(&a, 0);
    cc_view v;
    int r = loaded(s) && s->tab_open ? cc_sv_active(s, &a, &v) : 0;
    if (r < 0) {
        arena_free(&a);
        return w->b->oom = 1, CC_ERR;
    }
    if (!r) {
        arena_free(&a);
        jw_null(w);
        return done(w);
    }
    const cc_model *m = s->open ? &s->model : NULL;
    double z[4];
    int plan = v.kind == CC_VIEW_PLAN && !cc_sv_resolve(m, &v, z);
    const cc_scene *sc = scene_now(s);
    const cc_storey *st = NULL;
    if (v.kind == CC_VIEW_PLAN && v.storey.n == 22 && m) {
        cc_gid sid;
        memcpy(sid.c, v.storey.p, 22);
        st = cc_model_storey(m, &sid);
    }
    jw_obj_begin(w);
    jw_key(w, "clip");
    cc_vis box;
    cc_vis_init(&box);
    if (plan && sc) plan_box(sc, &v, z, &box);
    if (plan && box.has_box) {
        jw_obj_begin(w);
        jw_key(w, "max"), jw_arr_begin(w);
        for (int k = 0; k < 3; k++) jw_f64(w, (double)box.max[k]);
        jw_arr_end(w);
        jw_key(w, "min"), jw_arr_begin(w);
        for (int k = 0; k < 3; k++) jw_f64(w, (double)box.min[k]);
        jw_arr_end(w);
        jw_obj_end(w);
    } else {
        jw_null(w);
    }
    cc_vis_free(&box);
    jw_key(w, "cut");
    if (plan) jw_f64(w, z[CC_RANGE_CUT]);
    else jw_null(w);
    jw_key(w, "elevation");
    if (st) jw_f64(w, st->elevation);
    else jw_null(w);
    jw_key(w, "id"), jw_str_n(w, v.id.c, 22);
    jw_key(w, "look");
    if (v.kind == CC_VIEW_PLAN) jw_str(w, cc_look_name(v.look));
    else jw_null(w);
    jw_key(w, "storey");
    if (v.kind == CC_VIEW_PLAN) jw_str_n(w, v.storey.p, v.storey.n);
    else jw_null(w);
    jw_key(w, "type"), jw_str(w, cc_view_kind_name(v.kind));
    jw_obj_end(w);
    arena_free(&a);
    return done(w);
}

/* A C addition (annot.h, ticket C4): {"view"?: id (default: the active view), "unit"?: "mm"|"cm"|"m"}
 * -> the view's dimensions and texts resolved for drawing (cc_annotations_json); null without
 * that view (or nothing loaded). */
static int q_view_annotations(cc_session *s, const char *body, size_t n, JsonW *w) {
    Arena a;
    arena_init(&a, 0);
    JrError je;
    const JrVal *args = n ? jr_parse(&a, body, n, &je) : NULL;
    const JrVal *vid = args && args->type == JR_OBJ ? jr_get(args, "view") : NULL;
    const JrVal *un = args && args->type == JR_OBJ ? jr_get(args, "unit") : NULL;
    int unit = CC_UNIT_MM, rc;
    if (n && !args) {
        buf_puts(w->b, je.msg);
        arena_free(&a);
        return CC_ERR;
    }
    if (un && un->type == JR_STR && un->n == 2 && !memcmp(un->s, "cm", 2)) unit = CC_UNIT_CM;
    else if (un && un->type == JR_STR && un->n == 1 && un->s[0] == 'm') unit = CC_UNIT_M;
    else if (un && !(un->type == JR_STR && un->n == 2 && !memcmp(un->s, "mm", 2)) && un->type != JR_NULL) {
        buf_puts(w->b, "unit: expected \"mm\", \"cm\" or \"m\"");
        arena_free(&a);
        return CC_ERR;
    }
    if (vid && vid->type != JR_NULL && (vid->type != JR_STR || vid->n != 22)) {
        buf_puts(w->b, "view: expected a GlobalId");
        arena_free(&a);
        return CC_ERR;
    }
    cc_view *v = NULL, *all = NULL, act;
    size_t nv = 0;
    if (loaded(s) && vid && vid->type == JR_STR) {
        if (cc_sv_list(s, &a, &all, &nv)) return arena_free(&a), w->b->oom = 1, CC_ERR;
        for (size_t i = 0; i < nv && !v; i++)
            if (!memcmp(all[i].id.c, vid->s, 22)) v = &all[i];
    } else if (loaded(s) && s->tab_open) {
        int r = cc_sv_active(s, &a, &act);
        if (r < 0) return arena_free(&a), w->b->oom = 1, CC_ERR;
        if (r) v = &act;
    }
    if (v) cc_annotations_json(w, s->open ? &s->model : NULL, v, unit);
    else jw_null(w);
    rc = done(w);
    arena_free(&a);
    return rc;
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

/* `{:.0?}` of a Duration (core::time Debug): the largest unit with a non-zero integer part,
 * rounded half up. */
static void duration0(cc_buf *out, double ms) {
    uint64_t ns = ms <= 0 ? 0 : (uint64_t)(ms * 1e6 + 0.5);
    const char *unit;
    uint64_t div;
    if (ns >= 1000000000ull) unit = "s", div = 1000000000ull;
    else if (ns >= 1000000ull) unit = "ms", div = 1000000ull;
    else if (ns >= 1000ull) unit = "µs", div = 1000ull;
    else unit = "ns", div = 1;
    uint64_t q = ns / div, r = ns % div;
    if (div > 1 && r >= div / 2) q++;
    buf_u64(out, q);
    buf_puts(out, unit);
}

static void size_text(cc_buf *out, uint64_t bytes) { /* big.rs:376-392 */
    double b = (double)bytes, x;
    const char *unit;
    if (b >= 1e9) x = b / 1e9, unit = "GB";
    else if (b >= 1e6) x = b / 1e6, unit = "MB";
    else if (b >= 1e3) x = b / 1e3, unit = "kB";
    else {
        buf_u64(out, bytes);
        buf_puts(out, " B");
        return;
    }
    char t[64];
    snprintf(t, sizeof t, x < 9.95 ? "%.1f %s" : "%.0f %s", x, unit);
    buf_puts(out, t);
}

/* lib.rs:4620-4660 `status`, Status at lib.rs:4712-4725 (struct order). Nothing is hidden
 * (visible = total), nothing drawn by the host: fps null, idle, app_bytes 0. `triangles`: the
 * scene's triangles, each element counted (Deviation: Rust counts the GPU scene's). model_bytes:
 * big.rs:215 held = 225 per stored triangle (scene.rs:235 stored_triangles). */
static int q_status(cc_session *s, JsonW *w, cc_buf *out) {
    if (!loaded(s)) {
        buf_puts(out, "null");
        return CC_OK;
    }
    const cc_scene *sc = scene_now(s);
    uint64_t total = sc ? sc->nelements : 0, tri = 0, stored = 0;
    if (sc) {
        for (size_t k = 0; k < sc->nelements; k++) {
            const cc_mesh *m = cc_scene_local_mesh(sc, k);
            tri += m->ni / 3;
            if (sc->elements[k].shape < 0) stored += m->ni / 3;
            else stored += 1; /* each instance; the shape once below */
        }
        for (size_t i = 0; i < sc->nshapes; i++) stored += sc->shapes[i].ni / 3;
    }
    uint64_t model_bytes = (uint64_t)(225.0 * (double)stored);
    Buf text;
    buf_init(&text);
    buf_u64(&text, total), buf_putc(&text, '/'), buf_u64(&text, total);
    buf_puts(&text, " elements · "), buf_u64(&text, tri);
    buf_puts(&text, " triangles · loaded in "), duration0(&text, s->load_ms);
    buf_puts(&text, " · model ≈ "), size_text(&text, model_bytes);
    buf_puts(&text, " · idle");
    jw_obj_begin(w);
    jw_key(w, "visible"), jw_u64(w, total);
    jw_key(w, "total"), jw_u64(w, total);
    jw_key(w, "triangles"), jw_u64(w, tri);
    jw_key(w, "load_ms"), jw_f64(w, s->load_ms);
    jw_key(w, "model_bytes"), jw_u64(w, model_bytes);
    jw_key(w, "app_bytes"), jw_u64(w, 0);
    jw_key(w, "fps"), jw_null(w);
    jw_key(w, "idle"), jw_bool(w, 1);
    jw_key(w, "text"), jw_str_n(w, text.p ? text.p : "", text.len);
    jw_obj_end(w);
    buf_free(&text);
    return done(w);
}

/* A model query through cc_query (bimble_api), with the viewer's own texts for `element`
 * (calls.rs:302-309: serde's error, then "no element with this id"). Deviation: serde's
 * " at line L column C" suffix is not added. */
static int model_query(cc_session *s, const char *name, const char *body, size_t n, cc_buf *out) {
    int st = cc_query(&s->model, name, body, n, out);
    if (st == 200) return CC_OK;
    if (!strcmp(name, "element") || !strcmp(name, "elements")) {
        static const char BAD[] = "bad arguments: ", NOEL[] = "no element GlobalId(";
        if (out->len >= sizeof NOEL - 1 && !memcmp(out->p, NOEL, sizeof NOEL - 1)) {
            buf_clear(out);
            buf_puts(out, "no element with this id");
        } else if (out->len >= sizeof BAD - 1 && !memcmp(out->p, BAD, sizeof BAD - 1)) {
            memmove(out->p, out->p + sizeof BAD - 1, out->len - (sizeof BAD - 1));
            out->len -= sizeof BAD - 1;
        }
    }
    return CC_ERR;
}

/* calls.rs:340-344 `verbs` and `describe`: the verb table (cc_query), no model needed. A C addition's
 * schema (verb_table.h: rename) is answered too, where Rust says "no verb with this name". */
static int verb_query(cc_session *s, const char *name, const char *body, size_t n, cc_buf *out) {
    if (cc_query(&s->model, name, body, n, out) == 200) return CC_OK;
    static const char NONE[] = "no verb with this name";
    if (out->len == sizeof NONE - 1 && !memcmp(out->p, NONE, out->len)) {
        Arena a;
        arena_init(&a, 0);
        JrError je;
        const JrVal *v = jr_parse(&a, body, n, &je);
        const JrVal *nm = v && v->type == JR_OBJ ? jr_get(v, "name") : NULL;
        for (size_t i = 0; nm && nm->type == JR_STR && i < cc_verb_c_addition_count; i++)
            if (strlen(cc_verb_c_additions[i].name) == nm->n && !memcmp(cc_verb_c_additions[i].name, nm->s, nm->n)) {
                buf_clear(out);
                buf_puts(out, cc_verb_c_additions[i].schema);
                arena_free(&a);
                return CC_OK;
            }
        arena_free(&a);
    }
    return CC_ERR;
}

static int call_query(cc_session *s, const char *name, const char *body, size_t n, cc_buf *out) {
    JsonW w;
    jw_init(&w, out, 0);
    if (!strcmp(name, "state")) return q_state(s, &w);
    if (!strcmp(name, "actions")) return q_actions(s, &w);
    if (!strcmp(name, "history")) { /* calls.rs:346-359 */
        if (s->open) {
            cc_history_json(&s->hist, out);
            return out->oom ? CC_ERR : CC_OK;
        }
        buf_puts(out, "{\"redo\":[],\"undo\":[]}");
        return CC_OK;
    }
    { /* H1: the history store's queries (versions.h: versions, branches, version_changes, pending) */
        int rc = cc_versions_query(s, name, body, n, out);
        if (rc >= 0) return rc;
    }
    if (!strcmp(name, "visibility")) return q_visibility(s, &w);
    if (!strcmp(name, "visibility_sets")) { /* a C addition (visibility.h): the page draws from these */
        cc_vis_sets_json(&s->vis, &w);
        return done(&w);
    }
    if (!strcmp(name, "open_views")) return q_open_views(s, &w);
    if (!strcmp(name, "view_clip")) return q_view_clip(s, &w);
    if (!strcmp(name, "view_annotations")) return q_view_annotations(s, body, n, &w);
    if (!strcmp(name, "annotate")) { /* the annotate tool's state (calls.h "annotate") */
        if (!s->open) {
            buf_puts(out, "null");
            return CC_OK;
        }
        return annot_json(s, NULL, out);
    }
    if (!strcmp(name, "collab")) return q_collab(&w);
    if (!strcmp(name, "status")) return q_status(s, &w, out);
    if (!strcmp(name, "placing")) { /* calls.rs:408-414: null without an editable model */
        if (!s->open) {
            buf_puts(out, "null");
            return CC_OK;
        }
        placing_json(s, &w);
        return done(&w);
    }
    if (!strcmp(name, "ghost")) { /* the place tool's state (calls.h "tool"): null without an editable model */
        if (!s->open) {
            buf_puts(out, "null");
            return CC_OK;
        }
        return tool_json(s, NULL, out);
    }
    /* pointer (calls.rs:403, stubbed null, spec §1); standards stubbed null (spec §1). */
    if (!strcmp(name, "pointer") || !strcmp(name, "standards")) {
        buf_puts(out, "null");
        return CC_OK;
    }
    if (!strcmp(name, "links")) { /* links.h: [] without links (spec §1's stub) */
        cc_links_json(s, &w);
        int lrc = done(&w);
        if (lrc == CC_OK) cc_refs_links_append(s, out); /* L1: then the views' reference plans */
        return lrc;
    }
    /* ---- L1: a view's reference plans, placed (links_ref.h) ---- */
    if (!strcmp(name, "view_references")) return cc_refs_view_references(s, body, n, out) ? CC_ERR : CC_OK;
    if (!strcmp(name, "element")) { /* {"id", "link"}: an element of a linked model (links.h) */
        Arena a;
        arena_init(&a, 0);
        JrError je;
        const JrVal *v = jr_parse(&a, body, n, &je);
        const JrVal *lk = v && v->type == JR_OBJ ? jr_get(v, "link") : NULL, *id = lk ? jr_get(v, "id") : NULL;
        int rc = -2;
        if (lk && lk->type == JR_STR) {
            if (!id || id->type != JR_STR) buf_puts(out, "id: a string"), rc = -1;
            else rc = cc_link_element_json(s, lk->s, lk->n, id->s, id->n, out);
        }
        arena_free(&a);
        if (rc != -2) return rc ? CC_ERR : CC_OK;
    }
    if (!strcmp(name, "measurements")) { /* calls.rs:425, state.rs measurements_json (ticket T1) */
        cc_measurements_json(&w, s->measure.dims, s->measure.n);
        return done(&w);
    }
    if (!strcmp(name, "measure")) { /* a C addition: the measure tool's state (calls.h "measure_*") */
        cc_measure_json(&s->measure, &w);
        return done(&w);
    }
    static const char *const EMPTY[] = {"recent_files", "schedule_list", "recent_actions"};
    for (size_t i = 0; i < sizeof EMPTY / sizeof EMPTY[0]; i++)
        if (!strcmp(name, EMPTY[i])) { /* spec §1: stub [] */
            buf_puts(out, "[]");
            return CC_OK;
        }
    if (!strcmp(name, "verbs") || !strcmp(name, "describe")) return verb_query(s, name, body, n, out);
    if (!strcmp(name, "sheet_layout")) return cc_sheet_layout(s, scene_now(s), body, n, out); /* SH1, sheets.h */
    if (!strcmp(name, "sheet_svg")) return cc_paper_sheet_svg(s, s->open ? scene_now(s) : NULL, body, n, out); /* D5, sheet_svg.h */
    static const char *const CUT[] = {"palette_match", "schedule_rows", "schedule_csv"};
    for (size_t i = 0; i < sizeof CUT / sizeof CUT[0]; i++)
        if (!strcmp(name, CUT[i])) {
            buf_puts(out, "query "), buf_puts(out, name), buf_puts(out, " is not in the C core");
            return CC_ERR;
        }
    if (s->open) return model_query(s, name, body, n, out); /* calls.rs:276-309 and :519-527 */
    if (!strcmp(name, "north") || !strcmp(name, "coordinates")) { /* ADR 0045, query.h: null without a model */
        buf_puts(out, "null");
        return CC_OK;
    }
    static const char *const EMPTY_NO_MODEL[] = {"sheets", "views", "schedules", "grids"};
    for (size_t i = 0; i < sizeof EMPTY_NO_MODEL / sizeof EMPTY_NO_MODEL[0]; i++)
        if (!strcmp(name, EMPTY_NO_MODEL[i])) { /* spec §1: stub [] */
            buf_puts(out, "[]");
            return CC_OK;
        }
    static const char *const MODEL[] = {"storeys", "materials", "elements", "element", "project"};
    for (size_t i = 0; i < sizeof MODEL / sizeof MODEL[0]; i++)
        if (!strcmp(name, MODEL[i])) { /* calls.rs `model(v)?` */
            buf_puts(out, READ_ONLY);
            return CC_ERR;
        }
    /* bimble-api dispatch.rs:784 */
    buf_puts(out, "unknown query ");
    debug_str(out, name);
    buf_puts(out, "; known: storeys, materials, elements, element, project, views, sheets, schedules");
    return CC_ERR;
}

/* ---- the file actions (ticket A5): new, open, save, save as, export IFC, close ----
 * Rust's ribbon runs `new`, `open`, `import_ifc`, `save`, `export_ifc4`, `export_ifc2x3`
 * (ribbon.rs:656-671) and asks the user through dialogs: where to open from, where to save a new
 * model, whether to drop unsaved changes (lib.rs:1688 new_model, Confirm::Open). The C core has
 * no dialog, so these actions take an optional JSON body instead (Rust's actions take none):
 *   {"path": "<file>", "discard": true|false}
 * - path: the file to open (open, import_ifc: required), to save to (save_as: required with
 *   files), to export to (export_ifc4/2x3: optional, else "<stem>.ifc" next to the model). In
 *   the wasm module (no files) a path is a file name: open reads the bytes the page staged,
 *   writes go to the page (wasm_api.h host_saved).
 * - discard: new, open, import_ifc and close refuse while there are unsaved changes (Rust asks
 *   first) unless it is true.
 * Two actions the ribbon does not have (not listed by query actions; enabled with a model):
 * `save_as` (Rust's Save of a new model asks where, lib.rs:1832-1846) and `close`.
 * Deviations: Rust's call answers Ok and shows a failure in the editor's message; here a refused
 * or failed open, new, close, save or save as answers 400 with the reason (an agent needs it).
 * An export still answers Ok and logs its outcome (share.rs:68-77). */

typedef struct {
    char *path; /* malloc'd; NULL: none */
    int discard;
} FileArgs;

/* Parses the optional body. 0 ok; -1 with the error text in `out`. */
static int file_args(const char *body, size_t n, FileArgs *fa, cc_buf *out) {
    memset(fa, 0, sizeof *fa);
    size_t i = 0;
    while (i < n && (body[i] == ' ' || body[i] == '\t' || body[i] == '\n' || body[i] == '\r')) i++;
    if (i == n) return 0; /* no body: Rust's action */
    Arena a;
    arena_init(&a, 0);
    JrError je;
    const JrVal *v = jr_parse(&a, body, n, &je);
    int rc = -1;
    if (!v) buf_puts(out, je.msg);
    else if (v->type == JR_NULL) rc = 0;
    else if (v->type != JR_OBJ) buf_puts(out, "the action's body is {\"path\": string, \"discard\": bool}");
    else {
        const JrVal *p = jr_get(v, "path"), *d = jr_get(v, "discard");
        if (p && p->type != JR_STR && p->type != JR_NULL) buf_puts(out, "path: a string");
        else if (d && d->type != JR_BOOL && d->type != JR_NULL) buf_puts(out, "discard: a bool");
        else if (p && p->type == JR_STR && (!p->n || strlen(p->s) < p->n)) buf_puts(out, "path: empty or with a NUL");
        else {
            rc = 0;
            fa->discard = d && d->type == JR_BOOL && d->b;
            if (p && p->type == JR_STR) {
                fa->path = malloc(p->n + 1);
                if (!fa->path) rc = -1, buf_puts(out, "out of memory");
                else memcpy(fa->path, p->s, p->n), fa->path[p->n] = 0;
            }
        }
    }
    arena_free(&a);
    return rc;
}

static int fail(cc_buf *out, const char *msg) {
    buf_puts(out, msg);
    return CC_ERR;
}

/* export IFC (share.rs:50-85 export_ifc): Rust asks where to save, proposing "<stem>.ifc"; the
 * host writes `path`, else that name next to the model (never over the open file). The
 * timestamp is now, UTC (date::iso_utc; the module's clock is the page's, wasm_api.h). */
/* `path`, else "<stem>.<ext>" next to the open file ("model.<ext>" without a stem). */
static void sibling(const cc_session *s, const char *path, const char *ext, char *to, size_t cap) {
    if (path) {
        snprintf(to, cap, "%s", path);
        return;
    }
    const char *slash = strrchr(s->path, '/');
    const char *name = slash ? slash + 1 : s->path;
    const char *dot = strrchr(name, '.');
    size_t dir = (size_t)(name - s->path), stem = dot && dot != name ? (size_t)(dot - name) : strlen(name);
    if (stem == 0) snprintf(to, cap, "%.*smodel.%s", (int)dir, s->path, ext);
    else snprintf(to, cap, "%.*s%.*s.%s", (int)dir, s->path, (int)stem, name, ext);
}

static void export_ifc(cc_session *s, cc_ifc_schema schema, const char *path) {
    const char *label = schema == CC_IFC4 ? "IFC4" : "IFC2x3";
    char to[4096 + 16];
    sibling(s, path, "ifc", to, sizeof to);
    if (!strcmp(to, s->path)) {
        cc_log(s->pf, "bimble-ccore: %s: export would overwrite the open file", to);
        return;
    }
    const char *file = strrchr(to, '/'); /* FILE_NAME holds the file name */
    file = file ? file + 1 : to;
    char stamp[32];
    cc_iso_utc(s->pf->unix_s(s->pf->ctx), stamp);
    Buf b;
    buf_init(&b);
    int ok = cc_ifc_export(&s->model, file, stamp, schema, &b) == 0 && !b.oom;
    if (ok) ok = s->pf->write_file(s->pf->ctx, to, b.p, b.len) == 0;
    buf_free(&b);
    /* share.rs:68-77: the outcome goes in the view message; the call itself succeeds. */
    if (ok && schema == CC_IFC4) cc_log(s->pf, "bimble-ccore: Saved %s (%s)", to, label);
    else if (ok)
        cc_log(s->pf, "bimble-ccore: Saved %s (%s): material values (density, strength) and the map conversion "
                      "are not in IFC2x3; the place and project north are kept in the building's placement",
                to, label);
    else cc_log(s->pf, "bimble-ccore: %s: cannot write", to);
}

/* share.rs:7-11 NOT_KEPT */
static const char NOT_KEPT[] =
    "A .bimbleview keeps each element's shape, class, name, GlobalId, storey and material (its family as imported, "
    "not as your standards draw it), the storeys and the grids. Not kept: property sets and quantities, types, views, "
    "sheets, schedules, annotations, reference plans, linked models and the history.";

/* File -> Share for viewing (share.rs:15-45): the scene shown as a gzipped .bimbleview
 * (viewfile.h, the raw stream byte for byte with Rust's): a model's, an IFC open read-only
 * (cc_ifc_scene_meta) or a view file open (cc_viewfile_meta_from_file). Rust asks where, proposing
 * "<stem>.bimbleview"; the host writes `path`, else that name next to the model (never over the
 * open file). Like export IFC, the call answers Ok and logs the outcome. */
static void share_for_viewing(cc_session *s, const char *path) {
    char to[4096 + 16];
    sibling(s, path, "bimbleview", to, sizeof to);
    if (!strcmp(to, s->path)) {
        cc_log(s->pf, "bimble-ccore: %s: the view file would overwrite the open file", to);
        return;
    }
    const cc_scene *sc = scene_now(s);
    if (!sc) {
        cc_log(s->pf, "bimble-ccore: %s: the scene cannot be built", to);
        return;
    }
    Buf b, msg;
    buf_init(&b), buf_init(&msg);
    cc_viewfile_meta mt;
    int ok = (s->view == CC_VIEW_IFC    ? cc_ifc_scene_meta(s->ifc, &mt)
              : s->view == CC_VIEW_FILE ? cc_viewfile_meta_from_file(&mt, s->vf)
                                        : cc_viewfile_meta_from_model(&mt, &s->model, sc)) == 0;
    if (ok) ok = cc_viewfile_write_meta(&mt, sc, 1, &b) == 0 && !b.oom;
    cc_viewfile_meta_free(&mt);
    if (ok) ok = s->pf->write_file(s->pf->ctx, to, b.p, b.len) == 0;
    if (ok) {
        size_text(&msg, b.len);
        cc_log(s->pf, "bimble-ccore: Saved %s (%s): opens in Bimble, also in a browser, read-only. %s", to,
               buf_cstr(&msg), NOT_KEPT);
    } else cc_log(s->pf, "bimble-ccore: %s: cannot write", to);
    buf_free(&b), buf_free(&msg);
}

static const char UNSAVED[] = "unsaved changes: save them first, or send {\"discard\": true}";

/* The file actions; -1: `name` is not one of them. */
static int file_action(cc_session *s, const char *name, const FileArgs *fa, cc_buf *out) {
    cc_error err;
    int opening = !strcmp(name, "open") || !strcmp(name, "import_ifc");
    if (opening || !strcmp(name, "new") || !strcmp(name, "close")) {
        if (opening && !fa->path) return fail(out, "open: the C core has no file dialog: send {\"path\": \"<file>\"}");
        if (cc_session_unsaved(s) && !fa->discard) return fail(out, UNSAVED);
        if (!strcmp(name, "close")) return cc_session_close(s), CC_OK;
        /* open: as the Rust viewer, an IFC or a .bimbleview read-only (cc_session_open_file);
         * import_ifc: an editable copy (lib.rs:1403 import_ifc). */
        int rc = !opening                  ? cc_session_new_model(s, &err)
                 : !strcmp(name, "open") ? cc_session_open_file(s, fa->path, &err)
                                         : cc_session_open(s, fa->path, &err);
        if (rc) return fail(out, err.msg);
        if (!opening) cc_log(s->pf, "bimble-ccore: New model, not saved"); /* lib.rs:1709 */
        return CC_OK;
    }
    if (!strcmp(name, "save") || !strcmp(name, "save_as")) {
        const char *to = NULL;
        if (!strcmp(name, "save_as")) {
            to = fa->path;
            if (!to && s->pf->files) return fail(out, "save_as: the C core has no file dialog: send {\"path\": \"<file>\"}");
        } else if (fa->path) return fail(out, "save takes no path: use save_as");
        else if (s->untitled && s->pf->files) /* lib.rs:1832: a new model has no file yet: Rust asks where */
            return fail(out, "a new model has no file yet: action save_as with {\"path\": \"<file>\"}");
        if (cc_session_save(s, to, &err)) return fail(out, err.msg);
        return CC_OK;
    }
    if (!strcmp(name, "export_ifc4") || !strcmp(name, "export_ifc2x3")) {
        export_ifc(s, !strcmp(name, "export_ifc4") ? CC_IFC4 : CC_IFC2X3, fa->path);
        return CC_OK;
    }
    if (!strcmp(name, "share_for_viewing")) {
        share_for_viewing(s, fa->path);
        return CC_OK;
    }
    if (!strcmp(name, "convert_open")) { /* ribbon.rs:659-663: import the open IFC */
        char ifc[sizeof s->path];
        memcpy(ifc, s->path, sizeof ifc);
        if (cc_session_open(s, ifc, &err)) return fail(out, err.msg);
        return CC_OK;
    }
    return -1;
}

static int gid_cmp(const void *a, const void *b);

/* The selection, sorted and unique (visibility.rs `selection`: scene order = GlobalId order). */
static cc_gid *selection_ids(const cc_session *s, size_t *n) {
    *n = 0;
    cc_gid *v = malloc((s->nalso + 1) * sizeof *v);
    if (!v) return NULL;
    if (s->has_primary) v[(*n)++] = s->primary;
    for (size_t i = 0; i < s->nalso; i++) v[(*n)++] = s->also[i];
    qsort(v, *n, sizeof *v, gid_cmp);
    size_t k = 0;
    for (size_t i = 0; i < *n; i++)
        if (!k || cc_gid_cmp(&v[k - 1], &v[i])) v[k++] = v[i];
    *n = k;
    return v;
}

/* ribbon.rs:708-775: the section box and visibility actions. 0 ok (or not one of them), -1 with
 * the error in `out` (out of memory). Session state only: no history step, rev unchanged. */
static int vis_action(cc_session *s, const char *name, cc_buf *out) {
    cc_vis *v = &s->vis;
    const cc_gid *primary = s->has_primary ? &s->primary : NULL;
    if (!strcmp(name, "section_box")) cc_vis_toggle(v, scene_now(s), primary);
    else if (!strcmp(name, "box_to_selection")) cc_vis_fit(v, scene_now(s), primary);
    else if (!strcmp(name, "reveal_hidden")) v->reveal = !v->reveal;
    else if (!strcmp(name, "clear_temporary")) cc_vis_clear_temporary(v);
    else if (!strcmp(name, "show_all_in_view")) cc_vis_show_all_in_view(v);
    else {
        static const char *const HIDE[] = {"hide_now", "isolate_now", "hide_in_view", "isolate_in_view", "unhide"};
        int a = -1;
        for (int i = 0; i < 5; i++)
            if (!strcmp(name, HIDE[i])) a = i; /* CC_VIS_HIDE_NOW … CC_VIS_UNHIDE */
        if (a < 0) return 0;
        size_t n;
        cc_gid *sel = selection_ids(s, &n);
        int rc = !sel || cc_vis_hide_or_isolate(v, a, sel, n);
        free(sel);
        if (rc) {
            buf_puts(out, "out of memory");
            return -1;
        }
        if (n && !v->reveal) s->has_primary = 0, s->nalso = 0; /* visibility.rs:106 clear_selection */
    }
    return 0;
}

/* link_model and manage_links (links.h). */
static int link_action(cc_session *s, const char *name, const char *body, size_t n, cc_buf *out) {
    Arena a;
    arena_init(&a, 0);
    JrError je;
    int rc = CC_ERR;
    size_t i = 0;
    while (i < n && (body[i] == ' ' || body[i] == '\t' || body[i] == '\n' || body[i] == '\r')) i++;
    const JrVal *v = i < n ? jr_parse(&a, body, n, &je) : NULL;
    if (i < n && !v) {
        buf_puts(out, je.msg);
        goto end;
    }
    if (v && v->type == JR_NULL) v = NULL;
    if (v && v->type != JR_OBJ) {
        buf_puts(out, !strcmp(name, "link_model") ? "the action's body is {\"path\": string}"
                                                  : "the action's body is {\"op\": string, \"name\": string}");
        goto end;
    }
    if (!strcmp(name, "link_model")) {
        const JrVal *p = v ? jr_get(v, "path") : NULL;
        if (!p || p->type != JR_STR) {
            buf_puts(out, "link_model: the C core has no file dialog: send {\"path\": \"<file>\"}");
            goto end;
        }
        if (!p->n || strlen(p->s) < p->n) {
            buf_puts(out, "path: empty or with a NUL");
            goto end;
        }
        cc_error err;
        if (cc_links_add(s, p->s, &err)) {
            buf_puts(out, err.msg);
            goto end;
        }
        rc = CC_OK;
        goto end;
    }
    const JrVal *op = v ? jr_get(v, "op") : NULL, *nm = v ? jr_get(v, "name") : NULL;
    if (op && op->type != JR_STR) {
        buf_puts(out, "op: a string");
        goto end;
    }
    if (!op || (op->n == 4 && !memcmp(op->s, "list", 4))) {
        JsonW w;
        jw_init(&w, out, 0);
        cc_links_json(s, &w);
        rc = done(&w);
        goto end;
    }
    static const char *const OPS_L[] = {"remove", "reload", "show", "hide"};
    int o = -1;
    for (int q = 0; q < 4; q++)
        if (op->n == strlen(OPS_L[q]) && !memcmp(op->s, OPS_L[q], op->n)) o = q;
    if (o < 0) {
        buf_puts(out, "op: one of list, remove, reload, show, hide");
        goto end;
    }
    if (!nm || nm->type != JR_STR) {
        buf_puts(out, "name: the linked file's name (query links)");
        goto end;
    }
    int li = cc_links_find(s, nm->s, nm->n);
    if (li < 0) {
        buf_puts(out, "no linked model "), buf_append(out, nm->s, nm->n);
        goto end;
    }
    if (o == 0) cc_links_remove(s, li);
    else if (o == 1) {
        cc_error err;
        if (cc_links_reload(s, li, &err)) {
            buf_puts(out, err.msg);
            goto end;
        }
    } else cc_links_set_visible(s, li, o == 2);
    rc = CC_OK;
end:
    arena_free(&a);
    return rc;
}

/* Actions the C core runs that the ribbon does not list (above). */
static const char *const HIDDEN_ACTIONS[] = {"save_as", "close"};

/* calls.rs:538-547 call_action, ribbon.rs:650 run_action. */
static int call_action(cc_session *s, const char *name, const char *body, size_t n, cc_buf *out) {
    const ActionRow *a = NULL;
    int hidden = 0;
    for (size_t i = 0; i < sizeof ACTIONS / sizeof ACTIONS[0]; i++)
        if (!strcmp(ACTIONS[i].name, name)) a = &ACTIONS[i];
    for (size_t i = 0; i < sizeof HIDDEN_ACTIONS / sizeof HIDDEN_ACTIONS[0]; i++)
        if (!strcmp(HIDDEN_ACTIONS[i], name)) hidden = 1;
    if (!a && !hidden) {
        buf_puts(out, "unknown action ");
        debug_str(out, name);
        return CC_ERR;
    }
    if (!action_enabled(s, name)) {
        buf_puts(out, "action ");
        debug_str(out, name);
        buf_puts(out, " is not enabled now");
        return CC_ERR;
    }
    if (!strcmp(name, "link_model") || !strcmp(name, "manage_links")) return link_action(s, name, body, n, out);
    /* ---- L1: link_pdf, one set_view / add_view Command (links_ref.h) ---- */
    if (!strcmp(name, "link_pdf")) {
        Arena ra;
        arena_init(&ra, 0);
        cc_cmds l;
        cc_cmds_init(&l);
        int lrc = cc_refs_link(s, "pdf", body, n, scene_now(s), &ra, &l, out) ? CC_ERR : try_apply(s, l.v, l.n, out);
        cc_cmds_free(&l);
        arena_free(&ra);
        return lrc;
    }
    FileArgs fa;
    if (file_args(body, n, &fa, out)) return CC_ERR;
    int rc = file_action(s, name, &fa, out);
    free(fa.path);
    if (rc != -1) return rc;
    if (!strcmp(name, "undo")) undo_redo(s, 1);
    else if (!strcmp(name, "redo")) undo_redo(s, 0);
    else if (!strncmp(name, "place_", 6)) {
        for (int k = 0; k < 4; k++)
            if (!strcmp(name + 6, KIND_NAME[k])) place_action(s, (cc_kind)k);
        if (s->placing) s->measure.on = 0, s->measure.has_first = s->measure.has_hover = 0; /* ribbon.rs:687 */
    } else if (!strcmp(name, "measure")) cc_measure_toggle(&s->measure); /* ribbon.rs:691 (ticket T1) */
    else if (vis_action(s, name, out)) return CC_ERR;
    /* home: the lab shows its own Home */
    return CC_OK;
}

/* ---- select (calls.rs:550-572) ---- */

static int gid_cmp(const void *a, const void *b) { return cc_gid_cmp(a, b); }

/* A read-only scene's element by GlobalId (calls.rs:552-556: the scene's ids; an id the file
 * holds that is not 22 bytes long cannot be selected here). */
static int scene_has(cc_session *s, const cc_gid *id) {
    const cc_scene *sc = scene_now(s);
    for (size_t k = 0; sc && k < sc->nelements; k++)
        if (sc->elements[k].id.c[0] && !memcmp(sc->elements[k].id.c, id->c, 22)) return 1;
    return 0;
}

static int call_select(cc_session *s, const char *body, size_t n, cc_buf *out) {
    Arena a;
    arena_init(&a, 0);
    JrError je;
    int rc = CC_ERR;
    const JrVal *v = jr_parse(&a, body, n, &je);
    if (!v) {
        buf_puts(out, je.msg);
        goto end;
    }
    if (v->type == JR_OBJ && jr_get(v, "link")) { /* an element of a linked model (links.h) */
        const JrVal *lk = jr_get(v, "link"), *ids = jr_get(v, "ids");
        if (lk->type != JR_STR) {
            buf_puts(out, "link: a string");
            goto end;
        }
        if (!ids || ids->type != JR_ARR || ids->n > 1 || (ids->n && ids->items[0].type != JR_STR)) {
            buf_puts(out, "ids: [] or one GlobalId (one linked element at a time)");
            goto end;
        }
        if (!loaded(s)) {
            buf_puts(out, NO_MODEL);
            goto end;
        }
        int li = cc_links_find(s, lk->s, lk->n);
        if (li < 0) {
            buf_puts(out, "no linked model "), buf_append(out, lk->s, lk->n);
            goto end;
        }
        if (!ids->n) {
            s->link_sel = -1;
            rc = CC_OK;
            goto end;
        }
        const JrVal *x = &ids->items[0];
        if (x->n != 22 || cc_link_find(&s->links[li], x->s) < 0) {
            buf_puts(out, "unknown GlobalIds: "), buf_append(out, x->s, x->n);
            goto end;
        }
        if (!s->links[li].visible) {
            buf_puts(out, "the linked model "), buf_append(out, lk->s, lk->n), buf_puts(out, " is hidden");
            goto end;
        }
        s->link_sel = li;
        memcpy(s->link_sel_id, x->s, 22);
        s->has_primary = 0, s->nalso = 0; /* one selection: theirs or ours (lib.rs:2100) */
        rc = CC_OK;
        goto end;
    }
    if (v->type != JR_ARR) {
        buf_puts(out, "invalid type: "), buf_puts(out, type_word(v)), buf_puts(out, ", expected a sequence");
        goto end;
    }
    for (size_t i = 0; i < v->n; i++)
        if (v->items[i].type != JR_STR) {
            buf_puts(out, "invalid type: "), buf_puts(out, type_word(&v->items[i])), buf_puts(out, ", expected a string");
            goto end;
        }
    if (!loaded(s)) {
        buf_puts(out, NO_MODEL);
        goto end;
    }
    cc_gid *ids = arena_alloc(&a, (v->n + 1) * sizeof *ids, 8);
    if (!ids) {
        buf_puts(out, "out of memory");
        goto end;
    }
    int unknown = 0;
    for (size_t i = 0; i < v->n; i++) {
        const JrVal *x = &v->items[i];
        int found = 0;
        if (x->n == 22) {
            memcpy(ids[i].c, x->s, 22);
            found = s->open ? cc_model_element(&s->model, &ids[i]) != NULL : scene_has(s, &ids[i]);
        }
        if (!found) {
            buf_puts(out, unknown++ ? ", " : "unknown GlobalIds: ");
            buf_append(out, x->s, x->n);
        }
    }
    if (unknown) goto end;
    s->link_sel = -1; /* a selection in the open model drops the linked one */
    s->has_primary = v->n > 0;
    if (v->n) s->primary = ids[0];
    s->nalso = 0;
    if (v->n > 1) { /* also_selected: a BTreeSet of scene indices (scene order = GlobalId order) */
        if (v->n - 1 > s->capalso) {
            cc_gid *p = realloc(s->also, (v->n - 1) * sizeof *p);
            if (!p) {
                buf_puts(out, "out of memory");
                goto end;
            }
            s->also = p, s->capalso = v->n - 1;
        }
        memcpy(s->also, ids + 1, (v->n - 1) * sizeof *ids);
        qsort(s->also, v->n - 1, sizeof *s->also, gid_cmp);
        size_t k = 0;
        for (size_t i = 0; i < v->n - 1; i++)
            if (!k || cc_gid_cmp(&s->also[k - 1], &s->also[i])) s->also[k++] = s->also[i];
        s->nalso = k;
    }
    rc = CC_OK;
end:
    arena_free(&a);
    return rc;
}

/* ---- tab (calls.rs:205-232) ---- */

static int call_tab(cc_session *s, const char *body, size_t n, cc_buf *out) {
    Arena a;
    arena_init(&a, 0);
    JrError je;
    int rc = CC_ERR;
    const JrVal *v = jr_parse(&a, body, n, &je);
    if (!v) {
        buf_puts(out, je.msg);
        goto end;
    }
    const JrVal *op = v->type == JR_OBJ ? jr_get(v, "op") : NULL, *t = v->type == JR_OBJ ? jr_get(v, "tab") : NULL;
    if (v->type != JR_OBJ) {
        buf_puts(out, "invalid type: "), buf_puts(out, type_word(v)), buf_puts(out, ", expected struct Args");
        goto end;
    }
    if (!op || op->type != JR_STR) {
        buf_puts(out, op ? "invalid type: op, expected a string" : "missing field `op`");
        goto end;
    }
    if (!t) {
        buf_puts(out, "missing field `tab`");
        goto end;
    }
    /* ribbon.rs:415-432 TabRef::from_json */
    const JrVal *ty = t->type == JR_OBJ ? jr_get(t, "type") : NULL;
    const char *tys = ty && ty->type == JR_STR ? ty->s : NULL;
    int is_view = 0;
    cc_gid tid;
    memset(&tid, 0, sizeof tid);
    if (tys && (!strcmp(tys, "view") || !strcmp(tys, "sheet"))) {
        const JrVal *id = jr_get(t, "id");
        cc_guid g;
        const char *ge = 0;
        if (!id || id->type != JR_STR || cc_guid_parse(id->s, id->n, &g, &ge)) {
            buf_puts(out, "tab id: ");
            buf_puts(out, ge && id && id->type == JR_STR ? ge : "invalid type, expected a GlobalId string");
            goto end;
        }
        is_view = !strcmp(tys, "view");
        memcpy(tid.c, g.c, 22);
    } else if (tys && !strcmp(tys, "schedule")) {
        const JrVal *nm = jr_get(t, "name");
        if (!nm || nm->type != JR_STR) {
            buf_puts(out, "a schedule tab needs a name");
            goto end;
        }
    } else if (!tys || (strcmp(tys, "project") && strcmp(tys, "materials"))) {
        buf_puts(out, "unknown tab type ");
        if (tys) buf_puts(out, "Some("), debug_str(out, tys), buf_putc(out, ')');
        else buf_puts(out, "None");
        goto end;
    }
    int close;
    if (!strcmp(op->s, "activate") || !strcmp(op->s, "open")) close = 0;
    else if (!strcmp(op->s, "close")) close = 1;
    else {
        buf_puts(out, "unknown op "), debug_str(out, op->s);
        goto end;
    }
    if (!strcmp(tys, "sheet")) { /* ticket SH1 (sheets.h): the page opens on that sheet, no tab kept */
        if (cc_sheet_known(s, &tid)) rc = CC_OK;
        else buf_puts(out, "no such sheet");
        goto end;
    }
    /* calls.rs:220-232 (views.h): any view of open_views opens; activate an open tab only. */
    if (cc_sv_sync(s)) {
        buf_puts(out, "out of memory");
        goto end;
    }
    int known = 0;
    if (is_view) {
        cc_view *vs;
        size_t nv;
        if (cc_sv_list(s, &a, &vs, &nv)) {
            buf_puts(out, "out of memory");
            goto end;
        }
        for (size_t i = 0; i < nv && !known; i++) known = !cc_gid_cmp(&vs[i].id, &tid);
    }
    if (!strcmp(op->s, "open")) {
        if (!is_view) {
            buf_puts(out, "op \"open\" takes a view");
            goto end;
        }
        if (!known) {
            buf_puts(out, "no such view");
            goto end;
        }
    } else if (!close && !(is_view && known && loaded(s) && cc_sv_tab_open(s, &tid))) {
        buf_puts(out, "no such open tab");
        goto end;
    }
    if (close) cc_sv_close(s, is_view && loaded(s), &tid); /* ribbon.rs:1484 tab_verb */
    else cc_sv_go(s, &tid);
    rc = CC_OK;
end:
    arena_free(&a);
    return rc;
}

/* ---- the dispatcher (calls.rs:74-130) ---- */

/* A parsed value back to compact JSON text: numbers as their own token (exact), strings
 * re-escaped, members in the document's order. Depth is bounded by the reader (JR_MAX_DEPTH). */
static void jr_write(cc_buf *b, const JrVal *v) {
    switch (v->type) {
    case JR_NULL: buf_puts(b, "null"); break;
    case JR_BOOL: buf_puts(b, v->b ? "true" : "false"); break;
    case JR_NUM: buf_append(b, v->s, v->n); break;
    case JR_STR: json_escape(b, v->s, v->n); break;
    case JR_ARR:
        buf_putc(b, '[');
        for (size_t i = 0; i < v->n; i++) {
            if (i) buf_putc(b, ',');
            jr_write(b, &v->items[i]);
        }
        buf_putc(b, ']');
        break;
    default:
        buf_putc(b, '{');
        for (size_t i = 0; i < v->n; i++) {
            if (i) buf_putc(b, ',');
            json_escape(b, v->members[i].key, v->members[i].key_n);
            buf_putc(b, ':');
            jr_write(b, &v->members[i].val);
        }
        buf_putc(b, '}');
    }
}

/* calls.rs:524-534 call_verb, plus ADR 0068 rule 3 (see calls.h "verb"): the body is the verb's
 * arguments; an object body may also carry the top-level keys "dry_run" (bool) and "if_rev"
 * (a non-negative integer), taken out before the verb reads its arguments. */
static int call_verb(cc_session *s, const char *name, const char *body, size_t n, cc_buf *out) {
    int blank = 1; /* args_json.trim().is_empty(): null */
    for (size_t i = 0; i < n; i++)
        if (body[i] != ' ' && body[i] != '\t' && body[i] != '\n' && body[i] != '\r') blank = 0;
    int dry_run = 0, rc = CC_ERR;
    int64_t if_rev = -1;
    Arena a;
    arena_init(&a, 0);
    cc_buf args; /* the arguments without dry_run and if_rev, when the body had them */
    buf_init(&args);
    const char *argp = body;
    size_t argn = blank ? 0 : n;
    if (!blank) { /* serde_json::from_str first: a body that does not parse fails before the model */
        JrError je;
        const JrVal *v = jr_parse(&a, body, n, &je);
        if (!v) {
            buf_puts(out, je.msg);
            goto done;
        }
        if (v->type == JR_OBJ) {
            int strip = 0;
            for (size_t i = 0; i < v->n; i++) {
                const JrMember *mb = &v->members[i];
                const JrVal *x = &mb->val;
                if (jr_key_is(mb, "dry_run")) {
                    if (x->type != JR_BOOL) {
                        buf_puts(out, "bad arguments: dry_run: expected a boolean");
                        goto done;
                    }
                    dry_run = x->b, strip = 1;
                } else if (jr_key_is(mb, "if_rev")) {
                    if (x->type != JR_NUM || !x->num_int || x->num_neg || x->n > 18) {
                        buf_puts(out, "bad arguments: if_rev: expected a non-negative integer");
                        goto done;
                    }
                    if_rev = 0;
                    for (size_t k = 0; k < x->n; k++) if_rev = if_rev * 10 + (x->s[k] - '0');
                    strip = 1;
                }
            }
            if (strip) {
                buf_putc(&args, '{');
                int first = 1;
                for (size_t i = 0; i < v->n; i++) {
                    const JrMember *mb = &v->members[i];
                    if (jr_key_is(mb, "dry_run") || jr_key_is(mb, "if_rev")) continue;
                    if (!first) buf_putc(&args, ',');
                    first = 0;
                    json_escape(&args, mb->key, mb->key_n);
                    buf_putc(&args, ':');
                    jr_write(&args, &mb->val);
                }
                buf_putc(&args, '}');
                if (args.oom) {
                    buf_puts(out, "out of memory");
                    goto done;
                }
                argp = args.p, argn = args.len;
            }
        }
    }
    if (!s->open) {
        buf_puts(out, READ_ONLY);
        goto done;
    }
    {
        cc_cmds l;
        cc_cmds_init(&l);
        /* CC_VERB_CONFLICT (if_rev is not s->rev) is a 400 too: the carriers know 200 and 400 only;
         * its text starts with "conflict: ". */
        if (cc_verb_run_ex(&s->model, name, argp, argn, dry_run, if_rev, s->rev, &l, out)) rc = CC_ERR;
        else if (dry_run) cc_commands_json(l.v, l.n, out), rc = out->oom ? CC_ERR : CC_OK;
        else rc = try_apply(s, l.v, l.n, out);
        cc_cmds_free(&l);
    }
done:
    buf_free(&args);
    arena_free(&a);
    return rc;
}

/* ---- the place tool (tool.h, calls.h "tool"; a C addition, ADR 0068 rule 2) ---- */

typedef struct {
    int has_at, has_ray, has_ray_tol, has_tol, free_, ortho;
    double at[2], ray[6], ray_tol[6], tol;
} ToolArgs;

static int tool_nums(const JrVal *v, double *dst, size_t n) {
    if (v->type != JR_ARR || v->n != n) return -1;
    for (size_t i = 0; i < n; i++) {
        if (v->items[i].type != JR_NUM || !(v->items[i].num >= -1e12 && v->items[i].num <= 1e12)) return -1;
        dst[i] = v->items[i].num;
    }
    return 0;
}

/* The pointer body: {"at":[x,y]} (world plan mm) or {"ray":[ox,oy,oz,dx,dy,dz]} (scene
 * coordinates, as the BMSH mesh) with "ray_tol" (the ray 12 px beside it) or "tol_mm"; "free",
 * "ortho" booleans. Empty or null: no position. */
static int tool_args(const char *body, size_t n, ToolArgs *t, cc_buf *out) {
    memset(t, 0, sizeof *t);
    size_t k = 0;
    while (k < n && (body[k] == ' ' || body[k] == '\n' || body[k] == '\t' || body[k] == '\r')) k++;
    if (k == n || (n - k == 4 && !memcmp(body + k, "null", 4))) return 0;
    Arena a;
    arena_init(&a, 0);
    JrError je;
    int rc = -1;
    const JrVal *v = jr_parse(&a, body, n, &je);
    if (!v) {
        buf_puts(out, je.msg);
        goto end;
    }
    if (v->type != JR_OBJ) {
        buf_puts(out, "invalid type: "), buf_puts(out, type_word(v)), buf_puts(out, ", expected a map");
        goto end;
    }
    for (size_t i = 0; i < v->n; i++) {
        const JrMember *m = &v->members[i];
        const JrVal *x = &m->val;
        const char *bad = NULL;
        if (jr_key_is(m, "at")) bad = tool_nums(x, t->at, 2) ? "at: expected [x, y] in mm" : NULL, t->has_at = 1;
        else if (jr_key_is(m, "ray")) bad = tool_nums(x, t->ray, 6) ? "ray: expected [ox, oy, oz, dx, dy, dz]" : NULL, t->has_ray = 1;
        else if (jr_key_is(m, "ray_tol"))
            bad = tool_nums(x, t->ray_tol, 6) ? "ray_tol: expected [ox, oy, oz, dx, dy, dz]" : NULL, t->has_ray_tol = 1;
        else if (jr_key_is(m, "tol_mm")) {
            if (x->type != JR_NUM || !(x->num >= 0.0 && x->num <= CC_MAX_MM)) bad = "tol_mm: expected a number within [0, 10000000]";
            else t->tol = x->num, t->has_tol = 1;
        } else if (jr_key_is(m, "free") || jr_key_is(m, "ortho")) {
            if (x->type != JR_BOOL) bad = jr_key_is(m, "free") ? "free: expected a boolean" : "ortho: expected a boolean";
            else if (jr_key_is(m, "free")) t->free_ = x->b;
            else t->ortho = x->b;
        } else {
            buf_puts(out, "unknown field: "), buf_append(out, m->key, m->key_n);
            goto end;
        }
        if (bad) {
            buf_puts(out, bad);
            goto end;
        }
    }
    rc = 0;
end:
    arena_free(&a);
    return rc;
}

static double storey_level(const cc_session *s) { /* edit.rs storey_elevation */
    const cc_storey *st = s->has_storey ? cc_model_storey(&s->model, &s->storey) : NULL;
    return st ? st->elevation : 0.0;
}

/* lib.rs placing_point: the pointer on the storey plane, snapped (or rounded, `free`), then the
 * ortho of a 2-point kind. No position, or the ray misses the plane: no hover. */
static void tool_pointer(cc_session *s, const ToolArgs *a) {
    cc_tool *t = &s->tool;
    const cc_scene *sc = scene_now(s);
    double org[3] = {0, 0, 0}, level = storey_level(s), p[2], tol = 0.0;
    if (sc) memcpy(org, sc->origin, sizeof org);
    if (!a->has_at && !a->has_ray) return; /* a click without a position: where the pointer was */
    t->has_hover = 0;
    if (a->has_at) p[0] = a->at[0], p[1] = a->at[1];
    else if (cc_ray_plane(a->ray, level - org[2], p)) return;
    else p[0] += org[0], p[1] += org[1];
    if (a->has_tol) tol = a->tol;
    else if (a->has_ray && a->has_ray_tol) {
        double q[2];
        if (!cc_ray_plane(a->ray_tol, level - org[2], q)) tol = cc_hypot(q[0] + org[0] - p[0], q[1] + org[1] - p[1]);
        if (!(tol <= CC_MAX_MM)) tol = 0.0;
    }
    int kind = CC_SNAP_NONE;
    double q[2];
    if (a->free_ || !sc) q[0] = cc_round10(p[0]), q[1] = cc_round10(p[1]);
    else {
        if (!t->has_targets || t->targets_rev != s->rev || t->targets_level != level) {
            double next = level + 4000.0; /* the storey above, or level + 4 m */
            for (size_t i = 0; i < s->model.nstoreys; i++) {
                double e = s->model.storeys[i].elevation;
                if (e > level + 1.0 && e < next) next = e;
            }
            t->has_targets = !cc_snap_targets_build(&t->targets, sc, level, next);
            t->targets_rev = s->rev, t->targets_level = level;
        }
        kind = cc_snap(&t->targets, p, tol, q);
    }
    if (a->ortho && t->npoints && (t->annot ? t->annot == 1 && t->npoints == 1 : s->placing_kind != CC_KIND_COLUMN)) {
        double r[2];
        cc_ortho(t->points[t->npoints - 1], q, r);
        if (r[0] != q[0] || r[1] != q[1]) kind = CC_SNAP_NONE;
        q[0] = r[0], q[1] = r[1];
    }
    t->has_hover = 1, t->hover[0] = q[0] + 0.0, t->hover[1] = q[1] + 0.0, t->hover_kind = kind;
}

/* The arguments of `verb place` for a and b (b NULL: a column), from the placing state. */
static void place_args(const cc_session *s, const double *a, const double *b, cc_buf *out) {
    JsonW w;
    jw_init(&w, out, 0);
    jw_obj_begin(&w);
    jw_key(&w, "a"), jw_arr_begin(&w), jw_f64(&w, a[0]), jw_f64(&w, a[1]), jw_arr_end(&w);
    if (b) jw_key(&w, "b"), jw_arr_begin(&w), jw_f64(&w, b[0]), jw_f64(&w, b[1]), jw_arr_end(&w);
    jw_key(&w, "kind"), jw_str(&w, KIND_NAME[s->placing_kind]);
    if (s->material) jw_key(&w, "material"), jw_str(&w, s->material);
    jw_key(&w, "sizes"), jw_obj_begin(&w);
    for (int i = 0; i < 5; i++) jw_key(&w, SIZE_KEY[SIZE_SORTED[i]]), jw_f64(&w, s->sizes[SIZE_SORTED[i]]);
    jw_obj_end(&w);
    jw_key(&w, "storey"), jw_str_n(&w, s->storey.c, 22);
    jw_obj_end(&w);
    jw_finish(&w);
}

/* edit.rs Editor::click: the commands of `verb place` for points a, b (a run, or a dry run for
 * the ghost), a slab raised by top_offset_mm. 0 ok, -1 with the error in `err`. */
static int place_cmds(cc_session *s, const double *a, const double *b, int dry_run, cc_cmds *l, cc_buf *err) {
    if (!s->has_storey) {
        buf_puts(err, "choose a storey");
        return -1;
    }
    Buf args;
    buf_init(&args);
    place_args(s, a, s->placing_kind == CC_KIND_COLUMN ? NULL : b, &args);
    int rc = args.oom ? (buf_puts(err, "out of memory"), -1)
                      : cc_verb_run_ex(&s->model, "place", args.p, args.len, dry_run, -1, s->rev, l, err) ? -1 : 0;
    buf_free(&args);
    if (!rc && s->placing_kind == CC_KIND_SLAB)
        for (size_t i = 0; i < l->n; i++)
            if (l->v[i].op == CC_CMD_ADD_ELEMENT) l->v[i].element.placement.origin[2] += s->top_offset_mm;
    return rc;
}

/* The tool's state: {"active","ghost","ghost_error","hover","kind","placed","points"} (sorted).
 * ghost: what the next click adds (tool.h cc_tool_ghost), null without one; ghost_error: why a
 * preview has no ghost (the place verb's error), else null; hover: {"at":[x,y],"snap":label or
 * null,"z":storey elevation} (world mm) or null; placed: the GlobalId a click just added, or null. */
static int tool_json(cc_session *s, const cc_gid *placed, cc_buf *out) {
    cc_tool *t = &s->tool;
    JsonW w;
    jw_init(&w, out, 0);
    jw_obj_begin(&w);
    jw_key(&w, "active"), jw_bool(&w, s->placing);
    Buf gerr;
    buf_init(&gerr);
    jw_key(&w, "ghost");
    int has_ghost = 0;
    if (s->placing && t->has_hover && !(s->placing_kind == CC_KIND_SLAB && s->polygon) &&
        (s->placing_kind == CC_KIND_COLUMN || t->npoints >= 1)) {
        cc_cmds l;
        cc_cmds_init(&l);
        const double *a = s->placing_kind == CC_KIND_COLUMN ? t->hover : t->points[0];
        if (!place_cmds(s, a, t->hover, 1, &l, &gerr)) {
            const cc_scene *sc = scene_now(s);
            double org[3] = {0, 0, 0};
            if (sc) memcpy(org, sc->origin, sizeof org);
            has_ghost = !cc_tool_ghost(&w, &s->model, l.v, l.n, org, &gerr);
        }
        cc_cmds_free(&l);
    }
    if (!has_ghost) jw_null(&w);
    jw_key(&w, "ghost_error");
    if (gerr.len) jw_str_n(&w, gerr.p, gerr.len);
    else jw_null(&w);
    buf_free(&gerr);
    jw_key(&w, "hover");
    if (s->placing && t->has_hover) {
        jw_obj_begin(&w);
        jw_key(&w, "at"), jw_arr_begin(&w), jw_f64(&w, t->hover[0]), jw_f64(&w, t->hover[1]), jw_arr_end(&w);
        jw_key(&w, "snap");
        if (cc_snap_label(t->hover_kind)) jw_str(&w, cc_snap_label(t->hover_kind));
        else jw_null(&w);
        jw_key(&w, "z"), jw_f64(&w, storey_level(s) + 0.0);
        jw_obj_end(&w);
    } else jw_null(&w);
    jw_key(&w, "kind"), jw_str(&w, KIND_NAME[s->placing ? s->placing_kind : s->kind]);
    jw_key(&w, "placed");
    if (placed) jw_str_n(&w, placed->c, 22);
    else jw_null(&w);
    jw_key(&w, "points"), jw_arr_begin(&w);
    for (size_t i = 0; s->placing && i < t->npoints; i++)
        jw_arr_begin(&w), jw_f64(&w, t->points[i][0]), jw_f64(&w, t->points[i][1]), jw_arr_end(&w);
    jw_arr_end(&w);
    jw_obj_end(&w);
    return done(&w);
}

/* ---- the annotate tool (ticket C4, calls.h "annotate"; annot.h) ---- */

/* The active view when it is a plan with an editable model: 1 (into `v`, built in `a`), else 0. */
static int annot_view(cc_session *s, Arena *a, cc_view *v) {
    if (!s->open || !s->tab_open) return 0;
    return cc_sv_active(s, a, v) == 1 && v->kind == CC_VIEW_PLAN && v->storey.n == 22;
}

/* The hover's feature anchor (annot.h cc_feature_near, within 1 mm on the plan's storey). */
static void annot_hover_anchor(cc_session *s) {
    cc_tool *t = &s->tool;
    t->annot_ft[2].has = 0;
    if (!t->has_hover || t->annot != 1) return;
    Arena a;
    arena_init(&a, 0);
    cc_view v;
    if (annot_view(s, &a, &v)) {
        cc_gid sid;
        memcpy(sid.c, v.storey.p, 22);
        double at[3];
        t->annot_ft[2].has = cc_feature_near(&s->model, t->hover, 1.0, &sid, &t->annot_ft[2].el, t->annot_ft[2].feat, at);
    }
    arena_free(&a);
}

/* f64::round, half away from zero. */
static double annot_round(double x) {
    double r = (double)(int64_t)(x + (x < 0 ? -0.5 : 0.5));
    return (x < 0 ? -x : x) < 4503599627370496.0 ? r + 0.0 : x;
}

static void w_pair(JsonW *w, const double *p) {
    jw_arr_begin(w), jw_f64(w, p[0] + 0.0), jw_f64(w, p[1] + 0.0), jw_arr_end(w);
}

/* The dimension a, b (offset `off`, `axis`) as a preview: {"a","axis","b","dir","mid","offset_mm","qa","qb","text","value_mm"}. */
static void annot_preview(JsonW *w, const double *a, const double *b, double off, cc_dim_axis axis) {
    static const char *const AXIS[] = {"aligned", "horizontal", "vertical"};
    double pa[3] = {a[0], a[1], 0}, pb[3] = {b[0], b[1], 0}, u[2], qa[2], qb[2], len;
    if (cc_dim_line(pa, pb, off, axis, u, qa, qb, &len)) {
        jw_null(w);
        return;
    }
    if (u[0] < 0.0 || (u[0] == 0.0 && u[1] < 0.0)) u[0] = -u[0], u[1] = -u[1];
    double mid[2] = {(qa[0] + qb[0]) / 2.0, (qa[1] + qb[1]) / 2.0};
    jw_obj_begin(w);
    jw_key(w, "a"), w_pair(w, a);
    jw_key(w, "axis"), jw_str(w, AXIS[axis]);
    jw_key(w, "b"), w_pair(w, b);
    jw_key(w, "dir"), w_pair(w, u);
    jw_key(w, "mid"), w_pair(w, mid);
    jw_key(w, "offset_mm"), jw_f64(w, off + 0.0);
    jw_key(w, "qa"), w_pair(w, qa);
    jw_key(w, "qb"), w_pair(w, qb);
    Buf t;
    buf_init(&t);
    cc_dim_format(&t, len, CC_UNIT_MM);
    jw_key(w, "text"), jw_str_n(w, t.oom ? "" : t.p, t.oom ? 0 : t.len);
    buf_free(&t);
    jw_key(w, "value_mm"), jw_f64(w, len);
    jw_obj_end(w);
}

static void annot_anchor_json(JsonW *w, const cc_tool *t, int k) {
    if (!t->annot_ft[k].has) {
        jw_null(w);
        return;
    }
    jw_obj_begin(w);
    jw_key(w, "element"), jw_str_n(w, t->annot_ft[k].el.c, 22);
    jw_key(w, "feature"), jw_str(w, t->annot_ft[k].feat);
    jw_obj_end(w);
}

/* The annotate tool's state, keys sorted:
 *   {"active","added","anchor","anchors","hover","mode","points","preview","text","view"}
 * added: the GlobalId a click just added, or null; anchor: the hover's element feature
 * ({"element","feature"}) or null; anchors: the clicked points' ones; hover: as the place tool's;
 * mode: "dimension" | "text" | null; preview: the dimension the next click makes (1 point: a to the
 * pointer, offset 0; 2 points: the third click's line) or null; view: the plan's id or null. */
static int annot_json(cc_session *s, const cc_gid *added, cc_buf *out) {
    cc_tool *t = &s->tool;
    JsonW w;
    jw_init(&w, out, 0);
    Arena a;
    arena_init(&a, 0);
    cc_view v;
    int on = t->annot && annot_view(s, &a, &v);
    jw_obj_begin(&w);
    jw_key(&w, "active"), jw_bool(&w, on);
    jw_key(&w, "added");
    if (added) jw_str_n(&w, added->c, 22);
    else jw_null(&w);
    jw_key(&w, "anchor"), annot_anchor_json(&w, t, 2);
    jw_key(&w, "anchors"), jw_arr_begin(&w);
    for (size_t i = 0; on && i < t->npoints && i < 2; i++) annot_anchor_json(&w, t, (int)i);
    jw_arr_end(&w);
    jw_key(&w, "hover");
    if (on && t->has_hover) {
        jw_obj_begin(&w);
        jw_key(&w, "at"), w_pair(&w, t->hover);
        jw_key(&w, "snap");
        if (cc_snap_label(t->hover_kind)) jw_str(&w, cc_snap_label(t->hover_kind));
        else jw_null(&w);
        jw_key(&w, "z"), jw_f64(&w, storey_level(s) + 0.0);
        jw_obj_end(&w);
    } else jw_null(&w);
    jw_key(&w, "mode");
    if (on) jw_str(&w, t->annot == 1 ? "dimension" : "text");
    else jw_null(&w);
    jw_key(&w, "points"), jw_arr_begin(&w);
    for (size_t i = 0; on && i < t->npoints; i++) w_pair(&w, t->points[i]);
    jw_arr_end(&w);
    jw_key(&w, "preview");
    if (on && t->annot == 1 && t->has_hover && t->npoints == 1) annot_preview(&w, t->points[0], t->hover, 0.0, CC_AXIS_ALIGNED);
    else if (on && t->annot == 1 && t->has_hover && t->npoints == 2) {
        cc_dim_axis ax;
        double off;
        cc_dim_axis_offset(t->points[0], t->points[1], t->hover, &ax, &off);
        annot_preview(&w, t->points[0], t->points[1], annot_round(off), ax);
    } else jw_null(&w);
    jw_key(&w, "text");
    if (on && t->annot == 2 && t->annot_text) jw_str(&w, t->annot_text);
    else jw_null(&w);
    jw_key(&w, "view");
    if (on) jw_str_n(&w, v.id.c, 22);
    else jw_null(&w);
    jw_obj_end(&w);
    arena_free(&a);
    return done(&w);
}

static void annot_point_arg(JsonW *w, const cc_tool *t, int k) {
    if (t->annot_ft[k].has) {
        jw_obj_begin(w);
        jw_key(w, "element"), jw_str_n(w, t->annot_ft[k].el.c, 22);
        jw_key(w, "feature"), jw_str(w, t->annot_ft[k].feat);
        jw_obj_end(w);
    } else w_pair(w, t->points[k]);
}

/* tool "annotate" {"mode": "dimension" | "text" | null, "text", "size_mm"}: starts (placing
 * stops) or stops the tool. */
static int annot_start(cc_session *s, const char *body, size_t n, cc_buf *out) {
    Arena a;
    arena_init(&a, 0);
    JrError je;
    const JrVal *v = jr_parse(&a, body, n, &je);
    int rc = CC_ERR, mode = 0;
    const char *bad = NULL;
    const JrVal *m = v && v->type == JR_OBJ ? jr_get(v, "mode") : NULL;
    const JrVal *tx = v && v->type == JR_OBJ ? jr_get(v, "text") : NULL;
    const JrVal *sz = v && v->type == JR_OBJ ? jr_get(v, "size_mm") : NULL;
    if (!v || v->type != JR_OBJ) bad = "annotate: expected {\"mode\": \"dimension\" | \"text\" | null, \"text\"}";
    else if (m && m->type == JR_STR && m->n == 9 && !memcmp(m->s, "dimension", 9)) mode = 1;
    else if (m && m->type == JR_STR && m->n == 4 && !memcmp(m->s, "text", 4)) mode = 2;
    else if (m && m->type != JR_NULL) bad = "mode: expected \"dimension\", \"text\" or null";
    if (!bad && mode == 2 && (!tx || tx->type != JR_STR || !tx->n)) bad = "text: the text mode needs a text";
    if (!bad && sz && sz->type != JR_NULL && (sz->type != JR_NUM || !(sz->num >= 0.5 && sz->num <= 50.0)))
        bad = "size_mm: expected 0.5 to 50";
    if (bad) {
        buf_puts(out, bad);
        goto end;
    }
    if (!s->open) {
        buf_puts(out, loaded(s) ? READ_ONLY : NO_MODEL);
        goto end;
    }
    cc_view pv;
    if (mode && !annot_view(s, &a, &pv)) {
        buf_puts(out, "dimensions and texts go in 2D views: open a plan");
        goto end;
    }
    cc_tool_clear(&s->tool);
    s->placing = 0;
    s->tool.annot = mode;
    s->tool.annot_size = sz && sz->type == JR_NUM ? sz->num : 3.5;
    if (mode == 2) {
        s->tool.annot_text = malloc(tx->n + 1);
        if (!s->tool.annot_text) {
            s->tool.annot = 0;
            buf_puts(out, "out of memory");
            goto end;
        }
        memcpy(s->tool.annot_text, tx->s, tx->n);
        s->tool.annot_text[tx->n] = 0;
    }
    rc = annot_json(s, NULL, out);
end:
    arena_free(&a);
    return rc;
}

/* pointer / click / cancel while the annotate tool is on. */
static int annot_call(cc_session *s, int pointer, int cancel, const ToolArgs *ta, cc_buf *out) {
    cc_tool *t = &s->tool;
    if (cancel) { /* Esc: the tool stops, the points go */
        cc_tool_clear(t);
        return annot_json(s, NULL, out);
    }
    Arena a;
    arena_init(&a, 0);
    cc_view v;
    int ok = annot_view(s, &a, &v);
    arena_free(&a);
    if (!ok) { /* the plan is no longer shown */
        cc_tool_clear(t);
        if (pointer) return annot_json(s, NULL, out);
        buf_puts(out, "dimensions and texts go in 2D views: open a plan");
        return CC_ERR;
    }
    tool_pointer(s, ta);
    annot_hover_anchor(s);
    if (pointer || !t->has_hover) return annot_json(s, NULL, out);
    Buf args;
    buf_init(&args);
    JsonW w;
    jw_init(&w, &args, 0);
    jw_obj_begin(&w);
    const char *verb;
    if (t->annot == 2) {
        verb = "add_text";
        jw_key(&w, "at"), w_pair(&w, t->hover);
        jw_key(&w, "size_mm"), jw_f64(&w, t->annot_size);
        jw_key(&w, "text"), jw_str(&w, t->annot_text ? t->annot_text : "");
    } else {
        if (t->npoints < 2) { /* a, then b (not on a) */
            size_t k = t->npoints;
            if (k == 1 && t->hover[0] == t->points[0][0] && t->hover[1] == t->points[0][1]) {
                jw_finish(&w);
                buf_free(&args);
                return annot_json(s, NULL, out);
            }
            t->points[k][0] = t->hover[0], t->points[k][1] = t->hover[1];
            t->annot_ft[k] = t->annot_ft[2];
            t->npoints++;
            jw_finish(&w);
            buf_free(&args);
            return annot_json(s, NULL, out);
        }
        verb = "add_dimension";
        cc_dim_axis ax;
        double off;
        cc_dim_axis_offset(t->points[0], t->points[1], t->hover, &ax, &off);
        static const char *const AXIS[] = {"aligned", "horizontal", "vertical"};
        jw_key(&w, "a"), annot_point_arg(&w, t, 0);
        jw_key(&w, "axis"), jw_str(&w, AXIS[ax]);
        jw_key(&w, "b"), annot_point_arg(&w, t, 1);
        jw_key(&w, "offset"), jw_f64(&w, annot_round(off));
    }
    jw_key(&w, "view"), jw_str_n(&w, v.id.c, 22);
    jw_obj_end(&w);
    jw_finish(&w);
    t->npoints = 0; /* the points are taken, also when the verb fails */
    cc_cmds l;
    cc_cmds_init(&l);
    int rc = CC_ERR;
    cc_gid id;
    int has_id = 0;
    if (args.oom) buf_puts(out, "out of memory");
    else if (!cc_verb_run_ex(&s->model, verb, args.p, args.len, 0, -1, s->rev, &l, out)) {
        for (size_t i = 0; i < l.n; i++)
            if ((l.v[i].op == CC_CMD_SET_VIEW || l.v[i].op == CC_CMD_ADD_VIEW) && l.v[i].view.nannotations)
                id = l.v[i].view.annotations[l.v[i].view.nannotations - 1].id, has_id = 1;
        rc = try_apply(s, l.v, l.n, out);
    }
    cc_cmds_free(&l);
    buf_free(&args);
    if (rc != CC_OK) return rc;
    if (t->annot == 2) cc_tool_clear(t); /* one text per start */
    return annot_json(s, has_id ? &id : NULL, out);
}

/* The measure tool (ticket T1, measure.h): the shown scene and its visible links, the box that
 * cuts (a plan's range, else the section box when on). */
static int measure_tool(cc_session *s, const char *name, const char *body, size_t n, cc_buf *out) {
    const cc_scene *sc = loaded(s) ? scene_now(s) : NULL;
    cc_mlayer layers[1 + CC_MAX_LINKS];
    size_t nl = 0;
    float mn[3], mx[3];
    int has_box = 0;
    if (sc) {
        layers[nl].sc = sc, layers[nl].vis = &s->vis, memset(layers[nl].off, 0, sizeof layers[nl].off), nl++;
        for (size_t i = 0; i < s->nlinks && nl < 1 + CC_MAX_LINKS; i++) { /* lib.rs snap_at `theirs` */
            const cc_scene *ls = s->links[i].visible ? cc_link_scene(&s->links[i]) : NULL;
            if (!ls) continue;
            layers[nl].sc = ls, layers[nl].vis = NULL;
            for (int k = 0; k < 3; k++) layers[nl].off[k] = (float)(ls->origin[k] - sc->origin[k]); /* lib.rs:1499 */
            nl++;
        }
        Arena a;
        arena_init(&a, 0);
        cc_view v;
        double z[4];
        if (s->tab_open && cc_sv_active(s, &a, &v) == 1 && v.kind == CC_VIEW_PLAN && !cc_sv_resolve(s->open ? &s->model : NULL, &v, z)) {
            cc_vis box;
            cc_vis_init(&box);
            plan_box(sc, &v, z, &box);
            if (box.has_box) has_box = 1, memcpy(mn, box.min, sizeof mn), memcpy(mx, box.max, sizeof mx);
            cc_vis_free(&box);
        } else if (s->vis.section_on && s->vis.has_box) {
            has_box = 1, memcpy(mn, s->vis.min, sizeof mn), memcpy(mx, s->vis.max, sizeof mx);
        }
        arena_free(&a);
    }
    cc_mview mv = {layers, nl, has_box ? mn : NULL, has_box ? mx : NULL, sc ? sc->origin : NULL};
    return cc_measure_call(&s->measure, name, body, n, sc ? &mv : NULL, out) == 200 ? CC_OK : CC_ERR;
}

/* op "tool" (calls.h). */
static int call_tool(cc_session *s, const char *name, const char *body, size_t n, cc_buf *out) {
    if (cc_measure_name(name)) return measure_tool(s, name, body, n, out); /* ticket T1 */
    if (cc_vis_tool_name(name)) { /* the section box's face drag (visibility.h): any scene shown */
        if (!loaded(s)) {
            buf_puts(out, NO_MODEL);
            return CC_ERR;
        }
        return cc_vis_tool(&s->vis, name, body, n, out) == 200 ? CC_OK : CC_ERR;
    }
    if (!strcmp(name, "annotate")) return annot_start(s, body, n, out);
    int pointer = !strcmp(name, "pointer"), click = !strcmp(name, "click"), cancel = !strcmp(name, "cancel");
    if (!pointer && !click && !cancel) {
        buf_puts(out, "unknown tool call ");
        debug_str(out, name);
        return CC_ERR;
    }
    ToolArgs a;
    if (tool_args(body, n, &a, out)) return CC_ERR;
    if (!s->open) {
        buf_puts(out, READ_ONLY);
        return CC_ERR;
    }
    if (s->tool.annot) return annot_call(s, pointer, cancel, &a, out);
    if (cancel) { /* lib.rs:3031 Esc: stops placing, the points go */
        s->placing = 0;
        cc_tool_clear(&s->tool);
        return tool_json(s, NULL, out);
    }
    if (!s->placing) {
        if (pointer) return tool_json(s, NULL, out);
        buf_puts(out, "nothing is being placed");
        return CC_ERR;
    }
    tool_pointer(s, &a);
    if (pointer || !s->tool.has_hover) return tool_json(s, NULL, out);
    if (s->placing_kind == CC_KIND_SLAB && s->polygon) {
        buf_puts(out, "a polygon slab is not placed by clicks yet: use the rectangle shape or the place verb");
        return CC_ERR;
    }
    cc_tool *t = &s->tool;
    if (t->npoints >= CC_TOOL_MAX_POINTS) t->npoints = 0;
    t->points[t->npoints][0] = t->hover[0], t->points[t->npoints][1] = t->hover[1], t->npoints++;
    if (t->npoints < (s->placing_kind == CC_KIND_COLUMN ? 1u : 2u)) return tool_json(s, NULL, out);
    double pa[2] = {t->points[0][0], t->points[0][1]}, pb[2] = {t->points[t->npoints - 1][0], t->points[t->npoints - 1][1]};
    t->npoints = 0; /* edit.rs: the points are taken, also when the element fails */
    cc_cmds l;
    cc_cmds_init(&l);
    int rc = CC_ERR;
    cc_gid id;
    int has_id = 0;
    if (!place_cmds(s, pa, pb, 0, &l, out)) {
        for (size_t i = 0; i < l.n; i++)
            if (l.v[i].op == CC_CMD_ADD_ELEMENT) id = l.v[i].element.id, has_id = 1;
        rc = try_apply(s, l.v, l.n, out);
    }
    cc_cmds_free(&l);
    if (rc != CC_OK) return rc;
    return tool_json(s, has_id ? &id : NULL, out);
}

int cc_call(cc_session *s, const char *op, const char *name, const char *body, size_t n, cc_buf *out) {
    if (cc_view_only_refuses(op, name, out)) return CC_ERR; /* view build only (view_only.h) */
    if (!strcmp(op, "query")) return call_query(s, name, body, n, out);
    if (!strcmp(op, "action")) return call_action(s, name, body, n, out);
    if (!strcmp(op, "select")) return call_select(s, body, n, out);
    if (!strcmp(op, "tab")) return call_tab(s, body, n, out);
    if (!strcmp(op, "set_placing")) return call_set_placing(s, body, n, out);
    if (!strcmp(op, "dismiss_belongs")) return CC_OK; /* calls.rs:108-114: nobody borrows here */
    if (!strcmp(op, "standards_action")) { /* calls.rs:172-187; spec §1: no-op */
        if (!strcmp(name, "revert") || !strcmp(name, "reset") || !strcmp(name, "save")) return CC_OK;
        buf_puts(out, "unknown standards action ");
        debug_str(out, name);
        return CC_ERR;
    }
    if (!strcmp(op, "undo") || !strcmp(op, "redo")) { /* calls.rs:89-93 */
        if (!s->open) {
            buf_puts(out, READ_ONLY);
            return CC_ERR;
        }
        undo_redo(s, !strcmp(op, "undo"));
        return CC_OK;
    }
    if (!strcmp(op, "apply")) { /* calls.rs:84-88 */
        cc_cmds l;
        cc_cmds_init(&l);
        int rc = cc_commands_parse(body, n, &l, out) ? CC_ERR : try_apply(s, l.v, l.n, out);
        cc_cmds_free(&l);
        return rc;
    }
    if (!strcmp(op, "verb")) return call_verb(s, name, body, n, out);
    if (!strcmp(op, "tool")) return call_tool(s, name, body, n, out);
    buf_puts(out, "unknown op "); /* the router checked cc_known_op */
    buf_puts(out, op);
    return CC_ERR;
}

void cc_change_json(cc_session *s, cc_buf *out) { /* calls.rs:134-167 (json!, sorted keys) */
    JsonW w;
    jw_init(&w, out, 0);
    jw_obj_begin(&w);
    jw_key(&w, "file");
    if (loaded(s)) jw_str(&w, s->file);
    else jw_null(&w);
    int linked = loaded(s) && s->link_sel >= 0 && (size_t)s->link_sel < s->nlinks;
    if (linked) { /* links.h: only while a linked element is selected */
        jw_key(&w, "linked"), jw_obj_begin(&w);
        jw_key(&w, "id"), jw_str_n(&w, s->link_sel_id, 22);
        jw_key(&w, "link"), jw_str(&w, s->links[s->link_sel].name);
        jw_obj_end(&w);
    }
    if (s->links_gen) jw_key(&w, "links"), jw_u64(&w, s->links_gen); /* links.h: once links changed */
    jw_key(&w, "placing");
    if (s->open) placing_json(s, &w);
    else jw_null(&w);
    jw_key(&w, "rev"), jw_u64(&w, s->rev);
    jw_key(&w, "selection"), jw_arr_begin(&w);
    if (linked) jw_str_n(&w, s->link_sel_id, 22);
    if (loaded(s) && s->has_primary) jw_str_n(&w, s->primary.c, 22);
    for (size_t i = 0; loaded(s) && i < s->nalso; i++) jw_str_n(&w, s->also[i].c, 22);
    jw_arr_end(&w);
    jw_key(&w, "tab");
    if (cc_sv_sync(s)) w.b->oom = 1;
    if (loaded(s) && s->tab_open && s->has_active) tab_json(&w, s);
    else jw_null(&w);
    jw_key(&w, "unsaved"), jw_bool(&w, cc_session_unsaved(s));
    jw_key(&w, "view"), jw_arr_begin(&w);
    { /* calls.rs:152-161 */
        const cc_vis *v = &s->vis;
        int l = loaded(s);
        jw_u64(&w, l && v->section_on);
        jw_u64(&w, l && v->has_temp ? v->temp.n : 0);
        jw_u64(&w, l && v->has_temp && v->temp_isolate);
        jw_u64(&w, l ? v->hidden.n : 0);
        jw_u64(&w, l ? v->isolated.n : 0);
        jw_u64(&w, l && cc_vis_hidden_in_view(v));
        jw_u64(&w, 0); /* on_paper: the C core has no On paper picture */
    }
    jw_u64(&w, loaded(s) ? s->ntabs : 0); /* open_tabs.len() */
    jw_arr_end(&w);
    jw_obj_end(&w);
    jw_finish(&w);
}
