/* Suite ifc_read: the IFC display reader (src/ifc_read.h) against Rust's read_str.
 * - golden/ifc/read/<name>/: scene.txt and every <k>.txt byte for byte, for the 19 inputs (the
 *   six crate fixtures, the nine ifc/fixtures, frame.ifc, frame2.ifc and their IFC2x3 forms).
 *   With the boolean kernel (manifold, CC_HAVE_MANIFOLD) all 19 must match, booleans and opening
 *   included (ticket C6: was a known difference). Without it (cc_boolean_set(NULL), or a build
 *   without manifold) booleans and opening must still differ (drawn uncut) and say so in a
 *   warning: a build with the kernel checks both paths.
 * - each input read twice: the same text; the BMSH of each scene has the layout's size.
 * - robustness: truncations and byte mutations of frame.ifc, mapped_item.ifc and curves.ifc:
 *   a scene or an error, no crash (run it under ASan/UBSan too); not IFC, empty, not STEP.
 * - threads (cc_jobs): every golden read on 1 thread and on 8 gives the same text; so does a
 *   synthetic model of 3000 products (several 1 MiB parse chunks, several batches of 1024,
 *   shared maps, openings, an unsupported placement shared by many products).
 * - timing per input (mean of runs).
 * - C-own goldens (ticket IFC2, golden/DEVIATIONS.md section 10): golden/ifc_c/fixtures/, types
 *   Rust's reader skips (Rust frozen at v0.19.0). golden/ifc_c/read/<name>/ is the C reader's own
 *   output (CCORE_WRITE_GOLDEN=1 writes it again), and each scene's volume and bounding box are
 *   checked against numbers computed by hand from the fixture's dimensions. */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <io.h>
#define mkdir(path, mode) mkdir(path) /* WIN1: no mode argument there */
#endif
#include "../src/boolean.h"
#include "../src/ifc_read.h"
#include "../src/jobs.h"
#include "../src/step.h"
#include "check.h"

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static const struct {
    const char *name, *dir, *file;
} G[] = {
    {"booleans", "test/fixtures", "booleans.ifc"},
    {"chain18", "test/fixtures", "chain18.ifc"},
    {"curves", "test/fixtures", "curves.ifc"},
    {"cyclic_map", "test/fixtures", "cyclic_map.ifc"},
    {"mirror", "test/fixtures", "mirror.ifc"},
    {"tilt", "test/fixtures", "tilt.ifc"},
    {"ishape_beam", "golden/ifc/fixtures", "ishape_beam.ifc"},
    {"circle_column", "golden/ifc/fixtures", "circle_column.ifc"},
    {"rect_hollow", "golden/ifc/fixtures", "rect_hollow.ifc"},
    {"triangulated", "golden/ifc/fixtures", "triangulated.ifc"},
    {"faceted_brep", "golden/ifc/fixtures", "faceted_brep.ifc"},
    {"mapped_item", "golden/ifc/fixtures", "mapped_item.ifc"},
    {"indexed_arc", "golden/ifc/fixtures", "indexed_arc.ifc"},
    {"opening", "golden/ifc/fixtures", "opening.ifc"},
    {"same_level", "golden/ifc/fixtures", "same_level.ifc"},
    {"frame", "golden/ifc", "frame.ifc"},
    {"frame.ifc2x3", "golden/ifc", "frame.ifc2x3.ifc"},
    {"frame2", "golden/ifc", "frame2.ifc"},
    {"frame2.ifc2x3", "golden/ifc", "frame2.ifc2x3.ifc"},
};
/* Without a boolean kernel (Rust: manifold3d): booleans are drawn as their first operand,
 * openings are not cut. With the kernel they match. */
static const char *KNOWN_DIFFERENT[] = {"booleans", "opening"};

static int file_exists(const char *dir, const char *name) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

/* TESS1 (ADR 0085): Rust's goldens are the exact detail; cc_ifc_read's default is standard. */
static int read_at(const char *t, size_t n, cc_ifc_detail d, cc_ifc_scene *s, cc_error *err) {
    cc_ifc_read_opts o = {d, 0, 0};
    return cc_ifc_read_with(t, n, &o, s, err, NULL);
}
static int read_exact(const char *t, size_t n, cc_ifc_scene *s, cc_error *err) {
    return read_at(t, n, CC_IFC_DETAIL_EXACT, s, err);
}

/* Every file of the folder matches: 1, else 0 with the first difference in `why`. */
static int compare_in(const char *root, const char *name, const cc_ifc_scene *s, int rc, const cc_error *err,
                      char *why, size_t wn) {
    char dir[256];
    snprintf(dir, sizeof dir, "%s/%s", root, name);
    Buf b;
    buf_init(&b);
    if (rc) cc_ifc_error_txt(&b, err);
    else cc_ifc_scene_txt(&b, s);
    size_t n;
    char *want = slurp(dir, "scene.txt", &n);
    int ok = b.len == n && !memcmp(b.p, want, n);
    if (!ok) {
        size_t k = 0, line = 1;
        while (k < b.len && k < n && b.p[k] == want[k]) line += want[k++] == '\n';
        size_t e = k;
        while (e < b.len && b.p[e] != '\n' && e - k < 160) e++;
        snprintf(why, wn, "scene.txt line %zu: got \"%.*s\"", line, (int)(e - k), b.p + k);
    }
    free(want);
    size_t ne = rc ? 0 : s->scene.nelements;
    int meshes = !rc && s->triangles <= 20000;
    for (size_t k = 0; ok && meshes && k < ne; k++) {
        char f[32];
        snprintf(f, sizeof f, "%03zu.txt", k);
        if (!file_exists(dir, f)) {
            ok = 0;
            snprintf(why, wn, "%s: no golden file", f);
            break;
        }
        buf_clear(&b);
        cc_ifc_mesh_txt(&b, s, k);
        want = slurp(dir, f, &n);
        if (b.len != n || memcmp(b.p, want, n)) {
            ok = 0;
            size_t q = 0, line = 1;
            while (q < b.len && q < n && b.p[q] == want[q]) line += want[q++] == '\n';
            snprintf(why, wn, "%s line %zu differs", f, line);
        }
        free(want);
    }
    if (ok) {
        char f[32];
        snprintf(f, sizeof f, "%03zu.txt", meshes ? ne : 0);
        if (file_exists(dir, f)) ok = 0, snprintf(why, wn, "golden has %s, the C scene has no such element", f);
    }
    buf_free(&b);
    return ok;
}

