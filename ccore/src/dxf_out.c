/* DXF export of a drawing: see dxf_out.h (ticket DXF1, ADR 0082). */
#include "dxf_out.h"
#include <stdlib.h>
#include <string.h>
#include "draw_annot.h" /* cc_turn_apply_point */
#include "draw_style.h" /* cc_category_label */
#include "fmt.h"
#include "ifc_math.h" /* cc_sin, cc_cos */
#include "math.h"

/* ---- colours and weights ---- */

/* The AutoCAD Color Index's RGB (1..255): the nine standard colours, the 24 hues x 10 shades,
 * the six greys. */
static void aci_rgb(int i, int out[3]) {
    static const unsigned char STD[10][3] = {{0, 0, 0},     {255, 0, 0},   {255, 255, 0}, {0, 255, 0},     {0, 255, 255},
                                             {0, 0, 255},   {255, 0, 255}, {255, 255, 255}, {128, 128, 128}, {192, 192, 192}};
    static const int GREY[6] = {51, 91, 132, 173, 214, 255};
    static const int BRIGHT[5] = {255, 165, 127, 76, 38};
    if (i < 10) {
        for (int k = 0; k < 3; k++) out[k] = STD[i][k];
        return;
    }
    if (i >= 250) {
        out[0] = out[1] = out[2] = GREY[i - 250];
        return;
    }
    int h = i / 10 - 1, j = i % 10, deg = h * 15, s = deg / 60, f4 = (deg % 60) / 15; /* f = f4 / 4 */
    int rise = 255 * f4 / 4, fall = 255 * (4 - f4) / 4, c[3];
    switch (s) {
    case 0: c[0] = 255, c[1] = rise, c[2] = 0; break;
    case 1: c[0] = fall, c[1] = 255, c[2] = 0; break;
    case 2: c[0] = 0, c[1] = 255, c[2] = rise; break;
    case 3: c[0] = 0, c[1] = fall, c[2] = 255; break;
    case 4: c[0] = rise, c[1] = 0, c[2] = 255; break;
    default: c[0] = 255, c[1] = 0, c[2] = fall; break;
    }
    for (int k = 0; k < 3; k++) {
        int v = j & 1 ? c[k] + (255 - c[k]) / 2 : c[k];
        out[k] = (v * BRIGHT[j / 2] + 127) / 255;
    }
}

/* The nearest ACI (1..255) to an RGB; ink (dark, all channels <= 32) is 7, the "black or white"
 * of CAD; 7 is never the nearest otherwise (it would draw white fills black). */
static int aci_of(cc_rgb c) {
    if (c.c[0] <= 32 && c.c[1] <= 32 && c.c[2] <= 32) return 7;
    int best = 1;
    long bd = -1;
    for (int i = 1; i <= 255; i++) {
        if (i == 7) continue;
        int r[3];
        aci_rgb(i, r);
        long d = 0;
        for (int k = 0; k < 3; k++) d += (long)(r[k] - c.c[k]) * (r[k] - c.c[k]);
        if (bd < 0 || d < bd) bd = d, best = i;
    }
    return best;
}

static long rgb_int(cc_rgb c) { return ((long)c.c[0] << 16) | ((long)c.c[1] << 8) | (long)c.c[2]; }

/* The nearest standard lineweight (1/100 mm) to a paper width in mm. */
static int lineweight_of(double mm) {
    static const int LW[24] = {0, 5, 9, 13, 15, 18, 20, 25, 30, 35, 40, 50, 53, 60, 70, 80, 90, 100, 106, 120, 140, 158, 200, 211};
    double w = mm * 100.0;
    int best = 0;
    double bd = -1.0;
    for (int i = 0; i < 24; i++) {
        double d = w - (double)LW[i];
        if (d < 0) d = -d;
        if (bd < 0 || d < bd) bd = d, best = LW[i];
    }
    return best;
}

/* ---- the writer ---- */

typedef struct {
    char *name; /* NUL-terminated, sanitized */
    size_t n;
    cc_rgb color;
    int lw, lt; /* lineweight; line type index (-1 continuous) */
    uint32_t handle;
} Layer;

typedef struct {
    double d[8];
    size_t n;
    char name[16];
    uint32_t handle;
} Ltype;

typedef struct {
    const cc_dxf_options *o;
    const cc_paper *p;
    int dry; /* pass 1: nothing written; layers, line types, extents and handles counted */
    char *chunk;
    size_t len;
    int err;
    uint32_t next; /* the next entity handle */
    uint32_t h_ms; /* *Model_Space's block record (the entities' owner) */
    Layer *layers;
    size_t nl, capl;
    Ltype lts[32];
    size_t nlt;
    double lo[2], hi[2];
    int any;
    Buf num, name;
    double turn_off[2]; /* a view: the turned scene origin (model = scene + this) */
} W;

static void flush(W *w) {
    if (w->dry || !w->len || w->err) return;
    if (w->o->sink(w->o->sink_ctx, w->chunk, w->len)) w->err = CC_DXF_SINK;
    w->len = 0;
}

static void put(W *w, const char *s, size_t n) {
    if (w->dry || w->err) return;
    while (n) {
        size_t room = CC_DXF_CHUNK - w->len, k = n < room ? n : room;
        memcpy(w->chunk + w->len, s, k);
        w->len += k, s += k, n -= k;
        if (w->len == CC_DXF_CHUNK) flush(w);
        if (w->err) return;
    }
}

