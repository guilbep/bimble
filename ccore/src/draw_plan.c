/* plan.rs `plan()` and template.rs of bimble-drawing (draw_plan.h). */
#pragma STDC FP_CONTRACT OFF /* a*b+c rounds twice, as in Rust */
#include "draw_plan.h"
#include <stdlib.h>
#include <string.h>
#include "draw_annot.h"
#include "draw_cut.h"
#include "draw_view.h"
#include "fmt.h"
#include "georef.h"
#include "ifc_out.h" /* CC_IFC_VERSION */
#include "json_w.h"
#include "math.h"

const uint32_t CC_STANDARD_SCALES[CC_NSTANDARD_SCALES] = {1,   2,   5,   10,   20,   25,   50,   75,   100,
                                                          125, 200, 250, 500, 1000, 1250, 2000, 2500, 5000};

/* Sheet margin around the frame, gap between the drawing and the frame or title block, grid
 * bubble diameter, distance between dimension chains, room under the drawing for its title. */
#define MARGIN 10.0
#define PAD 5.0
#define BUBBLE 8.0
#define DIM_STEP 7.0
#define TITLE_BAND 11.0
#define LEGEND_ROWS 4
#define LEGEND_ROW 6.0
#define LEGEND_COL 62.0

static double fmax_rs(double a, double b) { return a != a ? b : b != b ? a : a > b ? a : b; }
static double fmin_rs(double a, double b) { return a != a ? b : b != b ? a : a < b ? a : b; }
static double fabs_(double x) { return x < 0 ? -x : x; }

static cc_str cstr(const char *s) {
    cc_str r = {s, strlen(s)};
    return r;
}

/* memchr (not in the freestanding libc). */
static const char *find(const char *s, size_t n, char c) {
    for (size_t i = 0; i < n; i++)
        if (s[i] == c) return s + i;
    return NULL;
}

static int str_eq(cc_str a, cc_str b) { return a.n == b.n && (!a.n || !memcmp(a.p, b.p, a.n)); }

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }

static int eq_ignore_ascii_case(const char *a, size_t na, const char *b, size_t nb) {
    if (na != nb) return 0;
    for (size_t i = 0; i < na; i++)
        if (lower(a[i]) != lower(b[i])) return 0;
    return 1;
}

static int dup_str(Arena *a, cc_str s, cc_str *out) {
    char *p = arena_alloc(a, s.n + 1, 1);
    if (!p) return -1;
    if (s.n) memcpy(p, s.p, s.n);
    out->p = p;
    out->n = s.n;
    return 0;
}

/* ---- template.rs ---- */

static const char *const HIDDEN[8] = {"IfcReinforcingBar", "IfcReinforcingMesh", "IfcTendon",  "IfcTendonAnchor",
                                      "IfcSpace",          "IfcOpeningElement",  "IfcAnnotation", "IfcGeographicElement"};

int cc_plan_template_default(cc_plan_template *t, Arena *a) {
    memset(t, 0, sizeof *t);
    t->title = cstr("Plan {storey}, {scale}");
    t->level_note = cstr("{storey}: {level} m");
    t->dimension_unit = CC_TPL_CM;
    t->nhidden = 8;
    t->hidden_classes = arena_alloc(a, 8 * sizeof *t->hidden_classes, _Alignof(cc_str));
    if (!t->hidden_classes) return -1;
    for (int i = 0; i < 8; i++) t->hidden_classes[i] = cstr(HIDDEN[i]);
    t->seen_fills = 1;
    t->axes = CC_AXES_MODEL_OR_COLUMNS;
    t->dimensions = t->slab_levels = t->opening_crosses = t->opening_dimensions = t->element_labels =
        t->section_markers = 1;
    t->legend.show = 1;
    t->legend.title = cstr("Legend");
    t->legend.cut = cstr("{family}, cut");
    t->legend.seen = cstr("{family}, seen");
    t->legend.above = cstr("Above the cut");
    static const struct {
        double w; /* 0: none */
        int n;
        const char *e[2][2];
    } COLS[6] = {
        {0.0, 2, {{"PROJECT", "{project} {project_number}"}, {"CLIENT", "{client}"}}},
        {60.0, 1, {{"DRAWING", "{drawing}"}, {0, 0}}},
        {30.0, 2, {{"SCALE", "{scale}"}, {"FORMAT", "{paper}"}}},
        {30.0, 2, {{"DATE", "{date}"}, {"REVISION", "{revision}"}}},
        {24.0, 2, {{"DRAWN", "{drawn}"}, {"CHECKED", "{checked}"}}},
        {40.0, 1, {{"DRAWING N\xc2\xb0", "{drawing_no}"}, {0, 0}}},
    };
    t->title_block.height_mm = 28.0;
    t->title_block.ncolumns = 6;
    t->title_block.columns = arena_alloc(a, 6 * sizeof(cc_tb_column), _Alignof(cc_tb_column));
    if (!t->title_block.columns) return -1;
    for (int c = 0; c < 6; c++) {
        cc_tb_column *col = &t->title_block.columns[c];
        col->has_width = COLS[c].w != 0.0;
        col->width_mm = COLS[c].w;
        col->nentries = (size_t)COLS[c].n;
        col->entries = arena_alloc(a, col->nentries * sizeof *col->entries, _Alignof(cc_tb_entry));
        if (!col->entries) return -1;
        for (int i = 0; i < COLS[c].n; i++) {
            col->entries[i].label = cstr(COLS[c].e[i][0]);
            col->entries[i].value = cstr(COLS[c].e[i][1]);
            col->entries[i].size = 1.0;
        }
    }
    return 0;
}

static void jstr(JsonW *w, cc_str s) { jw_str_n(w, s.p ? s.p : "", s.n); }