static int compare(const char *name, const cc_ifc_scene *s, int rc, const cc_error *err, char *why, size_t wn) {
    return compare_in("golden/ifc/read", name, s, rc, err, why, wn);
}

/* The whole read as text: scene.txt and every element's mesh text, or the error. */
static void read_txt(const char *t, size_t n, Buf *out) {
    cc_ifc_scene s;
    cc_error err;
    if (cc_ifc_read(t, n, &s, &err)) cc_ifc_error_txt(out, &err);
    else {
        cc_ifc_scene_txt(out, &s);
        for (size_t k = 0; k < s.scene.nelements; k++) cc_ifc_mesh_txt(out, &s, k);
    }
    cc_ifc_scene_free(&s);
}

/* A synthetic model of `n` products: columns with their own extrusion, every third a placed
 * copy of a shared map, every fifth a wall with an opening, every seventh on a grid placement
 * (unsupported: warned), property sets between them (parsed past, several chunks). */
static void synthetic_model(Buf *t, size_t n) {
    buf_puts(t, "ISO-10303-21;\nHEADER;\nFILE_DESCRIPTION(('synthetic'),'2;1');\nFILE_SCHEMA(('IFC4'));\nENDSEC;\nDATA;\n"
                "#1=IFCSIUNIT(*,.LENGTHUNIT.,.MILLI.,.METRE.);\n#2=IFCSIUNIT(*,.PLANEANGLEUNIT.,$,.RADIAN.);\n"
                "#3=IFCUNITASSIGNMENT((#1,#2));\n#4=IFCCARTESIANPOINT((0.,0.,0.));\n#5=IFCAXIS2PLACEMENT3D(#4,$,$);\n"
                "#6=IFCGEOMETRICREPRESENTATIONCONTEXT($,'Model',3,1.E-05,#5,$);\n"
                "#7=IFCPROJECT('3Synthetic000000000001',$,'p',$,$,$,$,(#6),#3);\n#8=IFCLOCALPLACEMENT($,#5);\n"
                "#9=IFCBUILDINGSTOREY('3Synthetic000000000002',$,'Level 0',$,$,#8,$,$,.ELEMENT.,0.);\n"
                "#10=IFCDIRECTION((0.,0.,1.));\n#11=IFCCARTESIANPOINT((0.,0.));\n#12=IFCAXIS2PLACEMENT2D(#11,$);\n"
                "#13=IFCRECTANGLEPROFILEDEF(.AREA.,$,#12,300.,300.);\n#14=IFCEXTRUDEDAREASOLID(#13,#5,#10,2800.);\n"
                "#15=IFCSHAPEREPRESENTATION(#6,'Body','SweptSolid',(#14));\n#16=IFCREPRESENTATIONMAP(#5,#15);\n"
                "#17=IFCGRIDPLACEMENT($,$);\n");
    unsigned long long b = 100;
    Buf members;
    buf_init(&members);
    for (size_t i = 0; i < n; i++, b += 30) {
        char line[1024];
        double x = (double)(i % 60) * 1000.0, y = (double)(i / 60) * 1000.0;
        snprintf(line, sizeof line,
                 "#%llu=IFCCARTESIANPOINT((%.1f,%.1f,0.));\n#%llu=IFCAXIS2PLACEMENT3D(#%llu,$,$);\n"
                 "#%llu=IFCLOCALPLACEMENT(#8,#%llu);\n",
                 b, x, y, b + 1, b, b + 2, b + 1);
        buf_puts(t, line);
        unsigned long long pl = i % 7 == 6 ? 17 : b + 2;
        if (i % 3 == 0)
            snprintf(line, sizeof line,
                     "#%llu=IFCMAPPEDITEM(#16,#%llu);\n#%llu=IFCCARTESIANTRANSFORMATIONOPERATOR3D($,$,#4,1.,$);\n"
                     "#%llu=IFCSHAPEREPRESENTATION(#6,'Body','MappedRepresentation',(#%llu));\n",
                     b + 3, b + 4, b + 4, b + 5, b + 3);
        else
            snprintf(line, sizeof line,
                     "#%llu=IFCRECTANGLEPROFILEDEF(.AREA.,$,#12,%.1f,300.);\n#%llu=IFCEXTRUDEDAREASOLID(#%llu,#5,#10,%.1f);\n"
                     "#%llu=IFCSHAPEREPRESENTATION(#6,'Body','SweptSolid',(#%llu));\n",
                     b + 3, i % 5 == 0 ? 900.0 : 200.0 + (double)(i % 11) * 20.0, b + 4, b + 3,
                     2000.0 + (double)(i % 13) * 100.0, b + 5, b + 4);
        buf_puts(t, line);
        snprintf(line, sizeof line, "#%llu=IFCPRODUCTDEFINITIONSHAPE($,$,(#%llu));\n#%llu=%s('3Syn%018zu',$,'E%zu',$,$,#%llu,#%llu,$,$);\n",
                 b + 6, b + 5, b + 7, i % 5 == 0 ? "IFCWALL" : "IFCCOLUMN", i, i, pl, b + 6);
        buf_puts(t, line);
        if (i % 5 == 0) { /* an opening through the wall */
            snprintf(line, sizeof line,
                     "#%llu=IFCRECTANGLEPROFILEDEF(.AREA.,$,#12,200.,600.);\n#%llu=IFCEXTRUDEDAREASOLID(#%llu,#5,#10,1000.);\n"
                     "#%llu=IFCSHAPEREPRESENTATION(#6,'Body','SweptSolid',(#%llu));\n#%llu=IFCPRODUCTDEFINITIONSHAPE($,$,(#%llu));\n"
                     "#%llu=IFCOPENINGELEMENT('3SynO%017zu',$,$,$,$,#%llu,#%llu,$,.OPENING.);\n"
                     "#%llu=IFCRELVOIDSELEMENT('3SynV%017zu',$,$,$,#%llu,#%llu);\n",
                     b + 8, b + 9, b + 8, b + 10, b + 9, b + 11, b + 10, b + 12, i, b + 2, b + 11, b + 13, i, b + 7, b + 12);
            buf_puts(t, line);
        }
        for (int k = 0; k < 6; k++) { /* properties: not displayed, parsed past */
            snprintf(line, sizeof line, "#%llu=IFCPROPERTYSINGLEVALUE('Prop %d; ''x''',$,IFCLABEL('value %zu'),$);\n",
                     b + 20 + k, k, i);
            buf_puts(t, line);
        }
        char m[32];
        snprintf(m, sizeof m, "%s#%llu", members.len ? "," : "", b + 7);
        buf_puts(&members, m);
    }
    buf_puts(t, "#50=IFCRELCONTAINEDINSPATIALSTRUCTURE('3Synthetic000000000003',$,$,$,(");
    buf_append(t, members.p, members.len);
    buf_puts(t, "),#9);\nENDSEC;\nEND-ISO-10303-21;\n");
    buf_free(&members);
}