static void put_code(W *w, int code) {
    char b[8];
    int n = 0;
    char d[6];
    int k = 0;
    do d[k++] = (char)('0' + code % 10), code /= 10;
    while (code && k < 5);
    for (int i = k; i < 3; i++) b[n++] = ' ';
    while (k) b[n++] = d[--k];
    b[n++] = '\n';
    put(w, b, (size_t)n);
}

static void g_str(W *w, int code, const char *s) {
    put_code(w, code);
    put(w, s, strlen(s));
    put(w, "\n", 1);
}

static void g_int(W *w, int code, long v) {
    if (w->dry) return;
    put_code(w, code);
    buf_clear(&w->num);
    buf_i64(&w->num, v);
    put(w, w->num.p, w->num.len);
    put(w, "\n", 1);
}

static void g_hex(W *w, int code, uint32_t h) {
    if (w->dry) return;
    char b[9];
    int k = 0;
    do b[k++] = "0123456789ABCDEF"[h & 15], h >>= 4;
    while (h);
    char o[9];
    for (int i = 0; i < k; i++) o[i] = b[k - 1 - i];
    put_code(w, code);
    put(w, o, (size_t)k);
    put(w, "\n", 1);
}

static void g_num(W *w, int code, double x) {
    if (w->dry) return;
    if (x == 0.0) x = 0.0; /* no -0 */
    buf_clear(&w->num);
    if (fmt_f64_fixed(&w->num, x, -1)) {
        w->err = CC_DXF_NONFINITE;
        return;
    }
    put_code(w, code);
    put(w, w->num.p, w->num.len);
    put(w, "\n", 1);
}

/* A text value: ASCII as it is, any other character \U+XXXX (UTF-16 units), control characters
 * dropped, a broken UTF-8 byte '?'; at most ~250 bytes (DXF's string limit). */
static void g_text(W *w, int code, const char *s, size_t n) {
    if (w->dry) return;
    put_code(w, code);
    size_t out = 0;
    const unsigned char *u = (const unsigned char *)s;
    for (size_t i = 0; i < n && out < 240;) {
        unsigned c = u[i];
        uint32_t cp;
        size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        int ok = len && i + len <= n;
        for (size_t k = 1; ok && k < len; k++) ok = (u[i + k] & 0xC0) == 0x80;
        if (!ok) {
            put(w, "?", 1), out++, i++;
            continue;
        }
        cp = len == 1 ? c : len == 2 ? c & 0x1F : len == 3 ? c & 0x0F : c & 0x07;
        for (size_t k = 1; k < len; k++) cp = (cp << 6) | (u[i + k] & 0x3F);
        i += len;
        if (cp < 0x20 || cp == 0x7F) continue;
        if (cp < 0x80) {
            char ch = (char)cp;
            put(w, &ch, 1), out++;
            continue;
        }
        uint32_t units[2];
        int nu = 1;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            units[0] = 0xD800 + (cp >> 10), units[1] = 0xDC00 + (cp & 0x3FF), nu = 2;
        } else units[0] = cp;
        for (int q = 0; q < nu; q++) {
            char e[7] = {'\\', 'U', '+', 0, 0, 0, 0};
            for (int k = 0; k < 4; k++) e[3 + k] = "0123456789ABCDEF"[(units[q] >> (12 - 4 * k)) & 15];
            put(w, e, 7), out += 7;
        }
    }
    put(w, "\n", 1);
}

/* ---- coordinates ---- */

/* Rounded to 0.001 mm (the shortest text of the double is then at most 3 decimals). */
static double round3(double x) {
    double t = x * 1000.0;
    if (!(t > -4e15 && t < 4e15)) return x;
    double r = (double)(int64_t)(t + (t >= 0 ? 0.5 : -0.5));
    return r / 1000.0 + 0.0;
}

/* Paper -> the file's mm. */
static void to_file(const W *w, const double p[2], double out[2]) {
    if (w->o->sheet) {
        out[0] = round3(p[0]);
        out[1] = round3(w->p->height_mm - p[1]);
        return;
    }
    const cc_view_frame *f = &w->o->frame;
    out[0] = round3(f->x0 + (p[0] - f->ox) / f->k + w->turn_off[0]);
    out[1] = round3(f->y1 - (p[1] - f->oy) / f->k + w->turn_off[1]);
}

/* A paper length -> the file's. */
static double len_file(const W *w, double mm) { return round3(w->o->sheet ? mm : mm / w->o->frame.k); }

static void extend(W *w, const double q[2]) {
    if (!w->dry) return;
    for (int k = 0; k < 2; k++) {
        if (!w->any || q[k] < w->lo[k]) w->lo[k] = q[k];
        if (!w->any || q[k] > w->hi[k]) w->hi[k] = q[k];
    }
    w->any = 1;
}

static void g_xy(W *w, int code, const double p[2]) {
    double q[2];
    to_file(w, p, q);
    extend(w, q);
    g_num(w, code, q[0]);
    g_num(w, code + 10, q[1]);
}

/* ---- layers and line types ---- */

/* AutoCAD's forbidden characters in a name: < > / \ " : ; ? * | , = ` and control. */
static int forbidden(unsigned char c) {
    static const char F[] = "<>/\\\":;?*|,=`";
    for (size_t i = 0; i < sizeof F - 1; i++)
        if ((unsigned char)F[i] == c) return 1;
    return 0;
}

