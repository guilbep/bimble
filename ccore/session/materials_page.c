/* The materials page's data (ticket MAT1): query `materials_page`, what
 * crates/bimble-viewer/src/materials_page.rs shows, in one answer:
 *   {editable, rows: [...], library: [...]}
 * rows (material_rows, materials_page.rs:35-77): in an editable model its materials; in a file
 * read as it is (an IFC, a .bimbleview) the elements' material names with the family of the first
 * element of each name, read only. Sorted by the lowercase name (stable). Per row:
 *   id (null read only), name, category, label, ifc_category, elements (how many are made of it),
 *   props: the material's values, or the library grade's when it has none (materials_page.rs:285),
 *   from_library, grade (library::lookup of the name, or null), strength_symbol ("f" without a
 *   grade), own_props (the material has values: "Clear the values"), grade_differs (its values
 *   are not the grade's: "Use the values of <grade>"),
 *   colour ("#rrggbb", the family's screen colour: style.rs families, draw_style.c:438), hatch
 *   (the family's own screen pattern: lines [{angle_deg, spacing_px}] and stipple), paper_cut and
 *   paper_seen (the paper hatch of the family, hatches.rs MaterialHatch; null: the family's own).
 * library: library.rs library() with `have` (a material of that name is in the model).
 * Layer L5 (session). */
#include <string.h>
#include "calls.h"
#include "materials_page.h"
#include "json_w.h"
#include "library.h"
#include "session.h"
#include "verbs_materials.h"
#ifndef CC_VIEW_ONLY
#include "draw_style.h"
#endif

typedef struct {
    const cc_material *m; /* NULL read only */
    cc_str name;
    cc_category category;
    size_t elements;
    char key[96]; /* the lowercase name's start, for the sort */
    size_t keyn;
} Row;

/* str::to_lowercase on ASCII and the Latin-1 capitals (U+00C0..U+00DE but U+00D7), the rest as
 * is (DEVIATIONS: other scripts sort by their bytes). */
