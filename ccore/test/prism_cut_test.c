/* Suite prism_cut (ticket SLAB1): openings cut without the boolean kernel (src/prism_cut.h), as
 * the wasm build does. Synthetic models; each scene is built with the kernel (the native build)
 * and again with cc_boolean_set(NULL) (the wasm path): the cut solid's volume, area and box are
 * the kernel's within 1e-6 relative, and the volume is the hand's. Cases: a wall with two windows
 * and a door on its foot (boxes, faces on the host's faces), a column pierced across (a box along
 * x), a rectangular slab with a shaft (box), an L slab with a hexagonal shaft and a box shaft
 * (holes), and what stays uncut with its warning (a recess not through a polygon slab, a mesh
 * host). Without a kernel in the build only the hand's volumes are checked. */
#include <stdio.h>
#include <string.h>
#include "../src/boolean.h"
#include "../src/file.h"
#include "../src/math.h"
#include "../src/model.h"
#include "../src/scene.h"
#include "check.h"

#define HEAD                                                                                                                 \
    "{\"format\":\"bimble\",\"version\":1,\"model\":{\"project_id\":\"0PrismCut000000000000P\",\"project_name\":\"p\","        \
    "\"storeys\":{\"0PrismCut000000000000S\":{\"id\":\"0PrismCut000000000000S\",\"name\":\"L0\",\"elevation\":0.0}},"         \
    "\"materials\":{\"0PrismCut000000000000M\":{\"id\":\"0PrismCut000000000000M\",\"name\":\"C30/37\",\"category\":\"concrete\"}}," \
    "\"elements\":{\"0PrismCut000000000000E\":{\"id\":\"0PrismCut000000000000E\",\"class\":\"%s\",\"name\":\"e\","          \
    "\"storey\":\"0PrismCut000000000000S\",\"material\":\"0PrismCut000000000000M\","                                         \
    "\"placement\":{\"origin\":[100.0,200.0,0.0],\"z_axis\":[0.0,0.0,1.0],\"x_axis\":[0.0,1.0,0.0]},\"generator\":%s,"       \
    "\"openings\":[%s]}}}}"
#define PL "\"placement\":{\"origin\":[%g,%g,%g],\"z_axis\":[%g,%g,%g],\"x_axis\":[%g,%g,%g]}"

static char *op_buf(char *out, size_t cap, int k, const char *placement, const char *gen) {
    snprintf(out, cap, "{\"id\":\"0PrismCut000000000000%c\",%s,\"generator\":%s}", 'a' + k, placement, gen);
    return out;
}

static void measure(const cc_scene *s, double *vol, double *area, double box[6]) {
    double v = 0, ar = 0;
    for (int k = 0; k < 3; k++) box[k] = 1e300, box[3 + k] = -1e300;
    for (size_t e = 0; e < s->nelements; e++) {
        Arena a;
        arena_init(&a, 0);
        cc_mesh w;
        if (cc_scene_world_mesh(s, e, &a, &w)) continue;
        for (uint32_t i = 0; i < w.nv; i++)
            for (int k = 0; k < 3; k++) {
                double x = s->origin[k] + w.pos[i][k];
                box[k] = x < box[k] ? x : box[k], box[3 + k] = x > box[3 + k] ? x : box[3 + k];
            }
        for (uint32_t i = 0; i + 2 < w.ni; i += 3) {
            double p[3][3];
            for (int c = 0; c < 3; c++)
                for (int k = 0; k < 3; k++) p[c][k] = s->origin[k] + w.pos[w.idx[i + c]][k];
            v += (p[0][0] * (p[1][1] * p[2][2] - p[1][2] * p[2][1]) - p[0][1] * (p[1][0] * p[2][2] - p[1][2] * p[2][0]) +
                  p[0][2] * (p[1][0] * p[2][1] - p[1][1] * p[2][0])) / 6.0;
            double u[3], q[3];
            for (int k = 0; k < 3; k++) u[k] = p[1][k] - p[0][k], q[k] = p[2][k] - p[0][k];
            double c[3] = {u[1] * q[2] - u[2] * q[1], u[2] * q[0] - u[0] * q[2], u[0] * q[1] - u[1] * q[0]};
            ar += cc_sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]) / 2.0;
        }
        arena_free(&a);
    }
    *vol = v, *area = ar;
}
static int rel(double a, double b) {
    double d = a - b, m = b < 0 ? -b : b;
    return (d < 0 ? -d : d) <= 1e-6 * (m > 1 ? m : 1);
}

