/* See verbs_materials.h. Layer L3: no files, clock, threads or globals. */
#include "verbs_materials.h"
#include <string.h>
#include "guid.h"
#include "library.h"
#include "verbs.h"

static int finite_(double x) { return x - x == 0.0; }

/* ---- errors (the texts of verbs_levels.c: "bad arguments: …", "<verb>: …") ---- */

static int bad(Buf *err, const char *a, const char *b, const char *c) {
    buf_clear(err);
    buf_puts(err, "bad arguments: ");
    if (a) buf_puts(err, a);
    if (b) buf_puts(err, b);
    if (c) buf_puts(err, c);
    return -1;
}

static int bad_type(Buf *err, const char *field, const char *expected) {
    bad(err, "`", field, "`: expected ");
    buf_puts(err, expected);
    return -1;
}

static int oom(Buf *err, const char *verb) {
    buf_clear(err);
    buf_puts(err, verb);
    buf_puts(err, ": out of memory");
    return -1;
}

/* An object's members by name (the last of equal keys wins); an unknown one is refused; a null
 * value counts as absent. */
static int args_of(const JrVal *v, const char *const *F, int n, const JrVal **f, Buf *err) {
    for (int i = 0; i < n; i++) f[i] = NULL;
    if (v->type != JR_OBJ) return bad(err, "expected an object", 0, 0);
    for (size_t j = 0; j < v->n; j++) {
        const JrMember *mb = &v->members[j];
        int i = 0;
        while (i < n && !jr_key_is(mb, F[i])) i++;
        if (i == n) {
            buf_clear(err);
            buf_puts(err, "bad arguments: unknown field `");
            buf_append(err, mb->key, mb->key_n < 200 ? mb->key_n : 200);
            buf_puts(err, "`, expected one of");
            for (int k = 0; k < n; k++) buf_puts(err, k ? ", `" : " `"), buf_puts(err, F[k]), buf_putc(err, '`');
            return -1;
        }
        f[i] = mb->val.type == JR_NULL ? NULL : &mb->val;
    }
    return 0;
}

static int a_gid(const JrVal *v, const char *field, cc_gid *out, Buf *err) {
    if (!v) return bad(err, "missing field `", field, "`");
    if (v->type != JR_STR) return bad_type(err, field, "a GlobalId");
    if (!cc_gid_valid(v->s, v->n)) {
        bad(err, "invalid IFC GlobalId: ", 0, 0);
        cc_buf_debug_str(err, v->s, v->n < 200 ? v->n : 200);
        return -1;
    }
    memcpy(out->c, v->s, 22);
    return 0;
}

/* A string as given (the command trims nothing: Rust's page sends `text.trim()`, so do we). */
static int a_text(const JrVal *v, const char *field, cc_str *out, Buf *err) {
    if (v->type != JR_STR) return bad_type(err, field, "a string");
    *out = cc_str_trim((cc_str){v->s, v->n});
    return 0;
}

static int a_category(const JrVal *v, cc_category *out, Buf *err) {
    if (v->type == JR_STR)
        for (int c = 0; c < CC_CAT_COUNT; c++) {
            const char *n = cc_category_name((cc_category)c);
            if (strlen(n) == v->n && !memcmp(n, v->s, v->n)) {
                *out = (cc_category)c;
                return 0;
            }
        }
    return bad_type(err, "category", "one of concrete, steel, timber, masonry, insulation, earth, glass, other");
}

static int a_num(const JrVal *v, const char *field, double *out, Buf *err) {
    if (v->type != JR_NUM || !finite_(v->num)) return bad_type(err, field, "a number");
    *out = v->num + 0.0;
    return 0;
}

/* {density_kg_m3?, e_mpa?, strength_mpa?, source?}: the whole MaterialProps (absent: no value).
 * Ranges are MaterialProps::validate's, checked by the command. */
static int a_props(const JrVal *v, cc_material_props *p, Buf *err) {
    static const char *const F[] = {"density_kg_m3", "e_mpa", "strength_mpa", "source"};
    const JrVal *f[4];
    memset(p, 0, sizeof *p);
    if (v->type != JR_OBJ) return bad_type(err, "props", "an object");
    if (args_of(v, F, 4, f, err)) return -1;
    if (f[0] && (a_num(f[0], F[0], &p->density_kg_m3, err) || !(p->has_density = 1))) return -1;
    if (f[1] && (a_num(f[1], F[1], &p->e_mpa, err) || !(p->has_e = 1))) return -1;
    if (f[2] && (a_num(f[2], F[2], &p->strength_mpa, err) || !(p->has_strength = 1))) return -1;
    if (f[3]) {
        if (f[3]->type != JR_STR) return bad_type(err, F[3], "a string");
        p->source = (cc_str){f[3]->s, f[3]->n};
    }
    return 0;
}

