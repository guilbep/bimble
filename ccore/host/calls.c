/* The calls of the native host, answered from the open model (session.h): `Viewer::call` of
 * crates/bimble-viewer/src/calls.rs. Without a model (the host started without a file) the
 * answers are those of a viewer with nothing loaded.
 *
 * Key order: every object here is a `serde_json::json!` map in Rust, and preserve_order is off
 * (spec §3, "Canonical JSON"), so keys are written in byte order. Each producer is cited.
 * Deviations from Rust: docs/research/c-core-spec.md §1 (version, IFC always imported) and the
 * ones marked "Deviation" below. */
#define _POSIX_C_SOURCE 200809L
#ifdef __wasm__
#include "wasm_api.h" /* the few libc functions below, freestanding */
#else
#include <stdio.h>
#endif
#include <stdlib.h>
#include <string.h>

#include "calls.h"
#include "file.h"
#include "guid.h"
#include "json_r.h"
#include "json_w.h"
#include "library.h"
#include "query.h"
#include "scene.h"
#include "session.h"
#include "verbs.h"

#define CC_VERSION "0.1.0-ccore" /* `version` is the C core's own (spec §1, deviations) */

static const char READ_ONLY[] = "this file is read only: import it to edit"; /* viewer/lib.rs:65 */
static const char NO_MODEL[] = "no model open";                              /* calls.rs:552 */
#define CC_SIZE_MIN 10.0     /* verbs.rs:12 SIZE_MM */
#define CC_SIZE_MAX 100000.0

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
 * shortcut (`keys`, ribbon.rs:319). `pressed`: `action_on` (ribbon.rs:616) with the defaults: edges
 * and hatching on (bimble-render/src/lib.rs:1431-1432), the four plan annotations on
 * (bimble-drawing/src/template.rs:222-226), place_<kind> while placing that kind, else off.
 * `enabled`: action_enabled (ribbon.rs:555-612) for the actions the C core runs (save, undo, redo,
 * place_*, home); every other one is off (spec §1). Deviation: export_ifc4 is off, the C core has
 * no IFC writer yet. */
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

static cc_session *ses(void) { return cc_session_get(); }
static int has_model(void) { return ses()->open; }

/* ---- the 3D view (core/view.rs:584) ---- */

static cc_guid view3d(void) {
    static cc_guid g;
    static int made;
    if (!made) g = cc_guid_derived("bimble view 3d", 14), made = 1;
    return g;
}

static void tab_json(JsonW *w) { /* ribbon.rs:406 json!({"type": "view", "id": id}), sorted */
    cc_guid g = view3d();
    jw_obj_begin(w);
    jw_key(w, "id"), jw_str_n(w, g.c, 22);
    jw_key(w, "type"), jw_str(w, "view");
    jw_obj_end(w);
}

/* ---- the scene, built once per rev (status counts, visibility origin) ---- */

static cc_scene g_scene;
static int g_scene_ok;
static uint64_t g_scene_rev; /* 0: none built */

static const cc_scene *scene_now(void) {
    cc_session *s = ses();
    if (!s->open) return NULL;
    if (g_scene_rev != s->rev) {
        if (g_scene_rev) cc_scene_free(&g_scene);
        cc_error e;
        g_scene_ok = cc_scene_build(&g_scene, &s->model, &e) == 0;
        if (!g_scene_ok) fprintf(stderr, "bimble-ccore: scene: %s\n", e.msg);
        g_scene_rev = s->rev;
    }
    return g_scene_ok ? &g_scene : NULL;
}

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
static void rebuild(void) {
    cc_session *s = ses();
    s->rev++;
    if (s->has_primary && !cc_model_element(&s->model, &s->primary)) s->has_primary = 0;
    s->nalso = 0;
}

/* lib.rs:1725 `try_apply` through edit.rs:233 `Editor::apply` (no commands: no step, but the
 * scene is rebuilt). */
static int try_apply(const cc_command *cmds, size_t n, cc_buf *out) {
    cc_session *s = ses();
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
    }
    rebuild();
    return CC_OK;
}