int cc_plan_template_json(const cc_plan_template *t, Buf *b) {
    static const char *const UNIT[3] = {"mm", "cm", "m"}, *const AXES[3] = {"off", "model", "model-or-columns"};
    JsonW w;
    jw_init(&w, b, 1);
    jw_obj_begin(&w);
    jw_key(&w, "title");
    jstr(&w, t->title);
    jw_key(&w, "level_note");
    jstr(&w, t->level_note);
    jw_key(&w, "dimension_unit");
    jw_str(&w, UNIT[t->dimension_unit]);
    jw_key(&w, "hidden_classes");
    jw_arr_begin(&w);
    for (size_t i = 0; i < t->nhidden; i++) jstr(&w, t->hidden_classes[i]);
    jw_arr_end(&w);
    const struct {
        const char *k;
        int v;
    } flags1[] = {{"seen_fills", t->seen_fills}};
    jw_key(&w, flags1[0].k);
    jw_bool(&w, flags1[0].v);
    jw_key(&w, "axes");
    jw_str(&w, AXES[t->axes]);
    const struct {
        const char *k;
        int v;
    } flags[] = {{"dimensions", t->dimensions},         {"slab_levels", t->slab_levels},
                 {"opening_crosses", t->opening_crosses}, {"opening_dimensions", t->opening_dimensions},
                 {"element_labels", t->element_labels},   {"section_markers", t->section_markers}};
    for (int i = 0; i < 6; i++) {
        jw_key(&w, flags[i].k);
        jw_bool(&w, flags[i].v);
    }
    jw_key(&w, "legend");
    jw_obj_begin(&w);
    jw_key(&w, "show");
    jw_bool(&w, t->legend.show);
    jw_key(&w, "title");
    jstr(&w, t->legend.title);
    jw_key(&w, "cut");
    jstr(&w, t->legend.cut);
    jw_key(&w, "seen");
    jstr(&w, t->legend.seen);
    jw_key(&w, "above");
    jstr(&w, t->legend.above);
    jw_key(&w, "families");
    jw_obj_begin(&w);
    for (size_t i = 0; i < t->legend.nfamilies; i++) {
        jw_key_n(&w, t->legend.families[i].key.p, t->legend.families[i].key.n);
        jstr(&w, t->legend.families[i].value);
    }
    jw_obj_end(&w);
    jw_obj_end(&w);
    jw_key(&w, "title_block");
    jw_obj_begin(&w);
    jw_key(&w, "height_mm");
    jw_f64(&w, t->title_block.height_mm);
    jw_key(&w, "columns");
    jw_arr_begin(&w);
    for (size_t c = 0; c < t->title_block.ncolumns; c++) {
        const cc_tb_column *col = &t->title_block.columns[c];
        jw_obj_begin(&w);
        jw_key(&w, "width_mm");
        if (col->has_width)
            jw_f64(&w, col->width_mm);
        else
            jw_null(&w);
        jw_key(&w, "entries");
        jw_arr_begin(&w);
        for (size_t i = 0; i < col->nentries; i++) {
            const cc_tb_entry *e = &col->entries[i];
            jw_obj_begin(&w);
            jw_key(&w, "label");
            jstr(&w, e->label);
            jw_key(&w, "value");
            jstr(&w, e->value);
            jw_key(&w, "color");
            if (e->has_color) {
                Buf h;
                buf_init(&h);
                cc_rgb_hex(&h, e->color);
                jw_str_n(&w, h.p, h.len);
                buf_free(&h);
            } else {
                jw_null(&w);
            }
            if (e->size != 1.0) {
                jw_key(&w, "size");
                jw_f64(&w, e->size);
            }
            jw_obj_end(&w);
        }
        jw_arr_end(&w);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_obj_end(&w);
    jw_key(&w, "fields");
    jw_obj_begin(&w);
    for (size_t i = 0; i < t->nfields; i++) {
        jw_key_n(&w, t->fields[i].key.p, t->fields[i].key.n);
        jstr(&w, t->fields[i].value);
    }
    jw_obj_end(&w);
    jw_obj_end(&w);
    return jw_finish(&w) == JW_OK ? 0 : -1;
}

int cc_plan_template_check(const cc_plan_template *t, Buf *err) {
    double h = t->title_block.height_mm;
    if (!(h >= 10.0 && h <= 120.0)) {
        buf_puts(err, "title_block.height_mm must be between 10 and 120");
        return 1;
    }
    int many = t->title_block.ncolumns > 40;
    for (size_t c = 0; c < t->title_block.ncolumns; c++) many |= t->title_block.columns[c].nentries > 8;
    if (many) {
        buf_puts(err, "title block: at most 40 columns of 8 entries");
        return 1;
    }
    for (size_t c = 0; c < t->title_block.ncolumns; c++) {
        const cc_tb_column *col = &t->title_block.columns[c];
        if (col->has_width && !(col->width_mm >= 1.0 && col->width_mm <= 1000.0)) {
            buf_puts(err, "title block column widths must be between 1 and 1000 mm");
            return 1;
        }
        for (size_t i = 0; i < col->nentries; i++) {
            double s = col->entries[i].size;
            if (!(s >= 0.2 && s <= 5.0)) {
                buf_puts(err, "title block entry size must be between 0.2 and 5");
                return 1;
            }
        }
    }
    return 0;
}

int cc_plan_template_hides(const cc_plan_template *t, const char *cls, size_t n) {
    for (size_t i = 0; i < t->nhidden; i++)
        if (eq_ignore_ascii_case(t->hidden_classes[i].p, t->hidden_classes[i].n, cls, n)) return 1;
    return 0;
}

/* One UTF-8 character at s[*i] (advanced); U+FFFD for a bad byte. */
static uint32_t utf8_next(const char *s, size_t n, size_t *i) {
    const unsigned char *u = (const unsigned char *)s;
    unsigned c = u[*i];
    int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (!len || *i + (size_t)len > n) {
        (*i)++;
        return 0xfffd;
    }
    uint32_t cp = len == 1 ? c : len == 2 ? c & 0x1f : len == 3 ? c & 0x0f : c & 0x07;
    for (int k = 1; k < len; k++) cp = (cp << 6) | (u[*i + (size_t)k] & 0x3f);
    *i += (size_t)len;
    return cp;
}

/* char::is_whitespace (Unicode White_Space). */
static int is_ws(uint32_t c) {
    return (c >= 9 && c <= 13) || c == 32 || c == 0x85 || c == 0xa0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) ||
           c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000;
}

static int blank(cc_str s) {
    for (size_t i = 0; i < s.n;)
        if (!is_ws(utf8_next(s.p, s.n, &i))) return 0;
    return 1;
}

static int key_cmp(cc_str a, cc_str b) {
    size_t n = a.n < b.n ? a.n : b.n;
    int c = n ? memcmp(a.p, b.p, n) : 0;
    return c ? c : a.n < b.n ? -1 : a.n > b.n;
}

int cc_plan_template_with_project(cc_plan_template *t, const cc_kv *project, size_t n, Arena *a) {
    cc_kv *f = arena_alloc(a, (t->nfields + n + 1) * sizeof *f, _Alignof(cc_kv));
    if (!f) return -1;
    size_t nf = t->nfields;
    if (nf) memcpy(f, t->fields, nf * sizeof *f);
    for (size_t i = 0; i < n; i++) {
        if (blank(project[i].value)) continue;
        size_t j = 0;
        while (j < nf && key_cmp(f[j].key, project[i].key) < 0) j++;
        cc_kv kv;
        if (dup_str(a, project[i].key, &kv.key) || dup_str(a, project[i].value, &kv.value)) return -1;
        if (j < nf && !key_cmp(f[j].key, kv.key)) {
            f[j] = kv;
        } else {
            memmove(f + j + 1, f + j, (nf - j) * sizeof *f);
            f[j] = kv;
            nf++;
        }
    }
    t->fields = f;
    t->nfields = nf;
    return 0;
}

/* template::may_be_empty: the sheet's own keys and project::KNOWN's. */
static int may_be_empty(const char *k, size_t n) {
    static const char *const KEYS[] = {
        "phase", "revision", "drawn", "checked", "drawing_no",
        /* bimble_core::project::KNOWN */
        "project_number", "project_title", "project_address", "project_city", "building", "phase",
        "phase_description", "status", "end_date", "client", "client_address", "client_email", "client_phone",
        "architect", "architect_address", "architect_email", "architect_phone", "engineer", "author",
        "engineer_address", "engineer_email", "engineer_phone", "contractor", "contractor_address",
        "contractor_email", "contractor_phone"};
    for (size_t i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++)
        if (strlen(KEYS[i]) == n && !memcmp(KEYS[i], k, n)) return 1;
    return 0;
}

void cc_template_fill(Buf *out, const char *text, size_t n, const cc_str (*values)[2], size_t nvalues,
                      const cc_kv *fields, size_t nfields) {
    size_t i = 0;
    while (i < n) {
        const char *open = find(text + i, n - i, '{');
        if (!open) break;
        size_t at = (size_t)(open - text);
        buf_append(out, text + i, at - i);
        const char *close = find(open + 1, n - at - 1, '}');
        if (!close) {
            buf_append(out, open, n - at);
            return;
        }
        const char *key = open + 1;
        size_t kn = (size_t)(close - key);
        const cc_str *v = NULL;
        for (size_t j = 0; j < nvalues && !v; j++)
            if (values[j][0].n == kn && !memcmp(values[j][0].p, key, kn)) v = &values[j][1];
        for (size_t j = 0; j < nfields && !v; j++)
            if (fields[j].key.n == kn && !memcmp(fields[j].key.p, key, kn)) v = &fields[j].value;
        if (v)
            buf_append(out, v->p, v->n);
        else if (!may_be_empty(key, kn))
            buf_append(out, open, kn + 2);
        i = (size_t)(close - text) + 1;
    }
    buf_append(out, text + i, n - i);
}

/* ---- small helpers ---- */

void cc_view_frame_to_paper(const cc_view_frame *f, const double world[3], double out[2]) {
    double p[3] = {world[0] - f->origin[0], world[1] - f->origin[1], world[2] - f->origin[2]}, q[3];
    cc_turn_apply_point(&f->turn, p, q);
    out[0] = f->ox + (q[0] - f->x0) * f->k;
    out[1] = f->oy + (f->y1 - q[1]) * f->k;
}

void cc_plan_options_init(cc_plan_options *o) {
    memset(o, 0, sizeof *o);
    o->cut_above_mm = 1200.0;
    o->scale = 100;
    o->paper = CC_PAPER_A4;
    o->version = cstr(CC_IFC_VERSION);
}

/* f64::total_cmp. */
static int total_cmp(double a, double b) {
    int64_t x, y;
    memcpy(&x, &a, 8);
    memcpy(&y, &b, 8);
    x ^= (int64_t)(((uint64_t)(x >> 63)) >> 1);
    y ^= (int64_t)(((uint64_t)(y >> 63)) >> 1);
    return x < y ? -1 : x > y;
}

/* {:+.prec}: the sign always. */
static void fmt_signed(Buf *b, double x, int prec) {
    Buf t;
    buf_init(&t);
    fmt_f64_fixed(&t, x, prec);
    if (!t.len || t.p[0] != '-') buf_putc(b, '+');
    buf_append(b, t.p, t.len);
    buf_free(&t);
}