static void name_part(Buf *b, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        int bad = c < 0x20 || c == 0x7F || forbidden(c);
        buf_putc(b, bad ? '_' : (char)c);
    }
}

typedef enum { PART_LINE, PART_FILL, PART_TEXT } Part;

/* The layer name of an item's part (ring tag `t`) into w->name; 0, or 1 to leave it out. */
static int layer_name(W *w, const cc_item_tag *t, Part part) {
    Buf *b = &w->name;
    buf_clear(b);
    if (t->role == CC_TAG_PAPER && !w->o->sheet) return 1;
    if (t->elem1 && w->o->link_name) {
        size_t n = 0;
        const char *ln = w->o->link_name(w->o->link_ctx, t->elem1 - 1, &n);
        if (ln && n) {
            name_part(b, ln, n);
            buf_putc(b, '-');
        }
    }
    int fill = part == PART_FILL && (t->role == CC_TAG_HATCH || t->role == CC_TAG_VIEW);
    if (fill) {
        buf_puts(b, "Hatch-");
        buf_puts(b, t->cat ? cc_category_label((cc_category)(t->cat - 1)) : "Other");
        if (t->role == CC_TAG_VIEW) buf_puts(b, "-view");
    } else {
        switch ((cc_tag_role)t->role) {
        case CC_TAG_CUT:
        case CC_TAG_VIEW:
        case CC_TAG_ABOVE:
        case CC_TAG_HATCH:
            if (t->cls && t->cls_n) name_part(b, t->cls, t->cls_n);
            else buf_puts(b, "IfcBuildingElementProxy");
            buf_puts(b, t->role == CC_TAG_CUT || t->role == CC_TAG_HATCH ? "-cut" : t->role == CC_TAG_VIEW ? "-view" : "-above");
            break;
        case CC_TAG_GRID: buf_puts(b, "IfcGrid"); break;
        case CC_TAG_DIMENSION: buf_puts(b, "Annotation-dimension"); break;
        case CC_TAG_TEXT: buf_puts(b, "Annotation-text"); break;
        case CC_TAG_SYMBOL: buf_puts(b, "Annotation-symbol"); break;
        default: buf_puts(b, "Sheet"); break;
        }
    }
    if (b->len > 250) b->len = 250;
    buf_cstr(b);
    if (b->oom) w->err = CC_DXF_OOM;
    return 0;
}

static int layer_cmp(const char *a, size_t na, const char *b, size_t nb) {
    size_t n = na < nb ? na : nb;
    int c = memcmp(a, b, n);
    return c ? c : na < nb ? -1 : na > nb;
}

/* The index of w->name among the layers (sorted), or where it goes (*found 0). */
static size_t layer_find(const W *w, int *found) {
    size_t lo = 0, hi = w->nl;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = layer_cmp(w->layers[mid].name, w->layers[mid].n, w->name.p, w->name.len);
        if (!c) return *found = 1, mid;
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    *found = 0;
    return lo;
}

/* The line type of a dash pattern (paper mm), -1 continuous; added in pass 1. */
static int ltype_of(W *w, const double *d, size_t n) {
    if (!d || !n) return -1;
    double e[8];
    size_t m = n % 2 ? 2 * n : n; /* SVG repeats an odd list */
    if (m > 8) m = 8;
    for (size_t i = 0; i < m; i++) e[i] = d[i % n];
    for (size_t i = 0; i < w->nlt; i++)
        if (w->lts[i].n == m && !memcmp(w->lts[i].d, e, m * sizeof *e)) return (int)i;
    if (!w->dry || w->nlt == 32) return -1;
    Ltype *t = &w->lts[w->nlt];
    memcpy(t->d, e, m * sizeof *e);
    t->n = m;
    const char *base = m == 2 ? "DASHED" : m == 4 ? "DASHDOT" : "DASH";
    size_t same = 0;
    for (size_t i = 0; i < w->nlt; i++)
        if (!strncmp(w->lts[i].name, base, strlen(base)) && (w->lts[i].n == m)) same++;
    Buf b;
    buf_init(&b);
    buf_puts(&b, base);
    if (same) buf_u64(&b, same + 1);
    memset(t->name, 0, sizeof t->name);
    if (!b.oom) memcpy(t->name, b.p, b.len < 15 ? b.len : 15);
    buf_free(&b);
    return (int)w->nlt++;
}

/* The layer of the entity about to be written (w->name), its colour, weight and type: pass 1
 * adds it (the first entity's), pass 2 writes 8 and what differs. */
static void g_layer(W *w, cc_rgb color, int lw, int lt) {
    int found;
    size_t at = layer_find(w, &found);
    if (w->dry) {
        if (found) return;
        if (w->nl == w->capl) {
            size_t c = w->capl ? 2 * w->capl : 32;
            Layer *q = realloc(w->layers, c * sizeof *q);
            if (!q) {
                w->err = CC_DXF_OOM;
                return;
            }
            w->layers = q, w->capl = c;
        }
        char *nm = malloc(w->name.len + 1);
        if (!nm) {
            w->err = CC_DXF_OOM;
            return;
        }
        memcpy(nm, w->name.p, w->name.len + 1);
        memmove(&w->layers[at + 1], &w->layers[at], (w->nl - at) * sizeof *w->layers);
        Layer L = {nm, w->name.len, color, lw, lt, 0};
        w->layers[at] = L;
        w->nl++;
        return;
    }
    const Layer *L = found ? &w->layers[at] : NULL;
    g_text(w, 8, w->name.p, w->name.len);
    if (L && L->lt != lt) g_str(w, 6, lt < 0 ? "Continuous" : w->lts[lt].name);
    if (L && memcmp(L->color.c, color.c, 3)) g_int(w, 62, aci_of(color)), g_int(w, 420, rgb_int(color));
    if (L && L->lw != lw) g_int(w, 370, lw);
}