/* lib.rs:1744 `undo_redo`: nothing to take back is no change. */
static void undo_redo(int undo) {
    cc_session *s = ses();
    if (!s->open) return;
    if (undo ? cc_history_undo(&s->hist, &s->model) : cc_history_redo(&s->hist, &s->model)) {
        if (undo && s->nundo_ids) {
            if (!push_id(&s->redo_ids, &s->nredo_ids, &s->cap_redo_ids, s->undo_ids[s->nundo_ids - 1])) s->nundo_ids--;
        } else if (!undo && s->nredo_ids) {
            if (!push_id(&s->undo_ids, &s->nundo_ids, &s->cap_undo_ids, s->redo_ids[s->nredo_ids - 1])) s->nredo_ids--;
        }
        rebuild();
    }
}

/* ---- placing (edit.rs:387-460) ---- */

static const char *hint(const cc_session *s) { /* edit.rs:84-95 `Placing::hint`, no points */
    if (s->placing_kind == CC_KIND_SLAB && s->polygon) return "Click the next corner of the slab.";
    if (s->placing_kind == CC_KIND_COLUMN) return "Click where the column goes (on the storey level). Esc stops.";
    if (s->placing_kind == CC_KIND_SLAB) return "Click a corner of the slab.";
    return "Click the start point.";
}