/* str's {:?}: quoted, with \" \\ \n \r \t \0 and other control characters escaped. */
static void debug_str(Buf *b, const char *s, size_t n) {
    static const char H[] = "0123456789abcdef";
    buf_putc(b, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': buf_puts(b, "\\\""); break;
        case '\\': buf_puts(b, "\\\\"); break;
        case '\n': buf_puts(b, "\\n"); break;
        case '\r': buf_puts(b, "\\r"); break;
        case '\t': buf_puts(b, "\\t"); break;
        case 0: buf_puts(b, "\\0"); break;
        default:
            if (c < 0x20 || c == 0x7f) {
                buf_puts(b, "\\u{");
                if (c >= 16) buf_putc(b, H[c >> 4]);
                buf_putc(b, H[c & 15]);
                buf_putc(b, '}');
            } else {
                buf_putc(b, (char)c);
            }
        }
    }
    buf_putc(b, '"');
}

/* A growable array (malloc'd; the results are copied to the arena). */
typedef struct {
    void *p;
    size_t n, cap;
} Vec;

static void *vec_push(Vec *v, size_t sz) {
    if (v->n == v->cap) {
        size_t c = v->cap ? 2 * v->cap : 16;
        void *q = realloc(v->p, c * sz);
        if (!q) return NULL;
        v->p = q;
        v->cap = c;
    }
    return (char *)v->p + sz * v->n++;
}

static cc_ring *copy_rings(Arena *a, const cc_ring *r, size_t n) {
    cc_ring *o = arena_alloc(a, (n ? n : 1) * sizeof *o, _Alignof(cc_ring));
    if (!o) return NULL;
    for (size_t i = 0; i < n; i++) {
        o[i].n = r[i].n;
        o[i].p = arena_alloc(a, (r[i].n ? r[i].n : 1) * sizeof *o[i].p, _Alignof(double));
        if (!o[i].p) return NULL;
        if (r[i].n) memcpy(o[i].p, r[i].p, r[i].n * sizeof *o[i].p);
    }
    return o;
}

typedef double Seg[2][2];

/* What the plane cuts out of one element. */
typedef struct {
    cc_category cat;
    cc_str cls, material;
    cc_ring *loops, *open;
    size_t nloops, nopen;
} Cut;

/* An element entirely below the cut: its top outline and its visible edges. */
typedef struct {
    cc_category cat;
    cc_str cls;
    double top_z;
    cc_ring *top;
    size_t ntop;
    Seg *edges;
    size_t nedges;
} Seen;

/* sort_by(total_cmp of top_z), stable: a merge sort. */
static int sort_seen(Seen *v, size_t n) {
    if (n < 2) return 0;
    Seen *t = malloc(n * sizeof *t);
    if (!t) return -1;
    for (size_t w = 1; w < n; w *= 2) {
        for (size_t lo = 0; lo < n; lo += 2 * w) {
            size_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n, i = lo, j = mid, k = lo;
            while (i < mid && j < hi) t[k++] = total_cmp(v[j].top_z, v[i].top_z) < 0 ? v[j++] : v[i++];
            while (i < mid) t[k++] = v[i++];
            while (j < hi) t[k++] = v[j++];
        }
        memcpy(v, t, n * sizeof *v);
    }
    free(t);
    return 0;
}

typedef struct {
    double lo[2], hi[2];
    int any;
} Bounds;

static void bound(Bounds *b, const double p[2]) {
    for (int k = 0; k < 2; k++) {
        b->lo[k] = fmin_rs(b->lo[k], p[k]);
        b->hi[k] = fmax_rs(b->hi[k], p[k]);
    }
}

static Bounds bounds_init(void) {
    Bounds b = {{1.0 / 0.0, 1.0 / 0.0}, {-1.0 / 0.0, -1.0 / 0.0}, 0};
    return b;
}

static int bounds_ok(const Bounds *b) { return b->lo[0] <= b->hi[0]; }

/* Points in a polygon (even-odd), plan::inside. */
static int inside(const double p[2], const cc_ring *l) {
    int c = 0;
    for (size_t i = 0; i < l->n; i++) {
        const double *a = l->p[i], *b = l->p[(i + 1) % l->n];
        if ((a[1] > p[1]) != (b[1] > p[1]) && p[0] < a[0] + (p[1] - a[1]) / (b[1] - a[1]) * (b[0] - a[0])) c = !c;
    }
    return c;
}

/* ---- plan() ---- */

typedef struct {
    const cc_scene *s;
    const cc_model *m;
    cc_view_setup *su; /* the scene as seen from above (draw_view.h); cc_plan frees it */
    const cc_plan_options *o;
    const cc_style *style;
    const cc_plan_template *tpl;
    Arena *a;
    double ox, oy, k, lo[2], hi[2];
    cc_item_list items;
} Plan;

static void to_paper(const Plan *P, const double p[2], double out[2]) {
    out[0] = P->ox + (p[0] - P->lo[0]) * P->k;
    out[1] = P->oy + (P->hi[1] - p[1]) * P->k;
}

static cc_item *push_item(Plan *P) { return cc_item_list_push(&P->items); }

/* The scene element's material name (SceneElement::material). */
static cc_str element_material(const Plan *P, const cc_scene_element *e) {
    const cc_material *mt = cc_model_material(P->m, &P->m->elements[e->model_index].material);
    cc_str none = {"", 0};
    return mt ? mt->name : none;
}

/* Item::Lines of segments on paper. */
static int seg_lines(Plan *P, const Seg *v, size_t n, const cc_pen *pen) {
    cc_ring *r = arena_alloc(P->a, (n ? n : 1) * sizeof *r, _Alignof(cc_ring));
    double(*pts)[2] = arena_alloc(P->a, (2 * n ? 2 * n : 1) * sizeof *pts, _Alignof(double));
    cc_item *it = r && pts ? push_item(P) : NULL;
    if (!it) return -1;
    for (size_t i = 0; i < n; i++) {
        to_paper(P, v[i][0], pts[2 * i]);
        to_paper(P, v[i][1], pts[2 * i + 1]);
        r[i].p = pts + 2 * i;
        r[i].n = 2;
    }
    it->kind = CC_ITEM_LINES;
    it->rings = r;
    it->nrings = n;
    it->has_pen = 1;
    it->pen = *pen;
    return 0;
}

static cc_ring *paper_loops(Plan *P, const cc_ring *l, size_t n) {
    cc_ring *r = copy_rings(P->a, l, n);
    if (r)
        for (size_t i = 0; i < n; i++)
            for (size_t j = 0; j < r[i].n; j++) to_paper(P, l[i].p[j], r[i].p[j]);
    return r;
}

static int area_item(Plan *P, cc_ring *r, size_t n, const cc_area_look *look, const cc_pen *outline) {
    cc_item *it = r ? push_item(P) : NULL;
    if (!it) return -1;
    it->kind = CC_ITEM_AREA;
    it->rings = r;
    it->nrings = n;
    if (look) {
        it->has_fill = look->has_fill;
        it->fill = look->fill;
        it->hatches = look->hatches;
        it->nhatches = look->nhatches;
    }
    if (outline) {
        it->has_pen = 1;
        it->pen = *outline;
    }
    return 0;
}

static int text_item(Plan *P, const double pos[2], double size, const char *s, size_t n, cc_text_anchor anchor,
                     double angle, int bold) {
    char *t = arena_alloc(P->a, n + 1, 1);
    cc_item *it = t ? push_item(P) : NULL;
    if (!it) return -1;
    memcpy(t, s, n);
    it->kind = CC_ITEM_TEXT;
    it->pos[0] = pos[0];
    it->pos[1] = pos[1];
    it->size_mm = size;
    it->text = t;
    it->text_len = n;
    it->color = P->style->edges;
    it->anchor = anchor;
    it->angle_deg = angle;
    it->bold = bold;
    return 0;
}

/* plan::storey_setup (viewdraw::ViewSetup for a storey plan). */
typedef cc_view_setup Setup;