/* Reads on 1 thread and on 8: the same text. */
static void threads_same(const char *what, const char *t, size_t n) {
    Buf a, b;
    buf_init(&a), buf_init(&b);
    cc_jobs_set_threads(1);
    read_txt(t, n, &a);
    cc_jobs_set_threads(8);
    read_txt(t, n, &b);
    cc_jobs_set_threads(0);
    char m[128];
    snprintf(m, sizeof m, "%s: 1 thread and 8, the same scene", what);
    check_true(m, a.len == b.len && !memcmp(a.p, b.p, a.len));
    buf_free(&a), buf_free(&b);
}

static int bmsh_size_ok(const cc_ifc_scene *s) {
    Buf b;
    buf_init(&b);
    cc_error err;
    int rc = cc_ifc_scene_bmsh(s, 7, &b, &err);
    size_t ne = s->scene.nelements, nv = 0, ni = 0, ns = 0;
    for (size_t k = 0; k < ne; k++) {
        const cc_mesh *m = cc_scene_local_mesh(&s->scene, k);
        nv += m->nv, ni += m->ni, ns += m->ne;
    }
    int ok = rc == 0 && b.len == 52 + 48 * ne + 12 * nv + 4 * ni + 32 * ns && !memcmp(b.p, "BMSH", 4);
    buf_free(&b);
    return ok;
}

static void robustness(const char *dir, const char *file, uint64_t seed) {
    size_t in;
    char *t = slurp(dir, file, &in);
    char *mut = malloc(in + 1);
    uint64_t s = seed;
    size_t scenes = 0, errs = 0;
    for (int k = 0; k < 300; k++) {
        s ^= s << 13, s ^= s >> 7, s ^= s << 17;
        size_t len = in;
        memcpy(mut, t, in);
        if (k < 150) len = (size_t)(s % in);
        else {
            static const char pool[] = "#()$*,;'.=\\/0123456789EeX\n";
            for (int j = 0; j < 1 + (int)(s % 8); j++) {
                s ^= s << 13, s ^= s >> 7, s ^= s << 17;
                mut[s % in] = (s >> 40) & 1 ? pool[(s >> 20) % (sizeof pool - 1)] : (char)(s >> 32);
            }
        }
        cc_ifc_scene sc;
        cc_error err;
        int rc = cc_ifc_read(mut, len, &sc, &err);
        if (rc) errs++;
        else {
            scenes++;
            Buf b;
            buf_init(&b);
            cc_ifc_scene_txt(&b, &sc);
            for (size_t e = 0; e < sc.scene.nelements; e++) cc_ifc_mesh_txt(&b, &sc, e);
            buf_free(&b);
        }
        cc_ifc_scene_free(&sc);
    }
    printf("robustness %s: 150 truncations + 150 mutations: %zu scenes, %zu errors, no crash\n", file, scenes, errs);
    check_true("robustness: 300 runs", scenes + errs == 300);
    free(mut);
    free(t);
}

/* ---- C-own goldens (ticket IFC2): types past Rust's reader, checked against hand numbers */

/* Volume (mm^3, signed: outward triangles give > 0) and box (world mm) of every element. */
static void scene_measure(const cc_ifc_scene *s, double *vol, double *lo, double *hi) {
    *vol = 0;
    for (int k = 0; k < 3; k++) lo[k] = 1e300, hi[k] = -1e300;
    for (size_t e = 0; e < s->scene.nelements; e++) {
        const cc_scene_element *el = &s->scene.elements[e];
        const cc_mesh *m = cc_scene_local_mesh(&s->scene, e);
        for (uint32_t t = 0; t + 2 < m->ni; t += 3) {
            double p[3][3];
            for (int c = 0; c < 3; c++) {
                const float *q = m->pos[m->idx[t + c]];
                for (int k = 0; k < 3; k++)
                    p[c][k] = el->shape < 0 ? q[k]
                                            : (double)el->to_scene.x[k] * q[0] + (double)el->to_scene.y[k] * q[1] +
                                                  (double)el->to_scene.z[k] * q[2] + el->to_scene.t[k];
            }
            *vol += (p[0][0] * (p[1][1] * p[2][2] - p[1][2] * p[2][1]) - p[0][1] * (p[1][0] * p[2][2] - p[1][2] * p[2][0]) +
                     p[0][2] * (p[1][0] * p[2][1] - p[1][1] * p[2][0])) / 6.0;
            for (int c = 0; c < 3; c++)
                for (int k = 0; k < 3; k++) {
                    double w = p[c][k] + s->scene.origin[k];
                    lo[k] = w < lo[k] ? w : lo[k], hi[k] = w > hi[k] ? w : hi[k];
                }
        }
    }
}

static void write_in(const char *root, const char *name, const cc_ifc_scene *s, int rc, const cc_error *err) {
    char dir[256], path[320];
    snprintf(dir, sizeof dir, "%s/%s", root, name);
    mkdir(root, 0777);
    mkdir(dir, 0777);
    for (size_t k = 0;; k++) { /* stale element files */
        snprintf(path, sizeof path, "%s/%03zu.txt", dir, k);
        if (remove(path)) break;
    }
    Buf b;
    buf_init(&b);
    if (rc) cc_ifc_error_txt(&b, err);
    else cc_ifc_scene_txt(&b, s);
    snprintf(path, sizeof path, "%s/scene.txt", dir);
    FILE *f = fopen(path, "wb");
    if (f) fwrite(b.p, 1, b.len, f), fclose(f);
    for (size_t k = 0; !rc && s->triangles <= 20000 && k < s->scene.nelements; k++) {
        buf_clear(&b);
        cc_ifc_mesh_txt(&b, s, k);
        snprintf(path, sizeof path, "%s/%03zu.txt", dir, k);
        f = fopen(path, "wb");
        if (f) fwrite(b.p, 1, b.len, f), fclose(f);
    }
    buf_free(&b);
}
static void write_own(const char *name, const cc_ifc_scene *s, int rc, const cc_error *err) {
    write_in("golden/ifc_c/read", name, s, rc, err);
}

/* Expected by hand: elements, volume (mm^3) within rel_tol, the box within box_tol (mm), and a
 * warning the scene must not have (the type's old "unsupported" one). */