/* The head of an entity: its type, handle, owner, layer and properties. */
static void entity(W *w, const char *type, cc_rgb color, int lw, int lt) {
    uint32_t h = w->next++;
    if (!w->dry) {
        g_str(w, 0, type);
        g_hex(w, 5, h);
        g_hex(w, 330, w->h_ms);
        g_str(w, 100, "AcDbEntity");
    }
    g_layer(w, color, lw, lt);
}

/* ---- entities ---- */

/* The same point up to the last bits (a closed circle's end: c + r cos(2 pi) against c + r). */
static int same_pt(const double a[2], const double b[2]) {
    for (int k = 0; k < 2; k++) {
        double d = a[k] - b[k], m = a[k] < 0 ? -a[k] : a[k];
        if (d < 0) d = -d;
        if (d > 1e-9 * (m > 1 ? m : 1)) return 0;
    }
    return 1;
}

/* A ring of 16 points or more on one circle (paper mm): 1 with its centre and radius. */
static int is_circle(const cc_ring *r, double c[2], double *rad) {
    size_t n = r->n;
    if (n >= 2 && same_pt(r->p[0], r->p[n - 1])) n--;
    if (n < 16) return 0;
    double sx = 0, sy = 0;
    for (size_t i = 0; i < n; i++) sx += r->p[i][0], sy += r->p[i][1];
    c[0] = sx / (double)n, c[1] = sy / (double)n;
    double r0 = cc_hypot(r->p[0][0] - c[0], r->p[0][1] - c[1]);
    if (!(r0 > 1e-6)) return 0;
    for (size_t i = 1; i < n; i++) {
        double d = cc_hypot(r->p[i][0] - c[0], r->p[i][1] - c[1]) - r0;
        if (d > 1e-6 * r0 || d < -1e-6 * r0) return 0;
    }
    *rad = r0;
    return 1;
}

/* A ring as LINE, LWPOLYLINE or CIRCLE on the layer in w->name. */
static void ring_entity(W *w, const cc_ring *r, int closed, cc_rgb color, int lw, int lt) {
    if (r->n < 2) return;
    double c[2], rad;
    if (is_circle(r, c, &rad)) {
        entity(w, "CIRCLE", color, lw, lt);
        if (!w->dry) g_str(w, 100, "AcDbCircle");
        g_xy(w, 10, c);
        double cr = len_file(w, rad), q[2];
        to_file(w, c, q);
        if (w->dry) {
            double a[2] = {q[0] - cr, q[1] - cr}, b[2] = {q[0] + cr, q[1] + cr};
            extend(w, a), extend(w, b);
        }
        g_num(w, 40, cr);
        return;
    }
    size_t n = r->n;
    if (n > 2 && same_pt(r->p[0], r->p[n - 1])) n--, closed = 1;
    if (n == 2 && !closed) {
        entity(w, "LINE", color, lw, lt);
        if (!w->dry) g_str(w, 100, "AcDbLine");
        g_xy(w, 10, r->p[0]);
        g_xy(w, 11, r->p[1]);
        return;
    }
    entity(w, "LWPOLYLINE", color, lw, lt);
    if (!w->dry) g_str(w, 100, "AcDbPolyline");
    g_int(w, 90, (long)n);
    g_int(w, 70, closed ? 1 : 0);
    for (size_t i = 0; i < n; i++) g_xy(w, 10, r->p[i]);
}

/* The boundary paths of a HATCH: one closed polyline per ring. */
static void hatch_paths(W *w, const cc_item *it) {
    size_t np = 0;
    for (size_t i = 0; i < it->nrings; i++) np += it->rings[i].n >= 3;
    g_int(w, 91, (long)np);
    for (size_t i = 0; i < it->nrings; i++) {
        const cc_ring *r = &it->rings[i];
        if (r->n < 3) continue;
        size_t n = r->n;
        if (same_pt(r->p[0], r->p[n - 1])) n--;
        g_int(w, 92, 2); /* a polyline */
        g_int(w, 72, 0); /* no bulges */
        g_int(w, 73, 1); /* closed */
        g_int(w, 93, (long)n);
        for (size_t k = 0; k < n; k++) g_xy(w, 10, r->p[k]);
        g_int(w, 97, 0);
    }
}

static void hatch_head(W *w, const cc_item *it, const char *pattern, int solid, cc_rgb color, int lw) {
    entity(w, "HATCH", color, lw, -1);
    if (!w->dry) {
        g_str(w, 100, "AcDbHatch");
        g_num(w, 10, 0), g_num(w, 20, 0), g_num(w, 30, 0);
        g_num(w, 210, 0), g_num(w, 220, 0), g_num(w, 230, 1);
        g_str(w, 2, pattern);
        g_int(w, 70, solid);
        g_int(w, 71, 0);
    }
    hatch_paths(w, it);
    g_int(w, 75, 0); /* odd parity: holes stay empty */
    g_int(w, 76, solid ? 1 : 0);
}