static int str_eq(cc_str a, cc_str b) { return a.n == b.n && (!a.n || !memcmp(a.p, b.p, a.n)); }

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

/* The material named `name` (trimmed, ASCII case ignored: command.rs:329), but `except`. */
static const cc_material *named(const cc_model *m, cc_str name, const cc_gid *except) {
    cc_str t = cc_str_trim(name);
    for (size_t i = 0; i < m->nmaterials; i++) {
        const cc_material *x = &m->materials[i];
        cc_str o = cc_str_trim(x->name);
        if (except && !cc_gid_cmp(&x->id, except)) continue;
        if (o.n != t.n) continue;
        size_t k = 0;
        while (k < t.n && lower(o.p[k]) == lower(t.p[k])) k++;
        if (k == t.n) return x;
    }
    return NULL;
}

static int taken(Buf *err, const char *verb, cc_str name) {
    buf_clear(err);
    buf_puts(err, verb);
    buf_puts(err, ": there is already a material named ");
    cc_str t = cc_str_trim(name);
    cc_buf_debug_str(err, t.p, t.n < 200 ? t.n : 200);
    return -1;
}

static int no_material(Buf *err, const char *verb, const cc_gid *id) {
    buf_clear(err);
    buf_puts(err, verb);
    buf_puts(err, ": no material ");
    cc_buf_gid(err, id);
    return -1;
}

/* ---- verbs ---- */

int cc_vh_material_add(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const F[] = {"category", "library", "name"};
    const JrVal *f[3];
    if (args_of(args, F, 3, f, err)) return -1;
    cc_material x;
    memset(&x, 0, sizeof x);
    if (f[1]) {
        if (f[1]->type != JR_STR) return bad_type(err, "library", "a grade name of the library");
        size_t n = 0;
        const cc_library_material *lib = cc_library(&n), *hit = NULL;
        for (size_t i = 0; i < n && !hit; i++)
            if (strlen(lib[i].name) == f[1]->n && !memcmp(lib[i].name, f[1]->s, f[1]->n)) hit = &lib[i];
        if (!hit) {
            buf_clear(err);
            buf_puts(err, "material_add: no grade ");
            cc_buf_debug_str(err, f[1]->s, f[1]->n < 200 ? f[1]->n : 200);
            buf_puts(err, " in the library");
            return -1;
        }
        x = cc_library_to_material(hit); /* a new GlobalId, as to_material */
    } else {
        if (!f[2]) return bad(err, "missing field `name` (or `library`)", 0, 0);
        cc_guid g = cc_guid_new();
        memcpy(x.id.c, g.c, 22);
        x.category = CC_CAT_OTHER;
    }
    if (f[2] && a_text(f[2], "name", &x.name, err)) return -1;
    if (f[0] && a_category(f[0], &x.category, err)) return -1;
    if (!x.name.n) return bad(err, "`name` is blank", 0, 0);
    if (named(m, x.name, NULL)) return taken(err, "material_add", x.name);
    cc_command *c = cc_cmds_push(l, CC_CMD_ADD_MATERIAL);
    if (!c) return oom(err, "material_add");
    c->material = x;
    return 0;
}

int cc_vh_material_set(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const F[] = {"category", "id", "name", "props"};
    const JrVal *f[4];
    cc_gid id;
    if (args_of(args, F, 4, f, err) || a_gid(f[1], "id", &id, err)) return -1;
    const cc_material *old = cc_model_material(m, &id);
    if (!old) return no_material(err, "material_set", &id);
    cc_material x = *old;
    if ((f[2] && a_text(f[2], "name", &x.name, err)) || (f[0] && a_category(f[0], &x.category, err)) ||
        (f[3] && a_props(f[3], &x.props, err)))
        return -1;
    const cc_material_props *p = &x.props, *o = &old->props;
    int same = str_eq(x.name, old->name) && x.category == old->category && p->has_density == o->has_density &&
               p->has_e == o->has_e && p->has_strength == o->has_strength &&
               (!p->has_density || p->density_kg_m3 == o->density_kg_m3) && (!p->has_e || p->e_mpa == o->e_mpa) &&
               (!p->has_strength || p->strength_mpa == o->strength_mpa) && str_eq(p->source, o->source);
    if (same) return 0;
    cc_command *c = cc_cmds_push(l, CC_CMD_SET_MATERIAL);
    if (!c) return oom(err, "material_set");
    c->material = x;
    return 0;
}