static const struct {
    const char *name;
    size_t elements;
    double volume, rel_tol, lo[3], hi[3], box_tol;
    const char *gone;
    const char *want; /* a warning the scene must have, or NULL */
} OWN[] = {
    /* 300 x 300 x 3000 at (1000, 2000): 270,000,000 */
    {"product_rep", 1, 270e6, 1e-6, {850, 1850, 0}, {1150, 2150, 3000}, 1e-3, NULL, NULL},
    /* IfcMember 200 x 100 x 2000 at x 500, IfcBuildingElementProxy 1000^3 at x 2000, both under
     * an IfcProductRepresentation (ticket IFC3: the importer took neither): 4e7 + 1e9 */
    {"product_rep_member", 2, 1.04e9, 1e-9, {400, -500, 0}, {2500, 500, 2000}, 1e-3, NULL, NULL},
    /* Two extrusions in one Body: 200 x 200 x 1000 + 100 x 100 x 500 at x 1000 = 4.5e7 */
    {"multi_item", 1, 4.5e7, 1e-9, {-100, -100, 0}, {1050, 100, 1000}, 1e-3, NULL, NULL},
    /* Parametric profiles, each extruded 1000 along +Z, centred on the bounding box; areas:
     * L 100x100x10: 100*10 + 90*10 = 1900 */
    {"lshape", 1, 1.9e6, 1e-6, {-50, -50, 0}, {50, 50, 1000}, 1e-3, NULL, NULL},
    /* + fillet 10 (adds (1 - pi/4) 10^2), - 2 edge radii 5 (2 (1 - pi/4) 5^2): 1910.73; arcs as
     * chords: within 0.2 % */
    {"lshape_fillet", 1, 1910730.1, 2e-3, {-50, -50, 0}, {50, 50, 1000}, 1e-3, NULL, NULL},
    /* T 200 deep, flange 150x15, web 10: 150*15 + 185*10 = 4100 */
    {"tshape", 1, 4.1e6, 1e-6, {-75, -100, 0}, {75, 100, 1000}, 1e-3, NULL, NULL},
    /* U 200x80, web 8, flanges 12: 2*80*12 + 176*8 = 3328 */
    {"ushape", 1, 3.328e6, 1e-6, {-40, -100, 0}, {40, 100, 1000}, 1e-3, NULL, NULL},
    /* C 200x80, wall 3, girth 20: web 200*3 + flanges 2*77*3 + lips 2*17*3 = 1164 */
    {"cshape", 1, 1.164e6, 1e-6, {-40, -100, 0}, {40, 100, 1000}, 1e-3, NULL, NULL},
    /* Z 200, flanges 80 from the web's centre line x 12, web 8: 2*(80 + 4)*12 + 176*8 = 3424;
     * x from -80 to 80 (IfcOpenShell: the same) */
    {"zshape", 1, 3.424e6, 1e-6, {-80, -100, 0}, {80, 100, 1000}, 1e-3, NULL, NULL},
    /* asymmetric I 400 deep, bottom 200x20, top 150x15, web 10: 4000 + 2250 + 365*10 = 9900 */
    {"asym_ishape", 1, 9.9e6, 1e-6, {-100, -200, 0}, {100, 200, 1000}, 1e-3, NULL, NULL},
    /* trapezium bottom 300, top 200 from the bottom's left end - 50, height 100:
     * (300 + 200) / 2 * 100 = 25000; x from -50 to 300 centred: -175..175 */
    {"trapezium", 1, 25e6, 1e-6, {-175, -50, 0}, {175, 50, 1000}, 1e-3, NULL, NULL},
    /* ellipse 200 x 100: pi * 200 * 100 = 62831.85; chords within 0.5 %, the box within 1 mm */
    {"ellipse", 1, 62831853.1, 5e-3, {-200, -100, 0}, {200, 100, 1000}, 1.0, NULL, NULL},
    /* rounded rectangle 400x200, r 50: 80000 - (4 - pi) 50^2 = 77853.98; chords within 0.2 % */
    {"rounded_rect", 1, 77853981.6, 2e-3, {-200, -100, 0}, {200, 100, 1000}, 1e-3, NULL, NULL},
    /* Slopes (ticket IFC3; t = tan 0.1 rad = 0.1003346721): the reference points as IfcOpenShell
     * 0.8's source keeps them. I 200 x 400, web 10, flanges 20, thickness at the flange edge,
     * dy = 95 t at the web: 80000 - 2 * 95 (360 - dy) = 11600 + 190 dy = 13411.0408 */
    {"ishape_slope", 1, 13411040.8, 1e-6, {-100, -200, 0}, {100, 200, 1000}, 1e-3, NULL, NULL},
    /* U 200 x 80, web 8, flanges 12, thickness at x = 0: 16000 - 72 (352 + 16 t) / 2 =
     * 3328 - 576 t = 3270.2072 */
    {"ushape_slope", 1, 3270207.2, 1e-6, {-40, -100, 0}, {40, 100, 1000}, 1e-3, NULL, NULL},
    /* + fillets 10 at the two inner corners (+ r^2 (cot(phi/2) - (pi - phi)/2) each, phi the
     * corner's angle: +16.9288) and edge radii 5 at the toes (-4.2322 each): 3295.6004; arcs as
     * chords: within 0.2 % */
    {"ushape_slope_fillet", 1, 3295600.4, 2e-3, {-40, -100, 0}, {40, 100, 1000}, 1e-3, NULL, NULL},
    /* T 200 deep, flange 150 x 15, web 10, WebSlope 0.02 (s = tan 0.02), FlangeSlope 0.1: the web
     * half-width 5 + y s (thickness at y = 0), 5 - 100 s = 2.99973 at the toe; the flange's
     * underside y = 85 + (x - 37.5) t (thickness at x = +-37.5), 88.76255 at the tips; they meet at
     * y = (85 - 37.5 t + 5 t) / (1 - s t) = 81.90350, x = 6.63829. Shoelace of the 8 corners:
     * 3998.7685 (IfcOpenShell 3965.28: its web-side point is at x 5 with x/2 - 10, see
     * DEVIATIONS.md section 10) */
    {"tshape_slope", 1, 3998768.5, 1e-6, {-75, -100, 0}, {75, 100, 1000}, 1e-3, NULL, NULL},
    /* L 100 x 100 x 10, LegSlope 0.1, thickness at the box's centre lines: the faces
     * y = -40 - x t and x = -40 - y t meet at x = y = (-40 + 40 t) / (1 - t^2) = -36.35258;
     * shoelace of the 6 corners: 1795.0603 */
    {"lshape_slope", 1, 1795060.3, 1e-6, {-50, -50, 0}, {50, 50, 1000}, 1e-3, NULL, NULL},
    /* IfcCenterLineProfileDef (ticket IFC3): (0,0)-(1000,0)-(1000,500) offset 10 to each side,
     * mitred: the miter adds the outer 10 x 10 square what it takes from the inner one, so the
     * area is Thickness x length = 20 x 1500 = 30000; x 1000. The ends square: x 0..1010,
     * y -10..500 */
    {"centreline", 1, 3e7, 1e-9, {0, -10, 0}, {1010, 500, 1000}, 1e-3, NULL, NULL},
    /* closed: the ring 1020^2 - 980^2 = 80000; x 1000 */
    {"centreline_closed", 1, 8e7, 1e-9, {-10, -10, 0}, {1010, 1010, 1000}, 1e-3, NULL, NULL},
    /* IfcCompositeProfileDef: two 200 x 100 rectangles at x -300 and 300: 2 x 20000 x 1000 */
    {"composite", 1, 4e7, 1e-9, {-400, -50, 0}, {400, 50, 1000}, 1e-3, NULL, NULL},
    /* IfcMirroredProfileDef: the 200 x 100 rectangle at x 500 mirrored about y: x -600..-400 */
    {"mirrored", 1, 2e7, 1e-9, {-600, -50, 0}, {-400, 50, 1000}, 1e-3, NULL, NULL},
    /* Curves in a body (ticket IFC3): the curves-only proxy draws nothing (and is no element),
     * the member its 200 x 100 x 1000 extrusion: 2e7; the curves counted as "no body by design",
     * not "unsupported" */
    {"geometric_set", 1, 2e7, 1e-9, {-100, -50, 0}, {100, 50, 1000}, 1e-3, "IFCGEOMETRIC", NULL},
    /* IfcRevolvedAreaSolid: a 100x200 rectangle, x 400..500, turned about the Y axis
     * (right-handed: +X goes to -Z). Pappus: angle * area * centroid radius; chords within 0.3 %.
     * A quarter turn: pi/2 * 20000 * 450 = 14137166.9, box x 0..500, z -500..0 */
    {"revolved", 1, 14137166.9, 3e-3, {0, -100, -500}, {500, 100, 0}, 1e-3, NULL, NULL},
    /* A whole turn: 2 pi * 20000 * 450 = 56548667.8, box +-500 (within 1 mm: chords) */
    {"revolved_full", 1, 56548667.8, 3e-3, {-500, -100, -500}, {500, 100, 500}, 1.0, NULL, NULL},
    /* IfcExtrudedAreaSolidTapered 200x200 to 100x100 over 1000, a frustum:
     * h/3 (A1 + A2 + sqrt(A1 A2)) = 1000/3 (40000 + 10000 + 20000) = 23333333.3 */
    {"tapered", 1, 23333333.3, 1e-6, {-100, -100, 0}, {100, 100, 1000}, 1e-3, NULL, NULL},
    /* IfcCsgSolid(IfcBlock 1000x500x200) = 1e8, and an IfcRectangularPyramid 400x300x600 at
     * x 2000: 400*300*600/3 = 2.4e7; 1.24e8 */
    {"csg_block", 1, 1.24e8, 1e-6, {0, 0, 0}, {2400, 500, 600}, 1e-3, NULL, NULL},
    /* IfcSphere r 100: 4/3 pi 1e6 = 4188790.2; IfcRightCircularCylinder r 100 h 1000 at x 500:
     * pi 1e7 = 31415926.5; IfcRightCircularCone r 100 h 300 at x 1000: pi 1e4 * 100 = 3141592.7;
     * 38746309.4; chords within 1.5 %, the box within 1 mm */
    {"csg_round", 1, 38746309.4, 1.5e-2, {-100, -100, -100}, {1100, 100, 1000}, 1.0, NULL, NULL},
    /* 64 nested IfcBooleanResult (ticket IFC3: the bound was 32): a 1000^3 block at depth 64,
     * a disjoint 100^3 block cut off at each level: 1e9 */
    {"deep_nesting", 1, 1e9, 1e-9, {0, 0, 0}, {1000, 1000, 1000}, 1e-3, NULL, NULL},
    /* 65: one past the bound, the proxy drawn empty and warned; the plain member 100^3: 1e6 */
    {"deep_nesting_over", 1, 1e6, 1e-9, {-50, -50, 0}, {50, 50, 100}, 1e-3, NULL, "representation nesting too deep"},
    /* TESS1: a 5000 x 200 x 2800 wall whose opening stands 1 m beside it (nothing cut) and the same
     * wall 5 m away with its 1200 x 1400 window through: 2 x 2.8e9 - 1200 x 200 x 1400 = 5.264e9 */
    {"opening_beside", 2, 5.264e9, 1e-9, {0, 0, 0}, {5000, 5200, 2800}, 1e-3, NULL, NULL},
};