static void area(W *w, const cc_item *it) {
    int has_ring = 0;
    for (size_t i = 0; i < it->nrings; i++) has_ring |= it->rings[i].n >= 3;
    if (has_ring && (it->has_fill || it->nhatches) && !layer_name(w, &it->tag, PART_FILL)) {
        int white = it->fill.c[0] == 255 && it->fill.c[1] == 255 && it->fill.c[2] == 255;
        if (it->has_fill && !white) {
            hatch_head(w, it, "SOLID", 1, it->fill, 0);
            g_int(w, 98, 0);
        }
        for (size_t h = 0; h < it->nhatches; h++) {
            const cc_hatch *ht = &it->hatches[h];
            if (!(ht->spacing_mm > 0)) continue;
            hatch_head(w, it, "_USER", 0, ht->pen.color, lineweight_of(ht->pen.width_mm));
            double a = ht->angle_deg, rad = a * 0.017453292519943295;
            double sp = len_file(w, ht->spacing_mm), sh = len_file(w, ht->offset_mm);
            /* The lines' direction (cos a, sin a); their offset perpendicular to it. */
            double ca = cc_cos(rad), sa = cc_sin(rad);
            g_num(w, 52, a);
            g_num(w, 41, sp);
            g_int(w, 77, 0);
            g_int(w, 78, 1);
            g_num(w, 53, a);
            g_num(w, 43, round3(-sa * sh));
            g_num(w, 44, round3(ca * sh));
            g_num(w, 45, round3(-sa * sp));
            g_num(w, 46, round3(ca * sp));
            g_int(w, 79, 0);
            g_int(w, 98, 0);
        }
    }
    if (it->has_pen && !layer_name(w, &it->tag, PART_LINE)) {
        int lw = lineweight_of(it->pen.width_mm), lt = ltype_of(w, it->pen.dash, it->pen.ndash);
        for (size_t i = 0; i < it->nrings; i++) ring_entity(w, &it->rings[i], 1, it->pen.color, lw, lt);
    }
}

static void lines(W *w, const cc_item *it) {
    int lw = lineweight_of(it->pen.width_mm), lt = ltype_of(w, it->pen.dash, it->pen.ndash);
    for (size_t i = 0; i < it->nrings; i++) {
        if (layer_name(w, it->ring_tags ? &it->ring_tags[i] : &it->tag, PART_LINE)) continue;
        ring_entity(w, &it->rings[i], 0, it->pen.color, lw, lt);
    }
}

static void text(W *w, const cc_item *it) {
    if (!it->text_len || layer_name(w, &it->tag, PART_TEXT)) return;
    entity(w, "TEXT", it->color, 0, -1);
    if (!w->dry) g_str(w, 100, "AcDbText");
    g_xy(w, 10, it->pos);
    g_num(w, 40, len_file(w, it->size_mm));
    g_text(w, 1, it->text, it->text_len);
    if (it->angle_deg != 0.0) g_num(w, 50, it->angle_deg);
    if (!w->dry) g_str(w, 7, "Standard");
    if (it->anchor != CC_ANCHOR_START) {
        g_int(w, 72, it->anchor == CC_ANCHOR_MIDDLE ? 1 : 2);
        g_xy(w, 11, it->pos);
    }
    if (!w->dry) g_str(w, 100, "AcDbText");
}

static void entities(W *w) {
    for (size_t i = 0; i < w->p->nitems && !w->err; i++) {
        const cc_item *it = &w->p->items[i];
        if (it->kind == CC_ITEM_AREA) area(w, it);
        else if (it->kind == CC_ITEM_LINES) lines(w, it);
        else text(w, it);
    }
}

/* ---- the sections ---- */

static void table_head(W *w, const char *name, uint32_t h, size_t n) {
    g_str(w, 0, "TABLE");
    g_str(w, 2, name);
    g_hex(w, 5, h);
    g_str(w, 330, "0");
    g_str(w, 100, "AcDbSymbolTable");
    g_int(w, 70, (long)n);
}

static void record_head(W *w, const char *type, uint32_t h, uint32_t owner, const char *sub) {
    g_str(w, 0, type);
    g_hex(w, 5, h);
    g_hex(w, 330, owner);
    g_str(w, 100, "AcDbSymbolTableRecord");
    g_str(w, 100, sub);
}

static void ltype_record(W *w, uint32_t h, uint32_t owner, const char *name, const char *desc, const double *d, size_t n) {
    record_head(w, "LTYPE", h, owner, "AcDbLinetypeTableRecord");
    g_str(w, 2, name);
    g_int(w, 70, 0);
    g_str(w, 3, desc);
    g_int(w, 72, 65);
    g_int(w, 73, (long)n);
    double total = 0;
    for (size_t i = 0; i < n; i++) total += d[i];
    g_num(w, 40, total);
    for (size_t i = 0; i < n; i++) {
        g_num(w, 49, i % 2 ? -d[i] : d[i]);
        g_int(w, 74, 0);
    }
}