static int storey_setup(const cc_scene *s, const cc_model *m, cc_str storey, double cut_above, Setup *out, Buf *err) {
    /* Scene storeys: the model's (by id), sorted by elevation (stable). */
    size_t n = m->nstoreys;
    size_t *ord = malloc((n ? n : 1) * sizeof *ord);
    if (!ord) return -1;
    for (size_t i = 0; i < n; i++) {
        size_t j = i;
        while (j > 0 && total_cmp(m->storeys[ord[j - 1]].elevation, m->storeys[i].elevation) > 0) {
            ord[j] = ord[j - 1];
            j--;
        }
        ord[j] = i;
    }
    size_t idx = n;
    for (size_t i = 0; i < n && idx == n; i++)
        if (str_eq(m->storeys[ord[i]].name, storey)) idx = i;
    for (size_t i = 0; i < n && idx == n; i++)
        if (eq_ignore_ascii_case(m->storeys[ord[i]].name.p, m->storeys[ord[i]].name.n, storey.p, storey.n)) idx = i;
    if (idx == n) {
        if (!n) {
            buf_puts(err, "this model has no storeys");
        } else {
            buf_puts(err, "no storey ");
            debug_str(err, storey.p, storey.n);
            buf_puts(err, ". Storeys: ");
            for (size_t i = 0; i < n; i++) {
                const cc_storey *st = &m->storeys[ord[i]];
                if (i) buf_puts(err, ", ");
                buf_append(err, st->name.p, st->name.n);
                buf_puts(err, " (");
                fmt_signed(err, st->elevation / 1000.0, 3);
                buf_puts(err, " m)");
            }
        }
        free(ord);
        return 1;
    }
    const cc_storey *st = &m->storeys[ord[idx]];
    double oz = s->origin[2], level = st->elevation - oz, next = 1.0 / 0.0;
    for (size_t i = idx + 1; i < n; i++) {
        double z = m->storeys[ord[i]].elevation - oz;
        if (z > level + 1.0) {
            next = z;
            break;
        }
    }
    free(ord);
    out->scene = s;
    out->level = level;
    out->cut = level + cut_above;
    out->next = next;
    out->has_storey = 1;
    out->storey = st->name;
    out->storey_name = st->name;
    out->storey_elevation_mm = st->elevation;
    out->plan_down = 1;
    out->axes = 1;
    out->turn.kind = CC_TURN_PLAN;
    /* In project north (ADR 0045): turned by the building's angle, when it has one; only what
     * lies between this storey and the next is copied. */
    out->turn.a = cc_georef_plan_turn(&m->georef, 0);
    if (out->turn.a != 0.0) return cc_view_storey_turned(s, level + oz, next + oz, out);
    return 0;
}

static double legend_height(size_t n) { return n == 0 ? 0.0 : 10.0 + (double)(n < LEGEND_ROWS ? n : LEGEND_ROWS) * LEGEND_ROW + PAD; }
static double legend_width(size_t n) { return (double)((n + LEGEND_ROWS - 1) / LEGEND_ROWS) * LEGEND_COL + 4.0; }

static int long_enough(const Seg s) { return cc_hypot(s[1][0] - s[0][0], s[1][1] - s[0][1]) > 0.01; }

/* The model geometry of one element (plan.rs, the loop over scene elements). */
static int element_geometry(Plan *P, size_t k, const Setup *su, Arena *tmp, Vec *cuts, Vec *seen, Vec *projection,
                            Vec *above, Vec *edges, Vec *beams) {
    const cc_scene_element *e = &P->s->elements[k];
    cc_mesh m;
    if (cc_scene_world_mesh(P->s, k, tmp, &m)) return -1;
    double zmin = 1.0 / 0.0, zmax = -1.0 / 0.0;
    for (uint32_t i = 0; i < m.nv; i++) {
        zmin = fmin_rs(zmin, (double)m.pos[i][2]);
        zmax = fmax_rs(zmax, (double)m.pos[i][2]);
    }
    double level = su->level, cut = su->cut, next = su->next;
    int is_cut = zmin <= cut && zmax >= cut;
    if (is_cut) {
        cc_section sec;
        if (cc_cut_section(&m, cut, tmp, &sec)) return -1;
        if (sec.nloops || sec.nopen) {
            Cut *c = vec_push(cuts, sizeof(Cut));
            if (!c) return -1;
            c->cat = e->category;
            c->cls = e->class_name;
            c->material = element_material(P, e);
            c->nloops = sec.nloops;
            c->nopen = sec.nopen;
            c->loops = copy_rings(P->a, sec.loops, sec.nloops);
            c->open = copy_rings(P->a, sec.open, sec.nopen);
            if (!c->loops || !c->open) return -1;
        }
    }
    if (zmax >= level - 1.0 && zmin <= cut) {
        /* Edges whose two faces turn away are behind the element itself (a stair's underside
         * under its treads): not drawn (#22). */
        edges->n = 0;
        for (uint32_t i = 0; i < m.ne; i++) {
            if (i < m.nn && !cc_cut_edge_seen_from_above(m.normals[i])) continue;
            double pa[3] = {m.edges[i][0], m.edges[i][1], m.edges[i][2]}, pb[3] = {m.edges[i][3], m.edges[i][4], m.edges[i][5]};
            Seg c;
            if (!cc_cut_clip_z(pa, pb, level - 1.0, cut, c) || !long_enough(c)) continue;
            Seg *d = vec_push(edges, sizeof(Seg));
            if (!d) return -1;
            memcpy(d, c, sizeof c);
        }
        if (P->tpl->seen_fills && !is_cut) {
            /* What is seen of it from above: all its faces turned upwards (#22). */
            cc_ring *up;
            size_t nup;
            if (cc_cut_facing_up(&m, tmp, &up, &nup)) return -1;
            Seen *sn = vec_push(seen, sizeof(Seen));
            if (!sn) return -1;
            sn->cat = e->category;
            sn->cls = e->class_name;
            sn->top_z = zmax;
            sn->ntop = nup;
            sn->top = copy_rings(P->a, up, nup);
            sn->nedges = edges->n;
            sn->edges = arena_alloc(P->a, (edges->n ? edges->n : 1) * sizeof(Seg), _Alignof(double));
            if (!sn->top || !sn->edges) return -1;
            if (edges->n) memcpy(sn->edges, edges->p, edges->n * sizeof(Seg));
        } else {
            for (size_t i = 0; i < edges->n; i++) {
                Seg *d = vec_push(projection, sizeof(Seg));
                if (!d) return -1;
                memcpy(d, (Seg *)edges->p + i, sizeof(Seg));
            }
        }
    }
    /* The beams above the cut, for their labels (plans looking down). */
    if (P->tpl->element_labels && su->plan_down && zmin > cut && zmin < next &&
        e->class_name.n == 7 && eq_ignore_ascii_case(e->class_name.p, 7, "IfcBeam", 7)) {
        cc_beam_label l;
        int got = cc_beam_label_of(&m, e->category, element_material(P, e), P->a, &l);
        if (got < 0) return -1;
        cc_beam_label *d = got ? vec_push(beams, sizeof l) : NULL;
        if (got && !d) return -1;
        if (d) *d = l;
    }
    const cc_element *me = &P->m->elements[e->model_index];
    const cc_storey *st = cc_model_storey(P->m, &me->storey);
    if (su->has_storey && st && str_eq(st->name, su->storey) && zmax > cut) {
        for (uint32_t i = 0; i < m.ne; i++) {
            double pa[3] = {m.edges[i][0], m.edges[i][1], m.edges[i][2]}, pb[3] = {m.edges[i][3], m.edges[i][4], m.edges[i][5]};
            Seg c;
            if (!cc_cut_clip_z(pa, pb, cut, next, c) || !long_enough(c)) continue;
            Seg *d = vec_push(above, sizeof(Seg));
            if (!d) return -1;
            memcpy(d, c, sizeof c);
        }
    }
    return 0;
}

/* The paper size and where the drawing goes on it. Returns 0, or 1 with the error text. */
typedef struct {
    cc_paper_size paper;
    int portrait;
    double w, h, model[2], extra[2], need[2], room[2];
} Sheet;

static void room(double margin, double tb_h, double legend_h, double w, double h, double out[2]) {
    out[0] = w - 2.0 * margin;
    out[1] = h - 2.0 * margin - tb_h - legend_h;
}