static void own_goldens(void) {
    int write = getenv("CCORE_WRITE_GOLDEN") != NULL;
    for (size_t g = 0; g < sizeof OWN / sizeof *OWN; g++) {
        char file[128];
        snprintf(file, sizeof file, "%s.ifc", OWN[g].name);
        size_t tn;
        char *t = slurp("golden/ifc_c/fixtures", file, &tn);
        cc_ifc_scene s;
        cc_error err;
        int rc = read_exact(t, tn, &s, &err);
        if (write) write_own(OWN[g].name, &s, rc, &err);
        char why[256] = "", m[256];
        int ok = compare_in("golden/ifc_c/read", OWN[g].name, &s, rc, &err, why, sizeof why);
        double vol = 0, lo[3] = {0}, hi[3] = {0};
        if (!rc) scene_measure(&s, &vol, lo, hi);
        double rel = (vol - OWN[g].volume) / OWN[g].volume;
        double box = 0;
        for (int k = 0; k < 3; k++) {
            double a = lo[k] - OWN[g].lo[k], b = hi[k] - OWN[g].hi[k];
            a = a < 0 ? -a : a, b = b < 0 ? -b : b;
            box = a > box ? a : box, box = b > box ? b : box;
        }
        Buf b;
        buf_init(&b);
        if (!rc) cc_ifc_scene_txt(&b, &s);
        int clean = !rc && !strstr(buf_cstr(&b), "unsupported") && !strstr(buf_cstr(&b), "invalid profile");
        printf("%-16s %-8s elements %zu volume %.6g (hand %.6g, %+.2e) box [%.3f %.3f %.3f]-[%.3f %.3f %.3f] %s%s\n",
               OWN[g].name, ok ? "MATCHES" : "DIFFERS", rc ? 0 : s.scene.nelements, vol, OWN[g].volume, rel, lo[0],
               lo[1], lo[2], hi[0], hi[1], hi[2], clean ? "" : "WARNED ", ok ? "" : why);
        /* WIN2: the C-own goldens were written with the kernel. Without it (CC_NO_KERNEL=1: the
         * Windows job until manifold builds there) a scene with booleans is a known difference, as
         * "booleans" and "opening" above: it must differ and say why in its warning. */
        int uncut = !cc_boolean_kernel() && !rc && strstr(buf_cstr(&b), "(no boolean kernel in the C core)");
        snprintf(m, sizeof m, "ifc_c/%s: matches its C-own golden%s", OWN[g].name,
                 uncut ? " (no kernel: differs, drawn uncut and warned)" : "");
        check_true(m, uncut ? !ok : ok);
        snprintf(m, sizeof m, "ifc_c/%s: %zu element(s)", OWN[g].name, OWN[g].elements);
        check_true(m, !rc && s.scene.nelements == OWN[g].elements);
        snprintf(m, sizeof m, "ifc_c/%s: volume within %g of the hand value", OWN[g].name, OWN[g].rel_tol);
        check_true(m, (rel < 0 ? -rel : rel) <= OWN[g].rel_tol);
        snprintf(m, sizeof m, "ifc_c/%s: box within %g mm", OWN[g].name, OWN[g].box_tol);
        check_true(m, box <= OWN[g].box_tol);
        snprintf(m, sizeof m, "ifc_c/%s: no unsupported or invalid warning", OWN[g].name);
        check_true(m, clean);
        if (OWN[g].want) {
            snprintf(m, sizeof m, "ifc_c/%s: warns \"%s\"", OWN[g].name, OWN[g].want);
            check_true(m, strstr(buf_cstr(&b), OWN[g].want) != NULL);
        }
        if (OWN[g].gone) {
            snprintf(m, sizeof m, "ifc_c/%s: no \"%s\"", OWN[g].name, OWN[g].gone);
            check_true(m, !strstr(buf_cstr(&b), OWN[g].gone));
        }
        buf_free(&b);
        threads_same(OWN[g].name, t, tn);
        cc_ifc_scene_free(&s);
        free(t);
    }
}