static void layout(W *w, uint32_t h, uint32_t owner, const char *name, int order, uint32_t block_record) {
    g_str(w, 0, "LAYOUT");
    g_hex(w, 5, h);
    g_hex(w, 330, owner);
    g_str(w, 100, "AcDbPlotSettings");
    g_str(w, 1, "");
    g_str(w, 2, "none_device");
    g_str(w, 4, "ISO_A3_(420.00_x_297.00_MM)");
    g_str(w, 6, "");
    for (int c = 40; c <= 49; c++) g_num(w, c, 0);
    g_num(w, 140, 0), g_num(w, 141, 0), g_num(w, 142, 1), g_num(w, 143, 1);
    g_int(w, 70, 688);
    g_int(w, 72, 1);
    g_int(w, 73, 0);
    g_int(w, 74, 5);
    g_str(w, 7, "");
    g_int(w, 75, 16);
    g_num(w, 147, 1), g_num(w, 148, 0), g_num(w, 149, 0);
    g_str(w, 100, "AcDbLayout");
    g_str(w, 1, name);
    g_int(w, 70, 1);
    g_int(w, 71, order);
    g_num(w, 10, 0), g_num(w, 20, 0);
    g_num(w, 11, 420), g_num(w, 21, 297);
    g_num(w, 12, 0), g_num(w, 22, 0), g_num(w, 32, 0);
    g_num(w, 14, 0), g_num(w, 24, 0), g_num(w, 34, 0);
    g_num(w, 15, 0), g_num(w, 25, 0), g_num(w, 35, 0);
    g_num(w, 146, 0);
    g_num(w, 13, 0), g_num(w, 23, 0), g_num(w, 33, 0);
    g_num(w, 16, 1), g_num(w, 26, 0), g_num(w, 36, 0);
    g_num(w, 17, 0), g_num(w, 27, 1), g_num(w, 37, 0);
    g_int(w, 76, 0);
    g_hex(w, 330, block_record);
}

static void header_var_xyz(W *w, const char *name, double x, double y, double z) {
    g_str(w, 9, name);
    g_num(w, 10, x), g_num(w, 20, y), g_num(w, 30, z);
}