/* One case: the model's scene with the kernel and without; `cut` 1: the special case applies. */
static void run_case(const char *name, const char *cls, const char *gen, const char *openings, double hand, int cut) {
    static char text[16384];
    snprintf(text, sizeof text, HEAD, cls, gen, openings);
    cc_model m;
    cc_error err;
    char what[256];
    if (cc_model_load(&m, text, strlen(text), &err)) {
        printf("  %s: load: %s\n", name, err.msg);
        snprintf(what, sizeof what, "%s: loads", name);
        check_true(what, 0);
        return;
    }
    const cc_boolean *kernel = cc_boolean_kernel();
    double kv = 0, ka = 0, kb[6] = {0};
    for (int pass = kernel ? 0 : 1; pass < 2; pass++) {
        cc_boolean_set(pass ? NULL : kernel);
        cc_scene s;
        if (cc_scene_build(&s, &m, &err)) {
            snprintf(what, sizeof what, "%s: scene (%s)", name, pass ? "no kernel" : "kernel");
            check_true(what, 0);
            continue;
        }
        double v, ar, b[6];
        measure(&s, &v, &ar, b);
        printf("  %-22s %-9s volume %.10g area %.10g, %zu warning(s)\n", name, pass ? "no kernel" : "kernel", v, ar, s.nwarnings);
        if (!pass) {
            kv = v, ka = ar, memcpy(kb, b, sizeof kb);
            snprintf(what, sizeof what, "%s: the kernel's volume is the hand's", name);
            check_true(what, rel(v, hand));
        } else if (cut) {
            snprintf(what, sizeof what, "%s: without a kernel, cut: the hand's volume, no warning", name);
            check_true(what, rel(v, hand) && s.nwarnings == 0);
            if (kernel) {
                int same = rel(ar, ka);
                for (int k = 0; k < 6; k++) same &= rel(b[k], kb[k]);
                snprintf(what, sizeof what, "%s: without a kernel, the kernel's volume, area and box (1e-6)", name);
                check_true(what, same && rel(v, kv));
            }
        } else {
            snprintf(what, sizeof what, "%s: not a special case: uncut, warned", name);
            check_true(what, s.nwarnings == 1 && strstr(s.warnings[0].p, "not cut (no boolean kernel") != NULL);
        }
        cc_scene_free(&s);
    }
    cc_boolean_set(kernel);
    cc_model_free(&m);
}

/* An 8-corner box mesh generator (12 triangles, outward). */
static void box_mesh(char *out, size_t cap, double x0, double y0, double z0, double x1, double y1, double z1) {
    double P[8][3] = {{x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0}, {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}};
    static const int T[36] = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4, 1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7};
    size_t n = (size_t)snprintf(out, cap, "{\"kind\":\"mesh\",\"positions\":[");
    for (int i = 0; i < 8; i++) n += (size_t)snprintf(out + n, cap - n, "%s[%g,%g,%g]", i ? "," : "", P[i][0], P[i][1], P[i][2]);
    n += (size_t)snprintf(out + n, cap - n, "],\"indices\":[");
    for (int i = 0; i < 36; i++) n += (size_t)snprintf(out + n, cap - n, "%s%d", i ? "," : "", T[i]);
    snprintf(out + n, cap - n, "]}");
}