/* ---- TESS1 (ADR 0085, DEVIATIONS.md section 40): the standard and light details */

static void scene_text_all(const cc_ifc_scene *s, int rc, const cc_error *err, Buf *b) {
    if (rc) cc_ifc_error_txt(b, err);
    else {
        cc_ifc_scene_txt(b, s);
        for (size_t k = 0; k < s->scene.nelements; k++) cc_ifc_mesh_txt(b, s, k);
    }
}
static size_t shared_count(const cc_ifc_scene *s) {
    size_t n = 0;
    for (size_t k = 0; k < s->scene.nelements; k++) n += s->scene.elements[k].shape >= 0;
    return n;
}

/* Each input at standard and light against its exact read (Rust's golden): the same elements,
 * the same shared copies (as many found as before), the box within the detail's chord distance
 * (2 mm, 10 mm), the volume within 2 % (standard) and 5 % (light: 12 chords a circle), never more
 * triangles. An input whose standard read is not its exact read has a C-own golden
 * (golden/ifc_c/standard/<name>/, CCORE_WRITE_GOLDEN=1 writes it); the others are Rust's golden
 * still, byte for byte. Returns the number of inputs standard changed. */
static size_t detail_one(const char *name, const char *dir, const char *file, int write) {
    size_t tn;
    char *t = slurp(dir, file, &tn);
    cc_ifc_scene s0, s1, s2;
    cc_error e0, e1, e2;
    int r0 = read_exact(t, tn, &s0, &e0), r1 = read_at(t, tn, CC_IFC_DETAIL_STANDARD, &s1, &e1);
    int r2 = read_at(t, tn, CC_IFC_DETAIL_LIGHT, &s2, &e2);
    Buf a, b;
    buf_init(&a), buf_init(&b);
    scene_text_all(&s0, r0, &e0, &a);
    scene_text_all(&s1, r1, &e1, &b);
    int same = a.len == b.len && !memcmp(a.p, b.p, a.len);
    char m[256], why[256] = "";
    if (!same) {
        if (write) write_in("golden/ifc_c/standard", name, &s1, r1, &e1);
        snprintf(m, sizeof m, "standard %s: matches its C-own golden (golden/ifc_c/standard)", name);
        int ok = compare_in("golden/ifc_c/standard", name, &s1, r1, &e1, why, sizeof why);
        if (!ok) printf("  %s\n", why);
        check_true(m, ok);
    } else {
        char d[256];
        snprintf(d, sizeof d, "golden/ifc_c/standard/%s/scene.txt", name);
        snprintf(m, sizeof m, "standard %s: the exact read, no C-own golden left over", name);
        check_true(m, !file_exists(".", d));
    }
    const cc_ifc_scene *sc[2] = {&s1, &s2};
    const int rcs[2] = {r1, r2};
    const double box_tol[2] = {2.0 + 1e-3, 10.0 + 1e-3}, vol_tol[2] = {0.02, 0.05};
    const char *lv[2] = {"standard", "light"};
    double v0 = 0, lo0[3] = {0}, hi0[3] = {0};
    if (!r0) scene_measure(&s0, &v0, lo0, hi0);
    for (int d = 0; d < 2; d++) {
        snprintf(m, sizeof m, "%s %s: read as exact is (ok or the same error)", lv[d], name);
        check_true(m, rcs[d] == r0);
        if (r0 || rcs[d]) continue;
        double v, lo[3], hi[3], box = 0;
        scene_measure(sc[d], &v, lo, hi);
        for (int k = 0; k < 3; k++) {
            double x = lo[k] - lo0[k], y = hi[k] - hi0[k];
            x = x < 0 ? -x : x, y = y < 0 ? -y : y;
            box = x > box ? x : box, box = y > box ? y : box;
        }
        double rel = v0 != 0 ? (v - v0) / v0 : 0;
        rel = rel < 0 ? -rel : rel;
        printf("%-14s %-8s triangles %7zu -> %7zu  volume %+.3e  box %.3f mm  shared %zu -> %zu\n", name, lv[d],
               s0.triangles, sc[d]->triangles, rel, box, shared_count(&s0), shared_count(sc[d]));
        snprintf(m, sizeof m, "%s %s: the same elements", lv[d], name);
        check_true(m, sc[d]->scene.nelements == s0.scene.nelements);
        snprintf(m, sizeof m, "%s %s: as many shared copies and shapes", lv[d], name);
        check_true(m, shared_count(sc[d]) == shared_count(&s0) && sc[d]->scene.nshapes == s0.scene.nshapes);
        snprintf(m, sizeof m, "%s %s: no more triangles", lv[d], name);
        check_true(m, sc[d]->triangles <= s0.triangles);
        snprintf(m, sizeof m, "%s %s: box within %g mm", lv[d], name, box_tol[d]);
        check_true(m, box <= box_tol[d]);
        snprintf(m, sizeof m, "%s %s: volume within %g", lv[d], name, vol_tol[d]);
        check_true(m, rel <= vol_tol[d]);
    }
    threads_same(name, t, tn); /* the default (standard): 1 thread and 8 */
    buf_free(&a), buf_free(&b);
    cc_ifc_scene_free(&s0), cc_ifc_scene_free(&s1), cc_ifc_scene_free(&s2);
    free(t);
    return !same;
}