static int choose_paper(const cc_plan_options *o, double margin, double tb_h, double legend_h, double legend_w,
                        Sheet *sh, Buf *err) {
    if (o->bare) {
        sh->paper = o->has_paper ? o->paper : CC_PAPER_A4;
        sh->portrait = 0;
        sh->w = sh->need[0];
        sh->h = sh->need[1];
        room(margin, tb_h, legend_h, sh->w, sh->h, sh->room);
        return 0;
    }
    int first = o->has_paper ? (int)o->paper : 0, last = o->has_paper ? (int)o->paper : 4;
    for (int p = first; p <= last; p++)
        for (int portrait = 0; portrait < 2; portrait++) {
            double w, h, r[2];
            cc_paper_size_mm((cc_paper_size)p, &w, &h);
            if (portrait) {
                double t = w;
                w = h;
                h = t;
            }
            room(margin, tb_h, legend_h, w, h, r);
            if (sh->need[0] <= r[0] && sh->need[1] <= r[1] && legend_w <= r[0]) {
                sh->paper = (cc_paper_size)p;
                sh->portrait = portrait;
                sh->w = w;
                sh->h = h;
                sh->room[0] = r[0];
                sh->room[1] = r[1];
                return 0;
            }
        }
    /* The scale at which the drawing would fit the biggest paper (either orientation). */
    double bw, bh, r1[2], r2[2];
    cc_paper_size_mm((cc_paper_size)last, &bw, &bh);
    room(margin, tb_h, legend_h, bw, bh, r1);
    room(margin, tb_h, legend_h, bh, bw, r2);
    double f1 = fmax_rs(sh->model[0] / (r1[0] - sh->extra[0]), sh->model[1] / (r1[1] - sh->extra[1]));
    double f2 = fmax_rs(sh->model[0] / (r2[0] - sh->extra[0]), sh->model[1] / (r2[1] - sh->extra[1]));
    double n = fmin_rs(f1, f2);
    int hint = -1;
    if (n > 0.0)
        for (int i = 0; i < CC_NSTANDARD_SCALES && hint < 0; i++)
            if ((double)CC_STANDARD_SCALES[i] >= n) hint = i;
    buf_puts(err, "at 1:");
    buf_u64(err, o->scale);
    buf_puts(err, " the plan (");
    fmt_f64_fixed(err, sh->model[0] / 1000.0, 1);
    buf_puts(err, " \xc3\x97 ");
    fmt_f64_fixed(err, sh->model[1] / 1000.0, 1);
    buf_puts(err, " m) needs ");
    fmt_f64_fixed(err, sh->need[0], 0);
    buf_puts(err, " \xc3\x97 ");
    fmt_f64_fixed(err, sh->need[1], 0);
    buf_puts(err, " mm and does not fit on ");
    buf_puts(err, cc_paper_size_name((cc_paper_size)last));
    buf_puts(err, "; ");
    if (hint >= 0) {
        buf_puts(err, "1:");
        buf_u64(err, CC_STANDARD_SCALES[hint]);
        buf_puts(err, " fits on ");
        buf_puts(err, cc_paper_size_name((cc_paper_size)last));
    } else {
        buf_puts(err, "no scale up to 1:5000 fits");
    }
    return 1;
}

/* plan.rs, the view's crop (ADR 0046): what is cut, seen, projected and above clipped to the
 * rectangle `r` (turned scene mm), what is left out of it dropped, the beam labels whose middle
 * lies outside it too. In place; new rings in the plan's arena. */
static size_t crop_segs(const cc_crop_rect *r, Seg *v, size_t n) {
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        Seg c;
        if (cc_crop_segment(r, (const double(*)[2])v[i], c)) memcpy(v[k++], c, sizeof c);
    }
    return k;
}

static int crop_geometry(Plan *P, const cc_crop_rect *r, Vec *cuts, Vec *seen, Vec *projection, Vec *above,
                         Vec *beams) {
    Arena *a = P->a;
    Cut *cv = cuts->p;
    size_t kc = 0;
    for (size_t i = 0; i < cuts->n; i++) {
        Cut c = cv[i];
        cc_ring *loops = arena_alloc(a, (c.nloops ? c.nloops : 1) * sizeof *loops, _Alignof(cc_ring));
        if (!loops) return -1;
        size_t nl = 0;
        for (size_t j = 0; j < c.nloops; j++) {
            if (cc_crop_polygon(r, &c.loops[j], a, &loops[nl])) return -1;
            if (loops[nl].n) nl++;
        }
        Vec open = {0};
        for (size_t j = 0; j < c.nopen; j++) {
            cc_ring *pieces;
            size_t np;
            if (cc_crop_polyline(r, &c.open[j], a, &pieces, &np)) {
                free(open.p);
                return -1;
            }
            for (size_t q = 0; q < np; q++) {
                cc_ring *d = vec_push(&open, sizeof *d);
                if (!d) {
                    free(open.p);
                    return -1;
                }
                *d = pieces[q];
            }
        }
        c.loops = loops;
        c.nloops = nl;
        c.nopen = open.n;
        c.open = arena_alloc(a, (open.n ? open.n : 1) * sizeof *c.open, _Alignof(cc_ring));
        if (c.open && open.n) memcpy(c.open, open.p, open.n * sizeof *c.open);
        free(open.p);
        if (!c.open) return -1;
        if (c.nloops || c.nopen) cv[kc++] = c;
    }
    cuts->n = kc;
    Seen *sv = seen->p;
    size_t ks = 0;
    for (size_t i = 0; i < seen->n; i++) {
        Seen sn = sv[i];
        cc_ring *top = arena_alloc(a, (sn.ntop ? sn.ntop : 1) * sizeof *top, _Alignof(cc_ring));
        if (!top) return -1;
        size_t nt = 0;
        for (size_t j = 0; j < sn.ntop; j++) {
            if (cc_crop_polygon(r, &sn.top[j], a, &top[nt])) return -1;
            if (top[nt].n) nt++;
        }
        sn.top = top;
        sn.ntop = nt;
        sn.nedges = crop_segs(r, sn.edges, sn.nedges); /* the edges are the plan's own copy */
        if (sn.ntop || sn.nedges) sv[ks++] = sn;
    }
    seen->n = ks;
    projection->n = crop_segs(r, projection->p, projection->n);
    above->n = crop_segs(r, above->p, above->n);
    cc_beam_label *bv = beams->p;
    size_t kb = 0;
    for (size_t i = 0; i < beams->n; i++) {
        double mid[2] = {(bv[i].a[0] + bv[i].b[0]) / 2.0, (bv[i].a[1] + bv[i].b[1]) / 2.0};
        if (cc_crop_contains(r, mid)) bv[kb++] = bv[i];
    }
    beams->n = kb;
    return 0;
}