static void lower_key(Row *r) {
    size_t n = 0;
    for (size_t i = 0; i < r->name.n && n < sizeof r->key; i++) {
        unsigned char c = (unsigned char)r->name.p[i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        r->key[n++] = (char)c;
        if (c == 0xC3 && i + 1 < r->name.n && n < sizeof r->key) {
            unsigned char d = (unsigned char)r->name.p[++i];
            r->key[n++] = (char)(d >= 0x80 && d <= 0x9E && d != 0x97 ? d + 0x20 : d);
        }
    }
    r->keyn = n;
}

static int key_cmp(const Row *a, const Row *b) {
    size_t n = a->keyn < b->keyn ? a->keyn : b->keyn;
    int c = memcmp(a->key, b->key, n);
    return c ? c : (a->keyn > b->keyn) - (a->keyn < b->keyn);
}

static int str_eq(cc_str a, cc_str b) { return a.n == b.n && (!a.n || !memcmp(a.p, b.p, a.n)); }

static int props_empty(const cc_material_props *p) {
    return !p->has_density && !p->has_e && !p->has_strength && !p->source.n;
}

static int props_eq_lib(const cc_material_props *p, const cc_library_material *l) {
    return p->has_density && p->has_e && p->has_strength && p->density_kg_m3 == l->density_kg_m3 &&
           p->e_mpa == l->e_mpa && p->strength_mpa == l->strength_mpa && p->source.n == strlen(l->source) &&
           !memcmp(p->source.p, l->source, p->source.n);
}

static void hex(JsonW *w, unsigned r, unsigned g, unsigned b) {
    static const char H[] = "0123456789abcdef";
    char s[7] = {'#', H[r >> 4 & 15], H[r & 15], H[g >> 4 & 15], H[g & 15], H[b >> 4 & 15], H[b & 15]};
    jw_str_n(w, s, 7);
}

/* The family colours of GraphicStyle::default() (style.rs; draw_style.c:438-465), for the
 * viewer-only build that has no draw_style.c. */
static const unsigned char FAMILY_RGB[CC_CAT_COUNT][3] = {
    {214, 214, 210}, {150, 170, 196}, {226, 196, 150}, {206, 150, 128},
    {236, 220, 120}, {176, 160, 132}, {176, 214, 226}, {238, 238, 236},
};

static void graphics(JsonW *w, cc_category c, const void *style) {
#ifndef CC_VIEW_ONLY
    const cc_style *st = style;
    if (st) {
        const cc_fill *f = &st->families[c];
        jw_key(w, "colour"), hex(w, f->color.c[0], f->color.c[1], f->color.c[2]);
        jw_key(w, "hatch"), jw_obj_begin(w);
        jw_key(w, "lines"), jw_arr_begin(w);
        for (int i = 0; i < f->nlines; i++) {
            double a = 0, sp = 0;
            jw_obj_begin(w);
            if (cc_f32_shortest(f->lines[i].angle_deg, &a)) a = f->lines[i].angle_deg;
            if (cc_f32_shortest(f->lines[i].spacing_px, &sp)) sp = f->lines[i].spacing_px;
            jw_key(w, "angle_deg"), jw_f64(w, a);
            jw_key(w, "spacing_px"), jw_f64(w, sp);
            jw_obj_end(w);
        }
        jw_arr_end(w);
        jw_key(w, "stipple"), jw_bool(w, f->has_stipple);
        jw_obj_end(w);
        for (int cut = 1; cut >= 0; cut--) {
            const cc_hatch_type *h = cc_style_material_hatch(&st->paper, c, cut);
            jw_key(w, cut ? "paper_cut" : "paper_seen");
            if (h) jw_str_n(w, h->name.p, h->name.n);
            else jw_null(w);
        }
        return;
    }
#else
    (void)style;
#endif
    jw_key(w, "colour"), hex(w, FAMILY_RGB[c][0], FAMILY_RGB[c][1], FAMILY_RGB[c][2]);
    jw_key(w, "hatch"), jw_null(w);
    jw_key(w, "paper_cut"), jw_null(w);
    jw_key(w, "paper_seen"), jw_null(w);
}

static void props_json(JsonW *w, const cc_material_props *p) {
    jw_obj_begin(w);
    if (p->has_density) jw_key(w, "density_kg_m3"), jw_f64(w, p->density_kg_m3);
    if (p->has_e) jw_key(w, "e_mpa"), jw_f64(w, p->e_mpa);
    if (p->has_strength) jw_key(w, "strength_mpa"), jw_f64(w, p->strength_mpa);
    if (p->source.n) jw_key(w, "source"), jw_str_n(w, p->source.p, p->source.n);
    jw_obj_end(w);
}

static void lib_props_json(JsonW *w, const cc_library_material *l) {
    jw_obj_begin(w);
    jw_key(w, "density_kg_m3"), jw_f64(w, l->density_kg_m3);
    jw_key(w, "e_mpa"), jw_f64(w, l->e_mpa);
    jw_key(w, "strength_mpa"), jw_f64(w, l->strength_mpa);
    jw_key(w, "source"), jw_str(w, l->source);
    jw_obj_end(w);
}

/* A read-only row for `name` (family `c`): counted, or added. */
static int ro_add(Arena *a, Row **rows, size_t *n, size_t *cap, cc_str name, cc_category c) {
    if (!name.n) return 0;
    for (size_t i = 0; i < *n; i++)
        if (str_eq((*rows)[i].name, name)) return (*rows)[i].elements++, 0;
    if (*n == *cap) {
        size_t nc = *cap ? 2 * *cap : 16;
        Row *v = arena_alloc(a, nc * sizeof *v, _Alignof(Row));
        if (!v) return -1;
        if (*n) memcpy(v, *rows, *n * sizeof *v);
        *rows = v, *cap = nc;
    }
    Row *r = &(*rows)[(*n)++];
    memset(r, 0, sizeof *r);
    r->name = name, r->category = c, r->elements = 1;
    return 0;
}

int cc_materials_page_query(cc_session *s, Buf *out) {
    Arena a;
    arena_init(&a, 0);
    Row *rows = NULL;
    size_t n = 0, cap = 0;
    int bad = 0;
    if (s->open) {
        const cc_model *m = &s->model;
        n = cap = m->nmaterials;
        rows = arena_alloc(&a, (n ? n : 1) * sizeof *rows, _Alignof(Row));
        bad = !rows;
        for (size_t i = 0; i < n && !bad; i++) {
            Row *r = &rows[i];
            memset(r, 0, sizeof *r);
            r->m = &m->materials[i];
            r->name = r->m->name;
            r->category = r->m->category;
            for (size_t k = 0; k < m->nelements; k++) r->elements += !cc_gid_cmp(&m->elements[k].material, &r->m->id);
        }
    } else if (s->view == CC_VIEW_IFC && s->ifc) {
        const cc_scene *sc = &s->ifc->scene;
        for (size_t k = 0; k < sc->nelements && !bad; k++)
            bad = ro_add(&a, &rows, &n, &cap, s->ifc->info[sc->elements[k].model_index].material, sc->elements[k].category);
    } else if (s->view == CC_VIEW_FILE && s->vf) {
        for (size_t k = 0; k < s->vf->nelements && !bad; k++)
            bad = ro_add(&a, &rows, &n, &cap, s->vf->elements[k].material, s->vf->elements[k].category);
    }
    if (bad) {
        arena_free(&a);
        buf_puts(out, "out of memory");
        return CC_ERR;
    }
    for (size_t i = 0; i < n; i++) lower_key(&rows[i]);
    for (size_t i = 1; i < n; i++) { /* stable: sort_by_key */
        Row t = rows[i];
        size_t j = i;
        while (j > 0 && key_cmp(&rows[j - 1], &t) > 0) rows[j] = rows[j - 1], j--;
        rows[j] = t;
    }
    const void *style = NULL;
#ifndef CC_VIEW_ONLY
    cc_style *st = arena_alloc(&a, sizeof *st, _Alignof(cc_style));
    if (st && !cc_style_default(st, &a)) style = st;
#endif
    JsonW w;
    jw_init(&w, out, 0);
    jw_obj_begin(&w);
    jw_key(&w, "editable"), jw_bool(&w, s->open);
    jw_key(&w, "rows"), jw_arr_begin(&w);
    for (size_t i = 0; i < n; i++) {
        const Row *r = &rows[i];
        const cc_library_material *lib = cc_library_lookup(r->name.p, r->name.n);
        int own = r->m && !props_empty(&r->m->props);
        jw_obj_begin(&w);
        jw_key(&w, "id");
        if (r->m) jw_str_n(&w, r->m->id.c, 22);
        else jw_null(&w);
        jw_key(&w, "name"), jw_str_n(&w, r->name.p, r->name.n);
        jw_key(&w, "category"), jw_str(&w, cc_category_name(r->category));
        jw_key(&w, "ifc_category"), jw_str(&w, cc_category_ifc(r->category));
        jw_key(&w, "elements"), jw_u64(&w, r->elements);
        jw_key(&w, "props");
        if (own) props_json(&w, &r->m->props);
        else if (lib) lib_props_json(&w, lib);
        else jw_obj_begin(&w), jw_obj_end(&w);
        jw_key(&w, "from_library"), jw_bool(&w, !own && lib);
        jw_key(&w, "grade");
        if (lib) jw_str(&w, lib->name);
        else jw_null(&w);
        jw_key(&w, "strength_symbol"), jw_str(&w, lib ? lib->strength_symbol : "f");
        jw_key(&w, "own_props"), jw_bool(&w, own);
        jw_key(&w, "grade_differs"), jw_bool(&w, r->m && lib && !props_eq_lib(&r->m->props, lib));
        graphics(&w, r->category, style);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_key(&w, "library"), jw_arr_begin(&w);
    size_t nl = 0;
    const cc_library_material *lib = cc_library(&nl);
    for (size_t i = 0; i < nl; i++) {
        cc_str ln = {lib[i].name, strlen(lib[i].name)};
        int have = 0;
        for (size_t k = 0; k < n && !have; k++) have = str_eq(rows[k].name, ln);
        jw_obj_begin(&w);
        jw_key(&w, "name"), jw_str(&w, lib[i].name);
        jw_key(&w, "category"), jw_str(&w, cc_category_name(lib[i].category));
        jw_key(&w, "props"), lib_props_json(&w, &lib[i]);
        jw_key(&w, "strength_symbol"), jw_str(&w, lib[i].strength_symbol);
        jw_key(&w, "have"), jw_bool(&w, have);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_obj_end(&w);
    int rc = jw_finish(&w) == JW_OK ? CC_OK : CC_ERR;
    arena_free(&a);
    return rc;
}