/* An opening beside its host takes nothing: at standard the host is the file's box (12
 * triangles), not the kernel's split of it; the window is still cut (32). */
static void opening_beside(void) {
    size_t tn;
    char *t = slurp("golden/ifc_c/fixtures", "opening_beside.ifc", &tn);
    cc_ifc_scene s;
    cc_error err;
    int rc = read_at(t, tn, CC_IFC_DETAIL_STANDARD, &s, &err);
    size_t n0 = rc || s.scene.nelements < 2 ? 0 : cc_scene_local_mesh(&s.scene, 0)->ni / 3;
    size_t n1 = rc || s.scene.nelements < 2 ? 0 : cc_scene_local_mesh(&s.scene, 1)->ni / 3;
    printf("opening_beside standard: %zu and %zu triangles\n", n0, n1);
    check_true("standard: an opening beside its host leaves the host as given (12)", n0 == 12);
    check_true("standard: the window is cut (32)", n1 == 32);
    cc_ifc_scene_free(&s);
    free(t);
}

/* TESS1: CUSTOM with light's numbers is light, byte for byte; out of its bounds it is standard. */
static void custom_detail(void) {
    size_t tn;
    char *t = slurp("golden/ifc/fixtures", "circle_column.ifc", &tn);
    cc_ifc_read_opts o[4] = {{CC_IFC_DETAIL_LIGHT, 0, 0}, {CC_IFC_DETAIL_CUSTOM, 10.0, 30.0},
                             {CC_IFC_DETAIL_STANDARD, 0, 0}, {CC_IFC_DETAIL_CUSTOM, 0.0, 18.0}};
    Buf b[4];
    for (int k = 0; k < 4; k++) {
        cc_ifc_scene s;
        cc_error err;
        buf_init(&b[k]);
        int rc = cc_ifc_read_with(t, tn, &o[k], &s, &err, NULL);
        scene_text_all(&s, rc, &err, &b[k]);
        cc_ifc_scene_free(&s);
    }
    check_true("custom 10 mm, 30 degrees: light's scene", b[0].len == b[1].len && !memcmp(b[0].p, b[1].p, b[0].len));
    check_true("custom out of bounds (0 mm): standard's scene", b[2].len == b[3].len && !memcmp(b[2].p, b[3].p, b[2].len));
    cc_ifc_read_opts nan = {CC_IFC_DETAIL_CUSTOM, 0.0 / 0.0, 18.0}, bad = {(cc_ifc_detail)9, 1, 1};
    cc_ifc_scene s;
    cc_error err;
    Buf x;
    buf_init(&x);
    int rc = cc_ifc_read_with(t, tn, &nan, &s, &err, NULL);
    scene_text_all(&s, rc, &err, &x);
    cc_ifc_scene_free(&s);
    check_true("custom NaN: standard's scene", x.len == b[2].len && !memcmp(x.p, b[2].p, x.len));
    buf_clear(&x);
    rc = cc_ifc_read_with(t, tn, &bad, &s, &err, NULL);
    scene_text_all(&s, rc, &err, &x);
    cc_ifc_scene_free(&s);
    check_true("an unknown detail: standard's scene", x.len == b[2].len && !memcmp(x.p, b[2].p, x.len));
    buf_free(&x);
    for (int k = 0; k < 4; k++) buf_free(&b[k]);
    free(t);
}

static void details(void) {
    opening_beside();
    custom_detail();
    int write = getenv("CCORE_WRITE_GOLDEN") != NULL;
    size_t changed = 0, n = 0;
    for (size_t g = 0; g < sizeof G / sizeof *G; g++, n++) changed += detail_one(G[g].name, G[g].dir, G[g].file, write);
    for (size_t g = 0; g < sizeof OWN / sizeof *OWN; g++, n++) {
        char file[128];
        snprintf(file, sizeof file, "%s.ifc", OWN[g].name);
        changed += detail_one(OWN[g].name, "golden/ifc_c/fixtures", file, write);
    }
    printf("details: %zu of %zu inputs read otherwise at standard (C-own goldens), the others as Rust\n", changed, n);
}