static int plan_body(Plan *P, Arena *tmp, cc_paper *sheet, cc_plan_info *info, Buf *err, Vec *cuts, Vec *seen,
                     Vec *projection, Vec *above, Vec *edges, Vec *beams, Vec *scratch) {
    const cc_plan_options *o = P->o;
    const cc_plan_template *tpl = P->tpl;
    const cc_style *style = P->style;
    const cc_scene *s = P->s;
    Arena *a = P->a;
    if (o->scale < 1 || o->scale > 5000) {
        buf_puts(err, "scale 1:");
        buf_u64(err, o->scale);
        buf_puts(err, ": must be between 1:1 and 1:5000");
        return 1;
    }
    if (!(o->cut_above_mm >= -100000.0 && o->cut_above_mm <= 100000.0)) {
        buf_puts(err, "cut height must be a number of mm within \xc2\xb1"
                      "100 m");
        return 1;
    }
    int rc = cc_plan_template_check(tpl, err);
    if (rc) return rc;
    /* What is drawn: the scene as seen from above, the heights that matter, the storey. */
    rc = o->view ? cc_view_prepare(s, P->m, o->view, P->su, err)
                 : storey_setup(s, P->m, o->storey, o->cut_above_mm, P->su, err);
    if (rc) return rc;
    Setup su = *P->su;
    s = P->s = su.scene;
    double oz = s->origin[2], level = su.level, cut = su.cut;

    /* ---- model geometry (scene mm) */
    size_t hidden = 0;
    for (size_t k = 0; k < s->nelements; k++) {
        const cc_scene_element *e = &s->elements[k];
        if (cc_plan_template_hides(tpl, e->class_name.p, e->class_name.n)) {
            hidden++;
            continue;
        }
        arena_reset(tmp);
        if (element_geometry(P, k, &su, tmp, cuts, seen, projection, above, edges, beams)) return -1;
    }
    (void)level;
    /* Painter's order: low tops first. */
    if (sort_seen(seen->p, seen->n)) return -1;
    Cut *cv = cuts->p;
    Seen *sv = seen->p;
    Seg *pv = projection->p, *av = above->p;
    /* Columns' centres, for axes when the model has none: counted before the crop, so a
     * crop holding one column of a line still shows the line's axis. */
    for (size_t i = 0; i < cuts->n; i++) {
        if (!eq_ignore_ascii_case(cv[i].cls.p, cv[i].cls.n, "IfcColumn", 9)) continue;
        Bounds cb = bounds_init();
        for (size_t j = 0; j < cv[i].nloops; j++)
            for (size_t q = 0; q < cv[i].loops[j].n; q++) bound(&cb, cv[i].loops[j].p[q]);
        if (!bounds_ok(&cb)) continue;
        double *c = vec_push(scratch, 2 * sizeof(double));
        if (!c) return -1;
        c[0] = (cb.lo[0] + cb.hi[0]) / 2.0;
        c[1] = (cb.lo[1] + cb.hi[1]) / 2.0;
    }
    /* The view's crop region (ADR 0046): its geometry clipped to it, in the turned scene. */
    cc_crop_rect crop;
    int cropped = cc_view_crop_rect(o->view, &su.turn, s->origin, &crop);
    if (cropped && crop_geometry(P, &crop, cuts, seen, projection, above, beams)) return -1;
    cv = cuts->p;
    sv = seen->p;
    pv = projection->p, av = above->p;
    Bounds bb = bounds_init();
    for (size_t i = 0; i < cuts->n; i++) {
        for (size_t j = 0; j < cv[i].nloops; j++)
            for (size_t q = 0; q < cv[i].loops[j].n; q++) bound(&bb, cv[i].loops[j].p[q]);
        for (size_t j = 0; j < cv[i].nopen; j++)
            for (size_t q = 0; q < cv[i].open[j].n; q++) bound(&bb, cv[i].open[j].p[q]);
    }
    for (size_t i = 0; i < seen->n; i++)
        for (size_t j = 0; j < sv[i].ntop; j++)
            for (size_t q = 0; q < sv[i].top[j].n; q++) bound(&bb, sv[i].top[j].p[q]);
    for (size_t i = 0; i < seen->n; i++)
        for (size_t j = 0; j < sv[i].nedges; j++) bound(&bb, sv[i].edges[j][0]), bound(&bb, sv[i].edges[j][1]);
    for (size_t i = 0; i < projection->n; i++) bound(&bb, pv[i][0]), bound(&bb, pv[i][1]);
    for (size_t i = 0; i < above->n; i++) bound(&bb, av[i][0]), bound(&bb, av[i][1]);
    if (!bounds_ok(&bb) && o->view) {
        buf_puts(err, "nothing to draw in view ");
        buf_append(err, o->view->name.p, o->view->name.n);
        return 1;
    }
    if (!bounds_ok(&bb)) {
        buf_puts(err, "nothing to draw at storey ");
        buf_append(err, su.storey.p, su.storey.n);
        buf_puts(err, " (cut at ");
        fmt_signed(err, (cut + oz) / 1000.0, 3);
        buf_puts(err, " m)");
        return 1;
    }
    double blo[2] = {bb.lo[0], bb.lo[1]}, bhi[2] = {bb.hi[0], bb.hi[1]};

    /* ---- grid axes (scene mm): the model's grids (turned with the scene), or axes through the
     * columns; cropped, the axes within the crop. */
    cc_plan_axis *axes = NULL;
    size_t naxes = 0;
    int model_axes = 0;
    if (su.axes && cc_plan_axes(P->m, s->origin, tpl->axes, (const double(*)[2])scratch->p, scratch->n, blo, bhi, a,
                                &axes, &naxes, &model_axes))
        return -1;
    if (model_axes) cc_view_turn_axes(&su, axes, naxes);
    if (cropped) {
        size_t kept = 0;
        for (size_t i = 0; i < naxes; i++) {
            double sg[2][2] = {{axes[i].a[0], axes[i].a[1]}, {axes[i].b[0], axes[i].b[1]}}, c[2][2];
            if (!cc_crop_segment(&crop, (const double(*)[2])sg, c)) continue;
            axes[kept] = axes[i];
            memcpy(axes[kept].a, c[0], sizeof c[0]);
            memcpy(axes[kept].b, c[1], sizeof c[1]);
            kept++;
        }
        naxes = kept;
    }
    Bounds lh = bounds_init();
    bound(&lh, blo);
    bound(&lh, bhi);
    for (size_t i = 0; i < naxes; i++) bound(&lh, axes[i].a), bound(&lh, axes[i].b);
    P->lo[0] = lh.lo[0];
    P->lo[1] = lh.lo[1];
    P->hi[0] = lh.hi[0];
    P->hi[1] = lh.hi[1];
    int any_v = 0, any_h = 0; /* axes running up the sheet, and across */
    for (size_t i = 0; i < naxes; i++) {
        if (cc_plan_axis_vertical(&axes[i])) any_v = 1;
        else any_h = 1;
    }

    /* ---- legend entries */
    Vec legend = {0};
    if (tpl->legend.show) {
        for (int pass = 0; pass < 3; pass++) {
            for (int c = 0; c < (pass < 2 ? CC_CAT_COUNT : 1); c++) {
                int any = 0;
                if (pass == 0)
                    for (size_t i = 0; i < cuts->n && !any; i++) any = cv[i].cat == (cc_category)c && cv[i].nloops;
                else if (pass == 1)
                    for (size_t i = 0; i < seen->n && !any; i++) any = sv[i].cat == (cc_category)c && sv[i].ntop;
                else
                    any = above->n && tpl->legend.above.n;
                if (!any) continue;
                cc_legend_entry *le = vec_push(&legend, sizeof *le);
                if (!le) return -1;
                le->kind = pass == 0 ? CC_SWATCH_CUT : pass == 1 ? CC_SWATCH_SEEN : CC_SWATCH_ABOVE;
                le->cat = (cc_category)c;
                Buf t;
                buf_init(&t);
                if (pass < 2) {
                    /* fill(pattern, [("family", family_name(label))], fields) */
                    cc_str fam = cstr(cc_category_label((cc_category)c));
                    for (size_t f = 0; f < tpl->legend.nfamilies; f++)
                        if (str_eq(tpl->legend.families[f].key, fam)) fam = tpl->legend.families[f].value;
                    cc_str vals[1][2] = {{cstr("family"), fam}};
                    cc_str pat = pass == 0 ? tpl->legend.cut : tpl->legend.seen;
                    cc_template_fill(&t, pat.p, pat.n, (const cc_str(*)[2])vals, 1, tpl->fields, tpl->nfields);
                } else {
                    buf_append(&t, tpl->legend.above.p, tpl->legend.above.n);
                }
                cc_str l = {t.p, t.len};
                int bad = dup_str(a, l, &le->label);
                buf_free(&t);
                if (bad) {
                    free(legend.p);
                    return -1;
                }
            }
        }
    }
    /* A bare drawing's legend goes on its sheet's (PlanInfo::legend). */
    size_t legend_n = o->bare ? 0 : legend.n;
    double legend_h = legend_height(legend_n);
    info->nlegend = legend.n;
    info->legend = arena_alloc(a, (legend.n ? legend.n : 1) * sizeof *info->legend, _Alignof(cc_legend_entry));
    if (info->legend && legend.n) memcpy(info->legend, legend.p, legend.n * sizeof *info->legend);
    free(legend.p);
    if (!info->legend) return -1;

    /* ---- paper */
    double tb_h = o->bare ? 0.0 : tpl->title_block.height_mm, margin = o->bare ? 0.0 : MARGIN;
    double chains_v = !tpl->dimensions ? 0.0 : any_v ? 2.0 : 1.0, chains_h = !tpl->dimensions ? 0.0 : any_h ? 2.0 : 1.0;
    double top_extra = chains_v * DIM_STEP + (any_v ? BUBBLE + 4.0 : 0.0);
    double left_extra = chains_h * DIM_STEP + (any_h ? BUBBLE + 4.0 : 0.0);
    Sheet sh;
    memset(&sh, 0, sizeof sh);
    sh.extra[0] = left_extra + 2.0 * PAD;
    sh.extra[1] = top_extra + TITLE_BAND + 2.0 * PAD;
    sh.model[0] = P->hi[0] - P->lo[0];
    sh.model[1] = P->hi[1] - P->lo[1];
    double k = 1.0 / (double)o->scale;
    P->k = k;
    sh.need[0] = sh.model[0] * k + sh.extra[0];
    sh.need[1] = sh.model[1] * k + sh.extra[1];
    rc = choose_paper(o, margin, tb_h, legend_h, legend_width(legend_n), &sh, err);
    if (rc) return rc;
    double rw = sh.room[0], rh = sh.room[1], w = sh.w, h = sh.h;
    /* Centre the drawing and its decorations in the space above the legend and title block. */
    P->ox = margin + PAD + left_extra + (rw - sh.need[0]) / 2.0;
    P->oy = margin + PAD + top_extra + (rh - sh.need[1]) / 2.0;
    double ox = P->ox, oy = P->oy;
    cc_pen proj_pen, above_pen, cut_pen;
    if (cc_role_pen(style, CC_ROLE_SEEN, o->scale, a, &proj_pen) || cc_role_pen(style, CC_ROLE_ABOVE, o->scale, a, &above_pen) ||
        cc_role_pen(style, CC_ROLE_CUT, o->scale, a, &cut_pen))
        return -1;
    double t = (double)style->paper.text_mm;
    cc_annot_ctx ac = {a, &P->items, style, tpl, o->scale, ox, oy, k, {P->lo[0], P->lo[1]}, {P->hi[0], P->hi[1]}};
    Vec openings = {0}; /* (lo x, lo y, hi x, hi y) of the slabs' rectangular openings */

    /* ---- items, back to front */
    if (projection->n && seg_lines(P, pv, projection->n, &proj_pen)) return -1;
    Vec labels = {0}; /* (centre x, y, top z) */
    size_t seen_lines = 0, seen_elements = 0;
    for (size_t i = 0; i < seen->n; i++) {
        const Seen *sn = &sv[i];
        if (sn->nedges) {
            seen_lines += sn->nedges;
            if (seg_lines(P, sn->edges, sn->nedges, &proj_pen)) goto oom;
        }
        if (!sn->ntop) continue;
        seen_elements++;
        cc_area_look look;
        if (cc_seen_look(style, sn->cat, o->scale, a, &look) ||
            area_item(P, paper_loops(P, sn->top, sn->ntop), sn->ntop, &look, &proj_pen))
            goto oom;
        int slab = eq_ignore_ascii_case(sn->cls.p, sn->cls.n, "IfcSlab", 7);
        if (slab && (tpl->opening_crosses || tpl->opening_dimensions)) {
            /* Holes of the top that are rectangles: a cross (an opening through the slab). */
            size_t ncross = 0;
            cc_ring *cross = arena_alloc(a, 2 * sn->ntop * sizeof *cross, _Alignof(cc_ring));
            if (!cross) goto oom;
            for (size_t li = 0; li < sn->ntop; li++) {
                const cc_ring *l = &sn->top[li];
                int hole = 0;
                if (l->n >= 3)
                    for (size_t j = 0; j < sn->ntop && !hole; j++) hole = j != li && inside(l->p[0], &sn->top[j]);
                Bounds lb = bounds_init();
                for (size_t q = 0; q < l->n; q++) bound(&lb, l->p[q]);
                if (!bounds_ok(&lb)) continue;
                double dx = lb.hi[0] - lb.lo[0], dy = lb.hi[1] - lb.lo[1];
                int rect = fabs_(dx * dy - fabs_(cc_cut_area(l))) <= 0.01 * dx * dy;
                /* WAVE1B (B1, DEVIATIONS 34): a round hole (a sleeve: a regular polygon of 24 corners or
                 * more) gets a cross of two diameters at 45 degrees, with the "Openings" toggle */
                double rr = dx / 2.0;
                int round = !rect && l->n >= 24 && fabs_(dx - dy) <= 0.02 * dx &&
                            fabs_(fabs_(cc_cut_area(l)) - 3.141592653589793 * rr * rr) <= 0.03 * 3.141592653589793 * rr * rr;
                if (hole && round && tpl->opening_dimensions) {
                    double cx = (lb.lo[0] + lb.hi[0]) / 2.0, cy = (lb.lo[1] + lb.hi[1]) / 2.0, k = rr * 0.7071067811865476;
                    double w[4][2] = {{cx - k, cy - k}, {cx + k, cy + k}, {cx - k, cy + k}, {cx + k, cy - k}};
                    double(*c)[2] = arena_alloc(a, 4 * sizeof *c, _Alignof(double));
                    if (!c) goto oom;
                    for (int q = 0; q < 4; q++) to_paper(P, w[q], c[q]);
                    cross[ncross].p = c;
                    cross[ncross++].n = 2;
                    cross[ncross].p = c + 2;
                    cross[ncross++].n = 2;
                }
                if (hole && rect) {
                    double p[2], q[2];
                    to_paper(P, lb.lo, p);
                    to_paper(P, lb.hi, q);
                    if (tpl->opening_crosses) {
                        double(*c)[2] = arena_alloc(a, 4 * sizeof *c, _Alignof(double));
                        if (!c) goto oom;
                        c[0][0] = p[0], c[0][1] = p[1], c[1][0] = q[0], c[1][1] = q[1];
                        c[2][0] = p[0], c[2][1] = q[1], c[3][0] = q[0], c[3][1] = p[1];
                        cross[ncross].p = c;
                        cross[ncross++].n = 2;
                        cross[ncross].p = c + 2;
                        cross[ncross++].n = 2;
                    }
                    if (tpl->opening_dimensions) {
                        double *op = vec_push(&openings, 4 * sizeof(double));
                        if (!op) goto oom;
                        op[0] = lb.lo[0], op[1] = lb.lo[1], op[2] = lb.hi[0], op[3] = lb.hi[1];
                    }
                }
            }
            if (ncross) {
                cc_item *it = push_item(P);
                if (!it) goto oom;
                it->kind = CC_ITEM_LINES;
                it->rings = cross;
                it->nrings = ncross;
                it->has_pen = 1;
                it->pen = proj_pen;
            }
        }
        if (slab && tpl->slab_levels && su.plan_down) {
            Bounds tb = bounds_init();
            for (size_t j = 0; j < sn->ntop; j++)
                for (size_t q = 0; q < sn->top[j].n; q++) bound(&tb, sn->top[j].p[q]);
            if (bounds_ok(&tb)) {
                double p[2], q[2];
                to_paper(P, tb.lo, p);
                to_paper(P, tb.hi, q);
                if (fmin_rs(q[0] - p[0], p[1] - q[1]) >= 12.0) {
                    double *lv = vec_push(&labels, 3 * sizeof(double));
                    if (!lv) goto oom;
                    lv[0] = (p[0] + q[0]) / 2.0;
                    lv[1] = (p[1] + q[1]) / 2.0;
                    lv[2] = sn->top_z;
                }
            }
        }
    }
    for (size_t i = 0; i < labels.n; i++) {
        /* A small triangle pointing down onto the level, its value to the right. */
        const double *c = (const double *)labels.p + 3 * i;
        double r = 1.2, r16 = r * 1.6;
        cc_ring *tri = arena_alloc(a, sizeof *tri, _Alignof(cc_ring));
        double(*tp)[2] = arena_alloc(a, 3 * sizeof *tp, _Alignof(double));
        if (!tri || !tp) goto oom;
        tp[0][0] = c[0] - r, tp[0][1] = c[1] - r16;
        tp[1][0] = c[0] + r, tp[1][1] = c[1] - r16;
        tp[2][0] = c[0], tp[2][1] = c[1];
        tri->p = tp;
        tri->n = 3;
        cc_area_look ink = {1, style->edges, NULL, 0};
        if (area_item(P, tri, 1, &ink, NULL)) goto oom;
        Buf v;
        buf_init(&v);
        fmt_signed(&v, (c[2] + oz) / 1000.0 + 0.0, 2);
        double pos[2] = {c[0] + r + 0.8, c[1] - 0.4};
        int bad = text_item(P, pos, t * 0.8, v.p, v.len, CC_ANCHOR_START, 0.0, 0);
        buf_free(&v);
        if (bad) goto oom;
    }
    free(labels.p);
    labels.p = NULL;
    if (above->n && seg_lines(P, av, above->n, &above_pen)) goto oom;

    /* Grid axes, under what is cut; their bubbles' places and where they cross the drawing's
     * top and left edges, for the dimension chains. */
    cc_axes_drawn drawn;
    if (cc_annot_axis_lines(&ac, axes, naxes, chains_v, chains_h, &drawn)) goto oom;

    /* Fills and hatches of all cut elements first, then all outlines on top: touching elements
     * (a column against a wall) keep both outlines. */
    for (size_t i = 0; i < cuts->n; i++) {
        if (!cv[i].nloops) continue;
        cc_area_look look;
        if (cc_cut_look(style, cv[i].cat, o->scale, a, &look) ||
            area_item(P, paper_loops(P, cv[i].loops, cv[i].nloops), cv[i].nloops, &look, NULL))
            goto oom;
    }
    for (size_t i = 0; i < cuts->n; i++) {
        if (cv[i].nloops && area_item(P, paper_loops(P, cv[i].loops, cv[i].nloops), cv[i].nloops, NULL, &cut_pen))
            goto oom;
        if (cv[i].nopen) {
            cc_ring *r = paper_loops(P, cv[i].open, cv[i].nopen);
            cc_item *it = r ? push_item(P) : NULL;
            if (!it) goto oom;
            it->kind = CC_ITEM_LINES;
            it->rings = r;
            it->nrings = cv[i].nopen;
            it->has_pen = 1;
            it->pen = cut_pen;
        }
    }

    /* Openings (#24): their size inside, their position from the nearest axis. */
    for (size_t i = 0; i < openings.n; i++) {
        const double *op = (const double *)openings.p + 4 * i;
        if (cc_annot_opening(&ac, op, op + 2, axes, naxes)) goto oom;
    }
    free(openings.p);
    openings.p = NULL;
    /* Section labels (#24): beside what is cut, along the beams above. */
    if (tpl->element_labels) {
        for (size_t i = 0; i < cuts->n; i++)
            if (cc_annot_cut_label(&ac, cv[i].cls, cv[i].cat, cv[i].material, cv[i].loops, cv[i].nloops)) return -1;
        for (size_t i = 0; i < beams->n; i++)
            if (cc_annot_beam_label(&ac, (const cc_beam_label *)beams->p + i)) return -1;
    }
    /* Section marks (#24), on plans only. */
    if (tpl->section_markers && su.turn.kind != CC_TURN_SECTION &&
        cc_annot_section_marks(&ac, o->section_marks, o->nsection_marks, &su.turn, s->origin, &P->m->georef, blo, bhi))
        return -1;
    /* Bubbles over everything, then the dimension chains along the axes and overall. */
    if (cc_annot_bubbles(&ac, &drawn) || cc_annot_chains(&ac, &drawn, blo, bhi, any_v, any_h)) return -1;
    /* The view's own dimensions and texts (#33). */
    if (o->view && cc_annot_view(&ac, P->m, o->view, &su.turn, s->origin)) return -1;

    /* ---- texts */
    Buf nb, level_s, drawing, scale_s, paper_s, title;
    buf_init(&nb), buf_init(&level_s), buf_init(&drawing), buf_init(&scale_s), buf_init(&paper_s), buf_init(&title);
    buf_u64(&nb, o->scale);
    fmt_signed(&level_s, su.storey_elevation_mm / 1000.0 + 0.0, 2);
    /* What the drawing is: the view's name, else "Plan <storey>". */
    if (o->view) {
        buf_append(&drawing, o->view->name.p, o->view->name.n);
    } else {
        buf_puts(&drawing, "Plan ");
        buf_append(&drawing, su.storey_name.p, su.storey_name.n);
    }
    buf_puts(&scale_s, "1:");
    buf_append(&scale_s, nb.p, nb.len);
    buf_puts(&paper_s, cc_paper_size_name(sh.paper));
    if (sh.portrait) buf_puts(&paper_s, " portrait");
    cc_str drawing_no = {"", 0};
    for (size_t i = 0; i < tpl->nfields; i++)
        if (str_eq(tpl->fields[i].key, cstr("drawing_no"))) drawing_no = tpl->fields[i].value;
    const cc_str values[11][2] = {
        {cstr("project"), o->project},
        {cstr("storey"), su.storey_name},
        {cstr("level"), {level_s.p, level_s.len}},
        {cstr("view"), o->view ? o->view->name : cstr("")},
        {cstr("drawing"), {drawing.p, drawing.len}},
        {cstr("scale"), {scale_s.p, scale_s.len}},
        {cstr("n"), {nb.p, nb.len}},
        {cstr("drawing_no"), drawing_no},
        {cstr("paper"), {paper_s.p, paper_s.len}},
        {cstr("date"), o->date},
        /* env!("CARGO_PKG_VERSION") in Rust. */
        {cstr("bimble"), o->version},
    };
    /* A view is titled by its name (a sheet's "Ground floor plan, 1:100"); a plan without a view,
     * or a plan looking down whose view has no name, keeps the template's ("plan" in it is why
     * ceiling plans and sections never take it). */
    if (o->view && (!cc_view_name_blank(o->view->name) || !su.plan_down))
        cc_template_fill(&title, "{view}, {scale}", 15, values, 11, tpl->fields, tpl->nfields);
    else
        cc_template_fill(&title, tpl->title.p, tpl->title.n, values, 11, tpl->fields, tpl->nfields);
    double title_y = oy + sh.model[1] * k + PAD + TITLE_BAND - 4.0;
    /* A bare drawing's page reaches the end of its title (a small drawing, a long name). */
    double title_end = 0.0;
    int bad = 0;
    if (title.len) {
        double size = t * 2.0, pos[2] = {ox, title_y};
        bad = text_item(P, pos, size, title.p, title.len, CC_ANCHOR_START, 0.0, 1);
        double tw = cc_text_width_mm(title.p, title.len, size, 1, style->paper.font);
        title_end = ox + tw + PAD;
        cc_pen thick = cut_pen;
        thick.width_mm *= 0.7;
        thick.dash = NULL;
        thick.ndash = 0;
        cc_ring *r = arena_alloc(a, sizeof *r, _Alignof(cc_ring));
        double(*lp)[2] = arena_alloc(a, 2 * sizeof *lp, _Alignof(double));
        cc_item *it = !bad && r && lp ? push_item(P) : NULL;
        if (it) {
            lp[0][0] = ox, lp[0][1] = title_y + 1.0;
            lp[1][0] = ox + tw, lp[1][1] = title_y + 1.0;
            r->p = lp;
            r->n = 2;
            it->kind = CC_ITEM_LINES;
            it->rings = r;
            it->nrings = 1;
            it->has_pen = 1;
            it->pen = thick;
        } else {
            bad = 1;
        }
    }
    /* Scale bar, level note, legend, frame and title block: whole sheets only. */
    if (!bad && !o->bare) {
        cc_fill_values fv = {values, 11, tpl->fields, tpl->nfields};
        bad = cc_annot_scale_bar(&ac, w, title_y) || cc_annot_level_note(&ac, w, h, &fv) ||
              /* the legend, above the title block at the left */
              cc_draw_legend(&ac, MARGIN, h - MARGIN - tb_h, info->legend, info->nlegend) ||
              cc_frame_and_title_block(&ac, w, h, &fv, o->version);
    }
    char *sheet_title = NULL;
    size_t sheet_title_n = 0;
    if (!bad) {
        Buf st;
        buf_init(&st);
        if (o->project.n) {
            buf_append(&st, o->project.p, o->project.n);
            buf_puts(&st, " \xe2\x80\x93 ");
        }
        buf_append(&st, title.p, title.len);
        sheet_title = arena_alloc(a, st.len + 1, 1);
        if (sheet_title) {
            memcpy(sheet_title, st.p, st.len);
            sheet_title_n = st.len;
        } else {
            bad = 1;
        }
        buf_free(&st);
    }
    buf_free(&nb), buf_free(&level_s), buf_free(&drawing), buf_free(&scale_s), buf_free(&paper_s), buf_free(&title);
    if (bad) return -1;

    info->paper = sh.paper;
    info->portrait = sh.portrait;
    info->cut_elements = cuts->n;
    info->seen_elements = seen_elements;
    info->projection_lines = projection->n + seen_lines;
    info->above_lines = above->n;
    info->axes = drawn.nbubbles;
    info->model_axes = model_axes;
    info->hidden_elements = hidden;
    info->frame.turn = su.turn;
    memcpy(info->frame.origin, s->origin, sizeof s->origin);
    info->frame.x0 = P->lo[0];
    info->frame.y1 = P->hi[1];
    info->frame.k = k;
    info->frame.ox = ox;
    info->frame.oy = oy;
    info->frame.cut = cut;
    sheet->width_mm = o->bare ? fmax_rs(w, title_end) : w;
    sheet->height_mm = h;
    sheet->title = sheet_title;
    sheet->title_len = sheet_title_n;
    sheet->font = style->paper.font;
    sheet->nitems = P->items.n;
    cc_item *items = arena_alloc(a, (P->items.n ? P->items.n : 1) * sizeof *items, _Alignof(cc_item));
    if (!items) return -1;
    if (P->items.n) memcpy(items, P->items.p, P->items.n * sizeof *items);
    sheet->items = items;
    return 0;
oom:
    free(labels.p);
    free(openings.p);
    return -1;
}

int cc_plan(const cc_scene *s, const cc_model *m, const cc_plan_options *o, Arena *a, cc_paper *sheet,
            cc_plan_info *info, Buf *err) {
    memset(sheet, 0, sizeof *sheet);
    memset(info, 0, sizeof *info);
    Plan P;
    memset(&P, 0, sizeof P);
    cc_view_setup su;
    memset(&su, 0, sizeof su);
    P.su = &su;
    P.s = s;
    P.m = m;
    P.o = o;
    P.style = o->style;
    P.tpl = o->tpl;
    P.a = a;
    Arena tmp;
    arena_init(&tmp, 0);
    Vec cuts = {0}, seen = {0}, projection = {0}, above = {0}, edges = {0}, beams = {0}, scratch = {0};
    int rc = plan_body(&P, &tmp, sheet, info, err, &cuts, &seen, &projection, &above, &edges, &beams, &scratch);
    free(cuts.p), free(seen.p), free(projection.p), free(above.p), free(edges.p), free(P.items.p);
    free(beams.p), free(scratch.p);
    cc_view_setup_free(&su);
    arena_free(&tmp);
    return rc;
}