static void write_file(W *w) {
    /* Handles: the tables, blocks and objects first, then the entities. */
    uint32_t h = 1;
    uint32_t H_VPORT_T = h++, H_VPORT = h++, H_LTYPE_T = h++, H_LT_BB = h++, H_LT_BL = h++, H_LT_C = h++;
    for (size_t i = 0; i < w->nlt; i++) w->lts[i].handle = h++;
    uint32_t H_LAYER_T = h++, H_LAYER0 = h++;
    for (size_t i = 0; i < w->nl; i++) w->layers[i].handle = h++;
    uint32_t H_STYLE_T = h++, H_STYLE = h++, H_VIEW_T = h++, H_UCS_T = h++, H_APPID_T = h++, H_APPID = h++;
    uint32_t H_DIM_T = h++, H_DIM = h++, H_BR_T = h++, H_BR_MS = h++, H_BR_PS = h++;
    uint32_t H_BLK_MS = h++, H_END_MS = h++, H_BLK_PS = h++, H_END_PS = h++;
    uint32_t H_ROOT = h++, H_GROUP = h++, H_LAYDICT = h++, H_PSDICT = h++, H_PH = h++, H_LAYOUT_M = h++, H_LAYOUT_1 = h++;
    w->h_ms = H_BR_MS;
    uint32_t first = h, nent = w->next; /* pass 1 counted the entities from 0 */
    w->next = first;
    double lo[2] = {w->any ? w->lo[0] : 0, w->any ? w->lo[1] : 0}, hi[2] = {w->any ? w->hi[0] : 0, w->any ? w->hi[1] : 0};

    g_str(w, 999, "Bimble DXF export (AC1015, millimetres)");
    /* HEADER */
    g_str(w, 0, "SECTION");
    g_str(w, 2, "HEADER");
    g_str(w, 9, "$ACADVER"), g_str(w, 1, "AC1015");
    g_str(w, 9, "$DWGCODEPAGE"), g_str(w, 3, "ANSI_1252");
    header_var_xyz(w, "$INSBASE", 0, 0, 0);
    header_var_xyz(w, "$EXTMIN", lo[0], lo[1], 0);
    header_var_xyz(w, "$EXTMAX", hi[0], hi[1], 0);
    g_str(w, 9, "$LTSCALE"), g_num(w, 40, w->o->sheet ? 1.0 : (double)w->o->scale);
    g_str(w, 9, "$MEASUREMENT"), g_int(w, 70, 1);
    g_str(w, 9, "$INSUNITS"), g_int(w, 70, 4);
    g_str(w, 9, "$LUNITS"), g_int(w, 70, 2);
    g_str(w, 9, "$HANDSEED"), g_hex(w, 5, first + nent);
    g_str(w, 0, "ENDSEC");
    /* CLASSES */
    g_str(w, 0, "SECTION"), g_str(w, 2, "CLASSES"), g_str(w, 0, "ENDSEC");
    /* TABLES */
    g_str(w, 0, "SECTION");
    g_str(w, 2, "TABLES");
    table_head(w, "VPORT", H_VPORT_T, 1);
    record_head(w, "VPORT", H_VPORT, H_VPORT_T, "AcDbViewportTableRecord");
    g_str(w, 2, "*ACTIVE");
    g_int(w, 70, 0);
    g_num(w, 10, 0), g_num(w, 20, 0), g_num(w, 11, 1), g_num(w, 21, 1);
    g_num(w, 12, round3((lo[0] + hi[0]) / 2)), g_num(w, 22, round3((lo[1] + hi[1]) / 2));
    g_num(w, 13, 0), g_num(w, 23, 0), g_num(w, 14, 10), g_num(w, 24, 10), g_num(w, 15, 10), g_num(w, 25, 10);
    g_num(w, 16, 0), g_num(w, 26, 0), g_num(w, 36, 1), g_num(w, 17, 0), g_num(w, 27, 0), g_num(w, 37, 0);
    double vh = hi[1] - lo[1], vw = hi[0] - lo[0];
    if (vh < vw / 1.5) vh = vw / 1.5;
    g_num(w, 40, round3(vh > 0 ? vh * 1.1 : 1000)), g_num(w, 41, 1.5), g_num(w, 42, 50);
    g_num(w, 43, 0), g_num(w, 44, 0), g_num(w, 50, 0), g_num(w, 51, 0);
    g_int(w, 71, 0), g_int(w, 72, 100), g_int(w, 73, 1), g_int(w, 74, 3), g_int(w, 75, 0), g_int(w, 76, 0);
    g_int(w, 77, 0), g_int(w, 78, 0);
    g_str(w, 0, "ENDTAB");

    table_head(w, "LTYPE", H_LTYPE_T, 3 + w->nlt);
    ltype_record(w, H_LT_BB, H_LTYPE_T, "ByBlock", "", NULL, 0);
    ltype_record(w, H_LT_BL, H_LTYPE_T, "ByLayer", "", NULL, 0);
    ltype_record(w, H_LT_C, H_LTYPE_T, "Continuous", "Solid line", NULL, 0);
    for (size_t i = 0; i < w->nlt; i++) {
        Buf d;
        buf_init(&d);
        for (size_t k = 0; k < w->lts[i].n; k++) {
            buf_puts(&d, k % 2 ? "_ " : "__ ");
        }
        ltype_record(w, w->lts[i].handle, H_LTYPE_T, w->lts[i].name, d.oom ? "" : buf_cstr(&d), w->lts[i].d, w->lts[i].n);
        buf_free(&d);
    }
    g_str(w, 0, "ENDTAB");

    table_head(w, "LAYER", H_LAYER_T, 1 + w->nl);
    record_head(w, "LAYER", H_LAYER0, H_LAYER_T, "AcDbLayerTableRecord");
    g_str(w, 2, "0"), g_int(w, 70, 0), g_int(w, 62, 7), g_str(w, 6, "Continuous"), g_int(w, 370, -3);
    g_hex(w, 390, H_PH);
    for (size_t i = 0; i < w->nl; i++) {
        const Layer *L = &w->layers[i];
        record_head(w, "LAYER", L->handle, H_LAYER_T, "AcDbLayerTableRecord");
        g_text(w, 2, L->name, L->n);
        g_int(w, 70, 0);
        g_int(w, 62, aci_of(L->color));
        g_int(w, 420, rgb_int(L->color));
        g_str(w, 6, L->lt < 0 ? "Continuous" : w->lts[L->lt].name);
        g_int(w, 370, L->lw);
        g_hex(w, 390, H_PH);
    }
    g_str(w, 0, "ENDTAB");

    table_head(w, "STYLE", H_STYLE_T, 1);
    record_head(w, "STYLE", H_STYLE, H_STYLE_T, "AcDbTextStyleTableRecord");
    g_str(w, 2, "Standard"), g_int(w, 70, 0), g_num(w, 40, 0), g_num(w, 41, 1), g_num(w, 50, 0), g_int(w, 71, 0);
    g_num(w, 42, 2.5), g_str(w, 3, "arial.ttf"), g_str(w, 4, "");
    g_str(w, 0, "ENDTAB");

    table_head(w, "VIEW", H_VIEW_T, 0);
    g_str(w, 0, "ENDTAB");
    table_head(w, "UCS", H_UCS_T, 0);
    g_str(w, 0, "ENDTAB");

    table_head(w, "APPID", H_APPID_T, 1);
    record_head(w, "APPID", H_APPID, H_APPID_T, "AcDbRegAppTableRecord");
    g_str(w, 2, "ACAD"), g_int(w, 70, 0);
    g_str(w, 0, "ENDTAB");

    table_head(w, "DIMSTYLE", H_DIM_T, 1);
    g_str(w, 100, "AcDbDimStyleTable");
    g_int(w, 71, 0);
    g_str(w, 0, "DIMSTYLE");
    g_hex(w, 105, H_DIM);
    g_hex(w, 330, H_DIM_T);
    g_str(w, 100, "AcDbSymbolTableRecord");
    g_str(w, 100, "AcDbDimStyleTableRecord");
    g_str(w, 2, "Standard"), g_int(w, 70, 0);
    g_str(w, 0, "ENDTAB");

    table_head(w, "BLOCK_RECORD", H_BR_T, 2);
    record_head(w, "BLOCK_RECORD", H_BR_MS, H_BR_T, "AcDbBlockTableRecord");
    g_str(w, 2, "*Model_Space"), g_hex(w, 340, H_LAYOUT_M);
    record_head(w, "BLOCK_RECORD", H_BR_PS, H_BR_T, "AcDbBlockTableRecord");
    g_str(w, 2, "*Paper_Space"), g_hex(w, 340, H_LAYOUT_1);
    g_str(w, 0, "ENDTAB");
    g_str(w, 0, "ENDSEC");

    /* BLOCKS */
    g_str(w, 0, "SECTION");
    g_str(w, 2, "BLOCKS");
    for (int ps = 0; ps < 2; ps++) {
        const char *nm = ps ? "*Paper_Space" : "*Model_Space";
        uint32_t br = ps ? H_BR_PS : H_BR_MS;
        g_str(w, 0, "BLOCK"), g_hex(w, 5, ps ? H_BLK_PS : H_BLK_MS), g_hex(w, 330, br), g_str(w, 100, "AcDbEntity");
        if (ps) g_int(w, 67, 1);
        g_str(w, 8, "0"), g_str(w, 100, "AcDbBlockBegin"), g_str(w, 2, nm), g_int(w, 70, 0);
        g_num(w, 10, 0), g_num(w, 20, 0), g_num(w, 30, 0), g_str(w, 3, nm), g_str(w, 1, "");
        g_str(w, 0, "ENDBLK"), g_hex(w, 5, ps ? H_END_PS : H_END_MS), g_hex(w, 330, br), g_str(w, 100, "AcDbEntity");
        if (ps) g_int(w, 67, 1);
        g_str(w, 8, "0"), g_str(w, 100, "AcDbBlockEnd");
    }
    g_str(w, 0, "ENDSEC");

    /* ENTITIES */
    g_str(w, 0, "SECTION");
    g_str(w, 2, "ENTITIES");
    entities(w);
    g_str(w, 0, "ENDSEC");

    /* OBJECTS */
    g_str(w, 0, "SECTION");
    g_str(w, 2, "OBJECTS");
    g_str(w, 0, "DICTIONARY"), g_hex(w, 5, H_ROOT), g_str(w, 330, "0"), g_str(w, 100, "AcDbDictionary"), g_int(w, 281, 1);
    g_str(w, 3, "ACAD_GROUP"), g_hex(w, 350, H_GROUP);
    g_str(w, 3, "ACAD_LAYOUT"), g_hex(w, 350, H_LAYDICT);
    g_str(w, 3, "ACAD_PLOTSTYLENAME"), g_hex(w, 350, H_PSDICT);
    g_str(w, 0, "DICTIONARY"), g_hex(w, 5, H_GROUP), g_hex(w, 330, H_ROOT), g_str(w, 100, "AcDbDictionary"), g_int(w, 281, 1);
    g_str(w, 0, "DICTIONARY"), g_hex(w, 5, H_LAYDICT), g_hex(w, 330, H_ROOT), g_str(w, 100, "AcDbDictionary"), g_int(w, 281, 1);
    g_str(w, 3, "Layout1"), g_hex(w, 350, H_LAYOUT_1);
    g_str(w, 3, "Model"), g_hex(w, 350, H_LAYOUT_M);
    g_str(w, 0, "ACDBDICTIONARYWDFLT"), g_hex(w, 5, H_PSDICT), g_hex(w, 330, H_ROOT), g_str(w, 100, "AcDbDictionary");
    g_int(w, 281, 1), g_str(w, 3, "Normal"), g_hex(w, 350, H_PH);
    g_str(w, 100, "AcDbDictionaryWithDefault"), g_hex(w, 340, H_PH);
    g_str(w, 0, "ACDBPLACEHOLDER"), g_hex(w, 5, H_PH), g_hex(w, 330, H_PSDICT);
    layout(w, H_LAYOUT_M, H_LAYDICT, "Model", 0, H_BR_MS);
    layout(w, H_LAYOUT_1, H_LAYDICT, "Layout1", 1, H_BR_PS);
    g_str(w, 0, "ENDSEC");
    g_str(w, 0, "EOF");
}