int main(void) {
    size_t matched = 0;
    printf("%-14s %-9s %8s %9s  %s\n", "input", "result", "elements", "ms", "note");
    for (size_t g = 0; g < sizeof G / sizeof *G; g++) {
        size_t tn;
        char *t = slurp(G[g].dir, G[g].file, &tn);
        cc_ifc_scene s, s2;
        cc_error err, err2;
        int rc = read_exact(t, tn, &s, &err);
        char why[256] = "";
        int ok = compare(G[g].name, &s, rc, &err, why, sizeof why);
        int known = 0;
        for (size_t k = 0; k < sizeof KNOWN_DIFFERENT / sizeof *KNOWN_DIFFERENT; k++)
            known |= !strcmp(KNOWN_DIFFERENT[k], G[g].name);
        known &= !cc_boolean_kernel();
        /* Twice: the same text. */
        int rc2 = read_exact(t, tn, &s2, &err2);
        Buf a, b;
        buf_init(&a), buf_init(&b);
        if (!rc) cc_ifc_scene_txt(&a, &s);
        if (!rc2) cc_ifc_scene_txt(&b, &s2);
        for (size_t k = 0; !rc && k < s.scene.nelements; k++) cc_ifc_mesh_txt(&a, &s, k);
        for (size_t k = 0; !rc2 && k < s2.scene.nelements; k++) cc_ifc_mesh_txt(&b, &s2, k);
        check_true("read twice: identical", rc == rc2 && a.len == b.len && !memcmp(a.p, b.p, a.len));
        buf_free(&a), buf_free(&b);
        cc_ifc_scene_free(&s2);
        threads_same(G[g].name, t, tn);
        if (!rc) check_true("BMSH size", bmsh_size_ok(&s));
        /* Timing: mean of runs (at least 5, about 200 ms). */
        int runs = 0;
        double t0 = now_ms(), el = 0;
        while (runs < 5 || (el < 200 && runs < 1000)) {
            cc_ifc_scene x;
            cc_error e;
            cc_ifc_read(t, tn, &x, &e);
            cc_ifc_scene_free(&x);
            runs++;
            el = now_ms() - t0;
        }
        printf("%-14s %-9s %8zu %9.3f  %s%s\n", G[g].name, ok ? "MATCHES" : known ? "differs*" : "DIFFERS",
               rc ? 0 : s.scene.nelements, el / runs, rc ? err.msg : "", ok ? "" : why);
        check_true(G[g].name, known ? !ok : ok);
        matched += ok;
        cc_ifc_scene_free(&s);
        free(t);
    }
    printf("ifc/read: %zu of %zu goldens match (* = known difference: no boolean kernel)\n", matched,
           sizeof G / sizeof *G);
    printf("ifc/read: boolean kernel %s\n", cc_boolean_kernel() ? "manifold" : "none (uncut)");

    /* The uncut path in a build with the kernel: turned off, booleans and opening differ again
     * and the scene says why. */
    if (cc_boolean_kernel()) {
        const cc_boolean *k = cc_boolean_kernel();
        cc_boolean_set(NULL);
        for (size_t g = 0; g < sizeof G / sizeof *G; g++) {
            int known = 0;
            for (size_t j = 0; j < sizeof KNOWN_DIFFERENT / sizeof *KNOWN_DIFFERENT; j++)
                known |= !strcmp(KNOWN_DIFFERENT[j], G[g].name);
            if (!known) continue;
            size_t tn;
            char *t = slurp(G[g].dir, G[g].file, &tn);
            cc_ifc_scene s;
            cc_error err;
            int rc = read_exact(t, tn, &s, &err);
            char why[256] = "";
            int ok = compare(G[g].name, &s, rc, &err, why, sizeof why);
            Buf a;
            buf_init(&a);
            if (!rc) cc_ifc_scene_txt(&a, &s);
            int said = !rc && strstr(buf_cstr(&a), "(no boolean kernel in the C core)") != NULL;
            printf("%-14s no kernel: %s, %s\n", G[g].name, ok ? "MATCHES" : "differs (uncut)",
                   said ? "warned" : "NOT WARNED");
            check_true("no kernel: differs", !ok);
            check_true("no kernel: warned", said);
            buf_free(&a);
            cc_ifc_scene_free(&s);
            free(t);
        }
        cc_boolean_set(k);
    }

    /* Threads: a model of several parse chunks and element batches. */
    {
        Buf t;
        buf_init(&t);
        synthetic_model(&t, 3000);
        cc_ifc_scene s;
        cc_error err;
        int rc = cc_ifc_read(t.p, t.len, &s, &err);
        Buf a;
        buf_init(&a);
        if (!rc) cc_ifc_scene_txt(&a, &s);
        const char *w = "unsupported placement IFCGRIDPLACEMENT (used identity) (\xc3\x97" "428)";
        int warned = !rc && strstr(buf_cstr(&a), w) != NULL;
        printf("synthetic: %zu bytes, %s, %zu elements, %zu shapes\n", t.len, rc ? err.msg : "read",
               rc ? 0 : s.scene.nelements, rc ? 0 : s.scene.nshapes);
        check_true("synthetic: over 2 parse chunks", t.len > 2 * CC_STEP_CHUNK_BYTES);
        check_true("synthetic: 3000 elements", !rc && s.scene.nelements == 3000);
        check_true("synthetic: the grid placement warned once per product on it (428)", warned);
        if (!warned) printf("%s\n", buf_cstr(&a));
        buf_free(&a);
        cc_ifc_scene_free(&s);
        threads_same("synthetic", t.p, t.len);
        buf_free(&t);
    }

    /* Errors. */
    {
        cc_ifc_scene s;
        cc_error err;
        const char *notifc = "ISO-10303-21;\nHEADER;FILE_SCHEMA(('CONFIG_CONTROL_DESIGN'));ENDSEC;\nDATA;\nENDSEC;\n";
        int rc = cc_ifc_read(notifc, strlen(notifc), &s, &err);
        check_true("not IFC", rc != 0 && !strcmp(err.msg, "not an IFC file (schema \"CONFIG_CONTROL_DESIGN\")"));
        cc_ifc_scene_free(&s);
        rc = cc_ifc_read("PK\x03\x04\x14\x00", 6, &s, &err);
        check_true("ifcZIP: said so", rc != 0 && !strncmp(err.msg, "unsupported format: ifcZIP", 26));
        cc_ifc_scene_free(&s);
        const char *xml = "\xef\xbb\xbf<?xml version=\"1.0\"?><ifcXML/>";
        rc = cc_ifc_read(xml, strlen(xml), &s, &err);
        check_true("ifcXML: said so", rc != 0 && !strncmp(err.msg, "unsupported format: ifcXML", 26));
        cc_ifc_scene_free(&s);
        rc = cc_ifc_read("hello", 5, &s, &err);
        check_true("not STEP", rc != 0 && !strncmp(err.msg, "parse: line 1: ", 15));
        cc_ifc_scene_free(&s);
        rc = cc_ifc_read("", 0, &s, &err);
        check_true("empty: an error", rc != 0);
        cc_ifc_scene_free(&s);
        const char *empty = "ISO-10303-21;\nHEADER;FILE_SCHEMA(('IFC4'));ENDSEC;\nDATA;\nENDSEC;\n";
        rc = cc_ifc_read(empty, strlen(empty), &s, &err);
        check_true("IFC without products: an empty scene", rc == 0 && s.scene.nelements == 0);
        cc_ifc_scene_free(&s);
    }
    robustness("golden/ifc", "frame.ifc", 12345);
    robustness("golden/ifc/fixtures", "mapped_item.ifc", 777);
    robustness("test/fixtures", "curves.ifc", 4242);
    own_goldens();
    if (cc_boolean_kernel()) details(); /* the C-own goldens were written with the kernel */
    robustness("golden/ifc_c/fixtures", "lshape_fillet.ifc", 99);
    return report("ifc_read");
}