int main(void) {
    char o1[2048], o2[2048], o3[2048], g1[1024], g2[1024], g3[1024], ops[8192], pl[512];
    const char *ID = PL;
    (void)ID;
    printf("prism_cut: boolean kernel %s\n", cc_boolean_kernel() ? "manifold" : "none");

    /* A wall 6000 x 200 x 3000: two windows (mesh boxes through, wider than the wall), a door on
     * its foot (z from 0: a face on the host's bottom), opening placements identity. */
    snprintf(pl, sizeof pl, PL, 0., 0., 0., 0., 0., 1., 1., 0., 0.);
    box_mesh(g1, sizeof g1, 500, -300, 900, 1700, 300, 2100);
    box_mesh(g2, sizeof g2, 2500, -100, 900, 3500, 100, 2100); /* exactly the wall's thickness */
    box_mesh(g3, sizeof g3, 4500, -300, 0, 5400, 300, 2200);
    snprintf(ops, sizeof ops, "%s,%s,%s", op_buf(o1, sizeof o1, 0, pl, g1), op_buf(o2, sizeof o2, 1, pl, g2), op_buf(o3, sizeof o3, 2, pl, g3));
    run_case("wall, 2 windows, door", "IfcWall", "{\"kind\":\"wall\",\"length\":6000.0,\"thickness\":200.0,\"height\":3000.0}", ops,
             6000.0 * 200 * 3000 - 1200.0 * 200 * 1200 - 1000.0 * 200 * 1200 - 900.0 * 200 * 2200, 1);

    /* A column 400 x 600 x 3000 pierced across x by a 100 x 200 sleeve (an extruded rectangle
     * turned: its z along the column's x). */
    snprintf(pl, sizeof pl, PL, -400., -50., 1400., 1., 0., 0., 0., 1., 0.);
    snprintf(g1, sizeof g1, "{\"kind\":\"rect_column\",\"width\":100.0,\"depth\":200.0,\"height\":800.0}");
    /* placement: origin (-400, -50, 1400), z = +x, x = +y: the sleeve spans x -400..400, y -100..0, z 1350..1450 */
    op_buf(o1, sizeof o1, 0, pl, g1);
    run_case("column, sleeve across", "IfcColumn", "{\"kind\":\"rect_column\",\"width\":400.0,\"depth\":600.0,\"height\":3000.0}", o1,
             400.0 * 600 * 3000 - 400.0 * 100 * 200, 1);

    /* A rectangular slab 5000 x 4000 x 250 with a shaft 800 x 600 through it. */
    snprintf(pl, sizeof pl, PL, 0., 0., 0., 0., 0., 1., 1., 0., 0.);
    box_mesh(g1, sizeof g1, 1000, 1000, -50, 1800, 1600, 300);
    op_buf(o1, sizeof o1, 0, pl, g1);
    run_case("slab, box shaft", "IfcSlab", "{\"kind\":\"slab\",\"outline\":[[0,0],[5000,0],[5000,4000],[0,4000]],\"thickness\":250.0}", o1,
             5000.0 * 4000 * 250 - 800.0 * 600 * 250, 1);

    /* An L slab with a hexagonal shaft (an extruded polygon, z through) and a box shaft. */
    snprintf(pl, sizeof pl, PL, 0., 0., -100., 0., 0., 1., 1., 0., 0.);
    snprintf(g1, sizeof g1,
             "{\"kind\":\"extruded_polygon\",\"outline\":[[1000,500],[1400,500],[1600,846.4],[1400,1192.8],[1000,1192.8],[800,846.4]],\"height\":500.0}");
    box_mesh(g2, sizeof g2, 4000, 500, -10, 4500, 1500, 260);
    char pl0[512];
    snprintf(pl0, sizeof pl0, PL, 0., 0., 0., 0., 0., 1., 1., 0., 0.);
    snprintf(ops, sizeof ops, "%s,%s", op_buf(o1, sizeof o1, 0, pl, g1), op_buf(o2, sizeof o2, 1, pl0, g2));
    {
        double hex = 0; /* shoelace of the hexagon */
        double H[6][2] = {{1000, 500}, {1400, 500}, {1600, 846.4}, {1400, 1192.8}, {1000, 1192.8}, {800, 846.4}};
        for (int i = 0; i < 6; i++) hex += H[i][0] * H[(i + 1) % 6][1] - H[(i + 1) % 6][0] * H[i][1];
        hex /= 2;
        run_case("L slab, hex + box shafts", "IfcSlab",
                 "{\"kind\":\"slab\",\"outline\":[[0,0],[6000,0],[6000,2000],[2000,2000],[2000,5000],[0,5000]],\"thickness\":250.0}", ops,
                 (6000.0 * 2000 + 2000.0 * 3000) * 250 - hex * 250 - 500.0 * 1000 * 250, 1);
    }

    /* Not special: a recess 100 deep in the L slab (not through), and a shaft on its edge. */
    box_mesh(g1, sizeof g1, 500, 500, 150, 900, 900, 400);
    snprintf(pl, sizeof pl, PL, 0., 0., 0., 0., 0., 1., 1., 0., 0.);
    op_buf(o1, sizeof o1, 0, pl, g1);
    run_case("L slab, recess", "IfcSlab",
             "{\"kind\":\"slab\",\"outline\":[[0,0],[6000,0],[6000,2000],[2000,2000],[2000,5000],[0,5000]],\"thickness\":250.0}", o1,
             (6000.0 * 2000 + 2000.0 * 3000) * 250 - 400.0 * 400 * 100, 0);
    box_mesh(g1, sizeof g1, -200, 500, -10, 300, 900, 300);
    op_buf(o1, sizeof o1, 0, pl, g1);
    run_case("L slab, shaft on the edge", "IfcSlab",
             "{\"kind\":\"slab\",\"outline\":[[0,0],[6000,0],[6000,2000],[2000,2000],[2000,5000],[0,5000]],\"thickness\":250.0}", o1,
             (6000.0 * 2000 + 2000.0 * 3000) * 250 - 300.0 * 400 * 250, 0);
    return report("prism_cut");
}