static void placing_json(JsonW *w) { /* edit.rs:387-408, json! sorted */
    cc_session *s = ses();
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
    jw_key(w, "points"), jw_arr_begin(w), jw_arr_end(w); /* no clicks reach the host */
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
static void place_action(cc_kind k) {
    cc_session *s = ses();
    int on = s->placing && s->placing_kind == k;
    s->placing = !on;
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
static int call_set_placing(const char *body, size_t n, cc_buf *out) {
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
    cc_session *s = ses();
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
    if (has_polygon && polygon != s->polygon) s->polygon = polygon; /* points cleared: none here */
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
        if (s->placing) s->placing_kind = k; /* a new kind drops the points */
        else place_action(k);                /* calls.rs:198: `place_<kind>`, enabled with a model */
    }
    rc = CC_OK;
end:
    arena_free(&a);
    return rc;
}

/* ---- queries ---- */

static int q_state(JsonW *w) { /* calls.rs:239-258 */
    cc_session *s = ses();
    jw_obj_begin(w);
    jw_key(w, "editable"), jw_bool(w, s->open);
    jw_key(w, "file");
    if (s->open) jw_str(w, s->file);
    else jw_null(w);
    jw_key(w, "schema");
    if (s->open && s->schema_bimble) jw_str(w, ".bimble");
    else jw_null(w); /* a model's scene has no IFC schema */
    jw_key(w, "unsaved"), jw_bool(w, cc_session_unsaved(s));
    jw_key(w, "user"), jw_null(w);
    jw_key(w, "version"), jw_str(w, CC_VERSION);
    jw_obj_end(w);
    return done(w);
}

static int action_enabled(const char *name) { /* ribbon.rs:555-612, the C core's subset */
    cc_session *s = ses();
    if (!strcmp(name, "save")) return cc_session_unsaved(s);
    if (!strcmp(name, "undo")) return s->open && s->hist.nundo > 0;
    if (!strcmp(name, "redo")) return s->open && s->hist.nredo > 0;
    if (!strncmp(name, "place_", 6)) return s->open;
    if (!strcmp(name, "home")) return 1;
    return 0;
}

static int action_pressed(const ActionRow *a) {
    cc_session *s = ses();
    if (!strncmp(a->name, "place_", 6)) return s->open && s->placing && !strcmp(a->name + 6, KIND_NAME[s->placing_kind]);
    return a->pressed;
}

static int q_actions(JsonW *w) { /* calls.rs:325-339 */
    jw_arr_begin(w);
    for (size_t i = 0; i < sizeof ACTIONS / sizeof ACTIONS[0]; i++) {
        const ActionRow *a = &ACTIONS[i];
        jw_obj_begin(w);
        jw_key(w, "enabled"), jw_bool(w, action_enabled(a->name));
        jw_key(w, "label"), jw_str(w, a->label);
        jw_key(w, "name"), jw_str(w, a->name);
        jw_key(w, "pressed"), jw_bool(w, action_pressed(a));
        jw_key(w, "shortcut");
        if (a->shortcut) jw_str(w, a->shortcut);
        else jw_null(w);
        jw_key(w, "tab"), jw_str(w, a->tab);
        jw_obj_end(w);
    }
    jw_arr_end(w);
    return done(w);
}

static void capped_empty(JsonW *w) { /* calls.rs:57-63 `capped` */
    jw_obj_begin(w);
    jw_key(w, "count"), jw_u64(w, 0);
    jw_key(w, "ids"), jw_arr_begin(w), jw_arr_end(w);
    jw_obj_end(w);
}

static int q_visibility(JsonW *w) { /* calls.rs:442-460: nothing hidden, no section box */
    const cc_scene *sc = scene_now();
    jw_obj_begin(w);
    jw_key(w, "hidden_in_view"), capped_empty(w);
    jw_key(w, "hidden_now"), capped_empty(w);
    jw_key(w, "isolated_in_view"), capped_empty(w);
    jw_key(w, "isolated_now"), capped_empty(w);
    jw_key(w, "section_box");
    jw_obj_begin(w);
    jw_key(w, "box"), jw_null(w);
    jw_key(w, "on"), jw_bool(w, 0);
    jw_key(w, "origin_mm");
    if (sc) {
        jw_arr_begin(w);
        for (int i = 0; i < 3; i++) jw_f64(w, sc->origin[i]);
        jw_arr_end(w);
    } else jw_null(w);
    jw_obj_end(w);
    jw_obj_end(w);
    return done(w);
}

/* calls.rs:362-397. One view, the default 3D one (core/view.rs:584-594), its tab open and current
 * while `tab_open` (spec §1: no plans). A ViewSummary through to_value is a sorted map
 * (api/query.rs:164-178, `type` flattened), plus `auto_name` (viewing.rs:17-38). */
static int q_open_views(JsonW *w) {
    cc_session *s = ses();
    int tab = s->open && s->tab_open;
    cc_guid g = view3d();
    jw_obj_begin(w);
    jw_key(w, "active");
    if (s->open) jw_str_n(w, g.c, 22);
    else jw_null(w);
    jw_key(w, "current");
    if (tab) tab_json(w);
    else jw_null(w);
    jw_key(w, "paper");
    jw_obj_begin(w);
    jw_key(w, "offered"), jw_bool(w, 0);
    jw_key(w, "on"), jw_bool(w, 0);
    jw_obj_end(w);
    jw_key(w, "tabs"), jw_arr_begin(w);
    if (tab) tab_json(w);
    jw_arr_end(w);
    jw_key(w, "views"), jw_arr_begin(w);
    if (s->open) {
        jw_obj_begin(w);
        jw_key(w, "annotations"), jw_u64(w, 0);
        jw_key(w, "auto_name"), jw_obj_begin(w);
        jw_key(w, "kind"), jw_str(w, "3d");
        jw_key(w, "storey"), jw_null(w);
        jw_obj_end(w);
        jw_key(w, "crop"), jw_null(w);
        jw_key(w, "id"), jw_str_n(w, g.c, 22);
        jw_key(w, "name"), jw_str(w, "3D");
        jw_key(w, "references"), jw_u64(w, 0);
        jw_key(w, "scale"), jw_u64(w, 100);
        jw_key(w, "true_north"), jw_bool(w, 0);
        jw_key(w, "type"), jw_str(w, "three_d");
        jw_obj_end(w);
    }
    jw_arr_end(w);
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
static int q_status(JsonW *w, cc_buf *out) {
    cc_session *s = ses();
    if (!s->open) {
        buf_puts(out, "null");
        return CC_OK;
    }
    const cc_scene *sc = scene_now();
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
static int model_query(const char *name, const char *body, size_t n, cc_buf *out) {
    int st = cc_query(&ses()->model, name, body, n, out);
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

static int call_query(const char *name, const char *body, size_t n, cc_buf *out) {
    cc_session *s = ses();
    JsonW w;
    jw_init(&w, out, 0);
    if (!strcmp(name, "state")) return q_state(&w);
    if (!strcmp(name, "actions")) return q_actions(&w);
    if (!strcmp(name, "history")) { /* calls.rs:346-359 */
        if (s->open) {
            cc_history_json(&s->hist, out);
            return out->oom ? CC_ERR : CC_OK;
        }
        buf_puts(out, "{\"redo\":[],\"undo\":[]}");
        return CC_OK;
    }
    if (!strcmp(name, "visibility")) return q_visibility(&w);
    if (!strcmp(name, "open_views")) return q_open_views(&w);
    if (!strcmp(name, "collab")) return q_collab(&w);
    if (!strcmp(name, "status")) return q_status(&w, out);
    if (!strcmp(name, "placing")) { /* calls.rs:408-414: null without an editable model */
        if (!s->open) {
            buf_puts(out, "null");
            return CC_OK;
        }
        placing_json(&w);
        return done(&w);
    }
    /* pointer (calls.rs:403, stubbed null, spec §1); standards stubbed null (spec §1). */
    if (!strcmp(name, "pointer") || !strcmp(name, "standards")) {
        buf_puts(out, "null");
        return CC_OK;
    }
    static const char *const EMPTY[] = {"recent_files", "links", "measurements", "schedule_list", "recent_actions"};
    for (size_t i = 0; i < sizeof EMPTY / sizeof EMPTY[0]; i++)
        if (!strcmp(name, EMPTY[i])) { /* spec §1: stub [] */
            buf_puts(out, "[]");
            return CC_OK;
        }
    static const char *const CUT[] = {"palette_match", "verbs", "describe", "schedule_rows", "schedule_csv",
                                      "sheet_svg"};
    for (size_t i = 0; i < sizeof CUT / sizeof CUT[0]; i++)
        if (!strcmp(name, CUT[i])) {
            buf_puts(out, "query "), buf_puts(out, name), buf_puts(out, " is not in the C core");
            return CC_ERR;
        }
    if (s->open) return model_query(name, body, n, out); /* calls.rs:276-309 and :519-527 */
    static const char *const EMPTY_NO_MODEL[] = {"sheets", "views", "schedules"};
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

/* ---- save (lib.rs:1810-1857) ---- */

static void save(void) {
    cc_session *s = ses();
    Buf b;
    buf_init(&b);
    int ok = cc_model_save(&s->model, &b) == 0 && !b.oom;
#ifdef __wasm__
    /* No files in the module: the saved text is kept for the page (export host_saved). */
    if (ok) {
        buf_clear(&cc_wasm_saved);
        cc_wasm_saved.oom = 0;
        buf_append(&cc_wasm_saved, b.p, b.len);
        ok = !cc_wasm_saved.oom;
        cc_wasm_has_saved = ok;
    }
#else
    if (ok) {
        char tmp[4096 + 16];
        snprintf(tmp, sizeof tmp, "%s.tmp", s->path);
        FILE *f = fopen(tmp, "wb");
        ok = f && fwrite(b.p, 1, b.len, f) == b.len;
        if (f && fclose(f)) ok = 0;
        if (ok) ok = rename(tmp, s->path) == 0;
        else remove(tmp);
    }
#endif
    buf_free(&b);
    /* Rust shows the outcome in the editor's message, the call itself succeeds. */
    if (ok) s->saved_step = cc_session_step(s), fprintf(stderr, "bimble-ccore: Saved %s\n", s->path);
    else fprintf(stderr, "bimble-ccore: %s: cannot write\n", s->path);
}

/* calls.rs:538-547 call_action, ribbon.rs:650 run_action. */
static int call_action(const char *name, cc_buf *out) {
    const ActionRow *a = NULL;
    for (size_t i = 0; i < sizeof ACTIONS / sizeof ACTIONS[0]; i++)
        if (!strcmp(ACTIONS[i].name, name)) a = &ACTIONS[i];
    if (!a) {
        buf_puts(out, "unknown action ");
        debug_str(out, name);
        return CC_ERR;
    }
    if (!action_enabled(name)) {
        buf_puts(out, "action ");
        debug_str(out, name);
        buf_puts(out, " is not enabled now");
        return CC_ERR;
    }
    if (!strcmp(name, "save")) save();
    else if (!strcmp(name, "undo")) undo_redo(1);
    else if (!strcmp(name, "redo")) undo_redo(0);
    else if (!strncmp(name, "place_", 6)) {
        for (int k = 0; k < 4; k++)
            if (!strcmp(name + 6, KIND_NAME[k])) place_action((cc_kind)k);
    } /* home: the lab shows its own Home */
    return CC_OK;
}

/* ---- select (calls.rs:550-572) ---- */

static int gid_cmp(const void *a, const void *b) { return cc_gid_cmp(a, b); }

static int call_select(const char *body, size_t n, cc_buf *out) {
    Arena a;
    arena_init(&a, 0);
    JrError je;
    int rc = CC_ERR;
    const JrVal *v = jr_parse(&a, body, n, &je);
    if (!v) {
        buf_puts(out, je.msg);
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
    cc_session *s = ses();
    if (!s->open) {
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
            found = cc_model_element(&s->model, &ids[i]) != NULL;
        }
        if (!found) {
            buf_puts(out, unknown++ ? ", " : "unknown GlobalIds: ");
            buf_append(out, x->s, x->n);
        }
    }
    if (unknown) goto end;
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

static int call_tab(const char *body, size_t n, cc_buf *out) {
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
    int is_view = 0, is_3d = 0;
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
        cc_guid d = view3d();
        is_3d = is_view && !memcmp(g.c, d.c, 22);
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
    cc_session *s = ses();
    int has3d = s->open && is_3d; /* the only view (spec §1) */
    if (!strcmp(op->s, "open")) {
        if (!is_view) {
            buf_puts(out, "op \"open\" takes a view");
            goto end;
        }
        if (!has3d) {
            buf_puts(out, "no such view");
            goto end;
        }
    } else if (!close && !(has3d && s->tab_open)) {
        buf_puts(out, "no such open tab");
        goto end;
    }
    if (has3d) s->tab_open = !close; /* ribbon.rs:1484 tab_verb; other tabs: none open */
    rc = CC_OK;
end:
    arena_free(&a);
    return rc;
}

/* ---- the dispatcher (calls.rs:74-130) ---- */

int cc_call(const char *op, const char *name, const char *body, size_t n, cc_buf *out) {
    cc_session *s = ses();
    if (!strcmp(op, "query")) return call_query(name, body, n, out);
    if (!strcmp(op, "action")) return call_action(name, out);
    if (!strcmp(op, "select")) return call_select(body, n, out);
    if (!strcmp(op, "tab")) return call_tab(body, n, out);
    if (!strcmp(op, "set_placing")) return call_set_placing(body, n, out);
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
        undo_redo(!strcmp(op, "undo"));
        return CC_OK;
    }
    if (!strcmp(op, "apply")) { /* calls.rs:84-88 */
        cc_cmds l;
        cc_cmds_init(&l);
        int rc = cc_commands_parse(body, n, &l, out) ? CC_ERR : try_apply(l.v, l.n, out);
        cc_cmds_free(&l);
        return rc;
    }
    if (!strcmp(op, "verb")) { /* calls.rs:512-522 call_verb */
        int blank = 1; /* args_json.trim().is_empty(): null */
        for (size_t i = 0; i < n; i++)
            if (body[i] != ' ' && body[i] != '\t' && body[i] != '\n' && body[i] != '\r') blank = 0;
        if (!blank) { /* serde_json::from_str first: a body that does not parse fails before the model */
            Arena a;
            arena_init(&a, 0);
            JrError je;
            int ok = jr_parse(&a, body, n, &je) != NULL;
            if (!ok) buf_puts(out, je.msg);
            arena_free(&a);
            if (!ok) return CC_ERR;
        }
        if (!s->open) {
            buf_puts(out, READ_ONLY);
            return CC_ERR;
        }
        cc_cmds l;
        cc_cmds_init(&l);
        int rc = cc_verb_run(&s->model, name, body, blank ? 0 : n, &l, out) ? CC_ERR : try_apply(l.v, l.n, out);
        cc_cmds_free(&l);
        return rc;
    }
    buf_puts(out, "unknown op "); /* the router checked cc_known_op */
    buf_puts(out, op);
    return CC_ERR;
}

void cc_change_json(cc_buf *out) { /* calls.rs:134-167 (json!, sorted keys) */
    cc_session *s = ses();
    JsonW w;
    jw_init(&w, out, 0);
    jw_obj_begin(&w);
    jw_key(&w, "file");
    if (s->open) jw_str(&w, s->file);
    else jw_null(&w);
    jw_key(&w, "placing");
    if (s->open) placing_json(&w);
    else jw_null(&w);
    jw_key(&w, "rev"), jw_u64(&w, s->rev);
    jw_key(&w, "selection"), jw_arr_begin(&w);
    if (s->open && s->has_primary) jw_str_n(&w, s->primary.c, 22);
    for (size_t i = 0; s->open && i < s->nalso; i++) jw_str_n(&w, s->also[i].c, 22);
    jw_arr_end(&w);
    jw_key(&w, "tab");
    if (s->open && s->tab_open) tab_json(&w);
    else jw_null(&w);
    jw_key(&w, "unsaved"), jw_bool(&w, cc_session_unsaved(s));
    jw_key(&w, "view"), jw_arr_begin(&w);
    for (int i = 0; i < 7; i++) jw_u64(&w, 0); /* nothing hidden, no section box, not on paper */
    jw_u64(&w, s->open && s->tab_open ? 1 : 0); /* open_tabs.len() */
    jw_arr_end(&w);
    jw_obj_end(&w);
    jw_finish(&w);
}