int cc_dxf_write(const cc_paper *p, const cc_dxf_options *o) {
    W w;
    memset(&w, 0, sizeof w);
    w.o = o, w.p = p;
    buf_init(&w.num), buf_init(&w.name);
    if (!o->sheet) {
        if (!(o->frame.k > 0)) return CC_DXF_NONFINITE;
        double t[3];
        cc_turn_apply_point(&o->frame.turn, o->frame.origin, t);
        w.turn_off[0] = t[0], w.turn_off[1] = t[1];
    }
    /* Pass 1: the layers, line types, extents and the number of entities. */
    w.dry = 1;
    entities(&w);
    int rc = w.err;
    if (!rc) {
        w.dry = 0;
        w.chunk = malloc(CC_DXF_CHUNK);
        if (!w.chunk) rc = CC_DXF_OOM;
        else {
            write_file(&w);
            flush(&w);
            rc = w.err;
            if (!rc && (w.num.oom || w.name.oom)) rc = CC_DXF_OOM;
        }
    }
    free(w.chunk);
    for (size_t i = 0; i < w.nl; i++) free(w.layers[i].name);
    free(w.layers);
    buf_free(&w.num), buf_free(&w.name);
    return rc;
}

static int to_buf(void *ctx, const char *p, size_t n) {
    Buf *b = ctx;
    buf_append(b, p, n);
    return b->oom ? -1 : 0;
}

int cc_dxf_write_buf(const cc_paper *p, const cc_dxf_options *o, Buf *out) {
    cc_dxf_options q = *o;
    q.sink = to_buf;
    q.sink_ctx = out;
    int rc = cc_dxf_write(p, &q);
    return rc == CC_DXF_SINK ? CC_DXF_OOM : rc;
}