int cc_vh_material_remove(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const F[] = {"id"};
    const JrVal *f[1];
    cc_gid id;
    if (args_of(args, F, 1, f, err) || a_gid(f[0], "id", &id, err)) return -1;
    const cc_material *x = cc_model_material(m, &id);
    if (!x) return no_material(err, "material_remove", &id);
    size_t used = 0;
    for (size_t i = 0; i < m->nelements; i++) used += !cc_gid_cmp(&m->elements[i].material, &id);
    if (used) {
        buf_clear(err);
        buf_puts(err, "material_remove: ");
        buf_append(err, x->name.p, x->name.n < 200 ? x->name.n : 200);
        buf_puts(err, " is the material of ");
        buf_u64(err, used);
        buf_puts(err, used == 1 ? " element" : " elements");
        buf_puts(err, ": give them another material first");
        return -1;
    }
    cc_command *c = cc_cmds_push(l, CC_CMD_REMOVE_MATERIAL);
    if (!c) return oom(err, "material_remove");
    c->id = id;
    return 0;
}

/* ---- queries ---- */

const char *cc_category_ifc(cc_category c) { /* model.rs:61 */
    static const char *const T[] = {"concrete", "steel", "wood", "brick", "insulation", "earth", "glass", ""};
    return (unsigned)c < CC_CAT_COUNT ? T[c] : "";
}

void cc_material_library_json(JsonW *w) {
    size_t n = 0;
    const cc_library_material *lib = cc_library(&n);
    jw_arr_begin(w);
    for (size_t i = 0; i < n; i++) {
        const cc_library_material *x = &lib[i];
        jw_obj_begin(w);
        jw_key(w, "category"), jw_str(w, cc_category_name(x->category));
        jw_key(w, "density_kg_m3"), jw_f64(w, x->density_kg_m3);
        jw_key(w, "e_mpa"), jw_f64(w, x->e_mpa);
        jw_key(w, "ifc_category"), jw_str(w, cc_category_ifc(x->category));
        jw_key(w, "name"), jw_str(w, x->name);
        jw_key(w, "source"), jw_str(w, x->source);
        jw_key(w, "strength_mpa"), jw_f64(w, x->strength_mpa);
        jw_key(w, "strength_symbol"), jw_str(w, x->strength_symbol);
        jw_obj_end(w);
    }
    jw_arr_end(w);
}

int cc_materials_full_json(const cc_model *m, JsonW *w) {
    jw_arr_begin(w);
    for (size_t i = 0; i < m->nmaterials; i++) {
        const cc_material *x = &m->materials[i];
        const cc_material_props *p = &x->props;
        size_t used = 0;
        for (size_t k = 0; k < m->nelements; k++) used += !cc_gid_cmp(&m->elements[k].material, &x->id);
        jw_obj_begin(w);
        jw_key(w, "category"), jw_str(w, cc_category_name(x->category));
        jw_key(w, "element_count"), jw_u64(w, used);
        jw_key(w, "id"), jw_str_n(w, x->id.c, 22);
        jw_key(w, "name"), jw_str_n(w, x->name.p, x->name.n);
        if (p->has_density || p->has_e || p->has_strength || p->source.n) {
            jw_key(w, "props");
            jw_obj_begin(w);
            if (p->has_density) jw_key(w, "density_kg_m3"), jw_f64(w, p->density_kg_m3);
            if (p->has_e) jw_key(w, "e_mpa"), jw_f64(w, p->e_mpa);
            if (p->has_strength) jw_key(w, "strength_mpa"), jw_f64(w, p->strength_mpa);
            if (p->source.n) jw_key(w, "source"), jw_str_n(w, p->source.p, p->source.n);
            jw_obj_end(w);
        }
        jw_obj_end(w);
    }
    jw_arr_end(w);
    return 0;
}
